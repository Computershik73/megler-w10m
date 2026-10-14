// megler_gfx_host over Direct2D, DirectWrite and WIC (GfxHost.h says how, and
// on which threads). Plain COM: no WinRT, no precompiled header, so that the
// same file builds into the shell and into the core's test program.
//
// WHAT IS NOT AS megler.h SAYS, each of which says so once in the log when an
// app first asks for it:
//
//   - A colour glyph (an emoji) is drawn in its own colours only when it is
//     filled in the paint's colour and laid over the picture (SRC_OVER, no
//     shader, no stroke, no fake bold). Drawn any other way it is the
//     glyph's shape in the paint's colour or the shader's. Its colours are
//     those of the font's first palette at the paint's alpha; a colour
//     filter does not reach them.
//   - A shadow under a bitmap is the shadow of the rectangle it is drawn
//     into, not of the picture's own shape.
//   - A hairline, where Direct2D has no ID2D1Factory1 (it has on every
//     Windows 10): as wide as a pixel is under the matrix's mean scale,
//     which is not one pixel under a matrix that stretches one way.
//   - A transfer mode or a colour filter's mode whose number is not one of
//     the eighteen: drawn as SRC_OVER, the filter left out. A coordinate
//     that is no number, or beyond a thousand million: the call draws
//     nothing.
//
// And what megler.h leaves open, decided here:
//
//   - A rectangle clip under a matrix that keeps rectangles upright is whole
//     pixels: those whose centres are in it (Android's clipRect is not
//     smoothed). A clip by a turned rectangle or by a path is smoothed.
//   - save_layer's bounds are taken as their box in pixels, under any matrix.
//   - What is drawn into a layer is in the layer's own picture until its
//     restore: a flush while a layer is open puts into the memory what was
//     drawn outside it.
//   - A line feed, a carriage return and the other control characters of a
//     run take no room; a tab takes a space's.
//   - A "character" for letter spacing is DirectWrite's cluster: half the
//     spacing before it and half after, once for a surrogate pair.
//   - Fake bold is a stroke of a twenty-fourth of the text's size round the
//     glyphs' outlines, as the original has it; it and a paint's stroke
//     widen text_bounds and leave the advances as they are.
//   - text_bounds of a slanted run is the box of each glyph's own box
//     slanted: never smaller than the ink, and wider than it by at most the
//     slant times a glyph's height.
//   - A shadow's blur under a matrix that stretches one way grows by the
//     matrix's mean scale both ways, and stops at the widest blur an
//     unscaled canvas can ask for.
//   - A bitmap's rectangle has smoothed edges when the paint asks for
//     smoothing, whatever the sampling.
//   - A partly covered pixel of a smoothed edge, for the transfer modes that
//     can take colour away (CLEAR, SRC, SRC_IN, DST_IN, SRC_OUT, DST_ATOP,
//     MODULATE), is mixed by its coverage as megler.h says; for the others the
//     source is weighted by the coverage, which is the same thing for every
//     one of them but PLUS, where it differs by less than a step.
//
// ON THE GRAPHICS PROCESSOR (GfxHost.h says how the memory and the
// processor's picture are kept one). Every drawing call is put onto the
// picture in one of three ways:
//
//   - By Direct2D, as in memory: shapes, text and bitmaps laid over the
//     picture (SRC_OVER) in a colour or through a linear, radial or bitmap
//     brush, clipped by a rectangle or a geometric layer.
//   - By Direct2D on the processor where memory needed this file's
//     arithmetic, as the same formula ("Putting a drawing together on the
//     graphics processor" below):
//       drawColor           SRC and CLEAR within a rectangle clip, and an
//                           opaque SRC_OVER: Clear under an axis-aligned clip.
//                           SRC_OVER: a filled rectangle under the clip.
//       a layer's restore   SRC_OVER: DrawBitmap at the layer's alpha under
//                           the clip at the save.
//       transfer modes      the call's colours times its coverage (the
//                           shape's smoothed edge and the clip's) drawn into
//                           a work bitmap of the canvas's own, A; then
//                           DST_OVER, DST_OUT, SRC_ATOP, XOR, PLUS:
//                             DrawImage of A with that composite mode, which
//                             leaves the picture as it is where A is nothing,
//                             as megler.h's table does;
//                           SCREEN, OVERLAY, DARKEN, LIGHTEN: Direct2D's
//                             Blend effect over a copy of the box, whose
//                             formula is megler.h's for these four;
//                           CLEAR, SRC, SRC_IN, DST_IN, SRC_OUT, DST_ATOP,
//                             MODULATE, which take colour away where a pixel
//                             is partly covered: D * (1 - coverage) + the
//                             mode of A and D (Composite and
//                             ArithmeticComposite effects over a copy of the
//                             box), copied into the box. Every one of these
//                             is linear in the source, so the mode of A is
//                             the mode of the colour times the coverage.
//                           A layer by such a mode is its box at its alpha
//                           (ColorMatrix) as A, cut by a clip that is a path.
//       colour filters      on a shader's colours: the shader drawn over the
//                           box at the paint's alpha, then fifteen of the
//                           modes as a ColorMatrix (with the filter's colour
//                           fixed, each is a sum of the colour, its alpha
//                           and a constant), OVERLAY, DARKEN and LIGHTEN as
//                           the Blend effect with a Flood; then cut by the
//                           shape's coverage. On a bitmap's pixels: a copy of
//                           the part made in memory, as in memory, and
//                           uploaded for the drawing.
//       sweep gradients     their colours over the box worked out in memory
//                           (Direct2D has no such brush) and uploaded, then
//                           as a shader's colours.
//       fill and stroke     that overlap, not opaque: the colours cut once by
//                           the coverage of both.
//       shadows             the call's coverage through the GaussianBlur
//                           effect, moved, cut by the clip, in the shadow's
//                           colour.
//   - Through memory, for what is left: where a work bitmap cannot be made
//     (a box larger than the device's bitmaps) or an effect fails, the box
//     the call can change is read back, the call is put on by this file's
//     arithmetic as in memory, and the box is uploaded again. Nothing in the
//     scenes or the test apps' drawing comes this way.
//
// What differs from drawing in memory, all of it seen in the scenes'
// pictures against those drawn in memory (tools/build-tests.sh win):
//   - The edges of smoothed shapes, strokes and glyph outlines (text drawn as
//     glyph runs is the same pixel for pixel): the processor's rasterizer
//     covers an edge pixel a little differently from the software one, by
//     up to a few dozen steps on a pixel, and only on the edge.
//   - Bitmaps scaled with smooth sampling: the processor's bilinear
//     filtering weighs neighbours in coarser steps, which shows where the
//     picture itself has a sharp edge.
//   - Blending and effects work in floating point and round to whole 255ths
//     at the end of each step, where this file's arithmetic rounds every
//     product: the eighteen modes, as drawing calls, layers, drawColor and
//     colour filters, are within a step of megler.h's table (the scenes' check
//     allows two). OVERLAY's choice between its two formulas can fall the
//     other way on a pixel where 2*D is Da to within rounding.
//   - A shadow's blur is Direct2D's Gaussian at its quality setting: within a
//     few steps of this file's.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <objbase.h>

#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d3d11.h>
#include <dwrite_2.h>
#include <wincodec.h>

#include "player/GfxHost.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace megler::app {

namespace {

using D2D1::Matrix3x2F;

// MARK: COM, in the small

/// A reference to a COM object.
template <class T> class Ref {
public:
    Ref() = default;
    Ref(const Ref& other) : p_(other.p_) {
        if (p_) p_->AddRef();
    }
    Ref(Ref&& other) noexcept : p_(other.p_) { other.p_ = nullptr; }
    /// A reference to the same object as one of a class derived from T.
    template <class U> Ref(const Ref<U>& other) : p_(other.get()) {
        if (p_) p_->AddRef();
    }
    ~Ref() {
        if (p_) p_->Release();
    }
    Ref& operator=(const Ref& other) {
        if (other.p_) other.p_->AddRef();
        if (p_) p_->Release();
        p_ = other.p_;
        return *this;
    }
    Ref& operator=(Ref&& other) noexcept {
        if (this != &other) {
            if (p_) p_->Release();
            p_ = other.p_;
            other.p_ = nullptr;
        }
        return *this;
    }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    void reset() {
        if (p_) p_->Release();
        p_ = nullptr;
    }
    /// For a function that returns an object through a T**.
    T** put() {
        reset();
        return &p_;
    }
    /// Takes over a reference the caller holds.
    void attach(T* p) {
        reset();
        p_ = p;
    }
    template <class U> Ref<U> as() const {
        Ref<U> out;
        if (p_ && FAILED(p_->QueryInterface(__uuidof(U), reinterpret_cast<void**>(out.put())))) out.reset();
        return out;
    }

private:
    T* p_ = nullptr;
};

/// The reference count of an object of this file's that is handed to COM.
class Counted {
public:
    virtual ~Counted() = default;
    ULONG addRef() { return ULONG(InterlockedIncrement(&refs_)); }
    ULONG release() {
        const LONG left = InterlockedDecrement(&refs_);
        if (!left) delete this;
        return ULONG(left);
    }

private:
    LONG refs_ = 1;
};

// WIC's identifiers, written out: they live in libraries the shell does not
// link (uuid.lib, windowscodecs.lib). The values are wincodec.h's.
const GUID wicFactory1 = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
const GUID wicFactory2 = {0x317d06e8, 0x5f24, 0x433d, {0xbd, 0xf7, 0x79, 0xce, 0x68, 0xd8, 0xab, 0xc2}};
const GUID wic24bppBGR = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0c}};
const GUID wic32bppBGRA = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0f}};
const GUID wic32bppPBGRA = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x10}};
const GUID wicPng = {0x1b7cfaf4, 0x713f, 0x473c, {0xbb, 0xcd, 0x61, 0x37, 0x42, 0x5f, 0xae, 0xaf}};
const GUID wicJpeg = {0x19e4a5aa, 0x5662, 0x4fc5, {0xa0, 0xc0, 0x17, 0x58, 0x02, 0x8e, 0x10, 0x57}};

// Direct2D's built-in effects, likewise (d2d1effects.h's values; dxguid.lib is not linked).
const GUID effectArithmetic = {0xfc151437, 0x049a, 0x4784, {0xa2, 0x4a, 0xf1, 0xc4, 0xda, 0xf2, 0x09, 0x87}};
const GUID effectBlend = {0x81c5b77b, 0x13f8, 0x4cdd, {0xad, 0x20, 0xc8, 0x90, 0x54, 0x7a, 0xc6, 0x5d}};
const GUID effectColorMatrix = {0x921f03d6, 0x641c, 0x47df, {0x85, 0x2d, 0xb4, 0xbb, 0x61, 0x53, 0xae, 0x11}};
const GUID effectComposite = {0x48fc9f51, 0xf6ac, 0x48f1, {0x8b, 0x58, 0x3b, 0x28, 0xac, 0x46, 0xf7, 0x6d}};
const GUID effectFlood = {0x61c23c20, 0xae69, 0x4d8e, {0x94, 0xcf, 0x50, 0x07, 0x8d, 0xf6, 0x38, 0xf2}};
const GUID effectGaussianBlur = {0x1feb6d69, 0x2fe6, 0x4ac9, {0x8c, 0x58, 0x1d, 0x7f, 0x93, 0xe7, 0xa6, 0xa5}};

// MARK: The log

struct Typeface;
struct Surface;
struct GpuContext;

/// A bitmap of the core's kept on the graphics processor (draw_bitmap,
/// shader_bitmap), under its megler_gfx_image id.
struct Kept {
    uint32_t version = 0;
    Ref<ID2D1Bitmap1> bitmap;
    uint64_t bytes = 0;
    uint64_t used = 0; // when it was last drawn, by keptClock
};

/// Everything of the host's that outlives a call.
struct State {
    // The log.
    std::mutex logMutex;
    GfxHost::LineFn line = nullptr;
    void* lineUser = nullptr;
    std::set<std::string> said;

    // Direct2D and DirectWrite: made at the first call, under startMutex.
    std::mutex startMutex;
    std::atomic<int> started{0}; // 0 not tried, 1 there, -1 cannot be
    bool copyingAsked = false;
    bool copying = false;

    // The graphics processor (GfxHost.h): made at the first call too. `gpu`
    // is set once, before any canvas is made; `gpuLost` from then on, by
    // whichever thread finds the device gone.
    GfxHost::Drawing drawingAsked = GfxHost::Drawing::graphicsProcessor;
    bool gpu = false;
    std::atomic<bool> gpuLost{false};
    Ref<ID3D11Device> d3d;
    Ref<ID2D1Device> d2dDevice;
    Ref<ID2D1DeviceContext> gpuResource; // makes what several canvases share: a shader's brush, kept bitmaps
    UINT32 gpuMaxSize = 4096;            // the largest bitmap the device takes, each way
    std::string gpuName;                 // the adapter's, for the log
    // Contexts and target bitmaps of canvases that are gone, for the next ones (gpuMutex).
    std::mutex gpuMutex;
    std::vector<GpuContext*> freeContexts;
    std::vector<Ref<ID2D1Bitmap1>> freeTargets, freeReadables;
    uint64_t freeBytes = 0;
    // Bitmaps of the core's kept on the processor (keptMutex), and how many bytes of them there are.
    std::mutex keptMutex;
    std::unordered_map<uint32_t, Kept> kept;
    uint64_t keptBytes = 0, keptClock = 0;
    // Where a window's picture is shown without the memory (GfxHost::setWindow, present), and the target bitmap over
    // its texture, made for the texture it last gave (windowMutex).
    std::mutex windowMutex;
    GfxHost::Window window;
    ID3D11Texture2D* windowTexture = nullptr;
    Ref<ID2D1Bitmap1> windowBitmap;
    uint64_t windowCanvas = 0; // the serial of the canvas whose picture was shown last
    std::atomic<uint64_t> canvasSerial{1};
    Ref<ID2D1Factory> d2d;
    Ref<ID2D1Factory1> d2d1;
    Ref<IDWriteFactory> dwrite;
    Ref<IDWriteFactory2> dwrite2;
    Ref<IDWriteFontCollection> systemFonts;
    std::wstring locale;
    Typeface* defaultFace = nullptr;
    /// A render target of one pixel: brushes and bitmaps that several
    /// canvases use (a shader's) are made by it. Software render targets of
    /// one factory share what they make.
    Surface* resource = nullptr;
    uint32_t resourcePixel = 0;

    // WIC: made at the first image.
    std::mutex wicMutex;
    int wicState = 0;
    Ref<IWICImagingFactory> wic;

    // Stroke styles, by what they are.
    std::mutex strokeMutex;
    std::unordered_map<uint64_t, Ref<ID2D1StrokeStyle>> strokes;

    // Fonts out of an app's bytes.
    std::mutex fontMutex;
    bool fontLoadersRegistered = false;
    uint64_t nextFontKey = 1;
    std::unordered_map<uint64_t, std::shared_ptr<std::vector<uint8_t>>> fontData;

    // Android's own fonts (GfxHost::setFontFolder): a collection for each of Android's families, made when first asked for.
    struct AndroidFamily {
        bool tried = false;
        Ref<IDWriteFontCollection> collection; // null: its files are not there, and the system's fonts stand in
        std::wstring family;                   // as DirectWrite calls it
        DWRITE_FONT_STRETCH stretch = DWRITE_FONT_STRETCH_NORMAL;
    };
    std::wstring fontFolder; // set before the first drawing, read without a lock after
    std::mutex androidMutex; // what follows
    int folderLoaderState = 0; // 0 not registered, 1 registered, -1 DirectWrite would not
    AndroidFamily androidFamilies[8];

    std::atomic<uint64_t> clipSerial{1};
    std::atomic<uint64_t> canvases{0}, sessions{0}, direct{0}, composed{0}, layers{0}, runsShaped{0}, runsReused{0}, notes{0}, partTargets{0};
    // Where the drawing's time goes, in performance-counter ticks, and how much it moved: a line in the log every 5 s
    // while an app draws (noteDrawing).
    std::atomic<uint64_t> targetsMade{0}, makeTicks{0}, endTicks{0}, composeTicks{0}, composedPixels{0}, sessionPixels{0};
    // On the graphics processor: the memory uploaded, read back (the wait for the processor counted in), the calls
    // put on by Direct2D's effects and through memory, and the kept bitmaps made and drawn again.
    std::atomic<uint64_t> uploads{0}, uploadPixels{0}, uploadTicks{0}, readBacks{0}, readBackPixels{0}, readBackTicks{0}, byEffects{0},
        throughMemory{0}, keptMade{0}, keptReused{0};
    // ... and the window's pictures shown without the memory, and the read-backs of such a window's canvas, which come only
    // when something reads its memory.
    std::atomic<uint64_t> shown{0}, showTicks{0}, windowReadBacks{0}, windowReadBackPixels{0}, windowReadBackTicks{0};
    std::mutex statsMutex;
    uint64_t statsAt = 0; // when the last line was written (ticks); 0: never
    struct Totals {
        uint64_t sessions = 0, direct = 0, composed = 0, layers = 0, partTargets = 0, targetsMade = 0, makeTicks = 0, endTicks = 0, composeTicks = 0,
                 composedPixels = 0, sessionPixels = 0, uploads = 0, uploadPixels = 0, uploadTicks = 0, readBacks = 0, readBackPixels = 0,
                 readBackTicks = 0, byEffects = 0, throughMemory = 0, keptMade = 0, keptReused = 0, shown = 0, showTicks = 0, windowReadBacks = 0,
                 windowReadBackPixels = 0, windowReadBackTicks = 0;
    } statsLast;
};

State& G() {
    // Never destroyed: an app's threads may still be in a call when the process ends.
    static State* const state = new State();
    return *state;
}

void emit(const char* text) {
    State& s = G();
    GfxHost::LineFn fn;
    void* user;
    {
        std::lock_guard<std::mutex> lock(s.logMutex);
        fn = s.line;
        user = s.lineUser;
    }
    if (fn) fn(text, user);
}

/// A line in the log.
void say(const char* fmt, ...) {
    char text[600] = "gfx: ";
    va_list args;
    va_start(args, fmt);
    vsnprintf(text + 5, sizeof(text) - 5, fmt, args);
    va_end(args);
    emit(text);
}

inline uint64_t ticksNow() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return uint64_t(t.QuadPart);
}

inline double ticksToMs(uint64_t ticks) {
    static const double perMs = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return double(f.QuadPart) / 1000.0;
    }();
    return double(ticks) / perMs;
}

/// A line in the log the first time something of this `kind` happens, and
/// never again: what could not be drawn as asked, or failed.
void note(const char* kind, const char* fmt, ...) {
    State& s = G();
    {
        std::lock_guard<std::mutex> lock(s.logMutex);
        if (!s.said.insert(kind).second) return;
    }
    ++s.notes;
    char text[600] = "gfx: ";
    va_list args;
    va_start(args, fmt);
    vsnprintf(text + 5, sizeof(text) - 5, fmt, args);
    va_end(args);
    emit(text);
}

/// The same for a call that failed: once for each call and result.
void failed(const char* what, HRESULT hr) {
    char kind[96];
    snprintf(kind, sizeof(kind), "%s/%08lx", what, static_cast<unsigned long>(hr));
    note(kind, "%s failed, 0x%08lX (said once)", what, static_cast<unsigned long>(hr));
}

// MARK: Pixels

/// A pixel is the word 0xAARRGGBB, premultiplied; a colour argument is the
/// same word not premultiplied. These are megler.h's table in 255ths rounded
/// to the nearest, the arithmetic the core itself uses for single pixels.
inline uint32_t mul255(uint32_t a, uint32_t b) { return (a * b + 127) / 255; }

inline uint32_t premultiplied(uint32_t c) {
    const uint32_t a = c >> 24;
    if (a == 255) return c;
    return a << 24 | mul255((c >> 16) & 255, a) << 16 | mul255((c >> 8) & 255, a) << 8 | mul255(c & 255, a);
}

inline uint32_t unpremultiplied(uint32_t p) {
    const uint32_t a = p >> 24;
    if (!a) return 0;
    if (a == 255) return p;
    const uint32_t r = std::min<uint32_t>(255, (((p >> 16) & 255) * 255 + a / 2) / a);
    const uint32_t g = std::min<uint32_t>(255, (((p >> 8) & 255) * 255 + a / 2) / a);
    const uint32_t b = std::min<uint32_t>(255, ((p & 255) * 255 + a / 2) / a);
    return a << 24 | r << 16 | g << 8 | b;
}

/// Every channel of a premultiplied pixel by an alpha of 0..255.
inline uint32_t faded(uint32_t p, uint32_t alpha) {
    if (alpha >= 255 || !p) return p;
    return mul255(p >> 24, alpha) << 24 | mul255((p >> 16) & 255, alpha) << 16 | mul255((p >> 8) & 255, alpha) << 8 | mul255(p & 255, alpha);
}

inline uint32_t blendChannel(int mode, uint32_t s, uint32_t d, uint32_t sa, uint32_t da) {
    uint32_t v;
    switch (mode) {
    case MEGLER_GFX_BLEND_CLEAR: v = 0; break;
    case MEGLER_GFX_BLEND_SRC: v = s; break;
    case MEGLER_GFX_BLEND_DST: v = d; break;
    case MEGLER_GFX_BLEND_DST_OVER: v = d + mul255(s, 255 - da); break;
    case MEGLER_GFX_BLEND_SRC_IN: v = mul255(s, da); break;
    case MEGLER_GFX_BLEND_DST_IN: v = mul255(d, sa); break;
    case MEGLER_GFX_BLEND_SRC_OUT: v = mul255(s, 255 - da); break;
    case MEGLER_GFX_BLEND_DST_OUT: v = mul255(d, 255 - sa); break;
    case MEGLER_GFX_BLEND_SRC_ATOP: v = mul255(s, da) + mul255(d, 255 - sa); break;
    case MEGLER_GFX_BLEND_DST_ATOP: v = mul255(d, sa) + mul255(s, 255 - da); break;
    case MEGLER_GFX_BLEND_XOR: v = mul255(s, 255 - da) + mul255(d, 255 - sa); break;
    case MEGLER_GFX_BLEND_PLUS: v = s + d; break;
    case MEGLER_GFX_BLEND_MODULATE: v = mul255(s, d); break;
    case MEGLER_GFX_BLEND_SCREEN: v = s + d - mul255(s, d); break;
    case MEGLER_GFX_BLEND_OVERLAY: {
        // S*(1-Da) + D*(1-Sa) + (2*D <= Da ? 2*S*D : Sa*Da - 2*(Da-D)*(Sa-S)), every product a 255th.
        const int extra = 2 * d <= da ? int((2 * s * d + 127) / 255) : int(mul255(sa, da)) - int((2 * (da - d) * (sa - s) + 127) / 255);
        const int sum = int(mul255(s, 255 - da) + mul255(d, 255 - sa)) + extra;
        v = uint32_t(sum < 0 ? 0 : sum);
        break;
    }
    case MEGLER_GFX_BLEND_DARKEN: v = mul255(s, 255 - da) + mul255(d, 255 - sa) + std::min(mul255(s, da), mul255(d, sa)); break;
    case MEGLER_GFX_BLEND_LIGHTEN: v = mul255(s, 255 - da) + mul255(d, 255 - sa) + std::max(mul255(s, da), mul255(d, sa)); break;
    default: v = s + mul255(d, 255 - sa); break;
    }
    return v > 255 ? 255 : v;
}

/// S put on D by one of megler.h's eighteen numbers; both premultiplied.
inline uint32_t blended(int mode, uint32_t s, uint32_t d) {
    const uint32_t sa = s >> 24, da = d >> 24;
    if (mode == MEGLER_GFX_BLEND_SRC_OVER) {
        // By far the commonest: S + D*(1-Sa), and nothing to work out for a source that is all there or not there at all.
        if (sa == 255) return s;
        if (s == 0) return d;
        const uint32_t k = 255 - sa;
        const uint32_t a = sa + mul255(da, k), r = ((s >> 16) & 255) + mul255((d >> 16) & 255, k), g = ((s >> 8) & 255) + mul255((d >> 8) & 255, k),
                       b = (s & 255) + mul255(d & 255, k);
        return std::min<uint32_t>(a, 255) << 24 | std::min<uint32_t>(r, 255) << 16 | std::min<uint32_t>(g, 255) << 8 | std::min<uint32_t>(b, 255);
    }
    // A colour channel is never above its alpha in a pixel; a source that is
    // (arithmetic on a smoothed edge can make one) is brought back first.
    return blendChannel(mode, sa, da, sa, da) << 24 | blendChannel(mode, std::min((s >> 16) & 255, sa), (d >> 16) & 255, sa, da) << 16 |
           blendChannel(mode, std::min((s >> 8) & 255, sa), (d >> 8) & 255, sa, da) << 8 | blendChannel(mode, std::min(s & 255, sa), d & 255, sa, da);
}

/// Whether a source with no alpha leaves the picture as it is under this
/// mode. For the modes where it does not, a partly covered pixel has to be
/// mixed with what was there by its coverage (megler.h, "DRAWING").
inline bool keepsUncovered(int mode) {
    switch (mode) {
    case MEGLER_GFX_BLEND_CLEAR:
    case MEGLER_GFX_BLEND_SRC:
    case MEGLER_GFX_BLEND_SRC_IN:
    case MEGLER_GFX_BLEND_DST_IN:
    case MEGLER_GFX_BLEND_SRC_OUT:
    case MEGLER_GFX_BLEND_DST_ATOP:
    case MEGLER_GFX_BLEND_MODULATE: return false;
    default: return true;
    }
}

inline uint32_t addPixels(uint32_t a, uint32_t b) {
    const uint32_t aa = std::min<uint32_t>(255, (a >> 24) + (b >> 24)), r = std::min<uint32_t>(255, ((a >> 16) & 255) + ((b >> 16) & 255));
    const uint32_t g = std::min<uint32_t>(255, ((a >> 8) & 255) + ((b >> 8) & 255)), bl = std::min<uint32_t>(255, (a & 255) + (b & 255));
    return aa << 24 | r << 16 | g << 8 | bl;
}

inline D2D1_COLOR_F colorOf(uint32_t argb, uint32_t alpha = 255) {
    D2D1_COLOR_F c;
    c.r = float((argb >> 16) & 255) / 255.0f;
    c.g = float((argb >> 8) & 255) / 255.0f;
    c.b = float(argb & 255) / 255.0f;
    c.a = float(argb >> 24) / 255.0f * (float(alpha) / 255.0f);
    return c;
}

/// A coordinate as the first pixel whose centre is not before it: the edge
/// of a shape that is whole pixels, by megler.h's rule (a centre on a left or
/// top edge is inside, on a right or bottom edge outside).
inline int pixelEdge(double v) {
    if (!(v == v)) return 0;
    v = std::ceil(v - 0.5);
    if (v > 1000000000.0) return 1000000000;
    if (v < -1000000000.0) return -1000000000;
    return int(v);
}

inline int floorPixel(double v) {
    if (!(v == v)) return 0;
    v = std::floor(v);
    return v > 1000000000.0 ? 1000000000 : v < -1000000000.0 ? -1000000000 : int(v);
}

inline int ceilPixel(double v) {
    if (!(v == v)) return 0;
    v = std::ceil(v);
    return v > 1000000000.0 ? 1000000000 : v < -1000000000.0 ? -1000000000 : int(v);
}

/// A rectangle of whole pixels of a canvas.
struct Box {
    int l = 0, t = 0, r = 0, b = 0;
    bool empty() const { return l >= r || t >= b; }
    int width() const { return r - l; }
    int height() const { return b - t; }
    void intersect(const Box& o) {
        l = std::max(l, o.l);
        t = std::max(t, o.t);
        r = std::min(r, o.r);
        b = std::min(b, o.b);
        if (r < l) r = l;
        if (b < t) b = t;
    }
    void inflate(int by) {
        l -= by;
        t -= by;
        r += by;
        b += by;
    }
    void move(int dx, int dy) {
        l += dx;
        r += dx;
        t += dy;
        b += dy;
    }
    /// The box round this and `o`.
    void include(const Box& o) {
        if (o.empty()) return;
        if (empty()) {
            *this = o;
            return;
        }
        l = std::min(l, o.l);
        t = std::min(t, o.t);
        r = std::max(r, o.r);
        b = std::max(b, o.b);
    }
    bool covers(const Box& o) const { return o.empty() || (!empty() && l <= o.l && t <= o.t && r >= o.r && b >= o.b); }
};

/// The box of a rectangle through a matrix: where its four corners land.
void mappedBounds(const Matrix3x2F& m, float l, float t, float r, float b, double* x0, double* y0, double* x1, double* y1) {
    const D2D1_POINT_2F corner[4] = {m.TransformPoint(D2D1::Point2F(l, t)), m.TransformPoint(D2D1::Point2F(r, t)), m.TransformPoint(D2D1::Point2F(r, b)),
                                     m.TransformPoint(D2D1::Point2F(l, b))};
    *x0 = *x1 = corner[0].x;
    *y0 = *y1 = corner[0].y;
    for (int i = 1; i < 4; i++) {
        *x0 = std::min<double>(*x0, corner[i].x);
        *x1 = std::max<double>(*x1, corner[i].x);
        *y0 = std::min<double>(*y0, corner[i].y);
        *y1 = std::max<double>(*y1, corner[i].y);
    }
}

/// The pixels a rectangle through a matrix can touch, and `margin` more.
Box coverOf(const Matrix3x2F& m, float l, float t, float r, float b, int margin) {
    double x0, y0, x1, y1;
    mappedBounds(m, l, t, r, b, &x0, &y0, &x1, &y1);
    Box box;
    box.l = floorPixel(x0) - margin;
    box.t = floorPixel(y0) - margin;
    box.r = ceilPixel(x1) + margin;
    box.b = ceilPixel(y1) + margin;
    return box;
}

/// Whether a matrix takes upright rectangles to upright rectangles.
inline bool keepsUpright(const Matrix3x2F& m) { return (m._12 == 0 && m._21 == 0) || (m._11 == 0 && m._22 == 0); }

/// How much longer a matrix makes a length, about: 1 for one that only moves and turns.
inline float meanScale(const Matrix3x2F& m) {
    const float det = std::fabs(m._11 * m._22 - m._12 * m._21);
    return det > 0 ? std::sqrt(det) : 1.0f;
}

// MARK: Memory as a WIC bitmap

/// A lock on memory that is not WIC's: what Direct2D draws into, or reads a
/// picture from.
class MemoryLock final : public IWICBitmapLock, public Counted {
public:
    MemoryLock(uint8_t* data, UINT width, UINT height, UINT stride) : data_(data), width_(width), height_(height), stride_(stride) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IWICBitmapLock)) {
            *out = static_cast<IWICBitmapLock*>(this);
            addRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return addRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return release(); }
    HRESULT STDMETHODCALLTYPE GetSize(UINT* width, UINT* height) override {
        if (!width || !height) return E_INVALIDARG;
        *width = width_;
        *height = height_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStride(UINT* stride) override {
        if (!stride) return E_INVALIDARG;
        *stride = stride_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDataPointer(UINT* size, WICInProcPointer* data) override {
        if (!size || !data) return E_INVALIDARG;
        // (The last row may be shorter than a stride.)
        *size = height_ ? stride_ * (height_ - 1) + width_ * 4 : 0;
        *data = data_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelFormat(WICPixelFormatGUID* format) override {
        if (!format) return E_INVALIDARG;
        *format = wic32bppPBGRA;
        return S_OK;
    }

private:
    uint8_t* data_;
    UINT width_, height_, stride_;
};

/// Memory as an IWICBitmap: 32bppPBGRA, which is the core's bitmaps as they
/// are. Nothing is copied and nothing is owned; whoever makes one answers
/// for the memory outliving it.
class MemoryBitmap final : public IWICBitmap, public Counted {
public:
    MemoryBitmap(uint8_t* data, UINT width, UINT height, UINT stride) : data_(data), width_(width), height_(height), stride_(stride) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IWICBitmapSource) || iid == __uuidof(IWICBitmap)) {
            *out = static_cast<IWICBitmap*>(this);
            addRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return addRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return release(); }
    HRESULT STDMETHODCALLTYPE GetSize(UINT* width, UINT* height) override {
        if (!width || !height) return E_INVALIDARG;
        *width = width_;
        *height = height_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelFormat(WICPixelFormatGUID* format) override {
        if (!format) return E_INVALIDARG;
        *format = wic32bppPBGRA;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetResolution(double* x, double* y) override {
        if (!x || !y) return E_INVALIDARG;
        *x = *y = 96;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CopyPalette(IWICPalette*) override { return WINCODEC_ERR_PALETTEUNAVAILABLE; }
    HRESULT STDMETHODCALLTYPE CopyPixels(const WICRect* rect, UINT stride, UINT size, BYTE* out) override {
        WICRect all = {0, 0, INT(width_), INT(height_)};
        if (!rect) rect = &all;
        if (!out || rect->X < 0 || rect->Y < 0 || rect->Width < 0 || rect->Height < 0 || UINT(rect->X + rect->Width) > width_ ||
            UINT(rect->Y + rect->Height) > height_)
            return E_INVALIDARG;
        if (!rect->Width || !rect->Height) return S_OK;
        const UINT row = UINT(rect->Width) * 4;
        if (stride < row || size < stride * UINT(rect->Height - 1) + row) return E_INVALIDARG;
        for (INT y = 0; y < rect->Height; y++) memcpy(out + size_t(y) * stride, data_ + size_t(rect->Y + y) * stride_ + size_t(rect->X) * 4, row);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Lock(const WICRect* rect, DWORD, IWICBitmapLock** out) override {
        WICRect all = {0, 0, INT(width_), INT(height_)};
        if (!rect) rect = &all;
        if (!out || rect->X < 0 || rect->Y < 0 || rect->Width <= 0 || rect->Height <= 0 || UINT(rect->X + rect->Width) > width_ ||
            UINT(rect->Y + rect->Height) > height_)
            return E_INVALIDARG;
        *out = new (std::nothrow) MemoryLock(data_ + size_t(rect->Y) * stride_ + size_t(rect->X) * 4, UINT(rect->Width), UINT(rect->Height), stride_);
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE SetPalette(IWICPalette*) override { return WINCODEC_ERR_PALETTEUNAVAILABLE; }
    HRESULT STDMETHODCALLTYPE SetResolution(double, double) override { return S_OK; }

private:
    uint8_t* data_;
    UINT width_, height_, stride_;
};

// MARK: Starting

/// A delay-loaded DLL that is not there raises VcppException(
/// ERROR_SEVERITY_ERROR, ERROR_MOD_NOT_FOUND or ERROR_PROC_NOT_FOUND) at the
/// first call into it. (No C++ objects in these frames, for __try.)
bool missingDll(DWORD code) { return code == 0xC06D007E || code == 0xC06D007F; }

HRESULT createDirect2D(ID2D1Factory** out) {
    __try {
        D2D1_FACTORY_OPTIONS options = {};
        return D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory), &options, reinterpret_cast<void**>(out));
    } __except (missingDll(GetExceptionCode()) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    }
}

HRESULT createDirectWrite(IDWriteFactory** out) {
    __try {
        return DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(out));
    } __except (missingDll(GetExceptionCode()) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    }
}

/// WIC's factory, made at the first call that needs one. WIC is COM, and the
/// threads that ask are the app's, which have not initialised COM: once the
/// process's multithreaded apartment is held alive they are in it.
IWICImagingFactory* wic() {
    State& s = G();
    std::lock_guard<std::mutex> lock(s.wicMutex);
    if (s.wicState) return s.wic.get();
    s.wicState = -1;
    CO_MTA_USAGE_COOKIE cookie = nullptr;
    HRESULT hr = CoIncrementMTAUsage(&cookie);
    if (FAILED(hr)) failed("CoIncrementMTAUsage", hr);
    hr = CoCreateInstance(wicFactory2, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), reinterpret_cast<void**>(s.wic.put()));
    if (FAILED(hr)) hr = CoCreateInstance(wicFactory1, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), reinterpret_cast<void**>(s.wic.put()));
    if (FAILED(hr)) {
        s.wic.reset();
        say("the Windows Imaging Component is not to be had (0x%08lX): image files cannot be read or written", static_cast<unsigned long>(hr));
        return nullptr;
    }
    s.wicState = 1;
    return s.wic.get();
}

// MARK: The graphics processor's device, and what is kept on it

/// Whether a failure of Direct2D on the graphics processor is the device
/// going away; that is said once, and from then on everything is drawn in
/// memory (GfxHost.h). Any other failure is said once for its call.
bool gpuFailed(const char* what, HRESULT hr) {
    State& s = G();
    const HRESULT reason = s.d3d ? s.d3d->GetDeviceRemovedReason() : S_OK;
    if (hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || FAILED(reason)) {
        if (!s.gpuLost.exchange(true))
            say("the graphics processor's device is lost (%s: 0x%08lX, the device's reason 0x%08lX): from now on everything is drawn in memory", what,
                static_cast<unsigned long>(hr), static_cast<unsigned long>(reason));
        return true;
    }
    failed(what, hr);
    return false;
}

inline bool gpuGone() { return G().gpuLost.load(std::memory_order_relaxed); }

/// How many bytes of target bitmaps (and the bitmaps read-backs go through)
/// of canvases that are gone are kept for the next ones: a few windows'
/// worth, which is what an app makes again and again.
constexpr uint64_t freeBitmapBytes = 12ull << 20;

/// A bitmap on the graphics processor of this size, B, G, R, A premultiplied,
/// with these options: a target, one the processor's bitmaps are copied into
/// for reading, or one to draw from. Its pixels are not set.
Ref<ID2D1Bitmap1> gpuBitmap(int w, int h, D2D1_BITMAP_OPTIONS options) {
    State& s = G();
    Ref<ID2D1Bitmap1> made;
    if (!s.gpuResource || gpuGone() || w <= 0 || h <= 0 || UINT32(w) > s.gpuMaxSize || UINT32(h) > s.gpuMaxSize) return made;
    const HRESULT hr = s.gpuResource->CreateBitmap(
        D2D1::SizeU(UINT32(w), UINT32(h)), nullptr, 0,
        D2D1::BitmapProperties1(options, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f), made.put());
    if (FAILED(hr)) {
        gpuFailed("ID2D1DeviceContext::CreateBitmap", hr);
        made.reset();
    }
    return made;
}

constexpr D2D1_BITMAP_OPTIONS targetOptions = D2D1_BITMAP_OPTIONS_TARGET;
constexpr D2D1_BITMAP_OPTIONS readableOptions = D2D1_BITMAP_OPTIONS(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW);

/// A bitmap of that size from those kept (`readable`: one to read through,
/// else a target), or a new one.
Ref<ID2D1Bitmap1> takeBitmap(int w, int h, bool readable) {
    State& s = G();
    {
        std::lock_guard<std::mutex> lock(s.gpuMutex);
        std::vector<Ref<ID2D1Bitmap1>>& kept = readable ? s.freeReadables : s.freeTargets;
        for (size_t i = kept.size(); i-- > 0;) {
            const D2D1_SIZE_U size = kept[i]->GetPixelSize();
            if (size.width != UINT32(w) || size.height != UINT32(h)) continue;
            Ref<ID2D1Bitmap1> found = std::move(kept[i]);
            kept.erase(kept.begin() + std::ptrdiff_t(i));
            s.freeBytes -= uint64_t(w) * uint64_t(h) * 4;
            return found;
        }
    }
    return gpuBitmap(w, h, readable ? readableOptions : targetOptions);
}

/// A bitmap a canvas is done with, kept for the next one of its size while
/// the kept ones are few; the oldest go first.
void giveBitmap(Ref<ID2D1Bitmap1>&& bitmap, bool readable) {
    State& s = G();
    if (!bitmap || gpuGone()) return;
    const D2D1_SIZE_U size = bitmap->GetPixelSize();
    const uint64_t bytes = uint64_t(size.width) * size.height * 4;
    if (bytes > freeBitmapBytes / 2) return;
    std::lock_guard<std::mutex> lock(s.gpuMutex);
    (readable ? s.freeReadables : s.freeTargets).push_back(std::move(bitmap));
    s.freeBytes += bytes;
    while (s.freeBytes > freeBitmapBytes) {
        std::vector<Ref<ID2D1Bitmap1>>& from = s.freeTargets.empty() ? s.freeReadables : s.freeReadables.empty() ? s.freeTargets
                                                : s.freeTargets.size() >= s.freeReadables.size() ? s.freeTargets : s.freeReadables;
        const D2D1_SIZE_U gone = from.front()->GetPixelSize();
        s.freeBytes -= uint64_t(gone.width) * gone.height * 4;
        from.erase(from.begin());
    }
}

/// What a canvas draws with on the graphics processor: a device context of
/// its own, with two brushes. One surface of the canvas -- its picture, a
/// layer -- is the context's target at a time, the `owner`; the context
/// stays between BeginDraw and EndDraw while drawing goes from one to
/// another, and ends it (handing what was drawn to the processor) before
/// memory is uploaded or read back.
struct GpuContext {
    Ref<ID2D1DeviceContext> dc;
    Ref<ID2D1SolidColorBrush> brush, second;
    Ref<ID2D1ImageBrush> imageBrush; // a part of a kept bitmap, for draw_bitmap; made at the first
    ID2D1Image* target = nullptr; // dc's target now (the surface holds the reference)
    Surface* owner = nullptr;     // the surface drawing through it now
    bool inDraw = false;          // between BeginDraw and EndDraw

    // Direct2D's effects, which belong to the context that made them: one for each place an effect can have in one
    // call's graph (GfxHost.cpp, "Putting a drawing together on the graphics processor"), made when first needed.
    enum Role {
        coverIn,        // Composite: colours times the coverage
        filterMatrix,   // ColorMatrix: a colour filter that is a sum of products
        filterFlood,    // Flood: the filter's colour, for ...
        filterBlend,    // Blend: ... the three filters that are not
        opacity,        // ColorMatrix: a layer at its alpha
        shadowBlur,     // GaussianBlur: a shadow's coverage blurred
        held,           // Composite DESTINATION_OUT: what is left of the picture where the coverage takes it away
        modeComposite,  // Composite: a transfer mode of Porter and Duff's
        modeArithmetic, // ArithmeticComposite: MODULATE
        modeBlend,      // Blend: SCREEN, OVERLAY, DARKEN, LIGHTEN
        sum,            // Composite PLUS: the two added
        roles
    };
    Ref<ID2D1Effect> effects[roles];
    ID2D1Effect* effect(Role role);

    /// Nothing drawing through it, and what was drawn handed over.
    void finish();
};

ID2D1Effect* GpuContext::effect(Role role) {
    Ref<ID2D1Effect>& e = effects[role];
    if (e) return e.get();
    const GUID* id = &effectComposite;
    switch (role) {
    case filterMatrix:
    case opacity: id = &effectColorMatrix; break;
    case filterFlood: id = &effectFlood; break;
    case filterBlend:
    case modeBlend: id = &effectBlend; break;
    case shadowBlur: id = &effectGaussianBlur; break;
    case modeArithmetic: id = &effectArithmetic; break;
    default: break;
    }
    const HRESULT hr = dc->CreateEffect(*id, e.put());
    if (FAILED(hr)) {
        gpuFailed("ID2D1DeviceContext::CreateEffect", hr);
        e.reset();
    }
    return e.get();
}

/// A context from those kept, or a new one.
GpuContext* takeContext() {
    State& s = G();
    {
        std::lock_guard<std::mutex> lock(s.gpuMutex);
        if (!s.freeContexts.empty()) {
            GpuContext* c = s.freeContexts.back();
            s.freeContexts.pop_back();
            return c;
        }
    }
    if (!s.d2dDevice || gpuGone()) return nullptr;
    std::unique_ptr<GpuContext> c(new GpuContext());
    HRESULT hr = s.d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, c->dc.put());
    if (SUCCEEDED(hr)) hr = c->dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), c->brush.put());
    if (SUCCEEDED(hr)) hr = c->dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), c->second.put());
    if (FAILED(hr)) {
        gpuFailed("ID2D1Device::CreateDeviceContext", hr);
        return nullptr;
    }
    // Text in grey levels, never ClearType (megler.h): the bitmap may be scaled or blended afterwards.
    c->dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    return c.release();
}

/// A context a canvas is done with: kept, a few of them.
void giveContext(GpuContext* c) {
    if (!c) return;
    State& s = G();
    if (!gpuGone()) {
        c->finish();
        c->dc->SetTarget(nullptr);
        c->target = nullptr;
        // (The effects let go of the canvas's bitmaps they were last given.)
        for (Ref<ID2D1Effect>& e : c->effects)
            if (e)
                for (UINT32 i = 0, n = e->GetInputCount(); i < n; i++) e->SetInput(i, nullptr);
        std::lock_guard<std::mutex> lock(s.gpuMutex);
        if (s.freeContexts.size() < 8) {
            s.freeContexts.push_back(c);
            return;
        }
    }
    delete c;
}

// MARK: A surface: memory and the render target that draws into it

struct Clip {
    Box box;               // whole pixels of the canvas; nothing outside it can be drawn on
    Ref<ID2D1Geometry> g;  // in the canvas's pixels: what of the box is the clip. Null: all of it
    bool smooth = false;   // g's edge is smoothed
    uint64_t serial = 0;   // another number whenever the clip is another
};

struct Surface {
    uint8_t* px = nullptr; // B, G, R, A premultiplied, top row first
    int w = 0, h = 0, stride = 0;
    int ox = 0, oy = 0;    // which pixel of the canvas its first one is
    std::vector<uint32_t> own; // the memory, when it is this file's (a layer, a scratch)
    Ref<ID2D1RenderTarget> rt;
    Ref<ID2D1SolidColorBrush> brush, second;
    Ref<IWICBitmap> mirror; // when canvases are copied in and out: what Direct2D draws into
    bool drawing = false;   // between BeginDraw and EndDraw
    int pushed = 0;         // on the render target now: 0 nothing, 1 a rectangle clip, 2 that and a layer with a mask
    uint64_t pushedSerial = 0;
    bool clipApplied = false;
    // On the graphics processor (GfxHost.h) the picture is `gpu`, drawn through the canvas's context, and rt is that
    // context. The memory above -- the core's for a canvas's picture, this file's for a layer, made when first needed
    // -- is kept one picture with it by two boxes of the canvas's pixels, of which one at most is ever not empty:
    GpuContext* gc = nullptr;   // null: a surface in memory
    Ref<ID2D1Bitmap1> gpu;
    Ref<ID2D1Bitmap1> readable; // what read-backs go through, taken at the first
    Box gpuAhead;               // drawn on the processor and not yet read back into the memory
    Box memAhead;               // in the memory and not yet uploaded
    bool window = false;        // a window's picture, shown without the memory (present): its read-backs are counted apart

    Box bounds() const {
        Box b;
        b.l = ox;
        b.t = oy;
        b.r = ox + w;
        b.b = oy + h;
        return b;
    }
    uint32_t* row(int canvasY) const { return reinterpret_cast<uint32_t*>(px + size_t(canvasY - oy) * size_t(stride)); }

    /// Its own memory given back, and the render target with it.
    void release() {
        end();
        rt.reset();
        brush.reset();
        second.reset();
        mirror.reset();
        std::vector<uint32_t>().swap(own);
        px = nullptr;
        w = h = stride = 0;
    }

    /// A surface on the graphics processor covering `box` of the canvas,
    /// drawn through `context`, cleared to nothing: a layer. Its memory is
    /// made when something has to be put on in memory.
    bool allocateGpu(const Box& box, GpuContext* context) {
        if (box.empty() || !context) return false;
        gpu = takeBitmap(box.width(), box.height(), false);
        if (!gpu) return false;
        gc = context;
        rt = Ref<ID2D1RenderTarget>(context->dc);
        brush = context->brush;
        second = context->second;
        px = nullptr;
        std::vector<uint32_t>().swap(own);
        w = box.width();
        h = box.height();
        stride = w * 4;
        ox = box.l;
        oy = box.t;
        gpuAhead = memAhead = Box();
        if (!begin()) return false;
        rt->Clear(D2D1::ColorF(0, 0, 0, 0));
        return true;
    }

    /// The processor's bitmaps handed back to be kept for the next canvas --
    /// after what the context has drawn with them is handed to the
    /// processor, since the next canvas's context may draw into them first.
    void giveBackGpu() {
        if (!gc) return;
        gc->finish();
        unbind();
        giveBitmap(std::move(gpu), false);
        giveBitmap(std::move(readable), true);
        forgetGpu();
    }

    /// The context's target is not this surface's bitmap any more, which is
    /// to be let go of: another context may take it, or another bitmap be
    /// made where it was.
    void unbind() {
        if (!gc || !gpu || gc->target != gpu.get()) return;
        gc->finish();
        gc->dc->SetTarget(nullptr);
        gc->target = nullptr;
    }

    /// Nothing of the processor's any more (the device is gone, or its
    /// bitmaps were handed back): a surface in memory from here on.
    void forgetGpu() {
        if (gc && gc->owner == this) gc->owner = nullptr;
        if (gc && gpu && gc->target == gpu.get()) gc->target = nullptr;
        gc = nullptr;
        gpu.reset();
        readable.reset();
        rt.reset();
        brush.reset();
        second.reset();
        drawing = false;
        pushed = 0;
        pushedSerial = 0;
        clipApplied = false;
        gpuAhead = memAhead = Box();
    }

    /// The memory, made (transparent, as a new layer's picture is) if this
    /// file has to make it.
    bool memory() {
        if (px) return true;
        if (w <= 0 || h <= 0) return false;
        own.assign(size_t(w) * size_t(h), 0);
        px = reinterpret_cast<uint8_t*>(own.data());
        stride = w * 4;
        return true;
    }

    /// `box` of the memory, as it is, onto the processor's picture.
    void upload(Box box) {
        box.intersect(bounds());
        if (box.empty() || !px || !gpu || gpuGone()) return;
        gc->finish();
        const uint64_t from = ticksNow();
        const D2D1_RECT_U rect = D2D1::RectU(UINT32(box.l - ox), UINT32(box.t - oy), UINT32(box.r - ox), UINT32(box.b - oy));
        const HRESULT hr = gpu->CopyFromMemory(&rect, px + size_t(box.t - oy) * size_t(stride) + size_t(box.l - ox) * 4, UINT32(stride));
        G().uploadTicks += ticksNow() - from;
        ++G().uploads;
        G().uploadPixels += uint64_t(box.width()) * uint64_t(box.height());
        if (FAILED(hr)) gpuFailed("ID2D1Bitmap::CopyFromMemory", hr);
    }

    /// The memory has in `want` what the processor has drawn there: what it
    /// has drawn since the last read-back is read back, all of it.
    bool pull(const Box& want) {
        if (!gc) return true;
        if (!memory()) return false;
        Box need = gpuAhead;
        need.intersect(want);
        if (need.empty()) return true;
        Box box = gpuAhead;
        box.intersect(bounds());
        gpuAhead = Box();
        if (box.empty()) return true;
        if (gpuGone()) return false;
        gc->finish();
        const uint64_t from = ticksNow();
        struct Timed {
            uint64_t from;
            bool window;
            ~Timed() {
                const uint64_t ticks = ticksNow() - from;
                G().readBackTicks += ticks;
                if (window) G().windowReadBackTicks += ticks;
            }
        } timed{from, window};
        ++G().readBacks;
        G().readBackPixels += uint64_t(box.width()) * uint64_t(box.height());
        if (window) {
            ++G().windowReadBacks;
            G().windowReadBackPixels += uint64_t(box.width()) * uint64_t(box.height());
        }
        if (!readable) readable = takeBitmap(w, h, true);
        if (!readable) return false;
        const D2D1_POINT_2U at = D2D1::Point2U(UINT32(box.l - ox), UINT32(box.t - oy));
        const D2D1_RECT_U part = D2D1::RectU(UINT32(box.l - ox), UINT32(box.t - oy), UINT32(box.r - ox), UINT32(box.b - oy));
        HRESULT hr = readable->CopyFromBitmap(&at, gpu.get(), &part);
        if (FAILED(hr)) {
            gpuFailed("ID2D1Bitmap::CopyFromBitmap", hr);
            return false;
        }
        // (Mapping waits for the processor to have drawn everything before it: that wait is most of what a read-back costs.)
        D2D1_MAPPED_RECT mapped = {};
        hr = readable->Map(D2D1_MAP_OPTIONS_READ, &mapped);
        if (FAILED(hr) || !mapped.bits) {
            gpuFailed("ID2D1Bitmap1::Map", hr);
            return false;
        }
        const size_t bytes = size_t(box.width()) * 4;
        for (int y = box.t; y < box.b; y++)
            memcpy(row(y) + (box.l - ox), mapped.bits + size_t(y - oy) * mapped.pitch + size_t(box.l - ox) * 4, bytes);
        readable->Unmap();
        return true;
    }

    /// Drawn on the processor in `box` (of the canvas's pixels): to be read
    /// back at the next flush.
    void drew(Box box) {
        if (!gc) return;
        box.intersect(bounds());
        gpuAhead.include(box);
    }

    /// The context is to draw into another surface: this one's clip is
    /// taken off it.
    void detach() {
        if (!drawing) return;
        popClip();
        drawing = false;
        if (gc && gc->owner == this) gc->owner = nullptr;
    }

    bool beginGpu() {
        if (drawing) return true;
        if (!gpu || gpuGone()) return false;
        if (gc->owner && gc->owner != this) gc->owner->detach();
        // What the memory has goes up before anything is drawn over it.
        if (!memAhead.empty()) {
            upload(memAhead);
            memAhead = Box();
        }
        if (gc->target != gpu.get()) {
            gc->dc->SetTarget(gpu.get());
            gc->target = gpu.get();
        }
        if (!gc->inDraw) {
            gc->dc->BeginDraw();
            gc->inDraw = true;
            ++G().sessions;
        }
        gc->owner = this;
        drawing = true;
        pushed = 0;
        pushedSerial = 0;
        clipApplied = false;
        return true;
    }

    /// Memory of its own, all transparent, covering `box` of the canvas.
    bool allocate(const Box& box) {
        if (box.empty()) return false;
        if (box.width() != w || box.height() != h || own.empty()) {
            end();
            rt.reset();
            brush.reset();
            second.reset();
            mirror.reset();
            own.assign(size_t(box.width()) * size_t(box.height()), 0);
        } else {
            end();
            std::fill(own.begin(), own.end(), 0u);
        }
        px = reinterpret_cast<uint8_t*>(own.data());
        w = box.width();
        h = box.height();
        stride = w * 4;
        ox = box.l;
        oy = box.t;
        return true;
    }

    bool make() {
        State& s = G();
        if (gc) return gpu && rt && !gpuGone();
        if (rt) return true;
        if (!px || w <= 0 || h <= 0) return false;
        const uint64_t from = ticksNow();
        struct Timed {
            uint64_t from;
            ~Timed() {
                G().makeTicks += ticksNow() - from;
                ++G().targetsMade;
            }
        } timed{from};
        Ref<IWICBitmap> bitmap;
        if (s.copying) {
            IWICImagingFactory* factory = wic();
            if (!factory) return false;
            const HRESULT hr = factory->CreateBitmap(UINT(w), UINT(h), wic32bppPBGRA, WICBitmapCacheOnLoad, mirror.put());
            if (FAILED(hr)) {
                failed("IWICImagingFactory::CreateBitmap", hr);
                return false;
            }
            bitmap = mirror;
        } else {
            bitmap.attach(new (std::nothrow) MemoryBitmap(px, UINT(w), UINT(h), UINT(stride)));
            if (!bitmap) return false;
        }
        const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        HRESULT hr = s.d2d->CreateWicBitmapRenderTarget(bitmap.get(), &properties, rt.put());
        if (FAILED(hr)) {
            failed("ID2D1Factory::CreateWicBitmapRenderTarget", hr);
            rt.reset();
            mirror.reset();
            return false;
        }
        hr = rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), brush.put());
        if (SUCCEEDED(hr)) hr = rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), second.put());
        if (FAILED(hr)) {
            failed("ID2D1RenderTarget::CreateSolidColorBrush", hr);
            rt.reset();
            mirror.reset();
            return false;
        }
        return true;
    }

    /// The mirror's memory from the surface's (`in`), or back.
    void copyMirror(bool in) {
        Ref<IWICBitmapLock> lock;
        WICRect all = {0, 0, w, h};
        UINT size = 0, pitch = 0;
        BYTE* data = nullptr;
        HRESULT hr = mirror->Lock(&all, in ? WICBitmapLockWrite : WICBitmapLockRead, lock.put());
        if (SUCCEEDED(hr)) hr = lock->GetStride(&pitch);
        if (SUCCEEDED(hr)) hr = lock->GetDataPointer(&size, &data);
        if (FAILED(hr) || !data) {
            failed("IWICBitmap::Lock", hr);
            return;
        }
        for (int y = 0; y < h; y++) {
            if (in) memcpy(data + size_t(y) * pitch, px + size_t(y) * size_t(stride), size_t(w) * 4);
            else memcpy(px + size_t(y) * size_t(stride), data + size_t(y) * pitch, size_t(w) * 4);
        }
    }

    /// Ready to be drawn on: false when Direct2D cannot.
    bool begin() {
        if (gc) return beginGpu();
        if (drawing) return true;
        if (!make()) return false;
        if (mirror) copyMirror(true);
        rt->BeginDraw();
        G().sessionPixels += uint64_t(w) * uint64_t(h);
        drawing = true;
        pushed = 0;
        pushedSerial = 0;
        clipApplied = false;
        ++G().sessions;
        return true;
    }

    void popClip() {
        if (pushed == 2) rt->PopLayer();
        if (pushed >= 1) rt->PopAxisAlignedClip();
        pushed = 0;
        pushedSerial = 0;
        clipApplied = false;
    }

    /// Everything drawn so far is in the memory when this returns -- or, on
    /// the graphics processor, handed to it: pull() reads it back.
    void end() {
        if (gc) {
            if (drawing) gc->finish();
            return;
        }
        if (!drawing) return;
        popClip();
        const uint64_t from = ticksNow();
        const HRESULT hr = rt->EndDraw();
        G().endTicks += ticksNow() - from;
        drawing = false;
        if (mirror && SUCCEEDED(hr)) copyMirror(false);
        if (FAILED(hr)) {
            // A target that reports a failure is made again at its next drawing.
            failed("ID2D1RenderTarget::EndDraw", hr);
            rt.reset();
            brush.reset();
            second.reset();
            mirror.reset();
        }
    }

    /// The clip for what is drawn next (the surface is drawing).
    void useClip(const Clip& clip) {
        if (clipApplied && pushedSerial == clip.serial) return;
        popClip();
        pushedSerial = clip.serial;
        clipApplied = true;
        const Box mine = bounds();
        if (!clip.g && clip.box.l <= mine.l && clip.box.t <= mine.t && clip.box.r >= mine.r && clip.box.b >= mine.b) return;
        const D2D1_RECT_F rect = D2D1::RectF(float(clip.box.l - ox), float(clip.box.t - oy), float(clip.box.r - ox), float(clip.box.b - oy));
        rt->SetTransform(Matrix3x2F::Identity());
        rt->PushAxisAlignedClip(rect, D2D1_ANTIALIAS_MODE_ALIASED);
        pushed = 1;
        if (clip.g) {
            // (No layer object: Direct2D keeps its own since Windows 8.)
            rt->PushLayer(D2D1::LayerParameters(rect, clip.g.get(), clip.smooth ? D2D1_ANTIALIAS_MODE_PER_PRIMITIVE : D2D1_ANTIALIAS_MODE_ALIASED,
                                                Matrix3x2F::Translation(float(-ox), float(-oy))),
                          nullptr);
            pushed = 2;
        }
    }

    /// The matrix for what is drawn next: `m` takes the drawing's coordinates to the canvas's pixels.
    void setMatrix(const Matrix3x2F& m) { rt->SetTransform(m * Matrix3x2F::Translation(float(-ox), float(-oy))); }

    /// Only `box` (of the canvas's pixels) can be drawn on, whatever the
    /// canvas's clip, which is in what is drawn already (the surface is
    /// drawing). The next useClip puts the canvas's clip back.
    void useBoxClip(const Box& box) {
        popClip();
        rt->SetTransform(Matrix3x2F::Identity());
        rt->PushAxisAlignedClip(D2D1::RectF(float(box.l - ox), float(box.t - oy), float(box.r - ox), float(box.b - oy)), D2D1_ANTIALIAS_MODE_ALIASED);
        pushed = 1;
        pushedSerial = 0;
        clipApplied = false;
    }
};

void GpuContext::finish() {
    if (owner) owner->detach();
    if (!inDraw) return;
    inDraw = false;
    const uint64_t from = ticksNow();
    const HRESULT hr = dc->EndDraw();
    G().endTicks += ticksNow() - from;
    if (FAILED(hr)) gpuFailed("ID2D1DeviceContext::EndDraw", hr);
}

/// Direct2D and DirectWrite, at the first call from the core. False when the
/// device has not got them, or Direct2D cannot draw into memory.
bool start();

inline bool started() {
    const int state = G().started.load(std::memory_order_acquire);
    return state > 0 || (state == 0 && start());
}

// MARK: Shapes Direct2D keeps

/// A path as megler.h's draw_path has it, as a geometry. A stream that is not
/// well formed ends where it goes wrong; a figure with nothing after its
/// MOVE is left out. Null when nothing is in it.
Ref<ID2D1PathGeometry> pathGeometry(const float* path, int count, int fill) {
    Ref<ID2D1PathGeometry> geometry;
    Ref<ID2D1GeometrySink> sink;
    if (!path || count <= 0) return geometry;
    if (FAILED(G().d2d->CreatePathGeometry(geometry.put())) || FAILED(geometry->Open(sink.put()))) {
        geometry.reset();
        return geometry;
    }
    sink->SetFillMode(fill == MEGLER_GFX_EVEN_ODD ? D2D1_FILL_MODE_ALTERNATE : D2D1_FILL_MODE_WINDING);
    D2D1_POINT_2F start = D2D1::Point2F(0, 0);
    bool moved = false, open = false, any = false;
    int i = 0;
    while (i < count) {
        if (!(path[i] >= 0 && path[i] <= 4)) break;
        const int verb = int(path[i++]);
        const int need = verb == MEGLER_GFX_PATH_QUAD ? 4 : verb == MEGLER_GFX_PATH_CUBIC ? 6 : verb == MEGLER_GFX_PATH_CLOSE ? 0 : 2;
        if (i + need > count) break;
        // (Numbers, and none so large that arithmetic on it stops being one: see sane().)
        bool finite = true;
        for (int k = 0; k < need; k++) finite = finite && std::isfinite(path[i + k]) && std::fabs(path[i + k]) < 1.0e9f;
        if (!finite) break;
        if (verb == MEGLER_GFX_PATH_MOVE) {
            if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
            open = false;
            moved = true;
            start = D2D1::Point2F(path[i], path[i + 1]);
        } else if (verb == MEGLER_GFX_PATH_CLOSE) {
            if (open) sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            open = false;
            moved = false;
        } else {
            if (!open) {
                // (A stream that begins without a MOVE begins at the origin, as Android's Path does.)
                sink->BeginFigure(moved ? start : D2D1::Point2F(0, 0), D2D1_FIGURE_BEGIN_FILLED);
                open = true;
                any = true;
            }
            if (verb == MEGLER_GFX_PATH_LINE) {
                sink->AddLine(D2D1::Point2F(path[i], path[i + 1]));
            } else if (verb == MEGLER_GFX_PATH_QUAD) {
                sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(D2D1::Point2F(path[i], path[i + 1]), D2D1::Point2F(path[i + 2], path[i + 3])));
            } else {
                sink->AddBezier(D2D1::BezierSegment(D2D1::Point2F(path[i], path[i + 1]), D2D1::Point2F(path[i + 2], path[i + 3]),
                                                    D2D1::Point2F(path[i + 4], path[i + 5])));
            }
        }
        i += need;
    }
    if (open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close()) || !any) geometry.reset();
    return geometry;
}

/// A stroke's caps, joins and miter limit as Direct2D keeps them. Android's
/// miter limit and Direct2D's are the same number: with "miter or bevel" a
/// corner is bevelled when its miter would be longer than the limit times
/// the width (seen: with 4, between corners of 29.2 and 28.8 degrees).
/// `hairline`: a pixel wide whatever the matrix, which needs ID2D1Factory1.
Ref<ID2D1StrokeStyle> strokeStyle(int cap, int join, float miterLimit, bool hairline) {
    State& s = G();
    uint32_t miterBits;
    memcpy(&miterBits, &miterLimit, 4);
    const uint64_t key = uint64_t(miterBits) << 8 | uint64_t(cap & 3) << 4 | uint64_t(join & 3) << 2 | (hairline ? 1 : 0);
    std::lock_guard<std::mutex> lock(s.strokeMutex);
    auto found = s.strokes.find(key);
    if (found != s.strokes.end()) return found->second;
    const D2D1_CAP_STYLE capStyle = cap == MEGLER_GFX_CAP_ROUND ? D2D1_CAP_STYLE_ROUND : cap == MEGLER_GFX_CAP_SQUARE ? D2D1_CAP_STYLE_SQUARE : D2D1_CAP_STYLE_FLAT;
    const D2D1_LINE_JOIN joinStyle =
        join == MEGLER_GFX_JOIN_ROUND ? D2D1_LINE_JOIN_ROUND : join == MEGLER_GFX_JOIN_BEVEL ? D2D1_LINE_JOIN_BEVEL : D2D1_LINE_JOIN_MITER_OR_BEVEL;
    Ref<ID2D1StrokeStyle> style;
    HRESULT hr;
    if (hairline && s.d2d1) {
        D2D1_STROKE_STYLE_PROPERTIES1 properties = {};
        properties.startCap = properties.endCap = properties.dashCap = capStyle;
        properties.lineJoin = joinStyle;
        properties.miterLimit = miterLimit;
        properties.dashStyle = D2D1_DASH_STYLE_SOLID;
        properties.transformType = D2D1_STROKE_TRANSFORM_TYPE_HAIRLINE;
        Ref<ID2D1StrokeStyle1> style1;
        hr = s.d2d1->CreateStrokeStyle(&properties, nullptr, 0, style1.put());
        if (SUCCEEDED(hr)) style = Ref<ID2D1StrokeStyle>(style1);
    } else {
        hr = s.d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(capStyle, capStyle, capStyle, joinStyle, miterLimit), nullptr, 0, style.put());
    }
    if (FAILED(hr)) {
        failed("ID2D1Factory::CreateStrokeStyle", hr);
        style.reset();
        return style;
    }
    // (An app can ask for any miter limit: the table is not let grow for ever. A style in use is held by who uses it.)
    if (s.strokes.size() >= 256) s.strokes.clear();
    s.strokes.emplace(key, style);
    return style;
}

// MARK: Shaders

constexpr uint32_t canvasMagic = 0x31764347, shaderMagic = 0x31687347, faceMagic = 0x31667447;

struct Shader {
    uint32_t magic = shaderMagic;
    enum Kind { linear, radial, sweep, bitmap } kind = linear;
    // The brushes Direct2D fills with (not for a sweep), [0] for surfaces in
    // memory and [1] for those on the graphics processor, each made when it
    // is first drawn with (brushFor): their matrix, opacity and sampling are
    // set for each drawing, under `mutex`.
    std::mutex mutex;
    Ref<ID2D1Brush> brushes[2];
    Ref<ID2D1BitmapBrush> bitmapBrushes[2];
    Matrix3x2F local = Matrix3x2F::Identity(); // shader coordinates to the drawing's
    bool opaque = false;                       // every colour it can give has alpha 255
    // What it was made of. A gradient: the line from `from` to `to`, or the circle round `from`, and the stops.
    D2D1_POINT_2F from = {}, to = {};
    float radius = 0;
    std::vector<D2D1_GRADIENT_STOP> stops;
    D2D1_EXTEND_MODE extend = D2D1_EXTEND_MODE_CLAMP;
    // A bitmap: its pixels as they were when it was made (the graphics processor's brush is the kept bitmap's).
    std::vector<uint32_t> pixels;
    int width = 0, height = 0;
    // A sweep: where its centre is, and its colours at 1024 angles, premultiplied.
    float cx = 0, cy = 0;
    std::vector<uint32_t> table;

    /// The brush for a surface on the graphics processor or in memory, made
    /// now if it has not been. Null when it cannot be. Under `mutex`.
    ID2D1Brush* brushFor(bool onGpu);
    ID2D1BitmapBrush* bitmapBrushFor(bool onGpu) {
        brushFor(onGpu);
        return bitmapBrushes[onGpu ? 1 : 0].get();
    }
};

Shader* shaderOf(void* handle) {
    Shader* s = static_cast<Shader*>(handle);
    return s && s->magic == shaderMagic ? s : nullptr;
}

/// The colour of a gradient at t in 0..1: between two stops interpolated as
/// it stands, not premultiplied (the stops' positions never go down).
uint32_t gradientAt(const uint32_t* colors, const float* positions, int count, float t) {
    if (count <= 0) return 0;
    if (count == 1 || t <= positions[0]) return colors[0];
    for (int i = 1; i < count; i++) {
        if (t <= positions[i]) {
            const float span = positions[i] - positions[i - 1], k = span > 0 ? (t - positions[i - 1]) / span : 1;
            uint32_t out = 0;
            for (int shift = 0; shift < 32; shift += 8) {
                const float a = float((colors[i - 1] >> shift) & 255), b = float((colors[i] >> shift) & 255);
                out |= uint32_t(std::lround(a + (b - a) * k)) << shift;
            }
            return out;
        }
    }
    return colors[count - 1];
}

D2D1_EXTEND_MODE extendOf(int tile) {
    return tile == MEGLER_GFX_TILE_REPEAT ? D2D1_EXTEND_MODE_WRAP : tile == MEGLER_GFX_TILE_MIRROR ? D2D1_EXTEND_MODE_MIRROR : D2D1_EXTEND_MODE_CLAMP;
}

/// A gradient's stops as Direct2D takes them, and whether every colour of
/// them is opaque.
std::vector<D2D1_GRADIENT_STOP> stopsOf(const uint32_t* colors, const float* positions, int count, bool* opaque) {
    std::vector<D2D1_GRADIENT_STOP> stops(static_cast<size_t>(count));
    *opaque = true;
    for (int i = 0; i < count; i++) {
        float at = positions ? positions[i] : count > 1 ? float(i) / float(count - 1) : 0;
        if (!(at >= 0)) at = 0;
        if (at > 1) at = 1;
        stops[size_t(i)].position = at;
        stops[size_t(i)].color = colorOf(colors[i]);
        if ((colors[i] >> 24) != 255) *opaque = false;
    }
    return stops;
}

/// What makes the brushes and bitmaps several surfaces share: the render
/// target of one pixel for those in memory (software render targets of one
/// factory share what they make), the graphics processor's resource context
/// for the others (a device's contexts share what they make).
ID2D1RenderTarget* resourceMaker(bool onGpu) {
    State& s = G();
    if (onGpu) return s.gpuResource && !gpuGone() ? s.gpuResource.get() : nullptr;
    return s.resource ? s.resource->rt.get() : nullptr;
}

Ref<ID2D1Bitmap1> keptBitmap(const megler_gfx_image& image);

ID2D1Brush* Shader::brushFor(bool onGpu) {
    const int k = onGpu ? 1 : 0;
    if (brushes[k] || kind == sweep) return brushes[k].get();
    ID2D1RenderTarget* maker = resourceMaker(onGpu);
    if (!maker) return nullptr;
    HRESULT hr = S_OK;
    if (kind == bitmap) {
        Ref<ID2D1Bitmap> image;
        const D2D1_BITMAP_PROPERTIES properties = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        hr = maker->CreateBitmap(D2D1::SizeU(UINT32(width), UINT32(height)), pixels.data(), UINT32(width) * 4, properties, image.put());
        if (SUCCEEDED(hr))
            hr = maker->CreateBitmapBrush(image.get(), D2D1::BitmapBrushProperties(extend, extend, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR),
                                          bitmapBrushes[k].put());
        if (SUCCEEDED(hr)) brushes[k] = Ref<ID2D1Brush>(bitmapBrushes[k]);
    } else {
        Ref<ID2D1GradientStopCollection> collection;
        // Colours as they are (gamma 2.2 is "no conversion"), interpolated straight and premultiplied afterwards, which is what
        // ID2D1RenderTarget's collection does.
        hr = maker->CreateGradientStopCollection(stops.data(), UINT32(stops.size()), D2D1_GAMMA_2_2, extend, collection.put());
        if (SUCCEEDED(hr) && kind == linear) {
            Ref<ID2D1LinearGradientBrush> brush;
            hr = maker->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(from, to), collection.get(), brush.put());
            if (SUCCEEDED(hr)) brushes[k] = Ref<ID2D1Brush>(brush);
        } else if (SUCCEEDED(hr)) {
            Ref<ID2D1RadialGradientBrush> brush;
            hr = maker->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(from, D2D1::Point2F(0, 0), radius, radius), collection.get(), brush.put());
            if (SUCCEEDED(hr)) brushes[k] = Ref<ID2D1Brush>(brush);
        }
    }
    if (FAILED(hr)) {
        if (onGpu) gpuFailed("making a shader's brush", hr);
        else failed("making a shader's brush", hr);
        brushes[k].reset();
        bitmapBrushes[k].reset();
    }
    return brushes[k].get();
}

// MARK: Canvases

/// How many pixels of a canvas's own memory are kept from one drawing to the
/// next (a megabyte's worth).
constexpr size_t scratchKept = 256 * 1024;

/// One save: the clip it will bring back, and the layer it began, if it did.
struct Level {
    Clip clip;
    bool isLayer = false;
    std::unique_ptr<Surface> layer; // the layer's picture; null for a layer with no extent
    int alpha = 255, blend = MEGLER_GFX_BLEND_SRC_OVER;
};

/// A render target made over one part of a canvas's memory, kept for the
/// next time that part is drawn on (Canvas::fitBase).
struct PartTarget {
    Box box;
    Ref<ID2D1RenderTarget> rt;
    Ref<ID2D1SolidColorBrush> brush, second;
    Ref<IWICBitmap> mirror;
};

/// How many such targets a canvas keeps: the window's few dirty rectangles
/// that come back frame after frame (a spinner, a caret, a list's row).
constexpr size_t partTargetsKept = 4;

struct Canvas {
    uint32_t magic = canvasMagic;
    uint64_t serial = G().canvasSerial.fetch_add(1); // its own, never another's
    Surface base;                  // the memory, or the part of it the clip leaves open (fitBase); on the graphics processor all of it
    uint8_t* memory = nullptr;     // the canvas's memory as a whole
    int memoryW = 0, memoryH = 0, memoryStride = 0;
    GpuContext* gc = nullptr;      // on the graphics processor: what base and the layers are drawn through
    std::vector<PartTarget> parts; // render targets made over parts of it before, newest last
    std::vector<Level> levels;     // levels.back() is the state in force; levels[0] is the canvas's own
    Matrix3x2F matrix = Matrix3x2F::Identity();
    Surface cover, source, extra;  // memory of the canvas's own that a drawing call is put together in
    std::vector<uint8_t> blurA, blurB;
    std::vector<uint32_t> tint;    // a bitmap's pixels as a paint's alpha and colour filter make them
    // On the graphics processor, what a drawing call is put together in there (workSurface): its colours times its
    // coverage, the coverage alone, colours or a shadow's coverage before they are cut, and a copy of the picture.
    enum { workA, workC, workS, workD, works };
    Surface work[works];

    Clip& clip() { return levels.back().clip; }

    /// What is drawn into now: the innermost layer's picture, or the bitmap.
    /// Null inside a layer that has no extent.
    Surface* target() {
        for (size_t i = levels.size(); i-- > 0;)
            if (levels[i].isLayer) return levels[i].layer.get();
        if (!base.gc) fitBase();
        return &base;
    }

    /// The graphics processor's device is gone: the canvas is drawn in
    /// memory from now on. Its memory has what the last flush put there; an
    /// open layer keeps what was read back of it, which is nothing unless
    /// something had to be put on in memory.
    void leaveGpu() {
        if (!gc) return;
        for (Level& level : levels)
            if (level.layer && level.layer->gc) {
                level.layer->memory();
                level.layer->forgetGpu();
            }
        for (Surface& s : work) s.forgetGpu();
        base.forgetGpu();
        base.px = memory;
        base.w = memoryW;
        base.h = memoryH;
        base.stride = memoryStride;
        base.ox = base.oy = 0;
        delete gc;
        gc = nullptr;
    }

    /// The bitmap's render target made over the part of the memory the clip
    /// leaves open, when that is a small part of a large bitmap. A software
    /// render target's memory is copied in and out whole at every BeginDraw
    /// and EndDraw, whatever little is drawn: 0.9 ms for a 720x1280 window on
    /// the phone, a dozen milliseconds a frame for a window that redraws a
    /// small part of itself. So base covers what the clip leaves open; a
    /// drawing whose clip goes beyond it ends the drawing so far and moves
    /// base, and a new drawing on a canvas starts from what its clip asks.
    void fitBase() {
        if (!memory) return;
        Box full;
        full.r = memoryW;
        full.b = memoryH;
        Box want = clip().box;
        want.intersect(full);
        if (want.empty()) return;
        const Box now = base.bounds();
        const bool covers = want.l >= now.l && want.t >= now.t && want.r <= now.r && want.b <= now.b;
        // Small bitmaps, and clips that leave most of one open: the whole of it, as before.
        Box region = want;
        if (int64_t(memoryW) * memoryH < int64_t(256) * 256 || int64_t(region.width()) * region.height() * 2 > int64_t(memoryW) * memoryH) region = full;
        if (covers && (base.drawing || (region.l == now.l && region.t == now.t && region.r == now.r && region.b == now.b))) return;
        moveBase(region);
    }

    void moveBase(const Box& region) {
        base.end();
        // The target over the part it covered is kept; one made over the new part before is taken back.
        if (base.rt) {
            PartTarget kept;
            kept.box = base.bounds();
            kept.rt = std::move(base.rt);
            kept.brush = std::move(base.brush);
            kept.second = std::move(base.second);
            kept.mirror = std::move(base.mirror);
            parts.push_back(std::move(kept));
        }
        base.rt.reset();
        base.brush.reset();
        base.second.reset();
        base.mirror.reset();
        for (size_t i = parts.size(); i-- > 0;) {
            const Box& b = parts[i].box;
            if (b.l != region.l || b.t != region.t || b.r != region.r || b.b != region.b) continue;
            base.rt = std::move(parts[i].rt);
            base.brush = std::move(parts[i].brush);
            base.second = std::move(parts[i].second);
            base.mirror = std::move(parts[i].mirror);
            parts.erase(parts.begin() + std::ptrdiff_t(i));
            break;
        }
        while (parts.size() > partTargetsKept) parts.erase(parts.begin());
        base.px = memory + size_t(region.t) * size_t(memoryStride) + size_t(region.l) * 4;
        base.w = region.width();
        base.h = region.height();
        base.ox = region.l;
        base.oy = region.t;
        ++G().partTargets;
    }
};

Canvas* canvasOf(void* handle) {
    Canvas* c = static_cast<Canvas*>(handle);
    if (!c || c->magic != canvasMagic) return nullptr;
    // (Every call on a canvas comes through here, on the thread that draws it: the place to leave a lost device.)
    if (c->gc && gpuGone()) c->leaveGpu();
    return c;
}

void touchClip(Clip& clip) { clip.serial = G().clipSerial.fetch_add(1); }

Ref<ID2D1Geometry> rectangleGeometry(const Box& box) {
    Ref<ID2D1RectangleGeometry> rect;
    Ref<ID2D1Geometry> out;
    if (SUCCEEDED(G().d2d->CreateRectangleGeometry(D2D1::RectF(float(box.l), float(box.t), float(box.r), float(box.b)), rect.put()))) out = Ref<ID2D1Geometry>(rect);
    return out;
}

Ref<ID2D1Geometry> transformed(ID2D1Geometry* geometry, const Matrix3x2F& m) {
    Ref<ID2D1TransformedGeometry> moved;
    Ref<ID2D1Geometry> out;
    if (geometry && SUCCEEDED(G().d2d->CreateTransformedGeometry(geometry, m, moved.put()))) out = Ref<ID2D1Geometry>(moved);
    return out;
}

/// `a` and `b`, both in the canvas's pixels, combined into one geometry.
Ref<ID2D1Geometry> combined(ID2D1Geometry* a, ID2D1Geometry* b, D2D1_COMBINE_MODE mode) {
    Ref<ID2D1PathGeometry> result;
    Ref<ID2D1GeometrySink> sink;
    Ref<ID2D1Geometry> out;
    HRESULT hr = G().d2d->CreatePathGeometry(result.put());
    if (SUCCEEDED(hr)) hr = result->Open(sink.put());
    if (SUCCEEDED(hr)) hr = a->CombineWithGeometry(b, mode, nullptr, sink.get());
    if (SUCCEEDED(hr)) hr = sink->Close();
    if (FAILED(hr)) {
        failed("ID2D1Geometry::CombineWithGeometry", hr);
        return out;
    }
    out = Ref<ID2D1Geometry>(result);
    return out;
}

/// A shape, in the canvas's pixels, taken into the clip: what is inside both
/// stays (INTERSECT), or the shape is cut out (DIFFERENCE). `cover` is the
/// pixels the shape can touch.
void clipByGeometry(Clip& clip, ID2D1Geometry* shape, const Box& cover, int op, bool smooth) {
    if (clip.box.empty()) return;
    if (op == MEGLER_GFX_CLIP_INTERSECT) {
        if (!shape) {
            // Nothing is inside a shape with nothing in it.
            clip.box.r = clip.box.l;
        } else {
            clip.box.intersect(cover);
            if (clip.g) {
                Ref<ID2D1Geometry> both = combined(clip.g.get(), shape, D2D1_COMBINE_MODE_INTERSECT);
                if (both) clip.g = both;
            } else {
                shape->AddRef();
                clip.g.attach(shape);
            }
            clip.smooth = clip.smooth || smooth;
        }
    } else if (shape) {
        Ref<ID2D1Geometry> from = clip.g ? clip.g : rectangleGeometry(clip.box);
        if (from) {
            Ref<ID2D1Geometry> rest = combined(from.get(), shape, D2D1_COMBINE_MODE_EXCLUDE);
            if (rest) {
                clip.g = rest;
                clip.smooth = clip.smooth || smooth;
            }
        }
    }
    touchClip(clip);
}

// MARK: Putting a drawing onto the picture by this file's own arithmetic

/// Where the colours of a composed drawing come from.
struct Source {
    uint32_t solid = 0;               // premultiplied: the colour, when the two below are null
    const Surface* pixels = nullptr;  // a picture of the canvas's pixels
    const Shader* sweep = nullptr;    // a sweep gradient ...
    Matrix3x2F toShader;              // ... and what takes a pixel's centre to its coordinates
    uint32_t alpha = 255;             // multiplies every colour (a paint's, a layer's)
    int filterMode = -1;              // a colour filter: megler.h, "THE PAINT"
    uint32_t filter = 0;              // premultiplied
};

inline uint32_t sweepAt(const Shader& s, const Matrix3x2F& toShader, int x, int y) {
    const D2D1_POINT_2F p = toShader.TransformPoint(D2D1::Point2F(float(x) + 0.5f, float(y) + 0.5f));
    // Position 0 points along +x and positions grow clockwise as seen on screen, y being down.
    float turn = std::atan2(p.y - s.cy, p.x - s.cx) * 0.15915494f;
    if (turn < 0) turn += 1;
    const int index = std::min(1023, std::max(0, int(turn * 1024.0f)));
    return s.table[size_t(index)];
}

/// `box` of the target gets the source, as far as `cover` says (the alpha
/// byte of each of its pixels is how much of the pixel is covered; null:
/// all of every pixel), by the transfer mode. Both pictures are of the same
/// canvas, and the target is not drawing.
void composeUntimed(Surface& target, Box box, const Source& src, const Surface* cover, int blend);

/// compose, its time and pixels counted for the log's line. A target on the
/// graphics processor is read back where the drawing goes, put together in
/// memory, and uploaded again there.
void compose(Surface& target, Box box, const Source& src, const Surface* cover, int blend) {
    const uint64_t from = ticksNow();
    if (target.gc) {
        box.intersect(target.bounds());
        if (box.empty() || !target.pull(box)) return;
        composeUntimed(target, box, src, cover, blend);
        target.upload(box);
        ++G().throughMemory;
    } else {
        composeUntimed(target, box, src, cover, blend);
    }
    G().composeTicks += ticksNow() - from;
    if (!box.empty()) G().composedPixels += uint64_t(box.width()) * uint64_t(box.height());
}

void composeUntimed(Surface& target, Box box, const Source& src, const Surface* cover, int blend) {
    box.intersect(target.bounds());
    if (cover) box.intersect(cover->bounds());
    if (src.pixels) box.intersect(src.pixels->bounds());
    if (box.empty()) return;
    const bool keeps = keepsUncovered(blend);
    const bool plain = !src.pixels && !src.sweep;
    uint32_t solid = faded(src.solid, src.alpha);
    if (plain && src.filterMode >= 0) solid = blended(src.filterMode, src.filter, solid);
    for (int y = box.t; y < box.b; y++) {
        uint32_t* d = target.row(y) + (box.l - target.ox);
        const uint32_t* c = cover ? cover->row(y) + (box.l - cover->ox) : nullptr;
        const uint32_t* p = src.pixels ? src.pixels->row(y) + (box.l - src.pixels->ox) : nullptr;
        const int n = box.width();
        if (plain && !c) {
            // The whole of every pixel in one colour: drawColor, and the frame's first call.
            if (blend == MEGLER_GFX_BLEND_SRC || blend == MEGLER_GFX_BLEND_CLEAR || (blend == MEGLER_GFX_BLEND_SRC_OVER && (solid >> 24) == 255)) {
                std::fill(d, d + n, blend == MEGLER_GFX_BLEND_CLEAR ? 0u : solid);
            } else {
                for (int x = 0; x < n; x++) d[x] = blended(blend, solid, d[x]);
            }
            continue;
        }
        for (int x = 0; x < n; x++) {
            const uint32_t covered = c ? c[x] >> 24 : 255;
            if (!covered) continue;
            uint32_t s;
            if (plain) {
                s = solid;
            } else {
                s = p ? p[x] : sweepAt(*src.sweep, src.toShader, box.l + x, y);
                // (Most of a layer is often nothing at all, and nothing leaves most modes' pictures as they are.)
                if (!s && keeps && src.filterMode < 0) continue;
                s = faded(s, src.alpha);
                if (src.filterMode >= 0) s = blended(src.filterMode, src.filter, s);
            }
            if (covered == 255) {
                d[x] = blended(blend, s, d[x]);
            } else if (keeps) {
                d[x] = blended(blend, faded(s, covered), d[x]);
            } else {
                d[x] = addPixels(faded(d[x], 255 - covered), blended(blend, faded(s, covered), d[x]));
            }
        }
    }
}

/// How much of each pixel of `box` the clip lets through, when the clip is
/// more than its box: into the canvas's `extra` picture. Null when the clip
/// is its box, which lets every pixel through whole.
const Surface* clipCover(Canvas& cv, const Box& box) {
    Clip& clip = cv.clip();
    if (!clip.g) return nullptr;
    Surface& s = cv.extra;
    if (!s.allocate(box) || !s.begin()) return nullptr;
    s.useClip(clip);
    s.rt->SetTransform(Matrix3x2F::Identity());
    s.rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    s.brush->SetColor(D2D1::ColorF(1, 1, 1, 1));
    s.rt->FillRectangle(D2D1::RectF(0, 0, float(s.w), float(s.h)), s.brush.get());
    s.end();
    return &s;
}

// MARK: What a call draws

struct Shaped;
struct TextRun;

/// The shape of one drawing call, which can be drawn as it is (onto the
/// picture), in white (to learn which pixels it covers), or as a shadow.
struct Ink {
    enum Kind { rectangle, rounded, oval, geometry, glyphs } kind = rectangle;
    D2D1_RECT_F box = {};            // the first three
    float rx = 0, ry = 0;
    ID2D1Geometry* shape = nullptr;  // geometry
    const Shaped* text = nullptr;    // glyphs: a run laid out ...
    Matrix3x2F textMatrix;           // ... and what takes its own coordinates (the baseline's start at 0, 0) to the drawing's
    int style = MEGLER_GFX_FILL;
    float strokeWidth = 0;           // 0: a hairline
    Ref<ID2D1StrokeStyle> stroke;
    float reach = 0;                 // how far beyond the outline a stroke can draw, in the drawing's coordinates
    bool smooth = true;
    // The box of all of it in the drawing's coordinates, the stroke's reach not counted in.
    float l = 0, t = 0, r = 0, b = 0;
};

void drawGlyphs(Surface& s, const Ink& ink, const Matrix3x2F& world, ID2D1Brush* brush, bool ownColours, uint32_t alpha);

/// The pixels of the canvas the ink can touch: for memory to draw it in.
Box inkCover(const Ink& ink, const Matrix3x2F& m) {
    const bool stroked = ink.style != MEGLER_GFX_FILL && ink.kind != Ink::glyphs;
    const float reach = stroked ? ink.reach : 0;
    // (A hairline is a pixel wide whatever the matrix: its reach is in pixels.)
    return coverOf(m, ink.l - reach, ink.t - reach, ink.r + reach, ink.b + reach, stroked && !(ink.strokeWidth > 0) ? 3 : 2);
}

/// The ink on a surface that is drawing, through `m` (the drawing's
/// coordinates to the canvas's pixels), filled with `brush`.
void drawInk(Surface& s, const Ink& ink, const Matrix3x2F& m, ID2D1Brush* brush, bool ownColours, uint32_t alpha) {
    ID2D1RenderTarget* rt = s.rt.get();
    if (ink.kind == Ink::glyphs) {
        rt->SetTextAntialiasMode(ink.smooth ? D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE : D2D1_TEXT_ANTIALIAS_MODE_ALIASED);
        drawGlyphs(s, ink, m, brush, ownColours, alpha);
        return;
    }
    s.setMatrix(m);
    rt->SetAntialiasMode(ink.smooth ? D2D1_ANTIALIAS_MODE_PER_PRIMITIVE : D2D1_ANTIALIAS_MODE_ALIASED);
    const bool fill = ink.style != MEGLER_GFX_STROKE, outline = ink.style != MEGLER_GFX_FILL;
    // A hairline's style makes it a pixel wide; without such a style it is as wide as a pixel is long under the matrix, about.
    float width = ink.strokeWidth;
    if (outline && !(width > 0)) width = G().d2d1 ? 1.0f : 1.0f / meanScale(m);
    switch (ink.kind) {
    case Ink::rectangle:
        if (fill) rt->FillRectangle(ink.box, brush);
        if (outline) rt->DrawRectangle(ink.box, brush, width, ink.stroke.get());
        break;
    case Ink::rounded:
        if (fill) rt->FillRoundedRectangle(D2D1::RoundedRect(ink.box, ink.rx, ink.ry), brush);
        if (outline) rt->DrawRoundedRectangle(D2D1::RoundedRect(ink.box, ink.rx, ink.ry), brush, width, ink.stroke.get());
        break;
    case Ink::oval: {
        const D2D1_ELLIPSE ellipse = D2D1::Ellipse(D2D1::Point2F((ink.box.left + ink.box.right) / 2, (ink.box.top + ink.box.bottom) / 2),
                                                   (ink.box.right - ink.box.left) / 2, (ink.box.bottom - ink.box.top) / 2);
        if (fill) rt->FillEllipse(ellipse, brush);
        if (outline) rt->DrawEllipse(ellipse, brush, width, ink.stroke.get());
        break;
    }
    default:
        if (!ink.shape) break;
        if (fill) rt->FillGeometry(ink.shape, brush);
        if (outline) rt->DrawGeometry(ink.shape, brush, width, ink.stroke.get());
        break;
    }
}

/// Which pixels of `box` the ink covers and how much of each, the clip
/// counted in when `clipped`: into the canvas's `cover` picture, in white.
const Surface* inkCoverage(Canvas& cv, const Ink& ink, const Box& box, bool clipped) {
    Surface& s = cv.cover;
    if (!s.allocate(box) || !s.begin()) return nullptr;
    if (clipped) s.useClip(cv.clip());
    s.brush->SetColor(D2D1::ColorF(1, 1, 1, 1));
    drawInk(s, ink, cv.matrix, s.brush.get(), false, 255);
    s.end();
    return &s;
}

/// A shader's colours over all of `box`, whatever shape is drawn: into the
/// canvas's `source` picture. The brush's own coordinates are the drawing's.
const Surface* shaderPicture(Canvas& cv, Shader& shader, const Box& box, bool filterBitmap) {
    Surface& s = cv.source;
    if (!s.allocate(box) || !s.begin()) return nullptr;
    {
        std::lock_guard<std::mutex> lock(shader.mutex);
        ID2D1Brush* brush = shader.brushFor(false);
        if (!brush) {
            s.end();
            return nullptr;
        }
        brush->SetTransform(shader.local * cv.matrix);
        brush->SetOpacity(1.0f);
        if (ID2D1BitmapBrush* bitmap = shader.bitmapBrushFor(false))
            bitmap->SetInterpolationMode(filterBitmap ? D2D1_BITMAP_INTERPOLATION_MODE_LINEAR : D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        s.rt->SetTransform(Matrix3x2F::Translation(float(-box.l), float(-box.t)));
        s.rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        s.rt->FillRectangle(D2D1::RectF(float(box.l), float(box.t), float(box.r), float(box.b)), brush);
    }
    s.end();
    return &s;
}

/// The widest blur a shadow is given: that of the largest radius taken (150)
/// under a matrix that does not scale. A scaled-up canvas stops there.
constexpr float maxShadowSigma = 0.57735f * 150.0f + 0.5f;

/// A Gaussian blur of the alpha bytes of a picture, in place: Android's
/// shadow radius is a standard deviation of 0.57735 * radius + 0.5.
void blurCover(Canvas& cv, Surface& s, float sigma) {
    const int reach = std::max(1, int(std::ceil(sigma * 3)));
    std::vector<float> kernel(size_t(2 * reach + 1));
    float sum = 0;
    for (int i = -reach; i <= reach; i++) sum += kernel[size_t(i + reach)] = std::exp(-float(i * i) / (2 * sigma * sigma));
    for (float& k : kernel) k /= sum;
    const size_t n = size_t(s.w) * size_t(s.h);
    cv.blurA.resize(n);
    cv.blurB.resize(n);
    for (size_t i = 0; i < n; i++) cv.blurA[i] = uint8_t(s.own[i] >> 24);
    for (int y = 0; y < s.h; y++) {
        const uint8_t* in = cv.blurA.data() + size_t(y) * size_t(s.w);
        uint8_t* out = cv.blurB.data() + size_t(y) * size_t(s.w);
        for (int x = 0; x < s.w; x++) {
            float v = 0;
            const int from = std::max(0, x - reach), to = std::min(s.w - 1, x + reach);
            for (int k = from; k <= to; k++) v += float(in[k]) * kernel[size_t(k - x + reach)];
            out[x] = uint8_t(std::min(255.0f, v + 0.5f));
        }
    }
    for (int y = 0; y < s.h; y++) {
        const int from = std::max(0, y - reach), to = std::min(s.h - 1, y + reach);
        for (int x = 0; x < s.w; x++) {
            float v = 0;
            for (int k = from; k <= to; k++) v += float(cv.blurB[size_t(k) * size_t(s.w) + size_t(x)]) * kernel[size_t(k - y + reach)];
            s.own[size_t(y) * size_t(s.w) + size_t(x)] = uint32_t(std::min(255.0f, v + 0.5f)) << 24;
        }
    }
}

/// The paint's shadow under what a call draws: the ink's coverage, blurred,
/// moved by the paint's offset and laid on in the shadow's colour. Offset
/// and radius are the drawing's own: the offset goes through the matrix as
/// a direction, the blur grows with the matrix's scale.
bool gpuShadow(Canvas& cv, Surface& target, const Ink& ink, const megler_gfx_paint& paint);

void drawShadow(Canvas& cv, Surface& target, const Ink& ink, const megler_gfx_paint& paint) {
    if (target.gc && gpuShadow(cv, target, ink, paint)) return;
    // Opaque, the shadow's colour takes the paint's alpha; otherwise it keeps its own.
    uint32_t colour = paint.shadow_color;
    if ((colour >> 24) == 255) colour = (colour & 0x00FFFFFFu) | (paint.color & 0xFF000000u);
    if ((colour >> 24) == 0) return;
    const Matrix3x2F& m = cv.matrix;
    const float sigma = std::min((0.57735f * paint.shadow_radius + 0.5f) * meanScale(m), maxShadowSigma);
    const int reach = std::max(1, int(std::ceil(sigma * 3)));
    const float movedX = paint.shadow_dx * m._11 + paint.shadow_dy * m._21, movedY = paint.shadow_dx * m._12 + paint.shadow_dy * m._22;
    if (!(std::fabs(movedX) < 32768.0f && std::fabs(movedY) < 32768.0f)) return;
    const int dx = int(std::lround(movedX)), dy = int(std::lround(movedY));
    // Where the shadow can fall, and what of the ink can throw one there.
    Box lands = inkCover(ink, cv.matrix);
    lands.inflate(reach);
    lands.move(dx, dy);
    lands.intersect(cv.clip().box);
    lands.intersect(target.bounds());
    if (lands.empty()) return;
    Box from = lands;
    from.move(-dx, -dy);
    from.inflate(reach);
    if (int64_t(from.width()) * int64_t(from.height()) > (int64_t(1) << 24)) {
        note("shadow-size", "a shadow over %d x %d pixels is not drawn (said once)", from.width(), from.height());
        return;
    }
    target.end();
    const Surface* inked = inkCoverage(cv, ink, from, false);
    if (!inked) return;
    blurCover(cv, cv.cover, sigma);
    // The blurred coverage moved to where the shadow falls: the cover the shadow's colour is laid on through.
    Surface& moved = cv.source;
    if (!moved.allocate(lands)) return;
    for (int y = lands.t; y < lands.b; y++) memcpy(moved.row(y), cv.cover.row(y - dy) + (lands.l - dx - cv.cover.ox), size_t(lands.width()) * 4);
    if (const Surface* through = clipCover(cv, lands)) {
        for (int y = lands.t; y < lands.b; y++) {
            uint32_t* m = moved.row(y);
            const uint32_t* c = through->row(y);
            for (int x = 0; x < lands.width(); x++) m[x] = mul255(m[x] >> 24, c[x] >> 24) << 24;
        }
    }
    Source src;
    src.solid = premultiplied(colour);
    compose(target, lands, src, &moved, paint.blend);
}

// MARK: Putting a drawing together on the graphics processor

/// One of the canvas's work surfaces on the graphics processor
/// (Canvas::work) covering `box`, drawing, and cleared to nothing -- or, with
/// `keep`, not drawing and as it is, for a copy to go into. Its bitmap only
/// grows and may be larger than the box; what is beyond the box is never
/// shown. Null where the box is larger than the device's bitmaps.
Surface* workSurface(Canvas& cv, int which, const Box& box, bool keep = false) {
    Surface& s = cv.work[which];
    if (!cv.gc || box.empty() || gpuGone()) return nullptr;
    s.detach();
    const D2D1_SIZE_U size = s.gpu ? s.gpu->GetPixelSize() : D2D1::SizeU(0, 0);
    if (UINT32(box.width()) > size.width || UINT32(box.height()) > size.height) {
        // (Grown by 64 pixels at a time each way, so that a box a little larger than the last does not make another.)
        const UINT32 most = G().gpuMaxSize;
        const UINT32 w = std::min(most, std::max(size.width, (UINT32(box.width()) + 63) & ~63u));
        const UINT32 h = std::min(most, std::max(size.height, (UINT32(box.height()) + 63) & ~63u));
        if (UINT32(box.width()) > w || UINT32(box.height()) > h) return nullptr;
        s.gc = cv.gc;
        s.unbind();
        s.gpu = gpuBitmap(int(w), int(h), targetOptions);
        if (!s.gpu) return nullptr;
    }
    s.gc = cv.gc;
    s.rt = Ref<ID2D1RenderTarget>(cv.gc->dc);
    s.brush = cv.gc->brush;
    s.second = cv.gc->second;
    s.px = nullptr;
    s.ox = box.l;
    s.oy = box.t;
    s.w = box.width();
    s.h = box.height();
    s.stride = s.w * 4;
    s.gpuAhead = s.memAhead = Box();
    if (keep) return &s;
    if (!s.begin()) return nullptr;
    s.rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    return &s;
}

/// The composite mode of Direct2D's that is megler.h's transfer mode, and
/// whether it is one that leaves the picture as it was where the source is
/// nothing ("bounded"), so that DrawImage can put it on by itself.
bool compositeOf(int blend, D2D1_COMPOSITE_MODE* mode, bool* bounded) {
    *bounded = true;
    switch (blend) {
    case MEGLER_GFX_BLEND_SRC_OVER: *mode = D2D1_COMPOSITE_MODE_SOURCE_OVER; return true;
    case MEGLER_GFX_BLEND_DST_OVER: *mode = D2D1_COMPOSITE_MODE_DESTINATION_OVER; return true;
    case MEGLER_GFX_BLEND_DST_OUT: *mode = D2D1_COMPOSITE_MODE_DESTINATION_OUT; return true;
    case MEGLER_GFX_BLEND_SRC_ATOP: *mode = D2D1_COMPOSITE_MODE_SOURCE_ATOP; return true;
    case MEGLER_GFX_BLEND_XOR: *mode = D2D1_COMPOSITE_MODE_XOR; return true;
    case MEGLER_GFX_BLEND_PLUS: *mode = D2D1_COMPOSITE_MODE_PLUS; return true;
    default: break;
    }
    *bounded = false;
    switch (blend) {
    case MEGLER_GFX_BLEND_SRC_IN: *mode = D2D1_COMPOSITE_MODE_SOURCE_IN; return true;
    case MEGLER_GFX_BLEND_DST_IN: *mode = D2D1_COMPOSITE_MODE_DESTINATION_IN; return true;
    case MEGLER_GFX_BLEND_SRC_OUT: *mode = D2D1_COMPOSITE_MODE_SOURCE_OUT; return true;
    case MEGLER_GFX_BLEND_DST_ATOP: *mode = D2D1_COMPOSITE_MODE_DESTINATION_ATOP; return true;
    default: return false;
    }
}

/// Direct2D's Blend effect's mode for the four of megler.h's that are blends
/// (its formula, W3C's, is megler.h's for these four: S*(1-Da) + D*(1-Sa) +
/// Sa*Da*B(S/Sa, D/Da)); false for the others. Its MULTIPLY is not
/// MODULATE, whose alpha is Sa*Da.
bool blendOf(int blend, D2D1_BLEND_MODE* mode) {
    switch (blend) {
    case MEGLER_GFX_BLEND_SCREEN: *mode = D2D1_BLEND_MODE_SCREEN; return true;
    case MEGLER_GFX_BLEND_OVERLAY: *mode = D2D1_BLEND_MODE_OVERLAY; return true;
    case MEGLER_GFX_BLEND_DARKEN: *mode = D2D1_BLEND_MODE_DARKEN; return true;
    case MEGLER_GFX_BLEND_LIGHTEN: *mode = D2D1_BLEND_MODE_LIGHTEN; return true;
    default: return false;
    }
}

/// The ColorMatrix effect's matrix applied to premultiplied pixels as they
/// are. (Its mode called PREMULTIPLIED, the default, divides by alpha first
/// and multiplies after; STRAIGHT leaves the values alone.)
constexpr D2D1_COLORMATRIX_ALPHA_MODE matrixAsItStands = D2D1_COLORMATRIX_ALPHA_MODE_STRAIGHT;

/// A premultiplied colour as Direct2D's effects take a vector: 0..1 a channel.
inline D2D1_VECTOR_4F vectorOf(uint32_t premultipliedColour) {
    return D2D1::Vector4F(float((premultipliedColour >> 16) & 255) / 255.0f, float((premultipliedColour >> 8) & 255) / 255.0f, float(premultipliedColour & 255) / 255.0f,
                          float(premultipliedColour >> 24) / 255.0f);
}

/// A colour filter (megler.h, THE PAINT) over `colours`: each colour C becomes
/// the blend of S = the filter's colour and D = C. With S fixed, fifteen of
/// the eighteen are a sum of C, C's alpha and S, which the ColorMatrix effect
/// works out as it stands (premultiplied, clamped to 0..1); OVERLAY, DARKEN
/// and LIGHTEN are the Blend effect with the colour as a Flood.
Ref<ID2D1Image> filtered(GpuContext& g, ID2D1Image* colours, int mode, uint32_t filterColour) {
    Ref<ID2D1Image> out;
    const uint32_t f = premultiplied(filterColour);
    D2D1_BLEND_MODE blendMode;
    if (blendOf(mode, &blendMode) && mode != MEGLER_GFX_BLEND_SCREEN) {
        ID2D1Effect* flood = g.effect(GpuContext::filterFlood);
        ID2D1Effect* blend = g.effect(GpuContext::filterBlend);
        if (!flood || !blend) return out;
        // (Flood's colour is the pixel it makes, premultiplied: seen, it does not multiply by the alpha itself.)
        flood->SetValue(D2D1_FLOOD_PROP_COLOR, vectorOf(f));
        blend->SetInput(0, colours);
        blend->SetInputEffect(1, flood);
        blend->SetValue(D2D1_BLEND_PROP_MODE, blendMode);
        blend->GetOutput(out.put());
        return out;
    }
    // Each channel out = p * C + q * C's alpha + r, by the mode (for the alpha channel C and its alpha are one):
    const float fa = float(f >> 24) / 255.0f;
    const D2D1_VECTOR_4F fv = vectorOf(f);
    const float fc[4] = {fv.x, fv.y, fv.z, fv.w};
    D2D1_MATRIX_5X4_F m = {};
    float* cell = &m._11; // row k (input R, G, B, A, then the constant), column c (output R, G, B, A): cell[k * 4 + c]
    for (int c = 0; c < 4; c++) {
        float p = 0, q = 0, r = 0;
        switch (mode) {
        case MEGLER_GFX_BLEND_CLEAR: break;
        case MEGLER_GFX_BLEND_SRC: r = fc[c]; break;
        case MEGLER_GFX_BLEND_DST: p = 1; break;
        case MEGLER_GFX_BLEND_DST_OVER: p = 1, q = -fc[c], r = fc[c]; break;
        case MEGLER_GFX_BLEND_SRC_IN: q = fc[c]; break;
        case MEGLER_GFX_BLEND_DST_IN: p = fa; break;
        case MEGLER_GFX_BLEND_SRC_OUT: q = -fc[c], r = fc[c]; break;
        case MEGLER_GFX_BLEND_DST_OUT: p = 1 - fa; break;
        case MEGLER_GFX_BLEND_SRC_ATOP: p = 1 - fa, q = fc[c]; break;
        case MEGLER_GFX_BLEND_DST_ATOP: p = fa, q = -fc[c], r = fc[c]; break;
        case MEGLER_GFX_BLEND_XOR: p = 1 - fa, q = -fc[c], r = fc[c]; break;
        case MEGLER_GFX_BLEND_PLUS: p = 1, r = fc[c]; break;
        case MEGLER_GFX_BLEND_MODULATE: p = fc[c]; break;
        case MEGLER_GFX_BLEND_SCREEN: p = 1 - fc[c], r = fc[c]; break;
        default: p = 1 - fa, r = fc[c]; break; // SRC_OVER
        }
        if (c < 3) {
            cell[c * 4 + c] = p;
            cell[3 * 4 + c] = q;
        } else {
            cell[3 * 4 + 3] = p + q;
        }
        cell[4 * 4 + c] = r;
    }
    ID2D1Effect* matrix = g.effect(GpuContext::filterMatrix);
    if (!matrix) return out;
    matrix->SetInput(0, colours);
    matrix->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, m);
    matrix->SetValue(D2D1_COLORMATRIX_PROP_ALPHA_MODE, matrixAsItStands);
    matrix->SetValue(D2D1_COLORMATRIX_PROP_CLAMP_OUTPUT, TRUE);
    matrix->GetOutput(out.put());
    return out;
}

/// `a` -- the call's colours times its coverage, an image whose (0, 0) is
/// the box's corner -- put onto `target` in `box` by the transfer mode, on
/// the graphics processor. `c` is the coverage alone, in white: null where
/// every pixel of the box is covered whole, and needed only by the modes
/// that take colour away where a pixel is partly covered. As GfxHost.cpp's
/// top says: a bounded composite mode by DrawImage; the rest worked out over
/// a copy of the box, D, and copied into it:
///     the four blends               Blend(D, A)
///     the seven that take colour    D * (1 - C) + mode(A, D), the mode a
///     away                          Composite (SRC_IN, DST_IN, SRC_OUT,
///                                   DST_ATOP), A itself (SRC), an
///                                   ArithmeticComposite S*D (MODULATE) or
///                                   nothing (CLEAR)
/// False when it cannot be done there.
bool gpuPut(Canvas& cv, Surface& target, const Box& box, ID2D1Image* a, ID2D1Image* c, int blend) {
    GpuContext& g = *cv.gc;
    const D2D1_POINT_2F at = D2D1::Point2F(float(box.l - target.ox), float(box.t - target.oy));
    const D2D1_RECT_F part = D2D1::RectF(0, 0, float(box.width()), float(box.height()));
    D2D1_COMPOSITE_MODE composite = D2D1_COMPOSITE_MODE_SOURCE_OVER;
    bool bounded = false;
    const bool porterDuff = compositeOf(blend, &composite, &bounded);
    if (porterDuff && bounded) {
        if (!target.begin()) return false;
        target.useBoxClip(box);
        g.dc->DrawImage(a, &at, &part, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, composite);
    } else {
        // What is there now, copied: the effects read the copy and their result replaces the box.
        Surface* d = workSurface(cv, Canvas::workD, box, true);
        if (!d) return false;
        g.finish();
        const D2D1_POINT_2U origin = D2D1::Point2U(0, 0);
        const D2D1_RECT_U from = D2D1::RectU(UINT32(box.l - target.ox), UINT32(box.t - target.oy), UINT32(box.r - target.ox), UINT32(box.b - target.oy));
        const HRESULT hr = d->gpu->CopyFromBitmap(&origin, target.gpu.get(), &from);
        if (FAILED(hr)) {
            gpuFailed("ID2D1Bitmap::CopyFromBitmap", hr);
            return false;
        }
        ID2D1Image* there = d->gpu.get();
        Ref<ID2D1Image> result;
        D2D1_BLEND_MODE blendMode;
        if (blendOf(blend, &blendMode)) {
            ID2D1Effect* e = g.effect(GpuContext::modeBlend);
            if (!e) return false;
            e->SetInput(0, there);
            e->SetInput(1, a);
            e->SetValue(D2D1_BLEND_PROP_MODE, blendMode);
            e->GetOutput(result.put());
        } else {
            Ref<ID2D1Image> moded; // mode(A, D)
            if (porterDuff) {
                ID2D1Effect* e = g.effect(GpuContext::modeComposite);
                if (!e) return false;
                e->SetValue(D2D1_COMPOSITE_PROP_MODE, composite);
                e->SetInput(0, there);
                e->SetInput(1, a);
                e->GetOutput(moded.put());
            } else if (blend == MEGLER_GFX_BLEND_MODULATE) {
                ID2D1Effect* e = g.effect(GpuContext::modeArithmetic);
                if (!e) return false;
                e->SetValue(D2D1_ARITHMETICCOMPOSITE_PROP_COEFFICIENTS, D2D1::Vector4F(1, 0, 0, 0));
                e->SetValue(D2D1_ARITHMETICCOMPOSITE_PROP_CLAMP_OUTPUT, TRUE);
                e->SetInput(0, there);
                e->SetInput(1, a);
                e->GetOutput(moded.put());
            } else if (blend == MEGLER_GFX_BLEND_SRC) {
                moded = Ref<ID2D1Image>();
                a->AddRef();
                moded.attach(a);
            }
            if (!c) {
                // Every pixel covered whole: the mode alone; CLEAR leaves nothing.
                if (!moded) {
                    if (!target.begin()) return false;
                    target.useBoxClip(box);
                    target.rt->Clear(D2D1::ColorF(0, 0, 0, 0));
                    target.drew(box);
                    ++G().byEffects;
                    return true;
                }
                result = moded;
            } else {
                ID2D1Effect* held = g.effect(GpuContext::held);
                if (!held) return false;
                held->SetValue(D2D1_COMPOSITE_PROP_MODE, D2D1_COMPOSITE_MODE_DESTINATION_OUT);
                held->SetInput(0, there);
                held->SetInput(1, c);
                if (!moded) {
                    held->GetOutput(result.put());
                } else {
                    ID2D1Effect* sum = g.effect(GpuContext::sum);
                    if (!sum) return false;
                    sum->SetValue(D2D1_COMPOSITE_PROP_MODE, D2D1_COMPOSITE_MODE_PLUS);
                    sum->SetInputEffect(0, held);
                    sum->SetInput(1, moded.get());
                    sum->GetOutput(result.put());
                }
            }
        }
        if (!result || !target.begin()) return false;
        target.useBoxClip(box);
        g.dc->DrawImage(result.get(), &at, &part, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_BOUNDED_SOURCE_COPY);
    }
    target.drew(box);
    ++G().byEffects;
    // What the effects were given is handed to the processor before the next call gives them other inputs, and before the
    // work surfaces are drawn into or copied into again.
    g.finish();
    return true;
}

/// The call's colours over the whole of `box`, at the paint's alpha and
/// through its colour filter, before the shape cuts them: for a shader with
/// a filter, a sweep (whose colours are worked out here, Direct2D having no
/// such brush, and uploaded), and a fill and stroke that overlap.
Ref<ID2D1Image> gpuColours(Canvas& cv, const Box& box, const megler_gfx_paint& paint, Shader* shader) {
    Ref<ID2D1Image> out;
    GpuContext& g = *cv.gc;
    const bool sweep = shader && shader->kind == Shader::sweep;
    Surface* s = workSurface(cv, Canvas::workS, box, sweep);
    if (!s) return out;
    if (!shader) {
        s->rt->Clear(colorOf(paint.color));
    } else if (sweep) {
        Matrix3x2F back = shader->local * cv.matrix;
        if (!back.Invert()) return out;
        std::vector<uint32_t> pixels(size_t(box.width()) * size_t(box.height()));
        const uint32_t alpha = paint.color >> 24;
        for (int y = box.t; y < box.b; y++)
            for (int x = box.l; x < box.r; x++) pixels[size_t(y - box.t) * size_t(box.width()) + size_t(x - box.l)] = faded(sweepAt(*shader, back, x, y), alpha);
        g.finish();
        const D2D1_RECT_U all = D2D1::RectU(0, 0, UINT32(box.width()), UINT32(box.height()));
        const HRESULT hr = s->gpu->CopyFromMemory(&all, pixels.data(), UINT32(box.width()) * 4);
        ++G().uploads;
        G().uploadPixels += uint64_t(pixels.size());
        if (FAILED(hr)) {
            gpuFailed("ID2D1Bitmap::CopyFromMemory", hr);
            return out;
        }
    } else {
        std::lock_guard<std::mutex> lock(shader->mutex);
        ID2D1Brush* brush = shader->brushFor(true);
        if (!brush) return out;
        brush->SetTransform(shader->local * cv.matrix);
        brush->SetOpacity(float(paint.color >> 24) / 255.0f);
        if (ID2D1BitmapBrush* bitmap = shader->bitmapBrushFor(true))
            bitmap->SetInterpolationMode((paint.flags & MEGLER_GFX_FILTER_BITMAP) ? D2D1_BITMAP_INTERPOLATION_MODE_LINEAR : D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        s->setMatrix(Matrix3x2F::Identity());
        s->rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        s->rt->FillRectangle(D2D1::RectF(float(box.l), float(box.t), float(box.r), float(box.b)), brush);
    }
    Ref<ID2D1Image> colours;
    colours = Ref<ID2D1Image>(s->gpu);
    if (paint.filter_mode >= 0) return filtered(g, colours.get(), paint.filter_mode, paint.filter_color);
    return colours;
}

/// `colours` times the coverage `c`: what the shape cuts out of them.
Ref<ID2D1Image> covered(GpuContext& g, ID2D1Image* colours, ID2D1Image* c) {
    Ref<ID2D1Image> out;
    ID2D1Effect* in = g.effect(GpuContext::coverIn);
    if (!in) return out;
    in->SetValue(D2D1_COMPOSITE_PROP_MODE, D2D1_COMPOSITE_MODE_DESTINATION_IN);
    in->SetInput(0, colours);
    in->SetInput(1, c);
    in->GetOutput(out.put());
    return out;
}

/// The coverage of the ink in white, under the canvas's clip (`clipped`),
/// over `box`, into the work surface `which`.
Surface* gpuCoverage(Canvas& cv, const Ink& ink, const Box& box, bool clipped, int which) {
    Surface* c = workSurface(cv, which, box);
    if (!c) return nullptr;
    if (clipped) c->useClip(cv.clip());
    c->brush->SetColor(D2D1::ColorF(1, 1, 1, 1));
    drawInk(*c, ink, cv.matrix, c->brush.get(), false, 255);
    return c;
}

/// One drawing call's ink put on by what Direct2D does not lay on by itself
/// -- another transfer mode, a shader with a colour filter, a sweep, a fill
/// and stroke that overlap -- on the graphics processor. False when it
/// cannot be done there; the caller puts it on through memory then.
bool gpuPaintInk(Canvas& cv, Surface& target, const Ink& ink, const megler_gfx_paint& paint, Shader* shader) {
    Box box = inkCover(ink, cv.matrix);
    box.intersect(cv.clip().box);
    box.intersect(target.bounds());
    if (box.empty()) return true;
    const bool keeps = keepsUncovered(paint.blend);
    const bool apart = (shader && (shader->kind == Shader::sweep || paint.filter_mode >= 0)) || paint.style == MEGLER_GFX_FILL_AND_STROKE;
    Surface* c = nullptr;
    if (!keeps || apart) {
        c = gpuCoverage(cv, ink, box, true, Canvas::workC);
        if (!c) return false;
    }
    Ref<ID2D1Image> a;
    if (!apart) {
        // The shape in its colours, under the clip: colour times coverage, as Direct2D draws it onto nothing.
        Surface* s = workSurface(cv, Canvas::workA, box);
        if (!s) return false;
        s->useClip(cv.clip());
        if (shader) {
            std::lock_guard<std::mutex> lock(shader->mutex);
            ID2D1Brush* brush = shader->brushFor(true);
            if (!brush) return false;
            Matrix3x2F brushMatrix = shader->local;
            if (ink.kind == Ink::glyphs) {
                Matrix3x2F back = ink.textMatrix;
                if (back.Invert()) brushMatrix = shader->local * back;
            }
            brush->SetTransform(brushMatrix);
            brush->SetOpacity(float(paint.color >> 24) / 255.0f);
            if (ID2D1BitmapBrush* bitmap = shader->bitmapBrushFor(true))
                bitmap->SetInterpolationMode((paint.flags & MEGLER_GFX_FILTER_BITMAP) ? D2D1_BITMAP_INTERPOLATION_MODE_LINEAR
                                                                                    : D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            drawInk(*s, ink, cv.matrix, brush, false, 255);
        } else {
            s->brush->SetColor(colorOf(paint.color));
            drawInk(*s, ink, cv.matrix, s->brush.get(), true, paint.color >> 24);
        }
        a = Ref<ID2D1Image>(s->gpu);
    } else {
        // The colours over the whole box, and the shape (both its fill and its stroke, once) cutting them.
        Ref<ID2D1Image> colours = gpuColours(cv, box, paint, shader);
        if (!colours) return false;
        a = covered(*cv.gc, colours.get(), c->gpu.get());
        if (!a) return false;
    }
    return gpuPut(cv, target, box, a.get(), keeps ? nullptr : c->gpu.get(), paint.blend);
}

/// A drawColor that is not a Clear or a rectangle over the picture: the
/// colour over the box, cut by the clip.
bool gpuDrawColor(Canvas& cv, Surface& target, const Box& box, uint32_t colour, int blend) {
    const Clip& clip = cv.clip();
    Surface* c = nullptr;
    if (!keepsUncovered(blend) && clip.g) {
        c = workSurface(cv, Canvas::workC, box);
        if (!c) return false;
        c->useClip(clip);
        c->setMatrix(Matrix3x2F::Identity());
        c->rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        c->brush->SetColor(D2D1::ColorF(1, 1, 1, 1));
        c->rt->FillRectangle(D2D1::RectF(float(box.l), float(box.t), float(box.r), float(box.b)), c->brush.get());
    }
    Surface* s = workSurface(cv, Canvas::workA, box);
    if (!s) return false;
    s->useClip(clip);
    s->setMatrix(Matrix3x2F::Identity());
    s->rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    s->brush->SetColor(colorOf(colour));
    s->rt->FillRectangle(D2D1::RectF(float(box.l), float(box.t), float(box.r), float(box.b)), s->brush.get());
    return gpuPut(cv, target, box, s->gpu.get(), c ? c->gpu.get() : nullptr, blend);
}

/// A layer put onto `outer` at its restore by a mode other than SRC_OVER:
/// the layer's box at its alpha (the ColorMatrix effect), cut by the clip at
/// the save where that is more than its box.
bool gpuRestore(Canvas& cv, Surface& outer, Surface& layer, const Box& box, int alpha, int blend) {
    GpuContext& g = *cv.gc;
    Surface* s = workSurface(cv, Canvas::workS, box, true);
    if (!s) return false;
    g.finish();
    const D2D1_POINT_2U origin = D2D1::Point2U(0, 0);
    const D2D1_RECT_U from = D2D1::RectU(UINT32(box.l - layer.ox), UINT32(box.t - layer.oy), UINT32(box.r - layer.ox), UINT32(box.b - layer.oy));
    const HRESULT hr = s->gpu->CopyFromBitmap(&origin, layer.gpu.get(), &from);
    if (FAILED(hr)) {
        gpuFailed("ID2D1Bitmap::CopyFromBitmap", hr);
        return false;
    }
    Ref<ID2D1Image> a(s->gpu);
    if (alpha < 255) {
        ID2D1Effect* e = g.effect(GpuContext::opacity);
        if (!e) return false;
        const float k = float(alpha) / 255.0f;
        e->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, D2D1::Matrix5x4F(k, 0, 0, 0, 0, k, 0, 0, 0, 0, k, 0, 0, 0, 0, k, 0, 0, 0, 0));
        e->SetValue(D2D1_COLORMATRIX_PROP_ALPHA_MODE, matrixAsItStands);
        e->SetInput(0, a.get());
        a.reset();
        e->GetOutput(a.put());
    }
    const Clip& clip = cv.clip();
    Surface* c = nullptr;
    if (clip.g) {
        c = workSurface(cv, Canvas::workC, box);
        if (!c) return false;
        c->useClip(clip);
        c->setMatrix(Matrix3x2F::Identity());
        c->rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        c->brush->SetColor(D2D1::ColorF(1, 1, 1, 1));
        c->rt->FillRectangle(D2D1::RectF(float(box.l), float(box.t), float(box.r), float(box.b)), c->brush.get());
        a = covered(g, a.get(), c->gpu.get());
        if (!a) return false;
    }
    return gpuPut(cv, outer, box, a.get(), keepsUncovered(blend) || !c ? nullptr : c->gpu.get(), blend);
}

/// The paint's shadow under what a call draws, on the graphics processor:
/// the ink's coverage blurred (the GaussianBlur effect, a standard deviation
/// of 0.57735 * radius + 0.5 grown with the matrix's scale, as in memory),
/// moved by the offset, cut by the clip, in the shadow's colour, put on by
/// the paint's transfer mode.
bool gpuShadow(Canvas& cv, Surface& target, const Ink& ink, const megler_gfx_paint& paint) {
    uint32_t colour = paint.shadow_color;
    if ((colour >> 24) == 255) colour = (colour & 0x00FFFFFFu) | (paint.color & 0xFF000000u);
    if ((colour >> 24) == 0) return true;
    const Matrix3x2F& m = cv.matrix;
    const float sigma = std::min((0.57735f * paint.shadow_radius + 0.5f) * meanScale(m), maxShadowSigma);
    const int reach = std::max(1, int(std::ceil(sigma * 3)));
    const float movedX = paint.shadow_dx * m._11 + paint.shadow_dy * m._21, movedY = paint.shadow_dx * m._12 + paint.shadow_dy * m._22;
    if (!(std::fabs(movedX) < 32768.0f && std::fabs(movedY) < 32768.0f)) return true;
    const int dx = int(std::lround(movedX)), dy = int(std::lround(movedY));
    Box lands = inkCover(ink, cv.matrix);
    lands.inflate(reach);
    lands.move(dx, dy);
    lands.intersect(cv.clip().box);
    lands.intersect(target.bounds());
    if (lands.empty()) return true;
    Box from = lands;
    from.move(-dx, -dy);
    from.inflate(reach);
    GpuContext& g = *cv.gc;
    // The ink's coverage where it can throw a shadow into the box, unclipped; blurred.
    Surface* ink0 = gpuCoverage(cv, ink, from, false, Canvas::workS);
    if (!ink0) return false;
    ID2D1Effect* blur = g.effect(GpuContext::shadowBlur);
    if (!blur) return false;
    blur->SetInput(0, ink0->gpu.get());
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, sigma);
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, D2D1_GAUSSIANBLUR_OPTIMIZATION_QUALITY);
    blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_SOFT);
    Ref<ID2D1Image> blurred;
    blur->GetOutput(blurred.put());
    // Moved to where the shadow falls and cut by the clip: the shadow's coverage.
    Surface* c = workSurface(cv, Canvas::workC, lands);
    if (!c) return false;
    c->useClip(cv.clip());
    c->rt->SetTransform(Matrix3x2F::Identity());
    const D2D1_POINT_2F at = D2D1::Point2F(float(from.l + dx - lands.l), float(from.t + dy - lands.t));
    const D2D1_RECT_F part = D2D1::RectF(0, 0, float(from.width()), float(from.height()));
    g.dc->DrawImage(blurred.get(), &at, &part, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_SOURCE_OVER);
    // The shadow's colour times that coverage: each output channel is the colour's times the coverage's alpha.
    const D2D1_VECTOR_4F v = vectorOf(premultiplied(colour));
    ID2D1Effect* e = g.effect(GpuContext::opacity);
    if (!e) return false;
    e->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, D2D1::Matrix5x4F(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, v.x, v.y, v.z, v.w, 0, 0, 0, 0));
    e->SetValue(D2D1_COLORMATRIX_PROP_ALPHA_MODE, matrixAsItStands);
    e->SetInput(0, c->gpu.get());
    Ref<ID2D1Image> a;
    e->GetOutput(a.put());
    return gpuPut(cv, target, lands, a.get(), keepsUncovered(paint.blend) ? nullptr : c->gpu.get(), paint.blend);
}

/// Whether Direct2D can put this onto the picture itself: laid over what is
/// there, in a colour or through a brush, each pixel of the shape once.
bool drawsDirect(const megler_gfx_paint& paint, const Shader* shader, int style) {
    if (paint.blend != MEGLER_GFX_BLEND_SRC_OVER) return false;
    if (shader && (shader->kind == Shader::sweep || paint.filter_mode >= 0)) return false;
    // Filled and stroked, the two overlap: only what is opaque may be drawn twice.
    if (style == MEGLER_GFX_FILL_AND_STROKE && !(shader ? shader->opaque && (paint.color >> 24) == 255 : (paint.color >> 24) == 255)) return false;
    return true;
}

/// One drawing call's ink onto the canvas, by the paint.
void paintInk(Canvas& cv, const Ink& ink, const megler_gfx_paint& paint, Shader* shader) {
    Surface* target = cv.target();
    if (!target || cv.clip().box.empty()) return;
    if (paint.shadow_radius > 0 && (paint.shadow_color >> 24) != 0) drawShadow(cv, *target, ink, paint);
    const bool filterBitmap = (paint.flags & MEGLER_GFX_FILTER_BITMAP) != 0;
    if (drawsDirect(paint, shader, ink.style)) {
        if (!target->begin()) return;
        target->useClip(cv.clip());
        if (shader) {
            std::lock_guard<std::mutex> lock(shader->mutex);
            ID2D1Brush* brush = shader->brushFor(target->gc != nullptr);
            if (!brush) return;
            // The brush's coordinates are the drawing's; glyphs are drawn in the run's own, so those are undone for it.
            Matrix3x2F brushMatrix = shader->local;
            if (ink.kind == Ink::glyphs) {
                Matrix3x2F back = ink.textMatrix;
                if (back.Invert()) brushMatrix = shader->local * back;
            }
            brush->SetTransform(brushMatrix);
            brush->SetOpacity(float(paint.color >> 24) / 255.0f);
            if (ID2D1BitmapBrush* bitmap = shader->bitmapBrushFor(target->gc != nullptr))
                bitmap->SetInterpolationMode(filterBitmap ? D2D1_BITMAP_INTERPOLATION_MODE_LINEAR : D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            drawInk(*target, ink, cv.matrix, brush, false, 255);
        } else {
            target->brush->SetColor(colorOf(paint.color));
            drawInk(*target, ink, cv.matrix, target->brush.get(), true, paint.color >> 24);
        }
        if (target->gc) {
            Box drawn = inkCover(ink, cv.matrix);
            drawn.intersect(cv.clip().box);
            target->drew(drawn);
        }
        ++G().direct;
        return;
    }
    // On the graphics processor, by Direct2D's effects.
    if (target->gc && gpuPaintInk(cv, *target, ink, paint, shader)) return;
    // Drawn in white in memory of the canvas's own, to learn the pixels; the colours come from the paint, or the shader over those pixels.
    Box box = inkCover(ink, cv.matrix);
    box.intersect(cv.clip().box);
    box.intersect(target->bounds());
    if (box.empty()) return;
    target->end();
    const Surface* cover = inkCoverage(cv, ink, box, true);
    if (!cover) return;
    Source src;
    src.alpha = paint.color >> 24;
    src.filterMode = paint.filter_mode;
    src.filter = premultiplied(paint.filter_color);
    if (!shader) {
        src.solid = premultiplied(paint.color | 0xFF000000u);
    } else if (shader->kind == Shader::sweep) {
        Matrix3x2F back = shader->local * cv.matrix;
        if (!back.Invert()) return;
        src.sweep = shader;
        src.toShader = back;
    } else {
        src.pixels = shaderPicture(cv, *shader, box, filterBitmap);
        if (!src.pixels) return;
    }
    compose(*target, box, src, cover, paint.blend);
    ++G().composed;
}

/// The stroke's style for a paint, when it strokes.
void inkStroke(Ink& ink, const megler_gfx_paint& paint) {
    ink.style = paint.style;
    ink.smooth = (paint.flags & MEGLER_GFX_ANTI_ALIAS) != 0;
    ink.strokeWidth = paint.stroke_width > 0 ? paint.stroke_width : 0;
    if (paint.style != MEGLER_GFX_FILL) {
        ink.stroke = strokeStyle(paint.cap, paint.join, paint.miter_limit, !(paint.stroke_width > 0));
        // A miter reaches the limit times half the width from the corner, a square cap half the width times the root of two.
        const float furthest = paint.join == MEGLER_GFX_JOIN_MITER ? std::max(1.5f, paint.miter_limit) : 1.5f;
        ink.reach = ink.strokeWidth * 0.5f * furthest + 1.0f;
    }
}

/// What an entry point does, kept from ending the app: this file's own code
/// can run out of memory, and a C caller cannot be thrown through.
template <class Work> void guarded(const char* what, Work&& work) {
    try {
        work();
    } catch (const std::bad_alloc&) {
        note("memory", "out of memory in %s: the call draws or answers nothing (said once)", what);
    } catch (...) {
        note("exception", "an exception in %s: the call draws or answers nothing (said once)", what);
    }
}

/// A coordinate Direct2D can be given: a number, and not so large that arithmetic on it stops being one.
inline bool sane(float v) { return std::isfinite(v) && std::fabs(v) < 1.0e9f; }
inline bool sane(float a, float b, float c, float d) { return sane(a) && sane(b) && sane(c) && sane(d); }

/// A transfer mode's number as megler.h has it; any other is drawn as SRC_OVER, and the log says so once.
int knownBlend(int blend) {
    if (blend >= MEGLER_GFX_BLEND_CLEAR && blend <= MEGLER_GFX_BLEND_LIGHTEN) return blend;
    note("blend-number", "a transfer mode numbered %d is not one of megler.h's eighteen: it is drawn as SRC_OVER (said once)", blend);
    return MEGLER_GFX_BLEND_SRC_OVER;
}

/// The paint of a call with whatever is out of range put right. The core has
/// checked it (megler.h, "THE PAINT"); this is so that a paint from anywhere
/// else cannot take Direct2D somewhere it does not come back from.
megler_gfx_paint checked(const megler_gfx_paint& given) {
    megler_gfx_paint p = given;
    p.blend = knownBlend(p.blend);
    if (p.style < MEGLER_GFX_FILL || p.style > MEGLER_GFX_FILL_AND_STROKE) p.style = MEGLER_GFX_FILL;
    if (!(p.stroke_width >= 0) || !std::isfinite(p.stroke_width)) p.stroke_width = 0;
    if (p.cap < MEGLER_GFX_CAP_BUTT || p.cap > MEGLER_GFX_CAP_SQUARE) p.cap = MEGLER_GFX_CAP_BUTT;
    if (p.join < MEGLER_GFX_JOIN_MITER || p.join > MEGLER_GFX_JOIN_BEVEL) p.join = MEGLER_GFX_JOIN_MITER;
    if (!(p.miter_limit > 0) || !std::isfinite(p.miter_limit)) p.miter_limit = 4;
    if (p.miter_limit > 1000) p.miter_limit = 1000;
    if (!(p.text_size > 0) || !std::isfinite(p.text_size)) p.text_size = 12;
    if (!(p.text_scale_x > 0) || !std::isfinite(p.text_scale_x)) p.text_scale_x = 1;
    if (!std::isfinite(p.text_skew_x)) p.text_skew_x = 0;
    if (!std::isfinite(p.letter_spacing)) p.letter_spacing = 0;
    if (p.text_align < MEGLER_GFX_ALIGN_LEFT || p.text_align > MEGLER_GFX_ALIGN_RIGHT) p.text_align = MEGLER_GFX_ALIGN_LEFT;
    if (p.filter_mode > MEGLER_GFX_BLEND_LIGHTEN) {
        note("filter-number", "a colour filter by a mode numbered %d, which is not one of megler.h's eighteen, is left out (said once)", p.filter_mode);
        p.filter_mode = -1;
    }
    if (p.filter_mode < 0) p.filter_mode = -1;
    if (!(p.shadow_radius > 0) || !std::isfinite(p.shadow_radius) || !sane(p.shadow_dx) || !sane(p.shadow_dy)) p.shadow_radius = 0;
    // (A blur as wide as the screen would take as long as it sounds.)
    if (p.shadow_radius > 150) p.shadow_radius = 150;
    return p;
}

// MARK: megler_gfx_host: canvases

void* canvasNew(void*, void* pixels, int width, int height, int stride) {
    Canvas* cv = nullptr;
    guarded("canvas_new", [&] {
        if (!started() || !pixels || width <= 0 || height <= 0 || stride < width * 4) return;
        std::unique_ptr<Canvas> made(new Canvas());
        made->memory = static_cast<uint8_t*>(pixels);
        made->memoryW = width;
        made->memoryH = height;
        made->memoryStride = stride;
        made->base.px = made->memory;
        made->base.w = width;
        made->base.h = height;
        made->base.stride = stride;
        State& s = G();
        if (s.gpu && !gpuGone() && UINT32(width) <= s.gpuMaxSize && UINT32(height) <= s.gpuMaxSize) {
            // On the graphics processor: a context and a target of the canvas's size; the memory goes up before the first
            // drawing that does not replace it. (A canvas too large for the device's bitmaps is drawn in memory.)
            made->gc = takeContext();
            if (made->gc) made->base.gpu = takeBitmap(width, height, false);
            if (made->base.gpu) {
                made->base.gc = made->gc;
                made->base.rt = Ref<ID2D1RenderTarget>(made->gc->dc);
                made->base.brush = made->gc->brush;
                made->base.second = made->gc->second;
                made->base.memAhead = made->base.bounds();
            } else {
                giveContext(made->gc);
                made->gc = nullptr;
            }
        }
        if (!made->base.make()) return;
        made->levels.emplace_back();
        made->levels[0].clip.box = made->base.bounds();
        touchClip(made->levels[0].clip);
        ++G().canvases;
        cv = made.release();
    });
    return cv;
}

void restoreLevel(Canvas& cv);

void canvasFree(void*, void* canvas) {
    Canvas* cv = canvasOf(canvas);
    if (!cv) return;
    guarded("canvas_free", [&] {
        // (Every save has been restored by now, and the canvas flushed: megler.h. Should one not have been, its layers are still joined to the picture.)
        while (cv->levels.size() > 1) restoreLevel(*cv);
        cv->base.end();
        if (cv->gc) {
            // (And should it not have been flushed, the memory gets what was drawn now, before the memory is the core's again.)
            cv->base.pull(cv->base.bounds());
            cv->base.giveBackGpu();
            for (Surface& s : cv->work) {
                s.unbind();
                s.forgetGpu();
            }
            giveContext(cv->gc);
            cv->gc = nullptr;
        }
    });
    cv->magic = 0;
    delete cv;
}

/// Every 5 s while canvases are flushed: where the drawing's time went since the last line. Direct2D's own drawing
/// is what remains of the core's "the shell's drawing" after these.
void noteDrawing() {
    State& s = G();
    const uint64_t now = ticksNow();
    std::lock_guard<std::mutex> lock(s.statsMutex);
    if (s.statsAt && ticksToMs(now - s.statsAt) < 5000.0) return;
    State::Totals t;
    t.sessions = s.sessions;
    t.direct = s.direct;
    t.composed = s.composed;
    t.layers = s.layers;
    t.partTargets = s.partTargets;
    t.targetsMade = s.targetsMade;
    t.makeTicks = s.makeTicks;
    t.endTicks = s.endTicks;
    t.composeTicks = s.composeTicks;
    t.composedPixels = s.composedPixels;
    t.sessionPixels = s.sessionPixels;
    t.uploads = s.uploads;
    t.uploadPixels = s.uploadPixels;
    t.uploadTicks = s.uploadTicks;
    t.readBacks = s.readBacks;
    t.readBackPixels = s.readBackPixels;
    t.readBackTicks = s.readBackTicks;
    t.byEffects = s.byEffects;
    t.throughMemory = s.throughMemory;
    t.keptMade = s.keptMade;
    t.keptReused = s.keptReused;
    t.shown = s.shown;
    t.showTicks = s.showTicks;
    t.windowReadBacks = s.windowReadBacks;
    t.windowReadBackPixels = s.windowReadBackPixels;
    t.windowReadBackTicks = s.windowReadBackTicks;
    const State::Totals& l = s.statsLast;
    typedef unsigned long long ull;
    if (s.statsAt && t.sessions != l.sessions && s.gpu && !gpuGone()) {
        // On the graphics processor: what went up and came back, which is where the time goes now, the window's pictures
        // shown without the memory, and what is kept there. A read-back of a window shown so comes only when something read
        // its memory; those of other canvases are the contract's (the memory holds the picture when flush returns).
        const double seconds = ticksToMs(now - s.statsAt) / 1000.0;
        size_t keptCount;
        uint64_t keptBytes;
        {
            std::lock_guard<std::mutex> lock(s.keptMutex);
            keptCount = s.kept.size();
            keptBytes = s.keptBytes;
        }
        const uint64_t windowBacks = t.windowReadBacks - l.windowReadBacks;
        say("%.1f s (graphics processor): %llu drawings begun and ended (%.1f ms ending them), %llu calls drawn by Direct2D, %llu by its effects, %llu "
            "through memory (%.1f ms), %llu layers; %llu pictures shown without the memory (%.1f ms); %llu uploads of %.2f Mpx (%.1f ms); read-backs "
            "(the wait for the processor in them): %llu of other canvases, %.2f Mpx (%.1f ms), %llu of the shown window's, %.2f Mpx (%.1f ms); bitmaps: "
            "%llu uploaded, %llu drawn from those kept, %zu kept in %.1f MB",
            seconds, ull(t.sessions - l.sessions), ticksToMs(t.endTicks - l.endTicks), ull(t.direct - l.direct), ull(t.byEffects - l.byEffects),
            ull(t.throughMemory - l.throughMemory), ticksToMs(t.composeTicks - l.composeTicks), ull(t.layers - l.layers), ull(t.shown - l.shown),
            ticksToMs(t.showTicks - l.showTicks), ull(t.uploads - l.uploads), double(t.uploadPixels - l.uploadPixels) / 1e6,
            ticksToMs(t.uploadTicks - l.uploadTicks), ull(t.readBacks - l.readBacks - windowBacks),
            double((t.readBackPixels - l.readBackPixels) - (t.windowReadBackPixels - l.windowReadBackPixels)) / 1e6,
            ticksToMs((t.readBackTicks - l.readBackTicks) - (t.windowReadBackTicks - l.windowReadBackTicks)), ull(windowBacks),
            double(t.windowReadBackPixels - l.windowReadBackPixels) / 1e6, ticksToMs(t.windowReadBackTicks - l.windowReadBackTicks),
            ull(t.keptMade - l.keptMade), ull(t.keptReused - l.keptReused), keptCount, double(keptBytes) / 1048576.0);
    } else if (s.statsAt && t.sessions != l.sessions) {
        const double seconds = ticksToMs(now - s.statsAt) / 1000.0;
        const uint64_t sessions = t.sessions - l.sessions;
        say("%.1f s: %llu drawings begun and ended over %.1f Mpx in all (%.1f ms ending them), %llu calls drawn by Direct2D, %llu by the host's arithmetic over "
            "%.1f Mpx (%.1f ms), %llu layers, %llu render targets made (%.1f ms), a target moved to a part %llu times",
            seconds, ull(sessions), double(t.sessionPixels - l.sessionPixels) / 1e6, ticksToMs(t.endTicks - l.endTicks), ull(t.direct - l.direct),
            ull(t.composed - l.composed), double(t.composedPixels - l.composedPixels) / 1e6, ticksToMs(t.composeTicks - l.composeTicks), ull(t.layers - l.layers),
            ull(t.targetsMade - l.targetsMade), ticksToMs(t.makeTicks - l.makeTicks), ull(t.partTargets - l.partTargets));
    }
    s.statsLast = t;
    s.statsAt = now;
}

void flush(void*, void* canvas) {
    Canvas* cv = canvasOf(canvas);
    if (!cv) return;
    noteDrawing();
    // What is drawn into a layer that is still open is in the layer's picture, and reaches the memory at the layer's restore.
    guarded("flush", [&] {
        cv->base.end();
        // On the graphics processor: what was drawn outside the layers since the last flush is read back.
        if (cv->base.gc) cv->base.pull(cv->base.bounds());
        // The memory a composed drawing was put together in is kept for the next one, unless it is large: a phone has little.
        for (Surface* scratch : {&cv->cover, &cv->source, &cv->extra})
            if (scratch->own.capacity() > scratchKept) scratch->release();
        // So are the work surfaces on the graphics processor.
        for (Surface& s : cv->work)
            if (s.gpu) {
                const D2D1_SIZE_U size = s.gpu->GetPixelSize();
                if (uint64_t(size.width) * size.height > scratchKept * 4) {
                    s.unbind();
                    s.forgetGpu();
                }
            }
        if (cv->tint.capacity() > scratchKept) std::vector<uint32_t>().swap(cv->tint);
        if (cv->blurA.capacity() > scratchKept * 4) {
            std::vector<uint8_t>().swap(cv->blurA);
            std::vector<uint8_t>().swap(cv->blurB);
        }
    });
}

void touched(void*, void* canvas) {
    // In memory nothing is to be done: Direct2D reads the memory when it draws on it, and nothing drawn is waiting (the
    // core has flushed). On the graphics processor the memory is uploaded before the next drawing (Surface::beginGpu).
    Canvas* cv = canvasOf(canvas);
    if (!cv || !cv->base.gc) return;
    cv->base.gpuAhead = Box();
    cv->base.memAhead = cv->base.bounds();
}

/// A target bitmap over the window host's texture, made again when it gives
/// another texture (`fresh`: it did, and what that holds is not known).
/// (The bitmap holds the texture, so a new one is never at the address of
/// the old while the old is kept.)
Ref<ID2D1Bitmap1> windowBitmapFor(ID3D11Texture2D* texture, bool* fresh) {
    State& s = G();
    std::lock_guard<std::mutex> lock(s.windowMutex);
    *fresh = false;
    if (texture == s.windowTexture && s.windowBitmap) return s.windowBitmap;
    *fresh = true;
    s.windowBitmap.reset();
    s.windowTexture = nullptr;
    Ref<IDXGISurface> surface;
    HRESULT hr = texture->QueryInterface(__uuidof(IDXGISurface), reinterpret_cast<void**>(surface.put()));
    if (SUCCEEDED(hr))
        hr = s.gpuResource->CreateBitmapFromDxgiSurface(
            surface.get(),
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW),
                                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f),
            s.windowBitmap.put());
    if (FAILED(hr)) {
        gpuFailed("ID2D1DeviceContext::CreateBitmapFromDxgiSurface", hr);
        s.windowBitmap.reset();
        return s.windowBitmap;
    }
    s.windowTexture = texture;
    return s.windowBitmap;
}

/// megler_gfx_host.present: the canvas's picture on the graphics processor,
/// the part that changed, drawn into the window host's texture -- bottom row
/// first, as the panel shows it flipped, and copied as it is, alpha and all,
/// as present_pixels takes the memory -- and shown. No read-back and no
/// wait: the processor is handed the copy and the frame as it is handed the
/// drawing, and the swap chain's own frame latency is what holds the app's
/// thread back.
bool present(void*, void* canvas, int x, int y, int width, int height) {
    Canvas* cv = canvasOf(canvas);
    if (!cv || !cv->gc || !cv->base.gc || gpuGone()) return false;
    noteDrawing();
    bool shown = false;
    guarded("present", [&] {
        State& s = G();
        GfxHost::Window window;
        {
            std::lock_guard<std::mutex> lock(s.windowMutex);
            window = s.window;
        }
        if (!window.target || !window.show) return;
        const uint64_t from = ticksNow();
        Surface& base = cv->base;
        // What the memory has and the processor not yet (nothing drawn since canvas_new or touched) goes up first.
        if (!base.memAhead.empty()) {
            base.upload(base.memAhead);
            base.memAhead = Box();
        }
        Box part;
        part.l = x;
        part.t = y;
        part.r = x + width;
        part.b = y + height;
        if (width <= 0 || height <= 0) part = base.bounds();
        part.intersect(base.bounds());
        ID3D11Texture2D* texture = window.target(window.user, cv->memoryW, cv->memoryH);
        if (!texture) return;
        bool fresh = false;
        Ref<ID2D1Bitmap1> onto = windowBitmapFor(texture, &fresh);
        if (!onto || gpuGone()) return;
        // A texture not drawn into before (the first picture, another size, a window made again), or another canvas than the
        // one shown last (a new bitmap for the screen), gets the whole picture.
        {
            std::lock_guard<std::mutex> lock(s.windowMutex);
            if (s.windowCanvas != cv->serial) fresh = true;
            s.windowCanvas = cv->serial;
        }
        if (fresh) part = base.bounds();
        GpuContext& g = *cv->gc;
        // The copy joins the frame's own drawing (a context's target can change while it draws), which is ended once, here.
        if (g.owner) g.owner->detach();
        g.dc->SetTarget(onto.get());
        g.target = onto.get();
        if (!g.inDraw) {
            g.dc->BeginDraw();
            g.inDraw = true;
        }
        if (!part.empty()) {
            // Row y of the picture is row height - 1 - y of the texture; with nearest sampling every pixel lands whole.
            g.dc->SetTransform(Matrix3x2F(1, 0, 0, -1, 0, float(cv->memoryH)));
            g.dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_COPY);
            const D2D1_RECT_F rect = D2D1::RectF(float(part.l), float(part.t), float(part.r), float(part.b));
            g.dc->DrawBitmap(base.gpu.get(), &rect, 1.0f, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, &rect, nullptr);
            g.dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
        }
        g.inDraw = false;
        const uint64_t ending = ticksNow();
        const HRESULT hr = g.dc->EndDraw();
        s.endTicks += ticksNow() - ending;
        if (FAILED(hr)) {
            gpuFailed("ID2D1DeviceContext::EndDraw", hr);
            return;
        }
        window.show(window.user);
        base.window = true;
        ++s.shown;
        s.showTicks += ticksNow() - from;
        shown = true;
    });
    return shown;
}

void save(void*, void* canvas) {
    Canvas* cv = canvasOf(canvas);
    if (!cv) return;
    guarded("save", [&] {
        Level level;
        level.clip = cv->clip();
        cv->levels.push_back(std::move(level));
    });
}

void saveLayer(void*, void* canvas, const float* bounds, int alpha, int blend) {
    Canvas* cv = canvasOf(canvas);
    if (!cv) return;
    guarded("save_layer", [&] {
        Surface* outer = cv->target();
        Level level;
        level.clip = cv->clip();
        level.isLayer = true;
        level.alpha = alpha < 0 ? 0 : alpha > 255 ? 255 : alpha;
        level.blend = knownBlend(blend);
        if (bounds && !sane(bounds[0], bounds[1], bounds[2], bounds[3])) bounds = nullptr;
        if (bounds) {
            double x0, y0, x1, y1;
            mappedBounds(cv->matrix, bounds[0], bounds[1], bounds[2], bounds[3], &x0, &y0, &x1, &y1);
            Box box;
            box.l = pixelEdge(x0);
            box.t = pixelEdge(y0);
            box.r = pixelEdge(x1);
            box.b = pixelEdge(y1);
            level.clip.box.intersect(box);
            touchClip(level.clip);
        }
        // The layer's extent: the clip at the save, within the bounds, and no more than the picture it will be put onto.
        Box extent = level.clip.box;
        if (outer) extent.intersect(outer->bounds());
        else extent.r = extent.l;
        if (!extent.empty()) {
            std::unique_ptr<Surface> layer(new Surface());
            // On the graphics processor where the canvas is: a target of the extent's size, cleared.
            if (cv->gc ? layer->allocateGpu(extent, cv->gc) : layer->allocate(extent)) level.layer = std::move(layer);
        }
        ++G().layers;
        cv->levels.push_back(std::move(level));
    });
}

/// The innermost save undone: its clip goes, and a layer it began is put onto what was there before.
void restoreLevel(Canvas& cv) {
    if (cv.levels.size() <= 1) return;
    Level top = std::move(cv.levels.back());
    cv.levels.pop_back();
    if (!top.isLayer || !top.layer) return;
    struct GiveBack {
        Surface* layer;
        ~GiveBack() { layer->giveBackGpu(); }
    } giveBack{top.layer.get()};
    top.layer->end();
    Surface* outer = cv.target();
    // A DST layer leaves the picture as it was: it is thrown away.
    if (!outer || top.blend == MEGLER_GFX_BLEND_DST) return;
    // The blend takes in the layer's whole extent, as far as the clip at the save lets it.
    Box box = top.layer->bounds();
    box.intersect(cv.clip().box);
    if (box.empty()) return;
    if (top.layer->gc && outer->gc && top.blend == MEGLER_GFX_BLEND_SRC_OVER) {
        // On the graphics processor, laid over the picture at the layer's alpha, under the clip at the save.
        if (!outer->begin()) return;
        outer->useClip(cv.clip());
        outer->rt->SetTransform(Matrix3x2F::Identity());
        outer->rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        const Surface& layer = *top.layer;
        const D2D1_RECT_F onto = D2D1::RectF(float(layer.ox - outer->ox), float(layer.oy - outer->oy), float(layer.ox + layer.w - outer->ox),
                                             float(layer.oy + layer.h - outer->oy));
        outer->rt->DrawBitmap(layer.gpu.get(), onto, float(top.alpha) / 255.0f, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr);
        outer->drew(box);
        return;
    }
    // Another mode: by Direct2D's effects.
    if (top.layer->gc && outer->gc && gpuRestore(cv, *outer, *top.layer, box, top.alpha, top.blend)) return;
    outer->end();
    if (top.layer->gc && !top.layer->pull(top.layer->bounds())) return;
    Source src;
    src.pixels = top.layer.get();
    src.alpha = uint32_t(top.alpha);
    compose(*outer, box, src, clipCover(cv, box), top.blend);
}

void restore(void*, void* canvas) {
    Canvas* cv = canvasOf(canvas);
    if (!cv) return;
    guarded("restore", [&] { restoreLevel(*cv); });
}

void setMatrix(void*, void* canvas, const float* m) {
    Canvas* cv = canvasOf(canvas);
    if (!cv || !m) return;
    for (int i = 0; i < 6; i++)
        if (!std::isfinite(m[i])) return;
    cv->matrix = Matrix3x2F(m[0], m[3], m[1], m[4], m[2], m[5]);
}

void clipRect(void*, void* canvas, float left, float top, float right, float bottom, int op) {
    Canvas* cv = canvasOf(canvas);
    if (!cv) return;
    guarded("clip_rect", [&] {
        Clip& clip = cv->clip();
        if (clip.box.empty() || !sane(left, top, right, bottom)) return;
        if (left > right) std::swap(left, right);
        if (top > bottom) std::swap(top, bottom);
        if (keepsUpright(cv->matrix)) {
            // Whole pixels: those whose centres are in the rectangle.
            double x0, y0, x1, y1;
            mappedBounds(cv->matrix, left, top, right, bottom, &x0, &y0, &x1, &y1);
            Box box;
            box.l = pixelEdge(x0);
            box.t = pixelEdge(y0);
            box.r = pixelEdge(x1);
            box.b = pixelEdge(y1);
            if (op == MEGLER_GFX_CLIP_INTERSECT) {
                clip.box.intersect(box);
                touchClip(clip);
                return;
            }
            Box common = box;
            common.intersect(clip.box);
            if (common.empty()) return;
            Ref<ID2D1Geometry> cut = rectangleGeometry(box);
            clipByGeometry(clip, cut.get(), box, op, false);
            return;
        }
        Ref<ID2D1RectangleGeometry> rect;
        if (FAILED(G().d2d->CreateRectangleGeometry(D2D1::RectF(left, top, right, bottom), rect.put()))) return;
        Ref<ID2D1Geometry> turned = transformed(rect.get(), cv->matrix);
        if (!turned) return;
        clipByGeometry(clip, turned.get(), coverOf(cv->matrix, left, top, right, bottom, 0), op, true);
    });
}

void clipPath(void*, void* canvas, const float* path, int count, int fill, int op) {
    Canvas* cv = canvasOf(canvas);
    if (!cv) return;
    guarded("clip_path", [&] {
        Clip& clip = cv->clip();
        if (clip.box.empty()) return;
        Ref<ID2D1PathGeometry> shape = pathGeometry(path, count, fill);
        if (!shape) {
            clipByGeometry(clip, nullptr, Box(), op, true);
            return;
        }
        Ref<ID2D1Geometry> placed = transformed(shape.get(), cv->matrix);
        D2D1_RECT_F bounds = {};
        if (!placed || FAILED(placed->GetBounds(nullptr, &bounds))) return;
        Box cover;
        if (bounds.right > bounds.left && bounds.bottom > bounds.top) {
            cover.l = floorPixel(bounds.left);
            cover.t = floorPixel(bounds.top);
            cover.r = ceilPixel(bounds.right);
            cover.b = ceilPixel(bounds.bottom);
        }
        clipByGeometry(clip, cover.empty() ? nullptr : placed.get(), cover, op, true);
    });
}

// MARK: megler_gfx_host: drawing

void drawColor(void*, void* canvas, uint32_t color, int blend) {
    Canvas* cv = canvasOf(canvas);
    if (!cv) return;
    blend = knownBlend(blend);
    if (blend == MEGLER_GFX_BLEND_DST) return;
    guarded("draw_color", [&] {
        Surface* target = cv->target();
        if (!target) return;
        Box box = cv->clip().box;
        box.intersect(target->bounds());
        if (box.empty()) return;
        if (target->gc) {
            const Clip& clip = cv->clip();
            const bool replaces = blend == MEGLER_GFX_BLEND_SRC || blend == MEGLER_GFX_BLEND_CLEAR || (blend == MEGLER_GFX_BLEND_SRC_OVER && (color >> 24) == 255);
            if (replaces && !clip.g) {
                // Every pixel of the box gets the colour: what the memory had there need not go up first (a window's frame
                // begins so), and Direct2D's Clear, which an axis-aligned clip bounds, is the whole of it.
                if (box.covers(target->memAhead)) target->memAhead = Box();
                if (!target->begin()) return;
                target->useClip(clip);
                target->rt->Clear(blend == MEGLER_GFX_BLEND_CLEAR ? D2D1::ColorF(0, 0, 0, 0) : colorOf(color));
                target->drew(box);
                ++G().direct;
                return;
            }
            if (blend == MEGLER_GFX_BLEND_SRC_OVER) {
                // Over what is there, as far as the clip lets it: a rectangle filled through the clip.
                if (!target->begin()) return;
                target->useClip(clip);
                target->rt->SetTransform(Matrix3x2F::Identity());
                target->rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
                target->brush->SetColor(colorOf(color));
                target->rt->FillRectangle(D2D1::RectF(float(box.l - target->ox), float(box.t - target->oy), float(box.r - target->ox), float(box.b - target->oy)),
                                          target->brush.get());
                target->drew(box);
                ++G().direct;
                return;
            }
            if (gpuDrawColor(*cv, *target, box, color, blend)) return;
        }
        target->end();
        Source src;
        src.solid = premultiplied(color);
        compose(*target, box, src, clipCover(*cv, box), blend);
        ++G().composed;
    });
}

void drawRect(void*, void* canvas, float left, float top, float right, float bottom, const megler_gfx_paint* given, void* shader) {
    Canvas* cv = canvasOf(canvas);
    if (!cv || !given) return;
    guarded("draw_rect", [&] {
        const megler_gfx_paint fixed = checked(*given);
        const megler_gfx_paint* paint = &fixed;
        if (!sane(left, top, right, bottom)) return;
        Ink ink;
        ink.kind = Ink::rectangle;
        ink.box = D2D1::RectF(left, top, right, bottom);
        ink.l = left;
        ink.t = top;
        ink.r = right;
        ink.b = bottom;
        inkStroke(ink, *paint);
        paintInk(*cv, ink, *paint, shaderOf(shader));
    });
}

void drawRoundRect(void*, void* canvas, float left, float top, float right, float bottom, float rx, float ry, const megler_gfx_paint* given, void* shader) {
    Canvas* cv = canvasOf(canvas);
    if (!cv || !given) return;
    guarded("draw_round_rect", [&] {
        const megler_gfx_paint fixed = checked(*given);
        const megler_gfx_paint* paint = &fixed;
        if (!sane(left, top, right, bottom) || !sane(rx) || !sane(ry)) return;
        Ink ink;
        ink.kind = rx > 0 && ry > 0 ? Ink::rounded : Ink::rectangle;
        ink.box = D2D1::RectF(left, top, right, bottom);
        ink.rx = rx;
        ink.ry = ry;
        ink.l = left;
        ink.t = top;
        ink.r = right;
        ink.b = bottom;
        inkStroke(ink, *paint);
        paintInk(*cv, ink, *paint, shaderOf(shader));
    });
}

void drawOval(void*, void* canvas, float left, float top, float right, float bottom, const megler_gfx_paint* given, void* shader) {
    Canvas* cv = canvasOf(canvas);
    if (!cv || !given) return;
    guarded("draw_oval", [&] {
        const megler_gfx_paint fixed = checked(*given);
        const megler_gfx_paint* paint = &fixed;
        if (!sane(left, top, right, bottom)) return;
        Ink ink;
        ink.kind = Ink::oval;
        ink.box = D2D1::RectF(left, top, right, bottom);
        ink.l = left;
        ink.t = top;
        ink.r = right;
        ink.b = bottom;
        inkStroke(ink, *paint);
        paintInk(*cv, ink, *paint, shaderOf(shader));
    });
}

void drawPath(void*, void* canvas, const float* path, int count, int fill, const megler_gfx_paint* given, void* shader) {
    Canvas* cv = canvasOf(canvas);
    if (!cv || !given) return;
    guarded("draw_path", [&] {
        const megler_gfx_paint fixed = checked(*given);
        const megler_gfx_paint* paint = &fixed;
        Ref<ID2D1PathGeometry> shape = pathGeometry(path, count, fill);
        D2D1_RECT_F bounds = {};
        if (!shape || FAILED(shape->GetBounds(nullptr, &bounds)) || !(bounds.right >= bounds.left)) return;
        Ink ink;
        ink.kind = Ink::geometry;
        ink.shape = shape.get();
        ink.l = bounds.left;
        ink.t = bounds.top;
        ink.r = bounds.right;
        ink.b = bounds.bottom;
        inkStroke(ink, *paint);
        paintInk(*cv, ink, *paint, shaderOf(shader));
    });
}

/// A copy of a part of an image as a Direct2D bitmap, each pixel first made
/// what the paint's alpha and colour filter make of it when `paint` is given.
Ref<ID2D1Bitmap> bitmapCopy(Surface& maker, const megler_gfx_image& image, const Box& part, const megler_gfx_paint* paint, std::vector<uint32_t>& scratch) {
    Ref<ID2D1Bitmap> bitmap;
    const D2D1_BITMAP_PROPERTIES properties = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    const uint8_t* first = static_cast<const uint8_t*>(image.pixels) + size_t(part.t) * size_t(image.stride) + size_t(part.l) * 4;
    HRESULT hr;
    if (paint) {
        const uint32_t alpha = paint->color >> 24, filter = premultiplied(paint->filter_color);
        scratch.resize(size_t(part.width()) * size_t(part.height()));
        for (int y = 0; y < part.height(); y++) {
            const uint32_t* in = reinterpret_cast<const uint32_t*>(first + size_t(y) * size_t(image.stride));
            uint32_t* out = scratch.data() + size_t(y) * size_t(part.width());
            for (int x = 0; x < part.width(); x++) out[x] = blended(paint->filter_mode, filter, faded(in[x], alpha));
        }
        hr = maker.rt->CreateBitmap(D2D1::SizeU(UINT32(part.width()), UINT32(part.height())), scratch.data(), UINT32(part.width()) * 4, properties, bitmap.put());
    } else {
        hr = maker.rt->CreateBitmap(D2D1::SizeU(UINT32(part.width()), UINT32(part.height())), first, UINT32(image.stride), properties, bitmap.put());
    }
    if (FAILED(hr)) {
        failed("ID2D1RenderTarget::CreateBitmap", hr);
        bitmap.reset();
    }
    return bitmap;
}

/// From how many pixels on a part of an image is not copied for Direct2D but
/// shared with it, which costs an EndDraw before the call returns (the
/// image's memory is only good during the call).
constexpr int64_t sharedFromPixels = 256 * 256;

/// How many bytes of the core's bitmaps are kept on the graphics processor:
/// an app's icons, backgrounds and pictures of a screen or two. A bitmap
/// larger than half of it (six million pixels) is not kept: the part drawn
/// is copied for each drawing, as in memory.
constexpr uint64_t keptBudget = 48ull << 20;

/// The core's bitmap on the graphics processor, kept under its id: the one
/// kept if its version is the image's, else uploaded now and kept, the least
/// recently drawn going while the kept are over the budget. Null when it is
/// not to be kept, or cannot be made.
Ref<ID2D1Bitmap1> keptBitmap(const megler_gfx_image& image) {
    State& s = G();
    Ref<ID2D1Bitmap1> out;
    const uint64_t bytes = uint64_t(image.width) * uint64_t(image.height) * 4;
    if (!s.gpuResource || gpuGone() || !image.id || !image.pixels || bytes > keptBudget / 2 || UINT32(image.width) > s.gpuMaxSize ||
        UINT32(image.height) > s.gpuMaxSize)
        return out;
    std::lock_guard<std::mutex> lock(s.keptMutex);
    auto found = s.kept.find(image.id);
    if (found != s.kept.end()) {
        if (found->second.version == image.version) {
            found->second.used = ++s.keptClock;
            ++s.keptReused;
            return found->second.bitmap;
        }
        s.keptBytes -= found->second.bytes;
        s.kept.erase(found);
    }
    const uint64_t from = ticksNow();
    const HRESULT hr = s.gpuResource->CreateBitmap(
        D2D1::SizeU(UINT32(image.width), UINT32(image.height)), image.pixels, UINT32(image.stride),
        D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f), out.put());
    s.uploadTicks += ticksNow() - from;
    if (FAILED(hr)) {
        gpuFailed("ID2D1DeviceContext::CreateBitmap", hr);
        out.reset();
        return out;
    }
    ++s.keptMade;
    while (s.keptBytes + bytes > keptBudget && !s.kept.empty()) {
        auto oldest = s.kept.begin();
        for (auto it = s.kept.begin(); it != s.kept.end(); ++it)
            if (it->second.used < oldest->second.used) oldest = it;
        s.keptBytes -= oldest->second.bytes;
        s.kept.erase(oldest);
    }
    Kept& entry = s.kept[image.id];
    entry.version = image.version;
    entry.bitmap = out;
    entry.bytes = bytes;
    entry.used = ++s.keptClock;
    s.keptBytes += bytes;
    return out;
}

/// The part `part` (whole pixels) of an image, `src` of it stretched onto
/// `dst` through the canvas's matrix, onto a surface on the graphics
/// processor that is drawing, under the canvas's clip: from the kept bitmap,
/// a part of it through an image brush that is the part alone, its edge
/// pixels going on beyond it, so that nothing outside the part shows; with a
/// colour filter, a copy of the part made what the paint's alpha and the
/// filter make it, as in memory.
bool gpuBitmapOn(Surface& s, Canvas& cv, const megler_gfx_image& image, const Box& part, const float* src, const float* dst, const megler_gfx_paint& paint) {
    const bool filtered = paint.filter_mode >= 0, filterBitmap = (paint.flags & MEGLER_GFX_FILTER_BITMAP) != 0;
    const D2D1_RECT_F from = D2D1::RectF(src[0] - float(part.l), src[1] - float(part.t), src[2] - float(part.l), src[3] - float(part.t));
    const D2D1_RECT_F onto = D2D1::RectF(dst[0], dst[1], dst[2], dst[3]);
    const D2D1_BITMAP_INTERPOLATION_MODE sampling = filterBitmap ? D2D1_BITMAP_INTERPOLATION_MODE_LINEAR : D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR;
    s.useClip(cv.clip());
    s.setMatrix(cv.matrix);
    s.rt->SetAntialiasMode((paint.flags & MEGLER_GFX_ANTI_ALIAS) ? D2D1_ANTIALIAS_MODE_PER_PRIMITIVE : D2D1_ANTIALIAS_MODE_ALIASED);
    const float opacity = filtered ? 1.0f : float(paint.color >> 24) / 255.0f;
    Ref<ID2D1Bitmap1> kept = filtered ? Ref<ID2D1Bitmap1>() : keptBitmap(image);
    const bool whole = part.l == 0 && part.t == 0 && part.r == image.width && part.b == image.height;
    if (kept && whole) {
        s.rt->DrawBitmap(kept.get(), &onto, opacity, sampling, &from);
        return true;
    }
    if (!kept) {
        Ref<ID2D1Bitmap> copy = bitmapCopy(s, image, part, filtered ? &paint : nullptr, cv.tint);
        if (!copy) return false;
        s.rt->DrawBitmap(copy.get(), &onto, opacity, sampling, &from);
        return true;
    }
    GpuContext& gc = *s.gc;
    const D2D1_RECT_F within = D2D1::RectF(float(part.l), float(part.t), float(part.r), float(part.b));
    const D2D1_INTERPOLATION_MODE interpolation = filterBitmap ? D2D1_INTERPOLATION_MODE_LINEAR : D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR;
    // (An image brush's own coordinates have the source rectangle's corner at their origin, as a brush over a copy of the
    // part would.)
    const float sx = (dst[2] - dst[0]) / (src[2] - src[0]), sy = (dst[3] - dst[1]) / (src[3] - src[1]);
    const Matrix3x2F placed =
        Matrix3x2F::Translation(float(part.l) - src[0], float(part.t) - src[1]) * Matrix3x2F::Scale(sx, sy) * Matrix3x2F::Translation(dst[0], dst[1]);
    if (!gc.imageBrush) {
        const HRESULT hr = gc.dc->CreateImageBrush(kept.get(), D2D1::ImageBrushProperties(within, D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, interpolation),
                                                   D2D1::BrushProperties(opacity, placed), gc.imageBrush.put());
        if (FAILED(hr)) {
            gpuFailed("ID2D1DeviceContext::CreateImageBrush", hr);
            gc.imageBrush.reset();
            return false;
        }
    } else {
        gc.imageBrush->SetImage(kept.get());
        gc.imageBrush->SetSourceRectangle(&within);
        gc.imageBrush->SetInterpolationMode(interpolation);
        gc.imageBrush->SetTransform(placed);
        gc.imageBrush->SetOpacity(opacity);
    }
    s.rt->FillRectangle(onto, gc.imageBrush.get());
    // (The brush lets go of the bitmap, which may be dropped from the kept ones: Direct2D holds what it still has to draw.)
    gc.imageBrush->SetImage(nullptr);
    return true;
}

void drawBitmap(void*, void* canvas, const megler_gfx_image* image, const float* src, const float* dst, const megler_gfx_paint* given) {
    Canvas* cv = canvasOf(canvas);
    if (!cv || !image || !image->pixels || !src || !dst || !given || image->width <= 0 || image->height <= 0 || image->stride < image->width * 4) return;
    guarded("draw_bitmap", [&] {
        const megler_gfx_paint fixed = checked(*given);
        const megler_gfx_paint* paint = &fixed;
        if (!sane(src[0], src[1], src[2], src[3]) || !sane(dst[0], dst[1], dst[2], dst[3])) return;
        Surface* target = cv->target();
        if (!target || cv->clip().box.empty()) return;
        if (!(src[2] > src[0]) || !(src[3] > src[1]) || !(dst[2] > dst[0]) || !(dst[3] > dst[1])) return;
        // The whole pixels of the image the part touches: Direct2D is shown those alone, so nothing outside them can show at an edge.
        Box part;
        part.l = std::max(0, floorPixel(src[0]));
        part.t = std::max(0, floorPixel(src[1]));
        part.r = std::min(image->width, ceilPixel(src[2]));
        part.b = std::min(image->height, ceilPixel(src[3]));
        if (part.empty()) return;
        const D2D1_RECT_F from = D2D1::RectF(src[0] - float(part.l), src[1] - float(part.t), src[2] - float(part.l), src[3] - float(part.t));
        const D2D1_RECT_F onto = D2D1::RectF(dst[0], dst[1], dst[2], dst[3]);
        const bool smoothEdges = (paint->flags & MEGLER_GFX_ANTI_ALIAS) != 0, filterBitmap = (paint->flags & MEGLER_GFX_FILTER_BITMAP) != 0;
        const D2D1_BITMAP_INTERPOLATION_MODE sampling = filterBitmap ? D2D1_BITMAP_INTERPOLATION_MODE_LINEAR : D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR;

        Ink ink;
        ink.kind = Ink::rectangle;
        ink.box = onto;
        ink.l = dst[0];
        ink.t = dst[1];
        ink.r = dst[2];
        ink.b = dst[3];
        ink.smooth = smoothEdges;
        if (paint->shadow_radius > 0 && (paint->shadow_color >> 24) != 0) {
            note("bitmap-shadow", "a shadow under a bitmap is that of its rectangle, not of its own shape (said once)");
            drawShadow(*cv, *target, ink, *paint);
        }

        if (paint->blend == MEGLER_GFX_BLEND_SRC_OVER && target->gc) {
            if (!target->begin() || !gpuBitmapOn(*target, *cv, *image, part, src, dst, *paint)) return;
            Box drawn = inkCover(ink, cv->matrix);
            drawn.intersect(cv->clip().box);
            target->drew(drawn);
            ++G().direct;
            return;
        }
        if (target->gc) {
            // Another transfer mode, on the graphics processor: the bitmap drawn so into a work surface under the clip (its
            // colours times its rectangle's coverage), the rectangle's coverage in white where the mode needs it, and the two
            // put on by Direct2D's effects.
            Box box = inkCover(ink, cv->matrix);
            box.intersect(cv->clip().box);
            box.intersect(target->bounds());
            if (box.empty()) return;
            const bool keeps = keepsUncovered(paint->blend);
            Surface* c = keeps ? nullptr : gpuCoverage(*cv, ink, box, true, Canvas::workC);
            Surface* s = keeps || c ? workSurface(*cv, Canvas::workA, box) : nullptr;
            if (s && gpuBitmapOn(*s, *cv, *image, part, src, dst, *paint) && gpuPut(*cv, *target, box, s->gpu.get(), c ? c->gpu.get() : nullptr, paint->blend))
                return;
        }

        if (paint->blend == MEGLER_GFX_BLEND_SRC_OVER) {
            // Direct2D lays it over the picture. A colour filter is done to the copy's pixels first, the paint's alpha with it.
            const bool filtered = paint->filter_mode >= 0;
            const uint8_t* imageFirst = static_cast<const uint8_t*>(image->pixels);
            const uint8_t* imageLast = imageFirst + size_t(image->height) * size_t(image->stride);
            const bool overlaps = imageFirst < target->px + size_t(target->h) * size_t(target->stride) && target->px < imageLast;
            const bool share = !filtered && !overlaps && !G().copying && int64_t(part.width()) * int64_t(part.height()) >= sharedFromPixels;
            if (!target->begin()) return;
            Ref<ID2D1Bitmap> bitmap;
            if (share) {
                Ref<IWICBitmapLock> lock;
                lock.attach(new MemoryLock(const_cast<uint8_t*>(imageFirst) + size_t(part.t) * size_t(image->stride) + size_t(part.l) * 4, UINT(part.width()),
                                           UINT(part.height()), UINT(image->stride)));
                const D2D1_BITMAP_PROPERTIES properties =
                    D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
                const HRESULT hr = target->rt->CreateSharedBitmap(__uuidof(IWICBitmapLock), lock.get(), &properties, bitmap.put());
                if (FAILED(hr)) {
                    note("shared-bitmap", "Direct2D would not share a bitmap's memory (0x%08lX): large bitmaps are copied for each drawing (said once)",
                         static_cast<unsigned long>(hr));
                    bitmap.reset();
                }
            }
            const bool shared = bool(bitmap);
            if (!bitmap) bitmap = bitmapCopy(*target, *image, part, filtered ? paint : nullptr, cv->tint);
            if (!bitmap) return;
            target->useClip(cv->clip());
            target->setMatrix(cv->matrix);
            target->rt->SetAntialiasMode(smoothEdges ? D2D1_ANTIALIAS_MODE_PER_PRIMITIVE : D2D1_ANTIALIAS_MODE_ALIASED);
            target->rt->DrawBitmap(bitmap.get(), &onto, filtered ? 1.0f : float(paint->color >> 24) / 255.0f, sampling, &from);
            // What was shared is read when Direct2D draws, which is at EndDraw: before the memory is anyone else's again.
            if (shared) target->end();
            ++G().direct;
            return;
        }

        // Another transfer mode: the image's colours over the whole of the pixels the rectangle touches (as a brush, whose
        // edge pixels go on), the rectangle in white for the coverage, and the two put onto the picture here.
        Box box = inkCover(ink, cv->matrix);
        box.intersect(cv->clip().box);
        box.intersect(target->bounds());
        if (box.empty()) return;
        target->end();
        Surface& colours = cv->source;
        if (!colours.allocate(box) || !colours.begin()) return;
        Ref<ID2D1Bitmap> bitmap = bitmapCopy(colours, *image, part, nullptr, cv->tint);
        Ref<ID2D1BitmapBrush> brush;
        if (!bitmap) return;
        HRESULT hr = colours.rt->CreateBitmapBrush(bitmap.get(), D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, sampling), brush.put());
        if (FAILED(hr)) {
            failed("ID2D1RenderTarget::CreateBitmapBrush", hr);
            colours.end();
            return;
        }
        const float sx = (dst[2] - dst[0]) / (src[2] - src[0]), sy = (dst[3] - dst[1]) / (src[3] - src[1]);
        brush->SetTransform(Matrix3x2F::Translation(float(part.l) - src[0], float(part.t) - src[1]) * Matrix3x2F::Scale(sx, sy) *
                            Matrix3x2F::Translation(dst[0], dst[1]) * cv->matrix);
        colours.rt->SetTransform(Matrix3x2F::Translation(float(-box.l), float(-box.t)));
        colours.rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        colours.rt->FillRectangle(D2D1::RectF(float(box.l), float(box.t), float(box.r), float(box.b)), brush.get());
        colours.end();
        const Surface* cover = inkCoverage(*cv, ink, box, true);
        if (!cover) return;
        Source source;
        source.pixels = &colours;
        source.alpha = paint->color >> 24;
        source.filterMode = paint->filter_mode;
        source.filter = premultiplied(paint->filter_color);
        compose(*target, box, source, cover, paint->blend);
        ++G().composed;
    });
}

// MARK: megler_gfx_host: shaders

/// Whether canvases are made on the graphics processor now: where a shader's
/// first brush is made, at once, so that one that cannot be is NULL.
inline bool drawingOnGpu() { return G().gpu && !gpuGone(); }

void* shaderLinear(void*, float x0, float y0, float x1, float y1, const uint32_t* colors, const float* positions, int count, int tile) {
    Shader* made = nullptr;
    guarded("shader_linear", [&] {
        if (!started() || !colors || count < 1 || !sane(x0, y0, x1, y1)) return;
        std::unique_ptr<Shader> s(new Shader());
        s->kind = Shader::linear;
        s->stops = stopsOf(colors, positions, count, &s->opaque);
        s->extend = extendOf(tile);
        s->from = D2D1::Point2F(x0, y0);
        s->to = D2D1::Point2F(x1, y1);
        if (!s->brushFor(drawingOnGpu())) return;
        made = s.release();
    });
    return made;
}

void* shaderRadial(void*, float cx, float cy, float radius, const uint32_t* colors, const float* positions, int count, int tile) {
    Shader* made = nullptr;
    guarded("shader_radial", [&] {
        if (!started() || !colors || count < 1 || !sane(cx) || !sane(cy) || !sane(radius)) return;
        std::unique_ptr<Shader> s(new Shader());
        s->kind = Shader::radial;
        s->stops = stopsOf(colors, positions, count, &s->opaque);
        s->extend = extendOf(tile);
        s->from = D2D1::Point2F(cx, cy);
        s->radius = radius > 0 ? radius : 0.001f;
        if (!s->brushFor(drawingOnGpu())) return;
        made = s.release();
    });
    return made;
}

void* shaderSweep(void*, float cx, float cy, const uint32_t* colors, const float* positions, int count) {
    Shader* made = nullptr;
    guarded("shader_sweep", [&] {
        if (!started() || !colors || count < 1 || !sane(cx) || !sane(cy)) return;
        std::unique_ptr<Shader> s(new Shader());
        s->kind = Shader::sweep;
        s->cx = cx;
        s->cy = cy;
        std::vector<float> even;
        if (!positions) {
            for (int i = 0; i < count; i++) even.push_back(count > 1 ? float(i) / float(count - 1) : 0);
            positions = even.data();
        }
        s->table.resize(1024);
        s->opaque = true;
        for (int i = 0; i < 1024; i++) {
            const uint32_t c = gradientAt(colors, positions, count, (float(i) + 0.5f) / 1024.0f);
            if ((c >> 24) != 255) s->opaque = false;
            s->table[size_t(i)] = premultiplied(c);
        }
        made = s.release();
    });
    return made;
}

void* shaderBitmap(void*, const megler_gfx_image* image, int tile) {
    Shader* made = nullptr;
    guarded("shader_bitmap", [&] {
        if (!started() || !image || !image->pixels || image->width <= 0 || image->height <= 0 || image->stride < image->width * 4) return;
        std::unique_ptr<Shader> s(new Shader());
        s->kind = Shader::bitmap;
        s->extend = extendOf(tile);
        // (The copy the shader keeps: it does not follow the bitmap afterwards. On the graphics processor the kept bitmap
        // of this version is it, shared with draw_bitmap and with other shaders of the same bitmap.)
        s->width = image->width;
        s->height = image->height;
        s->pixels.resize(size_t(image->width) * size_t(image->height));
        for (int y = 0; y < image->height; y++)
            memcpy(s->pixels.data() + size_t(y) * size_t(image->width), static_cast<const uint8_t*>(image->pixels) + size_t(y) * size_t(image->stride),
                   size_t(image->width) * 4);
        if (drawingOnGpu()) {
            if (Ref<ID2D1Bitmap1> kept = keptBitmap(*image)) {
                const HRESULT hr = G().gpuResource->CreateBitmapBrush(
                    kept.get(), D2D1::BitmapBrushProperties(s->extend, s->extend, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR), s->bitmapBrushes[1].put());
                if (SUCCEEDED(hr)) s->brushes[1] = Ref<ID2D1Brush>(s->bitmapBrushes[1]);
                else s->bitmapBrushes[1].reset();
            }
        }
        if (!s->brushFor(drawingOnGpu())) return;
        s->opaque = true;
        for (int y = 0; y < image->height && s->opaque; y++) {
            const uint32_t* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(image->pixels) + size_t(y) * size_t(image->stride));
            for (int x = 0; x < image->width; x++)
                if ((row[x] >> 24) != 255) {
                    s->opaque = false;
                    break;
                }
        }
        made = s.release();
    });
    return made;
}

void shaderMatrix(void*, void* shader, const float* m) {
    Shader* s = shaderOf(shader);
    if (!s) return;
    for (int i = 0; m && i < 6; i++)
        if (!std::isfinite(m[i])) return;
    std::lock_guard<std::mutex> lock(s->mutex);
    s->local = m ? Matrix3x2F(m[0], m[3], m[1], m[4], m[2], m[5]) : Matrix3x2F::Identity();
}

void shaderFree(void*, void* shader) {
    Shader* s = shaderOf(shader);
    if (!s) return;
    s->magic = 0;
    delete s;
}

// MARK: Typefaces

/// A face: a family of a font collection at a weight, upright or italic.
/// DirectWrite picks the family's nearest font and makes up what the family
/// lacks (a bold, a slant). Never freed: megler.Gfx has no call for it.
struct Typeface {
    uint32_t magic = faceMagic;
    Ref<IDWriteFontCollection> collection; // null: the system's
    std::wstring family;
    int weight = 400;
    bool italic = false;
    DWRITE_FONT_STRETCH stretch = DWRITE_FONT_STRETCH_NORMAL;
    Ref<IDWriteTextFormat> format;
    // The face's vertical measures for an em of 1, y down from the baseline (megler.h, text_metrics).
    float top = -1, ascent = -0.8f, descent = 0.2f, bottom = 0.25f, leading = 0;
};

Typeface* faceOf(void* handle) {
    Typeface* f = static_cast<Typeface*>(handle);
    return f && f->magic == faceMagic ? f : G().defaultFace;
}

bool familyExists(IDWriteFontCollection* collection, const wchar_t* name) {
    UINT32 index = 0;
    BOOL exists = FALSE;
    return collection && SUCCEEDED(collection->FindFamilyName(name, &index, &exists)) && exists;
}

/// A font's ascent, descent and line gap as Android reads them, which is
/// FreeType's way: the 'hhea' table's, unless the font says its typographic
/// ones are to be used (the OS/2 table's fsSelection, bit 7) or 'hhea' has
/// none. DirectWrite's own are Windows': the OS/2 table's winAscent and
/// winDescent, which for Roboto make a line an eighth taller than it is on
/// Android (26.4 for an em of 20, where Android has 23.4). Leaves the face
/// as it is where the font has no such table.
void androidLineMeasures(IDWriteFontFace* fontFace, float em, Typeface& face) {
    struct Table {
        IDWriteFontFace* from;
        const uint8_t* bytes = nullptr;
        UINT32 size = 0;
        void* context = nullptr;
        Table(IDWriteFontFace* fontFace, UINT32 tag) : from(fontFace) {
            const void* data = nullptr;
            BOOL exists = FALSE;
            if (SUCCEEDED(from->TryGetFontTable(tag, &data, &size, &context, &exists)) && exists) bytes = static_cast<const uint8_t*>(data);
            else size = 0;
        }
        ~Table() {
            if (bytes) from->ReleaseFontTable(context);
        }
        int16_t at(UINT32 offset) const { return int16_t(uint16_t(bytes[offset] << 8 | bytes[offset + 1])); }
    };
    {
        const Table os2(fontFace, DWRITE_MAKE_OPENTYPE_TAG('O', 'S', '/', '2'));
        if (os2.size >= 64 && (uint16_t(os2.at(62)) & 0x80)) return; // typographic measures: what DirectWrite has given already
    }
    const Table hhea(fontFace, DWRITE_MAKE_OPENTYPE_TAG('h', 'h', 'e', 'a'));
    if (hhea.size < 10) return;
    const int ascender = hhea.at(4), descender = hhea.at(6), gap = hhea.at(8);
    if (ascender <= 0 || descender > 0 || (ascender == 0 && descender == 0)) return;
    face.ascent = -float(ascender) / em;
    face.descent = -float(descender) / em;
    face.leading = gap > 0 ? float(gap) / em : 0;
}

/// A face of `family` in `collection` (null: the system's), with its text
/// format and its measures. Null when DirectWrite will not make the format.
Typeface* makeFace(IDWriteFontCollection* collection, const std::wstring& family, int weight, bool italic, DWRITE_FONT_STRETCH stretch) {
    State& s = G();
    std::unique_ptr<Typeface> face(new Typeface());
    if (collection) {
        face->collection.attach(collection);
        collection->AddRef();
    }
    face->family = family;
    face->weight = weight < 1 ? 1 : weight > 999 ? 999 : weight;
    face->italic = italic;
    face->stretch = stretch;
    const DWRITE_FONT_STYLE style = italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;
    HRESULT hr = s.dwrite->CreateTextFormat(family.c_str(), collection, DWRITE_FONT_WEIGHT(face->weight), style, stretch, 16.0f, s.locale.c_str(), face->format.put());
    if (FAILED(hr)) {
        failed("IDWriteFactory::CreateTextFormat", hr);
        return nullptr;
    }
    face->format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    // The measures of the font DirectWrite will take for it.
    IDWriteFontCollection* from = collection ? collection : s.systemFonts.get();
    UINT32 index = 0;
    BOOL exists = FALSE;
    Ref<IDWriteFontFamily> fontFamily;
    Ref<IDWriteFont> font;
    if (from && SUCCEEDED(from->FindFamilyName(family.c_str(), &index, &exists)) && exists && SUCCEEDED(from->GetFontFamily(index, fontFamily.put())) &&
        SUCCEEDED(fontFamily->GetFirstMatchingFont(DWRITE_FONT_WEIGHT(face->weight), stretch, style, font.put()))) {
        DWRITE_FONT_METRICS m = {};
        font->GetMetrics(&m);
        if (m.designUnitsPerEm) {
            const float em = float(m.designUnitsPerEm);
            face->ascent = -float(m.ascent) / em;
            face->descent = float(m.descent) / em;
            face->leading = m.lineGap > 0 ? float(m.lineGap) / em : 0;
            Ref<IDWriteFontFace> fontFace;
            if (SUCCEEDED(font->CreateFontFace(fontFace.put()))) androidLineMeasures(fontFace.get(), em, *face);
            face->top = face->ascent;
            face->bottom = face->descent;
            // The highest and lowest any glyph reaches: the face's glyph box, where DirectWrite tells it.
            if (Ref<IDWriteFont1> font1 = font.as<IDWriteFont1>()) {
                DWRITE_FONT_METRICS1 m1 = {};
                font1->GetMetrics(&m1);
                if (m1.glyphBoxTop > m1.glyphBoxBottom) {
                    face->top = std::min(face->ascent, -float(m1.glyphBoxTop) / em);
                    face->bottom = std::max(face->descent, -float(m1.glyphBoxBottom) / em);
                }
            }
        }
    }
    return face.release();
}

/// The system's family for one of Android's: the name itself when the phone
/// has a family called that, else the first there is of a few of the kind.
std::wstring systemFamily(const char* family, int generic) {
    State& s = G();
    std::wstring wide;
    for (const char* at = family ? family : ""; *at; at++) wide += wchar_t(static_cast<unsigned char>(*at) < 0x80 ? *at : L'?');
    if (!wide.empty() && familyExists(s.systemFonts.get(), wide.c_str())) return wide;
    static const wchar_t* const sans[] = {L"Segoe UI", L"Segoe WP", L"Arial", L"Verdana", nullptr};
    static const wchar_t* const serif[] = {L"Times New Roman", L"Georgia", L"Cambria", nullptr};
    static const wchar_t* const mono[] = {L"Consolas", L"Courier New", L"Lucida Console", nullptr};
    static const wchar_t* const hand[] = {L"Segoe Script", L"Segoe Print", L"Comic Sans MS", nullptr};
    static const wchar_t* const casual[] = {L"Comic Sans MS", L"Segoe Print", nullptr};
    const std::string name = family ? family : "";
    const wchar_t* const* list = generic == MEGLER_GFX_MONOSPACE ? mono : generic == MEGLER_GFX_SERIF ? serif : sans;
    if (name.find("cursive") != std::string::npos) list = hand;
    else if (name.find("casual") != std::string::npos) list = casual;
    for (int pass = 0; pass < 2; pass++) {
        for (const wchar_t* const* at = list; *at; at++)
            if (familyExists(s.systemFonts.get(), *at)) return *at;
        list = sans;
    }
    return L"Segoe UI";
}

// MARK: Fonts out of an app's bytes

/// A font file that is memory, for DirectWrite: the bytes stay for as long
/// as the process does (typefaces are never freed).
class MemoryFontStream final : public IDWriteFontFileStream, public Counted {
public:
    explicit MemoryFontStream(std::shared_ptr<std::vector<uint8_t>> data) : data_(std::move(data)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDWriteFontFileStream)) {
            *out = static_cast<IDWriteFontFileStream*>(this);
            addRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return addRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return release(); }
    HRESULT STDMETHODCALLTYPE ReadFileFragment(const void** start, UINT64 offset, UINT64 size, void** context) override {
        if (context) *context = nullptr;
        if (!start || offset > data_->size() || size > data_->size() - offset) {
            if (start) *start = nullptr;
            return E_FAIL;
        }
        *start = data_->data() + size_t(offset);
        return S_OK;
    }
    void STDMETHODCALLTYPE ReleaseFileFragment(void*) override {}
    HRESULT STDMETHODCALLTYPE GetFileSize(UINT64* size) override {
        *size = data_->size();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetLastWriteTime(UINT64* time) override {
        *time = 0;
        return S_OK;
    }

private:
    std::shared_ptr<std::vector<uint8_t>> data_;
};

/// A font file's key here is the number its bytes were kept under.
class MemoryFontLoader final : public IDWriteFontFileLoader, public Counted {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDWriteFontFileLoader)) {
            *out = static_cast<IDWriteFontFileLoader*>(this);
            addRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return addRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return release(); }
    HRESULT STDMETHODCALLTYPE CreateStreamFromKey(const void* key, UINT32 keySize, IDWriteFontFileStream** stream) override {
        if (!stream) return E_POINTER;
        *stream = nullptr;
        if (!key || keySize != sizeof(uint64_t)) return E_INVALIDARG;
        uint64_t number;
        memcpy(&number, key, sizeof(number));
        std::shared_ptr<std::vector<uint8_t>> data;
        {
            State& s = G();
            std::lock_guard<std::mutex> lock(s.fontMutex);
            auto found = s.fontData.find(number);
            if (found != s.fontData.end()) data = found->second;
        }
        if (!data) return E_FAIL;
        *stream = new (std::nothrow) MemoryFontStream(std::move(data));
        return *stream ? S_OK : E_OUTOFMEMORY;
    }
};

MemoryFontLoader* fontLoader() {
    static MemoryFontLoader* const loader = new MemoryFontLoader();
    return loader;
}

/// The one file of a collection made of an app's font.
class OneFontEnumerator final : public IDWriteFontFileEnumerator, public Counted {
public:
    OneFontEnumerator(IDWriteFactory* factory, uint64_t key) : key_(key) {
        factory_.attach(factory);
        factory->AddRef();
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDWriteFontFileEnumerator)) {
            *out = static_cast<IDWriteFontFileEnumerator*>(this);
            addRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return addRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return release(); }
    HRESULT STDMETHODCALLTYPE MoveNext(BOOL* hasCurrentFile) override {
        *hasCurrentFile = FALSE;
        if (moved_) return S_OK;
        moved_ = true;
        const HRESULT hr = factory_->CreateCustomFontFileReference(&key_, sizeof(key_), fontLoader(), file_.put());
        if (SUCCEEDED(hr)) *hasCurrentFile = TRUE;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentFontFile(IDWriteFontFile** file) override {
        *file = file_.get();
        if (!*file) return E_FAIL;
        (*file)->AddRef();
        return S_OK;
    }

private:
    Ref<IDWriteFactory> factory_;
    Ref<IDWriteFontFile> file_;
    uint64_t key_;
    bool moved_ = false;
};

class MemoryCollectionLoader final : public IDWriteFontCollectionLoader, public Counted {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDWriteFontCollectionLoader)) {
            *out = static_cast<IDWriteFontCollectionLoader*>(this);
            addRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return addRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return release(); }
    HRESULT STDMETHODCALLTYPE CreateEnumeratorFromKey(IDWriteFactory* factory, const void* key, UINT32 keySize, IDWriteFontFileEnumerator** enumerator) override {
        if (!enumerator) return E_POINTER;
        *enumerator = nullptr;
        if (!factory || !key || keySize != sizeof(uint64_t)) return E_INVALIDARG;
        uint64_t number;
        memcpy(&number, key, sizeof(number));
        *enumerator = new (std::nothrow) OneFontEnumerator(factory, number);
        return *enumerator ? S_OK : E_OUTOFMEMORY;
    }
};

MemoryCollectionLoader* collectionLoader() {
    static MemoryCollectionLoader* const loader = new MemoryCollectionLoader();
    return loader;
}

// MARK: Android's own fonts

/// Android 9's families and the files of each, as its /system/etc/fonts.xml
/// has them. An app's layout is made for these: text in another font is
/// wider or narrower and its lines taller, and labels stop fitting. What
/// these fonts lack (other scripts, emoji) is the system's fallback, as for
/// any face.
struct AndroidFontFamily {
    const char* name;
    const wchar_t* files[13];
};
const AndroidFontFamily androidFontFamilies[] = {
    {"sans-serif",
     {L"Roboto-Regular.ttf", L"Roboto-Italic.ttf", L"Roboto-Bold.ttf", L"Roboto-BoldItalic.ttf", L"Roboto-Medium.ttf", L"Roboto-MediumItalic.ttf", L"Roboto-Light.ttf",
      L"Roboto-LightItalic.ttf", L"Roboto-Thin.ttf", L"Roboto-ThinItalic.ttf", L"Roboto-Black.ttf", L"Roboto-BlackItalic.ttf"}},
    {"sans-serif-condensed",
     {L"RobotoCondensed-Regular.ttf", L"RobotoCondensed-Italic.ttf", L"RobotoCondensed-Bold.ttf", L"RobotoCondensed-BoldItalic.ttf", L"RobotoCondensed-Medium.ttf",
      L"RobotoCondensed-MediumItalic.ttf", L"RobotoCondensed-Light.ttf", L"RobotoCondensed-LightItalic.ttf"}},
    {"serif", {L"NotoSerif-Regular.ttf", L"NotoSerif-Italic.ttf", L"NotoSerif-Bold.ttf", L"NotoSerif-BoldItalic.ttf"}},
    {"monospace", {L"DroidSansMono.ttf"}},
    {"serif-monospace", {L"CutiveMono.ttf"}},
    {"casual", {L"ComingSoon.ttf"}},
    {"cursive", {L"DancingScript-Regular.ttf", L"DancingScript-Bold.ttf"}},
    {"sans-serif-smallcaps", {L"CarroisGothicSC-Regular.ttf"}},
};
constexpr int androidFontFamilyCount = int(sizeof(androidFontFamilies) / sizeof(androidFontFamilies[0]));
static_assert(androidFontFamilyCount == int(sizeof(State::androidFamilies) / sizeof(State::AndroidFamily)), "a place in State for each of Android's families");

/// Which of Android's families a name asks for: fonts.xml's names and
/// aliases, and for any other name the kind the layer read out of it.
int androidFamilyIndex(const std::string& name, int generic) {
    const auto has = [&](const char* part) { return name.find(part) != std::string::npos; };
    if (has("condensed")) return 1;
    if (name == "serif-monospace" || has("courier")) return 4;
    if (has("monospace") || name == "monaco" || generic == MEGLER_GFX_MONOSPACE) return 3;
    if (name == "casual") return 5;
    if (name == "cursive") return 6;
    if (has("smallcaps")) return 7;
    if (generic == MEGLER_GFX_SERIF) return 2;
    return 0;
}

/// The files of one of Android's families that are in the folder.
class FolderFontEnumerator final : public IDWriteFontFileEnumerator, public Counted {
public:
    FolderFontEnumerator(IDWriteFactory* factory, int family) : family_(family) {
        factory_.attach(factory);
        factory->AddRef();
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDWriteFontFileEnumerator)) {
            *out = static_cast<IDWriteFontFileEnumerator*>(this);
            addRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return addRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return release(); }
    HRESULT STDMETHODCALLTYPE MoveNext(BOOL* hasCurrentFile) override {
        *hasCurrentFile = FALSE;
        file_.reset();
        const AndroidFontFamily& family = androidFontFamilies[family_];
        while (next_ < int(sizeof(family.files) / sizeof(family.files[0])) && family.files[next_]) {
            const std::wstring path = G().fontFolder + L"\\" + family.files[next_++];
            // (A file that is not there is passed over: the family is what is left of it.)
            if (SUCCEEDED(factory_->CreateFontFileReference(path.c_str(), nullptr, file_.put())) && file_) {
                *hasCurrentFile = TRUE;
                break;
            }
            file_.reset();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentFontFile(IDWriteFontFile** file) override {
        *file = file_.get();
        if (!*file) return E_FAIL;
        (*file)->AddRef();
        return S_OK;
    }

private:
    Ref<IDWriteFactory> factory_;
    Ref<IDWriteFontFile> file_;
    int family_;
    int next_ = 0;
};

/// A collection's key here is the number of one of Android's families.
class FolderCollectionLoader final : public IDWriteFontCollectionLoader, public Counted {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDWriteFontCollectionLoader)) {
            *out = static_cast<IDWriteFontCollectionLoader*>(this);
            addRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return addRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return release(); }
    HRESULT STDMETHODCALLTYPE CreateEnumeratorFromKey(IDWriteFactory* factory, const void* key, UINT32 keySize, IDWriteFontFileEnumerator** enumerator) override {
        if (!enumerator) return E_POINTER;
        *enumerator = nullptr;
        if (!factory || !key || keySize != sizeof(int32_t)) return E_INVALIDARG;
        int32_t family;
        memcpy(&family, key, sizeof(family));
        if (family < 0 || family >= androidFontFamilyCount) return E_INVALIDARG;
        *enumerator = new (std::nothrow) FolderFontEnumerator(factory, family);
        return *enumerator ? S_OK : E_OUTOFMEMORY;
    }
};

FolderCollectionLoader* folderLoader() {
    static FolderCollectionLoader* const loader = new FolderCollectionLoader();
    return loader;
}

/// One of Android's families out of the folder of Android's fonts: its
/// collection, and what DirectWrite calls it there. Null where there is no
/// such folder or the family's files are not in it; the system's fonts then
/// stand in, and the log says so once for each family.
const State::AndroidFamily* androidFamily(int index) {
    State& s = G();
    if (s.fontFolder.empty() || index < 0 || index >= androidFontFamilyCount) return nullptr;
    std::lock_guard<std::mutex> lock(s.androidMutex);
    State::AndroidFamily& family = s.androidFamilies[index];
    if (family.tried) return family.collection ? &family : nullptr;
    family.tried = true;
    if (s.folderLoaderState == 0) {
        const HRESULT hr = s.dwrite->RegisterFontCollectionLoader(folderLoader());
        if (FAILED(hr)) failed("IDWriteFactory::RegisterFontCollectionLoader", hr);
        s.folderLoaderState = SUCCEEDED(hr) ? 1 : -1;
    }
    if (s.folderLoaderState < 0) return nullptr;
    const int32_t key = index;
    Ref<IDWriteFontCollection> collection;
    const HRESULT hr = s.dwrite->CreateCustomFontCollection(folderLoader(), &key, sizeof(key), collection.put());
    // DirectWrite sorts a family's files into families of its own by their names: the one with the most fonts is it.
    Ref<IDWriteFontFamily> most;
    UINT32 fonts = 0;
    for (UINT32 i = 0, count = SUCCEEDED(hr) && collection ? collection->GetFontFamilyCount() : 0; i < count; i++) {
        Ref<IDWriteFontFamily> one;
        if (SUCCEEDED(collection->GetFontFamily(i, one.put())) && one->GetFontCount() > fonts) {
            fonts = one->GetFontCount();
            most = std::move(one);
        }
    }
    Ref<IDWriteLocalizedStrings> names;
    UINT32 length = 0;
    if (most && SUCCEEDED(most->GetFamilyNames(names.put())) && names->GetCount() && SUCCEEDED(names->GetStringLength(0, &length))) {
        std::wstring name(size_t(length) + 1, L'\0');
        if (SUCCEEDED(names->GetString(0, &name[0], length + 1))) {
            name.resize(length);
            Ref<IDWriteFont> regular;
            if (SUCCEEDED(most->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, regular.put()))) {
                family.stretch = regular->GetStretch();
                family.family = std::move(name);
                family.collection = std::move(collection);
            }
        }
    }
    if (!family.collection) {
        say("Android's fonts for %s are not in the folder of Android's fonts: the system's stand in for them", androidFontFamilies[index].name);
        return nullptr;
    }
    return &family;
}

// MARK: A run of text, laid out

/// Some glyphs of one font in a row, as DirectWrite's layout placed them.
struct TextRun {
    Ref<IDWriteFontFace> face;
    float em = 0;
    std::vector<UINT16> glyphs;
    std::vector<FLOAT> advances;
    std::vector<DWRITE_GLYPH_OFFSET> offsets;
    float x = 0, y = 0;     // the run's own baseline origin, from the start of the whole run's baseline
    bool sideways = false;
    UINT32 bidi = 0;
    bool ownColour = false; // a layer of a colour glyph: drawn in `colour`, not the paint's
    D2D1_COLOR_F colour = {};
};

enum : uint8_t { unitBreakAfter = 1, unitSpace = 2, unitNewline = 4 };

/// One run of text in one face at one size and spacing, measured: what
/// drawing and all five measuring functions answer from. Nothing in it
/// changes once it is made, so several threads may read one.
struct Shaped {
    std::vector<float> widths;   // each code unit's advance; 0 for a unit with none of its own
    std::vector<uint8_t> flags;  // unitBreakAfter | unitSpace | unitNewline
    float width = 0;             // the advances' sum
    bool inked = false;
    float inkL = 0, inkT = 0, inkR = 0, inkB = 0; // the ink's box from the baseline's start, y down
    std::vector<TextRun> runs;   // with a colour glyph's layers in place of the glyph
    std::vector<TextRun> plain;  // the same without them: for outlines, and for coverage
    bool coloured = false;
};

/// DirectWrite hands a layout's glyph runs to this, which keeps them.
class RunCollector final : public IDWriteTextRenderer, public Counted {
public:
    explicit RunCollector(std::vector<TextRun>* out) : out_(out) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDWritePixelSnapping) || iid == __uuidof(IDWriteTextRenderer)) {
            *out = static_cast<IDWriteTextRenderer*>(this);
            addRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return addRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return release(); }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) override {
        *disabled = TRUE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* transform) override {
        const DWRITE_MATRIX identity = {1, 0, 0, 1, 0, 0};
        *transform = identity;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* pixelsPerDip) override {
        *pixelsPerDip = 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE, const DWRITE_GLYPH_RUN* run, const DWRITE_GLYPH_RUN_DESCRIPTION*,
                                           IUnknown*) override {
        if (!run || !run->fontFace || !run->glyphCount || !run->glyphIndices) return S_OK;
        try {
            TextRun kept;
            kept.face.attach(run->fontFace);
            run->fontFace->AddRef();
            kept.em = run->fontEmSize;
            kept.glyphs.assign(run->glyphIndices, run->glyphIndices + run->glyphCount);
            if (run->glyphAdvances) kept.advances.assign(run->glyphAdvances, run->glyphAdvances + run->glyphCount);
            if (run->glyphOffsets) kept.offsets.assign(run->glyphOffsets, run->glyphOffsets + run->glyphCount);
            kept.x = x;
            kept.y = y;
            kept.sideways = run->isSideways != FALSE;
            kept.bidi = run->bidiLevel;
            out_->push_back(std::move(kept));
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*, FLOAT, FLOAT, IDWriteInlineObject*, BOOL, BOOL, IUnknown*) override { return S_OK; }

private:
    std::vector<TextRun>* out_;
};

DWRITE_GLYPH_RUN glyphRunOf(const TextRun& run) {
    DWRITE_GLYPH_RUN out = {};
    out.fontFace = run.face.get();
    out.fontEmSize = run.em;
    out.glyphCount = UINT32(run.glyphs.size());
    out.glyphIndices = run.glyphs.data();
    out.glyphAdvances = run.advances.empty() ? nullptr : run.advances.data();
    out.glyphOffsets = run.offsets.empty() ? nullptr : run.offsets.data();
    out.isSideways = run.sideways;
    out.bidiLevel = run.bidi;
    return out;
}

/// A run whose font has colour glyphs (an emoji's), as the layers DirectWrite
/// makes of it: one run for each colour, and what is not coloured as it was.
void appendColoured(Shaped& shaped, const TextRun& run) {
    State& s = G();
    Ref<IDWriteColorGlyphRunEnumerator> layers;
    if (s.dwrite2) {
        const DWRITE_GLYPH_RUN glyphRun = glyphRunOf(run);
        const HRESULT hr = s.dwrite2->TranslateColorGlyphRun(run.x, run.y, &glyphRun, nullptr, DWRITE_MEASURING_MODE_NATURAL, nullptr, 0, layers.put());
        if (FAILED(hr)) layers.reset(); // DWRITE_E_NOCOLOR: the usual answer
    }
    if (!layers) {
        shaped.runs.push_back(run);
        return;
    }
    for (;;) {
        BOOL more = FALSE;
        const DWRITE_COLOR_GLYPH_RUN* layer = nullptr;
        if (FAILED(layers->MoveNext(&more)) || !more || FAILED(layers->GetCurrentRun(&layer)) || !layer) break;
        const DWRITE_GLYPH_RUN& g = layer->glyphRun;
        if (!g.fontFace || !g.glyphCount) continue;
        TextRun kept;
        kept.face.attach(g.fontFace);
        g.fontFace->AddRef();
        kept.em = g.fontEmSize;
        kept.glyphs.assign(g.glyphIndices, g.glyphIndices + g.glyphCount);
        if (g.glyphAdvances) kept.advances.assign(g.glyphAdvances, g.glyphAdvances + g.glyphCount);
        if (g.glyphOffsets) kept.offsets.assign(g.glyphOffsets, g.glyphOffsets + g.glyphCount);
        kept.x = layer->baselineOriginX;
        kept.y = layer->baselineOriginY;
        kept.sideways = g.isSideways != FALSE;
        kept.bidi = g.bidiLevel;
        // Palette index 0xFFFF is "the text's own colour".
        if (layer->paletteIndex != 0xFFFF) {
            kept.ownColour = true;
            kept.colour.r = layer->runColor.r;
            kept.colour.g = layer->runColor.g;
            kept.colour.b = layer->runColor.b;
            kept.colour.a = layer->runColor.a;
            shaped.coloured = true;
        }
        shaped.runs.push_back(std::move(kept));
    }
}

/// Lays a run out. Null when DirectWrite will not.
std::shared_ptr<Shaped> shape(const Typeface& face, float size, float spacing, const uint16_t* text, int count) {
    State& s = G();
    auto shaped = std::make_shared<Shaped>();
    shaped->widths.assign(size_t(count), 0.0f);
    shaped->flags.assign(size_t(count), 0);
    // One run on one line: what would break a line or move to a tab stop is taken out before DirectWrite sees it. A line
    // feed and the other control characters become a zero width space, a tab a space.
    std::vector<wchar_t> clean(static_cast<size_t>(count));
    for (int i = 0; i < count; i++) {
        const uint16_t u = text[i];
        if (u == '\n') shaped->flags[size_t(i)] |= unitNewline;
        clean[size_t(i)] = u == '\t' ? L' ' : (u < 0x20 || (u >= 0x7F && u < 0xA0) || u == 0x2028 || u == 0x2029) ? wchar_t(0x200B) : wchar_t(u);
    }
    Ref<IDWriteTextLayout> layout;
    HRESULT hr = s.dwrite->CreateTextLayout(clean.data(), UINT32(count), face.format.get(), 0.0f, 0.0f, layout.put());
    if (FAILED(hr)) {
        failed("IDWriteFactory::CreateTextLayout", hr);
        return nullptr;
    }
    const DWRITE_TEXT_RANGE all = {0, UINT32(count)};
    layout->SetFontSize(size, all);
    if (spacing != 0) {
        // Half before each cluster and half after, as Android has it: a run starts half a spacing in.
        if (Ref<IDWriteTextLayout1> layout1 = layout.as<IDWriteTextLayout1>()) layout1->SetCharacterSpacing(spacing / 2, spacing / 2, 0.0f, all);
        else note("letter-spacing", "this DirectWrite has no IDWriteTextLayout1: letter spacing is not applied (said once)");
    }
    // Each cluster's advance goes to its first code unit; a line may break after a cluster's last.
    std::vector<DWRITE_CLUSTER_METRICS> clusters(static_cast<size_t>(count) + 1);
    UINT32 clusterCount = 0;
    hr = layout->GetClusterMetrics(clusters.data(), UINT32(clusters.size()), &clusterCount);
    if (FAILED(hr)) {
        failed("IDWriteTextLayout::GetClusterMetrics", hr);
        return nullptr;
    }
    int at = 0;
    float width = 0;
    for (UINT32 c = 0; c < clusterCount && at < count; c++) {
        const int length = std::max(1, int(clusters[c].length));
        const int last = std::min(count - 1, at + length - 1);
        shaped->widths[size_t(at)] = clusters[c].width;
        width += clusters[c].width;
        if (clusters[c].canWrapLineAfter) shaped->flags[size_t(last)] |= unitBreakAfter;
        if (clusters[c].isWhitespace)
            for (int k = at; k <= last; k++) shaped->flags[size_t(k)] |= unitSpace;
        at += length;
    }
    shaped->width = width;
    // The glyphs, by the layout's own drawing; and the ink's box, which DirectWrite gives as what hangs over the layout's (empty) box.
    Ref<RunCollector> collector;
    collector.attach(new RunCollector(&shaped->plain));
    hr = layout->Draw(nullptr, collector.get(), 0.0f, 0.0f);
    if (FAILED(hr)) {
        failed("IDWriteTextLayout::Draw", hr);
        return nullptr;
    }
    const float baseline = shaped->plain.empty() ? 0.0f : shaped->plain[0].y;
    for (TextRun& run : shaped->plain) run.y -= baseline;
    for (const TextRun& run : shaped->plain) appendColoured(*shaped, run);
    DWRITE_OVERHANG_METRICS over = {};
    if (!shaped->plain.empty() && SUCCEEDED(layout->GetOverhangMetrics(&over)) && over.right > -over.left && over.bottom > -over.top) {
        shaped->inked = true;
        shaped->inkL = -over.left;
        shaped->inkT = -over.top - baseline;
        shaped->inkR = over.right;
        shaped->inkB = over.bottom - baseline;
    }
    return shaped;
}

/// The runs laid out lately, by face, size, spacing and text. The framework
/// measures the same strings again and again (a layout pass measures every
/// piece of text it places, often several times), and a drawing follows.
/// Two generations: when the newer is full it becomes the older and the
/// older is dropped, so what is in use stays and what is not goes.
struct TextCache {
    struct Key {
        const Typeface* face;
        uint32_t size, spacing;
        std::u16string text;
        bool operator==(const Key& o) const { return face == o.face && size == o.size && spacing == o.spacing && text == o.text; }
    };
    struct Hash {
        size_t operator()(const Key& k) const {
            uint64_t h = 1469598103934665603ull ^ uint64_t(reinterpret_cast<uintptr_t>(k.face));
            h = (h ^ k.size) * 1099511628211ull;
            h = (h ^ k.spacing) * 1099511628211ull;
            for (char16_t c : k.text) h = (h ^ c) * 1099511628211ull;
            return size_t(h ^ (h >> 32));
        }
    };
    typedef std::unordered_map<Key, std::shared_ptr<Shaped>, Hash> Map;
    std::mutex mutex;
    Map newer, older;
    size_t newerUnits = 0;
    static constexpr size_t maxRuns = 4096, maxUnits = 256 * 1024;
};

TextCache& textCache() {
    static TextCache* const cache = new TextCache();
    return *cache;
}

/// The run, laid out now or found. Null for no text, or when DirectWrite fails.
std::shared_ptr<Shaped> shapedRun(void* typeface, const megler_gfx_paint& paint, const uint16_t* text, int count) {
    if (!text || count <= 0 || !started()) return nullptr;
    const Typeface* face = faceOf(typeface);
    if (!face) return nullptr;
    const float size = paint.text_size > 0 && std::isfinite(paint.text_size) ? paint.text_size : 12.0f;
    const float spacing = std::isfinite(paint.letter_spacing) ? paint.letter_spacing * size : 0.0f;
    TextCache& cache = textCache();
    TextCache::Key key;
    key.face = face;
    memcpy(&key.size, &size, 4);
    memcpy(&key.spacing, &spacing, 4);
    key.text.assign(reinterpret_cast<const char16_t*>(text), size_t(count));
    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        auto found = cache.newer.find(key);
        if (found != cache.newer.end()) {
            ++G().runsReused;
            return found->second;
        }
        found = cache.older.find(key);
        if (found != cache.older.end()) {
            std::shared_ptr<Shaped> again = found->second;
            cache.newer.emplace(key, again);
            cache.newerUnits += size_t(count);
            ++G().runsReused;
            return again;
        }
    }
    // (Outside the lock: two threads may lay the same run out at once, and one of the two is kept.)
    std::shared_ptr<Shaped> made = shape(*face, size, spacing, text, count);
    if (!made) return nullptr;
    ++G().runsShaped;
    std::lock_guard<std::mutex> lock(cache.mutex);
    if (cache.newer.size() >= TextCache::maxRuns || cache.newerUnits >= TextCache::maxUnits) {
        cache.older.swap(cache.newer);
        cache.newer.clear();
        cache.newerUnits = 0;
    }
    cache.newer.emplace(std::move(key), made);
    cache.newerUnits += size_t(count);
    return made;
}

inline float scaleOf(const megler_gfx_paint& paint) { return paint.text_scale_x > 0 && std::isfinite(paint.text_scale_x) ? paint.text_scale_x : 1.0f; }

/// A run's glyphs on a surface that is drawing, each run in the brush, or
/// (`ownColours`) a colour glyph's layers in their own colours at `alpha`.
void drawGlyphs(Surface& s, const Ink& ink, const Matrix3x2F& world, ID2D1Brush* brush, bool ownColours, uint32_t alpha) {
    const Shaped& shaped = *ink.text;
    s.setMatrix(ink.textMatrix * world);
    const std::vector<TextRun>& runs = ownColours ? shaped.runs : shaped.plain;
    for (const TextRun& run : runs) {
        const DWRITE_GLYPH_RUN glyphRun = glyphRunOf(run);
        ID2D1Brush* with = brush;
        if (ownColours && run.ownColour) {
            D2D1_COLOR_F colour = run.colour;
            colour.a *= float(alpha) / 255.0f;
            s.second->SetColor(colour);
            with = s.second.get();
        }
        s.rt->DrawGlyphRun(D2D1::Point2F(run.x, run.y), &glyphRun, with, DWRITE_MEASURING_MODE_NATURAL);
    }
}

/// The outlines of a run's glyphs as one geometry in the drawing's
/// coordinates: for text that is stroked, or thickened.
Ref<ID2D1Geometry> glyphOutlines(const Shaped& shaped, const Matrix3x2F& textMatrix) {
    State& s = G();
    std::vector<Ref<ID2D1Geometry>> parts;
    for (const TextRun& run : shaped.plain) {
        Ref<ID2D1PathGeometry> path;
        Ref<ID2D1GeometrySink> sink;
        if (FAILED(s.d2d->CreatePathGeometry(path.put())) || FAILED(path->Open(sink.put()))) continue;
        HRESULT hr = run.face->GetGlyphRunOutline(run.em, run.glyphs.data(), run.advances.empty() ? nullptr : run.advances.data(),
                                                  run.offsets.empty() ? nullptr : run.offsets.data(), UINT32(run.glyphs.size()), run.sideways, (run.bidi & 1) != 0,
                                                  sink.get());
        const HRESULT closed = sink->Close();
        if (FAILED(hr) || FAILED(closed)) continue;
        Ref<ID2D1Geometry> placed = transformed(path.get(), Matrix3x2F::Translation(run.x, run.y) * textMatrix);
        if (placed) parts.push_back(std::move(placed));
    }
    Ref<ID2D1Geometry> out;
    if (parts.empty()) return out;
    std::vector<ID2D1Geometry*> raw;
    for (const auto& part : parts) raw.push_back(part.get());
    Ref<ID2D1GeometryGroup> group;
    if (SUCCEEDED(s.d2d->CreateGeometryGroup(D2D1_FILL_MODE_WINDING, raw.data(), UINT32(raw.size()), group.put()))) out = Ref<ID2D1Geometry>(group);
    return out;
}

// MARK: megler_gfx_host: text

void drawText(void*, void* canvas, const uint16_t* text, int count, float x, float y, const megler_gfx_paint* given, void* typeface, void* shader) {
    Canvas* cv = canvasOf(canvas);
    if (!cv || !given) return;
    guarded("draw_text", [&] {
        const megler_gfx_paint fixed = checked(*given);
        const megler_gfx_paint* paint = &fixed;
        if (!sane(x) || !sane(y)) return;
        std::shared_ptr<Shaped> shaped = shapedRun(typeface, *paint, text, count);
        if (!shaped || shaped->plain.empty()) return;
        const float sx = scaleOf(*paint), skew = std::isfinite(paint->text_skew_x) ? paint->text_skew_x : 0.0f;
        const float width = shaped->width * sx;
        if (paint->text_align == MEGLER_GFX_ALIGN_CENTER) x -= width / 2;
        else if (paint->text_align == MEGLER_GFX_ALIGN_RIGHT) x -= width;
        // A point (gx, gy) of the run, y down from the baseline's start, is drawn at (x + sx * gx + skew * gy, y + gy).
        const Matrix3x2F textMatrix(sx, 0, skew, 1, x, y);
        Ink ink;
        ink.kind = Ink::glyphs;
        ink.text = shaped.get();
        ink.textMatrix = textMatrix;
        ink.smooth = (paint->flags & MEGLER_GFX_ANTI_ALIAS) != 0;
        // The box of the ink in the drawing's coordinates; a run with no ink of its own (spaces) still has its advance.
        const float inkL = shaped->inked ? shaped->inkL : 0, inkR = shaped->inked ? shaped->inkR : shaped->width;
        const float inkT = shaped->inked ? shaped->inkT : 0, inkB = shaped->inked ? shaped->inkB : 0;
        ink.l = x + sx * inkL + std::min(skew * inkT, skew * inkB);
        ink.r = x + sx * inkR + std::max(skew * inkT, skew * inkB);
        ink.t = y + inkT;
        ink.b = y + inkB;
        const bool bold = (paint->flags & MEGLER_GFX_FAKE_BOLD) != 0;
        Shader* brush = shaderOf(shader);
        Ref<ID2D1Geometry> outlines;
        if (paint->style != MEGLER_GFX_FILL || bold) {
            // Stroked, or thickened (by a stroke of a twenty-fourth of the size, the original's): the glyphs' outlines, as a shape like any other.
            outlines = glyphOutlines(*shaped, textMatrix);
            if (!outlines) return;
            if (shaped->coloured) note("colour-glyph-outline", "a colour glyph stroked or made bold is drawn as its outline, in the paint's colour (said once)");
            megler_gfx_paint thick = *paint;
            const float extra = bold ? paint->text_size / 24.0f : 0.0f;
            if (thick.style == MEGLER_GFX_FILL) {
                thick.style = MEGLER_GFX_FILL_AND_STROKE;
                thick.stroke_width = extra;
                thick.join = MEGLER_GFX_JOIN_ROUND;
            } else {
                thick.stroke_width = paint->stroke_width + extra;
            }
            ink.kind = Ink::geometry;
            ink.shape = outlines.get();
            inkStroke(ink, thick);
            paintInk(*cv, ink, thick, brush);
            return;
        }
        if (shaped->coloured && (brush || !drawsDirect(*paint, brush, MEGLER_GFX_FILL)))
            note("colour-glyph-mode", "a colour glyph drawn by a transfer mode other than SRC_OVER, or through a shader, is drawn in the paint's colour or the shader's (said once)");
        paintInk(*cv, ink, *paint, brush);
    });
}

float textMeasure(void*, const uint16_t* text, int count, const megler_gfx_paint* paint, void* typeface) {
    float width = 0;
    if (!paint) return 0;
    guarded("text_measure", [&] {
        if (std::shared_ptr<Shaped> shaped = shapedRun(typeface, *paint, text, count)) width = shaped->width * scaleOf(*paint);
    });
    return width;
}

float textMetrics(void*, const megler_gfx_paint* paint, void* typeface, float* out) {
    float spacing = 0;
    if (out) out[0] = out[1] = out[2] = out[3] = out[4] = 0;
    if (!paint || !started()) return 0;
    const Typeface* face = faceOf(typeface);
    if (!face) return 0;
    const float size = paint->text_size > 0 && std::isfinite(paint->text_size) ? paint->text_size : 12.0f;
    if (out) {
        out[0] = face->top * size;
        out[1] = face->ascent * size;
        out[2] = face->descent * size;
        out[3] = face->bottom * size;
        out[4] = face->leading * size;
    }
    spacing = (-face->ascent + face->descent + face->leading) * size;
    return spacing;
}

/// The left and right ends of a slanted run's ink: each glyph's own box (the
/// font's design box of it) slanted, which is closer than slanting the box
/// of the whole run -- a run's lowest point is seldom under its first letter.
/// False when a font will not say (the caller then slants the whole box).
bool slantedEnds(const Shaped& shaped, float sx, float skew, float* left, float* right) {
    bool any = false;
    float least = 0, most = 0;
    std::vector<DWRITE_GLYPH_METRICS> boxes;
    for (const TextRun& run : shaped.plain) {
        DWRITE_FONT_METRICS font = {};
        run.face->GetMetrics(&font);
        if (run.sideways || !font.designUnitsPerEm) return false;
        boxes.resize(run.glyphs.size());
        if (FAILED(run.face->GetDesignGlyphMetrics(run.glyphs.data(), UINT32(run.glyphs.size()), boxes.data(), FALSE))) return false;
        const float unit = run.em / float(font.designUnitsPerEm);
        const bool leftward = (run.bidi & 1) != 0;
        float pen = run.x;
        for (size_t i = 0; i < run.glyphs.size(); i++) {
            const DWRITE_GLYPH_METRICS& g = boxes[i];
            const float advance = run.advances.empty() ? float(g.advanceWidth) * unit : run.advances[i];
            // (A run that reads leftwards is placed from its right end: each glyph's origin is its advance before the pen.)
            float x = leftward ? pen - advance : pen, y = run.y;
            if (!run.offsets.empty()) {
                x += leftward ? -run.offsets[i].advanceOffset : run.offsets[i].advanceOffset;
                y -= run.offsets[i].ascenderOffset;
            }
            pen += leftward ? -advance : advance;
            const float l = x + float(g.leftSideBearing) * unit, r = x + (float(g.advanceWidth) - float(g.rightSideBearing)) * unit;
            const float t = y - float(g.verticalOriginY - g.topSideBearing) * unit;
            const float b = y - float(g.verticalOriginY - INT32(g.advanceHeight) + g.bottomSideBearing) * unit;
            if (!(r > l) || !(b > t)) continue; // a glyph with no ink (a space)
            const float lo = sx * l + std::min(skew * t, skew * b), hi = sx * r + std::max(skew * t, skew * b);
            least = any ? std::min(least, lo) : lo;
            most = any ? std::max(most, hi) : hi;
            any = true;
        }
    }
    if (any) {
        *left = least;
        *right = most;
    }
    return any;
}

void textBounds(void*, const uint16_t* text, int count, const megler_gfx_paint* paint, void* typeface, float* out) {
    if (!out) return;
    out[0] = out[1] = out[2] = out[3] = 0;
    if (!paint) return;
    guarded("text_bounds", [&] {
        std::shared_ptr<Shaped> shaped = shapedRun(typeface, *paint, text, count);
        if (!shaped || !shaped->inked) return;
        const float sx = scaleOf(*paint), skew = std::isfinite(paint->text_skew_x) ? paint->text_skew_x : 0.0f;
        float grow = 0;
        if ((paint->flags & MEGLER_GFX_FAKE_BOLD) != 0) grow += paint->text_size / 48.0f;
        if (paint->style != MEGLER_GFX_FILL) grow += paint->stroke_width / 2;
        float left = sx * shaped->inkL + std::min(skew * shaped->inkT, skew * shaped->inkB);
        float right = sx * shaped->inkR + std::max(skew * shaped->inkT, skew * shaped->inkB);
        if (skew != 0) slantedEnds(*shaped, sx, skew, &left, &right);
        out[0] = left - grow;
        out[1] = shaped->inkT - grow;
        out[2] = right + grow;
        out[3] = shaped->inkB + grow;
    });
}

/// How many code units of the first `length` of a run fit in `maxWidth`, by
/// characters or by words (megler.h, text_break). `whole`: these units are all
/// there is of the line, so its end is a place a line may end. `more` is set
/// when the answer may be larger with more of the text laid out: everything
/// laid out fits.
int breakWithin(const Shaped& shaped, int length, float sx, float maxWidth, bool words, bool whole, bool* more) {
    *more = false;
    // By characters: as many whole ones (a unit with no advance belongs to the one before it) as fit.
    int fit = 0;
    float used = 0;
    for (int i = 0; i < length;) {
        int next = i + 1;
        while (next < length && shaped.widths[size_t(next)] == 0 && !(shaped.flags[size_t(next - 1)] & unitBreakAfter)) next++;
        float advance = 0;
        for (int k = i; k < next; k++) advance += shaped.widths[size_t(k)] * sx;
        if (used + advance > maxWidth) break;
        used += advance;
        fit = next;
        i = next;
    }
    if (fit == length) {
        *more = !whole;
        return fit;
    }
    if (!words) return fit;
    // By words: the longest start that ends where a line may break and fits, the spaces at its end not counted against the width.
    int best = -1;
    float sum = 0, kept = 0; // the width up to here, and up to the last unit that is not a space
    bool over = false;
    for (int i = 0; i < length; i++) {
        sum += shaped.widths[size_t(i)] * sx;
        if (!(shaped.flags[size_t(i)] & (unitSpace | unitNewline))) kept = sum;
        if (kept > maxWidth) {
            over = true;
            break;
        }
        // (The end of what is laid out is only a place to break when it is the line's end.)
        const bool mayEnd = i + 1 == length ? whole : (shaped.flags[size_t(i)] & unitBreakAfter) != 0;
        if (mayEnd) best = i + 1;
    }
    // Spaces at the end ran on to the end of what is laid out: there may be more of them.
    if (!over && !whole) *more = true;
    return best > 0 ? best : fit;
}

int textBreak(void*, const uint16_t* text, int count, float maxWidth, const megler_gfx_paint* paint, void* typeface, bool words) {
    int answer = 0;
    if (!paint || !text || count <= 0) return 0;
    guarded("text_break", [&] {
        // A line feed ends the line and is counted in it.
        int line = count;
        for (int i = 0; i < count; i++)
            if (text[i] == '\n') {
                line = i + 1;
                break;
            }
        // The framework asks with all that is left of a paragraph, for every line of it. Only as much is laid out as can fit
        // (few letters are narrower than a fifth of the size), and more when that guess was short.
        const float size = paint->text_size > 0 ? paint->text_size : 12.0f, sx = scaleOf(*paint);
        const float narrowest = std::max(0.5f, size * sx * 0.2f);
        int length = maxWidth > 0 && maxWidth / narrowest < 100000.0f ? int(maxWidth / narrowest) + 24 : line;
        for (;;) {
            if (length > line) length = line;
            // (Not between the two halves of a pair.)
            if (length < line && text[length - 1] >= 0xD800 && text[length - 1] < 0xDC00) length++;
            std::shared_ptr<Shaped> shaped = shapedRun(typeface, *paint, text, length);
            if (!shaped) return;
            bool more = false;
            answer = breakWithin(*shaped, length, sx, maxWidth, words, length == line, &more);
            if (!more || length == line) return;
            length = length > line / 2 ? line : length * 2;
        }
    });
    return answer;
}

void textWidths(void*, const uint16_t* text, int count, const megler_gfx_paint* paint, void* typeface, float* out) {
    if (!out || count <= 0) return;
    for (int i = 0; i < count; i++) out[i] = 0;
    if (!paint) return;
    guarded("text_widths", [&] {
        std::shared_ptr<Shaped> shaped = shapedRun(typeface, *paint, text, count);
        if (!shaped) return;
        const float sx = scaleOf(*paint);
        for (int i = 0; i < count; i++) out[i] = shaped->widths[size_t(i)] * sx;
    });
}

// MARK: megler_gfx_host: typefaces

/// A face by Android's family name: Android's own font where the folder of
/// Android's fonts has it, else the system's nearest.
Typeface* faceByName(const char* family, int generic, int weight, bool italic) {
    const std::string name = family ? family : "";
    if (const State::AndroidFamily* own = androidFamily(androidFamilyIndex(name, generic))) {
        if (Typeface* made = makeFace(own->collection.get(), own->family, weight, italic, own->stretch)) return made;
    }
    const DWRITE_FONT_STRETCH stretch = name.find("condensed") != std::string::npos ? DWRITE_FONT_STRETCH_CONDENSED : DWRITE_FONT_STRETCH_NORMAL;
    return makeFace(nullptr, systemFamily(family, generic), weight, italic, stretch);
}

void* typefaceCreate(void*, const char* family, int generic, int weight, bool italic) {
    Typeface* made = nullptr;
    guarded("typeface_create", [&] {
        if (!started()) return;
        made = faceByName(family, generic, weight, italic);
    });
    return made;
}

void* typefaceDerive(void*, void* typeface, int weight, bool italic) {
    Typeface* made = nullptr;
    guarded("typeface_derive", [&] {
        if (!started()) return;
        const Typeface* from = faceOf(typeface);
        if (!from) return;
        made = makeFace(from->collection.get(), from->family, weight, italic, from->stretch);
    });
    return made;
}

void* typefaceFromData(void*, const void* data, size_t size) {
    Typeface* made = nullptr;
    guarded("typeface_from_data", [&] {
        if (!started() || !data || size < 12 || size > 0x7fffffffu) return;
        State& s = G();
        uint64_t key;
        {
            std::lock_guard<std::mutex> lock(s.fontMutex);
            if (!s.fontLoadersRegistered) {
                HRESULT hr = s.dwrite->RegisterFontFileLoader(fontLoader());
                if (SUCCEEDED(hr)) hr = s.dwrite->RegisterFontCollectionLoader(collectionLoader());
                if (FAILED(hr)) {
                    failed("IDWriteFactory::RegisterFontFileLoader", hr);
                    return;
                }
                s.fontLoadersRegistered = true;
            }
            key = s.nextFontKey++;
            // (The bytes are the app's: this is the copy.)
            s.fontData.emplace(key, std::make_shared<std::vector<uint8_t>>(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size));
        }
        auto forget = [&] {
            std::lock_guard<std::mutex> lock(s.fontMutex);
            s.fontData.erase(key);
        };
        // Whether it is a font at all, by DirectWrite's own reading of the file.
        Ref<IDWriteFontFile> file;
        BOOL supported = FALSE;
        DWRITE_FONT_FILE_TYPE fileType = DWRITE_FONT_FILE_TYPE_UNKNOWN;
        DWRITE_FONT_FACE_TYPE faceType = DWRITE_FONT_FACE_TYPE_UNKNOWN;
        UINT32 faces = 0;
        if (FAILED(s.dwrite->CreateCustomFontFileReference(&key, sizeof(key), fontLoader(), file.put())) ||
            FAILED(file->Analyze(&supported, &fileType, &faceType, &faces)) || !supported || !faces) {
            forget();
            return;
        }
        Ref<IDWriteFontCollection> collection;
        Ref<IDWriteFontFamily> fontFamily;
        Ref<IDWriteLocalizedStrings> names;
        Ref<IDWriteFont> font;
        UINT32 length = 0;
        if (FAILED(s.dwrite->CreateCustomFontCollection(collectionLoader(), &key, sizeof(key), collection.put())) || !collection->GetFontFamilyCount() ||
            FAILED(collection->GetFontFamily(0, fontFamily.put())) || FAILED(fontFamily->GetFamilyNames(names.put())) || !names->GetCount() ||
            FAILED(names->GetStringLength(0, &length))) {
            forget();
            return;
        }
        std::wstring name(size_t(length) + 1, L'\0');
        if (FAILED(names->GetString(0, &name[0], length + 1))) {
            forget();
            return;
        }
        name.resize(length);
        // The file's own font as it is: its weight and slant are what a face "at 400, upright" of it means.
        int weight = 400;
        bool italic = false;
        DWRITE_FONT_STRETCH stretch = DWRITE_FONT_STRETCH_NORMAL;
        if (SUCCEEDED(fontFamily->GetFont(0, font.put()))) {
            weight = int(font->GetWeight());
            italic = font->GetStyle() != DWRITE_FONT_STYLE_NORMAL;
            stretch = font->GetStretch();
        }
        made = makeFace(collection.get(), name, weight, italic, stretch);
        if (!made) forget();
    });
    return made;
}

// MARK: megler_gfx_host: image files

/// Where WIC's encoders write: memory that grows, handed to the core in one
/// piece when the file is complete (an encoder may go back to fill in what
/// it left open).
class MemoryStream final : public IStream, public Counted {
public:
    std::vector<uint8_t> bytes;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(ISequentialStream) || iid == __uuidof(IStream)) {
            *out = static_cast<IStream*>(this);
            addRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return addRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return release(); }
    HRESULT STDMETHODCALLTYPE Read(void* out, ULONG size, ULONG* read) override {
        const size_t left = at_ < bytes.size() ? bytes.size() - at_ : 0, n = std::min<size_t>(left, size);
        if (n) memcpy(out, bytes.data() + at_, n);
        at_ += n;
        if (read) *read = ULONG(n);
        return n == size ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Write(const void* data, ULONG size, ULONG* written) override {
        if (written) *written = 0;
        if (size > limit_ || at_ > limit_ - size) return STG_E_MEDIUMFULL;
        try {
            if (at_ + size > bytes.size()) bytes.resize(at_ + size);
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        }
        if (size) memcpy(bytes.data() + at_, data, size);
        at_ += size;
        if (written) *written = size;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* position) override {
        const int64_t base = origin == STREAM_SEEK_SET ? 0 : origin == STREAM_SEEK_CUR ? int64_t(at_) : int64_t(bytes.size());
        const int64_t to = base + move.QuadPart;
        if (to < 0 || uint64_t(to) > limit_) return STG_E_INVALIDFUNCTION;
        at_ = size_t(to);
        if (position) position->QuadPart = uint64_t(at_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER size) override {
        if (size.QuadPart > limit_) return STG_E_MEDIUMFULL;
        try {
            bytes.resize(size_t(size.QuadPart));
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD) override {
        if (!stat) return E_POINTER;
        memset(stat, 0, sizeof(*stat));
        stat->type = STGTY_STREAM;
        stat->cbSize.QuadPart = bytes.size();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream**) override { return E_NOTIMPL; }

private:
    size_t at_ = 0;
    static constexpr size_t limit_ = 0x7fffffff;
};

/// The first frame of an image file's bytes, and what kind of file it is.
bool openImage(const void* data, size_t size, Ref<IWICBitmapFrameDecode>& frame, bool* jpeg) {
    IWICImagingFactory* factory = data && size >= 8 && size <= 0x7fffffffu ? wic() : nullptr;
    if (!factory) return false;
    Ref<IWICStream> stream;
    Ref<IWICBitmapDecoder> decoder;
    // (The stream reads the caller's bytes where they are; it does not outlive the call.)
    if (FAILED(factory->CreateStream(stream.put())) || FAILED(stream->InitializeFromMemory(static_cast<BYTE*>(const_cast<void*>(data)), DWORD(size))) ||
        FAILED(factory->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put())) ||
        FAILED(decoder->GetFrame(0, frame.put())))
        return false;
    if (jpeg) {
        GUID container = {};
        *jpeg = SUCCEEDED(decoder->GetContainerFormat(&container)) && container == wicJpeg;
    }
    return true;
}

bool imageSize(void*, const void* data, size_t size, int* width, int* height) {
    bool ok = false;
    guarded("image_size", [&] {
        Ref<IWICBitmapFrameDecode> frame;
        UINT w = 0, h = 0;
        // The decoder is made and the frame's header read; no pixel is decoded.
        if (!started() || !openImage(data, size, frame, nullptr) || FAILED(frame->GetSize(&w, &h)) || !w || !h || w > 0x7fffffffu || h > 0x7fffffffu) return;
        if (width) *width = int(w);
        if (height) *height = int(h);
        ok = true;
    });
    return ok;
}

/// Pixels as files keep them (alpha beside the colour) made the core's
/// (colour multiplied by alpha), with the core's own rounding.
void premultiplyRows(uint8_t* pixels, int width, int height, int stride) {
    for (int y = 0; y < height; y++) {
        uint32_t* row = reinterpret_cast<uint32_t*>(pixels + size_t(y) * size_t(stride));
        for (int x = 0; x < width; x++)
            if ((row[x] >> 24) != 255) row[x] = premultiplied(row[x]);
    }
}

bool imageDecode(void*, const void* data, size_t size, void* pixels, int width, int height, int stride) {
    bool ok = false;
    guarded("image_decode", [&] {
        if (!started() || !pixels || width <= 0 || height <= 0 || stride < width * 4) return;
        IWICImagingFactory* factory = wic();
        Ref<IWICBitmapFrameDecode> frame;
        bool jpeg = false;
        UINT w = 0, h = 0;
        if (!factory || !openImage(data, size, frame, &jpeg) || FAILED(frame->GetSize(&w, &h)) || !w || !h) return;
        const bool scaled = w != UINT(width) || h != UINT(height);
        const UINT bytes = UINT(stride) * UINT(height - 1) + UINT(width) * 4;
        // The larger of two sizes is a filter that averages what a pixel covers; for the (unasked-for) other way, between neighbours.
        const WICBitmapInterpolationMode filter = UINT(width) <= w && UINT(height) <= h ? WICBitmapInterpolationModeFant : WICBitmapInterpolationModeLinear;
        HRESULT hr;
        if (!scaled || jpeg) {
            // As it is, or a JPEG, which has no alpha and which its decoder can hand over smaller by itself: scaled, then converted.
            Ref<IWICBitmapSource> from(frame);
            if (scaled) {
                Ref<IWICBitmapScaler> scaler;
                hr = factory->CreateBitmapScaler(scaler.put());
                if (SUCCEEDED(hr)) hr = scaler->Initialize(frame.get(), UINT(width), UINT(height), filter);
                if (FAILED(hr)) return;
                from = Ref<IWICBitmapSource>(scaler);
            }
            Ref<IWICFormatConverter> converter;
            hr = factory->CreateFormatConverter(converter.put());
            if (SUCCEEDED(hr)) hr = converter->Initialize(from.get(), wic32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
            if (SUCCEEDED(hr)) hr = converter->CopyPixels(nullptr, UINT(stride), bytes, static_cast<BYTE*>(pixels));
            if (FAILED(hr)) return;
            premultiplyRows(static_cast<uint8_t*>(pixels), width, height, stride);
            ok = true;
            return;
        }
        // Another size and perhaps alpha: decoded whole, premultiplied, and scaled as premultiplied pixels, so that the colour
        // of pixels that do not show does not bleed into those that do.
        if (uint64_t(w) * uint64_t(h) > (uint64_t(1) << 28)) return;
        std::vector<uint32_t> whole(size_t(w) * size_t(h));
        Ref<IWICFormatConverter> converter;
        hr = factory->CreateFormatConverter(converter.put());
        if (SUCCEEDED(hr)) hr = converter->Initialize(frame.get(), wic32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
        if (SUCCEEDED(hr)) hr = converter->CopyPixels(nullptr, w * 4, UINT(whole.size() * 4), reinterpret_cast<BYTE*>(whole.data()));
        if (FAILED(hr)) return;
        premultiplyRows(reinterpret_cast<uint8_t*>(whole.data()), int(w), int(h), int(w) * 4);
        Ref<IWICBitmap> source;
        source.attach(new MemoryBitmap(reinterpret_cast<uint8_t*>(whole.data()), w, h, w * 4));
        Ref<IWICBitmapScaler> scaler;
        hr = factory->CreateBitmapScaler(scaler.put());
        if (SUCCEEDED(hr)) hr = scaler->Initialize(source.get(), UINT(width), UINT(height), filter);
        if (SUCCEEDED(hr)) hr = scaler->CopyPixels(nullptr, UINT(stride), bytes, static_cast<BYTE*>(pixels));
        ok = SUCCEEDED(hr);
    });
    return ok;
}

bool imageEncode(void*, const void* pixels, int width, int height, int stride, int format, int quality, megler_gfx_write write, void* sink) {
    bool ok = false;
    guarded("image_encode", [&] {
        if (!started() || !pixels || width <= 0 || height <= 0 || stride < width * 4 || !write) return;
        IWICImagingFactory* factory = wic();
        if (!factory) return;
        const bool jpeg = format == MEGLER_GFX_JPEG;
        // What the file is written from: for a PNG the colours as they were before they were multiplied by alpha; for a JPEG
        // blue, green and red as they are in memory, which is the picture over black.
        const size_t rowBytes = size_t(width) * (jpeg ? 3 : 4);
        std::vector<uint8_t> rows(rowBytes * size_t(height));
        for (int y = 0; y < height; y++) {
            const uint32_t* in = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(pixels) + size_t(y) * size_t(stride));
            uint8_t* out = rows.data() + size_t(y) * rowBytes;
            if (jpeg) {
                for (int x = 0; x < width; x++) {
                    out[3 * x] = uint8_t(in[x]);
                    out[3 * x + 1] = uint8_t(in[x] >> 8);
                    out[3 * x + 2] = uint8_t(in[x] >> 16);
                }
            } else {
                uint32_t* straight = reinterpret_cast<uint32_t*>(out);
                for (int x = 0; x < width; x++) straight[x] = unpremultiplied(in[x]);
            }
        }
        Ref<MemoryStream> stream;
        stream.attach(new MemoryStream());
        Ref<IWICBitmapEncoder> encoder;
        Ref<IWICBitmapFrameEncode> frame;
        Ref<IPropertyBag2> options;
        HRESULT hr = factory->CreateEncoder(jpeg ? wicJpeg : wicPng, nullptr, encoder.put());
        if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.get(), WICBitmapEncoderNoCache);
        if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(frame.put(), options.put());
        if (SUCCEEDED(hr) && jpeg && options) {
            PROPBAG2 option = {};
            wchar_t name[] = L"ImageQuality";
            option.pstrName = name;
            VARIANT value = {};
            value.vt = VT_R4;
            value.fltVal = float(quality < 0 ? 0 : quality > 100 ? 100 : quality) / 100.0f;
            options->Write(1, &option, &value);
        }
        if (SUCCEEDED(hr)) hr = frame->Initialize(options.get());
        if (SUCCEEDED(hr)) hr = frame->SetSize(UINT(width), UINT(height));
        WICPixelFormatGUID pixelFormat = jpeg ? wic24bppBGR : wic32bppBGRA;
        if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&pixelFormat);
        // (An encoder may answer with another format than the one asked for; these two take theirs.)
        if (SUCCEEDED(hr) && !(pixelFormat == (jpeg ? wic24bppBGR : wic32bppBGRA))) hr = E_FAIL;
        if (SUCCEEDED(hr)) hr = frame->WritePixels(UINT(height), UINT(rowBytes), UINT(rows.size()), rows.data());
        if (SUCCEEDED(hr)) hr = frame->Commit();
        if (SUCCEEDED(hr)) hr = encoder->Commit();
        if (FAILED(hr)) {
            failed(jpeg ? "writing a JPEG" : "writing a PNG", hr);
            return;
        }
        ok = !stream->bytes.empty() && write(sink, stream->bytes.data(), stream->bytes.size());
    });
    return ok;
}

void bitmapGone(void*, uint32_t id) {
    // In memory no copy of a bitmap is kept from one call to the next; on the graphics processor the kept one goes.
    State& s = G();
    std::lock_guard<std::mutex> lock(s.keptMutex);
    auto found = s.kept.find(id);
    if (found == s.kept.end()) return;
    s.keptBytes -= found->second.bytes;
    s.kept.erase(found);
}

// MARK: Starting, in full

/// Whether Direct2D draws into memory through a surface as this file makes
/// them now: a red square, and its pixels read back.
bool drawsIntoMemory() {
    uint32_t pixels[16 * 16] = {};
    Surface s;
    s.px = reinterpret_cast<uint8_t*>(pixels);
    s.w = s.h = 16;
    s.stride = 64;
    if (!s.begin()) return false;
    s.rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    s.brush->SetColor(D2D1::ColorF(1, 0, 0, 1));
    s.rt->SetTransform(Matrix3x2F::Identity());
    s.rt->FillRectangle(D2D1::RectF(4, 4, 12, 12), s.brush.get());
    s.end();
    return s.rt && pixels[4 * 16 + 4] == 0xFFFF0000u && pixels[11 * 16 + 11] == 0xFFFF0000u && pixels[3 * 16 + 3] == 0 && pixels[12 * 16 + 12] == 0;
}

/// Whether a canvas on the graphics processor draws and reads back as this
/// file makes them now: a red square on nothing, and its pixels read back.
bool drawsOnGpu() {
    uint32_t pixels[16 * 16] = {};
    GpuContext* gc = takeContext();
    if (!gc) return false;
    bool ok = false;
    {
        Surface s;
        s.px = reinterpret_cast<uint8_t*>(pixels);
        s.w = s.h = 16;
        s.stride = 64;
        s.gc = gc;
        s.gpu = takeBitmap(16, 16, false);
        s.rt = Ref<ID2D1RenderTarget>(gc->dc);
        s.brush = gc->brush;
        if (s.gpu && s.begin()) {
            s.rt->Clear(D2D1::ColorF(0, 0, 0, 0));
            s.rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
            s.rt->SetTransform(Matrix3x2F::Identity());
            s.brush->SetColor(D2D1::ColorF(1, 0, 0, 1));
            s.rt->FillRectangle(D2D1::RectF(4, 4, 12, 12), s.brush.get());
            s.drew(s.bounds());
            ok = s.pull(s.bounds()) && !gpuGone() && pixels[4 * 16 + 4] == 0xFFFF0000u && pixels[11 * 16 + 11] == 0xFFFF0000u && pixels[3 * 16 + 3] == 0 &&
                 pixels[12 * 16 + 12] == 0;
        }
        s.giveBackGpu();
    }
    giveContext(gc);
    return ok;
}

const char* levelName(D3D_FEATURE_LEVEL level) {
    switch (level) {
    case D3D_FEATURE_LEVEL_9_1: return "9_1";
    case D3D_FEATURE_LEVEL_9_2: return "9_2";
    case D3D_FEATURE_LEVEL_9_3: return "9_3";
    case D3D_FEATURE_LEVEL_10_0: return "10_0";
    case D3D_FEATURE_LEVEL_10_1: return "10_1";
    case D3D_FEATURE_LEVEL_11_0: return "11_0";
    case D3D_FEATURE_LEVEL_11_1: return "11_1";
    default: return "12 or later";
    }
}

/// Everything of the graphics processor's let go: canvases are drawn in memory.
void dropGpu() {
    State& s = G();
    s.gpu = false;
    {
        std::lock_guard<std::mutex> lock(s.gpuMutex);
        for (GpuContext* c : s.freeContexts) delete c;
        s.freeContexts.clear();
        s.freeTargets.clear();
        s.freeReadables.clear();
        s.freeBytes = 0;
    }
    s.gpuResource.reset();
    s.d2dDevice.reset();
    s.d3d.reset();
}

/// The graphics processor, unless drawing in memory was asked for: a
/// hardware Direct3D 11 device that Direct2D can draw with (BGRA support; or
/// WARP, for a test), Direct2D's device on it, and a canvas there tried.
/// False, said in the log, when canvases are to be drawn in memory.
bool startGpu() {
    State& s = G();
    if (s.drawingAsked == GfxHost::Drawing::memory || s.copying) return false;
    if (!s.d2d1) {
        say("Direct2D has no ID2D1Factory1 here: canvases are drawn in memory");
        return false;
    }
    const bool warp = s.drawingAsked == GfxHost::Drawing::warp;
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
                                               D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_2,  D3D_FEATURE_LEVEL_9_1};
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_9_1;
    const D3D_DRIVER_TYPE type = warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
    HRESULT hr = D3D11CreateDevice(nullptr, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, UINT(sizeof(levels) / sizeof(levels[0])),
                                   D3D11_SDK_VERSION, s.d3d.put(), &level, nullptr);
    // (A Direct3D that does not know 11_1 refuses a list with it in.)
    if (hr == E_INVALIDARG)
        hr = D3D11CreateDevice(nullptr, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1, UINT(sizeof(levels) / sizeof(levels[0]) - 1),
                               D3D11_SDK_VERSION, s.d3d.put(), &level, nullptr);
    if (FAILED(hr)) {
        s.d3d.reset();
        say("no Direct3D device of the graphics processor's that Direct2D can draw with (0x%08lX): canvases are drawn in memory", static_cast<unsigned long>(hr));
        return false;
    }
    Ref<IDXGIDevice> dxgi = s.d3d.as<IDXGIDevice>();
    Ref<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc = {};
    char name[96] = "a graphics processor";
    if (dxgi && SUCCEEDED(dxgi->GetAdapter(adapter.put())) && SUCCEEDED(adapter->GetDesc(&desc)))
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
    char described[160];
    snprintf(described, sizeof(described), "%s, Direct3D feature level %s%s", name, levelName(level), warp ? " (WARP, Direct3D's software device)" : "");
    s.gpuName = described;
    hr = dxgi ? s.d2d1->CreateDevice(dxgi.get(), s.d2dDevice.put()) : E_NOINTERFACE;
    if (SUCCEEDED(hr)) hr = s.d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, s.gpuResource.put());
    if (FAILED(hr)) {
        say("Direct2D cannot draw on %s (0x%08lX): canvases are drawn in memory", described, static_cast<unsigned long>(hr));
        dropGpu();
        return false;
    }
    s.gpuMaxSize = s.gpuResource->GetMaximumBitmapSize();
    s.gpu = true;
    if (!drawsOnGpu()) {
        say("a canvas on %s does not draw or read back as it should: canvases are drawn in memory", described);
        dropGpu();
        s.gpuLost = false;
        return false;
    }
    return true;
}

bool start() {
    State& s = G();
    std::lock_guard<std::mutex> lock(s.startMutex);
    if (s.started.load()) return s.started.load() > 0;
    bool ok = false;
    try {
        HRESULT hr = createDirect2D(s.d2d.put());
        if (FAILED(hr)) {
            say("Direct2D is not to be had (d2d1.dll, 0x%08lX): a Java app's views cannot be drawn", static_cast<unsigned long>(hr));
        } else if (FAILED(hr = createDirectWrite(s.dwrite.put()))) {
            say("DirectWrite is not to be had (dwrite.dll, 0x%08lX): a Java app's views cannot be drawn", static_cast<unsigned long>(hr));
        } else {
            s.d2d1 = s.d2d.as<ID2D1Factory1>();
            s.dwrite2 = s.dwrite.as<IDWriteFactory2>();
            if (FAILED(s.dwrite->GetSystemFontCollection(s.systemFonts.put(), FALSE))) s.systemFonts.reset();
            wchar_t locale[LOCALE_NAME_MAX_LENGTH] = L"";
            s.locale = GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) > 0 && locale[0] ? locale : L"en-us";
            // The memory itself first; a bitmap of WIC's own, copied in and out, when Direct2D refuses that (or a test asks).
            s.copying = s.copyingAsked;
            bool draws = drawsIntoMemory();
            if (!draws && !s.copying) {
                s.copying = true;
                draws = drawsIntoMemory();
                if (draws) say("Direct2D does not draw into memory that is not WIC's own here: every canvas is copied to a WIC bitmap and back, which is slow");
            }
            if (!draws) {
                say("Direct2D cannot draw into memory here (see the failures above): a Java app's views cannot be drawn");
            } else {
                s.resource = new Surface();
                s.resource->px = reinterpret_cast<uint8_t*>(&s.resourcePixel);
                s.resource->w = s.resource->h = 1;
                s.resource->stride = 4;
                s.defaultFace = faceByName("sans-serif", MEGLER_GFX_SANS, 400, false);
                if (!s.resource->make() || !s.defaultFace) {
                    say("Direct2D and DirectWrite are there, but no brush or no text format can be made: a Java app's views cannot be drawn");
                } else {
                    char name[96] = "";
                    WideCharToMultiByte(CP_UTF8, 0, s.defaultFace->family.c_str(), -1, name, sizeof(name), nullptr, nullptr);
                    const bool gpu = startGpu();
                    const std::string where = gpu ? "on the graphics processor (" + s.gpuName + "), read back into the app's bitmaps at each flush"
                                              : s.copying ? "into WIC bitmaps that are copied from and to the app's memory"
                                                          : "straight into the app's bitmaps";
                    say("Direct2D %s, DirectWrite %s; canvases draw %s; the default face is %s%s", s.d2d1 ? "1.1 or later" : "1.0 (no ID2D1Factory1: hairlines are approximate)",
                        s.dwrite2 ? "2 or later" : "before 2 (no IDWriteFactory2: emoji are not coloured)", where.c_str(), name,
                        s.defaultFace->collection ? ", Android's own" : s.fontFolder.empty() ? "" : ", the system's");
                    ok = true;
                }
            }
        }
    } catch (...) {
        say("starting Direct2D and DirectWrite ended in an exception: a Java app's views cannot be drawn");
    }
    s.started.store(ok ? 1 : -1, std::memory_order_release);
    return ok;
}

} // namespace

// MARK: GfxHost

GfxHost& GfxHost::shared() {
    // Never destroyed: an app's threads may still be in a call when the process ends.
    static GfxHost* const host = new GfxHost();
    return *host;
}

void GfxHost::setLog(LineFn fn, void* user) {
    State& s = G();
    std::lock_guard<std::mutex> lock(s.logMutex);
    s.line = fn;
    s.lineUser = user;
}

void GfxHost::setCopying(bool copying) {
    State& s = G();
    std::lock_guard<std::mutex> lock(s.startMutex);
    s.copyingAsked = copying;
}

void GfxHost::setDrawing(Drawing drawing) {
    State& s = G();
    std::lock_guard<std::mutex> lock(s.startMutex);
    if (s.started.load()) return;
    s.drawingAsked = drawing;
}

void GfxHost::loseDevice() {
    State& s = G();
    if (!s.gpu || s.gpuLost.exchange(true)) return;
    say("the graphics processor's device is taken as lost (a test asks): from now on everything is drawn in memory");
}

void GfxHost::setWindow(const Window& window) {
    State& s = G();
    std::lock_guard<std::mutex> lock(s.windowMutex);
    s.window = window;
    s.windowBitmap.reset();
    s.windowTexture = nullptr;
}

ID3D11Device* GfxHost::device() {
    State& s = G();
    if (s.started.load(std::memory_order_acquire) <= 0 || !s.gpu || gpuGone()) return nullptr;
    return s.d3d.get();
}

void GfxHost::lockDevice() {
    State& s = G();
    if (s.started.load(std::memory_order_acquire) <= 0 || !s.gpu) return;
    if (Ref<ID2D1Multithread> lock = s.d2d.as<ID2D1Multithread>()) lock->Enter();
}

void GfxHost::unlockDevice() {
    State& s = G();
    if (s.started.load(std::memory_order_acquire) <= 0 || !s.gpu) return;
    if (Ref<ID2D1Multithread> lock = s.d2d.as<ID2D1Multithread>()) lock->Leave();
}

void GfxHost::setFontFolder(const wchar_t* folder) {
    State& s = G();
    std::lock_guard<std::mutex> lock(s.startMutex);
    if (s.started.load()) return;
    s.fontFolder = folder ? folder : L"";
    while (!s.fontFolder.empty() && (s.fontFolder.back() == L'\\' || s.fontFolder.back() == L'/')) s.fontFolder.pop_back();
}

GfxHost::Counts GfxHost::counts() {
    State& s = G();
    Counts c;
    c.canvases = s.canvases;
    c.sessions = s.sessions;
    c.direct = s.direct;
    c.composed = s.composed;
    c.layers = s.layers;
    c.runsShaped = s.runsShaped;
    c.runsReused = s.runsReused;
    c.notes = s.notes;
    c.partTargets = s.partTargets;
    c.started = s.started.load() > 0;
    c.copying = s.copying;
    c.gpu = s.gpu;
    c.gpuLost = s.gpuLost.load();
    c.uploads = s.uploads;
    c.readBacks = s.readBacks;
    c.windowReadBacks = s.windowReadBacks;
    c.shown = s.shown;
    c.byEffects = s.byEffects;
    c.throughMemory = s.throughMemory;
    c.keptMade = s.keptMade;
    c.keptReused = s.keptReused;
    if (c.started && s.gpu) snprintf(c.device, sizeof(c.device), "%s", s.gpuName.c_str());
    return c;
}

megler_gfx_host GfxHost::callbacks() {
    megler_gfx_host host = {};
    host.user = this;
    host.canvas_new = &canvasNew;
    host.canvas_free = &canvasFree;
    host.flush = &flush;
    host.touched = &touched;
    host.save = &save;
    host.save_layer = &saveLayer;
    host.restore = &restore;
    host.set_matrix = &setMatrix;
    host.clip_rect = &clipRect;
    host.clip_path = &clipPath;
    host.draw_color = &drawColor;
    host.draw_rect = &drawRect;
    host.draw_round_rect = &drawRoundRect;
    host.draw_oval = &drawOval;
    host.draw_path = &drawPath;
    host.draw_bitmap = &drawBitmap;
    host.draw_text = &drawText;
    host.text_measure = &textMeasure;
    host.text_metrics = &textMetrics;
    host.text_bounds = &textBounds;
    host.text_break = &textBreak;
    host.text_widths = &textWidths;
    host.typeface_create = &typefaceCreate;
    host.typeface_derive = &typefaceDerive;
    host.typeface_from_data = &typefaceFromData;
    host.shader_linear = &shaderLinear;
    host.shader_radial = &shaderRadial;
    host.shader_sweep = &shaderSweep;
    host.shader_bitmap = &shaderBitmap;
    host.shader_matrix = &shaderMatrix;
    host.shader_free = &shaderFree;
    host.image_size = &imageSize;
    host.image_decode = &imageDecode;
    host.image_encode = &imageEncode;
    host.bitmap_gone = &bitmapGone;
    host.present = &present;
    return host;
}

} // namespace megler::app
