// A Java app's drawing: megler_gfx_host (core/include/megler.h, whose comment is
// the contract) over Direct2D, DirectWrite and the Windows Imaging Component,
// which every Windows 10 has, the phone's 1703+ included.
//
// An app whose user interface is Java draws with android.graphics. The core
// runs those classes and keeps the bitmaps (plain memory: B, G, R, A
// premultiplied, top row first); what it asks of the shell is the drawing
// proper, text, and image files. That is this class, and nothing else in the
// shell draws for an app.
//
// There are two ways of drawing, and one is chosen for the process at the
// first call: on the graphics processor, wherever Direct3D 11 gives a device
// of the hardware's own that can share textures with Direct2D (BGRA
// support), and in memory, by Direct2D's software rasterizer, where it does
// not, where the device is lost, or where a test or the phone's flag file
// "gfx-software" asks for it (setDrawing). The software rasterizer was
// measured on the phone (a Snapdragon 820): a frame that redraws a caret,
// 46,000 pixels and a dozen calls, cost 18-20 ms of drawing, half of it in
// EndDraw; that is why the graphics processor is the first choice.
//
// On the graphics processor. One Direct3D 11 device and one ID2D1Device on
// it serve every canvas. A canvas has a device context of its own
// (ID2D1DeviceContext; contexts and target bitmaps are kept for the next
// canvas when one is freed, so that a canvas still costs microseconds to
// make) and a target bitmap of the canvas's size in the graphics processor's
// memory, which is its picture. The core and the window read the core's
// memory, so the two are kept one picture by megler.h's flush and touched,
// both ways and only where needed:
//   - What the memory holds is uploaded (ID2D1Bitmap::CopyFromMemory) before
//     the first drawing after canvas_new or touched -- not at all when that
//     drawing is a drawColor that replaces every pixel, which is how every
//     frame of a window begins.
//   - What is drawn goes to the graphics processor, and the box round all of
//     it since the last flush is remembered. At flush that box alone is read
//     back: copied into a bitmap the processor can read
//     (D2D1_BITMAP_OPTIONS_CPU_READ), mapped, and its rows copied into the
//     memory. Mapping waits for the graphics processor to have drawn
//     everything before it: that wait is the price of the round trip, and
//     the log's line says what it costs.
//   - A layer (save_layer) is a target bitmap of its extent, cleared, drawn
//     into, and put onto the picture under it at its restore.
//   - Bitmaps drawn from the core's memory (draw_bitmap, shader_bitmap) are
//     uploaded once and kept under megler_gfx_image's id and version, dropped
//     at bitmap_gone or when the kept ones pass a budget (least recently
//     drawn first).
//   - Text is DirectWrite's glyph runs drawn by the context, in grey levels
//     as before; Direct2D keeps the glyphs' pictures on the processor.
//   - What Direct2D lays on by itself in memory it lays on here too: shapes,
//     bitmaps and text over what is there (SRC_OVER), clipped by a rectangle
//     or by a layer with a geometric mask. drawColor is a Clear or a filled
//     rectangle; a layer at SRC_OVER is DrawBitmap at the layer's alpha. The
//     other transfer modes, colour filters, shadows and the rest of the
//     host's own arithmetic are done by Direct2D's effects where they say
//     the same thing (GfxHost.cpp lists which, and what differs); anything
//     left reads the box it changes back into memory, puts the drawing on
//     there by the arithmetic below, and uploads the box again: slower, and
//     the same picture.
//
// In memory. A canvas is a Direct2D software render target
// (ID2D1Factory::CreateWicBitmapRenderTarget) over an IWICBitmap of this
// file's own, whose lock hands Direct2D the core's memory itself: nothing is
// copied, and a canvas costs a few microseconds to make. Direct2D collects
// what it is asked to draw and puts it into the memory at EndDraw -- Flush
// alone does not, as was seen on Windows 11 -- so a canvas is "drawing"
// between its first call after a flush and the next megler_gfx_host.flush,
// which is EndDraw. `touched` has nothing to do: the memory is read when it
// is drawn on, and no copy of it is kept. Should a Direct2D refuse a bitmap
// that is not WIC's own (none has been seen to), every canvas falls back to
// a WIC bitmap that is copied from the memory before a drawing and back
// after it; setCopying forces that, for a test of it.
//
// What Direct2D draws itself is what Android draws most: shapes, bitmaps and
// text laid over what is there (SRC_OVER), clipped by a rectangle or, through
// a layer with a geometric mask, by any shape. Everything else is drawn into
// memory of this file's own and put onto the picture by its own arithmetic,
// which is megler.h's table in whole numbers of 255ths: the other seventeen
// transfer modes, save_layer (every layer is a bitmap of this file's, joined
// to the picture at its restore), colour filters on shaders, sweep gradients
// (Direct2D has no such brush), shadows (blurred here), and drawColor. What
// is not done as megler.h says is listed at the top of GfxHost.cpp, and each
// such thing says so once in the log when an app first asks for it.
//
// Text is DirectWrite's: a run is laid out once (IDWriteTextLayout: shaping,
// and the system's fallback fonts for what the face lacks), its glyph runs,
// advances, break places and ink box are kept, and drawing and all five
// measuring functions answer from that. Runs are remembered by text, face,
// size and spacing, since the framework measures the same strings over and
// over. A typeface is a family in a collection of Android's own font files
// (setFontFolder), in the system's collection where those are not to be had,
// or in one made of a font file's bytes; NULL is Roboto, or Segoe UI.
//
// Threads. The callers are the app's threads (never the UI thread). The
// Direct2D factory is the multithreaded kind, which serialises every call
// into Direct2D by a lock of its own -- the device's and every context's
// calls included, and with them Direct2D's use of the Direct3D device -- so
// canvases may be drawn on from several threads at once; this file never
// calls the Direct3D device's context itself, which is what would need
// ID2D1Multithread's lock. A canvas's context, its target bitmaps and the
// bitmap its read-backs go through are its own, used by one thread at a
// time as the canvas is. DirectWrite's shared factory and WIC's objects take
// calls from any thread; this file's own tables (remembered text, typefaces,
// stroke styles, a shader's brushes, the kept bitmaps, the contexts and
// target bitmaps kept for the next canvas) have a lock each. WIC is COM: the
// process's multithreaded apartment is kept alive from the first image on
// (CoIncrementMTAUsage), and a thread that never initialised COM is then in
// it.
//
// A device that is lost (DXGI_ERROR_DEVICE_REMOVED, D2DERR_RECREATE_TARGET)
// is said once in the log, and from then on everything is drawn in memory:
// a canvas goes over to it at its next call, keeping what its last flush
// put into the memory (what was drawn since is lost, as Android loses a
// window's drawing with its surface).
//
// d2d1.dll and dwrite.dll are delay-loaded (tools/build-appx.sh) and first
// called under a guard, so a device without one gets a sentence in the log
// and an app that draws nothing, not a Megler that does not start.
// windowscodecs.dll is reached through CoCreateInstance alone; d3d11.dll is
// the shell's anyway (WindowHost).
//
// Nothing here is WinRT: the file is plain COM C++, built into the shell and
// into the core's test program on a PC (tools/build-tests.sh win, the mode
// --gfx-windows), which is where it has been seen drawing.
//
// One GfxHost serves the process: shared().
#pragma once

#include "megler.h"

#include <cstdint>

namespace megler::app {

class GfxHost {
public:
    static GfxHost& shared();
    GfxHost(const GfxHost&) = delete;
    GfxHost& operator=(const GfxHost&) = delete;

    /// The callbacks for megler_run_config.gfx, with this as their user.
    megler_gfx_host callbacks();

    /// Where this host's own lines go (the shell's log; the test's output):
    /// what it found at its first call, and each thing it cannot draw as
    /// asked, once. Any thread may be the caller. Before the first drawing.
    typedef void (*LineFn)(const char* line, void* user);
    void setLog(LineFn fn, void* user);

    /// Where canvases are drawn: on the graphics processor where there is a
    /// device for it (the default), on WARP (Direct3D's software device,
    /// which draws as a graphics processor would: for a test on a machine
    /// without one, or of a second rasterizer), or in memory by Direct2D's
    /// software rasterizer. Before the first drawing.
    enum class Drawing { graphicsProcessor, warp, memory };
    void setDrawing(Drawing drawing);

    /// Every canvas draws into a WIC bitmap of its own that is copied from
    /// and to the core's memory, as if Direct2D had refused the memory
    /// itself; in memory, then. Before the first drawing.
    void setCopying(bool copying);

    /// For a test: the graphics processor's device is taken as lost from
    /// now on, as if Direct3D had said DXGI_ERROR_DEVICE_REMOVED.
    void loseDevice();

    /// A folder with Android's own fonts in it (Roboto-Regular.ttf and the
    /// rest of Android 9's /system/fonts that its named families are made
    /// of): they are then the faces an app gets by Android's family names,
    /// the default face among them, measured as Android measures them. A
    /// family whose files are not there is the system's nearest, as all are
    /// without this. Before the first drawing.
    void setFontFolder(const wchar_t* folder);

    /// What has been done so far, for a test or a line in a log.
    struct Counts {
        uint64_t canvases = 0;     // made
        uint64_t sessions = 0;     // BeginDraw ... EndDraw
        uint64_t direct = 0;       // drawing calls Direct2D put onto the picture itself
        uint64_t composed = 0;     // drawing calls put onto the picture by this file's arithmetic
        uint64_t layers = 0;       // save_layer
        uint64_t runsShaped = 0;   // text runs laid out
        uint64_t runsReused = 0;   // ... and found already laid out
        uint64_t notes = 0;        // kinds of thing said in the log as not done right
        uint64_t partTargets = 0;  // times a canvas's render target was moved to the part its clip leaves open
        bool started = false;      // Direct2D and DirectWrite are there
        bool copying = false;      // canvases are copied in and out
        // On the graphics processor:
        bool gpu = false;          // canvases are made there
        bool gpuLost = false;      // ... and the device has been lost since
        uint64_t uploads = 0;      // times memory was uploaded to a canvas's picture
        uint64_t readBacks = 0;    // ... and read back from it
        uint64_t byEffects = 0;    // drawing calls put on by Direct2D's effects
        uint64_t throughMemory = 0; // ... and by this file's arithmetic, read back and uploaded again
        uint64_t keptMade = 0;     // bitmaps of the core's uploaded to be kept
        uint64_t keptReused = 0;   // ... and drawn again from there
        char device[128] = {};     // the graphics processor's name, and Direct3D's feature level
    };
    Counts counts();

private:
    GfxHost() = default;
};

} // namespace megler::app
