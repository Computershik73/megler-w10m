/*
 * Megler for Windows 10 Mobile: what the app shell sees of the core.
 *
 * The core is plain C; the shell is C++/WinRT. Nothing here names a WinRT or
 * Direct3D type: pointers to such things cross as void *.
 *
 * Strings are UTF-8. Paths use forward or back slashes. Unless it says
 * otherwise a function may be called from any thread.
 */
#ifndef MEGLER_H
#define MEGLER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ setup */

enum { MEGLER_LOG_DEBUG = 0, MEGLER_LOG_INFO, MEGLER_LOG_WARN, MEGLER_LOG_ERROR };

/* A line of the log. May be called from any thread, one call at a time. */
typedef void (*megler_log_fn)(int level, const char *line, void *user);

typedef struct megler_config {
    /* Where everything the layer keeps lives: each app's files, saves and settings (the package's LocalState folder). */
    const char *data_dir;
    /* The installed package's folder: ANGLE's DLLs and the self-test's libraries are read from it. */
    const char *package_dir;
    megler_log_fn log;
    void *log_user;
    /* What the OS allows the app to use and what it uses now (MemoryManager), in bytes; either may be NULL. */
    uint64_t (*memory_limit)(void);
    uint64_t (*memory_usage)(void);
} megler_config;

/*
 * Once, at start-up. Returns false when this process cannot run Android code
 * at all -- the codeGeneration capability is missing, so no memory can be made
 * executable. The APK functions below still work then.
 */
bool megler_init(const megler_config *config);
const char *megler_version(void);
void megler_free(void *p);

/* ------------------------------------------------------------------- APKs */

/*
 * What an APK is and whether it can run here, as JSON (free with megler_free;
 * NULL only when out of memory):
 *
 *   {
 *     "ok": true,                      false: "error" says why the file could not be read
 *     "package": "com.example.game",
 *     "label": "Example Game",         the application's name, when it is a plain string
 *     "versionName": "1.2.3", "versionCode": 123,
 *     "minSdk": 16, "targetSdk": 28,
 *     "abis": ["armeabi-v7a", "arm64-v8a"],
 *     "abi": "armeabi-v7a",            the one that would be used; absent when there is none
 *     "runnable": true,                false: "reason" says why not (no 32-bit ARM code, no native code at all, ...)
 *     "reason": "...",
 *     "engine": "native-activity",     what would drive it: "native-activity", "sdl", "unity", "cocos2d-x", "unreal", "godot", "java", "unknown"
 *     "supported": true,               whether that engine has a driver here yet
 *     "activity": "android.app.NativeActivity",   the launcher activity's class
 *     "orientation": "landscape",      how that activity asks to be held (android:screenOrientation): "landscape" for
 *                                      landscape, sensorLandscape, reverseLandscape and userLandscape; "portrait" for
 *                                      portrait and its three like; "unspecified" for everything else and for none,
 *                                      which on a phone means upright unless it is turned
 *     "nativeLibrary": "main",         android.app.lib_name, for a NativeActivity
 *     "hasIcon": true,                 whether megler_apk_icon will find one
 *     "libraries": [ { "name": "libmain.so", "size": 123456, "needs": ["libc.so", "liblog.so"],
 *                      "armFunctions": 0, "thumbFunctions": 812,     exported functions by mode
 *                      "importsNotInSystem": 3 } ],                  imports this layer's system libraries lack
 *                                                                    (another library of the app may provide them)
 *     "notes": ["..."]                 things worth telling the user: ARM-mode code, Vulkan only, ...
 *   }
 *
 * It reads the APK and nothing else: no code is loaded.
 */
char *megler_apk_report(const char *apk_path);

/*
 * The APK's launcher icon as the bytes of its image file (free with megler_free),
 * or false when it has none this layer can find. A PNG when the APK has one in
 * any density; otherwise a WebP, which Windows 10 Mobile cannot decode.
 */
bool megler_apk_icon(const char *apk_path, uint8_t **data, size_t *size);

/* ---------------------------------------------------------------- running
 *
 * One app runs per process: Android libraries cannot be unloaded, so after
 * megler_stop (or the app's own exit) the process must end before another app
 * -- or the same one again -- can start.
 *
 * The screen. The app draws with OpenGL ES, which ANGLE turns into Direct3D
 * 11. ANGLE makes the Direct3D device; the shell makes a swap chain on that
 * same device and shows it. Each frame the app renders into a texture the
 * shell provided, and the shell copies that texture to its back buffer and
 * presents:
 *
 *   attach_device    once, when ANGLE has its device (an ID3D11Device *; the
 *                    shell takes its own reference). Make the swap chain here.
 *   acquire_target   when the app creates its window surface: return an
 *                    ID3D11Texture2D * of exactly width x height,
 *                    DXGI_FORMAT_B8G8R8A8_UNORM, bindable as a render target
 *                    and a shader resource, made on that device. The layer
 *                    takes no reference; it stays valid until release_target.
 *   present          a frame is complete in the target. Copy it to the back
 *                    buffer (CopyResource -- nothing that changes the device
 *                    context's pipeline state, which is ANGLE's) and Present.
 *                    The image is upside down by Direct3D's convention (row 0
 *                    is the bottom of the picture): flip it when showing it.
 *   release_target   the window surface is gone.
 *
 * All four are called on the app's own render thread, never on the UI thread.
 *
 * An app whose user interface is Java and that draws no OpenGL does none of
 * that: its views are drawn in memory (megler_gfx_host below) and each finished
 * picture of the whole screen comes through one more call:
 *
 *   present_pixels   `pixels` is the picture: width x height pixels of four
 *                    bytes, in memory B, G, R, A, colour already multiplied by
 *                    alpha, the top row first, `stride` bytes from one row to
 *                    the next (DXGI_FORMAT_B8G8R8A8_UNORM, row 0 the top: no
 *                    flip). Width and height are megler_run_config's.
 *                    COPY IT BEFORE RETURNING: the memory is the app's own
 *                    bitmap and is drawn into again as soon as this returns.
 *                    Then show the copy, on the shell's own thread or this
 *                    one. The picture is the whole window with nothing behind
 *                    it: take it as opaque and ignore the alpha byte (where
 *                    the app drew nothing the bytes are zero, which is black).
 *                    It replaces present for such an app: attach_device and
 *                    acquire_target are never called for it, so the device
 *                    and swap chain are the shell's own, made when the first
 *                    picture comes. Called when something on screen changed,
 *                    not at a steady rate, on the app's main thread (the one
 *                    its Activity runs on), one call at a time, never on the
 *                    UI thread. `pixels` NULL would mean the app has nothing
 *                    to show any more; the layer does not pass that on today,
 *                    and a shell that is handed it leaves the last picture.
 *                    May be NULL: such an app then runs unseen. An app that
 *                    draws both ways, views over OpenGL, is not provided for.
 */
typedef struct megler_window_host {
    void *user;
    bool (*attach_device)(void *user, void *d3d11_device);
    void *(*acquire_target)(void *user, int width, int height);
    void (*present)(void *user);
    void (*release_target)(void *user);
    void (*present_pixels)(void *user, const void *pixels, int width, int height, int stride);
} megler_window_host;

/*
 * Sound. The layer mixes everything the app plays into one stream of 16-bit
 * stereo and hands it over ten milliseconds at a time:
 *
 *   open    once, when the app first makes a sound: start an output at `rate`
 *           Hz (44100). Return false if there is none; the app then runs silent.
 *   write   `frames` stereo frames (2 * frames samples) to play next. It should
 *           return when they are queued, blocking while more than about 50 ms
 *           is already waiting: the layer's mixer runs as fast as this lets it.
 *           Called on the mixer's thread only.
 *
 * All of it may be left NULL.
 */
typedef struct megler_audio_host {
    void *user;
    bool (*open)(void *user, int rate);
    void (*write)(void *user, const int16_t *samples, int frames);
} megler_audio_host;

/*
 * Films. An app that plays a video file hands it to Android's decoders; here
 * the layer copies the file out to one of its own and the shell decodes it
 * with whatever the system has (Media Foundation's source reader on a phone):
 *
 *   open         `path` is an MP4 (or whatever the app shipped) in the data
 *                folder. Fill in what it holds and return a handle, or NULL
 *                when it cannot be played.
 *   read_video   the next picture into `rgba`: width * height * 4 bytes, red
 *                first, top row first. 1, 0 at the end of the file, -1 when
 *                it cannot be decoded. `time_us` is when it is to be shown.
 *   read_audio   up to `max_frames` frames of 16-bit sound, in the file's own
 *                rate and channels, interleaved. How many, 0 at the end, -1.
 *   seek         both go on from this time.
 *   close        the handle is done with.
 *
 * Called from the app's threads, one call at a time for a handle. All of it
 * may be left NULL: the app is then told its file cannot be opened.
 */
typedef struct megler_media_info {
    int width, height;                 /* 0: no picture */
    int frame_rate_num, frame_rate_den;
    int sample_rate, channels;         /* 0: no sound */
    int64_t duration_us;
} megler_media_info;

typedef struct megler_media_host {
    void *user;
    void *(*open)(void *user, const char *path, megler_media_info *info);
    int (*read_video)(void *user, void *media, uint8_t *rgba, int64_t *time_us);
    int (*read_audio)(void *user, void *media, int16_t *samples, int max_frames, int64_t *time_us);
    bool (*seek)(void *user, void *media, int64_t time_us);
    void (*close)(void *user, void *media);
} megler_media_host;

/*
 * Drawing, for an app whose user interface is Java.
 *
 * Such an app draws with android.graphics: a Canvas over a Bitmap, a Paint,
 * text, pictures out of image files. The layer runs those classes and keeps
 * the bitmaps and all the bookkeeping (save counts, the current matrix, the
 * clip's bounds, paths taken apart, parameters checked). What it asks of the
 * shell is the drawing proper, text and image files: on a phone Direct2D,
 * DirectWrite and WIC. This comment is the whole contract. Where the numbers
 * come from, the framework's megler.Gfx class and what it packs into arrays, is
 * written out in core/src/drivers/hk_java_gfx.c; nothing of that packing
 * reaches the shell.
 *
 * Any function may be NULL. What it would have drawn is then missing, what it
 * would have returned is zero, and the log says so once.
 *
 * PIXELS AND COORDINATES
 *
 *   A bitmap is memory the layer owns: width x height pixels of four bytes,
 *   in memory B, G, R, A (the word 0xAARRGGBB), colour already multiplied by
 *   alpha (so no colour byte is above the alpha byte), top row first, `stride`
 *   bytes from one row to the next (4 * width). That is
 *   DXGI_FORMAT_B8G8R8A8_UNORM with premultiplied alpha, WIC's 32bppPBGRA.
 *   The address does not change while the bitmap lives.
 *
 *   x grows to the right and y DOWN. (0, 0) is the top left corner of the top
 *   left pixel: pixel (i, j) covers i..i+1 by j..j+1 and its centre is at
 *   (i + 0.5, j + 0.5).
 *
 *   A colour argument (a paint's, a gradient's, draw_color's) is 0xAARRGGBB
 *   and NOT premultiplied. Only pixels in memory are premultiplied. Colours
 *   are sRGB values used as they are: nothing is converted to linear light
 *   for blending or for gradients, and there is no colour management.
 *
 *   A matrix is six floats and takes a point of the drawing's own coordinates
 *   to pixels:
 *       x' = m[0] * x + m[1] * y + m[2]
 *       y' = m[3] * x + m[4] * y + m[5]
 *   (As a D2D1_MATRIX_3X2_F: _11 = m[0], _12 = m[3], _21 = m[1], _22 = m[4],
 *   _31 = m[2], _32 = m[5].) It can rotate, skew and mirror; there is no
 *   perspective. A rectangle is left, top, right, bottom, with left <= right
 *   and top <= bottom.
 *
 *   Handles (canvases, typefaces, shaders) are the shell's own pointers,
 *   opaque to the layer. A NULL typeface is the default face; a NULL shader is
 *   no shader.
 *
 * THREADS
 *
 *   The app's threads call these, mostly its main thread; never the shell's
 *   UI thread. Calls that use one canvas come one at a time. Calls for
 *   different canvases, and the text measuring, typeface, shader and image
 *   functions, may come from several threads at once (apps decode pictures
 *   and lay out text in the background): the layer does not serialise them.
 *   That includes one shader or one typeface used by drawing calls on two
 *   canvases from two threads at the same moment, and shader_matrix for a
 *   shader that another thread is drawing with: the shell keeps such a
 *   shader whole itself (a lock, or a brush made per call from what the
 *   shader holds). Android promises an app no more than that its own two
 *   threads do not corrupt each other's drawing.
 *
 * CANVASES
 *
 *   canvas_new    a canvas that draws into a bitmap's memory. What the memory
 *                 holds at that moment is its picture to start with. The
 *                 matrix is the identity and the clip is the whole bitmap.
 *                 The shell may draw straight into the memory, or keep a
 *                 surface of its own and collect drawing calls; flush and
 *                 touched are what keeps the memory and such a surface one
 *                 picture. Several canvases can exist on one bitmap. NULL:
 *                 it cannot be made, and nothing is drawn through it.
 *   canvas_free   done with. Every save has been restored and the canvas has
 *                 been flushed by then. The memory itself is the layer's, and
 *                 is not touched after this returns.
 *   flush         when this returns, the memory holds everything drawn through
 *                 the canvas so far: the layer, the app or another canvas is
 *                 about to read it. Only called when something was drawn since
 *                 the last flush.
 *   touched       the memory was written by someone else (the layer itself,
 *                 the app's native code, another canvas) since the last flush:
 *                 take it as the picture again before drawing more. Called
 *                 once, before the next drawing call on the canvas, however
 *                 many writes there were; and not at all when that call is a
 *                 draw_color that replaces every pixel (SRC or CLEAR with
 *                 nothing saved and no clip), which is how every frame of an
 *                 app's window begins. Only a shell whose drawing is in the
 *                 memory when each drawing call returns needs neither
 *                 function. One that draws straight into the memory but only
 *                 has it there after ending a batch (Direct2D's EndDraw) needs
 *                 flush to end the batch, and touched only if it keeps
 *                 anything of the picture outside the memory.
 *                 A flush can come while layers are open (save_layer below):
 *                 the memory then holds what was drawn outside them, as on
 *                 Android, where a layer is a picture of its own until its
 *                 restore; the layers stay open and what is drawn next still
 *                 goes into them.
 *
 *   save          remembers the clip. restore brings back the clip of the
 *                 matching save. They nest; the layer never calls restore
 *                 without a save before it, and does the counting.
 *   save_layer    a save, and from here to its restore everything is drawn
 *                 into a transparent layer of its own instead of onto the
 *                 picture. At that restore the layer is put onto what was
 *                 there before: each of its pixels multiplied by alpha / 255
 *                 (alpha is 0..255) and combined by `blend` (MEGLER_GFX_BLEND_*
 *                 below, the layer being the source). `bounds`, when not
 *                 NULL, is a rectangle in the matrix of that moment that is
 *                 all the layer has to cover: the clip is intersected with it
 *                 until the restore. Under a matrix that turns or slants it,
 *                 what the layer covers and the clip is cut to is the
 *                 upright box round the rectangle in the bitmap's pixels, as
 *                 on Android (and as the layer's own clip box has it), not
 *                 the slanted rectangle. The blend takes in the layer's whole
 *                 extent (the clip at the save, within bounds), transparent
 *                 pixels too: a DST_IN layer erases what its empty parts
 *                 cover. Layers nest. A layer has to outlive a flush (above)
 *                 and can stay open across many calls, so it is a surface of
 *                 the shell's own, drawn into and put onto the picture at the
 *                 restore; something that must be closed before a batch of
 *                 drawing ends, like Direct2D's PushLayer, cannot be it.
 *   set_matrix    the matrix for what is drawn from now on, whole: the layer
 *                 keeps the current matrix and does Android's concatenation
 *                 itself. save and restore do not keep it; after a restore
 *                 the layer sets it again when it differs.
 *
 *   clip_rect     op MEGLER_GFX_CLIP_INTERSECT (1): from now on only what is
 *   clip_path     inside the shape and inside the clip so far can be drawn
 *                 on. MEGLER_GFX_CLIP_DIFFERENCE (0): the shape is cut out of
 *                 the clip. The shape is taken in the matrix of this moment
 *                 and is fixed in pixels after that: a later set_matrix does
 *                 not move it. Under a matrix that rotates, the clip is the
 *                 rotated shape, not its bounding box. A path is clipped to
 *                 as it would be filled (`fill` as for draw_path); one with
 *                 nothing in it (count can be 0) has no inside. Whether the
 *                 clip's edge is smoothed is the shell's choice. The layer
 *                 keeps the clip's bounding box itself and makes no drawing
 *                 call while that is empty.
 *
 * DRAWING
 *
 *   A shape is drawn with a megler_gfx_paint (below). `style` says whether its
 *   inside is filled, its outline stroked, or both. A stroke is centred on
 *   the outline, half its width to each side; the width is in the drawing's
 *   coordinates and goes through the matrix like everything else; width 0 is
 *   a hairline, one pixel wide whatever the matrix. Caps are for the ends of
 *   open figures and joins for corners. A miter join that would reach
 *   further than miter_limit times the stroke's width, measured from the
 *   corner's inner point to its tip, is a bevel (PostScript's rule and
 *   Android's; Direct2D's "miter or bevel" with the same number).
 *
 *   With MEGLER_GFX_ANTI_ALIAS in the paint's flags, edges are smoothed.
 *   Without it a pixel is drawn whole or not at all, by whether its centre is
 *   inside the shape (a centre exactly on a left or top edge is inside, on a
 *   right or bottom edge outside). Either way a filled rectangle with whole
 *   numbers for corners, under a matrix that only translates by whole
 *   numbers, covers exactly those pixels.
 *
 *   What is drawn is combined with what is there by the paint's `blend`:
 *   one of the numbers below, Android's PorterDuff modes. S is what is drawn
 *   and D what is there, both premultiplied, Sa and Da their alphas:
 *
 *       0 CLEAR     0                 6 DST_IN    D*Sa                 12 PLUS      min(S + D, 1)
 *       1 SRC       S                 7 SRC_OUT   S*(1-Da)             13 MODULATE  S*D
 *       2 DST       D                 8 DST_OUT   D*(1-Sa)             14 SCREEN    S + D - S*D
 *       3 SRC_OVER  S + D*(1-Sa)      9 SRC_ATOP  S*Da + D*(1-Sa)
 *       4 DST_OVER  D + S*(1-Da)     10 DST_ATOP  D*Sa + S*(1-Da)
 *       5 SRC_IN    S*Da             11 XOR       S*(1-Da) + D*(1-Sa)
 *
 *      15 OVERLAY   S*(1-Da) + D*(1-Sa) + (2*D <= Da ? 2*S*D : Sa*Da - 2*(Da-D)*(Sa-S))
 *      16 DARKEN    S*(1-Da) + D*(1-Sa) + min(S*Da, D*Sa)
 *      17 LIGHTEN   S*(1-Da) + D*(1-Sa) + max(S*Da, D*Sa)
 *
 *   Each is for every channel, the alpha too (with Sa for S and Da for D;
 *   the last three then all come to Sa + Da - Sa*Da). 3 is the ordinary case
 *   and by far the commonest. A paint's blend is never DST, which draws
 *   nothing; a layer's can be, and such a layer is thrown away at its
 *   restore. Whatever the mode, only pixels inside the shape drawn (and the
 *   clip) change: a rectangle drawn with CLEAR clears that rectangle. For a
 *   partly covered pixel of a smoothed edge the result is mixed with what
 *   was there by the coverage.
 *
 *   A shader, when not NULL, gives the colours in place of paint->color: the
 *   fill, the stroke or the glyphs are filled with it, and the alpha of
 *   paint->color (its top byte) multiplies the result.
 *
 *   draw_color       fills the whole clip with a colour, whatever the matrix.
 *                    `blend` as above: CLEAR empties the clip to transparent,
 *                    SRC replaces what is there, alpha included.
 *   draw_rect        the rectangle; draw_oval the ellipse that touches the
 *   draw_oval        rectangle's four sides; draw_round_rect the rectangle
 *   draw_round_rect  with a quarter ellipse of radii rx, ry at each corner
 *                    (0 < rx <= half the width, 0 < ry <= half the height).
 *   draw_path        a path: `count` floats, each figure a verb followed by
 *                    its points, every coordinate absolute:
 *                        MEGLER_GFX_PATH_MOVE  (0)  x y              a figure starts here
 *                        MEGLER_GFX_PATH_LINE  (1)  x y              a line to
 *                        MEGLER_GFX_PATH_QUAD  (2)  cx cy x y        a quadratic Bezier: control, end
 *                        MEGLER_GFX_PATH_CUBIC (3)  ax ay bx by x y  a cubic Bezier: two controls, end
 *                        MEGLER_GFX_PATH_CLOSE (4)                   a line back to the figure's start; the figure is closed
 *                    The stream is well formed: it begins with a MOVE, every
 *                    figure does, and nothing follows a CLOSE but a MOVE or
 *                    the end. A figure without CLOSE is open: filled as if
 *                    closed, stroked with caps at its two ends. A MOVE with
 *                    nothing after it draws nothing. `fill` says what is
 *                    inside: MEGLER_GFX_WINDING (0), where the outline winds
 *                    round a point a number of times that is not zero, or
 *                    MEGLER_GFX_EVEN_ODD (1), where a ray from it crosses the
 *                    outline an odd number of times. Arcs, the corners of
 *                    Android's round rectangles and its ovals inside paths
 *                    arrive as cubics; drawArc, drawLine(s) and drawPoint(s)
 *                    arrive as paths too (a line is one MOVE and one LINE,
 *                    stroked; a point is a small square or circle, filled).
 *   draw_bitmap      the part `src` (a rectangle in the image's pixels, inside
 *                    the image) of another bitmap, stretched onto the
 *                    rectangle `dst` of the drawing's coordinates, through the
 *                    matrix. Each pixel is multiplied by the alpha of
 *                    paint->color and combined by paint->blend; the rest of
 *                    paint->color is not used. MEGLER_GFX_FILTER_BITMAP in the
 *                    flags: smooth sampling (bilinear); without it the
 *                    nearest pixel. MEGLER_GFX_ANTI_ALIAS in the flags: the
 *                    edges of the rectangle it lands on are smoothed, as a
 *                    filled rectangle's are, where they fall between pixels
 *                    or the matrix turns them; without it they are hard.
 *                    Pixels outside `src` must not show at
 *                    the edges of the result: Android draws its stretchable
 *                    pictures (nine-patches) as a dozen pieces of one bitmap.
 *                    The image's memory is only good during the call: a shell
 *                    that draws later copies what it needs first. The memory
 *                    can be the canvas's own; it has been flushed.
 *   draw_text        one run of text on one line, its baseline at y. x is
 *                    where the run starts for text_align 0, its middle for 1,
 *                    its end for 2, by the width text_measure gives. See TEXT.
 *
 * THE PAINT
 *
 *   The layer fills it in from Android's Paint and has checked it: every
 *   enumeration is in range, text_size, miter_limit and text_scale_x are
 *   above zero, stroke_width is not negative.
 *
 *   A colour filter (filter_mode not -1) changes the colours of what is drawn
 *   before it is combined with the picture: each colour C about to be drawn
 *   -- every pixel of the image in draw_bitmap, every colour the shader
 *   gives; premultiplied, and already multiplied by the alpha of
 *   paint->color -- becomes the blend above, by the number in filter_mode,
 *   of S = filter_color and D = C. Then that is combined with the picture by
 *   paint->blend. (5, SRC_IN, is the common filter: the picture's shape in
 *   the filter's colour, which is how Android tints icons.) For a plain
 *   colour the layer has done it already, so a filter only arrives in
 *   draw_bitmap and with a shader.
 *
 *   A shadow (shadow_radius above 0) is a blurred copy of what the call
 *   draws, in one colour, under it and moved by (shadow_dx, shadow_dy). As
 *   Android draws it:
 *     - Offset and radius are in the drawing's coordinates, like everything
 *       else of the call: the offset goes through the matrix as a direction
 *       (a turned canvas turns it, a doubled one doubles it), and the blur
 *       grows with the matrix's scale. The blur's standard deviation is
 *       0.57735 * radius + 0.5 before that scale. (The original draws both
 *       in the bitmap's pixels whatever the matrix; the two agree while the
 *       matrix only moves, which is nearly always.)
 *     - Its colour is shadow_color's red, green and blue. Its alpha is
 *       shadow_color's when that is less than 255, and the paint's (the
 *       alpha of paint->color) when shadow_color is opaque; times how much
 *       the call's shape covers each pixel. The shape's own colour, shader
 *       and colour filter do not show in the shadow.
 *     - It is part of the call: the clip cuts it, paint->blend combines it
 *       with the picture as it does the shape, and it is drawn first, the
 *       shape over it.
 *   Text is what uses it; of the other calls only those of a canvas on a
 *   bitmap are given one on Android, which is every canvas here.
 *
 * TEXT
 *
 *   Text is UTF-16, `count` code units, not terminated. It can hold whatever
 *   a Java string can: unpaired surrogates, control characters. It is one
 *   run on one line; a line feed or a tab in it draws nothing and is not a
 *   line break. Shaping and the fallback to other fonts for characters the
 *   face lacks are the system's. The size is paint->text_size, in the
 *   drawing's coordinates: the height of the em, as Android and DirectWrite
 *   both mean it. Three paint fields change the plain run: every advance is
 *   multiplied by text_scale_x and the glyphs are that much wider; every
 *   character's advance grows by letter_spacing * text_size * text_scale_x
 *   -- every cluster's, not every code unit's: a surrogate pair, a letter
 *   with its combining marks or a ligature the face does not split gets it
 *   once -- half of it before the cluster and half after, so that a run
 *   starts half a spacing in and ends half a spacing early, as on Android;
 *   and a point of a glyph at (x, y) from its place on the baseline, y
 *   down, is drawn at (x + text_skew_x * y, y), so -0.25 leans to the right.
 *   The skew, the style and the fake bold below change what a glyph looks
 *   like and where its ink reaches, never an advance. The measuring
 *   functions answer for the same paint that draws, scale and spacing
 *   included, and are called very often (the framework lays text out by
 *   measuring it piece by piece): they want a cache.
 *
 *   The paint's style applies to glyphs as to shapes: filled, their outlines
 *   stroked with stroke_width, or both. MEGLER_GFX_FAKE_BOLD thickens them by
 *   about text_size / 24 (DirectWrite's bold simulation will do).
 *   MEGLER_GFX_ANTI_ALIAS smooths them in grey levels; no ClearType, the
 *   bitmap may be scaled or blended afterwards. Underlines and strike-through
 *   are drawn by the layer as rectangles.
 *
 *   What has no glyph: a tab advances as a space does and draws nothing (the
 *   framework's own line layout places what follows a tab; a tab only comes
 *   here inside a run that is measured or drawn as it stands). Every other
 *   control character -- below U+0020, and U+007F to U+009F -- and the
 *   characters Unicode says to ignore when a face lacks them (zero-width
 *   joiners and spaces, direction marks, variation selectors) advance
 *   nothing and draw nothing. A line feed is one of those to text_measure,
 *   text_widths, text_bounds and draw_text. None of this was read out of
 *   Android's text code, which is not at hand here: it is what Android's
 *   widgets are seen to do, and is the rule to keep the functions agreeing
 *   with each other.
 *
 *   Glyphs with colours of their own (emoji) keep them: paint->color does
 *   not paint them and a shader does not fill them. The paint's alpha, its
 *   colour filter and its blend apply to them as to a bitmap.
 *
 *   text_measure   the run's advance width: from where it starts to where
 *                  the next character would go.
 *   text_metrics   the face's vertical measures at paint->text_size, as
 *                  distances from the baseline with y down, into out[0..4]:
 *                      out[0]  top      the highest any glyph of the face reaches (negative)
 *                      out[1]  ascent   the recommended distance above the baseline (negative)
 *                      out[2]  descent  the recommended distance below it (positive)
 *                      out[3]  bottom   the lowest any glyph reaches (positive)
 *                      out[4]  leading  the extra gap the face asks for between lines (zero or more)
 *                  and returns the line spacing, -ascent + descent + leading.
 *                  top <= ascent and bottom >= descent. They do not depend
 *                  on the text. (DirectWrite's font metrics times
 *                  text_size / designUnitsPerEm: ascent, descent, lineGap,
 *                  and the glyph box for top and bottom.)
 *   text_bounds    the smallest rectangle round the run's ink when drawn at
 *                  (0, 0) aligned left: out[0..3] = left, top, right, bottom,
 *                  y down from the baseline (top is negative for most text).
 *                  All zero when nothing would be drawn. It is the ink as
 *                  this paint draws it: slanted by text_skew_x (the box of
 *                  the slanted glyphs, not the upright box slanted), thicker
 *                  by fake bold, and wider all round by half the stroke width
 *                  when the style strokes. The shadow is not part of it.
 *   text_break     how many code units from the start fit in max_width.
 *                  `words` false: as many whole characters (clusters) as fit,
 *                  possibly 0. `words` true: what the first line would
 *                  hold were the text wrapped at max_width: the longest start
 *                  that ends where a line may break (after a space, by the
 *                  Unicode rules) and fits, spaces at its end not counting
 *                  against the width; when not even the first word fits,
 *                  what `words` false gives. Either way a line feed ends the
 *                  line and is counted in it: the answer never reaches past
 *                  the first line feed, and is `count` only when all of the
 *                  text fits and no line feed comes before its last unit.
 *                  (The original's typesetter ends a line there in both
 *                  modes, and the framework's layout cuts paragraphs at line
 *                  feeds before it asks. Android's own Paint.breakText, which
 *                  is `words` false, measures on past a line feed; an app
 *                  that hands it text of several lines gets a smaller answer
 *                  here.)
 *   text_widths    the advance of each code unit into out[0..count): a unit
 *                  with none of its own (the second half of a surrogate pair,
 *                  a combining mark, the later letters of a ligature unless
 *                  the system splits it) gets 0. They add up to text_measure.
 *
 * TYPEFACES
 *
 *   typeface_create     a face by Android's family name. `family` is in
 *                       lower case: "sans-serif", "serif", "monospace",
 *                       "sans-serif-medium", "sans-serif-condensed",
 *                       "cursive", or anything an app thinks of. The layer
 *                       has read out of it what Android would: `generic` is
 *                       MEGLER_GFX_SANS (0), MEGLER_GFX_SERIF (1) or
 *                       MEGLER_GFX_MONOSPACE (2); `weight` is 1..999 as CSS and
 *                       DirectWrite count it, 400 regular and 700 bold. Any
 *                       system font of that kind will do; on Android the
 *                       sans-serif is Roboto.
 *   typeface_derive     the same font as `typeface` (NULL: the default) at
 *                       another weight, upright or italic.
 *   typeface_from_data  a face out of a font file's bytes (TrueType or
 *                       OpenType; apps carry their own fonts). The bytes are
 *                       the app's: copy them. NULL when it is not a font.
 *
 *   A typeface is never freed: megler.Gfx has no call for it. The layer makes
 *   each derived face once. The default face, NULL, is the upright
 *   sans-serif at weight 400.
 *
 * SHADERS
 *
 *   A shader's coordinates are the drawing's, at the draw that uses it: it
 *   moves with the canvas's matrix. shader_matrix gives it a matrix of its
 *   own, applied first (shader coordinates to drawing coordinates); NULL is
 *   the identity again.
 *
 *   shader_linear   colours along the line from (x0, y0) to (x1, y1).
 *   shader_radial   colours by the distance from (cx, cy): position 0 at the
 *                   centre, 1 at `radius`.
 *   shader_sweep    colours by the angle round (cx, cy): position 0 points
 *                   along +x (three o'clock), positions grow clockwise as
 *                   seen on screen, 1 is the full turn.
 *                   For all three: `count` colours (at least 2), 0xAARRGGBB
 *                   not premultiplied, each at positions[i], which are in
 *                   0..1 and never go down. Between two stops the colour is
 *                   interpolated as it stands, not premultiplied, and
 *                   premultiplied afterwards (Direct2D's straight mode).
 *                   Before the first stop and after the last, by `tile`:
 *                   MEGLER_GFX_TILE_CLAMP (0) the end colours go on,
 *                   MEGLER_GFX_TILE_REPEAT (1) the gradient starts again,
 *                   MEGLER_GFX_TILE_MIRROR (2) it runs back and forth.
 *   shader_bitmap   an image as the pattern: its top left pixel's corner at
 *                   (0, 0), a pixel to a unit, continued by `tile` in both
 *                   directions (clamp: the edge pixels go on). The pixels are
 *                   copied when the shader is made: it does not follow the
 *                   bitmap afterwards, and the memory is not used after the
 *                   call returns. MEGLER_GFX_FILTER_BITMAP of the paint that
 *                   draws with it chooses the sampling.
 *   shader_free     done with.
 *
 * IMAGE FILES
 *
 *   image_size     whether the bytes are an image file the shell can decode,
 *                  and its size in pixels. Apps ask for the size alone, often:
 *                  read the header, do not decode.
 *   image_decode   the same bytes into `pixels`: a bitmap of the format above,
 *                  exactly width x height. The layer asks for the picture's
 *                  own size divided by 1, 2, 4... (Android's inSampleSize),
 *                  never below 1 pixel; when that is not the picture's own
 *                  size, scale with a filter that is good for reducing. The
 *                  first frame of an animation. A picture without alpha is
 *                  opaque (A = 255). Orientation in the file's metadata is
 *                  not applied. False: it could not be decoded, and the
 *                  memory may hold anything.
 *   image_encode   the bitmap as an image file, handed to `write` in as many
 *                  pieces as is convenient (its second argument is `sink`,
 *                  passed through; false from it means stop and fail).
 *                  MEGLER_GFX_PNG (1): with alpha, the colours un-premultiplied
 *                  as PNG wants them; `quality` means nothing.
 *                  MEGLER_GFX_JPEG (0): `quality` 0..100; alpha is dropped and
 *                  the colours written as they are in memory, which is the
 *                  picture over black. False when it cannot be written.
 *   bitmap_gone    a bitmap whose image was passed in (megler_gfx_image.id) is
 *                  freed. See below.
 *
 * An image handed to draw_bitmap or shader_bitmap comes with two numbers for
 * a shell that keeps textures: `id` is the bitmap's own for as long as it
 * lives, and `version` is different whenever the pixels may have changed. A
 * copy kept under (id, version) can be used again, and bitmap_gone says when
 * to drop it. A shell that copies every time needs none of the three.
 */
enum { MEGLER_GFX_FILL = 0, MEGLER_GFX_STROKE = 1, MEGLER_GFX_FILL_AND_STROKE = 2 };
enum { MEGLER_GFX_CAP_BUTT = 0, MEGLER_GFX_CAP_ROUND = 1, MEGLER_GFX_CAP_SQUARE = 2 };
enum { MEGLER_GFX_JOIN_MITER = 0, MEGLER_GFX_JOIN_ROUND = 1, MEGLER_GFX_JOIN_BEVEL = 2 };
enum { MEGLER_GFX_ALIGN_LEFT = 0, MEGLER_GFX_ALIGN_CENTER = 1, MEGLER_GFX_ALIGN_RIGHT = 2 };
enum { MEGLER_GFX_ANTI_ALIAS = 1, MEGLER_GFX_FILTER_BITMAP = 2, MEGLER_GFX_FAKE_BOLD = 4 };
enum {
    MEGLER_GFX_BLEND_CLEAR = 0, MEGLER_GFX_BLEND_SRC = 1, MEGLER_GFX_BLEND_DST = 2, MEGLER_GFX_BLEND_SRC_OVER = 3, MEGLER_GFX_BLEND_DST_OVER = 4,
    MEGLER_GFX_BLEND_SRC_IN = 5, MEGLER_GFX_BLEND_DST_IN = 6, MEGLER_GFX_BLEND_SRC_OUT = 7, MEGLER_GFX_BLEND_DST_OUT = 8, MEGLER_GFX_BLEND_SRC_ATOP = 9,
    MEGLER_GFX_BLEND_DST_ATOP = 10, MEGLER_GFX_BLEND_XOR = 11, MEGLER_GFX_BLEND_PLUS = 12, MEGLER_GFX_BLEND_MODULATE = 13, MEGLER_GFX_BLEND_SCREEN = 14,
    MEGLER_GFX_BLEND_OVERLAY = 15, MEGLER_GFX_BLEND_DARKEN = 16, MEGLER_GFX_BLEND_LIGHTEN = 17
};
enum { MEGLER_GFX_PATH_MOVE = 0, MEGLER_GFX_PATH_LINE = 1, MEGLER_GFX_PATH_QUAD = 2, MEGLER_GFX_PATH_CUBIC = 3, MEGLER_GFX_PATH_CLOSE = 4 };
enum { MEGLER_GFX_WINDING = 0, MEGLER_GFX_EVEN_ODD = 1 };
enum { MEGLER_GFX_CLIP_DIFFERENCE = 0, MEGLER_GFX_CLIP_INTERSECT = 1 };
enum { MEGLER_GFX_TILE_CLAMP = 0, MEGLER_GFX_TILE_REPEAT = 1, MEGLER_GFX_TILE_MIRROR = 2 };
enum { MEGLER_GFX_SANS = 0, MEGLER_GFX_SERIF = 1, MEGLER_GFX_MONOSPACE = 2 };
enum { MEGLER_GFX_JPEG = 0, MEGLER_GFX_PNG = 1 };

typedef struct megler_gfx_paint {
    uint32_t color;                    /* 0xAARRGGBB, not premultiplied; its alpha is the paint's alpha */
    int style;                         /* MEGLER_GFX_FILL, _STROKE, _FILL_AND_STROKE */
    float stroke_width;                /* 0: a hairline */
    int flags;                         /* MEGLER_GFX_ANTI_ALIAS | MEGLER_GFX_FILTER_BITMAP | MEGLER_GFX_FAKE_BOLD */
    int cap, join;                     /* MEGLER_GFX_CAP_*, MEGLER_GFX_JOIN_* */
    float miter_limit;                 /* 4 unless the app says otherwise */
    float text_size;                   /* the em's height, in the drawing's coordinates */
    int text_align;                    /* MEGLER_GFX_ALIGN_*: what draw_text's x is */
    float text_scale_x;                /* 1: as the face is */
    float text_skew_x;                 /* 0: upright */
    float letter_spacing;              /* in ems, added to every advance */
    int blend;                         /* MEGLER_GFX_BLEND_*; never DST */
    int filter_mode;                   /* -1: no colour filter; else a MEGLER_GFX_BLEND_* number */
    uint32_t filter_color;             /* 0xAARRGGBB, not premultiplied */
    float shadow_radius;               /* 0: no shadow */
    float shadow_dx, shadow_dy;
    uint32_t shadow_color;             /* 0xAARRGGBB, not premultiplied */
} megler_gfx_paint;

/* Another bitmap's pixels as the source of a drawing call. */
typedef struct megler_gfx_image {
    const void *pixels;
    int width, height, stride;
    uint32_t id;                       /* this bitmap's, never 0, until bitmap_gone(id) */
    uint32_t version;                  /* changes whenever the pixels may have */
} megler_gfx_image;

typedef bool (*megler_gfx_write)(void *sink, const void *data, size_t size);

typedef struct megler_gfx_host {
    void *user;

    void *(*canvas_new)(void *user, void *pixels, int width, int height, int stride);
    void (*canvas_free)(void *user, void *canvas);
    void (*flush)(void *user, void *canvas);
    void (*touched)(void *user, void *canvas);

    void (*save)(void *user, void *canvas);
    void (*save_layer)(void *user, void *canvas, const float *bounds, int alpha, int blend);
    void (*restore)(void *user, void *canvas);
    void (*set_matrix)(void *user, void *canvas, const float *matrix);
    void (*clip_rect)(void *user, void *canvas, float left, float top, float right, float bottom, int op);
    void (*clip_path)(void *user, void *canvas, const float *path, int count, int fill, int op);

    void (*draw_color)(void *user, void *canvas, uint32_t color, int blend);
    void (*draw_rect)(void *user, void *canvas, float left, float top, float right, float bottom, const megler_gfx_paint *paint, void *shader);
    void (*draw_round_rect)(void *user, void *canvas, float left, float top, float right, float bottom, float rx, float ry,
                            const megler_gfx_paint *paint, void *shader);
    void (*draw_oval)(void *user, void *canvas, float left, float top, float right, float bottom, const megler_gfx_paint *paint, void *shader);
    void (*draw_path)(void *user, void *canvas, const float *path, int count, int fill, const megler_gfx_paint *paint, void *shader);
    void (*draw_bitmap)(void *user, void *canvas, const megler_gfx_image *image, const float *src, const float *dst, const megler_gfx_paint *paint);
    void (*draw_text)(void *user, void *canvas, const uint16_t *text, int count, float x, float y, const megler_gfx_paint *paint,
                      void *typeface, void *shader);

    float (*text_measure)(void *user, const uint16_t *text, int count, const megler_gfx_paint *paint, void *typeface);
    float (*text_metrics)(void *user, const megler_gfx_paint *paint, void *typeface, float *out);
    void (*text_bounds)(void *user, const uint16_t *text, int count, const megler_gfx_paint *paint, void *typeface, float *out);
    int (*text_break)(void *user, const uint16_t *text, int count, float max_width, const megler_gfx_paint *paint, void *typeface, bool words);
    void (*text_widths)(void *user, const uint16_t *text, int count, const megler_gfx_paint *paint, void *typeface, float *out);

    void *(*typeface_create)(void *user, const char *family, int generic, int weight, bool italic);
    void *(*typeface_derive)(void *user, void *typeface, int weight, bool italic);
    void *(*typeface_from_data)(void *user, const void *data, size_t size);

    void *(*shader_linear)(void *user, float x0, float y0, float x1, float y1, const uint32_t *colors, const float *positions, int count, int tile);
    void *(*shader_radial)(void *user, float cx, float cy, float radius, const uint32_t *colors, const float *positions, int count, int tile);
    void *(*shader_sweep)(void *user, float cx, float cy, const uint32_t *colors, const float *positions, int count);
    void *(*shader_bitmap)(void *user, const megler_gfx_image *image, int tile);
    void (*shader_matrix)(void *user, void *shader, const float *matrix);
    void (*shader_free)(void *user, void *shader);

    bool (*image_size)(void *user, const void *data, size_t size, int *width, int *height);
    bool (*image_decode)(void *user, const void *data, size_t size, void *pixels, int width, int height, int stride);
    bool (*image_encode)(void *user, const void *pixels, int width, int height, int stride, int format, int quality, megler_gfx_write write, void *sink);
    void (*bitmap_gone)(void *user, uint32_t id);
} megler_gfx_host;

typedef struct megler_run_config {
    const char *apk_path;
    /* Split APKs of the same app (config.armeabi_v7a.apk and the like), if any. */
    const char *const *split_apks;
    int split_count;
    /* The surface the app draws to, in pixels, and the density Android should report (160 is 1:1; a phone is 320-480). */
    int width, height;
    int density_dpi;
    megler_window_host window;
    megler_audio_host audio;
    megler_media_host media;
    /*
     * Drawing for an app whose user interface is Java (see megler_gfx_host). All of it may be left NULL: such an app still starts,
     * lays its views out and takes input, but nothing is drawn (its bitmaps hold only what the layer does itself: erasing, single
     * pixels, copies), text measures nothing wide and nothing high, and image files do not decode. Other apps never use it.
     */
    megler_gfx_host gfx;
    /*
     * Where the files are that an app whose code is Java runs on: a folder laid out like Android's /system, the "Android root"
     * (docs/JAVA.md, "Starting on Android's libcore": framework/ with libcore's DEX files, the Java framework and the jars;
     * lib/ with libcore's native libraries; usr/ with ICU's data and the time zones). It is only read, so it can stay in the
     * installed package. NULL: the folder `android` in megler_config.package_dir. An app with a native engine never opens it;
     * a Java app started without it is refused, and megler_last_error says what is missing.
     */
    const char *android_root;
    /* The app has ended: by its own exit (status), by a crash (reason names it), or by megler_stop. Called once, from any thread. */
    void (*on_exit)(void *user, int status, const char *reason);
    void *user;
} megler_run_config;

/*
 * Starts the app on threads of its own and returns. False, with the reason in
 * the log and in megler_last_error, when it cannot start.
 */
bool megler_run(const megler_run_config *config);
bool megler_running(void);
const char *megler_last_error(void);

/* Touch, in surface pixels with y down (after undoing the flip). phase: 0 down, 1 move, 2 up, 3 cancel. */
void megler_touch(int phase, int pointer_id, float x, float y);
/* An Android key code (AKEYCODE_BACK is 4), pressed or released. */
void megler_key(int android_keycode, bool down);
/* The app went to the background or came back; lost or gained input focus. */
void megler_set_paused(bool paused);
void megler_set_focused(bool focused);
/* Asks the app to finish. Returns at once; on_exit follows. */
void megler_stop(void);
/* Frames presented since the app started. */
uint32_t megler_frames(void);

/* ------------------------------------------------------------ diagnostics */

typedef void (*megler_line_fn)(const char *line, void *user);

/*
 * The core's own tests, run in this process: the host layer, the APK reader,
 * and -- with the guest test libraries from <package_dir>/selftest -- real
 * Android code loaded and called, and a small app started and stopped.
 * Returns the number of failed checks. Takes a few seconds; call it from a
 * background thread.
 *
 * That small app is the one app this process may run (see "running"): after
 * the self-test, megler_run fails until Megler is closed and opened again.
 *
 * Each line is also appended to <data_dir>/megler-selftest-steps.log and
 * flushed before the next step, so a step that ends the process has still
 * said how far the test got.
 */
int megler_selftest(megler_line_fn out, void *user);

/*
 * Facts about this device that decide how Android code can run on it, one per
 * line as "name: value" -- whether a thread survives in ARM (not Thumb) state
 * and for how long, what the thread-pointer registers hold, what executable
 * memory costs, the Direct3D feature level ANGLE gets. Takes several seconds;
 * call it from a background thread. docs/ARCHITECTURE.md says what each
 * answer means. The lines also go, flushed one by one, to
 * <data_dir>/megler-probe-steps.log: some probes can end the process.
 */
void megler_probe(megler_line_fn out, void *user);

#ifdef __cplusplus
}
#endif

#endif /* MEGLER_H */
