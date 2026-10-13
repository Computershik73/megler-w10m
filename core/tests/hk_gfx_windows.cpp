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

extern "C" {

/// The host, its lines going to `line`. `copying`: with every canvas copied
/// to a WIC bitmap and back (GfxHost::setCopying), the way of drawing a
/// Direct2D that refused the app's memory would be left with.
const megler_gfx_host* hk_gfx_windows_host(void (*line)(const char* text, void* user), void* user, int copying) {
    static megler_gfx_host host;
    megler::app::GfxHost& gfx = megler::app::GfxHost::shared();
    gfx.setLog(line, user);
    gfx.setCopying(copying != 0);
    host = gfx.callbacks();
    return &host;
}

/// What the host did, in a line.
void hk_gfx_windows_counts(char* out, size_t size) {
    const megler::app::GfxHost::Counts c = megler::app::GfxHost::shared().counts();
    snprintf(out, size,
             "%llu canvases, %llu times Direct2D began and ended drawing, %llu drawing calls put on by Direct2D and %llu by the host's own arithmetic, %llu layers; "
             "%llu runs of text laid out and %llu found laid out; %llu kinds of thing said to be drawn not as asked",
             static_cast<unsigned long long>(c.canvases), static_cast<unsigned long long>(c.sessions), static_cast<unsigned long long>(c.direct),
             static_cast<unsigned long long>(c.composed), static_cast<unsigned long long>(c.layers), static_cast<unsigned long long>(c.runsShaped),
             static_cast<unsigned long long>(c.runsReused), static_cast<unsigned long long>(c.notes));
}

} // extern "C"
