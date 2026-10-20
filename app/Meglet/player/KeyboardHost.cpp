#include "pch.h"

#include "player/KeyboardHost.h"

#include "shell/ShellLog.h"

namespace meglet::app {

KeyboardHost& KeyboardHost::shared() {
    // Never destroyed: the app's main thread may be in a call when the process ends.
    static KeyboardHost* const host = new KeyboardHost();
    return *host;
}

meglet_keyboard_host KeyboardHost::callbacks() {
    meglet_keyboard_host host = {};
    host.user = this;
    host.show = &showThunk;
    host.hide = &hideThunk;
    return host;
}

void KeyboardHost::attach(std::function<void(int, int, int)> show, std::function<void()> hide) {
    std::lock_guard<std::mutex> lock(mutex_);
    dispatcher_ = winrt::Windows::UI::Core::CoreWindow::GetForCurrentThread().Dispatcher();
    show_ = std::move(show);
    hide_ = std::move(hide);
    ++generation_;
}

void KeyboardHost::detach() {
    std::lock_guard<std::mutex> lock(mutex_);
    show_ = nullptr;
    hide_ = nullptr;
    ++generation_;
}

/// Runs `work` on the UI thread, later; returns at once.
void KeyboardHost::post(std::function<void()> work) {
    winrt::Windows::UI::Core::CoreDispatcher dispatcher{nullptr};
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dispatcher = dispatcher_;
    }
    if (!dispatcher) return;
    try {
        dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [work = std::move(work)] { work(); });
    } catch (const winrt::hresult_error&) {
        // The window is going away; so is the request.
    }
}

void KeyboardHost::showThunk(void* user, int kind, int flags, int enter) {
    auto* self = static_cast<KeyboardHost*>(user);
    unsigned generation;
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        generation = self->generation_;
    }
    self->post([self, generation, kind, flags, enter] {
        std::function<void(int, int, int)> show;
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            if (generation == self->generation_) show = self->show_;
        }
        if (show) show(kind, flags, enter);
    });
}

void KeyboardHost::hideThunk(void* user) {
    auto* self = static_cast<KeyboardHost*>(user);
    unsigned generation;
    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        generation = self->generation_;
    }
    self->post([self, generation] {
        std::function<void()> hide;
        {
            std::lock_guard<std::mutex> lock(self->mutex_);
            if (generation == self->generation_) hide = self->hide_;
        }
        if (hide) hide();
    });
}

} // namespace meglet::app
