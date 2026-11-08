// The test program's way to the shell's window: player/WindowHost.cpp itself,
// for mg_tests --window-windows (mg_tests_main.c). A WindowHost made with no
// panel -- a test program has no XAML -- makes its swap chains and presents
// to them as the player's does, and they are never shown; everything else is
// the shell's: the device it takes (GfxHost's, on this PC's graphics
// processor, or one of the test's own standing for ANGLE's), the pictures
// counted as they come, the first kept and said in the log with what its
// Present said, and the player told of it at once (onFirstPicture), which is
// what the player's busy overlay goes by.
//
// Each way a picture comes is tried, on a window of its own: drawn by GfxHost
// on the graphics processor into the window's texture, then straight into the
// swap chain's buffer; drawn in memory (present_pixels, present_pixels_part);
// a game's frame in ANGLE's texture (attach_device, acquire_target, present);
// and a picture whose Present is refused (the flag fail-present), which the
// player must be told of all the same, and the window's state say.
//
// Only tools/build-tests.sh win builds this file, with WindowHost.cpp beside it
// and the shell's stand-ins of mg_storage_windows.cpp.
#include "pch.h"

#include "player/GfxHost.h"
#include "player/WindowHost.h"

#include <cstdio>
#include <memory>
#include <vector>

extern "C" {
// mg_gfx_windows.cpp: the shell's own megler_gfx_host.
const megler_gfx_host* mg_gfx_windows_host(void (*line)(const char* text, void* user), void* user, int way);
// mg_storage_windows.cpp: the shell's flags and its log, as the hosts built into the tests have them.
void mg_shell_flag(const char* name, int on);
int mg_shell_said(const char* fragment);
}

namespace {

using megler::app::GfxHost;
using megler::app::WindowHost;

struct Checker {
    void (*line)(const char* text, void* user);
    int* checks;
    int failed = 0;
    const char* what = "";
    void operator()(bool ok, const char* why) {
        ++*checks;
        if (ok) return;
        ++failed;
        char text[400];
        snprintf(text, sizeof(text), "  FAIL the window, %s: %s", what, why);
        line(text, nullptr);
    }
};

/// The player's side of onFirstPicture: how often it was told, and on which thread.
struct Told {
    int times = 0;
    DWORD thread = 0;
};

std::shared_ptr<Told> listen(WindowHost& window) {
    auto told = std::make_shared<Told>();
    window.onFirstPicture([told] {
        told->times++;
        told->thread = GetCurrentThreadId();
    });
    return told;
}

/// What the window's first picture should be: come the way `way` says at `width` x `height`, tried, and on the screen
/// (`shown`) or refused with `result`.
void checkFirst(Checker& check, WindowHost& window, WindowHost::Way way, int width, int height, bool shown, HRESULT result) {
    const WindowHost::FirstPicture f = window.firstPicture();
    char why[300];
    snprintf(why, sizeof(why), "the first picture is not as it came (came %d, way %d, %dx%d, tried %d, Present 0x%08X, shown %d)", int(f.came),
             int(f.way), f.width, f.height, int(f.tried), unsigned(f.result), int(f.shown));
    check(f.came && f.way == way && f.width == width && f.height == height && f.tried && f.result == result && f.shown == shown, why);
}

} // namespace

extern "C" {

/// The window's checks: lines to `line`, the number of checks added to `checks`; how many failed.
int mg_window_windows_run(void (*line)(const char* text, void* user), int* checks) {
    Checker check{line, checks};
    GfxHost& gfx = GfxHost::shared();
    const megler_gfx_host* h = mg_gfx_windows_host(line, nullptr, 0);
    const DWORD self = GetCurrentThreadId();
    char text[600];

    // The views drawn on the graphics processor: GfxHost draws the canvas into the window's texture, which is copied to the
    // swap chain's buffer and presented; the next picture goes straight into the buffer.
    {
        check.what = "the views drawn on the graphics processor";
        line("  a Java app's views drawn on the graphics processor (GfxHost's window), with no panel", nullptr);
        auto window = std::make_unique<WindowHost>(nullptr, 64, 48);
        auto told = listen(*window);
        gfx.setWindow(window->gfxWindow());
        std::vector<uint32_t> memory(size_t(64) * 48, 0);
        void* cv = h->canvas_new(h->user, memory.data(), 64, 48, 64 * 4);
        h->draw_color(h->user, cv, 0xFF336699u, MEGLER_GFX_BLEND_SRC);
        check(window->pictures() == 0 && told->times == 0, "a picture was counted, or the player told, before any came");
        if (!gfx.device()) {
            line("  (no graphics processor here for Direct2D: the window's pictures from it are not tried)", nullptr);
        } else {
            check(h->present(h->user, cv, 0, 0, 64, 48), "GfxHost did not show the canvas in the window");
            check(window->pictures() == 1 && window->presented() == 1, "the picture was not counted as it came and as presented");
            checkFirst(check, *window, WindowHost::Way::gpu, 64, 48, true, S_OK);
            check(told->times == 1 && told->thread == self, "the player was not told once, on the thread the picture came on");
            check(mg_shell_said("window: the app's first picture, 64x48, came ") == 1 &&
                      mg_shell_said("ms after Play: the views drawn on the graphics processor into the window's texture, copied to the swap chain; Present put it "
                                    "on the screen") == 1,
                  "the log does not say how the first picture came");
            // The next: straight into the swap chain's buffer, counted; the first kept as it was, the player not told again.
            h->draw_color(h->user, cv, 0xFF993366u, MEGLER_GFX_BLEND_SRC);
            check(h->present(h->user, cv, 0, 0, 64, 48), "GfxHost did not show the second picture");
            check(window->pictures() == 2 && window->presented() == 2 && told->times == 1, "the second picture was not counted, or the player told again");
            checkFirst(check, *window, WindowHost::Way::gpu, 64, 48, true, S_OK);
            check(mg_shell_said("window: the app's pictures go straight into the swap chain's buffers") >= 1, "the second picture did not go straight in");
            // Asked after the first came, the player is told at once.
            auto late = listen(*window);
            check(late->times == 1 && late->thread == self, "a player that asks after the first picture came was not told at once");
            const std::string state = window->state();
            snprintf(text, sizeof(text), "    %s", state.c_str());
            line(text, nullptr);
            check(state.find("2 pictures came, 2 Presents went, 0 failed") != std::string::npos, "the window's state does not say its pictures");
        }
        h->canvas_free(h->user, cv);
        gfx.setWindow(GfxHost::Window());
    }

    // The views drawn in memory: present_pixels, then a part of them (present_pixels_part).
    {
        check.what = "the views drawn in memory";
        line("  a Java app's views drawn in memory (present_pixels, present_pixels_part), with no panel", nullptr);
        auto window = std::make_unique<WindowHost>(nullptr, 32, 24);
        auto told = listen(*window);
        const megler_window_host cb = window->callbacks();
        std::vector<uint32_t> pixels(size_t(32) * 24, 0xFF00C000u);
        cb.present_pixels(cb.user, pixels.data(), 32, 24, 32 * 4);
        check(window->pictures() == 1 && window->presented() == 1, "the picture was not counted as it came and as presented");
        checkFirst(check, *window, WindowHost::Way::memory, 32, 24, true, S_OK);
        check(told->times == 1 && told->thread == self, "the player was not told once, on the thread the picture came on");
        cb.present_pixels_part(cb.user, pixels.data(), 32, 24, 32 * 4, 4, 4, 8, 8);
        check(window->pictures() == 2 && window->presented() == 2 && told->times == 1, "the part was not counted, or the player told again");
    }

    // A game's frame: a device of the test's own stands for ANGLE's, its texture the window's target.
    {
        check.what = "a game's frame";
        line("  a game's frame in ANGLE's texture (attach_device, acquire_target, present), with no panel", nullptr);
        winrt::com_ptr<ID3D11Device> angle;
        winrt::com_ptr<ID3D11DeviceContext> context;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, angle.put(),
                                     nullptr, context.put())) &&
            FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, angle.put(),
                                     nullptr, context.put()))) {
            check(false, "no Direct3D device to stand for ANGLE's");
        } else {
            auto window = std::make_unique<WindowHost>(nullptr, 40, 30);
            auto told = listen(*window);
            const megler_window_host cb = window->callbacks();
            check(cb.attach_device(cb.user, angle.get()), "the window made no swap chain on ANGLE's device");
            auto* target = static_cast<ID3D11Texture2D*>(cb.acquire_target(cb.user, 40, 30));
            check(target != nullptr, "the window gave no target texture");
            if (target) {
                winrt::com_ptr<ID3D11RenderTargetView> view;
                if (SUCCEEDED(angle->CreateRenderTargetView(target, nullptr, view.put()))) {
                    const float red[4] = {1, 0, 0, 1};
                    context->ClearRenderTargetView(view.get(), red);
                    context->Flush();
                }
                cb.present(cb.user);
                check(window->pictures() == 1 && window->presented() == 1, "the frame was not counted as it came and as presented");
                checkFirst(check, *window, WindowHost::Way::game, 40, 30, true, S_OK);
                check(told->times == 1 && told->thread == self, "the player was not told once, on the thread the frame came on");
                cb.release_target(cb.user);
            }
        }
    }

    // Present refused (the flag fail-present): the picture is counted as it came and the player told all the same, which
    // takes its busy overlay away; the log and the window's state say Present failed.
    {
        check.what = "a picture whose Present is refused";
        line("  a picture whose Present is refused (the flag fail-present), with no panel", nullptr);
        mg_shell_flag("fail-present", 1);
        auto window = std::make_unique<WindowHost>(nullptr, 32, 24);
        auto told = listen(*window);
        const megler_window_host cb = window->callbacks();
        std::vector<uint32_t> pixels(size_t(32) * 24, 0xFFC00000u);
        cb.present_pixels(cb.user, pixels.data(), 32, 24, 32 * 4);
        check(window->pictures() == 1 && window->presented() == 0, "the picture was not counted as it came, or counted as presented");
        checkFirst(check, *window, WindowHost::Way::memory, 32, 24, false, DXGI_ERROR_INVALID_CALL);
        check(told->times == 1, "the player was not told of a picture whose Present failed");
        check(mg_shell_said("ms after Play: the views drawn in memory (present_pixels); Present failed, DXGI_ERROR_INVALID_CALL (0x887A0001): it is not on "
                            "the screen") == 1,
              "the log does not say the first picture's Present failed");
        cb.present_pixels(cb.user, pixels.data(), 32, 24, 32 * 4);
        const std::string state = window->state();
        snprintf(text, sizeof(text), "    %s", state.c_str());
        line(text, nullptr);
        check(state.find("2 pictures came, 0 Presents went, 2 failed (the last 0x887A0001)") != std::string::npos &&
                  state.find("and none has been on the screen") != std::string::npos,
              "the window's state does not say its Presents failed");
        mg_shell_flag("fail-present", 0);
    }
    return check.failed;
}

} // extern "C"
