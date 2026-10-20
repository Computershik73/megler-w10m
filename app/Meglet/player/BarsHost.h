// The phone's bars for a Java app: meglet_system_bars_host (core/include/meglet.h), whose one call -- which bars the app's
// window wants hidden, and whether its status bar is light -- arrives on the app's own main thread and must return at
// once, handed over to the UI thread, where the player's screen hides and shows the phone's status and navigation bars
// (ui/PlayerScreen.cpp, through ui/AppShell.cpp).
//
// What the bars then cover goes the other way through session::setInsets (player/Session.h).
//
// One BarsHost serves the process: shared().
#pragma once

#include "meglet.h"

#include <functional>
#include <mutex>

namespace meglet::app {

class BarsHost {
public:
    static BarsHost& shared();
    BarsHost(const BarsHost&) = delete;
    BarsHost& operator=(const BarsHost&) = delete;

    /// The callbacks for meglet_run_config.bars, with this as their user.
    meglet_system_bars_host callbacks();

    /// UI thread: the screen that shows the app takes the requests from now on, on this thread, in the order asked.
    void attach(std::function<void(int bars)> wanted);
    /// UI thread: nobody takes them any more (requests still on their way are dropped).
    void detach();

private:
    BarsHost() = default;

    static void wantedThunk(void* user, int bars);

    std::mutex mutex_; // everything below
    winrt::Windows::UI::Core::CoreDispatcher dispatcher_{nullptr};
    std::function<void(int)> wanted_;
    unsigned generation_ = 0; // goes up at every attach and detach: a request posted before is dropped after
};

} // namespace meglet::app
