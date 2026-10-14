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
// How the memory is drawn into. A canvas is a Direct2D software render target
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
// into Direct2D by a lock of its own, so canvases may be drawn on from
// several threads at once; DirectWrite's shared factory and WIC's objects
// take calls from any thread; this file's own tables (remembered text,
// typefaces, stroke styles, a shader's brush) have a lock each. WIC is COM:
// the process's multithreaded apartment is kept alive from the first image
// on (CoIncrementMTAUsage), and a thread that never initialised COM is then
// in it.
//
// d2d1.dll and dwrite.dll are delay-loaded (tools/build-appx.sh) and first
// called under a guard, so a device without one gets a sentence in the log
// and an app that draws nothing, not a Megler that does not start.
// windowscodecs.dll is reached through CoCreateInstance alone.
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

    /// Every canvas draws into a WIC bitmap of its own that is copied from
    /// and to the core's memory, as if Direct2D had refused the memory
    /// itself. Before the first drawing.
    void setCopying(bool copying);

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
    };
    Counts counts();

private:
    GfxHost() = default;
};

} // namespace megler::app
