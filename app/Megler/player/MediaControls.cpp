#include "pch.h"

#include "player/MediaControls.h"

#include "player/Session.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.h>

namespace megler::app::mediaControls {

using namespace winrt;
using namespace winrt::Windows::Media;
using winrt::Windows::UI::Core::CoreDispatcher;
using winrt::Windows::UI::Core::CoreDispatcherPriority;

namespace {

// Android's media key codes (android.view.KeyEvent).
constexpr int KEYCODE_MEDIA_PLAY_PAUSE = 85, KEYCODE_MEDIA_STOP = 86, KEYCODE_MEDIA_NEXT = 87, KEYCODE_MEDIA_PREVIOUS = 88,
              KEYCODE_MEDIA_REWIND = 89, KEYCODE_MEDIA_FAST_FORWARD = 90, KEYCODE_MEDIA_PLAY = 126, KEYCODE_MEDIA_PAUSE = 127;
// Android's PlaybackState: its STATE_* and ACTION_* bits.
constexpr int STATE_NONE = 0, STATE_STOPPED = 1, STATE_PAUSED = 2, STATE_ERROR = 7;
constexpr long long ACTION_STOP = 1, ACTION_REWIND = 8, ACTION_SKIP_TO_PREVIOUS = 16, ACTION_SKIP_TO_NEXT = 32,
                    ACTION_FAST_FORWARD = 64;

// Only the UI thread touches the controls; other threads post to it (onUi).
SystemMediaTransportControls gControls{nullptr};
event_token gPressed{};
MediaPlaybackStatus gShown = MediaPlaybackStatus::Closed;
bool gSessionSaid = false; // the app's media session has said its state: the sound alone no longer decides
int gHeardFor = 0, gSilentFor = 0; // milliseconds, counted by heard()

std::mutex gMutex; // the two below, read by other threads
CoreDispatcher gDispatcher{nullptr};
unsigned gGeneration = 0; // goes up at every start and stop: what was posted before is dropped after

/// Runs `f` on the UI thread, unless the controls have been stopped or started again by then.
void onUi(std::function<void()> f) {
    CoreDispatcher dispatcher{nullptr};
    unsigned generation;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        dispatcher = gDispatcher;
        generation = gGeneration;
    }
    if (!dispatcher) return;
    dispatcher.RunAsync(CoreDispatcherPriority::Normal, [f = std::move(f), generation] {
        {
            std::lock_guard<std::mutex> lock(gMutex);
            if (generation != gGeneration) return;
        }
        if (!gControls) return;
        try {
            f();
        } catch (winrt::hresult_error const& e) {
            shellLog::write(format("media controls: the app's media session could not be told to them (0x%08X)", unsigned(e.code().value)));
        }
    });
}

const char* statusName(MediaPlaybackStatus s) {
    switch (s) {
    case MediaPlaybackStatus::Playing: return "playing";
    case MediaPlaybackStatus::Paused: return "paused";
    case MediaPlaybackStatus::Stopped: return "stopped";
    case MediaPlaybackStatus::Changing: return "changing";
    default: return "closed";
    }
}

void show(MediaPlaybackStatus status, const char* why) {
    if (!gControls || status == gShown) return;
    try {
        gControls.PlaybackStatus(status);
    } catch (winrt::hresult_error const& e) {
        // (Taken as shown all the same: asked again at every poll, it would fail at every poll.)
        shellLog::write(format("media controls: %s refused (0x%08X, %s)", statusName(status), unsigned(e.code().value), why));
        gShown = status;
        return;
    }
    gShown = status;
    shellLog::write(format("media controls: %s (%s)", statusName(status), why));
}

void onButton(SystemMediaTransportControls const&, SystemMediaTransportControlsButtonPressedEventArgs const& args) {
    int key = 0;
    const char* what = "";
    switch (args.Button()) {
    case SystemMediaTransportControlsButton::Play: key = KEYCODE_MEDIA_PLAY, what = "play"; break;
    case SystemMediaTransportControlsButton::Pause: key = KEYCODE_MEDIA_PAUSE, what = "pause"; break;
    case SystemMediaTransportControlsButton::Stop: key = KEYCODE_MEDIA_STOP, what = "stop"; break;
    case SystemMediaTransportControlsButton::Next: key = KEYCODE_MEDIA_NEXT, what = "next"; break;
    case SystemMediaTransportControlsButton::Previous: key = KEYCODE_MEDIA_PREVIOUS, what = "previous"; break;
    case SystemMediaTransportControlsButton::FastForward: key = KEYCODE_MEDIA_FAST_FORWARD, what = "fast forward"; break;
    case SystemMediaTransportControlsButton::Rewind: key = KEYCODE_MEDIA_REWIND, what = "rewind"; break;
    default: return;
    }
    // (On a thread of Windows'. The key goes into the app's queue, as a key of the keyboard's does.)
    shellLog::write(format("media controls: %s pressed; the app is sent the media key %d", what, key));
    megler_key(key, true);
    megler_key(key, false);
}

// MARK: The app's media session

void stateThunk(void*, int state, long long actions) {
    // A media app's sound goes through Windows.Media's player once its session plays (player/AudioHost.h).
    if (state != STATE_NONE && state != STATE_ERROR && state != STATE_STOPPED && state != STATE_PAUSED)
        if (const auto audio = session::audio()) audio->mediaSessionPlays();
    onUi([state, actions] {
        gSessionSaid = state != STATE_NONE;
        gControls.IsStopEnabled((actions & ACTION_STOP) != 0);
        gControls.IsNextEnabled((actions & ACTION_SKIP_TO_NEXT) != 0);
        gControls.IsPreviousEnabled((actions & ACTION_SKIP_TO_PREVIOUS) != 0);
        gControls.IsFastForwardEnabled((actions & ACTION_FAST_FORWARD) != 0);
        gControls.IsRewindEnabled((actions & ACTION_REWIND) != 0);
        // (Play and pause stay on whatever the app says: Windows keeps the sound going in the background only so.)
        const MediaPlaybackStatus status = state == STATE_NONE || state == STATE_ERROR ? MediaPlaybackStatus::Closed
                                           : state == STATE_STOPPED                   ? MediaPlaybackStatus::Stopped
                                           : state == STATE_PAUSED                    ? MediaPlaybackStatus::Paused
                                                                                      : MediaPlaybackStatus::Playing;
        show(status, format("the app's media session: state %d", state).c_str());
    });
}

fire_and_forget showArt(std::vector<uint32_t> argb, int width, int height) {
    using namespace winrt::Windows::Graphics::Imaging;
    using namespace winrt::Windows::Storage::Streams;
    unsigned generation;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        generation = gGeneration;
    }
    // 0xAARRGGBB in memory is the bytes B, G, R, A: BGRA8, alpha not premultiplied.
    std::vector<uint8_t> bytes(argb.size() * 4);
    memcpy(bytes.data(), argb.data(), bytes.size());
    InMemoryRandomAccessStream stream;
    try {
        BitmapEncoder encoder = co_await BitmapEncoder::CreateAsync(BitmapEncoder::PngEncoderId(), stream);
        encoder.SetPixelData(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Straight, uint32_t(width), uint32_t(height), 96, 96, bytes);
        co_await encoder.FlushAsync();
    } catch (winrt::hresult_error const& e) {
        shellLog::write(format("media controls: the art could not be made a picture (0x%08X)", unsigned(e.code().value)));
        co_return;
    }
    // (Back on the UI thread, where this began.)
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (generation != gGeneration) co_return;
    }
    if (!gControls) co_return;
    try {
        stream.Seek(0);
        auto updater = gControls.DisplayUpdater();
        updater.Thumbnail(RandomAccessStreamReference::CreateFromStream(stream));
        updater.Update();
    } catch (winrt::hresult_error const& e) {
        shellLog::write(format("media controls: the art was not taken (0x%08X)", unsigned(e.code().value)));
    }
}

// What the controls were told last, on the UI thread: the same again is not told, and the same picture is not made a PNG again.
// Spotify's metadata came many times a second as it played, each with its picture, and each was encoded and shown anew (652).
std::string gToldTitle, gToldArtist, gToldAlbum;
uint64_t gToldArt = 0; // FNV-1a of the picture's pixels and size, 0 for none
bool gTold = false;

void metadataThunk(void*, const char* title, const char* artist, const char* album, const uint32_t* art, int width, int height) {
    std::string t = title ? title : "", a = artist ? artist : "", al = album ? album : "";
    std::vector<uint32_t> pixels;
    if (art && width > 0 && height > 0 && width <= 1024 && height <= 1024) pixels.assign(art, art + size_t(width) * size_t(height));
    onUi([t, a, al, pixels = std::move(pixels), width, height]() mutable {
        uint64_t key = 0;
        if (!pixels.empty()) {
            key = 1469598103934665603ull;
            const auto mix = [&key](uint32_t v) {
                for (int i = 0; i < 4; i++, v >>= 8) key = (key ^ (v & 0xFF)) * 1099511628211ull;
            };
            mix(uint32_t(width));
            mix(uint32_t(height));
            for (uint32_t p : pixels) mix(p);
            if (!key) key = 1;
        }
        if (gTold && key == gToldArt && t == gToldTitle && a == gToldArtist && al == gToldAlbum) return;
        const bool newArt = !gTold || key != gToldArt;
        gTold = true;
        gToldTitle = t;
        gToldArtist = a;
        gToldAlbum = al;
        gToldArt = key;
        if (!newArt) pixels.clear(); // (the picture shown stays)
        auto updater = gControls.DisplayUpdater();
        updater.Type(MediaPlaybackType::Music);
        updater.MusicProperties().Title(hs(t));
        updater.MusicProperties().Artist(hs(a));
        updater.MusicProperties().AlbumTitle(hs(al));
        if (newArt && pixels.empty()) updater.Thumbnail(nullptr);
        updater.Update();
        shellLog::write("media controls: now \"" + t + "\"" + (a.empty() ? std::string() : " by " + a) +
                        (pixels.empty() ? std::string() : format(", with a picture of %dx%d", width, height)));
        if (!pixels.empty()) showArt(std::move(pixels), width, height);
    });
}

} // namespace

/*
 * Windows' controls may refuse any of these calls where the device keeps them to itself (an Xbox's are the console's, its
 * guide's): what failed is said, the controls are let go of, and the app runs on without them -- never an exception into
 * the player's poll, which then left its busy overlay and its end undone at every poll after.
 */
void start() {
    if (gControls) return;
    try {
        gControls = SystemMediaTransportControls::GetForCurrentView();
    } catch (winrt::hresult_error const& e) {
        shellLog::write(format("media controls: not there (0x%08X); the app's sound stops in the background", unsigned(e.code().value)));
        return;
    }
    try {
        {
            std::lock_guard<std::mutex> lock(gMutex);
            gDispatcher = winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread().Dispatcher();
            ++gGeneration;
        }
        gTold = false; // (a session's controls start with nothing told)
        gControls.IsEnabled(true);
        gControls.IsPlayEnabled(true);
        gControls.IsPauseEnabled(true);
        gControls.IsStopEnabled(false);
        gControls.IsNextEnabled(false);
        gControls.IsPreviousEnabled(false);
        // (A type, even with nothing else said: Windows then handles the sound as media.)
        gControls.DisplayUpdater().Type(MediaPlaybackType::Music);
        gControls.DisplayUpdater().Update();
        gPressed = gControls.ButtonPressed(&onButton);
    } catch (winrt::hresult_error const& e) {
        shellLog::write(format("media controls: refused (0x%08X %s); the app's sound stops in the background", unsigned(e.code().value),
                               narrow(e.message()).c_str()));
        {
            std::lock_guard<std::mutex> lock(gMutex);
            gDispatcher = nullptr;
            ++gGeneration;
        }
        try {
            if (gPressed) gControls.ButtonPressed(gPressed);
        } catch (winrt::hresult_error const&) {
        }
        gPressed = {};
        gControls = nullptr;
        return;
    }
    gShown = MediaPlaybackStatus::Closed;
    gSessionSaid = false;
    gHeardFor = gSilentFor = 0;
    shellLog::write("media controls: on, for the app's sound in the background");
}

void stop() {
    if (!gControls) return;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gDispatcher = nullptr;
        ++gGeneration;
    }
    gTold = false;
    try {
        gControls.ButtonPressed(gPressed);
        gControls.PlaybackStatus(MediaPlaybackStatus::Closed);
        gControls.DisplayUpdater().ClearAll();
        gControls.DisplayUpdater().Update();
        gControls.IsEnabled(false);
    } catch (winrt::hresult_error const& e) {
        shellLog::write(format("media controls: not all let go of (0x%08X)", unsigned(e.code().value)));
    }
    gPressed = {};
    gControls = nullptr;
    shellLog::write("media controls: off");
}

void heard(bool audible) {
    if (!gControls || gSessionSaid) return;
    // About four times a second. Playing after two seconds of sound, so that a click or a short sound of the app's
    // does not bring up the controls; paused after five of silence.
    constexpr int tick = 250;
    if (audible) {
        gSilentFor = 0;
        gHeardFor += tick;
        if (gHeardFor >= 2000) show(MediaPlaybackStatus::Playing, "the app is heard");
    } else {
        gHeardFor = 0;
        gSilentFor += tick;
        if (gSilentFor >= 5000 && gShown == MediaPlaybackStatus::Playing) show(MediaPlaybackStatus::Paused, "the app is silent");
    }
}

megler_media_session_host callbacks() {
    megler_media_session_host host = {};
    host.user = nullptr;
    host.state = &stateThunk;
    host.metadata = &metadataThunk;
    return host;
}

} // namespace megler::app::mediaControls
