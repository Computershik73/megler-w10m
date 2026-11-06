#include "pch.h"

#include "player/AudioHost.h"

#include "shell/Files.h"
#include "shell/Platform.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include <xaudio2.h>
#include <eventtoken.h>
#include <roapi.h>

#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.MediaProperties.h>
#include <winrt/Windows.Media.Playback.h>

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

/*
 * Windows.Phone.Media.Devices.AudioRoutingManager, the phone's own: whether a call's sound is in the earpiece or the
 * loudspeaker. Declared here from the phone contract's metadata (winmdidl's IDL of Windows.Phone.PhoneContract.winmd): the
 * SDK's C++/WinRT headers have no projection of the contract. It routes sound of the Communications category.
 */
struct __declspec(uuid("79340D20-71CC-4526-9F29-FC8D2486418B")) __declspec(novtable) IAudioRoutingManager : ::IInspectable {
    virtual HRESULT __stdcall GetAudioEndpoint(int* endpoint) = 0;
    virtual HRESULT __stdcall SetAudioEndpoint(int endpoint) = 0;
    virtual HRESULT __stdcall add_AudioEndpointChanged(void* handler, EventRegistrationToken* token) = 0;
    virtual HRESULT __stdcall remove_AudioEndpointChanged(EventRegistrationToken token) = 0;
    virtual HRESULT __stdcall get_AvailableAudioEndpoints(unsigned* value) = 0;
};
struct __declspec(uuid("977FB2A4-5590-4A6F-ADDE-6A3D0AD58250")) __declspec(novtable) IAudioRoutingManagerStatics : ::IInspectable {
    virtual HRESULT __stdcall GetDefault(IAudioRoutingManager** manager) = 0;
};
enum { endpointDefault = 0, endpointEarpiece = 1, endpointSpeakerphone = 2 };

/// The phone's endpoint for a call's sound; what went wrong, or empty. Not on a PC, which has no such class.
std::string setPhoneEndpoint(int endpoint) {
    const winrt::hstring name = L"Windows.Phone.Media.Devices.AudioRoutingManager";
    winrt::com_ptr<IAudioRoutingManagerStatics> statics;
    winrt::com_ptr<IAudioRoutingManager> manager;
    HRESULT hr = RoGetActivationFactory(static_cast<HSTRING>(winrt::get_abi(name)), __uuidof(IAudioRoutingManagerStatics), statics.put_void());
    if (FAILED(hr)) return format("there is no AudioRoutingManager (0x%08X)", unsigned(hr));
    hr = statics->GetDefault(manager.put());
    if (FAILED(hr) || !manager) return format("AudioRoutingManager.GetDefault failed (0x%08X)", unsigned(hr));
    hr = manager->SetAudioEndpoint(endpoint);
    if (FAILED(hr)) return format("SetAudioEndpoint(%d) failed (0x%08X)", endpoint, unsigned(hr));
    return std::string();
}

std::atomic<int> gCallRoute{0};

} // namespace

int AudioHost::callRoute() { return gCallRoute.load(); }

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
    int route = 0;               // MEGLER_AUDIO_ROUTE_*: a call's (Communications) when not the default
    std::string device;          // what the mastering voice plays into, for the log
    HANDLE blockEnded = nullptr; // set whenever a block has played
    std::vector<int16_t> ring[ringSize];
    int ringFrames[ringSize] = {};
    int next = 0;

    /// Null, and why, when XAudio2 would not start. The calling thread is in
    /// the multithreaded apartment (without it CreateMasteringVoice is
    /// CO_E_NOTINITIALIZED).
    static std::unique_ptr<Engine> make(AudioHost* owner, int rate, std::string& why, bool media = false, int route = 0) {
        auto fail = [&why](const char* what, HRESULT hr) {
            why = format("%s (0x%08X)", what, unsigned(hr));
            return std::unique_ptr<Engine>();
        };
        auto engine = std::make_unique<Engine>();
        engine->owner = owner;
        engine->rate = rate;
        engine->route = route;
        engine->blockEnded = CreateEventExW(nullptr, nullptr, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
        HRESULT hr = createXAudio2(&engine->xaudio);
        if (FAILED(hr)) return fail("XAudio2Create failed", hr);
        engine->registered = SUCCEEDED(engine->xaudio->RegisterForCallbacks(engine.get()));
        // A media app's sound is of the Media category: with the backgroundMediaPlayback capability and the system media
        // controls (player/MediaControls.cpp) it goes on while the app is in the background. A game's stays a game's. A
        // call's is of Communications, which the phone routes to the earpiece or the loudspeaker (AudioRoutingManager).
        hr = engine->xaudio->CreateMasteringVoice(&engine->master, XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0, nullptr,
                                                  nullptr, route ? AudioCategory_Communications : media ? AudioCategory_Media : AudioCategory_GameEffects);
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

/*
 * Windows.Media's player, playing what the mixer writes: a MediaStreamSource of 16-bit stereo PCM at the mixer's rate, whose
 * samples are the blocks queued here, 20 ms at the most each. The player asks for a sample on a thread of its own
 * (SampleRequested): one is made of what is queued, or the request waits (a deferral) for write() to bring the next block.
 * write() holds the mixer back while more than about 50 ms waits to be taken, as it does for XAudio2; a player that has
 * taken nothing for a second (Windows paused it: a call, another app's sound) has the blocks dropped at their own pace
 * until it takes again. Made and let go of by the owner thread; the player's own controls are off, the system media
 * controls being the app's (MediaControls).
 */
struct AudioHost::MediaOut {
    int rate = 0;
    winrt::Windows::Media::Core::MediaStreamSource source{nullptr};
    winrt::Windows::Media::Playback::MediaPlayer player{nullptr};
    winrt::event_token requested{}, starting{}, failed{}, stateChanged{};
    std::mutex m; // all below
    std::condition_variable taken; // a sample went to the player, or it is closing or broken
    std::vector<int16_t> queue;    // interleaved stereo, from `head` on
    size_t head = 0;
    winrt::Windows::Media::Core::MediaStreamSourceSampleRequest pending{nullptr};
    winrt::Windows::Media::Core::MediaStreamSourceSampleRequestDeferral deferral{nullptr};
    int64_t nextTime = 0; // the next sample's time in the stream, in 100 ns
    bool closing = false;
    bool stalled = false; // nothing taken for a second: write() does not wait for it
    unsigned stalls = 0;
    std::atomic<bool> broken{false};
    std::atomic<uint64_t> framesTaken{0};
    std::atomic<unsigned> stateLines{0};

    size_t queuedFrames() const { return (queue.size() - head) / 2; }

    /// Under m, with something queued: the next sample.
    winrt::Windows::Media::Core::MediaStreamSample take() {
        const size_t frames = std::min(queuedFrames(), size_t(rate / 50));
        winrt::Windows::Storage::Streams::Buffer buffer(uint32_t(frames * 4));
        buffer.Length(uint32_t(frames * 4));
        memcpy(buffer.data(), queue.data() + head, frames * 4);
        head += frames * 2;
        if (head * 2 > queue.size()) {
            queue.erase(queue.begin(), queue.begin() + ptrdiff_t(head));
            head = 0;
        }
        auto sample = winrt::Windows::Media::Core::MediaStreamSample::CreateFromBuffer(buffer, winrt::Windows::Foundation::TimeSpan{nextTime});
        const int64_t duration = int64_t(frames) * 10000000 / rate;
        sample.Duration(winrt::Windows::Foundation::TimeSpan{duration});
        nextTime += duration;
        framesTaken += frames;
        if (stalled) {
            stalled = false;
            shellLog::write("audio: Windows.Media's player takes the app's sound again");
        }
        return sample;
    }
};

namespace {

const char* playbackStateName(winrt::Windows::Media::Playback::MediaPlaybackState state) {
    using winrt::Windows::Media::Playback::MediaPlaybackState;
    switch (state) {
    case MediaPlaybackState::Opening: return "opening";
    case MediaPlaybackState::Buffering: return "buffering";
    case MediaPlaybackState::Playing: return "playing";
    case MediaPlaybackState::Paused: return "paused";
    default: return "none";
    }
}

} // namespace

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
    host.route = &AudioHost::routeThunk;
    return host;
}

void AudioHost::routeThunk(void* user, int route) {
    AudioHost* self = static_cast<AudioHost*>(user);
    if (route < MEGLER_AUDIO_ROUTE_DEFAULT || route > MEGLER_AUDIO_ROUTE_SPEAKER) route = MEGLER_AUDIO_ROUTE_DEFAULT;
    gCallRoute.store(route);
    if (self->route_.exchange(route) == route) return;
    shellLog::write(format("audio: the app's sound is to go %s", route == MEGLER_AUDIO_ROUTE_EARPIECE  ? "to the earpiece, for a call"
                                                                 : route == MEGLER_AUDIO_ROUTE_SPEAKER ? "to the loudspeaker, for a call"
                                                                                                       : "where it always does"));
    std::lock_guard<std::mutex> lock(self->mutex_);
    self->wake_.notify_all();
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

std::shared_ptr<AudioHost::MediaOut> AudioHost::makeMedia(int rate, std::string& why) {
    using namespace winrt::Windows::Media;
    using winrt::Windows::Foundation::TimeSpan;
    try {
        auto out = std::make_shared<MediaOut>();
        out->rate = rate;
        Core::AudioStreamDescriptor descriptor(MediaProperties::AudioEncodingProperties::CreatePcm(uint32_t(rate), 2, 16));
        out->source = Core::MediaStreamSource(descriptor);
        out->source.CanSeek(false);
        // No time buffered ahead: the mixer's blocks are the app's clock (a video's pictures go by them).
        out->source.BufferTime(TimeSpan{0});
        std::weak_ptr<MediaOut> weak = out;
        out->starting = out->source.Starting([weak](auto&&, Core::MediaStreamSourceStartingEventArgs const& e) {
            // The stream cannot seek: asked to start somewhere (the first time, or after Windows paused it), it starts
            // where its next sample is.
            auto request = e.Request();
            if (!request.StartPosition()) return;
            int64_t at = 0;
            if (auto self = weak.lock()) {
                std::lock_guard<std::mutex> lock(self->m);
                at = self->nextTime;
            }
            request.SetActualStartPosition(TimeSpan{at});
        });
        out->requested = out->source.SampleRequested([weak](auto&&, Core::MediaStreamSourceSampleRequestedEventArgs const& e) {
            auto self = weak.lock();
            if (!self) return;
            auto request = e.Request();
            std::unique_lock<std::mutex> lock(self->m);
            if (self->closing) return; // (no sample: the stream's end)
            if (self->queuedFrames()) {
                request.Sample(self->take());
                lock.unlock();
                self->taken.notify_all();
                return;
            }
            self->pending = request;
            self->deferral = request.GetDeferral();
        });
        out->player = Playback::MediaPlayer();
        // The system media controls are the app's (MediaControls): a pause from the lock screen goes to the app, not to the
        // player, which would stop taking the app's sound while the app went on playing.
        out->player.CommandManager().IsEnabled(false);
        out->player.AudioCategory(Playback::MediaPlayerAudioCategory::Media);
        out->player.RealTimePlayback(true);
        AudioHost* host = this;
        out->failed = out->player.MediaFailed([weak, host](auto&&, Playback::MediaPlayerFailedEventArgs const& e) {
            shellLog::write(format("audio: Windows.Media's player failed: error %d (0x%08X) ", int(e.Error()), unsigned(e.ExtendedErrorCode().value)) +
                            narrow(std::wstring(e.ErrorMessage())));
            if (auto self = weak.lock()) {
                self->broken.store(true);
                self->taken.notify_all();
            }
            host->wake_.notify_all();
        });
        out->stateChanged = out->player.PlaybackSession().PlaybackStateChanged([weak](Playback::MediaPlaybackSession const& session, auto&&) {
            auto self = weak.lock();
            // (The first forty: a player that pauses and plays all the time is said by then.)
            if (!self || self->stateLines++ >= 40) return;
            shellLog::write(std::string("audio: Windows.Media's player is ") + playbackStateName(session.PlaybackState()));
        });
        out->player.Source(Core::MediaSource::CreateFromMediaStreamSource(out->source));
        out->player.Play();
        return out;
    } catch (winrt::hresult_error const& e) {
        why = format("0x%08X ", unsigned(e.code().value)) + narrow(std::wstring(e.message()));
        return nullptr;
    }
}

void AudioHost::retireMedia(std::shared_ptr<MediaOut>& out) {
    if (!out) return;
    winrt::Windows::Media::Core::MediaStreamSourceSampleRequestDeferral deferral{nullptr};
    {
        std::lock_guard<std::mutex> lock(out->m);
        out->closing = true;
        // A request still waiting gets no sample: the stream's end.
        if (out->pending) deferral = out->deferral;
        out->pending = nullptr;
        out->deferral = nullptr;
    }
    out->taken.notify_all();
    try {
        if (deferral) deferral.Complete();
        out->player.PlaybackSession().PlaybackStateChanged(out->stateChanged);
        out->player.MediaFailed(out->failed);
        out->source.SampleRequested(out->requested);
        out->source.Starting(out->starting);
        out->player.Pause();
        out->player.Source(nullptr);
        out->player.Close();
    } catch (winrt::hresult_error const&) {
    }
    // (write() may hold it a moment longer: it goes on this thread.)
    for (int i = 0; i < 300 && out.use_count() > 1; ++i) Sleep(5);
    out.reset();
}

void AudioHost::mediaSessionPlays() {
    if (sessionPlayed_.exchange(true) || !media_.load()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    wake_.notify_all();
}

void AudioHost::ownerLoop() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const std::wstring flags = platform::localStateDirectory() + L"\\flags\\";
    const bool playerAlways = files::exists(flags + L"audio-player"), xaudioAlways = files::exists(flags + L"audio-xaudio2");
    if (playerAlways) shellLog::write("flag: audio-player is on");
    if (xaudioAlways) shellLog::write("flag: audio-xaudio2 is on");
    // A media app's sound goes through Windows.Media's player (MediaOut), every other through XAudio2 (Engine).
    const auto playerWanted = [&] {
        return media_.load() && route_.load() == MEGLER_AUDIO_ROUTE_DEFAULT && !mediaFailed_.load() && !xaudioAlways &&
               (playerAlways || sessionPlayed_.load());
    };
    unsigned long long retryAt = 0;
    unsigned failures = 0;
    for (;;) {
        std::shared_ptr<Engine> old;
        std::shared_ptr<MediaOut> oldMedia;
        bool have = false, haveMedia = false, wasLost = false, wantMedia = false;
        int rate = 0, route = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            const bool settled = playerWanted() ? mediaOut_ && mediaOut_->rate == rate_.load() && !mediaOut_->broken.load() && !engine_
                                                : !mediaOut_ && (engine_ ? engine_->rate == rate_.load() && engine_->route == route_.load()
                                                                         : GetTickCount64() < retryAt);
            if (!quit_ && !lost_.load() && settled) wake_.wait_for(lock, std::chrono::milliseconds(500));
            if (quit_) break;
            rate = rate_.load();
            route = route_.load();
            wantMedia = playerWanted();
            wasLost = lost_.exchange(false);
            if (mediaOut_ && (!wantMedia || mediaOut_->broken.load() || mediaOut_->rate != rate)) oldMedia = std::move(mediaOut_);
            haveMedia = mediaOut_ != nullptr;
            // (An engine still good plays on until the player is there.)
            if (engine_ && (wasLost || engine_->rate != rate || engine_->route != route || (wantMedia && haveMedia))) old = std::move(engine_);
            have = engine_ != nullptr;
        }
        if (oldMedia) {
            if (oldMedia->broken.load()) {
                mediaFailed_.store(true);
                wantMedia = false;
                shellLog::write("audio: the app's sound goes through XAudio2 from now on");
            }
            retireMedia(oldMedia);
        }
        if (wantMedia && !haveMedia) {
            std::string why;
            const unsigned long long began = GetTickCount64();
            std::shared_ptr<MediaOut> made = makeMedia(rate, why);
            if (made) {
                shellLog::write(format("audio: Windows.Media's player playing the mixer's 16-bit stereo at %d Hz, %s; built in %llu ms", rate,
                                       playerAlways ? "as the flag audio-player has it" : "the app's media session playing (it goes on in the background)",
                                       GetTickCount64() - began));
                setStatus(format("%d Hz", rate));
                failures = 0;
                std::lock_guard<std::mutex> lock(mutex_);
                mediaOut_ = std::move(made);
                if (engine_) old = std::move(engine_);
                have = false;
            } else {
                mediaFailed_.store(true);
                wantMedia = false;
                shellLog::write("audio: Windows.Media's player could not be made (" + why + "): the app's sound goes through XAudio2");
            }
        }
        if (wantMedia) {
            if (old) {
                if (wasLost) shellLog::write(format("audio: XAudio2 critical error 0x%08X (the output changed?)", unsigned(lostCode_.load())));
                // What it still holds (50 ms or so) plays out first.
                for (int i = 0; i < 20; ++i) {
                    XAUDIO2_VOICE_STATE state = {};
                    old->source->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
                    if (!state.BuffersQueued) break;
                    Sleep(10);
                }
                retire(old);
            }
            // The phone's endpoint as before a call the app made.
            if (routedOnce_) {
                const std::string failed = setPhoneEndpoint(endpointDefault);
                shellLog::write(failed.empty() ? std::string("audio: routed as before the call") : "audio: the call's sound could not be routed: " + failed);
                routedOnce_ = false;
            }
            logCounts();
            continue;
        }
        if (old) {
            shellLog::write(wasLost ? format("audio: XAudio2 critical error 0x%08X (the output changed?); building a new engine",
                                             unsigned(lostCode_.load()))
                            : old->rate != rate ? std::string("audio: another rate is wanted; building a new engine")
                                                : std::string("audio: the sound goes elsewhere (a call began or ended); building a new engine"));
            retire(old);
        }
        if (!have && GetTickCount64() >= retryAt) {
            std::string why;
            const unsigned long long began = GetTickCount64();
            std::shared_ptr<Engine> made = Engine::make(this, rate, why, media_.load(), route);
            if (made) {
                shellLog::write(format("audio: XAudio2 playing 16-bit stereo at %d Hz into %s%s; built in %llu ms", rate,
                                       made->device.c_str(), route ? ", a call's (Communications)" : "", GetTickCount64() - began));
                // The phone's endpoint for it: the earpiece or the loudspeaker for a call, the default once one has ended. An app
                // that never makes a call never asks the phone.
                if (route || routedOnce_) {
                    const std::string failed = setPhoneEndpoint(route == MEGLER_AUDIO_ROUTE_EARPIECE  ? endpointEarpiece
                                                                : route == MEGLER_AUDIO_ROUTE_SPEAKER ? endpointSpeakerphone
                                                                                                      : endpointDefault);
                    if (!failed.empty()) shellLog::write("audio: the call's sound could not be routed: " + failed);
                    else shellLog::write(route == MEGLER_AUDIO_ROUTE_EARPIECE  ? "audio: routed to the earpiece"
                                         : route == MEGLER_AUDIO_ROUTE_SPEAKER ? "audio: routed to the loudspeaker"
                                                                               : "audio: routed as before the call");
                    routedOnce_ = route != MEGLER_AUDIO_ROUTE_DEFAULT;
                }
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
    std::shared_ptr<MediaOut> lastMedia;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last = std::move(engine_);
        lastMedia = std::move(mediaOut_);
    }
    retireMedia(lastMedia);
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
    std::shared_ptr<MediaOut> media;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        engine = engine_;
        media = mediaOut_;
    }
    std::string output = "there is no engine";
    if (media) {
        // How far behind what it has taken the player plays: the delay the app's sound has on its way to the speaker,
        // beyond the 50 ms queued here.
        const double taken = double(media->framesTaken.load()) / double(media->rate);
        try {
            const auto session = media->player.PlaybackSession();
            const double at = double(session.Position().count()) / 1e7;
            output = format("Windows.Media's player has taken %.2f s of sound and is %s at %.2f s of it, %.0f ms behind", taken,
                            playbackStateName(session.PlaybackState()), at, (taken - at) * 1000.0);
        } catch (winrt::hresult_error const& e) {
            output = format("Windows.Media's player has taken %.2f s of sound (its session: 0x%08X)", taken, unsigned(e.code().value));
        }
    } else if (engine) {
        XAUDIO2_VOICE_STATE state = {};
        engine->source->GetState(&state, 0);
        output = format("XAudio2 has played %llu frames and holds %u blocks", (unsigned long long)state.SamplesPlayed, unsigned(state.BuffersQueued));
    }
    const int peak = peak_.exchange(0);
    shellLog::write(format("audio: %llu blocks from the mixer in %.1f s, %llu of them silence, peak %d of 32767 since the last line; "
                           "%llu given to %s, %llu dropped at their own pace, %llu refused; %s",
                           (unsigned long long)in, double(now - countsFirstAt_) / 1000.0, (unsigned long long)blocksSilent_.load(), peak,
                           (unsigned long long)blocksQueued_.load(), media ? "the output" : "XAudio2", (unsigned long long)blocksDropped_.load(),
                           (unsigned long long)blocksRefused_.load(), output.c_str()));
    engine.reset();
    media.reset();
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
    else lastAudible_.store(GetTickCount64());
    for (int seen = peak_.load(); peak > seen && !peak_.compare_exchange_weak(seen, peak);) {}

    std::shared_ptr<Engine> held;
    std::shared_ptr<MediaOut> media;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        held = engine_;
        media = mediaOut_;
    }
    if (media) {
        if (!paused_.load() && writeMedia(*media, samples, frames)) {
            paceDeadline_ = 0;
            ++blocksQueued_;
        } else {
            dropAtItsOwnPace(frames);
        }
        return;
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

bool AudioHost::writeMedia(MediaOut& out, const int16_t* samples, int frames) {
    const size_t limit = size_t(out.rate) * queuedMilliseconds / 1000;
    winrt::Windows::Media::Core::MediaStreamSourceSampleRequestDeferral deferral{nullptr};
    {
        std::unique_lock<std::mutex> lock(out.m);
        // Hold the mixer back while more than about 50 ms waits to be taken; a player that takes nothing for a second has
        // stopped taking (the blocks are then dropped at their own pace, without that wait, until it takes again).
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (out.queuedFrames() > limit) {
            if (out.closing || out.broken.load() || out.stalled || paused_.load()) return false;
            if (out.taken.wait_until(lock, until) == std::cv_status::timeout && out.queuedFrames() > limit) {
                out.stalled = true;
                if (out.stalls++ < 20)
                    shellLog::write("audio: Windows.Media's player has taken nothing for a second; the app's sound is dropped at its own pace "
                                    "until it takes again");
                return false;
            }
        }
        if (out.closing || out.broken.load()) return false;
        out.queue.insert(out.queue.end(), samples, samples + size_t(frames) * 2);
        // A request waiting for this block has it now.
        if (out.pending) {
            out.pending.Sample(out.take());
            out.pending = nullptr;
            deferral = out.deferral;
            out.deferral = nullptr;
        }
    }
    if (deferral) deferral.Complete();
    return true;
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
