#include "pch.h"

#include "ui/UiShots.h"

#include "library/Library.h"
#include "megler.h"
#include "player/Session.h"
#include "player/WindowHost.h"
#include "shell/Platform.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"
#include "ui/AppShell.h"
#include "ui/SeparateApps.h"

#include <winrt/Windows.Graphics.Imaging.h>

#include <set>

namespace megler::app::ui::uiShots {

using namespace winrt;
using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::UI::Xaml;

namespace {

std::set<std::wstring>& seen() {
    static std::set<std::wstring> names;
    return names;
}

/// The names of the flags that begin with ui-, not seen before; each is removed once found, so that it is not acted on
/// again the next time Megler starts -- by the user, after the run that set it.
std::vector<std::wstring> newFlags() {
    std::vector<std::wstring> found;
    WIN32_FIND_DATAW data;
    const std::wstring folder = platform::localStateDirectory() + L"\\flags\\";
    const std::wstring pattern = folder + L"ui-*";
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return found;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (seen().insert(data.cFileName).second) found.push_back(data.cFileName);
    } while (FindNextFileW(h, &data));
    FindClose(h);
    for (const std::wstring& name : found) DeleteFileW((folder + name).c_str());
    std::sort(found.begin(), found.end());
    return found;
}

/// The number at the end of a name, after its last '-'; -1 when there is none.
int lastNumber(const std::wstring& name) {
    const size_t dash = name.rfind(L'-');
    if (dash == std::wstring::npos || dash + 1 >= name.size()) return -1;
    int n = 0;
    for (size_t i = dash + 1; i < name.size(); ++i) {
        if (name[i] < L'0' || name[i] > L'9') return -1;
        n = n * 10 + (name[i] - L'0');
    }
    return n;
}

fire_and_forget shoot(std::wstring name) {
    try {
        auto content = Window::Current().Content();
        Media::Imaging::RenderTargetBitmap target;
        co_await target.RenderAsync(content);
        auto pixels = co_await target.GetPixelsAsync();
        const uint32_t width = uint32_t(target.PixelWidth()), height = uint32_t(target.PixelHeight());
        auto folder = ApplicationData::Current().LocalFolder();
        auto file = co_await folder.CreateFileAsync(L"shot-ui-" + name + L".bmp", CreationCollisionOption::ReplaceExisting);
        auto stream = co_await file.OpenAsync(FileAccessMode::ReadWrite);
        auto encoder = co_await BitmapEncoder::CreateAsync(BitmapEncoder::BmpEncoderId(), stream);
        std::vector<uint8_t> bytes(pixels.Length());
        memcpy(bytes.data(), pixels.data(), bytes.size());
        // The picture is premultiplied; a BMP has no alpha that anything reads, and the window is opaque.
        encoder.SetPixelData(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Ignore, width, height, 96, 96, bytes);
        co_await encoder.FlushAsync();
        stream.Close();
        shellLog::write(format("ui: a picture of the window, %ux%u: shot-ui-%s.bmp", width, height, narrow(name).c_str()));
    } catch (const hresult_error& e) {
        shellLog::write(format("ui: the window's picture failed (0x%08X)", unsigned(e.code().value)));
    }
}

// Runs only on demand in the foreground app. Use the normal input queue
// and space samples with a dispatcher timer instead of blocking the UI.
void scrollSample(bool upward) {
    static DispatcherTimer timer{nullptr};
    if (timer || session::snapshot().state != session::State::running) return;
    auto window = session::window();
    if (!window || !window->presented()) return;
    const float x = window->surfaceWidth() * 0.50f;
    const float from = window->surfaceHeight() * (upward ? 0.76f : 0.27f);
    const float to = window->surfaceHeight() * (upward ? 0.27f : 0.76f);
    const auto began = GetTickCount64();
    const auto before = window->presented();
    window->startFrameSample();
    shellLog::write("ui: scroll sample started");
    session::touch(0, 0, x, from);
    timer = DispatcherTimer();
    timer.Interval(std::chrono::milliseconds(16));
    timer.Tick([window, x, from, to, began, before](auto&&, auto&&) {
        const auto elapsed = GetTickCount64() - began;
        if (session::window() != window || session::snapshot().state != session::State::running) {
            window->endFrameSample();
            timer.Stop();
            timer = nullptr;
            return;
        }
        if (elapsed < 320) {
            session::touch(1, 0, x, from + (to - from) * (float(elapsed) / 320));
        } else if (timer.Interval() < std::chrono::milliseconds(100)) {
            // Release once even if the dispatcher stalled during the gesture.
            session::touch(2, 0, x, to);
            timer.Interval(std::chrono::milliseconds(100));
        } else if (elapsed >= 2000) {
            shellLog::write(format("ui: scroll sample ended, %u presents in %llu ms",
                                   unsigned(window->presented() - before), elapsed));
            auto intervals = window->endFrameSample();
            if (!intervals.empty()) {
                std::sort(intervals.begin(), intervals.end());
                const size_t p95 = (intervals.size() * 95 + 99) / 100 - 1;
                shellLog::write(format("ui: scroll intervals: %u, p50 %.1f ms, p95 %.1f ms, max %.1f ms",
                    unsigned(intervals.size()), intervals[intervals.size() / 2], intervals[p95], intervals.back()));
            }
            timer.Stop();
            timer = nullptr;
        }
    });
    timer.Start();
}

} // namespace

void poll(AppShell& shell) {
    static unsigned long long next = 0;
    const unsigned long long now = GetTickCount64();
    if (now < next) return;
    next = now + 1000;
    for (const std::wstring& flag : newFlags()) {
        shellLog::write("ui: flag " + narrow(flag));
        if (flag.rfind(L"ui-shot-", 0) == 0) {
            shoot(flag.substr(8));
        } else if (flag.rfind(L"ui-scroll-up-", 0) == 0) {
            scrollSample(true);
        } else if (flag.rfind(L"ui-scroll-down-", 0) == 0) {
            scrollSample(false);
        } else if (flag.rfind(L"ui-tap-", 0) == 0) {
            int x, y;
            const auto window = session::window();
            if (window && session::snapshot().state == session::State::running &&
                swscanf_s(flag.c_str(), L"ui-tap-%d-%d", &x, &y) == 2 &&
                x >= 0 && y >= 0 && x < window->surfaceWidth() && y < window->surfaceHeight()) {
                session::touch(0, 0, float(x), float(y));
                session::touch(2, 0, float(x), float(y));
            }
        } else if (flag.rfind(L"ui-dump-module-", 0) == 0) {
            // A DLL of the system's as it lies in memory (megler_dump_module), to read a fault in its code.
            const std::wstring module = flag.substr(15);
            megler_dump_module(narrow(module).c_str(), narrow(platform::localStateDirectory() + L"\\module-" + module + L".img").c_str());
        } else if (flag == L"ui-probe-breakpoints") {
            // Whether the system stops a thread at a hardware breakpoint (megler_probe_breakpoints): said in the log.
            char out[2048];
            megler_probe_breakpoints(out, sizeof(out));
            shellLog::write(std::string("ui: breakpoints: ") + out);
        } else if (flag.rfind(L"ui-frame-", 0) == 0) {
            if (auto window = session::window())
                window->requestScreenshot(platform::localStateDirectory() + L"\\shot-" + flag.substr(3) + L".bmp");
        } else if (flag.rfind(L"ui-tab-", 0) == 0) {
            const int tab = lastNumber(flag);
            if (tab >= 0 && tab <= 3) shell.selectTab(tab);
        } else if (flag.rfind(L"ui-open-", 0) == 0) {
            const int n = lastNumber(flag);
            const auto entries = Library::instance().entries();
            if (n >= 0 && size_t(n) < entries.size()) shell.push(makeAppDetailScreen(shell, entries[size_t(n)].id));
        } else if (flag.rfind(L"ui-back-", 0) == 0) {
            // (A Back the phone's button would leave to the system -- the app went to the back with it -- does nothing
            // more here: only the button has Windows take the window away. ui-quit closes an app.)
            if (!shell.goBack()) shellLog::write("ui: the Back is the system's now; only the phone's button has Windows take the window away");
        } else if (flag.rfind(L"ui-quit-", 0) == 0) {
            shell.quitAndClose();
        } else if (flag.rfind(L"ui-dialog-", 0) == 0) {
            // The dialog up now (the first start's question, an alert) answered with its button n, from the left.
            const int n = lastNumber(flag);
            shellLog::write(shell.dialogShown() ? format("ui: the dialog answered with its button %d", n) : std::string("ui: no dialog to answer"));
            if (n >= 0) shell.pressDialog(n);
        } else if (flag == L"ui-update-all") {
            separateApps::updateAll(shell);
        } else if (flag.rfind(L"ui-uninstall-", 0) == 0) {
            separateApps::uninstall(shell, narrow(flag.substr(13)));
        } else if (flag.rfind(L"ui-update-", 0) == 0) {
            const std::string package = narrow(flag.substr(10));
            bool found = false;
            for (const auto& e : Library::instance().entries()) {
                if (e.report.package != package) continue;
                separateApps::install(shell, e.id);
                found = true;
                break;
            }
            if (!found) shellLog::write("ui: no app of the package " + package + " in the library to update");
        }
    }
}

} // namespace megler::app::ui::uiShots
