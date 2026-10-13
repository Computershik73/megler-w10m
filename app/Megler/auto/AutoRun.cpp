// The auto run (AutoRun.h): which stages this build has, the thread they run
// on, and megler-auto.log. tools/build-appx.sh writes AutoConfig.h with
// MEGLER_AUTO_PROBE, MEGLER_AUTO_SELFTEST, MEGLER_AUTO_GFX, MEGLER_AUTO_PLAY and
// MEGLER_AUTO_JAVA set to 0 or 1.
#include "pch.h"

#include "auto/AutoRun.h"

#include "library/Library.h"
#include "player/Session.h"
#include "player/WindowHost.h"
#include "shell/Checks.h"
#include "shell/Files.h"
#include "shell/Platform.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"
#include "ui/AppShell.h"

#include "megler.h"

#include "AutoConfig.h"

#ifndef MEGLER_AUTO_PROBE
#define MEGLER_AUTO_PROBE 0
#endif
#ifndef MEGLER_AUTO_SELFTEST
#define MEGLER_AUTO_SELFTEST 0
#endif
#ifndef MEGLER_AUTO_PLAY
#define MEGLER_AUTO_PLAY 0
#endif
#ifndef MEGLER_AUTO_JAVA
#define MEGLER_AUTO_JAVA 0
#endif
#ifndef MEGLER_AUTO_GFX
#define MEGLER_AUTO_GFX 0
#endif

#if MEGLER_AUTO_GFX
// The one place where the shell looks past megler.h into the core: the scenes
// the core's tests draw through a megler_gfx_host, here handed the shell's own
// host, so that a phone says in one unattended run whether its Direct2D,
// DirectWrite and WIC draw what a PC's do. Only a build made with --auto gfx
// has any of it.
#include "player/GfxHost.h"

#include "../../../core/tests/hk_gfx_scenes.h"
#endif

namespace megler::app::autorun {

const char* modes() {
    static const std::string text = [] {
        std::string s;
        auto add = [&s](bool on, const char* name) {
            if (on) s += (s.empty() ? "" : ",") + std::string(name);
        };
        add(MEGLER_AUTO_PROBE, "probe");
        add(MEGLER_AUTO_SELFTEST, "selftest");
        add(MEGLER_AUTO_GFX, "gfx");
        add(MEGLER_AUTO_PLAY, "play");
        add(MEGLER_AUTO_JAVA, "java");
        return s;
    }();
    return text.c_str();
}

#if MEGLER_AUTO_PROBE || MEGLER_AUTO_SELFTEST || MEGLER_AUTO_GFX || MEGLER_AUTO_PLAY || MEGLER_AUTO_JAVA
namespace {

/// How long the play stage lets a game run before it calls the run done
/// (the game is left running: the logs are fetched from under it). The Java
/// test app is done with sooner: it is pressed, and then told to end.
int playSeconds = MEGLER_AUTO_JAVA ? 45 : 120;

std::atomic<bool> gChecksDone{false};

std::wstring autoLogPath() { return platform::localStateDirectory() + L"\\megler-auto.log"; }
void autoLine(const std::string& text) { files::append(autoLogPath(), timeOfDay() + " " + text + "\r\n"); }

void finish() {
    autoLine("AUTO DONE");
    shellLog::write("auto run: AUTO DONE");
}

/// A stage, and its DONE line in megler-auto.log whatever happens.
void runStage(checks::Kind kind) {
    autoLine(std::string(checks::name(kind)) + ": started");
    std::string done;
    try {
        done = checks::run(kind);
    } catch (const std::exception& error) {
        done = std::string("DONE ") + checks::name(kind) + ": aborted by an exception: " + error.what();
    } catch (...) {
        done = std::string("DONE ") + checks::name(kind) + ": aborted by an exception";
    }
    autoLine(done);
}

#if MEGLER_AUTO_GFX
/// The scenes of core/tests/hk_gfx_scenes.c drawn by the shell's own
/// megler_gfx_host on this device, on this thread (never the UI thread, as an
/// app's drawing never is): what they say goes to megler-auto.log line by
/// line, and the pictures into LocalState as shot-gfx-NN-name.bmp, which is
/// where and how tools/phone.py shots looks for an auto run's pictures.
void runGfx() {
    autoLine("gfx: started");
    const std::wstring& local = platform::localStateDirectory();
    for (const std::wstring& name : files::list(local, L"shot-gfx-*.bmp")) files::remove(local + L"\\" + name);
    GfxHost& gfx = GfxHost::shared();
    gfx.setLog(
        [](const char* line, void*) {
            shellLog::write(line);
            autoLine(line);
        },
        nullptr);
    if (files::exists(local + L"\\flags\\gfx-copy")) {
        autoLine("gfx: the flag gfx-copy is on: every canvas is copied to a WIC bitmap and back");
        gfx.setCopying(true);
    }
    const megler_gfx_host host = gfx.callbacks();
    const std::string folder = narrow(local);
    hk_gfx_scenes_config config = {};
    config.host = &host;
    config.out_dir = folder.c_str();
    config.file_prefix = "shot-gfx-";
    config.bmp = true;
    int checks = 0;
    const int failed = hk_gfx_scenes_run(&config, [](const char* line, void*) { autoLine(line); }, nullptr, &checks);
    gfx.setLog([](const char* line, void*) { shellLog::write(line); }, nullptr);
    const GfxHost::Counts counts = gfx.counts();
    const std::string done = format("DONE gfx: %d checks, %d failed; %llu canvases, %llu drawing calls put on by Direct2D and %llu by the host's own "
                                    "arithmetic, %llu runs of text laid out, %llu kinds of thing said to be drawn not as asked%s",
                                    checks, failed, counts.canvases, counts.direct, counts.composed, counts.runsShaped, counts.notes,
                                    counts.copying ? "; canvases were copied in and out" : "");
    autoLine(done);
    shellLog::write("auto run: " + done);
}
#endif

void runAll(const std::string& description) {
    const std::wstring& local = platform::localStateDirectory();
    // Every stage's log started afresh, this build's stages or not: an update
    // keeps LocalState, and an earlier run's results (of a build with other
    // stages, say) must not pass for this one's -- tools/phone.py pulls
    // whatever is there. The core's own step logs too (megler.h: each check
    // writes one, flushed line by line, for the checks that end the process).
    for (const wchar_t* name : {L"megler-auto.log", checks::logName(checks::Kind::probe), checks::logName(checks::Kind::selftest),
                                L"megler-probe-steps.log", L"megler-selftest-steps.log"})
        files::write(local + L"\\" + name, std::string_view());
    autoLine("auto run: " + description + "; modes " + modes());
    shellLog::write(std::string("auto run: ") + modes() + " -- results in megler-auto.log");
#if MEGLER_AUTO_PROBE
    runStage(checks::Kind::probe);
#endif
#if MEGLER_AUTO_SELFTEST
    runStage(checks::Kind::selftest);
#endif
#if MEGLER_AUTO_GFX
    runGfx();
#endif
    gChecksDone = true;
#if !MEGLER_AUTO_PLAY && !MEGLER_AUTO_JAVA
    finish();
#endif
}

} // namespace
#endif

bool start(std::string description) {
#if MEGLER_AUTO_PROBE || MEGLER_AUTO_SELFTEST || MEGLER_AUTO_GFX || MEGLER_AUTO_PLAY || MEGLER_AUTO_JAVA
    // The stack of the Device screen's own checks (shell/Checks.cpp): the
    // self-test calls Android code on the thread it runs on.
    return platform::startThread([description] { runAll(description); }, size_t(8) << 20);
#else
    (void)description;
    return false;
#endif
}

void tick(ui::AppShell& shell) {
#if MEGLER_AUTO_PLAY || MEGLER_AUTO_JAVA
    enum class Stage { waiting, playing, done };
    static Stage stage = Stage::waiting;
    static std::chrono::steady_clock::time_point began;
    static int reported = 0;
    if (stage == Stage::done || !gChecksDone) return;
    if (stage == Stage::waiting) {
#if MEGLER_AUTO_JAVA
        // The package's own Java test app (demo\hello.apk: tools/build-appx.sh --java), copied into the library afresh:
        // an older build's copy may lie there.
        static bool javaAdded = false;
        if (!javaAdded) {
            javaAdded = true;
            if (!Library::hasPackaged("hello.apk") || !Library::instance().addPackaged("hello.apk")) {
                autoLine("java: this package has no demo\\hello.apk, or it could not be copied into the library (build with --java)");
                finish();
                stage = Stage::done;
            }
            return;
        }
#else
        // The flag "demo" (a file of that name in LocalState\flags): the
        // package's own demo is what is played, added first if need be.
        static const bool demoWanted = files::exists(platform::localStateDirectory() + L"\\flags\\demo");
        static bool demoAdded = false;
        if (demoWanted && !demoAdded) {
            demoAdded = true;
            if (!Library::instance().find("megler-demo.apk")) Library::instance().addDemo();
            return;
        }
        // The flag "app-NAME": NAME.apk is what is played -- the package's own copy of it (a --java package has
        // hello.apk and demos.apk), put into the library afresh, or else whatever the library has by that name.
        static std::string wantedApp;
        static bool wantedRead = false;
        if (!wantedRead) {
            wantedRead = true;
            for (const std::wstring& name : files::list(platform::localStateDirectory() + L"\\flags", L"app-*"))
                if (name.size() > 4) wantedApp = narrow(name.substr(4)) + ".apk";
            if (!wantedApp.empty()) {
                if (Library::hasPackaged(wantedApp)) Library::instance().addPackaged(wantedApp);
                return;
            }
        }
#endif
        // Once the library has read everything it holds.
        const auto entries = Library::instance().entries();
        for (const auto& entry : entries)
            if (!entry.analysed || entry.analysing) return;
        const ApkEntry* pick = nullptr;
#if MEGLER_AUTO_JAVA
        for (const auto& entry : entries)
            if (entry.id == "hello.apk") pick = &entry;
        if (!pick || !pick->report.ready()) {
            autoLine("java: hello.apk cannot be started: " + (pick ? pick->report.status() : std::string("it is not in the library")));
            finish();
            stage = Stage::done;
            return;
        }
#else
        for (const auto& entry : entries) {
            if (!entry.report.runnable) continue;
            if (!wantedApp.empty()) {
                if (entry.id == wantedApp) pick = &entry;
                continue;
            }
            const bool demo = entry.id == "megler-demo.apk";
            if (!pick || (demo == demoWanted && (pick->id == "megler-demo.apk") != demoWanted)) pick = &entry;
        }
        if (!pick && !wantedApp.empty()) {
            autoLine("play: " + wantedApp + " (the flag app-...) is not in the library or cannot run here");
            finish();
            stage = Stage::done;
            return;
        }
        if (!pick) {
            autoLine(format("play: nothing in the library can run (%u APKs there)", unsigned(entries.size())));
            finish();
            stage = Stage::done;
            return;
        }
#endif
        autoLine("play: " + pick->id + " (" + pick->report.package + ", " + pick->report.engine + ")");
        shellLog::write("auto run: playing " + pick->id);
        shell.push(ui::makePlayerScreen(shell, pick->id));
        began = std::chrono::steady_clock::now();
        stage = Stage::playing;
        return;
    }
    const session::Snapshot snap = session::snapshot();
    const long long elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
    const int seconds = int(elapsedMs / 1000);
    const auto window = session::window();
    const unsigned frames = window ? window->presented() : 0;
    const bool ended = snap.state == session::State::exited || snap.state == session::State::failed;

    // Flags for a run nobody watches or touches (files in LocalState\flags;
    // only their names are read, since what the Device Portal puts there
    // cannot be opened):
    //   shots            the screen every five seconds, as shot-NNN.bmp in LocalState
    //                    (the Java run takes them without being asked)
    //   tap-S-X-Y        a finger down at second S at X,Y of a 1280x720
    //                    picture (y down) fitted into the app's surface and
    //                    centred: a game that changes its resolution is
    //                    tapped in the same places, and so is one that keeps
    //                    to 16:9 on a screen that did not turn to landscape
    //                    (a locked phone's). Up a moment later, and the
    //                    screen two seconds after that, as shot-tap-S.bmp
    //   px-S-X-Y         the same at X,Y of the app's surface as it is, in
    //                    pixels: for an app that is held upright, whose rows
    //                    and buttons are where its own log says
    //   key-S-C          the key with Android's code C (66 Enter, 4 Back, 19 to
    //                    22 the arrows up, down, left, right) pressed at second
    //                    S and let go a moment later; the screen two seconds
    //                    after, as shot-key-S.bmp
    //   play-N           the run lasts N seconds instead of 120 (45 for the Java run)
    struct Tap {
        long long ms;
        int second, x, y, stage;
        bool direct; // x and y are pixels of the app's surface as they are (the Java run's own taps)
    };
    static std::vector<Tap> taps, keys;        // (a key is a Tap whose x is its code)
    static bool shots = MEGLER_AUTO_JAVA != 0, flagsRead = false;
    static int lastShot = -1;
    if (!flagsRead) {
        flagsRead = true;
        const std::wstring folder = platform::localStateDirectory() + L"\\flags";
        shots = shots || files::exists(folder + L"\\shots");
        for (const std::wstring& name : files::list(folder, L"play-*")) {
            int wanted = 0;
            if (swscanf_s(name.c_str(), L"play-%d", &wanted) == 1 && wanted >= 10 && wanted <= 3600) playSeconds = wanted;
        }
        for (const std::wstring& name : files::list(folder, L"tap-*")) {
            Tap t = {};
            if (swscanf_s(name.c_str(), L"tap-%d-%d-%d", &t.second, &t.x, &t.y) == 3) {
                t.ms = 1000ll * t.second;
                taps.push_back(t);
            }
        }
        for (const std::wstring& name : files::list(folder, L"px-*")) {
            Tap t = {};
            if (swscanf_s(name.c_str(), L"px-%d-%d-%d", &t.second, &t.x, &t.y) == 3) {
                t.ms = 1000ll * t.second;
                t.direct = true;
                taps.push_back(t);
            }
        }
        for (const std::wstring& name : files::list(folder, L"key-*")) {
            Tap t = {};
            if (swscanf_s(name.c_str(), L"key-%d-%d", &t.second, &t.x) == 2) {
                t.ms = 1000ll * t.second;
                keys.push_back(t);
            }
        }
        if (shots || !taps.empty()) autoLine(format("play: %s%u taps to make", shots ? "screenshots every 5 s; " : "", unsigned(taps.size())));
    }
#if MEGLER_AUTO_JAVA
    // The Java test app says in the log what it does ("I/HelloTest: ..."), and
    // where its button is once it has been laid out. Its lines are copied
    // into megler-auto.log, and its button is tapped where the app says it is:
    // twice, four and nine seconds after its first picture; Back seven
    // seconds after the second tap, which ends the app, and with it the run.
    // An app that has not said where its button is ten seconds after its
    // first picture is tapped where the button would be at the test's own
    // size, by rule of three: a guess, and the log says so.
    {
        static uint64_t cursor = 0;
        static bool cursorSet = false, planned = false;
        static long long firstPictureMs = -1;
        static int buttonX = -1, buttonY = -1;
        std::vector<std::string> lines;
        if (!cursorSet) {
            // (What the log held before the app was started is not the app's.)
            cursorSet = true;
            cursor = shellLog::take(0, lines);
            lines.clear();
        }
        cursor = shellLog::take(cursor, lines);
        for (const std::string& line : lines) {
            const size_t at = line.find("HelloTest: ");
            if (at == std::string::npos) continue;
            autoLine("java: the app: " + line.substr(at + 11, 200));
            const size_t button = line.find(" button=");
            int x = 0, y = 0, w = 0, h = 0;
            if (line.find("layout focus=true") != std::string::npos && button != std::string::npos &&
                sscanf_s(line.c_str() + button, " button=%d,%d,%dx%d", &x, &y, &w, &h) == 4 && w > 0 && h > 0) {
                buttonX = x + w / 2;
                buttonY = y + h / 2;
            }
        }
        if (frames > 0 && firstPictureMs < 0) {
            firstPictureMs = elapsedMs;
            autoLine(format("java: the first picture after %lld ms%s", elapsedMs, window ? (": " + window->description()).c_str() : ""));
        }
        if (!planned && firstPictureMs >= 0 && window && (buttonX >= 0 || elapsedMs >= firstPictureMs + 10000)) {
            planned = true;
            if (buttonX < 0) {
                // With Android 9's action bar over it the button's middle is some 136 dp down, and a phone's shorter side is some 450 dp.
                buttonX = window->surfaceWidth() / 2;
                buttonY = 136 * std::min(window->surfaceWidth(), window->surfaceHeight()) / 450;
                autoLine(format("java: the app has not said where its button is; tapping at %d,%d, which is a guess", buttonX, buttonY));
            } else {
                autoLine(format("java: the button's middle is at %d,%d of %dx%d, by the app's own account", buttonX, buttonY, window->surfaceWidth(),
                                window->surfaceHeight()));
            }
            const long long from = std::max(elapsedMs, firstPictureMs + 4000);
            for (int i = 0; i < 2; i++) {
                Tap t = {};
                t.ms = from + 5000ll * i;
                t.second = int(t.ms / 1000);
                t.x = buttonX;
                t.y = buttonY;
                t.direct = true;
                taps.push_back(t);
            }
            Tap back = {};
            back.ms = from + 12000;
            back.second = int(back.ms / 1000);
            back.x = 4; // AKEYCODE_BACK
            keys.push_back(back);
        }
    }
#endif
    if (!ended && window) {
        const std::wstring state = platform::localStateDirectory();
        if (shots && seconds / 5 > lastShot) {
            lastShot = seconds / 5;
            window->requestScreenshot(state + widen(format("\\shot-%03d.bmp", seconds)));
        }
        for (Tap& t : taps) {
            const float surfaceW = float(window->surfaceWidth()), surfaceH = float(window->surfaceHeight());
            const float fit = std::min(surfaceW / 1280.0f, surfaceH / 720.0f);
            const float tapX = t.direct ? float(t.x) : (surfaceW - 1280.0f * fit) / 2 + float(t.x) * fit;
            const float tapY = t.direct ? float(t.y) : (surfaceH - 720.0f * fit) / 2 + float(t.y) * fit;
            if (t.stage == 0 && elapsedMs >= t.ms) {
                session::touch(0, 1, tapX, tapY);
                t.stage = 1;
                autoLine(format("play: %d s, tap at %d,%d (%.0f,%.0f of %dx%d)", seconds, t.x, t.y, double(tapX), double(tapY),
                                window->surfaceWidth(), window->surfaceHeight()));
            } else if (t.stage == 1 && elapsedMs >= t.ms + 150) {
                session::touch(2, 1, tapX, tapY);
                t.stage = 2;
            } else if (t.stage == 2 && elapsedMs >= t.ms + 2000) {
                window->requestScreenshot(state + widen(format("\\shot-tap-%03d.bmp", t.second)));
                t.stage = 3;
            }
        }
        for (Tap& t : keys) {
            if (t.stage == 0 && elapsedMs >= t.ms) {
                megler_key(t.x, true);
                t.stage = 1;
                autoLine(format("play: %d s, key %d", seconds, t.x));
            } else if (t.stage == 1 && elapsedMs >= t.ms + 120) {
                megler_key(t.x, false);
                t.stage = 2;
            } else if (t.stage == 2 && elapsedMs >= t.ms + 2000) {
                window->requestScreenshot(state + widen(format("\\shot-key-%03d.bmp", t.second)));
                t.stage = 3;
            }
        }
    }
    // A line every ten seconds: how far it has got is in the auto log too.
    if (!ended && seconds / 10 > reported && seconds < playSeconds) {
        reported = seconds / 10;
        autoLine(format("play: %d s, %u frames shown", seconds, frames));
        return;
    }
    if (!ended && seconds < playSeconds) return;
    if (ended)
        autoLine(format("play: ended after %d s, %u frames shown, status %d%s%s", seconds, frames, snap.exitStatus,
                        snap.reason.empty() ? "" : ": ", snap.reason.substr(0, 300).c_str()));
    else
        autoLine(format("play: still running after %d s, %u frames shown", seconds, frames));
    finish();
    stage = Stage::done;
#else
    (void)shell;
#endif
}

} // namespace megler::app::autorun
