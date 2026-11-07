#include "pch.h"

#include "player/DisplayHost.h"

namespace megler::app {

DisplayHost& DisplayHost::shared() {
    // Never destroyed: the app's threads may be in a call when the process ends.
    static DisplayHost* const host = new DisplayHost();
    return *host;
}

megler_display_host DisplayHost::callbacks() {
    megler_display_host host = {};
    host.user = this;
    host.orientation = &orientationThunk;
    return host;
}

void DisplayHost::attach(std::function<void(int)> orientation) {
    std::function<void(int)> now;
    int last = MEGLER_ORIENTATION_UNSPECIFIED;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dispatcher_ = winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread().Dispatcher();
        orientation_ = std::move(orientation);
        ++generation_;
        if (asked_) {
            now = orientation_;
            last = last_;
        }
    }
    // (The app asked before the screen was there to take it: the screen hears it now, as it would have.)
    if (now) now(last);
}

void DisplayHost::detach() {
    std::lock_guard<std::mutex> lock(mutex_);
    orientation_ = nullptr;
    ++generation_;
}

void DisplayHost::orientationThunk(void* user, int requested) {
    auto* self = static_cast<DisplayHost*>(user);
    winrt::Windows::UI::Core::CoreDispatcher dispatcher{nullptr};
    unsigned generation;
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        self->asked_ = true;
        self->last_ = requested;
        dispatcher = self->dispatcher_;
        generation = self->generation_;
    }
    if (!dispatcher) return;
    try {
        dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [self, generation, requested] {
            std::function<void(int)> orientation;
            {
                std::lock_guard<std::mutex> lock(self->mutex_);
                if (generation == self->generation_) orientation = self->orientation_;
            }
            if (orientation) orientation(requested);
        });
    } catch (const winrt::hresult_error&) {
        // The window is going away; so is the request.
    }
}

} // namespace megler::app
