#include "pch.h"

#include "player/BarsHost.h"

namespace megler::app {

BarsHost& BarsHost::shared() {
    // Never destroyed: the app's main thread may be in a call when the process ends.
    static BarsHost* const host = new BarsHost();
    return *host;
}

megler_system_bars_host BarsHost::callbacks() {
    megler_system_bars_host host = {};
    host.user = this;
    host.wanted = &wantedThunk;
    return host;
}

void BarsHost::attach(std::function<void(int)> wanted) {
    std::lock_guard<std::mutex> lock(mutex_);
    dispatcher_ = winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread().Dispatcher();
    wanted_ = std::move(wanted);
    ++generation_;
}

void BarsHost::detach() {
    std::lock_guard<std::mutex> lock(mutex_);
    wanted_ = nullptr;
    ++generation_;
}

void BarsHost::wantedThunk(void* user, int bars) {
    auto* self = static_cast<BarsHost*>(user);
    winrt::Windows::UI::Core::CoreDispatcher dispatcher{nullptr};
    unsigned generation;
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        dispatcher = self->dispatcher_;
        generation = self->generation_;
    }
    if (!dispatcher) return;
    try {
        dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [self, generation, bars] {
            std::function<void(int)> wanted;
            {
                std::lock_guard<std::mutex> lock(self->mutex_);
                if (generation == self->generation_) wanted = self->wanted_;
            }
            if (wanted) wanted(bars);
        });
    } catch (const winrt::hresult_error&) {
        // The window is going away; so is the request.
    }
}

} // namespace megler::app
