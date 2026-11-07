// Which way up the running app wants its screen: megler_display_host (core/include/megler.h), whose one call -- Android's
// SCREEN_ORIENTATION_* of the activity in front, from its manifest or from setRequestedOrientation -- arrives on one of the
// app's threads and must return at once, handed over to the UI thread, where the player's screen turns the phone's display
// (DisplayInformation::AutoRotationPreferences, through ui/AppShell.cpp) or, where the window does not turn, gives the app a
// surface of the shape it asked for (ui/PlayerScreen.cpp).
//
// The surface's new size goes the other way through session::setSurface (player/Session.h).
//
// One DisplayHost serves the process: shared().
#pragma once

#include "megler.h"

#include <functional>
#include <mutex>

namespace megler::app {

class DisplayHost {
public:
    static DisplayHost& shared();
    DisplayHost(const DisplayHost&) = delete;
    DisplayHost& operator=(const DisplayHost&) = delete;

    /// The callbacks for megler_run_config.display, with this as their user.
    megler_display_host callbacks();

    /// UI thread: the screen that shows the app takes the requests from now on, on this thread, in the order asked. One that
    /// came before anyone took them is handed over at once.
    void attach(std::function<void(int requested)> orientation);
    /// UI thread: nobody takes them any more (requests still on their way are dropped).
    void detach();

private:
    DisplayHost() = default;

    static void orientationThunk(void* user, int requested);

    std::mutex mutex_; // everything below
    winrt::Windows::UI::Core::CoreDispatcher dispatcher_{nullptr};
    std::function<void(int)> orientation_;
    unsigned generation_ = 0; // goes up at every attach and detach: a request posted before is dropped after
    bool asked_ = false;      // the app has asked for something, last_ (before anyone took it, too)
    int last_ = MEGLER_ORIENTATION_UNSPECIFIED;
};

} // namespace megler::app
