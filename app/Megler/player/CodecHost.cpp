#include "pch.h"

#include "player/CodecHost.h"

#include "player/GfxHost.h"
#include "shell/Files.h"
#include "shell/Platform.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include <d3d10.h> // ID3D10Multithread
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

/// Pictures decoded on the graphics processor, as venetium-w10m's Chromium decodes them on ANGLE's device: the decoder is
/// given the device the app's views are drawn on (GfxHost's, made with the video interfaces) through a DXGI device manager
/// (MFT_MESSAGE_SET_D3D_MANAGER) and decodes there (DXVA); each picture is turned into B8G8R8A8 by the video processor
/// (which feature level 9_3 has: a shader there cannot sample NV12) into a texture of this file's own, and the decoder's
/// sample goes back to it at once, so its pool never runs dry however many pictures an app holds. A TextureView shows such
/// a picture from there (GfxHost::showVideoFrame); only where an app wants it in memory is it read back (read_picture).
/// The device's immediate context is Direct2D's too: every call that may use it is made under GfxHost's lock, and the
/// device is multithread-protected for what the decoder does on threads of its own.
struct GpuVideo {
    bool ok = false;
    std::string described;              // for the log: where, or why not
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> immediate;
    winrt::com_ptr<ID3D11VideoDevice> video;
    winrt::com_ptr<ID3D11VideoContext> videoContext;
    winrt::com_ptr<IMFDXGIDeviceManager> manager;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_9_1;
};

/// The flag file codec-no-gpu in LocalState\flags: pictures decoded in software, as before, to compare.
bool gpuWanted() {
    static const bool wanted = !files::exists(platform::localStateDirectory() + L"\\flags\\codec-no-gpu");
    return wanted;
}

/// Whether a decoder's own calls (Media Foundation's, which use the device's immediate context for its DXVA work) are made
/// under GfxHost's lock, as everything of this file's own that uses the context is: unless the flag file codec-gpu-unlocked
/// is in LocalState\flags, which leaves them to the device's multithread protection alone, so that Direct2D's drawing
/// does not wait for a decode -- to compare.
bool lockDecoderCalls() {
    static const bool locked = !files::exists(platform::localStateDirectory() + L"\\flags\\codec-gpu-unlocked");
    return locked;
}

uint64_t nowNs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

/// Made the first time a picture decoder asks after the app's first drawing (GfxHost has its device, or knows it has none,
/// from then on); a decoder asked for before that decodes in software, and the next one asks again. Said once in the log.
GpuVideo& gpuVideo() {
    static GpuVideo* const g = new GpuVideo(); // never destroyed: decoders may outlive everything else
    static std::mutex mutex;
    static std::atomic<bool> settled{false};
    if (settled.load(std::memory_order_acquire)) return *g;
    std::lock_guard<std::mutex> lock(mutex);
    if (settled.load(std::memory_order_relaxed)) return *g;
    {
        GfxHost& gfx = GfxHost::shared();
        ID3D11Device* device = gfx.device();
        if (!device && gpuWanted() && !gfx.counts().started) return *g; // (no drawing yet: asked again by the next decoder)
        if (!gpuWanted()) {
            g->described = "not asked for (the flag codec-no-gpu)";
        } else if (!device) {
            g->described = "no graphics processor's device (the views are drawn in memory)";
        } else if (!gfx.videoSupport()) {
            g->described = "the device has no video interfaces";
        } else {
            g->device.copy_from(device);
            g->device->GetImmediateContext(g->immediate.put());
            g->level = g->device->GetFeatureLevel();
            g->video = g->device.try_as<ID3D11VideoDevice>();
            g->videoContext = g->immediate.try_as<ID3D11VideoContext>();
            UINT token = 0;
            HRESULT hr = g->video && g->videoContext ? MFCreateDXGIDeviceManager(&token, g->manager.put()) : E_NOINTERFACE;
            if (SUCCEEDED(hr)) {
                if (auto mt = g->device.try_as<ID3D10Multithread>()) mt->SetMultithreadProtected(TRUE);
                hr = g->manager->ResetDevice(g->device.get(), token);
            }
            if (FAILED(hr)) {
                g->described = format("the device's video interfaces cannot be had (0x%08X)", unsigned(hr));
                g->manager = nullptr;
            } else {
                g->ok = true;
                g->described = format("on the graphics processor (DXVA, feature level %d_%d), shown from there", int(g->level) >> 12, (int(g->level) >> 8) & 15);
            }
        }
        shellLog::write("codec: pictures are decoded " + std::string(g->ok ? "" : "in software: ") + g->described);
    }
    settled.store(true, std::memory_order_release);
    return *g;
}

/// GfxHost's lock on the device, held for whatever may use its immediate context; nothing where `on` is false.
class DeviceLock {
public:
    explicit DeviceLock(bool on) : on_(on) {
        if (on_) GfxHost::shared().lockDevice();
    }
    ~DeviceLock() {
        if (on_) GfxHost::shared().unlockDevice();
    }
    DeviceLock(const DeviceLock&) = delete;
    DeviceLock& operator=(const DeviceLock&) = delete;

private:
    bool on_;
};

/// The textures a decoder's pictures are turned into, B8G8R8A8 of the pictures' visible size: one each while the picture is
/// held, then back here for the next (at most four kept). Shared with the pictures, which may outlive their decoder.
struct GpuPool {
    struct Slot {
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::com_ptr<ID3D11VideoProcessorOutputView> view; // made for the decoder's enumerator of that generation
        uint64_t madeFor = 0;
    };
    std::mutex mutex;
    int width = 0, height = 0;
    std::vector<Slot> free;
    winrt::com_ptr<ID3D11Texture2D> staging; // for read_picture, under GfxHost's lock

    /// A slot of width x height: a kept one, or a new texture; empty when none can be made.
    Slot take(ID3D11Device* device, int w, int h) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (w != width || h != height) {
                free.clear();
                staging = nullptr;
                width = w;
                height = h;
            }
            if (!free.empty()) {
                Slot s = std::move(free.back());
                free.pop_back();
                return s;
            }
        }
        Slot s;
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = UINT(w);
        desc.Height = UINT(h);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, s.texture.put()))) s.texture = nullptr;
        return s;
    }

    void give(Slot&& s) {
        if (!s.texture) return;
        D3D11_TEXTURE2D_DESC desc = {};
        s.texture->GetDesc(&desc);
        std::lock_guard<std::mutex> lock(mutex);
        if (int(desc.Width) == width && int(desc.Height) == height && free.size() < 4) free.push_back(std::move(s));
    }
};

} // namespace

/// A picture out of a decoder: its buffer held (locked) until the core lets it go, or a copy of it; or, decoded on the
/// graphics processor, a texture of its pool's (MEGLER_CODEC_GPU), and its memory once read_picture has read it there.
struct CodecHost::Picture {
    megler_codec_picture info = {};
    winrt::com_ptr<IMFMediaBuffer> buffer;
    winrt::com_ptr<IMF2DBuffer> planar; // when locked as one
    std::vector<uint8_t> copy;
    std::shared_ptr<GpuPool> pool;
    GpuPool::Slot slot;
    megler_codec_picture read = {}; // a picture on the graphics processor as read into `copy`

    ~Picture() {
        if (planar) planar->Unlock2D();
        else if (buffer) buffer->Unlock();
        if (pool) pool->give(std::move(slot));
    }
};

/// One decoder. `transform` and everything about it are the worker's alone; `mutex` guards what is waiting to be pulled.
struct CodecHost::Codec {
    int kind = 0;
    std::unique_ptr<Worker> worker;
    winrt::com_ptr<IMFTransform> transform;
    winrt::com_ptr<IMFActivate> activation;
    megler_codec_format initialFormat = {};
    std::vector<uint8_t> configuration;
    std::string name;
    DWORD in = 0, out = 0;           // stream ids
    bool decoderSamples = false;     // the decoder brings its own output samples
    bool lowLatency = false;         // MF_LOW_LATENCY was taken
    bool p010 = false;               // the pictures come as P010 (ten bits a sample), and are made NV12's eight here
    bool tenBitsFirst = false;       // P010 is taken where NV12 is offered too (CodecHost::preferTenBits, for the tests)
    DWORD outSize = 0, outAlign = 0; // of ours
    winrt::com_ptr<IMFSample> spare; // an output sample of ours the decoder did not fill

    // On the graphics processor (gpuVideo()): the decoder took the device, and what turns its pictures into colour there.
    bool gpu = false;
    bool gpuOff = false;             // the video processor failed: pictures read back from the decoder's buffers instead
    bool bigSaid = false;            // pictures too big to keep a copy of each on this feature level, said
    std::string gpuWhy;              // why it decodes in software where the device is there
    winrt::com_ptr<ID3D11VideoProcessorEnumerator> vpEnumerator;
    winrt::com_ptr<ID3D11VideoProcessor> vp;
    uint64_t vpGeneration = 0;       // which enumerator a pool slot's output view was made for
    int vpInWidth = 0, vpInHeight = 0, vpOutWidth = 0, vpOutHeight = 0;
    struct InputView {
        winrt::com_ptr<ID3D11Texture2D> texture;
        UINT slice = 0;
        winrt::com_ptr<ID3D11VideoProcessorInputView> view;
    };
    std::vector<InputView> inputViews; // the decoder's textures' slices, as the video processor reads them
    std::shared_ptr<GpuPool> pool = std::make_shared<GpuPool>();
    uint64_t gpuPictures = 0;
    // Every 5 s while it decodes, what its time went to (sayTimes): the decoder's calls, and the video processor's.
    uint64_t timesFrom = 0, callNs = 0, colourNs = 0, timesSamples = 0, timesPictures = 0, timesGpu = 0;

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

    /// The decoder given the graphics processor's device (gpuVideo()), where it takes one: it then decodes there (DXVA).
    /// Before its types are set, as Media Foundation asks. Else it decodes in software, as before, and `gpuWhy` says why.
    void useGpu() {
        gpu = gpuOff = false;
        gpuWhy.clear();
        dropProcessor();
        if (!isVideo(kind) || !transform) return;
        GpuVideo& g = gpuVideo();
        if (!g.ok) return;
        winrt::com_ptr<IMFAttributes> attributes;
        if (FAILED(transform->GetAttributes(attributes.put())) || !attributes || !MFGetAttributeUINT32(attributes.get(), MF_SA_D3D11_AWARE, FALSE)) {
            gpuWhy = "it takes no Direct3D 11 device";
            return;
        }
        step = "ProcessMessage(SET_D3D_MANAGER)";
        DeviceLock lock(true);
        const HRESULT hr = transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, ULONG_PTR(g.manager.get()));
        if (FAILED(hr)) {
            gpuWhy = format("it refused the device (0x%08X)", unsigned(hr));
            return;
        }
        gpu = true;
    }

    /// The decoder in software again, after it would not be set up with the device.
    void leaveGpu(const std::string& why) {
        if (!gpu) return;
        DeviceLock lock(true);
        transform->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
        gpu = false;
        gpuWhy = why;
        dropProcessor();
    }

    void dropProcessor() {
        vp = nullptr;
        vpEnumerator = nullptr;
        inputViews.clear();
        vpInWidth = vpInHeight = vpOutWidth = vpOutHeight = 0;
    }

    /// The video processor for the pictures as the output type has them now: the decoded size in, the visible part out at
    /// its own size, B8G8R8A8; colour as the stream says (BT.601 or BT.709, video or full range, the same defaults as the
    /// core's own conversion), and nothing of the driver's own (no "enhancements"). Under GfxHost's lock.
    bool videoProcessor() {
        if (vp && vpInWidth == frameWidth && vpInHeight == frameHeight && vpOutWidth == visibleWidth && vpOutHeight == visibleHeight) return true;
        dropProcessor();
        GpuVideo& g = gpuVideo();
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc = {};
        desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        desc.InputFrameRate = {30, 1};
        desc.InputWidth = UINT(frameWidth);
        desc.InputHeight = UINT(frameHeight);
        desc.OutputFrameRate = {30, 1};
        desc.OutputWidth = UINT(visibleWidth);
        desc.OutputHeight = UINT(visibleHeight);
        desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        step = "CreateVideoProcessor";
        HRESULT hr = g.video->CreateVideoProcessorEnumerator(&desc, vpEnumerator.put());
        UINT in = 0, out = 0;
        if (SUCCEEDED(hr)) hr = vpEnumerator->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &in);
        if (SUCCEEDED(hr)) hr = vpEnumerator->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &out);
        if (SUCCEEDED(hr) && (!(in & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) || !(out & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))) hr = E_NOTIMPL;
        if (SUCCEEDED(hr)) hr = g.video->CreateVideoProcessor(vpEnumerator.get(), 0, vp.put());
        if (FAILED(hr)) {
            fail(format("no video processor from NV12 %dx%d to B8G8R8A8 %dx%d (0x%08X): its pictures are read back instead", frameWidth, frameHeight, visibleWidth,
                        visibleHeight, unsigned(hr)));
            dropProcessor();
            gpuOff = true;
            return false;
        }
        vpGeneration++;
        vpInWidth = frameWidth;
        vpInHeight = frameHeight;
        vpOutWidth = visibleWidth;
        vpOutHeight = visibleHeight;
        const bool b709 = matrix == MEGLER_CODEC_BT709 || (matrix != MEGLER_CODEC_BT601 && visibleHeight >= 720);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE from = {}, to = {};
        from.YCbCr_Matrix = b709 ? 1 : 0;
        from.Nominal_Range = fullRange ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255 : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        to.RGB_Range = 0; // 0 to 255
        g.videoContext->VideoProcessorSetStreamColorSpace(vp.get(), 0, &from);
        g.videoContext->VideoProcessorSetOutputColorSpace(vp.get(), &to);
        g.videoContext->VideoProcessorSetStreamFrameFormat(vp.get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
        g.videoContext->VideoProcessorSetStreamAutoProcessingMode(vp.get(), 0, FALSE);
        const RECT source = {cropX, cropY, cropX + visibleWidth, cropY + visibleHeight}, whole = {0, 0, visibleWidth, visibleHeight};
        g.videoContext->VideoProcessorSetStreamSourceRect(vp.get(), 0, TRUE, &source);
        g.videoContext->VideoProcessorSetStreamDestRect(vp.get(), 0, TRUE, &whole);
        g.videoContext->VideoProcessorSetOutputTargetRect(vp.get(), TRUE, &whole);
        return true;
    }

    /// The video processor's view of a slice of one of the decoder's textures, made once (the decoder's pool is a few
    /// texture arrays that stay as they are until the output type changes).
    ID3D11VideoProcessorInputView* inputView(ID3D11Texture2D* texture, UINT slice) {
        for (const InputView& v : inputViews)
            if (v.texture.get() == texture && v.slice == slice) return v.view.get();
        if (inputViews.size() >= 64) inputViews.clear();
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc = {};
        desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        desc.Texture2D.ArraySlice = slice;
        InputView v;
        v.texture.copy_from(texture);
        v.slice = slice;
        if (FAILED(gpuVideo().video->CreateVideoProcessorInputView(texture, vpEnumerator.get(), &desc, v.view.put()))) return nullptr;
        inputViews.push_back(std::move(v));
        return inputViews.back().view.get();
    }

    /// A picture the decoder made on the graphics processor, turned into colour there, into a texture of the pool's, and
    /// queued as MEGLER_CODEC_GPU; the decoder's sample is let go as this returns. False where the sample is not on the
    /// graphics processor or that cannot be done: it is then read back as any other.
    bool takeGpuPicture(IMFSample* sample) {
        winrt::com_ptr<IMFMediaBuffer> buffer;
        if (FAILED(sample->GetBufferByIndex(0, buffer.put()))) return false;
        auto dxgi = buffer.try_as<IMFDXGIBuffer>();
        winrt::com_ptr<ID3D11Texture2D> texture;
        UINT slice = 0;
        if (!dxgi || FAILED(dxgi->GetResource(__uuidof(ID3D11Texture2D), texture.put_void())) || FAILED(dxgi->GetSubresourceIndex(&slice))) return false;
        // (The video processor uses the immediate context: under GfxHost's lock, however the decoder's own calls are made.)
        DeviceLock device(true);
        const uint64_t from = nowNs();
        struct Spent {
            uint64_t& into;
            uint64_t from;
            ~Spent() { into += nowNs() - from; }
        } spent{colourNs, from};
        if (!videoProcessor()) return false;
        GpuVideo& g = gpuVideo();
        ID3D11VideoProcessorInputView* input = inputView(texture.get(), slice);
        GpuPool::Slot out = pool->take(g.device.get(), visibleWidth, visibleHeight);
        if (out.texture && (!out.view || out.madeFor != vpGeneration)) {
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC desc = {};
            desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            out.view = nullptr;
            if (FAILED(g.video->CreateVideoProcessorOutputView(out.texture.get(), vpEnumerator.get(), &desc, out.view.put()))) out.view = nullptr;
            out.madeFor = vpGeneration;
        }
        if (!input || !out.texture || !out.view) {
            fail(format("a picture of %dx%d on the graphics processor has no %s there: read back instead", visibleWidth, visibleHeight,
                        !input ? "input view" : !out.texture ? "texture to be turned into" : "output view"));
            return false;
        }
        D3D11_VIDEO_PROCESSOR_STREAM stream = {};
        stream.Enable = TRUE;
        stream.pInputSurface = input;
        step = "VideoProcessorBlt";
        const HRESULT hr = g.videoContext->VideoProcessorBlt(vp.get(), out.view.get(), 0, 1, &stream);
        if (FAILED(hr)) {
            fail(format("VideoProcessorBlt 0x%08X: its pictures are read back instead", unsigned(hr)));
            gpuOff = true;
            return false;
        }
        LONGLONG time = 0;
        if (FAILED(sample->GetSampleTime(&time))) time = lastInputUs * 10;
        auto picture = std::make_unique<Picture>();
        picture->pool = pool;
        picture->slot = std::move(out);
        megler_codec_picture& p = picture->info;
        p.format = MEGLER_CODEC_GPU;
        p.width = visibleWidth;
        p.height = visibleHeight;
        p.matrix = matrix;
        p.full_range = fullRange;
        p.time_us = time / 10;
        if (!pictures++)
            shellLog::write(format("codec: the first %s picture from %s, %dx%d on the graphics processor (decoded %dx%d, slice %u of the decoder's texture), at %lld ms",
                                   kindName(kind), name.c_str(), visibleWidth, visibleHeight, frameWidth, frameHeight, slice, (long long)(time / 10000)));
        gpuPictures++;
        std::lock_guard<std::mutex> lock(mutex);
        ready.push_back(picture.release());
        return true;
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
        // (A new output type is new textures of the decoder's, maybe of a new size: the video processor is made again.)
        dropProcessor();
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
            matrix = m == MFVideoTransferMatrix_BT709 ? MEGLER_CODEC_BT709 : m == MFVideoTransferMatrix_BT601 ? MEGLER_CODEC_BT601 : 0;
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
    bool configure(const megler_codec_format& f) {
        DeviceLock lock(gpu && lockDecoderCalls());
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

    /// A decoded picture into the queue: on the graphics processor, where it was decoded there (takeGpuPicture); else its
    /// buffer held, or (a decoder's own buffer, the graphics processor's too) copied.
    void takePicture(IMFSample* sample) {
        if (gpu && !gpuOff) {
            // Below feature level 10_0 every picture held is a B8G8R8A8 copy of its own, four bytes a pixel, as on
            // venetium-w10m: up to 1280x720 there (an app holds up to eight, 30 MB); larger ones are read back as NV12.
            if (gpuVideo().level < D3D_FEATURE_LEVEL_10_0 && size_t(visibleWidth) * size_t(visibleHeight) > size_t(1280) * 720) {
                if (!bigSaid)
                    shellLog::write(format("codec: %s pictures of %dx%d are decoded on the graphics processor and read back: at feature level 9_3 a copy "
                                           "of each held there would be %d MB",
                                           kindName(kind), visibleWidth, visibleHeight, visibleWidth * visibleHeight * 4 >> 20));
                bigSaid = true;
            } else if (takeGpuPicture(sample)) {
                return;
            }
        }
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
        // The chroma follows the luma of all the rows there are: a picture on the graphics processor (read back here, the
        // buffer mapping its texture) has its texture's height of them, which may be more than the output type's (a
        // decoder's textures are of whole macroblocks).
        size_t lumaRows = size_t(frameHeight);
        if (auto dxgi = buffer.try_as<IMFDXGIBuffer>()) {
            winrt::com_ptr<ID3D11Texture2D> texture;
            D3D11_TEXTURE2D_DESC desc = {};
            if (SUCCEEDED(dxgi->GetResource(__uuidof(ID3D11Texture2D), texture.put_void())) && texture) {
                texture->GetDesc(&desc);
                if (int(desc.Height) > frameHeight) lumaRows = desc.Height;
            }
        }
        if (auto planar = buffer.try_as<IMF2DBuffer>(); planar && SUCCEEDED(planar->Lock2D(&base, &pitch)) && base && pitch > 0) {
            picture->planar = planar;
            length = DWORD(pitch) * DWORD(lumaRows) * 3 / 2;
        } else if (SUCCEEDED(buffer->Lock(&base, nullptr, &length)) && base) {
            pitch = stride;
            lumaRows = size_t(frameHeight);
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
        const BYTE* chroma = base + lumaRows * size_t(pitch) + size_t(cropY / 2) * size_t(pitch) + size_t(cropX) * bytes;
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

    /// Every 5 s of decoding, a line of what the time went to: the decoder's calls (under GfxHost's lock, or not: the flag
    /// codec-gpu-unlocked), and the video processor's turning its pictures into colour; where a picture decoder's time goes
    /// is where an app's drawing waits.
    void sayTimes() {
        const uint64_t now = nowNs();
        if (!timesFrom) timesFrom = now;
        if (now - timesFrom < 5000000000ull || !isVideo(kind)) return;
        const double ms = 1e-6;
        shellLog::write(format("codec: %s (%s), %.1f s: %llu samples, %llu pictures (%llu on the graphics processor); %.0f ms in the decoder's calls%s, %.0f ms "
                               "turning pictures into colour",
                               kindName(kind), name.c_str(), double(now - timesFrom) * 1e-9, (unsigned long long)timesSamples, (unsigned long long)timesPictures,
                               (unsigned long long)timesGpu, double(callNs - std::min(callNs, colourNs)) * ms,
                               gpu ? (lockDecoderCalls() ? " (holding the device's lock)" : " (not holding the device's lock)") : "", double(colourNs) * ms));
        timesFrom = now;
        callNs = colourNs = timesSamples = timesPictures = timesGpu = 0;
    }

    bool push(const uint8_t* data, size_t size, int64_t timeUs) {
        if (!transform) { fail("no active decoder"); return false; }
        const uint64_t from = nowNs(), picturesBefore = pictures, gpuBefore = gpuPictures;
        struct Said {
            Codec& c;
            uint64_t from, pictures, gpu;
            ~Said() {
                c.callNs += nowNs() - from;
                c.timesSamples++;
                c.timesPictures += c.pictures - pictures;
                c.timesGpu += c.gpuPictures - gpu;
                c.sayTimes();
            }
        } said{*this, from, picturesBefore, gpuBefore};
        // (A decoder on the graphics processor uses the device's immediate context, which Direct2D draws with too.)
        DeviceLock lock(gpu && lockDecoderCalls());
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
        if (!transform) return;
        DeviceLock lock(gpu && lockDecoderCalls());
        step = "ProcessMessage(COMMAND_DRAIN)";
        transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        takeOutputs();
    }

    void flush() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (Picture* p : ready) delete p;
            ready.clear();
            sound.clear();
        }
        // On Mobile's Microsoft H265 MFT, COMMAND_FLUSH leaves a black first
        // picture and drops the tail of the next GOP (also on the Lumia 950 XL).
        // Recreate the MFT, with owned configuration bytes, for an independent
        // decode after a seek. All COM work stays on this codec's timed worker.
        DeviceLock lock(gpu && lockDecoderCalls());
        if (kind == MEGLER_CODEC_H265 && activation) {
            spare = nullptr;
            transform = nullptr;
            step = "ShutdownObject after HEVC flush";
            activation->ShutdownObject();
            step = "ActivateObject after HEVC flush";
            const HRESULT hr = activation->ActivateObject(__uuidof(IMFTransform), transform.put_void());
            initialFormat.config = configuration.empty() ? nullptr : configuration.data();
            initialFormat.config_size = configuration.size();
            const bool wasGpu = gpu;
            if (SUCCEEDED(hr) && wasGpu) useGpu();
            if (FAILED(hr) || !configure(initialFormat)) {
                fail("could not recreate HEVC decoder after flush");
                transform = nullptr;
                gpu = false;
            } else {
                shellLog::write(std::string("codec: HEVC decoder recreated after flush") + (wasGpu && !gpu ? ", in software now (" + gpuWhy + ")" : ""));
            }
        } else if (transform) {
            step = "ProcessMessage(COMMAND_FLUSH)";
            transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        }
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
    host.read_picture = &readPictureThunk;
    host.show_picture = &showPictureThunk;
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
        for (int kind = MEGLER_CODEC_H264; kind <= MEGLER_CODEC_MP3; kind++) {
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
    if (kind < 1 || kind > MEGLER_CODEC_MP3) return false;
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

void* CodecHost::openThunk(void*, int kind, const megler_codec_format* asked) {
    auto codec = std::make_unique<Codec>();
    codec->kind = kind;
    codec->tenBitsFirst = gTenBitsFirst;
    codec->worker = std::make_unique<Worker>();
    auto ok = std::make_shared<bool>(false);
    Codec* c = codec.get();
    // The format, and the configuration bytes it points at, the work's own: it may outlive this call.
    megler_codec_format wanted = *asked;
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
            c->useGpu();
            *ok = c->configure(wanted);
            if (!*ok && c->gpu) {
                // (Set up for the graphics processor it would not be: in software, as before.)
                c->leaveGpu("its types were refused with the device");
                c->failures = 0;
                *ok = c->configure(wanted);
            }
            if (*ok) {
                c->activation = found.activates[i];
                c->initialFormat = wanted;
                c->configuration = *config;
                c->initialFormat.config = c->configuration.empty() ? nullptr : c->configuration.data();
            }
            if (!*ok) {
                c->transform = nullptr;
                found.activates[i]->ShutdownObject();
            }
        }
        if (*ok) {
            if (isVideo(kind))
                shellLog::write(format("codec: an %s decoder, %s, for %dx%d%s, %s", kindName(kind), c->name.c_str(), wanted.width, wanted.height,
                                       c->lowLatency ? ", low latency" : lowLatencyWanted() ? ", low latency refused" : ", low latency not asked for (flag)",
                                       c->gpu ? "on the graphics processor"
                                       : c->gpuWhy.empty() ? "in software"
                                                           : ("in software: " + c->gpuWhy).c_str()));
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

bool CodecHost::readPictureThunk(void*, void* picture, megler_codec_picture* out) {
    auto* p = static_cast<Picture*>(picture);
    if (p->info.format != MEGLER_CODEC_GPU) {
        *out = p->info;
        return true;
    }
    if (p->read.data) {
        *out = p->read;
        return true;
    }
    GpuVideo& g = gpuVideo();
    if (!p->slot.texture || !p->pool || !g.ok) return false;
    const int w = p->info.width, h = p->info.height;
    bool ok = false;
    {
        // One staging texture of the pool's, the picture copied into it and mapped (which waits for the copy).
        DeviceLock lock(true);
        std::lock_guard<std::mutex> staging(p->pool->mutex);
        winrt::com_ptr<ID3D11Texture2D>& s = p->pool->staging;
        D3D11_TEXTURE2D_DESC desc = {};
        if (s) s->GetDesc(&desc);
        if (!s || int(desc.Width) != w || int(desc.Height) != h) {
            s = nullptr;
            desc = {};
            desc.Width = UINT(w);
            desc.Height = UINT(h);
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_STAGING;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(g.device->CreateTexture2D(&desc, nullptr, s.put()))) s = nullptr;
        }
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (s) {
            g.immediate->CopyResource(s.get(), p->slot.texture.get());
            if (SUCCEEDED(g.immediate->Map(s.get(), 0, D3D11_MAP_READ, 0, &mapped)) && mapped.pData) {
                p->copy.resize(size_t(w) * size_t(h) * 4);
                for (int y = 0; y < h; y++)
                    memcpy(p->copy.data() + size_t(y) * size_t(w) * 4, static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, size_t(w) * 4);
                g.immediate->Unmap(s.get(), 0);
                ok = true;
            }
        }
    }
    if (!ok) return false;
    p->read = p->info;
    p->read.format = MEGLER_CODEC_BGRA;
    p->read.data = p->copy.data();
    p->read.stride = w * 4;
    *out = p->read;
    return true;
}

bool CodecHost::showPictureThunk(void*, void* picture, int textureId) {
    auto* p = static_cast<Picture*>(picture);
    return p->info.format == MEGLER_CODEC_GPU && p->slot.texture &&
           GfxHost::shared().showVideoFrame(textureId, p->slot.texture.get(), p->info.width, p->info.height);
}

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
        DeviceLock lock(c->gpu && lockDecoderCalls());
        if (c->transform) {
            c->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            c->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        }
        shellLog::write(format("codec: an %s decoder (%s) closed after %llu samples, %llu pictures (%llu of them on the graphics processor), %llu blocks of "
                               "sound, %llu failures",
                               kindName(c->kind), c->name.c_str(), (unsigned long long)c->pushed, (unsigned long long)c->pictures,
                               (unsigned long long)c->gpuPictures, (unsigned long long)c->blocks, (unsigned long long)c->failures));
        c->spare = nullptr;
        c->dropProcessor();
        c->transform = nullptr;
        c->activation = nullptr;
    });
    if (!ran) {
        c->gaveUp("closing it");
        return;
    }
    // The worker's thread is joined here, its work done; the transform was let go where it was made.
    delete c;
}

} // namespace megler::app
