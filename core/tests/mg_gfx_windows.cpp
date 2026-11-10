// The test program's way to the shell's drawing: megler_gfx_host as
// app/Megler/player/GfxHost.cpp implements it over Direct2D, DirectWrite
// and WIC, for mg_tests --gfx-windows (mg_tests_main.c), which hands it to
// the scenes of mg_gfx_scenes.c.
//
// Only tools/build-tests.sh win builds this file, with GfxHost.cpp itself
// beside it: the build for qemu-arm has no Direct2D to link, and the phone's
// package gets its GfxHost from the shell.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>

#include "player/GfxHost.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

/// A window for GfxHost to show pictures in (GfxHost::setWindow), as the
/// shell's WindowHost is: a texture on GfxHost's device that holds the
/// picture bottom row first, and a count of the pictures shown.
struct TestWindow {
    ID3D11Texture2D* texture = nullptr;
    int width = 0, height = 0;
    int shown = 0;
} g_window;

ID3D11Texture2D* windowTarget(void*, int width, int height) {
    if (g_window.texture && (g_window.width != width || g_window.height != height)) {
        g_window.texture->Release();
        g_window.texture = nullptr;
    }
    if (!g_window.texture) {
        ID3D11Device* device = megler::app::GfxHost::shared().device();
        if (!device) return nullptr;
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = UINT(width);
        td.Height = UINT(height);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &g_window.texture))) return nullptr;
        g_window.width = width;
        g_window.height = height;
    }
    return g_window.texture;
}

void windowShow(void*) { g_window.shown++; }

/// What the window shows, top row first, read through a staging copy under
/// Direct2D's lock on the device, as WindowHost uses the device's context.
bool readWindow(std::vector<uint32_t>& out) {
    megler::app::GfxHost& gfx = megler::app::GfxHost::shared();
    ID3D11Device* device = gfx.device();
    if (!device || !g_window.texture) return false;
    D3D11_TEXTURE2D_DESC td = {};
    g_window.texture->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &staging))) return false;
    ID3D11DeviceContext* context = nullptr;
    device->GetImmediateContext(&context);
    bool ok = false;
    gfx.lockDevice();
    context->CopyResource(staging, g_window.texture);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        out.resize(size_t(td.Width) * td.Height);
        for (UINT y = 0; y < td.Height; y++)
            memcpy(out.data() + size_t(td.Height - 1 - y) * td.Width, static_cast<const uint8_t*>(mapped.pData) + size_t(y) * mapped.RowPitch, size_t(td.Width) * 4);
        context->Unmap(staging, 0);
        ok = true;
    }
    gfx.unlockDevice();
    context->Release();
    staging->Release();
    return ok;
}

} // namespace

extern "C" {

/// The host, its lines going to `line`, drawing as `way` says: 0 on this
/// machine's graphics processor (where it has none, in memory, said in its
/// log), 1 in memory with every canvas copied to a WIC bitmap and back
/// (GfxHost::setCopying, the way a Direct2D that refused the app's memory
/// would be left with), 2 in memory, 3 on WARP, Direct3D's software device,
/// which draws as a graphics processor does. The environment's
/// MEGLER_GFX_FONTS names a folder of Android's own fonts
/// (GfxHost::setFontFolder; build/android9/root/fonts), for a run with those.
const megler_gfx_host* mg_gfx_windows_host(void (*line)(const char* text, void* user), void* user, int way) {
    static megler_gfx_host host;
    megler::app::GfxHost& gfx = megler::app::GfxHost::shared();
    gfx.setLog(line, user);
    gfx.setCopying(way == 1);
    gfx.setDrawing(way == 3   ? megler::app::GfxHost::Drawing::warp
                   : way == 0 ? megler::app::GfxHost::Drawing::graphicsProcessor
                              : megler::app::GfxHost::Drawing::memory);
    if (const char* fonts = std::getenv("MEGLER_GFX_FONTS")) {
        std::wstring folder;
        for (const char* at = fonts; *at; at++) folder += wchar_t(static_cast<unsigned char>(*at));
        gfx.setFontFolder(folder.c_str());
    }
    host = gfx.callbacks();
    return &host;
}

/// What the host did, in a line.
void mg_gfx_windows_counts(char* out, size_t size) {
    const megler::app::GfxHost::Counts c = megler::app::GfxHost::shared().counts();
    typedef unsigned long long ull;
    int n = snprintf(out, size,
                     "%llu canvases, %llu times Direct2D began and ended drawing, %llu drawing calls put on by Direct2D and %llu by the host's own arithmetic, "
                     "%llu layers; %llu runs of text laid out and %llu found laid out; %llu kinds of thing said to be drawn not as asked; %llu times a target "
                     "moved to a part",
                     ull(c.canvases), ull(c.sessions), ull(c.direct), ull(c.composed), ull(c.layers), ull(c.runsShaped), ull(c.runsReused), ull(c.notes),
                     ull(c.partTargets));
    if (c.gpu && n > 0 && size_t(n) < size)
        snprintf(out + n, size - size_t(n),
                 "; on the graphics processor (%s%s): %llu uploads, %llu read-backs, %llu calls by Direct2D's effects and %llu through memory, %llu bitmaps "
                 "kept and drawn again %llu times",
                 c.device, c.gpuLost ? ", lost since" : "", ull(c.uploads), ull(c.readBacks), ull(c.byEffects), ull(c.throughMemory), ull(c.keptMade),
                 ull(c.keptReused));
}

/// Whether the host draws on a graphics processor (or WARP) now.
int mg_gfx_windows_on_gpu(void) {
    const megler::app::GfxHost::Counts c = megler::app::GfxHost::shared().counts();
    return c.gpu && !c.gpuLost;
}

/// The graphics processor's device taken as lost, for the test of what a canvas does then.
void mg_gfx_windows_lose_device(void) { megler::app::GfxHost::shared().loseDevice(); }

/// The window composed of an OpenGL ES view's frames and the views
/// (megler_gfx_host.gl_frame, gl_place): a second Direct3D device stands for
/// ANGLE's, a texture of it shared by handle for a GL surface, its rows
/// bottom first as OpenGL's are. The frame shows where the views leave a
/// hole, the right way up, under what the views have there; a new frame is
/// shown without the views being shown again; a layer taken away leaves the
/// views alone; a layer over the views is over them, its premultiplied alpha
/// kept where it is translucent; nothing is read back. Twice: the texture shared plainly, and
/// shared with a keyed mutex, as gecko-w10m's ANGLE shares every pbuffer --
/// "ANGLE's" device then draws holding it and lets go of it for gl_frame, as
/// the core does, and the shell copies holding it (without, Direct3D drops
/// the copy and the frame is nothing). Lines to `line`; checks that failed.
int mg_gfx_windows_check_gl(const megler_gfx_host* h, void (*line)(const char* text, void* user), int* checks) {
    megler::app::GfxHost& gfx = megler::app::GfxHost::shared();
    int failed = 0;
    char text[300];
    bool keyedRun = false;
    auto check = [&](bool ok, const char* what) {
        ++*checks;
        if (ok) return;
        ++failed;
        snprintf(text, sizeof(text), "  FAIL the window composed of OpenGL ES frames and the views%s: %s", keyedRun ? " (a keyed mutex)" : "", what);
        line(text, nullptr);
    };
    line("  the window composed on the graphics processor of an OpenGL ES view's frames and the views (megler_gfx_host.gl_frame, gl_place)", nullptr);
    // ANGLE's device, on the adapter GfxHost's is on (a texture is shared on one adapter only, and ANGLE and the shell use the
    // phone's one), and the surface's texture: 32 x 24, its bottom half (rows 0 to 11, the picture's bottom) blue, its top yellow.
    ID3D11Device* angle = nullptr;
    ID3D11DeviceContext* angleContext = nullptr;
    IDXGIDevice* drawing = nullptr;
    IDXGIAdapter* adapter = nullptr;
    if (gfx.device()) gfx.device()->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&drawing));
    if (drawing) drawing->GetAdapter(&adapter);
    const HRESULT made = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                           nullptr, 0, D3D11_SDK_VERSION, &angle, nullptr, &angleContext);
    if (adapter) adapter->Release();
    if (drawing) drawing->Release();
    if (FAILED(made)) {
        check(false, "no second Direct3D device");
        return failed;
    }
    const int fw = 32, fh = 24;
    for (int keyed = 0; keyed < 2; keyed++) {
        keyedRun = keyed != 0;
        std::vector<uint32_t> frame(size_t(fw) * fh);
        for (int y = 0; y < fh; y++)
            for (int x = 0; x < fw; x++) frame[size_t(y) * fw + x] = y < fh / 2 ? 0xFF0000FFu : 0xFFFFFF00u;
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = UINT(fw);
        td.Height = UINT(fh);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = keyed ? D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX : D3D11_RESOURCE_MISC_SHARED;
        ID3D11Texture2D* surface = nullptr;
        IDXGIResource* resource = nullptr;
        IDXGIKeyedMutex* mutex = nullptr;
        HANDLE handle = nullptr;
        if (FAILED(angle->CreateTexture2D(&td, nullptr, &surface)) || FAILED(surface->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&resource))) ||
            FAILED(resource->GetSharedHandle(&handle)) || (keyed && FAILED(surface->QueryInterface(__uuidof(IDXGIKeyedMutex), reinterpret_cast<void**>(&mutex))))) {
            check(false, "no shared texture on the second device");
            if (mutex) mutex->Release();
            if (resource) resource->Release();
            if (surface) surface->Release();
            continue;
        }
        // "ANGLE" draws holding the mutex, as the core has ANGLE's device hold it from the pbuffer's making; and lets go of it
        // for the shell, as the core does around gl_frame. (What glFinish does on ANGLE's side: the frame is in the texture.)
        const auto draw = [&]() {
            if (mutex) mutex->AcquireSync(0, INFINITE);
            angleContext->UpdateSubresource(surface, 0, nullptr, frame.data(), UINT(fw * 4), 0);
            D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0};
            ID3D11Query* done = nullptr;
            if (SUCCEEDED(angle->CreateQuery(&qd, &done))) {
                angleContext->End(done);
                angleContext->Flush();
                while (angleContext->GetData(done, nullptr, 0, 0) == S_FALSE) SwitchToThread();
                done->Release();
            }
            if (mutex) mutex->ReleaseSync(0);
        };
        draw();
        megler::app::GfxHost::Window window;
        window.target = windowTarget;
        window.show = windowShow;
        gfx.setWindow(window);
        // The views: white, a hole where the GL view is (16, 8, 32 x 24), and over the hole's lower right a half transparent red.
        const int w = 64, hgt = 48;
        std::vector<uint32_t> memory(size_t(w) * hgt, 0), shown;
        void* cv = h->canvas_new(h->user, memory.data(), w, hgt, w * 4);
        megler_gfx_paint p = {};
        p.miter_limit = 4;
        p.text_size = 12;
        p.text_scale_x = 1;
        p.blend = MEGLER_GFX_BLEND_SRC_OVER;
        p.filter_mode = -1;
        h->draw_color(h->user, cv, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
        p.blend = MEGLER_GFX_BLEND_CLEAR;
        h->draw_rect(h->user, cv, 16, 8, 48, 32, &p, nullptr);
        p.blend = MEGLER_GFX_BLEND_SRC_OVER;
        p.color = 0x80FF0000u;
        h->draw_rect(h->user, cv, 40, 20, 48, 32, &p, nullptr);
        const megler::app::GfxHost::Counts before = gfx.counts();
        // The frame comes before the views have been shown with the layer placed: kept, not shown.
        const int shownBefore = g_window.shown;
        check(h->gl_frame(h->user, 0, handle, fw, fh), "the first frame was not taken");
        check(g_window.shown == shownBefore, "a frame was shown before the views were");
        h->gl_place(h->user, 0, 16, 8, 32, 24, 0);
        check(h->present(h->user, cv, 0, 0, w, hgt) && readWindow(shown), "the views were not shown");
        // (Within two steps a channel: the mix is the processor's arithmetic.)
        const auto alike = [](uint32_t a, uint32_t b) {
            for (int s = 0; s < 32; s += 8)
                if (abs(int((a >> s) & 255) - int((b >> s) & 255)) > 2) return false;
            return true;
        };
        check(shown.size() == memory.size() && (shown[2 * w + 2] | 0xFF000000u) == 0xFFFFFFFFu, "the views are not white beside the GL view");
        check(shown.size() == memory.size() && (shown[10 * w + 20] & 0x00FFFFFFu) == 0x00FFFF00u, "the frame's top (yellow) is not at the GL view's top");
        check(shown.size() == memory.size() && (shown[30 * w + 20] & 0x00FFFFFFu) == 0x000000FFu, "the frame's bottom (blue) is not at the GL view's bottom");
        check(shown.size() == memory.size() && alike(shown[30 * w + 44] | 0xFF000000u, 0xFF80007Fu), "half transparent red over the frame's blue is not their mix");
        // A new frame: green all over. Shown without the views being shown again.
        for (uint32_t& px : frame) px = 0xFF00FF00u;
        draw();
        const int shownAgain = g_window.shown;
        check(h->gl_frame(h->user, 0, handle, fw, fh) && g_window.shown == shownAgain + 1 && readWindow(shown), "the new frame was not shown");
        check(shown.size() == memory.size() && (shown[10 * w + 20] & 0x00FFFFFFu) == 0x0000FF00u && (shown[2 * w + 2] | 0xFF000000u) == 0xFFFFFFFFu,
              "the new frame (green) is not under the views' hole, or the views changed");
        if (mutex) {
            // Handed over, the mutex is there to be taken back at once: the shell gave it back after its copy.
            check(mutex->AcquireSync(0, 0) == S_OK, "the shell kept the surface's keyed mutex");
            mutex->ReleaseSync(0);
        }
        // The layer taken away: the views alone there again, at the next showing.
        h->gl_place(h->user, 0, 0, 0, 0, 0, 0);
        check(h->present(h->user, cv, 16, 8, 32, 24) && readWindow(shown) && shown[10 * w + 20] == 0u, "the views' hole does not show nothing once the layer is gone");
        // The layer over the views (MEGLER_GL_ABOVE: a SurfaceView on top of the window, Flutter's view over HERE WeGo's map),
        // the views white all over now, no hole, the half transparent red still: the frame's left half clear, its right half
        // blue at half alpha, premultiplied. Translucent (MEGLER_GL_TRANSLUCENT), the views are seen through its left half and
        // under its right half, the red too; opaque, the frame covers them, whatever its alpha.
        h->draw_color(h->user, cv, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
        p.blend = MEGLER_GFX_BLEND_SRC_OVER;
        p.color = 0x80FF0000u;
        h->draw_rect(h->user, cv, 40, 20, 48, 32, &p, nullptr);
        for (int y = 0; y < fh; y++)
            for (int x = 0; x < fw; x++) frame[size_t(y) * fw + x] = x < fw / 2 ? 0u : 0x80000080u;
        draw();
        h->gl_place(h->user, 0, 16, 8, 32, 24, MEGLER_GL_ABOVE | MEGLER_GL_TRANSLUCENT);
        check(h->gl_frame(h->user, 0, handle, fw, fh) && h->present(h->user, cv, 0, 0, w, hgt) && readWindow(shown), "a frame over the views was not shown");
        check(shown.size() == memory.size() && (shown[10 * w + 20] | 0xFF000000u) == 0xFFFFFFFFu, "the views are not seen where a translucent frame over them is clear");
        check(shown.size() == memory.size() && alike(shown[10 * w + 40] | 0xFF000000u, 0xFF7F7FFFu), "a translucent frame's half blue over the white views is not their mix");
        check(shown.size() == memory.size() && alike(shown[30 * w + 44] | 0xFF000000u, 0xFF7F3FBFu), "a translucent frame is not over the views' half transparent red");
        check(shown.size() == memory.size() && (shown[2 * w + 2] | 0xFF000000u) == 0xFFFFFFFFu, "the views beside a frame over them changed");
        h->gl_place(h->user, 0, 16, 8, 32, 24, MEGLER_GL_ABOVE);
        check(h->present(h->user, cv, 0, 0, w, hgt) && readWindow(shown), "an opaque frame over the views was not shown");
        check(shown.size() == memory.size() && (shown[10 * w + 20] & 0x00FFFFFFu) == 0u && (shown[10 * w + 40] & 0x00FFFFFFu) == 0x80u,
              "an opaque frame over the views is not over them whatever its alpha");
        h->gl_place(h->user, 0, 0, 0, 0, 0, 0);
        check(h->present(h->user, cv, 0, 0, w, hgt) && readWindow(shown) && (shown[10 * w + 20] | 0xFF000000u) == 0xFFFFFFFFu, "the views are not alone again once the layer over them is gone");
        const megler::app::GfxHost::Counts after = gfx.counts();
        check(after.readBacks == before.readBacks, "something was read back");
        snprintf(text, sizeof(text), "    %s: %d pictures shown, %llu read-backs", keyed ? "shared with a keyed mutex" : "shared plainly", g_window.shown,
                 static_cast<unsigned long long>(after.readBacks - before.readBacks));
        line(text, nullptr);
        h->canvas_free(h->user, cv);
        gfx.setWindow(megler::app::GfxHost::Window());
        if (g_window.texture) g_window.texture->Release();
        g_window = TestWindow();
        if (mutex) mutex->Release();
        resource->Release();
        surface->Release();
    }
    angleContext->Release();
    angle->Release();
    return failed;
}

/// The window's picture shown from its canvas without the memory
/// (megler_gfx_host.present), into a window of the test's own: shown with no
/// read-back and the memory left alone; of a part, that part alone; and,
/// after a flush, the memory exactly the picture shown, as the core's later
/// reader gets it. Lines to `line`; how many checks failed.
int mg_gfx_windows_check_canvas(const megler_gfx_host* h, void (*line)(const char* text, void* user), int* checks) {
    int failed = 0;
    auto check = [&](bool ok, const char* what) {
        ++*checks;
        if (!ok) { ++failed; line(what, nullptr); }
    };
    line("  bitmap canvases composed without CPU readbacks, compared with bitmap drawing", nullptr);
    auto& gfx = megler::app::GfxHost::shared();
    std::vector<uint32_t> source(16 * 16, 0), actual(32 * 32, 0), expected(32 * 32, 0);
    void* sc = h->canvas_new(h->user, source.data(), 16, 16, 64);
    void* ac = h->canvas_new(h->user, actual.data(), 32, 32, 128);
    void* ec = h->canvas_new(h->user, expected.data(), 32, 32, 128);
    check(sc && ac && ec && h->draw_canvas && h->canvas_discard, "  FAIL canvas composition callbacks or canvases missing");
    if (!sc || !ac || !ec) return failed;
    megler_gfx_paint paint = {};
    paint.color = 0xFFFFFFFFu; paint.miter_limit = 4; paint.text_size = 12; paint.text_scale_x = 1;
    paint.blend = MEGLER_GFX_BLEND_SRC_OVER; paint.filter_mode = -1;
    const float src[] = {0, 0, 16, 16}, dst[] = {4, 4, 28, 28};
    unsigned version = 0;
    for (int filter = -1; filter < 18; ++filter) {
        for (int blend = 0; blend < 18; ++blend) {
            h->draw_color(h->user, sc, 0xA04080C0u, MEGLER_GFX_BLEND_SRC);
            paint.color = 0xC0F02040u; paint.blend = MEGLER_GFX_BLEND_SRC_OVER; paint.filter_mode = -1;
            h->draw_rect(h->user, sc, 2, 3, 10, 14, &paint, nullptr);
            const auto before = source;
            h->draw_color(h->user, ac, 0xE030A050u, MEGLER_GFX_BLEND_SRC);
            h->draw_color(h->user, ec, 0xE030A050u, MEGLER_GFX_BLEND_SRC);
            h->save(h->user, ac); h->save(h->user, ec);
            h->clip_rect(h->user, ac, 6, 5, 25, 27, MEGLER_GFX_CLIP_INTERSECT);
            h->clip_rect(h->user, ec, 6, 5, 25, 27, MEGLER_GFX_CLIP_INTERSECT);
            paint.color = 0xA0FFFFFFu; paint.blend = blend; paint.filter_mode = filter; paint.filter_color = 0x806030E0u;
            const auto counts = gfx.counts();
            check(h->draw_canvas(h->user, ac, sc, src, dst, &paint), "  FAIL a supported canvas composition was rejected");
            check(gfx.counts().readBacks == counts.readBacks && source == before, "  FAIL canvas composition read its source back");
            h->restore(h->user, ac); h->restore(h->user, ec);
            h->flush(h->user, sc);
            megler_gfx_image image = {source.data(), 16, 16, 64, 0x7F000001u, ++version};
            // Apply the same clip to the bitmap reference.
            h->save(h->user, ec);
            h->clip_rect(h->user, ec, 6, 5, 25, 27, MEGLER_GFX_CLIP_INTERSECT);
            h->draw_bitmap(h->user, ec, &image, src, dst, &paint);
            h->restore(h->user, ec);
            h->flush(h->user, ac); h->flush(h->user, ec);
            bool same = true;
            for (size_t i = 0; i < actual.size(); ++i)
                for (int shift = 0; shift < 32; shift += 8)
                    if (std::abs(int((actual[i] >> shift) & 255) - int((expected[i] >> shift) & 255)) > 3) same = false;
            if (!same) {
                char text[120]; snprintf(text, sizeof(text), "  FAIL canvas pixels differ: filter %d, blend %d", filter, blend);
                check(false, text);
            } else check(true, "");
        }
    }
    const float part[] = {1, 1, 15, 15};
    const auto before = gfx.counts();
    check(!h->draw_canvas(h->user, ac, sc, part, dst, &paint), "  FAIL a subrectangle did not request the bitmap fallback");
    check(!h->draw_canvas(h->user, sc, sc, src, dst, &paint), "  FAIL self-copy did not request the bitmap fallback");
    h->save_layer(h->user, sc, src, 255, MEGLER_GFX_BLEND_SRC_OVER);
    check(!h->draw_canvas(h->user, ac, sc, src, dst, &paint), "  FAIL an open source layer did not request fallback");
    h->restore(h->user, sc);
    const auto memory = source;
    h->canvas_discard(h->user, sc);
    check(source == memory && gfx.counts().readBacks == before.readBacks, "  FAIL discarding a temporary read it back");
    h->canvas_free(h->user, ac); h->canvas_free(h->user, ec);
    h->bitmap_gone(h->user, 0x7F000001u);
    // A changing upload must preserve earlier unflushed drawings, other
    // canvases and a shader made from the original immutable image.
    {
        std::vector<uint32_t> pixels(256 * 256, 0xFF102030u), a(32 * 32), b(32 * 32);
        void* ca = h->canvas_new(h->user, a.data(), 32, 32, 128);
        void* cb = h->canvas_new(h->user, b.data(), 32, 32, 128);
        megler_gfx_image image = {pixels.data(), 256, 256, 1024, 0x7F000002u, 1};
        const auto counts = gfx.counts();
        void* shader = h->shader_bitmap(h->user, &image, 0);
        check(ca && cb && shader, "  FAIL changing image fixture could not be created");
        paint.color = 0xFFFFFFFFu; paint.blend = MEGLER_GFX_BLEND_SRC_OVER; paint.filter_mode = -1;
        const float whole[] = {0, 0, 256, 256};
        for (unsigned i = 0; i < 12; ++i) {
            std::fill(pixels.begin(), pixels.end(), 0xFF102030u + i * 0x00080704u);
            image.version = i + 1;
            const float stripe[] = {float(i * 2), 0, float(i * 2 + 2), 16};
            h->draw_bitmap(h->user, ca, &image, whole, stripe, &paint);
            h->draw_bitmap(h->user, cb, &image, whole, stripe, &paint);
        }
        h->draw_rect(h->user, ca, 0, 20, 32, 32, &paint, shader);
        h->flush(h->user, ca); h->flush(h->user, cb);
        for (unsigned i = 0; i < 12; ++i)
            check(a[8 * 32 + i * 2 + 1] == 0xFF102030u + i * 0x00080704u && b[8 * 32 + i * 2 + 1] == a[8 * 32 + i * 2 + 1],
                "  FAIL changing image overwrote an earlier frame or another canvas");
        check(a[24 * 32 + 8] == 0xFF102030u, "  FAIL changing upload mutated an immutable shader snapshot");
        check(gfx.counts().keptMade == counts.keptMade + 1, "  FAIL changing upload allocated a global bitmap for every version");
        check(gfx.counts().uploads >= counts.uploads + 22, "  FAIL changing image did not reuse private upload textures");
        h->shader_free(h->user, shader); h->canvas_free(h->user, ca); h->canvas_free(h->user, cb);
        h->bitmap_gone(h->user, image.id);
    }
    return failed;
}

int mg_gfx_windows_check_present(const megler_gfx_host* h, void (*line)(const char* text, void* user), int* checks) {
    megler::app::GfxHost& gfx = megler::app::GfxHost::shared();
    int failed = 0;
    char text[300];
    auto check = [&](bool ok, const char* what) {
        ++*checks;
        if (ok) return;
        ++failed;
        snprintf(text, sizeof(text), "  FAIL the window shown from its canvas: %s", what);
        line(text, nullptr);
    };
    line("  the window's picture shown from its canvas on the graphics processor, without the memory (megler_gfx_host.present)", nullptr);
    megler::app::GfxHost::Window window;
    window.target = windowTarget;
    window.show = windowShow;
    gfx.setWindow(window);
    const int w = 64, hgt = 48;
    std::vector<uint32_t> memory(size_t(w) * hgt, 0), shown, before;
    void* cv = h->canvas_new(h->user, memory.data(), w, hgt, w * 4);
    check(cv != nullptr, "no canvas");
    if (!cv) return failed;
    megler_gfx_paint p = {};
    p.miter_limit = 4;
    p.text_size = 12;
    p.text_scale_x = 1;
    p.blend = MEGLER_GFX_BLEND_SRC_OVER;
    p.filter_mode = -1;
    p.flags = MEGLER_GFX_ANTI_ALIAS;
    h->draw_color(h->user, cv, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
    p.color = 0xFFFF0000u;
    h->draw_rect(h->user, cv, 8, 8, 24, 24, &p, nullptr);
    p.color = 0x802040C0u;
    h->draw_oval(h->user, cv, 20, 20, 50, 44, &p, nullptr);
    const megler::app::GfxHost::Counts first = gfx.counts();
    const bool whole = h->present(h->user, cv, 0, 0, w, hgt);
    const megler::app::GfxHost::Counts second = gfx.counts();
    check(whole && g_window.shown == 1, "the whole picture was not shown");
    check(second.readBacks == first.readBacks, "showing it read the picture back");
    check(memory[10 * w + 10] == 0, "showing it wrote the memory");
    check(readWindow(shown) && shown[10 * w + 10] == 0xFFFF0000u && shown[2 * w + 60] == 0xFFFFFFFFu && shown[32 * w + 35] != 0xFFFFFFFFu,
          "the window does not show the picture (red at 10,10, white at 60,2, the oval at 35,32)");
    // A part: what changed inside it is shown, what changed outside it is not (the core says what changed; it said this).
    before = shown;
    p.color = 0xFF00A000u;
    h->draw_rect(h->user, cv, 40, 4, 56, 12, &p, nullptr);
    p.color = 0xFF000000u;
    h->draw_rect(h->user, cv, 0, 40, 8, 48, &p, nullptr);
    check(h->present(h->user, cv, 40, 4, 16, 8) && g_window.shown == 2, "a part was not shown");
    bool partRight = readWindow(shown) && shown[6 * w + 45] == 0xFF00A000u && shown[44 * w + 2] == before[44 * w + 2];
    for (int y = 0; y < hgt && partRight; y++)
        for (int x = 0; x < w; x++)
            if ((x < 40 || x >= 56 || y < 4 || y >= 12) && shown[size_t(y) * w + x] != before[size_t(y) * w + x]) {
                partRight = false;
                break;
            }
    check(partRight, "a part shown changed the window outside the part, or not inside it");
    // The whole again, and then the memory brought up to date as the core does before it reads it: exactly the picture shown.
    check(h->present(h->user, cv, 0, 0, w, hgt) && readWindow(shown), "the whole picture was not shown again");
    const megler::app::GfxHost::Counts third = gfx.counts();
    h->flush(h->user, cv);
    const megler::app::GfxHost::Counts fourth = gfx.counts();
    check(fourth.readBacks == third.readBacks + 1 && fourth.windowReadBacks == third.windowReadBacks + 1, "the flush did not read the window's canvas back, once");
    check(shown.size() == memory.size() && !memcmp(shown.data(), memory.data(), memory.size() * 4), "after a flush the memory is not the picture shown");
    // The memory written behind the canvas's back (touched), and shown with nothing drawn since: the memory's picture.
    for (int y = 30; y < 40; y++)
        for (int x = 4; x < 60; x++) memory[size_t(y) * w + x] = 0xFF00FFFFu;
    h->touched(h->user, cv);
    check(h->present(h->user, cv, 0, 0, w, hgt) && readWindow(shown) && !memcmp(shown.data(), memory.data(), memory.size() * 4),
          "the memory written behind the canvas was not what was shown next");
    h->canvas_free(h->user, cv);
    snprintf(text, sizeof(text), "    %d pictures shown, %llu read-backs of the window's canvas (the one flush)", g_window.shown,
             static_cast<unsigned long long>(gfx.counts().windowReadBacks));
    line(text, nullptr);
    // What it costs here: a frame of a 720x1280 window that redraws a strip of 720x64, shown so, against the same frame
    // flushed (read back) as before. The processor is waited for once, at the end of each hundred, by reading the window.
    {
        const int ww = 720, wh = 1280;
        std::vector<uint32_t> screen(size_t(ww) * wh, 0);
        void* sc = h->canvas_new(h->user, screen.data(), ww, wh, ww * 4);
        LARGE_INTEGER frequency, t0, t1;
        QueryPerformanceFrequency(&frequency);
        double ways[2] = {0, 0};
        if (sc) {
            h->draw_color(h->user, sc, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
            h->present(h->user, sc, 0, 0, ww, wh);
            for (int way = 0; way < 2; way++) {
                QueryPerformanceCounter(&t0);
                for (int i = 0; i < 100; i++) {
                    h->save(h->user, sc);
                    h->clip_rect(h->user, sc, 0, 600, float(ww), 664, MEGLER_GFX_CLIP_INTERSECT);
                    h->draw_color(h->user, sc, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
                    p.color = 0xFFE0E0E0u;
                    for (int k = 0; k < 7; k++) h->draw_rect(h->user, sc, float(k * 100 + 10), 610, float(k * 100 + 90), 612, &p, nullptr);
                    p.color = 0xFF2060C0u;
                    if (i & 1) h->draw_rect(h->user, sc, 300, 614, 302, 650, &p, nullptr);
                    h->restore(h->user, sc);
                    if (way == 0) h->present(h->user, sc, 0, 600, ww, 64);
                    else h->flush(h->user, sc);
                }
                readWindow(shown);
                QueryPerformanceCounter(&t1);
                ways[way] = double(t1.QuadPart - t0.QuadPart) * 1e6 / double(frequency.QuadPart) / 100.0;
            }
            h->canvas_free(h->user, sc);
            snprintf(text, sizeof(text),
                     "    a frame that redraws a strip of 720x64 of a 720x1280 window and shows it so %.0f us; flushed and read back instead %.0f us", ways[0],
                     ways[1]);
            line(text, nullptr);
        }
    }
    gfx.setWindow(megler::app::GfxHost::Window());
    if (g_window.texture) g_window.texture->Release();
    g_window = TestWindow();
    return failed;
}

/// A TextureView's video under the views (GfxHost::showVideoFrame, draw_texture on the window's own canvas), as a decoder on
/// the graphics processor shows its pictures: the window has the video's picture where the view drew it, scaled up, the
/// views over it there and their own picture elsewhere; a new picture is shown composed with the views as they were last
/// shown, nothing of them drawn (showVideoFrame says 2); drawn at half its alpha it is among the views again, and no longer
/// under them (1). The pictures are textures of the device as a decoder's are, top row first. Lines to `line`; checks
/// that failed.
int mg_gfx_windows_check_video(const megler_gfx_host* h, void (*line)(const char* text, void* user), int* checks) {
    megler::app::GfxHost& gfx = megler::app::GfxHost::shared();
    int failed = 0;
    char text[300];
    auto check = [&](bool ok, const char* what) {
        ++*checks;
        if (ok) return;
        ++failed;
        snprintf(text, sizeof(text), "  FAIL a video under the views: %s", what);
        line(text, nullptr);
    };
    line("  a TextureView's video under the views: composed with them, a new picture shown with nothing of the views drawn", nullptr);
    ID3D11Device* device = gfx.device();
    check(device != nullptr, "no device");
    if (!device) return failed;
    megler::app::GfxHost::Window window;
    window.target = windowTarget;
    window.show = windowShow;
    gfx.setWindow(window);
    // A picture of 16 x 16, one colour above and another below (to see it the right way up), as a decoder's is made.
    auto picture = [&](uint32_t top, uint32_t bottom) {
        std::vector<uint32_t> px(16 * 16);
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) px[size_t(y) * 16 + x] = y < 8 ? top : bottom;
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = td.Height = 16;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET;
        D3D11_SUBRESOURCE_DATA data = {px.data(), 16 * 4, 0};
        ID3D11Texture2D* t = nullptr;
        if (FAILED(device->CreateTexture2D(&td, &data, &t))) t = nullptr;
        return t;
    };
    ID3D11Texture2D* green = picture(0xFF00C000u, 0xFF00C0C0u);
    ID3D11Texture2D* blue = picture(0xFF0000C0u, 0xFFC0C000u);
    const int w = 64, hgt = 48, id = 9;
    std::vector<uint32_t> memory(size_t(w) * hgt, 0), shown;
    void* cv = h->canvas_new(h->user, memory.data(), w, hgt, w * 4);
    check(cv && green && blue, "no canvas or no pictures");
    if (cv && green && blue) {
        megler_gfx_paint p = {};
        p.miter_limit = 4;
        p.text_size = 12;
        p.text_scale_x = 1;
        p.blend = MEGLER_GFX_BLEND_SRC_OVER;
        p.filter_mode = -1;
        p.color = 0xFFFF0000u;
        // The window shown once from the canvas, which makes it the window's; then the first picture, which no view has drawn
        // yet: kept for its view.
        h->draw_color(h->user, cv, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
        h->present(h->user, cv, 0, 0, w, hgt);
        check(gfx.showVideoFrame(id, green, 16, 16) == 1, "a picture of a video that is under no views was not kept for its view (1)");
        // The view draws it into 16,8 - 48,40 (twice its size), and the views draw a red square over part of it.
        h->draw_color(h->user, cv, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
        h->draw_texture(h->user, cv, id, 16, 8, 48, 40, 255);
        h->draw_rect(h->user, cv, 20, 10, 28, 18, &p, nullptr);
        check(h->present(h->user, cv, 0, 0, w, hgt) && readWindow(shown), "the window was not shown");
        check(shown.size() == size_t(w) * hgt && shown[14 * w + 40] == 0xFF00C000u && shown[34 * w + 40] == 0xFF00C0C0u,
              "the video's picture is not where its view drew it, the right way up (green at 40,14, cyan at 40,34)");
        check(shown.size() == size_t(w) * hgt && shown[14 * w + 24] == 0xFFFF0000u, "the views are not over the video (red at 24,14)");
        check(shown.size() == size_t(w) * hgt && shown[4 * w + 4] == 0xFFFFFFFFu && shown[24 * w + 56] == 0xFFFFFFFFu,
              "the views are not the window's picture outside the video (white at 4,4 and 56,24)");
        // A new picture: shown at once, composed with the views as they were, nothing of them drawn.
        const int was = g_window.shown;
        check(gfx.showVideoFrame(id, blue, 16, 16) == 2, "a new picture of a video under the views was not shown composed (2)");
        check(g_window.shown == was + 1 && readWindow(shown) && shown[14 * w + 40] == 0xFF0000C0u && shown[34 * w + 40] == 0xFFC0C000u &&
                  shown[14 * w + 24] == 0xFFFF0000u && shown[4 * w + 4] == 0xFFFFFFFFu,
              "the new picture is not what the window shows, under the views as they were");
        // A drawing of a part of the window (a small view's animation over the video) clips the list to that part: the video is
        // still under the whole of its view, and its next picture is shown there whole.
        h->save(h->user, cv);
        h->clip_rect(h->user, cv, 20, 10, 28, 18, MEGLER_GFX_CLIP_INTERSECT);
        h->draw_color(h->user, cv, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
        h->draw_texture(h->user, cv, id, 16, 8, 48, 40, 255);
        h->draw_rect(h->user, cv, 20, 10, 28, 18, &p, nullptr);
        h->restore(h->user, cv);
        h->present(h->user, cv, 20, 10, 8, 8);
        check(gfx.showVideoFrame(id, green, 16, 16) == 2 && readWindow(shown) && shown[14 * w + 40] == 0xFF00C000u && shown[34 * w + 40] == 0xFF00C0C0u &&
                  shown[14 * w + 24] == 0xFFFF0000u,
              "after the views were drawn in a part over the video, its next picture is not shown whole under them");
        // The views drawn again without it (its view scrolled away): the video is no longer under them, and its window is the views'.
        h->draw_color(h->user, cv, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
        check(h->present(h->user, cv, 0, 0, w, hgt) && readWindow(shown) && shown[14 * w + 40] == 0xFFFFFFFFu && shown[34 * w + 40] == 0xFFFFFFFFu,
              "the views drawn again without the video still show it");
        check(gfx.showVideoFrame(id, blue, 16, 16) == 1 && readWindow(shown) && shown[34 * w + 40] == 0xFFFFFFFFu,
              "a video the views no longer draw was still put under them");
        // Its view drawing it again puts it back; drawn at half its alpha it is among the views, and no longer under them.
        h->draw_color(h->user, cv, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
        h->draw_texture(h->user, cv, id, 16, 8, 48, 40, 255);
        check(h->present(h->user, cv, 0, 0, w, hgt) && gfx.showVideoFrame(id, green, 16, 16) == 2, "a video drawn again by its view was not under the views");
        h->draw_color(h->user, cv, 0xFFFFFFFFu, MEGLER_GFX_BLEND_SRC);
        h->draw_texture(h->user, cv, id, 16, 8, 48, 40, 128);
        h->present(h->user, cv, 0, 0, w, hgt);
        check(gfx.showVideoFrame(id, green, 16, 16) == 1, "a video its view draws among the views was still taken as under them");
        h->texture_frame(h->user, id, nullptr, 0, 0);
        h->canvas_free(h->user, cv);
    }
    if (green) green->Release();
    if (blue) blue->Release();
    gfx.setWindow(megler::app::GfxHost::Window());
    if (g_window.texture) g_window.texture->Release();
    g_window = TestWindow();
    return failed;
}

} // extern "C"
