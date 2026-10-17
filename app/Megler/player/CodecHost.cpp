#include "pch.h"

#include "player/CodecHost.h"

#include "shell/Files.h"
#include "shell/Platform.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>

#include <condition_variable>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace megler::app {

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
    case MEGLER_CODEC_H264: return "H.264";
    case MEGLER_CODEC_H265: return "H.265";
    case MEGLER_CODEC_AAC: return "AAC";
    case MEGLER_CODEC_MP3: return "MP3";
    default: return "?";
    }
}

bool isVideo(int kind) { return kind == MEGLER_CODEC_H264 || kind == MEGLER_CODEC_H265; }

/// What MFTEnumEx is asked for a kind: the category and the compressed input type.
bool inputOf(int kind, GUID& category, MFT_REGISTER_TYPE_INFO& input) {
    switch (kind) {
    case MEGLER_CODEC_H264: input = { MFMediaType_Video, MFVideoFormat_H264 }; break;
    case MEGLER_CODEC_H265: input = { MFMediaType_Video, MFVideoFormat_HEVC }; break;
    case MEGLER_CODEC_AAC: input = { MFMediaType_Audio, MFAudioFormat_AAC }; break;
    case MEGLER_CODEC_MP3: input = { MFMediaType_Audio, MFAudioFormat_MP3 }; break;
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

/// A thread in COM's multithreaded apartment that runs one piece of work at a time for whoever waits for it.
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

    /// Runs `work` on the thread and returns when it has run.
    void run(const std::function<void()>& work) {
        std::unique_lock<std::mutex> lock(mutex_);
        idle_.wait(lock, [this] { return !work_; });
        bool finished = false;
        work_ = &work;
        finished_ = &finished;
        wake_.notify_all();
        done_.wait(lock, [&finished] { return finished; });
    }

private:
    void loop() {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            wake_.wait(lock, [this] { return quit_ || work_; });
            if (quit_ && !work_) break;
            const std::function<void()>* work = work_;
            lock.unlock();
            (*work)();
            lock.lock();
            work_ = nullptr;
            *finished_ = true;
            finished_ = nullptr;
            done_.notify_all();
            idle_.notify_one();
        }
        lock.unlock();
        if (SUCCEEDED(com)) CoUninitialize();
    }

    std::mutex mutex_;
    std::condition_variable wake_, done_, idle_;
    const std::function<void()>* work_ = nullptr;
    bool* finished_ = nullptr;
    bool quit_ = false;
    std::thread thread_;
};

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
    megler_codec_picture info = {};
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

    /// The picture's output type: NV12, the size, the visible part, the rows.
    bool chooseVideoOutput() {
        for (DWORD i = 0;; i++) {
            winrt::com_ptr<IMFMediaType> type;
            if (FAILED(transform->GetOutputAvailableType(out, i, type.put()))) break;
            GUID subtype = {};
            type->GetGUID(MF_MT_SUBTYPE, &subtype);
            if (subtype != MFVideoFormat_NV12) continue;
            const HRESULT hr = transform->SetOutputType(out, type.get(), 0);
            if (FAILED(hr)) {
                fail(format("NV12 output refused (0x%08X)", unsigned(hr)));
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
            if (stride <= 0) stride = frameWidth;
            const UINT32 m = MFGetAttributeUINT32(type.get(), MF_MT_YUV_MATRIX, MFVideoTransferMatrix_Unknown);
            matrix = m == MFVideoTransferMatrix_BT709 ? MEGLER_CODEC_BT709 : m == MFVideoTransferMatrix_BT601 ? MEGLER_CODEC_BT601 : 0;
            fullRange = MFGetAttributeUINT32(type.get(), MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235) == MFNominalRange_0_255;
            return streamInfo();
        }
        std::string offered;
        for (DWORD i = 0;; i++) {
            winrt::com_ptr<IMFMediaType> type;
            if (FAILED(transform->GetOutputAvailableType(out, i, type.put()))) break;
            GUID subtype = {};
            type->GetGUID(MF_MT_SUBTYPE, &subtype);
            offered += (offered.empty() ? "" : " ") + subtypeName(subtype);
        }
        fail("no NV12 among the outputs it offers (" + (offered.empty() ? std::string("none") : offered) + ")");
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
    bool configure(const megler_codec_format& f) {
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
            // after two units of avc.hkau), not when the decoder's whole picture buffer is full (18 to 24 units, varying with
            // its threads). Android's decoders hand pictures out as soon, and apps seek and start by it.
            winrt::com_ptr<IMFAttributes> attributes;
            lowLatency = lowLatencyWanted() && SUCCEEDED(transform->GetAttributes(attributes.put())) && attributes &&
                         SUCCEEDED(attributes->SetUINT32(MF_LOW_LATENCY, TRUE));
            type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            type->SetGUID(MF_MT_SUBTYPE, kind == MEGLER_CODEC_H264 ? MFVideoFormat_H264 : MFVideoFormat_HEVC);
            if (f.width > 0 && f.height > 0) MFSetAttributeSize(type.get(), MF_MT_FRAME_SIZE, UINT32(f.width), UINT32(f.height));
            type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_MixedInterlaceOrProgressive);
        } else {
            type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, UINT32(f.sample_rate));
            type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, UINT32(f.channels));
            if (kind == MEGLER_CODEC_AAC) {
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
        HRESULT hr = transform->SetInputType(in, type.get(), 0);
        if (FAILED(hr)) {
            fail(format("the input type was refused (0x%08X)", unsigned(hr)));
            return false;
        }
        if (isVideo(kind) ? !chooseVideoOutput() : !chooseAudioOutput(f.channels > 2 ? 2 : f.channels)) return false;
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
        const BYTE* luma = base + size_t(cropY) * size_t(pitch) + size_t(cropX);
        const BYTE* chroma = base + size_t(frameHeight) * size_t(pitch) + size_t(cropY / 2) * size_t(pitch) + size_t(cropX);
        if (decoderSamples) {
            // The decoder's own buffer goes back to it: the visible part copied out, NV12 as it was.
            const size_t w = size_t(visibleWidth), h = size_t(visibleHeight), ch = (h + 1) / 2;
            picture->copy.resize(w * h + ((w + 1) & ~size_t(1)) * ch);
            for (size_t y = 0; y < h; y++) memcpy(picture->copy.data() + y * w, luma + y * size_t(pitch), w);
            const size_t cw = (w + 1) & ~size_t(1);
            for (size_t y = 0; y < ch; y++) memcpy(picture->copy.data() + w * h + y * cw, chroma + y * size_t(pitch), cw);
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
        megler_codec_picture& p = picture->info;
        p.format = MEGLER_CODEC_NV12;
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
                    shellLog::write(format("codec: %s pictures are now %dx%d, %dx%d of them shown from %d,%d, rows of %d", kindName(kind), frameWidth, frameHeight,
                                           visibleWidth, visibleHeight, cropX, cropY, stride));
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
        HRESULT hr = transform->ProcessInput(in, sample.get(), 0);
        if (hr == MF_E_NOTACCEPTING) {
            // Output first, then the sample again.
            if (!takeOutputs()) return false;
            hr = transform->ProcessInput(in, sample.get(), 0);
        }
        if (FAILED(hr)) {
            fail(format("ProcessInput 0x%08X for a sample of %zu bytes at %lld ms", unsigned(hr), size, (long long)(timeUs / 1000)));
            return false;
        }
        return takeOutputs();
    }

    void drain() {
        transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        takeOutputs();
    }

    void flush() {
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

megler_codec_host CodecHost::callbacks() {
    megler_codec_host host = {};
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

std::string CodecHost::summary() {
    std::string line;
    serviceWorker().run([&] {
        for (int kind = MEGLER_CODEC_H264; kind <= MEGLER_CODEC_MP3; kind++) {
            const Found found = decodersFor(kind);
            line += format("%s%s: %s", line.empty() ? "" : "; ", kindName(kind), found.names.empty() ? "none" : found.names[0].c_str());
        }
    });
    return line;
}

bool CodecHost::supportsThunk(void*, int kind) {
    // Asked once for each kind by MediaCodecList, and kept: what Windows has does not change while an app runs.
    static std::mutex mutex;
    static int known[8] = {};
    if (kind < 1 || kind > MEGLER_CODEC_MP3) return false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (known[kind]) return known[kind] > 0;
    }
    bool there = false;
    serviceWorker().run([&] {
        const Found found = decodersFor(kind);
        there = !found.activates.empty();
        std::string names;
        for (const std::string& n : found.names) names += (names.empty() ? "" : ", ") + n;
        shellLog::write(format("codec: %s decoders: %s", kindName(kind), there ? names.c_str() : "none"));
    });
    std::lock_guard<std::mutex> lock(mutex);
    known[kind] = there ? 1 : -1;
    return there;
}

void* CodecHost::openThunk(void*, int kind, const megler_codec_format* asked) {
    auto codec = std::make_unique<Codec>();
    codec->kind = kind;
    codec->worker = std::make_unique<Worker>();
    bool ok = false;
    Codec* c = codec.get();
    c->worker->run([&] {
        const Found found = decodersFor(kind);
        for (size_t i = 0; i < found.activates.size() && !ok; i++) {
            winrt::com_ptr<IMFTransform> transform;
            const HRESULT hr = found.activates[i]->ActivateObject(__uuidof(IMFTransform), transform.put_void());
            c->name = found.names[i];
            if (FAILED(hr)) {
                c->fail(format("not made (0x%08X)", unsigned(hr)));
                continue;
            }
            c->transform = transform;
            c->failures = 0;
            ok = c->configure(*asked);
            if (!ok) {
                c->transform = nullptr;
                found.activates[i]->ShutdownObject();
            }
        }
        if (ok) {
            if (isVideo(kind))
                shellLog::write(format("codec: an %s decoder, %s, for %dx%d%s", kindName(kind), c->name.c_str(), asked->width, asked->height,
                                       c->lowLatency ? ", low latency" : lowLatencyWanted() ? ", low latency refused" : ", low latency not asked for (flag)"));
            else
                shellLog::write(format("codec: an %s decoder, %s, for %d Hz in %d channels (gives %d Hz in %d)", kindName(kind), c->name.c_str(), asked->sample_rate,
                                       asked->channels, c->rate, c->outChannels));
        } else {
            shellLog::write(format("codec: no %s decoder could be made (%zu found)", kindName(kind), found.activates.size()));
        }
    });
    return ok ? codec.release() : nullptr;
}

bool CodecHost::pushThunk(void*, void* codec, const uint8_t* data, size_t size, int64_t timeUs) {
    auto* c = static_cast<Codec*>(codec);
    bool ok = false;
    c->worker->run([&] { ok = c->push(data, size, timeUs); });
    return ok;
}

void CodecHost::drainThunk(void*, void* codec) {
    auto* c = static_cast<Codec*>(codec);
    c->worker->run([c] { c->drain(); });
}

void* CodecHost::pullPictureThunk(void*, void* codec, megler_codec_picture* picture) {
    auto* c = static_cast<Codec*>(codec);
    std::lock_guard<std::mutex> lock(c->mutex);
    if (c->ready.empty()) return nullptr;
    Picture* p = c->ready.front();
    c->ready.pop_front();
    *picture = p->info;
    return p;
}

void CodecHost::releasePictureThunk(void*, void* picture) { delete static_cast<Picture*>(picture); }

bool CodecHost::pullSoundThunk(void*, void* codec, megler_codec_sound* sound) {
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
    c->worker->run([c] { c->flush(); });
}

void CodecHost::closeThunk(void*, void* codec) {
    auto* c = static_cast<Codec*>(codec);
    c->worker->run([c] {
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
    // The worker's thread is joined here, its work done; the transform was let go where it was made.
    delete c;
}

} // namespace megler::app
