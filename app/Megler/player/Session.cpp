#include "pch.h"

#include "player/Session.h"

#include "player/CameraHost.h"
#include "player/CodecHost.h"
#include "player/GfxHost.h"
#include "player/KeyboardHost.h"
#include "player/MediaHost.h"
#include "player/PhoneState.h"
#include "player/StorageHost.h"

#include "shell/Checks.h"
#include "shell/Files.h"
#include "shell/Platform.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include "megler.h"

namespace megler::app::session {
namespace {

constexpr int androidKeyBack = 4; // AKEYCODE_BACK

// gMutex guards the state below and is never held across a call into the
// core: the core may answer a call with on_exit on the same thread (megler_stop
// of an app whose driver has nothing to stop), and on_exit takes it.
std::mutex gMutex;
Snapshot gSnapshot;
std::shared_ptr<WindowHost> gWindow;
std::shared_ptr<AudioHost> gAudio;
// A failed start's screen and sound are kept, not freed: nothing says the
// core has let go of their callbacks.
std::vector<std::shared_ptr<WindowHost>> gOldWindows;
std::vector<std::shared_ptr<AudioHost>> gOldAudio;
// What megler_run_config points at, alive for the rest of the process.
std::string gApkPath;
bool gSuspended = false, gVisible = true, gFocused = true;

// What the app was last told, and the order it is told in: held across the
// calls, by the one thread telling.
std::mutex gTellMutex;
bool gPausedTold = false, gFocusedTold = true;

/// Tells the running app what changed about its place on the phone.
void tell() {
    std::lock_guard<std::mutex> telling(gTellMutex);
    bool paused, focused;
    std::shared_ptr<AudioHost> audio;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (gSnapshot.state != State::running) return;
        paused = gSuspended || !gVisible;
        focused = gFocused;
        audio = gAudio;
    }
    // Focus goes before a pause and comes back after a resume, as Android's
    // own order has it.
    if (!focused && gFocusedTold) {
        megler_set_focused(false);
        gFocusedTold = false;
    }
    if (paused != gPausedTold) {
        // An app left in the background is not heard there, whether or not
        // it stops its own sound when paused.
        if (audio) audio->setPaused(paused);
        megler_set_paused(paused);
        gPausedTold = paused;
        shellLog::write(paused ? "session: paused" : "session: resumed");
    }
    if (focused && !gFocusedTold) {
        megler_set_focused(true);
        gFocusedTold = true;
    }
}

/// The drawing for an app whose user interface is Java (player/GfxHost.h),
/// its lines in the shell's log. It makes nothing until such an app first
/// draws, so a game pays nothing for it. It draws on the graphics processor
/// where there is a device for it; the flag "gfx-software" (a file of that
/// name in LocalState\flags) has it draw in memory by Direct2D's software
/// rasterizer instead, as it did before, to compare the two on a phone. The
/// flag "gfx-copy" has every canvas drawn in memory and copied to a WIC
/// bitmap and back: the slowest way, for a phone on which the others show
/// wrong pictures.
megler_gfx_host gfxCallbacks() {
    GfxHost& gfx = GfxHost::shared();
    gfx.setLog([](const char* line, void*) { shellLog::write(line); }, nullptr);
    // Android's own fonts, where the package has them (tools/dvm-android-root.sh): an app's text is then as wide and
    // as tall as its layout was made for.
    gfx.setFontFolder((platform::installDirectory() + L"\\android\\fonts").c_str());
    if (files::exists(platform::localStateDirectory() + L"\\flags\\gfx-copy")) {
        shellLog::write("flag: gfx-copy is on");
        gfx.setCopying(true);
    }
    if (files::exists(platform::localStateDirectory() + L"\\flags\\gfx-software")) {
        shellLog::write("flag: gfx-software is on: canvases are drawn in memory, not on the graphics processor");
        gfx.setDrawing(GfxHost::Drawing::memory);
    }
    return gfx.callbacks();
}

// megler_run_config.on_exit: once, from any thread.
void onExit(void*, int status, const char* reason) {
    const std::string why = reason ? reason : "";
    shellLog::write(format("session: the app has ended, status %d%s%s", status, why.empty() ? "" : ": ", why.c_str()));
    std::lock_guard<std::mutex> lock(gMutex);
    gSnapshot.state = State::exited;
    gSnapshot.exitStatus = status;
    gSnapshot.reason = why;
}

} // namespace

Snapshot snapshot() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gSnapshot;
}

bool spent() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gSnapshot.state == State::starting || gSnapshot.state == State::running || gSnapshot.state == State::exited;
}

void forget() {
    std::lock_guard<std::mutex> lock(gMutex);
    if (gSnapshot.state != State::failed) return;
    gSnapshot = Snapshot();
    if (gWindow) gOldWindows.push_back(std::move(gWindow));
    if (gAudio) gOldAudio.push_back(std::move(gAudio));
    gWindow = nullptr;
    gAudio = nullptr;
}

std::string unavailable() {
    const char* const reopen = " Android libraries cannot be unloaded: close Megler and open it again to play.";
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (gSnapshot.state == State::starting || gSnapshot.state == State::running) return "An app is running.";
        if (gSnapshot.state == State::exited) return std::string("An app has already run since Megler was opened.") + reopen;
    }
    if (checks::selfTestStarted())
        return std::string("The self-test has run its own small app since Megler was opened, and one app is all a start of "
                           "Megler can run.") + reopen;
    return std::string();
}

std::shared_ptr<WindowHost> window() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gWindow;
}

std::shared_ptr<AudioHost> audio() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gAudio;
}

bool start(const ApkEntry& apk, winrt::Windows::UI::Xaml::Controls::SwapChainPanel const& panel, int width, int height,
           int densityDpi) {
    std::shared_ptr<WindowHost> window;
    std::shared_ptr<AudioHost> audio;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (gSnapshot.state != State::idle) return false;
        gSnapshot = Snapshot();
        gSnapshot.state = State::starting;
        gSnapshot.apkId = apk.id;
        gSnapshot.title = apk.title();
        gApkPath = narrow(apk.path);
        window = gWindow = std::make_shared<WindowHost>(panel, width, height);
        audio = gAudio = std::make_shared<AudioHost>();
    }
    // The sound output takes a second and a half to build the first time:
    // begun now, at the rate the core's mixer runs at, it is there before the
    // app's first sound.
    audio->prepare(44100);
    // The phone's time zone and battery, which an app asks the layer for.
    phoneState::tell();
    shellLog::write(format("session: starting %s (%s) at %dx%d, %d dpi", apk.id.c_str(), apk.report.package.c_str(), width, height,
                           densityDpi));
    std::thread([window, audio, width, height, densityDpi] {
        megler_run_config config = {};
        config.apk_path = gApkPath.c_str();
        config.split_apks = nullptr;
        config.split_count = 0;
        config.width = width;
        config.height = height;
        config.density_dpi = densityDpi;
        config.window = window->callbacks();
        config.audio = audio->callbacks();
        config.media = MediaHost::shared().callbacks();
        // A Java app's MediaCodec: Media Foundation's decoder transforms, one sample at a time (player/CodecHost.h).
        config.codec = CodecHost::shared().callbacks();
        // A Java app's android.hardware.Camera: the phone's cameras through Windows.Media.Capture (player/CameraHost.h), held
        // upright by nature on a phone, what was learnt of them kept in LocalState.
        CameraHost::shared().configure(platform::localStateDirectory() + L"\\camera.txt", platform::localStateDirectory() + L"\\flags",
                                       platform::isMobile());
        config.camera = CameraHost::shared().callbacks();
        config.storage = StorageHost::shared().callbacks();
        // For an app that is Java: its drawing, and where Android's libcore and the framework are -- NULL is the folder
        // `android` of the installed package, which is where tools/build-appx.sh --libcore (or --java) puts them. An app
        // with a native engine uses neither.
        config.gfx = gfxCallbacks();
        // The window's pictures drawn on the graphics processor are shown from there, in this window (WindowHost.h).
        GfxHost::shared().setWindow(window->gfxWindow());
        config.android_root = nullptr;
        config.keyboard = KeyboardHost::shared().callbacks();
        config.on_exit = &onExit;
        config.user = nullptr;
        const bool started = megler_run(&config);
        {
            std::lock_guard<std::mutex> lock(gMutex);
            if (!started) {
                const char* error = megler_last_error();
                gSnapshot.state = State::failed;
                gSnapshot.reason = error && *error ? error : "The app could not be started.";
                shellLog::write("session: megler_run failed: " + gSnapshot.reason);
                return;
            }
            // (on_exit may already have come: an app that ends at once.)
            if (gSnapshot.state != State::starting) return;
            gSnapshot.state = State::running;
            shellLog::write("session: running");
        }
        // Should the phone have been locked, or the app left, meanwhile.
        tell();
    }).detach();
    return true;
}

void stop() {
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (gSnapshot.state != State::running) return;
        gSnapshot.stopRequested = true;
    }
    shellLog::write("session: stop requested");
    megler_stop();
}

void touch(int phase, int pointer, float x, float y) {
    // No lock: sixty moves a second per finger, and the core takes calls
    // from any thread and ignores them when no app runs.
    megler_touch(phase, pointer, x, y);
}

void backKey() {
    megler_key(androidKeyBack, true);
    megler_key(androidKeyBack, false);
}

// (Like touch: the core takes these from any thread and ignores them when no Java app runs.)
void textCommit(const std::string& utf8) {
    if (!utf8.empty()) megler_text_commit(utf8.c_str());
}

void textKey(int key) { megler_text_key(key); }

void keyboardClosed() { megler_keyboard_closed(); }

void keyboardHeight(int pixels) { megler_keyboard_height(pixels < 0 ? 0 : pixels); }

void setSuspended(bool suspended) {
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gSuspended = suspended;
    }
    tell();
}

void setVisible(bool visible) {
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gVisible = visible;
    }
    tell();
}

void setFocused(bool focused) {
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gFocused = focused;
    }
    tell();
}

} // namespace megler::app::session
