// The test program's way to the shell's drawing: megler_gfx_host as
// app/Megler/player/GfxHost.cpp implements it over Direct2D, DirectWrite
// and WIC, for hk_tests --gfx-windows (hk_tests_main.c), which hands it to
// the scenes of hk_gfx_scenes.c.
//
// Only tools/build-tests.sh win builds this file, with GfxHost.cpp itself
// beside it: the build for qemu-arm has no Direct2D to link, and the phone's
// package gets its GfxHost from the shell.
#include "player/GfxHost.h"

#include <cstdio>
#include <cstdlib>
#include <string>

extern "C" {

/// The host, its lines going to `line`, drawing as `way` says: 0 on this
/// machine's graphics processor (where it has none, in memory, said in its
/// log), 1 in memory with every canvas copied to a WIC bitmap and back
/// (GfxHost::setCopying, the way a Direct2D that refused the app's memory
/// would be left with), 2 in memory, 3 on WARP, Direct3D's software device,
/// which draws as a graphics processor does. The environment's
/// MEGLER_GFX_FONTS names a folder of Android's own fonts
/// (GfxHost::setFontFolder; build/android9/root/fonts), for a run with those.
const megler_gfx_host* hk_gfx_windows_host(void (*line)(const char* text, void* user), void* user, int way) {
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
void hk_gfx_windows_counts(char* out, size_t size) {
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
int hk_gfx_windows_on_gpu(void) {
    const megler::app::GfxHost::Counts c = megler::app::GfxHost::shared().counts();
    return c.gpu && !c.gpuLost;
}

/// The graphics processor's device taken as lost, for the test of what a canvas does then.
void hk_gfx_windows_lose_device(void) { megler::app::GfxHost::shared().loseDevice(); }

} // extern "C"
