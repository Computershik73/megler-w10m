#include "pch.h"

#include "player/AudioHost.h"

#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include <xaudio2.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace megler::app {

namespace {

/// The blocks XAudio2 may hold at once, each alive until it has played. Far
/// more than the 50 ms kept queued, so a block is never reused while queued.
constexpr int ringSize = 16;
/// How much may wait in XAudio2 before write() holds the mixer back.
constexpr int queuedMilliseconds = 50;
/// After a failure to build the engine, how long until the next try.
constexpr unsigned long long retryMilliseconds = 2000;

long long qpcNow() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

long long qpcFrequency() {
    static const long long f = [] {
        LARGE_INTEGER t;
        QueryPerformanceFrequency(&t);
        return t.QuadPart;
    }();
    return f;
}

/// XAudio2Create, with XAudio2_9.dll delay-loaded: a device without it gets
/// an HRESULT here instead of an app that fails to start. (The delay-load
/// helper raises VcppException(ERROR_SEVERITY_ERROR, ERROR_MOD_NOT_FOUND or
/// ERROR_PROC_NOT_FOUND); no C++ objects in this frame, for __try.)
HRESULT createXAudio2(IXAudio2** out) {
    __try {
        return XAudio2Create(out, 0, XAUDIO2_DEFAULT_PROCESSOR);
    } __except (GetExceptionCode() == 0xC06D007E || GetExceptionCode() == 0xC06D007F ? EXCEPTION_EXECUTE_HANDLER
                                                                                    : EXCEPTION_CONTINUE_SEARCH) {
        return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    }
}

} // namespace

/// XAudio2's side: the engine, a source voice of the mixer's format, and the
/// ring of blocks it plays. Built and destroyed on a thread that is in COM's
/// multithreaded apartment, fed from the mixer's; the callbacks arrive on
/// XAudio2's own and only signal.
struct AudioHost::Engine final : public IXAudio2VoiceCallback, public IXAudio2EngineCallback {
    AudioHost* owner = nullptr; // null: the sound check's own engine
    IXAudio2* xaudio = nullptr;
    IXAudio2MasteringVoice* master = nullptr;
    IXAudio2SourceVoice* source = nullptr;
    bool registered = false;
    int rate = 0;
    std::string device;          // what the mastering voice plays into, for the log
    HANDLE blockEnded = nullptr; // set whenever a block has played
    std::vector<int16_t> ring[ringSize];
    int ringFrames[ringSize] = {};
    int next = 0;

    /// Null, and why, when XAudio2 would not start. The calling thread is in
    /// the multithreaded apartment (without it CreateMasteringVoice is
    /// CO_E_NOTINITIALIZED).
    static std::unique_ptr<Engine> make(AudioHost* owner, int rate, std::string& why) {
        auto fail = [&why](const char* what, HRESULT hr) {
            why = format("%s (0x%08X)", what, unsigned(hr));
            return std::unique_ptr<Engine>();
        };
        auto engine = std::make_unique<Engine>();
        engine->owner = owner;
        engine->rate = rate;
        engine->blockEnded = CreateEventExW(nullptr, nullptr, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
        HRESULT hr = createXAudio2(&engine->xaudio);
        if (FAILED(hr)) return fail("XAudio2Create failed", hr);
        engine->registered = SUCCEEDED(engine->xaudio->RegisterForCallbacks(engine.get()));
        hr = engine->xaudio->CreateMasteringVoice(&engine->master);
        if (FAILED(hr)) return fail("there is no output device", hr);
        WAVEFORMATEX wave = {};
        wave.wFormatTag = WAVE_FORMAT_PCM;
        wave.nChannels = 2;
        wave.nSamplesPerSec = DWORD(rate);
        wave.wBitsPerSample = 16;
        wave.nBlockAlign = 4;
        wave.nAvgBytesPerSec = DWORD(rate) * 4;
        hr = engine->xaudio->CreateSourceVoice(&engine->source, &wave, 0, XAUDIO2_DEFAULT_FREQ_RATIO, engine.get());
        if (FAILED(hr)) return fail("CreateSourceVoice failed", hr);
        hr = engine->source->Start(0);
        if (FAILED(hr)) return fail("the voice didn't start", hr);
        XAUDIO2_VOICE_DETAILS details = {};
        engine->master->GetVoiceDetails(&details);
        DWORD mask = 0;
        engine->master->GetChannelMask(&mask);
        float volume = 0;
        engine->master->GetVolume(&volume);
        engine->device = format("an output of %u channels at %u Hz (channel mask 0x%X, XAudio2's own volume %.2f)",
                                unsigned(details.InputChannels), unsigned(details.InputSampleRate), unsigned(mask), double(volume));
        return engine;
    }

    ~Engine() {
        // Destroying a voice waits for its callbacks to return.
        if (source) source->DestroyVoice();
        if (master) master->DestroyVoice();
        if (xaudio) {
            if (registered) xaudio->UnregisterForCallbacks(this);
            xaudio->Release();
        }
        if (blockEnded) CloseHandle(blockEnded);
    }

    /// The frames of the blocks still queued: the last `queued` submitted.
    int queuedFrames(unsigned queued) const {
        int frames = 0;
        for (unsigned i = 0; i < queued && i < ringSize; ++i) frames += ringFrames[(next + ringSize - 1 - int(i)) % ringSize];
        return frames;
    }

    void lost(HRESULT error) {
        if (owner) {
            owner->lostCode_.store(long(error));
            owner->lost_.store(true);
            owner->wake_.notify_all();
        }
        if (blockEnded) SetEvent(blockEnded);
    }

    // IXAudio2VoiceCallback (XAudio2's thread; nothing here may block)
    void __stdcall OnVoiceProcessingPassStart(UINT32) override {}
    void __stdcall OnVoiceProcessingPassEnd() override {}
    void __stdcall OnStreamEnd() override {}
    void __stdcall OnBufferStart(void*) override {}
    void __stdcall OnBufferEnd(void*) override {
        if (blockEnded) SetEvent(blockEnded);
    }
    void __stdcall OnLoopEnd(void*) override {}
    void __stdcall OnVoiceError(void*, HRESULT error) override { lost(error); }

    // IXAudio2EngineCallback: a critical error -- the output device went
    // away or changed (headphones, Bluetooth) -- stops the engine for good;
    // the owner thread builds a new one.
    void __stdcall OnProcessingPassStart() override {}
    void __stdcall OnProcessingPassEnd() override {}
    void __stdcall OnCriticalError(HRESULT error) override { lost(error); }
};

AudioHost::AudioHost() = default;

AudioHost::~AudioHost() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    if (owner_.joinable()) owner_.join();
}

megler_audio_host AudioHost::callbacks() {
    megler_audio_host host = {};
    host.user = this;
    host.open = &AudioHost::openThunk;
    host.write = &AudioHost::writeThunk;
    return host;
}

bool AudioHost::openThunk(void* user, int rate) { return static_cast<AudioHost*>(user)->open(rate); }

void AudioHost::writeThunk(void* user, const int16_t* samples, int frames) {
    static_cast<AudioHost*>(user)->write(samples, frames);
}

void AudioHost::setStatus(const std::string& text) {
    std::lock_guard<std::mutex> lock(statusMutex_);
    status_ = text;
}

std::string AudioHost::status() {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return status_;
}

void AudioHost::prepare(int rate) {
    std::lock_guard<std::mutex> lock(mutex_);
    rate_.store(rate > 0 ? rate : 44100);
    if (!owner_.joinable()) owner_ = std::thread([this] { ownerLoop(); });
    wake_.notify_all();
}

bool AudioHost::open(int rate) {
    shellLog::write(format("audio: the app's first sound; the mixer asks for %d Hz", rate));
    prepare(rate);
    // The engine may not be there yet, or ever: write() takes the blocks at
    // their own pace either way, which is all the core needs to know.
    return true;
}

// MARK: The owner thread

void AudioHost::retire(std::shared_ptr<Engine>& engine) {
    if (!engine) return;
    // write() may be in the middle of a block with it (for a second at the
    // most, when the engine has stopped taking them); the engine is to go on
    // this thread, not on the mixer's.
    for (int i = 0; i < 300 && engine.use_count() > 1; ++i) Sleep(5);
    engine.reset();
}

void AudioHost::ownerLoop() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    unsigned long long retryAt = 0;
    unsigned failures = 0;
    for (;;) {
        std::shared_ptr<Engine> old;
        bool have = false, wasLost = false;
        int rate = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            const bool settled = engine_ ? engine_->rate == rate_.load() : GetTickCount64() < retryAt;
            if (!quit_ && !lost_.load() && settled) wake_.wait_for(lock, std::chrono::milliseconds(500));
            if (quit_) break;
            rate = rate_.load();
            wasLost = lost_.exchange(false);
            if (engine_ && (wasLost || engine_->rate != rate)) old = std::move(engine_);
            have = engine_ != nullptr;
        }
        if (old) {
            shellLog::write(wasLost ? format("audio: XAudio2 critical error 0x%08X (the output changed?); building a new engine",
                                             unsigned(lostCode_.load()))
                                    : std::string("audio: another rate is wanted; building a new engine"));
            retire(old);
        }
        if (!have && GetTickCount64() >= retryAt) {
            std::string why;
            const unsigned long long began = GetTickCount64();
            std::shared_ptr<Engine> made = Engine::make(this, rate, why);
            if (made) {
                shellLog::write(format("audio: XAudio2 playing 16-bit stereo at %d Hz into %s; built in %llu ms", rate,
                                       made->device.c_str(), GetTickCount64() - began));
                setStatus(format("%d Hz", rate));
                failures = 0;
                std::lock_guard<std::mutex> lock(mutex_);
                engine_ = std::move(made);
            } else {
                setStatus("no sound: " + why);
                // Said once, and again when it has gone on for a while.
                if (failures == 0 || failures % 30 == 0)
                    shellLog::write("audio: no sound: " + why + "; the app runs silent while this lasts");
                ++failures;
                retryAt = GetTickCount64() + retryMilliseconds;
            }
        }
        logCounts();
    }
    std::shared_ptr<Engine> last;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last = std::move(engine_);
    }
    retire(last);
    if (SUCCEEDED(com)) CoUninitialize();
}

void AudioHost::logCounts() {
    const uint64_t in = blocksIn_.load();
    if (in == countsLoggedIn_) return; // nothing has come since the last line
    const unsigned long long now = GetTickCount64();
    if (countsFirstAt_ == 0) {
        countsFirstAt_ = now;
        countsNextAt_ = now + 1000;
        return;
    }
    if (now < countsNextAt_) return;
    // A second after the first block, four seconds later, then every half minute.
    countsNextAt_ = now + (now - countsFirstAt_ < 3000 ? 4000 : 30000);
    countsLoggedIn_ = in;

    std::shared_ptr<Engine> engine;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        engine = engine_;
    }
    XAUDIO2_VOICE_STATE state = {};
    if (engine) engine->source->GetState(&state, 0);
    const int peak = peak_.exchange(0);
    shellLog::write(format("audio: %llu blocks from the mixer in %.1f s, %llu of them silence, peak %d of 32767 since the last line; "
                           "%llu given to XAudio2, %llu dropped at their own pace, %llu refused; %s",
                           (unsigned long long)in, double(now - countsFirstAt_) / 1000.0, (unsigned long long)blocksSilent_.load(), peak,
                           (unsigned long long)blocksQueued_.load(), (unsigned long long)blocksDropped_.load(),
                           (unsigned long long)blocksRefused_.load(),
                           engine ? format("XAudio2 has played %llu frames and holds %u blocks", (unsigned long long)state.SamplesPlayed,
                                           unsigned(state.BuffersQueued)).c_str()
                                  : "there is no engine"));
    engine.reset();
}

// MARK: The mixer's thread

void AudioHost::dropAtItsOwnPace(int frames) {
    // The block is dropped when its own time has passed, on a clock that adds
    // up (a Sleep per block would run slow by the system timer's grain).
    const long long now = qpcNow();
    if (paceDeadline_ == 0 || now - paceDeadline_ > qpcFrequency() / 5) paceDeadline_ = now;
    paceDeadline_ += (long long)frames * qpcFrequency() / rate_.load();
    if (paceDeadline_ > now) Sleep(DWORD((paceDeadline_ - now) * 1000 / qpcFrequency()));
    ++blocksDropped_;
}

void AudioHost::write(const int16_t* samples, int frames) {
    if (!samples || frames <= 0) return;

    int peak = 0;
    for (int i = 0; i < frames * 2; ++i) peak = std::max(peak, std::abs(int(samples[i])));
    ++blocksIn_;
    if (peak == 0) ++blocksSilent_;
    for (int seen = peak_.load(); peak > seen && !peak_.compare_exchange_weak(seen, peak);) {}

    std::shared_ptr<Engine> held;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        held = engine_;
    }
    // Nothing to play through, or nothing to be played.
    if (!held || paused_.load() || lost_.load()) {
        dropAtItsOwnPace(frames);
        return;
    }
    paceDeadline_ = 0;

    // Hold the mixer back while more than about 50 ms waits to be played.
    Engine& engine = *held;
    const int limit = engine.rate * queuedMilliseconds / 1000;
    XAUDIO2_VOICE_STATE state = {};
    for (int waits = 0; waits < 100; ++waits) {
        engine.source->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
        if (state.BuffersQueued < ringSize - 2 && engine.queuedFrames(state.BuffersQueued) <= limit) break;
        if (lost_.load() || paused_.load()) {
            ++blocksDropped_;
            return;
        }
        WaitForSingleObjectEx(engine.blockEnded, 10, FALSE);
    }
    // (A voice that plays nothing for a second -- the engine stopped under
    // us -- must not have the ring written over what it still holds.)
    if (state.BuffersQueued >= ringSize - 2) {
        if (blocksRefused_++ == 0) shellLog::write("audio: XAudio2 has stopped taking blocks: a second passed and none of those queued has played");
        return;
    }

    std::vector<int16_t>& block = engine.ring[engine.next];
    block.assign(samples, samples + size_t(frames) * 2);
    XAUDIO2_BUFFER buffer = {};
    buffer.AudioBytes = UINT32(block.size() * sizeof(int16_t));
    buffer.pAudioData = reinterpret_cast<const BYTE*>(block.data());
    const HRESULT hr = engine.source->SubmitSourceBuffer(&buffer);
    if (FAILED(hr)) {
        if (blocksRefused_++ == 0) shellLog::write(format("audio: XAudio2 refused a block, 0x%08X", unsigned(hr)));
        return;
    }
    engine.ringFrames[engine.next] = frames;
    engine.next = (engine.next + 1) % ringSize;
    ++blocksQueued_;
}

// MARK: The Device page's check

void AudioHost::testTone(const std::function<void(const std::string&)>& line, double level) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        constexpr int rate = 44100, frames = rate; // a second
        // (Declared before the engine: it outlives the voice that plays it.)
        std::vector<int16_t> tone(size_t(frames) * 2);
        std::string why;
        const unsigned long long began = GetTickCount64();
        line("sound: trying XAudio2");
        std::unique_ptr<Engine> engine = Engine::make(nullptr, rate, why);
        if (!engine) {
            line("sound: NOT AVAILABLE: " + why);
        } else {
            line(format("sound: XAudio2 into %s; built in %llu ms", engine->device.c_str(), GetTickCount64() - began));
            for (int i = 0; i < frames; ++i) {
                // Faded in and out over 20 ms, so it does not click.
                const double edge = std::min(1.0, double(std::min(i, frames - 1 - i)) / 882.0);
                const int16_t s = int16_t(std::sin(double(i) * 2.0 * 3.14159265358979 * 440.0 / double(rate)) * level * edge * 32767.0);
                tone[size_t(i) * 2] = tone[size_t(i) * 2 + 1] = s;
            }
            XAUDIO2_BUFFER buffer = {};
            buffer.AudioBytes = UINT32(tone.size() * sizeof(int16_t));
            buffer.pAudioData = reinterpret_cast<const BYTE*>(tone.data());
            line("sound: playing a 440 Hz tone for a second -- listen");
            const HRESULT hr = engine->source->SubmitSourceBuffer(&buffer);
            if (FAILED(hr)) {
                line(format("sound: XAudio2 refused the tone (0x%08X)", unsigned(hr)));
            } else {
                XAUDIO2_VOICE_STATE state = {};
                for (int i = 0; i < 300; ++i) { // three seconds at the most
                    engine->source->GetState(&state, 0);
                    if (state.BuffersQueued == 0) break;
                    WaitForSingleObjectEx(engine->blockEnded, 10, FALSE);
                }
                line(format("sound: XAudio2 says it played %llu of %d frames%s", (unsigned long long)state.SamplesPlayed, frames,
                            state.BuffersQueued ? " and then stopped" : ""));
                line("sound: if nothing was heard, look at the phone's own volume first: a volume key, then the arrow, then Media + Apps");
                // The tail leaves the device before the voice goes.
                Sleep(150);
            }
        }
    }
    if (SUCCEEDED(com)) CoUninitialize();
}

} // namespace megler::app
