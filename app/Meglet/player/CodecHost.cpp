#include "pch.h"

#include "player/CodecHost.h"

#include "shell/Files.h"
#include "shell/Platform.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace meglet::app {

namespace {

/// A delay-loaded DLL that is not there raises VcppException(ERROR_SEVERITY_ERROR,
/// ERROR_MOD_NOT_FOUND or ERROR_PROC_NOT_FOUND) at the first call into it, as in MediaHost.cpp.
/// (No C++ objects in these frames, for __try.)
bool missingDll(DWORD code) { return code == 0xC06D007E || code == 0xC06D007F; }

HRESULT startMediaFoundation() {
    __try {
        return MFStartup(MF_VERSION, MFSTARTUP_LITE);
    } __except (missingDll(GetExceptionCode()) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    }
}

HRESULT enumerate(const GUID* category, const MFT_REGISTER_TYPE_INFO* input, IMFActivate*** found, UINT32* count) {
    __try {
        return MFTEnumEx(*category, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER, input, nullptr, found, count);
    } __except (missingDll(GetExceptionCode()) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    }
}

/// Media Foundation, started once for the process (MFStartup counts; this is never undone).
bool started() {
    static std::once_flag once;
    static HRESULT result = E_FAIL;
    std::call_once(once, [] {
        result = startMediaFoundation();
        if (FAILED(result)) shellLog::write(format("codec: Media Foundation would not start (0x%08X): no decoders for MediaCodec", unsigned(result)));
    });
    return SUCCEEDED(result);
}

const char* kindName(int kind) {
    switch (kind) {
    case MEGLET_CODEC_H264: return "H.264";
    case MEGLET_CODEC_H265: return "H.265";
    case MEGLET_CODEC_AAC: return "AAC";
    case MEGLET_CODEC_MP3: return "MP3";
    default: return "?";
    }
}

bool isVideo(int kind) { return kind == MEGLET_CODEC_H264 || kind == MEGLET_CODEC_H265; }

/// What MFTEnumEx is asked for a kind: the category and the compressed input type.
bool inputOf(int kind, GUID& category, MFT_REGISTER_TYPE_INFO& input) {
    switch (kind) {
    case MEGLET_CODEC_H264: input = { MFMediaType_Video, MFVideoFormat_H264 }; break;
    case MEGLET_CODEC_H265: input = { MFMediaType_Video, MFVideoFormat_HEVC }; break;
    case MEGLET_CODEC_AAC: input = { MFMediaType_Audio, MFAudioFormat_AAC }; break;
    case MEGLET_CODEC_MP3: input = { MFMediaType_Audio, MFAudioFormat_MP3 }; break;
    default: return false;
    }
    category = isVideo(kind) ? MFT_CATEGORY_VIDEO_DECODER : MFT_CATEGORY_AUDIO_DECODER;
    return true;
}

/// The decoders Windows has for a kind, best first (MFT_ENUM_FLAG_SORTANDFILTER), with their names.
struct Found {
    std::vector<winrt::com_ptr<IMFActivate>> activates;
    std::vector<std::string> names;
};

Found decodersFor(int kind) {
    Found found;
    GUID category = {};
    MFT_REGISTER_TYPE_INFO input = {};
    if (!inputOf(kind, category, input) || !started()) return found;
    IMFActivate** list = nullptr;
    UINT32 count = 0;
    const HRESULT hr = enumerate(&category, &input, &list, &count);
    if (FAILED(hr)) {
        shellLog::write(format("codec: MFTEnumEx for %s failed (0x%08X)", kindName(kind), unsigned(hr)));
        return found;
    }
    for (UINT32 i = 0; i < count; i++) {
        winrt::com_ptr<IMFActivate> activate;
        activate.attach(list[i]);
        wchar_t* name = nullptr;
        UINT32 length = 0;
        std::string text = "(no name)";
        if (SUCCEEDED(activate->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &length)) && name) {
            text = narrow(name);
            CoTaskMemFree(name);
        }
        found.names.push_back(text);
        found.activates.push_back(std::move(activate));
    }
    CoTaskMemFree(list);
    return found;
}

/// How long a decoder's call may take before it is given up on: a decoder that does not return from a call of Media
/// Foundation's (the phone's HEVCVideoExtension did not, from its first sample) would otherwise hold the app's thread
/// for ever. Far longer than any call takes that returns.
constexpr std::chrono::milliseconds callLimit{8000};

/// A thread in COM's multithreaded apartment that runs one piece of work at a time for whoever waits for it. A piece of
/// work that has not returned within its limit is left to the thread, which is given up on: that run and every later
/// one answer false at once, and the worker is never destroyed (its thread may yet come back to it). So what a piece of
/// work uses must outlive the caller's wait: captured by value, or shared.
class Worker {
public:
    Worker() : thread_([this] { loop(); }) {}
    ~Worker() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            quit_ = true;
        }
        wake_.notify_all();
        thread_.join();
    }
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    /// Runs `work` on the thread and returns true when it has run; false when it had not after `limit`, or when an
    /// earlier run had not.
    bool run(std::function<void()> work, std::chrono::milliseconds limit = callLimit) {
        auto job = std::make_shared<Job>();
        job->work = std::move(work);
        std::unique_lock<std::mutex> lock(mutex_);
        if (stuck_) return false;
        idle_.wait(lock, [this] { return !job_ || stuck_; });
        if (stuck_) return false;
        job_ = job;
        wake_.notify_all();
        if (!done_.wait_for(lock, limit, [&job] { return job->finished; })) {
            stuck_ = true;
            idle_.notify_all();
            return false;
        }
        return true;
    }

    bool stuck() {
        std::lock_guard<std::mutex> lock(mutex_);
        return stuck_;
    }

private:
    struct Job {
        std::function<void()> work;
        bool finished = false;
    };

    void loop() {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            wake_.wait(lock, [this] { return quit_ || job_; });
            if (quit_ && !job_) break;
            std::shared_ptr<Job> job = job_;
            lock.unlock();
            job->work();
            lock.lock();
            job->finished = true;
            job_ = nullptr;
            done_.notify_all();
            idle_.notify_one();
        }
        lock.unlock();
        if (SUCCEEDED(com)) CoUninitialize();
    }

    std::mutex mutex_;
    std::condition_variable wake_, done_, idle_;
    std::shared_ptr<Job> job_;
    bool quit_ = false, stuck_ = false;
    std::thread thread_;
};

/// Decoders (by name) that did not return from a call: not taken again, in this run or a later one. They are kept, a
/// name a line, in LocalState\codec-hung.txt: on the phone HEVCVideoExtension never came back from its first
/// ProcessOutput, and without the file every app would wait for it again, once a run. Deleting the file tries them again.
std::mutex gHungMutex;
std::vector<std::string> gHung;
bool gHungRead = false;

std::wstring hungFile() { return platform::localStateDirectory() + L"\\codec-hung.txt"; }

/// With gHungMutex held.
void readHung() {
    if (gHungRead) return;
    gHungRead = true;
    std::string text;
    if (!files::read(hungFile(), text)) return;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) gHung.push_back(line);
        at = end + 1;
    }
}

bool hung(const std::string& name) {
    std::lock_guard<std::mutex> lock(gHungMutex);
    readHung();
    return std::find(gHung.begin(), gHung.end(), name) != gHung.end();
}

/// The worker for supports(), which no decoder's thread is there for yet.
Worker& serviceWorker() {
    // Never destroyed: an app's threads may still be in a call when the process ends.
    static Worker* const worker = new Worker();
    return *worker;
}

/// Whether picture decoders are asked for low latency (MF_LOW_LATENCY): unless the flag file
/// codec-no-low-latency is in LocalState\flags, which brings the decoders' own way back, to compare.
bool lowLatencyWanted() {
    static const bool wanted = !files::exists(platform::localStateDirectory() + L"\\flags\\codec-no-low-latency");
    return wanted;
}

std::string subtypeName(const GUID& g) {
    if (g == MFVideoFormat_NV12) return "NV12";
    if (g == MFVideoFormat_P010) return "P010";
    if (g == MFVideoFormat_YV12) return "YV12";
    if (g == MFVideoFormat_IYUV) return "IYUV";
    if (g == MFVideoFormat_I420) return "I420";
    if (g == MFVideoFormat_YUY2) return "YUY2";
    if (g == MFAudioFormat_PCM) return "PCM";
    if (g == MFAudioFormat_Float) return "float";
    return format("%08lX", g.Data1);
}

} // namespace

/// A picture out of a decoder: its buffer held (locked) until the core lets it go, or a copy of it.
struct CodecHost::Picture {
    meglet_codec_picture info = {};
    winrt::com_ptr<IMFMediaBuffer> buffer;
    winrt::com_ptr<IMF2DBuffer> planar; // when locked as one
    std::vector<uint8_t> copy;

    ~Picture() {
        if (planar) planar->Unlock2D();
        else if (buffer) buffer->Unlock();
    }
};

/// One decoder. `transform` and everything about it are the worker's alone; `mutex` guards what is waiting to be pulled.
struct CodecHost::Codec {
    int kind = 0;
    std::unique_ptr<Worker> worker;
    winrt::com_ptr<IMFTransform> transform;
    std::string name;
    DWORD in = 0, out = 0;           // stream ids
    bool decoderSamples = false;     // the decoder brings its own output samples
    bool lowLatency = false;         // MF_LOW_LATENCY was taken
    bool p010 = false;               // the pictures come as P010 (ten bits a sample), and are made NV12's eight here
    bool tenBitsFirst = false;       // P010 is taken where NV12 is offered too (CodecHost::preferTenBits, for the tests)
    DWORD outSize = 0, outAlign = 0; // of ours
    winrt::com_ptr<IMFSample> spare; // an output sample of ours the decoder did not fill

    // Pictures: what the output type says.
    int frameWidth = 0, frameHeight = 0, cropX = 0, cropY = 0, visibleWidth = 0, visibleHeight = 0, stride = 0;
    int matrix = 0;
    bool fullRange = false;
    // Sound: what the output type says, and the channels the core is given.
    int rate = 0, channels = 0, outChannels = 0;
    int64_t lastInputUs = 0;

    uint64_t pushed = 0, pictures = 0, blocks = 0, failures = 0;
    std::atomic<const char*> step{"nothing"}; // the call of Media Foundation's being made, for when one does not return

    std::mutex mutex;
    std::deque<Picture*> ready;
    struct Block {
        std::vector<int16_t> samples;
        int frames = 0, rate = 0, channels = 0;
        int64_t timeUs = 0;
    };
    std::deque<Block> sound;
    Block handed; // the block the core was given last: its samples stay until its next call

    ~Codec() {
        for (Picture* p : ready) delete p;
    }

    void fail(const std::string& what) {
        if (++failures <= 3) shellLog::write(format("codec: %s (%s): %s", kindName(kind), name.c_str(), what.c_str()));
    }

    /// Its worker did not come back from a call: said, with the call, and the decoder is not taken again.
    void gaveUp(const char* doing) {
        shellLog::write(format("codec: %s (%s): %s has not returned from %s after %lld s; given up on, and not taken again (LocalState\\codec-hung.txt)",
                               kindName(kind), name.c_str(), doing, step.load(), (long long)(callLimit.count() / 1000)));
        std::lock_guard<std::mutex> lock(gHungMutex);
        readHung();
        if (!name.empty() && std::find(gHung.begin(), gHung.end(), name) == gHung.end()) {
            gHung.push_back(name);
            files::append(hungFile(), name + "\r\n");
        }
    }

    /// The first output type of this subtype the decoder offers, or null.
    winrt::com_ptr<IMFMediaType> offeredOutput(const GUID& wanted) {
        for (DWORD i = 0;; i++) {
            winrt::com_ptr<IMFMediaType> type;
            if (FAILED(transform->GetOutputAvailableType(out, i, type.put()))) return nullptr;
            GUID subtype = {};
            type->GetGUID(MF_MT_SUBTYPE, &subtype);
            if (subtype == wanted) return type;
        }
    }

    /// The picture's output type: NV12 (or, from a decoder of ten-bit streams that offers nothing else, P010), the size,
    /// the visible part, the rows.
    bool chooseVideoOutput() {
        step = "GetOutputAvailableType";
        winrt::com_ptr<IMFMediaType> type = tenBitsFirst ? offeredOutput(MFVideoFormat_P010) : nullptr;
        p010 = type != nullptr;
        if (!type) type = offeredOutput(MFVideoFormat_NV12);
        if (!type) p010 = (type = offeredOutput(MFVideoFormat_P010)) != nullptr;
        if (type) {
            step = "SetOutputType";
            const HRESULT hr = transform->SetOutputType(out, type.get(), 0);
            if (FAILED(hr)) {
                fail(format("%s output refused (0x%08X)", p010 ? "P010" : "NV12", unsigned(hr)));
                return false;
            }
            UINT32 w = 0, h = 0;
            MFGetAttributeSize(type.get(), MF_MT_FRAME_SIZE, &w, &h);
            frameWidth = int(w);
            frameHeight = int(h);
            cropX = cropY = 0;
            visibleWidth = frameWidth;
            visibleHeight = frameHeight;
            MFVideoArea area = {};
            if ((SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), nullptr)) ||
                 SUCCEEDED(type->GetBlob(MF_MT_GEOMETRIC_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), nullptr))) &&
                area.Area.cx > 0 && area.Area.cy > 0 && area.OffsetX.value >= 0 && area.OffsetY.value >= 0 &&
                area.OffsetX.value + area.Area.cx <= frameWidth && area.OffsetY.value + area.Area.cy <= frameHeight) {
                cropX = area.OffsetX.value & ~1;
                cropY = area.OffsetY.value & ~1;
                visibleWidth = area.Area.cx;
                visibleHeight = area.Area.cy;
            }
            stride = int(INT32(MFGetAttributeUINT32(type.get(), MF_MT_DEFAULT_STRIDE, 0)));
            if (stride <= 0) stride = frameWidth * (p010 ? 2 : 1);
            const UINT32 m = MFGetAttributeUINT32(type.get(), MF_MT_YUV_MATRIX, MFVideoTransferMatrix_Unknown);
            matrix = m == MFVideoTransferMatrix_BT709 ? MEGLET_CODEC_BT709 : m == MFVideoTransferMatrix_BT601 ? MEGLET_CODEC_BT601 : 0;
            fullRange = MFGetAttributeUINT32(type.get(), MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235) == MFNominalRange_0_255;
            return streamInfo();
        }
        std::string offered;
        for (DWORD i = 0;; i++) {
            winrt::com_ptr<IMFMediaType> other;
            if (FAILED(transform->GetOutputAvailableType(out, i, other.put()))) break;
            GUID subtype = {};
            other->GetGUID(MF_MT_SUBTYPE, &subtype);
            offered += (offered.empty() ? "" : " ") + subtypeName(subtype);
        }
        fail("no NV12 or P010 among the outputs it offers (" + (offered.empty() ? std::string("none") : offered) + ")");
        return false;
    }

    /// The sound's output type: 16-bit PCM, two channels where the decoder can mix down to them.
    bool chooseAudioOutput(int wantChannels) {
        winrt::com_ptr<IMFMediaType> best;
        int bestChannels = 0;
        for (DWORD i = 0;; i++) {
            winrt::com_ptr<IMFMediaType> type;
            if (FAILED(transform->GetOutputAvailableType(out, i, type.put()))) break;
            GUID subtype = {};
            type->GetGUID(MF_MT_SUBTYPE, &subtype);
            if (subtype != MFAudioFormat_PCM || MFGetAttributeUINT32(type.get(), MF_MT_AUDIO_BITS_PER_SAMPLE, 0) != 16) continue;
            const int ch = int(MFGetAttributeUINT32(type.get(), MF_MT_AUDIO_NUM_CHANNELS, 0));
            // The one with the channels asked for (at most two), else the fewest above that, else any.
            const auto score = [wantChannels](int c) { return c == wantChannels ? 0 : c > 0 && c <= 2 ? 1 : c > 2 ? 100 + c : 1000; };
            if (!best || score(ch) < score(bestChannels)) {
                best = type;
                bestChannels = ch;
            }
        }
        if (!best) {
            fail("no 16-bit PCM among the outputs it offers");
            return false;
        }
        HRESULT hr = E_FAIL;
        if (bestChannels > 2) {
            // Asked to mix down: the same type with two channels.
            winrt::com_ptr<IMFMediaType> stereo;
            if (SUCCEEDED(MFCreateMediaType(stereo.put())) && SUCCEEDED(best->CopyAllItems(stereo.get()))) {
                const UINT32 r = MFGetAttributeUINT32(best.get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
                stereo->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
                stereo->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
                stereo->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, r * 4);
                stereo->DeleteItem(MF_MT_AUDIO_CHANNEL_MASK);
                hr = transform->SetOutputType(out, stereo.get(), 0);
                if (SUCCEEDED(hr)) best = stereo;
            }
        }
        if (FAILED(hr)) hr = transform->SetOutputType(out, best.get(), 0);
        if (FAILED(hr)) {
            fail(format("PCM output refused (0x%08X)", unsigned(hr)));
            return false;
        }
        rate = int(MFGetAttributeUINT32(best.get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0));
        channels = int(MFGetAttributeUINT32(best.get(), MF_MT_AUDIO_NUM_CHANNELS, 0));
        outChannels = channels > 2 ? 2 : channels;
        if (rate <= 0 || channels <= 0) {
            fail("its PCM output says no rate or no channels");
            return false;
        }
        return streamInfo();
    }

    bool streamInfo() {
        MFT_OUTPUT_STREAM_INFO info = {};
        const HRESULT hr = transform->GetOutputStreamInfo(out, &info);
        if (FAILED(hr)) {
            fail(format("GetOutputStreamInfo 0x%08X", unsigned(hr)));
            return false;
        }
        decoderSamples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        outSize = info.cbSize;
        outAlign = info.cbAlignment;
        spare = nullptr;
        return true;
    }

    /// The decoder set up for `format`; false, said in the log, when it will not be.
    bool configure(const meglet_codec_format& f) {
        DWORD ins = 0, outs = 0;
        if (SUCCEEDED(transform->GetStreamCount(&ins, &outs)) && (ins != 1 || outs != 1)) {
            fail(format("%lu inputs and %lu outputs", ins, outs));
            return false;
        }
        if (transform->GetStreamIDs(1, &in, 1, &out) != S_OK) in = out = 0;
        winrt::com_ptr<IMFMediaType> type;
        MFCreateMediaType(type.put());
        if (isVideo(kind)) {
            // Low latency: a picture comes out as soon as the stream's own reordering allows (on this PC's H.264 decoder,
            // after two units of avc.mgau), not when the decoder's whole picture buffer is full (18 to 24 units, varying with
            // its threads). Android's decoders hand pictures out as soon, and apps seek and start by it.
            winrt::com_ptr<IMFAttributes> attributes;
            lowLatency = lowLatencyWanted() && SUCCEEDED(transform->GetAttributes(attributes.put())) && attributes &&
                         SUCCEEDED(attributes->SetUINT32(MF_LOW_LATENCY, TRUE));
            type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            type->SetGUID(MF_MT_SUBTYPE, kind == MEGLET_CODEC_H264 ? MFVideoFormat_H264 : MFVideoFormat_HEVC);
            if (f.width > 0 && f.height > 0) MFSetAttributeSize(type.get(), MF_MT_FRAME_SIZE, UINT32(f.width), UINT32(f.height));
            type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_MixedInterlaceOrProgressive);
        } else {
            type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, UINT32(f.sample_rate));
            type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, UINT32(f.channels));
            if (kind == MEGLET_CODEC_AAC) {
                // MF_MT_USER_DATA: what follows WAVEFORMATEX in a HEAACWAVEINFO (payload 0, raw access units; profile not said),
                // then the AudioSpecificConfig.
                std::vector<uint8_t> data(12, 0);
                data[2] = 0xFE;
                if (f.config && f.config_size) data.insert(data.end(), f.config, f.config + f.config_size);
                type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
                type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
                type->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0xFE);
                type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                type->SetBlob(MF_MT_USER_DATA, data.data(), UINT32(data.size()));
            } else {
                type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_MP3);
            }
        }
        step = "SetInputType";
        HRESULT hr = transform->SetInputType(in, type.get(), 0);
        if (FAILED(hr)) {
            fail(format("the input type was refused (0x%08X)", unsigned(hr)));
            return false;
        }
        if (isVideo(kind) ? !chooseVideoOutput() : !chooseAudioOutput(f.channels > 2 ? 2 : f.channels)) return false;
        step = "ProcessMessage(BEGIN_STREAMING)";
        transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    }

    /// An output sample of ours, big enough for what the output type says.
    winrt::com_ptr<IMFSample> outputSample() {
        if (spare) return std::exchange(spare, nullptr);
        winrt::com_ptr<IMFSample> sample;
        winrt::com_ptr<IMFMediaBuffer> buffer;
        DWORD size = outSize;
        if (!size) size = isVideo(kind) ? DWORD(frameWidth) * DWORD(frameHeight) * 3 / 2 : DWORD(rate) * DWORD(channels) * 2;
        if (FAILED(MFCreateSample(sample.put())) || FAILED(MFCreateAlignedMemoryBuffer(size, outAlign ? outAlign - 1 : 0, buffer.put())) ||
            FAILED(sample->AddBuffer(buffer.get())))
            return nullptr;
        return sample;
    }

    /// A decoded picture into the queue: its buffer held, or (a decoder's own buffer) copied.
    void takePicture(IMFSample* sample) {
        auto picture = std::make_unique<Picture>();
        LONGLONG time = 0;
        if (FAILED(sample->GetSampleTime(&time))) time = lastInputUs * 10;
        winrt::com_ptr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(buffer.put()))) {
            fail("a picture's buffer could not be had");
            return;
        }
        BYTE* base = nullptr;
        LONG pitch = 0;
        DWORD length = 0;
        if (auto planar = buffer.try_as<IMF2DBuffer>(); planar && SUCCEEDED(planar->Lock2D(&base, &pitch)) && base && pitch > 0) {
            picture->planar = planar;
            length = DWORD(pitch) * DWORD(frameHeight) * 3 / 2;
        } else if (SUCCEEDED(buffer->Lock(&base, nullptr, &length)) && base) {
            pitch = stride;
        } else {
            fail("a picture's buffer could not be locked");
            return;
        }
        picture->buffer = buffer;
        if (DWORD(pitch) * DWORD(frameHeight) * 3 / 2 > length || visibleWidth <= 0 || visibleHeight <= 0) {
            fail(format("a picture of %lu bytes is not %dx%d NV12 with rows of %ld", length, frameWidth, frameHeight, pitch));
            return;
        }
        const size_t bytes = p010 ? 2 : 1; // a sample's
        const BYTE* luma = base + size_t(cropY) * size_t(pitch) + size_t(cropX) * bytes;
        const BYTE* chroma = base + size_t(frameHeight) * size_t(pitch) + size_t(cropY / 2) * size_t(pitch) + size_t(cropX) * bytes;
        if (decoderSamples || p010) {
            // The decoder's own buffer goes back to it, or P010 is made NV12: the visible part copied out, eight bits a
            // sample (P010 has its ten in the high bits of sixteen: the high byte of each, little-endian).
            const size_t w = size_t(visibleWidth), h = size_t(visibleHeight), ch = (h + 1) / 2, cw = (w + 1) & ~size_t(1);
            picture->copy.resize(w * h + cw * ch);
            uint8_t* to = picture->copy.data();
            for (size_t y = 0; y < h; y++, to += w) {
                const BYTE* from = luma + y * size_t(pitch);
                if (!p010) memcpy(to, from, w);
                else for (size_t x = 0; x < w; x++) to[x] = from[2 * x + 1];
            }
            for (size_t y = 0; y < ch; y++, to += cw) {
                const BYTE* from = chroma + y * size_t(pitch);
                if (!p010) memcpy(to, from, cw);
                else for (size_t x = 0; x < cw; x++) to[x] = from[2 * x + 1];
            }
            if (picture->planar) picture->planar->Unlock2D();
            else picture->buffer->Unlock();
            picture->planar = nullptr;
            picture->buffer = nullptr;
            luma = picture->copy.data();
            chroma = picture->copy.data() + w * h;
            pitch = LONG(w);
            picture->info.chroma_stride = int(cw);
        } else {
            picture->info.chroma_stride = int(pitch);
        }
        meglet_codec_picture& p = picture->info;
        p.format = MEGLET_CODEC_NV12;
        p.width = visibleWidth;
        p.height = visibleHeight;
        p.data = luma;
        p.chroma = chroma;
        p.stride = int(pitch);
        p.matrix = matrix;
        p.full_range = fullRange;
        p.time_us = time / 10;
        if (!pictures++)
            shellLog::write(format("codec: the first %s picture from %s, %dx%d (decoded %dx%d, rows of %ld), at %lld ms", kindName(kind), name.c_str(), visibleWidth,
                                   visibleHeight, frameWidth, frameHeight, long(pitch), (long long)(time / 10000)));
        std::lock_guard<std::mutex> lock(mutex);
        ready.push_back(picture.release());
    }

    /// Decoded sound into the queue, as 16-bit PCM in one or two channels.
    void takeSound(IMFSample* sample) {
        LONGLONG time = 0;
        const bool timed = SUCCEEDED(sample->GetSampleTime(&time));
        winrt::com_ptr<IMFMediaBuffer> buffer;
        BYTE* data = nullptr;
        DWORD length = 0;
        if (FAILED(sample->ConvertToContiguousBuffer(buffer.put())) || FAILED(buffer->Lock(&data, nullptr, &length)) || !data) {
            fail("a block of sound could not be read");
            return;
        }
        Block b;
        b.frames = int(length / DWORD(channels * 2));
        b.rate = rate;
        b.channels = outChannels;
        b.timeUs = timed ? time / 10 : lastInputUs;
        const int16_t* from = reinterpret_cast<const int16_t*>(data);
        if (channels == outChannels) {
            b.samples.assign(from, from + size_t(b.frames) * size_t(channels));
        } else {
            b.samples.resize(size_t(b.frames) * 2);
            for (int i = 0; i < b.frames; i++) {
                b.samples[size_t(i) * 2] = from[size_t(i) * size_t(channels)];
                b.samples[size_t(i) * 2 + 1] = from[size_t(i) * size_t(channels) + 1];
            }
        }
        buffer->Unlock();
        if (!b.frames) return;
        if (!blocks++)
            shellLog::write(format("codec: the first %s sound from %s, %d frames at %d Hz in %d channels (%d decoded), at %lld ms", kindName(kind), name.c_str(), b.frames,
                                   rate, outChannels, channels, (long long)(b.timeUs / 1000)));
        std::lock_guard<std::mutex> lock(mutex);
        sound.push_back(std::move(b));
    }

    /// Everything the decoder has to give, into the queues; false when it failed.
    bool takeOutputs() {
        for (int guard = 0; guard < 1000; guard++) {
            winrt::com_ptr<IMFSample> ours;
            if (!decoderSamples) {
                ours = outputSample();
                if (!ours) {
                    fail("no memory for an output sample");
                    return false;
                }
            }
            MFT_OUTPUT_DATA_BUFFER buffer = {};
            buffer.dwStreamID = out;
            buffer.pSample = ours.get();
            DWORD status = 0;
            step = "ProcessOutput";
            const HRESULT hr = transform->ProcessOutput(0, 1, &buffer, &status);
            if (buffer.pEvents) buffer.pEvents->Release();
            winrt::com_ptr<IMFSample> theirs;
            if (decoderSamples && buffer.pSample) theirs.attach(buffer.pSample);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                if (ours) spare = ours;
                return true;
            }
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE || (buffer.dwStatus & MFT_OUTPUT_DATA_BUFFER_FORMAT_CHANGE)) {
                const bool chosen = isVideo(kind) ? chooseVideoOutput() : chooseAudioOutput(outChannels);
                if (isVideo(kind))
                    shellLog::write(format("codec: %s pictures are now %dx%d %s, %dx%d of them shown from %d,%d, rows of %d bytes", kindName(kind), frameWidth,
                                           frameHeight, p010 ? "P010 (made NV12 here)" : "NV12", visibleWidth, visibleHeight, cropX, cropY, stride));
                else
                    shellLog::write(format("codec: %s sound is now %d Hz in %d channels", kindName(kind), rate, channels));
                if (!chosen) return false;
                if (hr == MF_E_TRANSFORM_STREAM_CHANGE) continue;
            }
            if (FAILED(hr)) {
                fail(format("ProcessOutput 0x%08X", unsigned(hr)));
                return false;
            }
            IMFSample* sample = decoderSamples ? theirs.get() : ours.get();
            if (!sample || (buffer.dwStatus & MFT_OUTPUT_DATA_BUFFER_NO_SAMPLE)) continue;
            if (isVideo(kind)) takePicture(sample);
            else takeSound(sample);
        }
        fail("it would not stop giving output");
        return false;
    }

    bool push(const uint8_t* data, size_t size, int64_t timeUs) {
        winrt::com_ptr<IMFSample> sample;
        winrt::com_ptr<IMFMediaBuffer> buffer;
        BYTE* to = nullptr;
        if (FAILED(MFCreateSample(sample.put())) || FAILED(MFCreateMemoryBuffer(DWORD(size), buffer.put())) || FAILED(buffer->Lock(&to, nullptr, nullptr))) {
            fail("no memory for an input sample");
            return false;
        }
        memcpy(to, data, size);
        buffer->Unlock();
        buffer->SetCurrentLength(DWORD(size));
        sample->AddBuffer(buffer.get());
        sample->SetSampleTime(timeUs * 10);
        lastInputUs = timeUs;
        pushed++;
        step = "ProcessInput";
        HRESULT hr = transform->ProcessInput(in, sample.get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            // Output first, then the sample again.
            if (!takeOutputs()) return false;
            step = "ProcessInput";
            hr = transform->ProcessInput(in, sample.get(), 0);
        }
        if (FAILED(hr)) {
            fail(format("ProcessInput 0x%08X for a sample of %zu bytes at %lld ms", unsigned(hr), size, (long long)(timeUs / 1000)));
            return false;
        }
        return takeOutputs();
    }

    void drain() {
        step = "ProcessMessage(COMMAND_DRAIN)";
        transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        takeOutputs();
    }

    void flush() {
        step = "ProcessMessage(COMMAND_FLUSH)";
        transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        std::lock_guard<std::mutex> lock(mutex);
        for (Picture* p : ready) delete p;
        ready.clear();
        sound.clear();
    }
};

CodecHost& CodecHost::shared() {
    // Never destroyed: an app's threads may still be in a call when the process ends.
    static CodecHost* const host = new CodecHost();
    return *host;
}

meglet_codec_host CodecHost::callbacks() {
    meglet_codec_host host = {};
    host.user = this;
    host.reorder = 0; // Media Foundation's decoders hand pictures out in display order
    host.supports = &supportsThunk;
    host.open = &openThunk;
    host.push = &pushThunk;
    host.drain = &drainThunk;
    host.pull_picture = &pullPictureThunk;
    host.release_picture = &releasePictureThunk;
    host.pull_sound = &pullSoundThunk;
    host.flush = &flushThunk;
    host.close = &closeThunk;
    return host;
}

namespace {
std::atomic<bool> gTenBitsFirst{false};
}

void CodecHost::preferTenBits(bool prefer) { gTenBitsFirst = prefer; }

bool CodecHost::checkCallLimit() {
    // Left as a stuck decoder's worker is: never destroyed.
    auto* worker = new Worker();
    const bool quick = worker->run([] {}, std::chrono::milliseconds(2000));
    const bool slow = worker->run([] { Sleep(400); }, std::chrono::milliseconds(50));
    const auto from = std::chrono::steady_clock::now();
    const bool after = worker->run([] {}, std::chrono::milliseconds(2000));
    const bool atOnce = std::chrono::steady_clock::now() - from < std::chrono::milliseconds(100);
    return quick && !slow && !after && atOnce && worker->stuck();
}

std::string CodecHost::summary() {
    auto line = std::make_shared<std::string>();
    serviceWorker().run([line] {
        for (int kind = MEGLET_CODEC_H264; kind <= MEGLET_CODEC_MP3; kind++) {
            const Found found = decodersFor(kind);
            *line += format("%s%s: %s", line->empty() ? "" : "; ", kindName(kind), found.names.empty() ? "none" : found.names[0].c_str());
        }
    });
    return *line;
}

bool CodecHost::supportsThunk(void*, int kind) {
    // Asked once for each kind by MediaCodecList, and kept: what Windows has does not change while an app runs.
    static std::mutex mutex;
    static int known[8] = {};
    if (kind < 1 || kind > MEGLET_CODEC_MP3) return false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (known[kind]) return known[kind] > 0;
    }
    auto there = std::make_shared<bool>(false);
    serviceWorker().run([kind, there] {
        const Found found = decodersFor(kind);
        *there = !found.activates.empty();
        std::string names;
        for (const std::string& n : found.names) names += (names.empty() ? "" : ", ") + n;
        shellLog::write(format("codec: %s decoders: %s", kindName(kind), *there ? names.c_str() : "none"));
    });
    std::lock_guard<std::mutex> lock(mutex);
    known[kind] = *there ? 1 : -1;
    return *there;
}

void* CodecHost::openThunk(void*, int kind, const meglet_codec_format* asked) {
    auto codec = std::make_unique<Codec>();
    codec->kind = kind;
    codec->tenBitsFirst = gTenBitsFirst;
    codec->worker = std::make_unique<Worker>();
    auto ok = std::make_shared<bool>(false);
    Codec* c = codec.get();
    // The format, and the configuration bytes it points at, the work's own: it may outlive this call.
    meglet_codec_format wanted = *asked;
    auto config = std::make_shared<std::vector<uint8_t>>(asked->config, asked->config + (asked->config ? asked->config_size : 0));
    const bool ran = c->worker->run([c, kind, wanted, config, ok]() mutable {
        wanted.config = config->empty() ? nullptr : config->data();
        const Found found = decodersFor(kind);
        for (size_t i = 0; i < found.activates.size() && !*ok; i++) {
            if (hung(found.names[i])) {
                shellLog::write(format("codec: %s (%s) passed over: it did not return from a call before (LocalState\\codec-hung.txt)", kindName(kind),
                                       found.names[i].c_str()));
                continue;
            }
            winrt::com_ptr<IMFTransform> transform;
            c->name = found.names[i];
            c->step = "ActivateObject";
            const HRESULT hr = found.activates[i]->ActivateObject(__uuidof(IMFTransform), transform.put_void());
            if (FAILED(hr)) {
                c->fail(format("not made (0x%08X)", unsigned(hr)));
                continue;
            }
            c->transform = transform;
            c->failures = 0;
            *ok = c->configure(wanted);
            if (!*ok) {
                c->transform = nullptr;
                found.activates[i]->ShutdownObject();
            }
        }
        if (*ok) {
            if (isVideo(kind))
                shellLog::write(format("codec: an %s decoder, %s, for %dx%d%s", kindName(kind), c->name.c_str(), wanted.width, wanted.height,
                                       c->lowLatency ? ", low latency" : lowLatencyWanted() ? ", low latency refused" : ", low latency not asked for (flag)"));
            else
                shellLog::write(format("codec: an %s decoder, %s, for %d Hz in %d channels (gives %d Hz in %d)", kindName(kind), c->name.c_str(), wanted.sample_rate,
                                       wanted.channels, c->rate, c->outChannels));
        } else {
            shellLog::write(format("codec: no %s decoder could be made (%zu found)", kindName(kind), found.activates.size()));
        }
    });
    if (!ran) {
        c->gaveUp("opening it");
        codec.release(); // left to its worker's thread, which may yet come back to it
        return nullptr;
    }
    return *ok ? codec.release() : nullptr;
}

bool CodecHost::pushThunk(void*, void* codec, const uint8_t* data, size_t size, int64_t timeUs) {
    auto* c = static_cast<Codec*>(codec);
    if (c->worker->stuck()) return false;
    auto ok = std::make_shared<bool>(false);
    auto unit = std::make_shared<std::vector<uint8_t>>(data, data + size);
    if (!c->worker->run([c, unit, timeUs, ok] { *ok = c->push(unit->data(), unit->size(), timeUs); })) {
        c->gaveUp("a sample");
        return false;
    }
    return *ok;
}

void CodecHost::drainThunk(void*, void* codec) {
    auto* c = static_cast<Codec*>(codec);
    if (!c->worker->stuck() && !c->worker->run([c] { c->drain(); })) c->gaveUp("the end of the stream");
}

void* CodecHost::pullPictureThunk(void*, void* codec, meglet_codec_picture* picture) {
    auto* c = static_cast<Codec*>(codec);
    std::lock_guard<std::mutex> lock(c->mutex);
    if (c->ready.empty()) return nullptr;
    Picture* p = c->ready.front();
    c->ready.pop_front();
    *picture = p->info;
    return p;
}

void CodecHost::releasePictureThunk(void*, void* picture) { delete static_cast<Picture*>(picture); }

bool CodecHost::pullSoundThunk(void*, void* codec, meglet_codec_sound* sound) {
    auto* c = static_cast<Codec*>(codec);
    std::lock_guard<std::mutex> lock(c->mutex);
    if (c->sound.empty()) return false;
    c->handed = std::move(c->sound.front());
    c->sound.pop_front();
    sound->samples = c->handed.samples.data();
    sound->frames = c->handed.frames;
    sound->sample_rate = c->handed.rate;
    sound->channels = c->handed.channels;
    sound->time_us = c->handed.timeUs;
    return true;
}

void CodecHost::flushThunk(void*, void* codec) {
    auto* c = static_cast<Codec*>(codec);
    if (!c->worker->stuck() && !c->worker->run([c] { c->flush(); })) c->gaveUp("a flush");
}

void CodecHost::closeThunk(void*, void* codec) {
    auto* c = static_cast<Codec*>(codec);
    if (c->worker->stuck()) {
        // Its worker's thread is still in the decoder: it and the decoder are left to it, not freed under it.
        shellLog::write(format("codec: an %s decoder (%s) closed while a call of its had not returned: left as it is", kindName(c->kind), c->name.c_str()));
        return;
    }
    const bool ran = c->worker->run([c] {
        if (c->transform) {
            c->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            c->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
        shellLog::write(format("codec: an %s decoder (%s) closed after %llu samples, %llu pictures, %llu blocks of sound, %llu failures", kindName(c->kind),
                               c->name.c_str(), (unsigned long long)c->pushed, (unsigned long long)c->pictures, (unsigned long long)c->blocks,
                               (unsigned long long)c->failures));
        c->spare = nullptr;
        c->transform = nullptr;
    });
    if (!ran) {
        c->gaveUp("closing it");
        return;
    }
    // The worker's thread is joined here, its work done; the transform was let go where it was made.
    delete c;
}

} // namespace meglet::app
