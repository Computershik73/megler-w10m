// The one run of an Android app this process has (core/include/megler.h: one
// app runs per process, since Android libraries cannot be unloaded): its
// state for the screens to poll, and everything the shell tells the running
// app -- touches, Back, pause, focus, stop.
//
// The core's callbacks arrive on its own threads and only record what
// happened here; the player screen sees it at its next poll.
#pragma once

#include "library/Library.h"
#include "player/AudioHost.h"
#include "player/WindowHost.h"

#include <memory>
#include <string>

namespace megler::app::session {

enum class State {
    idle,     // nothing has run
    starting, // megler_run is being called
    running,
    exited, // on_exit came: by the app's own exit, a crash, or megler_stop
    failed, // megler_run returned false: nothing started
};

struct Snapshot {
    State state = State::idle;
    std::string apkId;
    std::string title;
    int exitStatus = 0;
    /// on_exit's reason (empty for a plain exit), or megler_last_error after a
    /// start that failed.
    std::string reason;
    bool stopRequested = false;
};

Snapshot snapshot();

/// Whether an app is running or has run: nothing else can start until the
/// process has ended. A start that failed (megler_run returned false) does not
/// count; forget() makes the session idle again after one.
bool spent();
void forget();
/// Why Play cannot be offered -- an app has run, or the self-test has run
/// its own -- in a sentence for the user; empty when it can.
std::string unavailable();

/// UI thread. Starts `apk` with a surface of `width` x `height` pixels shown
/// in `panel`; megler_run itself is called on a worker thread, so that the UI
/// thread stays free for the swap chain the app's render thread will ask it
/// to take (WindowHost). False when a run is already there.
bool start(const ApkEntry& apk, winrt::Windows::UI::Xaml::Controls::SwapChainPanel const& panel, int width, int height,
           int densityDpi);

/// The run's screen and its sound, or null before start().
std::shared_ptr<WindowHost> window();
std::shared_ptr<AudioHost> audio();

/// megler_stop, if the app runs; on_exit follows.
void stop();
/// Surface pixels, y down. phase: 0 down, 1 move, 2 up, 3 cancel.
void touch(int phase, int pointer, float x, float y);
/// AKEYCODE_BACK, pressed and released.
void backKey();

// The app's place on the phone, from the application's and the window's
// events. Android has one "paused": here it is suspended or not visible.
void setSuspended(bool suspended);
void setVisible(bool visible);
void setFocused(bool focused);

} // namespace megler::app::session
