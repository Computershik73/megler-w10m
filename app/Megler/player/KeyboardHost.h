// The touch keyboard for a Java app's text fields: megler_keyboard_host
// (core/include/megler.h), whose two calls arrive on the app's own main thread
// and must return at once, handed over to the UI thread, where the player's
// screen has the means to bring Windows' keyboard up (ui/PlayerScreen.cpp:
// a text box nobody sees, which is what Windows gives a keyboard to).
//
// What the user types goes the other way through session::textCommit and
// its neighbours (player/Session.h).
//
// One KeyboardHost serves the process: shared().
#pragma once

#include "megler.h"

#include <functional>
#include <mutex>

namespace megler::app {

class KeyboardHost {
public:
    static KeyboardHost& shared();
    KeyboardHost(const KeyboardHost&) = delete;
    KeyboardHost& operator=(const KeyboardHost&) = delete;

    /// The callbacks for megler_run_config.keyboard, with this as their user.
    megler_keyboard_host callbacks();

    /// UI thread: the screen that shows the app takes the requests from now
    /// on; both functions are called on this thread, in the order asked.
    void attach(std::function<void(int kind, int flags, int enter)> show, std::function<void()> hide);
    /// UI thread: nobody takes them any more (requests still on their way are dropped).
    void detach();

private:
    KeyboardHost() = default;

    static void showThunk(void* user, int kind, int flags, int enter);
    static void hideThunk(void* user);
    void post(std::function<void()> work);

    std::mutex mutex_; // everything below
    winrt::Windows::UI::Core::CoreDispatcher dispatcher_{nullptr};
    std::function<void(int, int, int)> show_;
    std::function<void()> hide_;
    unsigned generation_ = 0; // goes up at every attach and detach: a request posted before is dropped after
};

} // namespace megler::app
