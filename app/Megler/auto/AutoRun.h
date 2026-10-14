// The unattended on-device run: a build made with
//
//   tools/build-appx.sh arm --auto probe|selftest|gfx|storage|net|play|java (or several, with commas)
//
// does, at start-up, without anyone touching the phone:
//
//  * probe: megler_probe (and the shell's own check that the loader takes
//    ANGLE as packaged), into LocalState\megler-probe.log;
//  * selftest: megler_selftest, into LocalState\megler-selftest.log
//
// -- the same checks, through the same code, as the Device screen's two
// buttons (shell/Checks.h) -- and
//
//  * gfx: the scenes the core's tests draw through a megler_gfx_host
//    (core/tests/hk_gfx_scenes.c), drawn by the shell's own host
//    (player/GfxHost.h: Direct2D, DirectWrite, WIC) on this device and
//    checked; what it says is in megler-auto.log, and its pictures are
//    LocalState\shot-gfx-*.bmp;
//  * storage: the phone's folders in an app's storage (player/StorageHost.h)
//    gone through as an app goes through them, by the layer's own file
//    functions (megler_storage_check): in the music, the pictures, the videos
//    and every other link, a folder made, a file written, read back,
//    stat'ed, renamed, moved and removed, the folder removed. A line in
//    megler-auto.log for each link the shell found and for each step, then how
//    Windows answered, then "DONE storage: N checks, M failed". It works in a
//    folder of its own in each, megler-storage-check, removes it again, and
//    touches nothing else of the user's. It runs no app;
//  * net: the network gone through as an app goes through it, by the
//    layer's own C library (megler_net_check): "localhost", the interfaces
//    (names, not addresses), a public name resolved and a HEAD request
//    answered on its port 80, the same against a server of its own on
//    127.0.0.1, a UDP round trip on 127.0.0.1. A line in megler-auto.log for
//    each step, then "DONE net: N checks, M failed" (and how many steps the
//    phone itself refused, when it refused an app its own loopback). It runs
//    no app;
//  * play: opens the player on the library's game (the first that can run
//    and is not the demo; the demo when there is nothing else) as a tap on
//    Play would, and lets it run for two minutes or until it ends. What it
//    did is in megler.log. Not with selftest, which uses up the one app a
//    start of Megler can run;
//  * java: adds the Java test app the package carries (demo\hello.apk, in a
//    package built with --java) to the library and opens the player on it;
//    takes its picture every five seconds (shot-NNN.bmp); copies what the
//    app says in the log ("I/HelloTest: ...") into megler-auto.log; taps its
//    button, twice, where the app's own log says the button is; presses
//    Back, which ends the app and with it the run. Not with selftest or
//    play.
//
// LocalState\megler-auto.log says what ran and ends with the line "AUTO DONE";
// tools/phone.py auto installs such a build, launches it, waits for that line
// and brings every log back. A normal build (no --auto) compiles this to
// nothing: start() returns false.
//
// Every log here is written a line at a time, the file opened, appended to
// and closed each time: the Device Portal won't copy a file another process
// holds open for writing.
#pragma once

#include <string>

namespace megler::app::ui {
class AppShell;
}

namespace megler::app::autorun {

/// Starts the run on a thread of its own if this build has one, after the
/// shell has called megler_init. `description` is the logs' first line
/// ("Megler 0.1.0.7 (ARM, Windows.Mobile 10.0.15254.603)"). False, doing
/// nothing, in a normal build.
bool start(std::string description);

/// The UI thread's part, four times a second (AppShell::poll): the play
/// stage, which goes through the shell's own pages. Nothing in a build
/// without it.
void tick(ui::AppShell& shell);

/// "probe,selftest", "probe", "selftest", "gfx", "storage", "net", "play", "java"..., or "" in a normal build.
const char* modes();

} // namespace megler::app::autorun
