#include "pch.h"

#include "ui/UiShots.h"

#include "library/Library.h"
#include "shell/Platform.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"
#include "ui/AppShell.h"

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

/// The names of the flags that begin with ui-, not seen before.
std::vector<std::wstring> newFlags() {
    std::vector<std::wstring> found;
    WIN32_FIND_DATAW data;
    const std::wstring pattern = platform::localStateDirectory() + L"\\flags\\ui-*";
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return found;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (seen().insert(data.cFileName).second) found.push_back(data.cFileName);
    } while (FindNextFileW(h, &data));
    FindClose(h);
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
        } else if (flag.rfind(L"ui-tab-", 0) == 0) {
            const int tab = lastNumber(flag);
            if (tab >= 0 && tab <= 3) shell.selectTab(tab);
        } else if (flag.rfind(L"ui-open-", 0) == 0) {
            const int n = lastNumber(flag);
            const auto entries = Library::instance().entries();
            if (n >= 0 && size_t(n) < entries.size()) shell.push(makeAppDetailScreen(shell, entries[size_t(n)].id));
        } else if (flag.rfind(L"ui-back-", 0) == 0) {
            shell.goBack();
        }
    }
}

} // namespace megler::app::ui::uiShots
