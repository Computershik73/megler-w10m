#include "pch.h"

#include "player/Session.h"

#include "player/BarsHost.h"
#include "player/MediaControls.h"
#include "player/SystemHost.h"
#include "library/PhoneAppsList.h"
#include "player/CameraHost.h"
#include "player/CodecHost.h"
#include "player/GfxHost.h"
#include "player/KeyboardHost.h"
#include "player/BiometricHost.h"
#include "player/LocationHost.h"
#include "player/MicrophoneHost.h"
#include "player/MediaHost.h"
#include "player/PermissionHost.h"
#include "player/PhoneEvents.h"
#include "player/PhoneState.h"
#include "player/StorageHost.h"

#include "shell/AppPackage.h"
#include "shell/Checks.h"
#include "shell/Files.h"
#include "shell/PicturesDiag.h"
#include "shell/Platform.h"
#include "shell/Settings.h"
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
std::string gApkPath, gSplitsDir, gAppsDir;
bool gSuspended = false, gVisible = true, gFocused = true;
// A Java app is heard in the background, as on Android (its own music and video players keep playing there, and the
// system media controls stand for it: player/MediaControls.cpp); an engine's game is not, whether or not it stops its
// own sound when paused.
bool gBackgroundAudio = false;

// What the app was last told, and the order it is told in: held across the
// calls, by the one thread telling.
std::mutex gTellMutex;
bool gPausedTold = false, gFocusedTold = true;

/// Tells the running app what changed about its place on the phone.
void tell() {
    std::lock_guard<std::mutex> telling(gTellMutex);
    bool paused, focused, silenced;
    std::shared_ptr<AudioHost> audio;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (gSnapshot.state != State::running) return;
        paused = gSuspended || !gVisible;
        focused = gFocused;
        audio = gAudio;
        silenced = gSuspended || (!gVisible && !gBackgroundAudio);
    }
    // Focus goes before a pause and comes back after a resume, as Android's
    // own order has it.
    if (!focused && gFocusedTold) {
        megler_set_focused(false);
        gFocusedTold = false;
    }
    if (paused != gPausedTold) {
        megler_set_paused(paused);
        gPausedTold = paused;
        shellLog::write(paused ? "session: paused" : "session: resumed");
    }
    if (audio) audio->setPaused(silenced);
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
    gfx.setFontFolder(((appPackage::isSingleApp() ? appPackage::single().androidRoot : platform::installDirectory() + L"\\android") +
                       L"\\fonts").c_str());
    if (files::exists(platform::localStateDirectory() + L"\\flags\\gfx-copy")) {
        shellLog::write("flag: gfx-copy is on");
        gfx.setCopying(true);
    }
    if (files::exists(platform::localStateDirectory() + L"\\flags\\gfx-software")) {
        shellLog::write("flag: gfx-software is on: canvases are drawn in memory, not on the graphics processor");
        gfx.setDrawing(GfxHost::Drawing::memory);
    }
    // The flag "gfx-video-drawn": a TextureView's video drawn among the views, the window drawn again for each picture, as
    // before 636, instead of laid under them (GfxHost::setVideoUnder) -- to compare.
    if (files::exists(platform::localStateDirectory() + L"\\flags\\gfx-video-drawn")) {
        shellLog::write("flag: gfx-video-drawn is on: a TextureView's video is drawn among the views, not laid under them");
        gfx.setVideoUnder(false);
    }
    // The flag "gfx-profile": where the drawing's time goes, call by call (GfxHost.cpp, "The calls profiled").
    if (files::exists(platform::localStateDirectory() + L"\\flags\\gfx-profile")) {
        shellLog::write("flag: gfx-profile is on");
        gfx.setProfiling(true);
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
        // An app in several APKs: the folder of its splits beside its base (library/Library.h).
        gSplitsDir = apk.splitsDir.empty() ? std::string() : narrow(apk.splitsDir);
        window = gWindow = std::make_shared<WindowHost>(panel, width, height);
        audio = gAudio = std::make_shared<AudioHost>();
        gBackgroundAudio = apk.report.engine == "java";
        // The other apps the phone has, for a launcher (library/PhoneAppsList.h), where the folder is there.
        const std::wstring apps = phoneAppsList::folder();
        gAppsDir = !apps.empty() && files::exists(apps + L"\\apps.txt") ? narrow(apps) : std::string();
    }
    systemHost::setRunningApp(apk.title(), apk.report.package);
    // The app's data to be cleared as it starts (Android's Clear storage, megler_ask_clear_data): the user's answers to its
    // permissions go with it, as Android resets them; the core clears the files when it starts the app.
    if (megler_clear_data_asked(apk.report.package.c_str())) permissionHost::forgetAll(apk.report.package);
    audio->setMedia(gBackgroundAudio);
    // The sound output takes a second and a half to build the first time:
    // begun now, at the rate the core's mixer runs at, it is there before the
    // app's first sound.
    audio->prepare(44100);
    // The phone's time zone and battery, which an app asks the layer for.
    phoneState::tell();
    shellLog::write(format("session: starting %s (%s%s) at %dx%d, %d dpi", apk.id.c_str(), apk.report.package.c_str(),
                           apk.report.splits.empty() ? "" : format(", with %u splits", unsigned(apk.report.splits.size())).c_str(), width, height,
                           densityDpi));
    // Google Play Store with Google's own Google Play services always, which it waits for to be installed (AppShell::playStoreCanStart):
    // with microG's its front page had no Android ID ("Server error").
    const bool playStore = apk.report.package == "com.android.vending";
    // What the app's own package declares to Windows of the phone's messages and calls, which are watched for it alone
    // (megler_appx_capabilities; -1, a report that names no permissions: both, as before).
    const int capabilities = apk.report.capabilities();
    const bool messages = capabilities < 0 || (capabilities & MEGLER_APPX_CAP_CHAT), calls = capabilities < 0 || (capabilities & MEGLER_APPX_CAP_CALL_HISTORY);
    std::thread([window, audio, width, height, densityDpi, playStore, messages, calls] {
        // The flags a diagnosis over USB asked for, in place before the run reads them (shell/PicturesDiag.h).
        picturesDiag::waitForFlags();
        megler_run_config config = {};
        config.apk_path = gApkPath.c_str();
        // Google's own Google Play services, or microG's where the app (or Megler) is set to it (ui/SettingsScreen.cpp).
        const bool microg = !playStore && settings::get(settings::googleServices, "") == "microg";
        const std::string servicePath = narrow(appPackage::serviceApk(microg));
        // Said as what was chosen: Google's own when its package is installed, else microG's even where the setting is Google's
        // own (the log said "Google's own" for microG then, and a user's Play Store failed with microG's settings).
        const bool usesMicroG = !servicePath.empty() && servicePath == narrow(appPackage::microgApk());
        shellLog::write(std::string("session: Google Play services: ") +
                        (servicePath.empty() ? "none registered"
                         : !usesMicroG     ? "Google's own"
                         : microg          ? "microG's"
                                           : "microG's (Google's own is not installed as an app of its own: the library installs it)"));
        config.gms_apk_path = servicePath.empty() ? nullptr : servicePath.c_str();
        const std::string gsfPath = narrow(appPackage::gsfApk());
        config.gsf_apk_path = gsfPath.empty() ? nullptr : gsfPath.c_str();
        config.splits_dir = gSplitsDir.empty() ? nullptr : gSplitsDir.c_str();
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
        // A Java app's LocationManager: the phone's location through Windows.Devices.Geolocation, or the place set in Settings
        // (player/LocationHost.h).
        config.location = LocationHost::shared().callbacks();
        config.microphone = MicrophoneHost::shared().callbacks();
        // A Java app's BiometricPrompt and FingerprintManager: Windows Hello (player/BiometricHost.h).
        config.biometric = biometricHost::callbacks();
        // For an app that is Java: its drawing, and where Android's libcore and the framework are -- NULL is the folder
        // `android` of the installed package, which is where tools/build-appx.sh --libcore (or --java) puts them. An app
        // with a native engine uses neither.
        config.gfx = gfxCallbacks();
        // The window's pictures drawn on the graphics processor are shown from there, in this window (WindowHost.h).
        GfxHost::shared().setWindow(window->gfxWindow());
        // An app's own package has them in the shared folder, where Megler put them for its build (shell/AppPackage.h).
        // (With '/', as the core joins its paths.)
        static const std::string sharedRoot = [] {
            std::string root = appPackage::isSingleApp() ? narrow(appPackage::single().androidRoot) : std::string();
            for (char& c : root)
                if (c == '\\') c = '/';
            return root;
        }();
        config.android_root = sharedRoot.empty() ? nullptr : sharedRoot.c_str();
        config.keyboard = KeyboardHost::shared().callbacks();
        // The phone's bars a Java app wants hidden (player/BarsHost.h); what they cover goes back through setInsets.
        config.bars = BarsHost::shared().callbacks();
        config.media_session = mediaControls::callbacks();
        config.system = systemHost::callbacks();
        config.apps_dir = gAppsDir.empty() ? nullptr : gAppsDir.c_str();
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
        // The phone's text messages and calls from now on, for the app (megler_sms_received, megler_call_logged): those its
        // permissions ask for. An app that asks for neither has Windows asked for neither store.
        phoneEvents::start(messages, calls);
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

/*
 * A Back within a second of the window's coming back is taken for the one that brought it back, and not given to the app. On a
 * phone, Back on the Start screen returns to the app last shown, and the press reached that app too: Spotify on a Lumia 650
 * was given Back 0.7 s after it was resumed so, finished its main activity, and Megler said it had ended (653).
 */
static std::atomic<unsigned long long> gShownAt{0};

void backKey() {
    const unsigned long long shown = gShownAt.load();
    if (shown && GetTickCount64() - shown < 1000) {
        shellLog::write("session: a Back as the window came back, the one that brought it back: not the app's");
        return;
    }
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

void setInsets(int left, int top, int right, int bottom) { megler_set_insets(left, top, right, bottom); }

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
        if (visible && !gVisible) gShownAt.store(GetTickCount64());
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
