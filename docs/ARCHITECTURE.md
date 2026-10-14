# How Megler runs Android code on Windows 10 Mobile

an earlier project runs Android games on an iPhone by loading
their native libraries into its own process and standing in for Android around them.
This is the same idea on Windows 10 Mobile. Only the translation layer is ported:
there is no emulated Android system here, and no QEMU.

The phone's CPU runs the app's code directly. Nothing is emulated or recompiled.
What this layer does is everything else: load the libraries, be the C library and
the NDK they link against, be the Java side they call through JNI, and turn their
OpenGL ES into Direct3D.

Each section below says what is **proven** (by a test that runs under qemu-arm, or
on this PC, or on a phone), and what is only **expected** until it has run on a
phone. [STATUS.md](STATUS.md) keeps the list of what has.

## The one fact everything follows from

Windows 10 Mobile is a 32-bit ARM system, on every phone, including those with
64-bit processors. A 32-bit kernel does not save or restore AArch64 state, and user
code cannot enter it. So:

- **`arm64-v8a` libraries cannot run.** Only `armeabi-v7a` (and the older `armeabi`).
  This is the mirror image of Megler on iOS, which runs only 64-bit code.
- An APK that has dropped its 32-bit libraries cannot run here at all. The app page
  says so. Older versions of most games still have them.

## The stack

```
┌──────────────────────────────────────────────────────────────────┐
│ Megler.exe: C++/WinRT shell (app/Megler)                          │
│   library, app page, SwapChainPanel, touch, device checks         │
├─────────────────────────── megler.h ────────────────────────────────┤
│ core (C11, core/src)                                               │
│   run/      one app: APKs, data directories, the driver            │
│   drivers/  the Java side of each kind of app (NativeActivity)     │
│   jni/      JNIEnv over a Java world implemented in C              │
│   ndk/      libandroid, EGL/GLES over ANGLE, OpenSL ES             │
│   bionic/   libc, libm, libdl, liblog, pthreads, files             │
│   ld/       the dynamic linker; guest thread-local storage         │
│   arm/      what is rewritten in guest code; the assembly stubs    │
│   apk/ zip/ the APK, its manifest and resources                    │
│   host/     the OS: Win32 app container, or POSIX for the tests    │
├──────────────────────────────────────────────────────────────────┤
│ ANGLE (libEGL.dll, libGLESv2.dll) → Direct3D 11                    │
└──────────────────────────────────────────────────────────────────┘
```

The core is plain C that three compilers build: MSVC for ARM (the phone), GCC for
`arm-linux-gnueabihf` (the same code under qemu-arm, where the tests execute guest
code) and MSVC for x64 (this PC, for what needs no ARM). `host/` is the only place
that includes an OS header.

## Two ABIs in one process

Guest code is compiled for Android's `armeabi-v7a`; the host is Windows on ARM. Both
are AAPCS, both are little-endian ILP32, and the phone's build of the core is
Thumb-2 like most guest code. A guest function calling a host function is an
ordinary call. The differences, and how each is bridged:

### Floating point: softfp against hard-float

Android passes floating-point arguments and results in the integer registers
(`softfp`); Windows on ARM passes them in VFP registers. A host function that takes
a `double` would look for it in `d0` while the guest put it in `r0:r1`.

So **nothing that crosses the boundary has a float in its signature.** A wrapper for
`sin` is `u64 g_sin(u64 x)`: a 64-bit integer goes in an even pair of registers,
exactly where softfp puts a double, and a 32-bit one where it puts a float. The
wrapper converts bits to a value (`hk_d`, `hk_f`), calls the host's function, and
converts back. All of libm, every OpenGL ES function with a float parameter, the JNI
calls that return floats, and the NDK's `AMotionEvent_getX` are written this way; no
assembly is involved.

The same holds the other way: the host calls a guest function through a pointer
typed with integers, passing each float as its bits.

*Proven:* `core/tests` calls guest functions with mixed int/float/double arguments
across the stack boundary, has guest code call libm and a host callback, and checks
that floats given to `glClearColor`/`glUniform4f`/`glLineWidth` by a guest arrive
intact in a stand-in OpenGL ES that takes real floats. The host under qemu is
hard-float, like the phone.

Not bridged: a structure of only floats passed **by value** to a host function (the
hard-float rules put it in VFP registers). No NDK interface does that.

### Variadic functions and `va_list`

Variadic functions take everything in integer registers and on the stack in both
conventions, and a `va_list` is a single pointer in both. A host `printf` is called
by the guest as it is; a guest `va_list` is handed to the host's `vsnprintf`.

*Proven:* the tests format and scan through guest calls, including 64-bit and
floating-point arguments.

### The thread pointer

bionic keeps its thread pointer in `TPIDRURO`, and compiled code reads it inline:
`mrc p15, 0, rX, c13, c0, 3`. On Windows that register is the kernel's; the TEB is
in `TPIDRURW`.

The loader finds every such instruction in a library's code and replaces it with a
branch to three instructions in the library's island: read the host's thread block
(`mrc ... c13, c0, 2`, the TEB), load the guest's thread pointer from a fixed offset
in it (a `TlsAlloc` slot: the first 64 are an array inside the TEB), branch back. No
other register or flag changes, which is what the original instruction promised.
The offset of the slot array in the TEB (0xE10) is checked against `TlsGetValue`
before it is trusted.

What the guest's thread pointer points at is this layer's own block: bionic's slots,
then each loaded library's thread-local data (ELF TLS, variant 1).

*Proven:* under qemu, with glibc's thread pointer standing where the TEB will be:
initial-exec TLS in a Thumb library (four rewritten reads), `__tls_get_addr` in an
ARM one, per-thread values across eight threads. *Expected:* the TEB offset; the
device probe prints both registers and whether the check passed.

### ARM mode

This is the part that Windows makes hard. Windows on ARM runs Thumb-2 only. A thread
that is in ARM (not Thumb) state when the kernel rebuilds its context comes back
in Thumb state, and whatever it was executing is then decoded as Thumb. Microsoft's
ABI document says a switch to ARM "may succeed", and that exceptions or interrupts
may then fault the application.

Android libraries contain ARM code in two forms:

**PLT entries.** A linker writes them in ARM mode even in a library that is
otherwise Thumb, so every library has them. The loader recognises each entry by its
instructions and the GOT slot it jumps through, and rewrites it into one ARM
instruction and a literal:

```
ldr pc, [pc, #-4]     ; E51FF004
.word thunk | 1       ; Thumb code in the island: jump through the GOT slot
```

A single instruction leaves no "middle" to be interrupted in, only "before". And
those four bytes read as Thumb are `F004 E51F`: a `BLX` with its low bit set, which
the architecture defines as UNDEFINED. A thread resumed there in Thumb state takes
an illegal-instruction exception at once, and the fault handler finishes the jump.
The entry works in either state.

*Proven:* the rewriting, on lld's and on OpenSSL's PLTs; and that entering those
bytes in Thumb state faults and is rescued (qemu raises the illegal instruction, as
a CPU must); and on a phone, that Windows delivers that exception to a vectored
handler with a context that can be resumed: the device probe does exactly this 200
times, at 5 µs each on a Snapdragon 820. One thing differs from Linux there. Windows
takes the state a thread resumes in from bit 0 of the context's `Pc` and ignores
the T bit of `Cpsr`, so the handler resumes Thumb code at an odd address
(`hk_fault_resume_at`).

**Whole functions.** Real libraries are built in ARM mode too: all 3885 functions
of the OpenSSL build the loader was tried on, and 42 of the 44 in CPU-Z's
`libcpuid.so`. The loader counts them and the app page reports them. **They are called as they are**, in ARM state.
Under qemu that is correct (Linux keeps a thread's state), so the tests pass on ARM
libraries too.

Whether a phone keeps a thread in ARM state is something Windows does not promise
(its ABI is Thumb-2 only), so the device probe measures it: a loop in ARM state for
a thousand iterations, then a million, up to a billion and a half, on a thread whose
faults are caught. **On the phone tried (a Snapdragon 820, build 15254) it survives**
all of it, an undefined instruction met in ARM state is reported and resumed like
one met in Thumb state, and the ARM-mode test library passes every test there. So
ARM libraries run as they are. The probe's verdict is kept
(`LocalState/arm-state.txt`), and on a phone where it says otherwise they would need
translating to Thumb-2 first. Thumb-2 has an equivalent for nearly every ARM
instruction, so the translation keeps addresses and layout; it is designed
([ARM-TO-THUMB.md](ARM-TO-THUMB.md)) and not written, having not been needed.

### Direct system calls

`svc 0` with a Linux system call number means something else to Windows. bionic's
own calls are replaced wholesale by the shims, so this matters only for code that
makes system calls inline. The loader counts the sites (`svc 0` after a number goes
into `r7`); they are not handled. `__clear_cache` in compiler-rt is one: a JIT inside
an app will reach it.

### Everything else that differs

| | Android (bionic, 32-bit) | Here |
|---|---|---|
| `pthread_mutex_t`, `pthread_cond_t`, `sem_t` | one 32-bit word each, statically initialised | implemented in that word, bionic's bit layout, waiting on its address (`WaitOnAddress`; a futex under Linux) |
| `wchar_t` | 32 bits, UTF-8 multibyte | all of `<wchar.h>` is this layer's own (Windows' is 16 bits) |
| `errno` | Linux numbers, per thread | the guest's own; host errors are translated |
| `struct stat`, `dirent`, `sigaction`, `tm` | bionic's layouts | filled in field by field |
| `FILE` | `__sF[3]`, 84 bytes apart, fields old headers read inline | this layer's own, with that layout at its start |
| `malloc`/`memalign`/`free` | one `free` for all | every block has a header saying where the host's block starts |
| file descriptors | pipes, eventfd, epoll, sockets | this layer's own table; pipes and eventfds are implemented here, sockets are the host's behind it (see "The network") |
| paths | `/data/data/<pkg>`, `/sdcard`, `/proc/cpuinfo` | a mount table onto folders under `LocalState` (see "Files and the shared storage"); the `/proc` and `/sys` files code reads are made up |
| signals | delivered asynchronously | between the app's own threads they are: a thread in the middle of Android code is stopped, sent to the handler and put back (`SuspendThread`, `SetThreadContext`, the vectored handler); one that waits or sleeps in this layer is woken for it. Nothing comes from outside the process |
| a thread's stack | all there from the start; code moves `sp` by any amount and writes | Windows hands a stack out a page at a time, through a guard page that code built for Windows touches in order. Android code does not, and a write beyond the guard page ends the process with no handler run. So the whole stack is committed, down to a guard page of its own, the first time a thread is given a guest thread pointer (`hk_thread_whole_stack`) |
| `mmap`/`munmap` | any range of pages, unmapped in any pieces | a reservation of the host's, which gives addresses back only whole: a region remembers which of its pages the app still has, decommits what is unmapped, and is released with its last page |
| address space | about 4 GB for a 32-bit process on a 64-bit kernel | 2 GB. An APK is read, never mapped; large textures an app uploads as plain RGBA8 are kept as DXT5 once it has asked for a great deal (`hk_gltex.c`) |

## Loading a library

`ld/hk_ld.c` follows Android's linker where that decides whether a library works: an
import is looked up first in the importing library's own scope (itself, then its
`DT_NEEDED` closure breadth-first) and only then in the system; dependencies are
mapped, relocated and initialised first; initialisers get `(argc, argv, envp)`.

One reservation holds `[island] image [island]`. Segments are committed read-write,
filled, relocated (REL, Android's packed APS2, RELR) and patched, and then each gets
its final protection: text becomes execute-read. Nothing is written to it again.
That order is what the app container allows: with the `codeGeneration` capability,
`VirtualProtectFromApp` turns a committed read-write page into an executable one,
and never both at once.

An island is the library's own bit of executable memory, within a branch's reach
(16 MB) of its text: the thread-pointer trampolines, the PLT thunks, and a stub for
every import nothing provides (it logs the name when called and returns zero, so a
missing function is a line in the log and not a crash at load).

Memory an app asks for as writable and executable at once (a JIT's code heap) cannot
exist. Such a region is writable until something executes in it and executable until
something writes to it: the fault handler flips the page.

## Graphics

ANGLE turns OpenGL ES into Direct3D 11, and most GLES calls are bound straight to
ANGLE's functions. This layer adds the display, the window, and the float adapters.

- The display is made as an ANGLE Direct3D 11 display, falling back to feature level
  9_3, which most Windows phones are. At 9_3 ANGLE gives OpenGL ES 2.0 only.
- The app's window is an `ANativeWindow`; its window surface is a pbuffer over a
  texture the shell provides, on ANGLE's own Direct3D device. `eglSwapBuffers` hands
  the finished frame to the shell, which copies it to its swap chain's back buffer
  and presents. The shell owns the swap chain because it has to live on a
  `SwapChainPanel` in the app's XAML.
- `ANativeWindow_setBuffersGeometry` sets the size frames are rendered at, as on
  Android.

*Proven:* the EGL and GLES plumbing, against a stand-in; and on a phone, that ANGLE
loads from the package and makes a display and a context (Direct3D 11 at feature
level 11_1 and OpenGL ES 3.0 on an Adreno 530; the device probe prints these and
which texture formats there are). *Expected:* the window surface over the shell's
texture and everything after it, up to the picture on the screen. Configurations
that are alike in every attribute can differ in channel order, which decides whether
ANGLE takes the texture, so the surface is made with the first candidate that
works, the app's own first.

There is no Vulkan on Windows 10 Mobile, so apps that need it cannot run.

### A Java app's views, on the graphics processor

An app whose user interface is Java draws its views through `megler_gfx_host`
(core/include/megler.h): the core keeps the bitmaps in memory and asks the shell to
draw into them (`app/Megler/player/GfxHost.cpp`). The first shell drew with
Direct2D's software rasterizer straight into that memory. On the phone that was
slow: a frame that redraws a caret (5% of a 720 x 1280 window, a dozen calls)
cost 18-20 ms of drawing, half of it in Direct2D's `EndDraw`, and a full window
11-26 ms. So the shell draws on the graphics processor now, with the same
contract, and keeps the software way for when there is no device for it.

- **One device.** At the first call the shell makes a hardware Direct3D 11 device
  with BGRA support (the phone's Adreno 530 has feature level 11), an
  `ID2D1Device` over it, and from then on Direct2D 1.1 draws: shapes, text
  (DirectWrite's glyph runs, in grey levels), bitmaps, gradients and effects.
  Where there is none, or it is lost, or the flag file `gfx-software` asks for
  it, canvases are software render targets over memory as before, said once in
  the log.
- **A canvas** is a device context of its own and a target bitmap of its size on
  the processor. What the memory holds is uploaded before the first drawing after
  `canvas_new` or `touched`, unless that drawing replaces every pixel (a window's
  frame begins with `drawColor`). At `flush` the box round everything drawn since
  the last flush is copied to a bitmap the processor can read and mapped, and its
  rows go into the memory, which the core and `present_pixels` read. Mapping waits
  for the processor to finish: that, and the copy, is the price of keeping the
  contract, and the log's `gfx:` line says what it is.
- **Layers** are target bitmaps of their extent, drawn back at their restore.
  **Bitmaps** drawn from the core's memory are uploaded once and kept under the
  core's id and version (`megler_gfx_image`) until `bitmap_gone` or a memory budget.
- **The host's own arithmetic** (the other seventeen transfer modes, colour
  filters, shadows, `drawColor`, layers put on by a mode) is done by Direct2D's
  built-in effects where their formula is megler.h's (Composite, Blend,
  ArithmeticComposite, Flood, GaussianBlur); what is left reads back the box it
  changes, is put on in memory as before, and is uploaded again. GfxHost.cpp
  lists which is which.
- **Threads.** The Direct2D factory is the multithreaded kind: it serialises
  every call, contexts' and the device's included, and with them Direct2D's use
  of the Direct3D device, which the shell never calls itself. Each canvas is
  drawn by one thread at a time (megler.h), and its context, targets and read-back
  bitmap are its own; the kept bitmaps, and the contexts and targets kept for the
  next canvas, have locks.

`tools/build-tests.sh win` draws the scenes (core/tests/hk_gfx_scenes.c) both
ways, on this PC's graphics processor and in memory, and replays the Java test
apps' recorded drawing.

**Presenting straight from the processor (not built).** Each frame of a window is
still drawn on the processor, read back into memory, and uploaded again by the
window host to a texture of its own device. The read-back is the one wait in the
frame. Removing it needs the core:

- megler_gfx_host would get an optional `present(canvas, x, y, width, height)`:
  show the part of the window's canvas that changed, from the shell's own
  picture, without a flush; false when it cannot (the core then flushes and
  calls `present_pixels` as today, which is also what NULL means).
- The core would call it in place of flushing the window's bitmap and calling
  `present_pixels(_part)`, when the window's picture is that one canvas's alone
  -- no OpenGL frames composed under the views, nothing written into the memory
  behind the canvas. The canvas then stays unflushed; anything that reads the
  window's memory later (a screenshot, `View.getDrawingCache`, composing with a
  GL view) flushes it first, as the core already does before every read
  (`bm_settle`), and the shell reads back then.
- The shell's window host would make its swap chain on GfxHost's device instead
  of a device of its own, and copy the changed part from the canvas's target
  bitmap into its texture (`CopySubresourceRegion`, or a `DrawBitmap` with the
  flip the texture's bottom-up rows need) before presenting. Both run on the
  app's thread that calls `present`, under Direct2D's lock (`ID2D1Multithread`),
  since the window host would then use the device's context itself.

### OpenGL ES from an app's Java

An app whose user interface is Java draws its views in memory (megler_gfx_host, and
`present_pixels` hands the shell the whole window as a picture). Some of its views
can be OpenGL ES surfaces: a `GLSurfaceView` with a `Renderer`, or any
`SurfaceView` (or `SurfaceHolder`, or `Surface`) an app makes an EGL window surface
on through `EGL14` or `EGL10`. And some apps use EGL with no view at all: a pbuffer
to ask the GPU's name, or to draw off the screen. This is how that is done here.
Android's picture of it is kept: the surface lies *under* the window, and the
views are drawn over it with a hole where the `SurfaceView` is.

**One OpenGL ES in the process.** The Java natives call the very functions native
code is bound to, ANGLE's, by name through the core (`hk_egl_host`). Two things
differ from a guest's binding: a function with float parameters is ANGLE's own and
not its adapter (the caller is the core, which passes floats as the host does),
and nothing is traced. The texture policy (`hk_gltex.c`) is the same for both.
EGL is the core's too: its one display (made on the layer's service thread), its
configuration rules (no 10-bit configurations), contexts made without a
configuration where ANGLE allows it.

**The GLES classes** (`android.opengl.GLES10` to `GLES32`) declare 832 native
methods in the framework. They are mechanical and generated:
`tools/gles-natives.py` reads the framework's Java declarations and Khronos' C
prototypes (the NDK's headers) and lays one onto the other in order, as Android's
own generator does, into `core/src/drivers/hk_java_gles.c` (checked in). An array
and the offset after it are one pointer: the array's own memory from that element
on (the runtime never moves or copies an array). A `java.nio` buffer is one
pointer: what Android's `NIOAccess` gives -- a direct buffer's address plus its
position times its element size, or for a buffer over an array, that array's
memory from `arrayOffset + position`. An `int` or `long` where C has a pointer is
an offset into the bound buffer object (`glDrawElements(..., int offset)`). A
`String` where C has `const GLchar *` is its UTF-8. What returns a string or a
buffer, takes a `String[]` or a callback, or otherwise differs from C (39 of the
832) is written by hand in `core/src/drivers/hk_java_gl.c`.

**EGL from Java** is the framework's own Java -- `javax.microedition.khronos.egl`
(`MeglerEGL`, EGL10 and EGL11) and `android.opengl.EGL14` over it -- on the natives
of `megler.EGLNative`: a context (an OpenGL ES version, a context to share with),
a pbuffer surface, a window surface (a pbuffer the size of its view, bound to the
view), make current, swap, destroy. Every context and every surface is one
configuration, 8 bits a channel, 24 bits of depth and 8 of stencil; the Java side
offers the app several configurations (8888, 888, 565, each with and without
depth and stencil) so that a chooser looking for an exact match finds one, and
all of them are that one underneath. A version-1 context (`GL10`, the default of
`GLSurfaceView`) is asked of ANGLE and, where ANGLE has no OpenGL ES 1, is a
version-2 one, said in the log.

**`GLSurfaceView`.** `megler.Native.startGL(view, renderer, version)` starts a GL
thread of the layer's own for that view (C, in `hk_java_gl.c`), a thread of the
app's (Java attached, named `GLThread N`, and a guest thread, since the renderer
may call the app's native code). It makes its context, and then each turn asks the
view (`GLSurfaceView.meglerFrame`, the overlay's) what to do: the events queued with
`queueEvent` are run there, on the GL thread with the context current, and the
answer is one of: end, no surface, paused, draw, or nothing to draw. It draws --
`onSurfaceCreated` once for each context, `onSurfaceChanged` for each new surface
or size, `onDrawFrame`, then a swap -- when the render mode is
`RENDERMODE_CONTINUOUSLY`, or for `RENDERMODE_WHEN_DIRTY` when `requestRender`
was called; otherwise it waits until something wakes it (`requestRender`,
`queueEvent`, the surface made, changed or destroyed, `onPause`, `onResume`,
the view leaving its window). As Android's `GLThread` does: on `onPause` the
surface is released and, unless `setPreserveEGLContextOnPause(true)`, the
context too, so `onSurfaceCreated` comes again after `onResume`; a new surface
is always followed by `onSurfaceChanged`; when the view leaves its window the
thread ends and its context goes; a renderer that throws ends its thread as an
exception nothing catches ends any Java thread here (logged as `FATAL
EXCEPTION`). EGL called by the renderer on that thread answers for the
thread's own context and surface, which are real objects that other threads
can share (`eglGetCurrentContext` as the share of a context made elsewhere).
The configuration `onSurfaceCreated` is given is what the app's
`EGLConfigChooser` chose, or one of the sizes it asked for.

**Composition, as built (the first way; `hk_java_gl.c`).** A window surface is a pbuffer on
ANGLE's device. At each swap its frame is read back into memory (`glReadPixels`
on the default framebuffer, whatever the app had bound; as B, G, R, A where
ANGLE offers `GL_EXT_read_format_bgra`, so that rows are copied as they are).
The core then composes the window: the views' last picture over the frames of
the surfaces where the views left a hole, premultiplied "over" (a dimmed dialog
over a GL view dims it), a surface of another size than its view scaled into
the view's rectangle. It hands the result to the shell through `present_pixels`
-- or `present_pixels_part` with the rectangle of the GL view, for a new GL
frame under unchanged views. The shell is unchanged. To compose again when only
one of the two changed, the core keeps a copy of the views' picture and of the
composed one, from the moment the app first makes a window surface (an app
without one pays nothing). When the views' picture changes, each row of a GL
view's rectangle is classified once: wholly hole (the frame's row is copied),
wholly covered (left alone), or mixed (composed pixel by pixel). Where a
surface is on the screen comes from the `SurfaceView` itself, which says so
(`EGLNative.place`) each time it is drawn with its hole: the hole and the frame
come from the same picture of the views. A surface's first frames before the
views have shown the hole are not shown, as on Android.

`present_pixels` is now called on the app's main thread (the views) and on the
threads that swap (GL frames), one call at a time under the compositor's lock.

**Pacing.** A swap of a window surface returns no sooner than a sixtieth of a
second after the one before was due, as a swap interval of 1 would have it, so
a renderer in `RENDERMODE_CONTINUOUSLY` draws about sixty frames a second and
not as many as it can. The shell's own `Present(1)` paces the screen as well.
A swap of a pbuffer does nothing and returns at once, as on Android.

**What a swap costs.** The log says it every five seconds while GL frames
are shown (`java: OpenGL frames ...`: reading back, composing, the shell's
part, the longest). Measured so far only without ANGLE, on the stand-in that
draws, whose read-back is a copy of memory: at 720 x 1280 with a 720 x 1182
surface under the views (AIDA64's main screen), 1.45 ms of a swap's own work
on this PC -- 1.27 reading back, 0.12 composing (the hole's rows are copied
as they come), 0.06 the shell's copy of the rectangle -- and 0.9 ms to
compose a new picture of the views over the last frame; under qemu (an
emulated ARM core) AIDA64's 720 x 1182 surface, 8.1 ms reading back and 0.4
composing. On the phone the read-back is ANGLE's: a copy of the surface to
a staging texture and a wait for the GPU to finish the frame, on top of
copying 3.4 MB out of it. That is the number to read in the phone's log
before deciding on the faster way.

**The faster way (not built).** Reading back stalls the CPU until the GPU has
finished the frame, and composing is the CPU's work. Both go if the GPU
composes, on one Direct3D device:

- The shell would use ANGLE's device for a Java app as it does for a game. Today
  a Java app's pictures are put on a device of the shell's own, made at the
  first picture (`WindowHost::makeOwnDevice`); a game's frames come on ANGLE's,
  through `attach_device` and `acquire_target`. At feature level 9_3, where most
  of these phones stop, two devices cannot share a texture, so it has to be
  ANGLE's. The core would make ANGLE's display when the app starts (or at its
  first EGL surface), call `attach_device`, and take a target texture with
  `acquire_target`, as for a game.
- The core would compose with OpenGL ES itself, in a context of its own on a
  window surface over that target (exactly a game's window surface): each
  frame, the app's surface as a texture (its pbuffer bound with
  `eglBindTexImage`, or a texture shared through a share group) drawn as a quad
  at its view's rectangle, then the views' picture as a texture drawn over it
  with premultiplied blending, then `present` -- what the original Megler's
  render thread does on iOS. The views' picture is uploaded only where it
  changed (`glTexSubImage2D` of the changed rectangle), which the shell does by
  `UpdateSubresource` today.
- Two threads use the GPU then (the app's GL thread renders, the compositor
  samples its result): a fence (`glFenceSync`, or `EGL_KHR_fence_sync`) or a
  `glFinish` on the app's thread before the frame is handed over, and two
  surfaces a view in turn so that the app draws the next frame while the last
  is composed.
- What it saves per frame: the read-back (a GPU flush and a copy of the
  surface into memory the CPU can read), the swizzle and composition on the
  CPU, and the shell's flip of the composed rectangle and its upload. What it
  costs: the views' picture must be composed every GL frame on the GPU (cheap),
  a fourth way for the window host to be driven (a Java app on ANGLE's
  device), and the order of who makes the device (today ANGLE is never loaded
  for a Java app that draws no OpenGL; it would be, at its first surface).
- The core's side would be in `hk_java_gl.c` and `hk_egl.c` (a window surface
  of the core's own on the shell's texture already exists for games); the
  shell's side is a few lines (`attach_device` arriving for a Java app too,
  and `present_pixels` no longer used while it is).

## Sound

An app's PCM goes through OpenSL ES buffer queues; `ndk/hk_opensles.c` is the object
model and `ndk/hk_audio.c` a mixer. Each player is a voice: a queue of the app's own
buffers in the app's own format. One thread mixes the playing voices to 16-bit
stereo at 44.1 kHz, ten milliseconds at a time, calls each player's "buffer done"
callback (where apps queue the next buffer, as from Android's audio thread), and
hands the result to the shell's output, which paces it. With no output the queues
are still consumed in real time, so an app that paces itself by its audio callbacks
keeps running.

*Proven:* a guest playing three buffers at 22050 Hz mono through the callback chain
arrives as the right number of frames at the right level on both channels.

### Films

The core decodes nothing. An app that plays a film asks Android for an extractor
(the film's tracks and samples) and a decoder for each track; `ndk/hk_media.c` is
both, as `libmediandk.so`, and passes the work through. The extractor copies the
film (a range of the APK, or whatever a data source reads) to a file under the data
folder and has the shell open it: `megler_media_host` in `megler.h` is open, the next
picture as RGBA, the next block of sound as 16-bit PCM, seek, close. A sample the
extractor hands out is a ticket naming the film and the track; a decoder given
tickets asks the shell for as many pictures or blocks, in order. It is full after
a handful that nobody has taken the output of, as a real decoder is, because an
engine paces itself by that. A picture released "to the surface" goes to the
`SurfaceTexture` the surface was made from: the listener is told on its handler's
thread, and `updateTexImage`, on the thread with the app's context, uploads it and
binds it to the app's external texture through an EGLImage. The shell's side is
`player/MediaHost.cpp`: Media Foundation's source reader, two readers to a film
(one stream each), on a thread of its own.

## Files and the shared storage

Android's paths are looked up in a mount table (`bionic/hk_libc_io.c`). What an
app sees, and where it is under the data folder (`LocalState` on a phone):

| Android | Here |
|---|---|
| `/data/data/<package>`, `/data/user/0/<package>` | `apps/<package>/data` |
| `/storage/emulated/0`, and its other names `/sdcard`, `/mnt/sdcard`, `/storage/self/primary`, `/storage/sdcard0` | `sdcard`: **one storage for every app**, with Android's standard folders in it from the start (`Alarms`, `DCIM`, `Documents`, `Download`, `Movies`, `Music`, `Notifications`, `Pictures`, `Podcasts`, `Ringtones`) |
| `/storage/emulated/0/Android/data/<package>`, `.../Android/obb/<package>` | `apps/<package>/sdcard/Android/data/<package>`, `.../obb/<package>`: the app's own part stays with the app. Another package's is not there |
| `/storage/emulated/0/<name>` for a link | a folder of the phone's own, through the shell |

The other names of the storage are rewritten to the one before a path is looked
up, as Android's symbolic links would be followed, so `realpath` and `getcwd`
give `/storage/emulated/0/...`. A directory that exists only because something
is mounted below it (`/storage`, `/storage/emulated`, `/mnt`) can be stat'ed,
opened and listed, and a listing shows what is mounted in a directory whether
or not the host's folder has an entry of that name, once. A mount's own name
cannot be removed or renamed (`EBUSY`) and making it finds it there (`EEXIST`).
Below the shared storage a mount's name is matched without regard to the case
of ASCII letters, as Android's storage does not tell `Music` from `music`.
`statfs` and `statvfs` answer for every path with the real size and free room
of the volume the data folder is on, in 4 KB blocks.

**Links.** The user's music, pictures and videos are not the app's to open: on
the phone tried, `CreateFile2` and `FindFirstFileExW` are refused there (error
5) with every library capability declared. What Windows gives an app is
handles, through Windows.Storage's broker. So the host layer has *foreign
paths* (`host/hk_host.h`): `megler-link:/2/Camera Roll/a.jpg` is a file of root
2, and whoever owns the roots is asked for the operating system's own handle
for it -- a file's, a directory's to list, or whichever is there for its size
and times -- and to make, remove and rename by name. Every function of the
host layer that takes a path takes these, and from the handle on a file in a
link is read, written, mapped and listed by the same code as any other. A
link is a mount whose host path is such a root. The owner of the roots is the
shell, through `megler_storage_host` in `megler.h`: its links by name, each
writable or not, and four functions. A link that is not writable is refused
every change in `run/hk_run.c`, before the shell is asked, whichever part of
the layer wanted it. A rename between a link and anything else is `EXDEV`, as
between two file systems: the app copies and removes.

The shell's side is `player/StorageHost.cpp`. Its links are the three
libraries' folders to save to as `Music`, `Pictures` and `Movies`
(`StorageLibrary`), the pictures' `Camera Roll` as `DCIM`, and any folder the
user added in Settings with the folder picker, which Windows remembers for the
app (`FutureAccessList`, the name kept as its metadata): the documents
library, the downloads and a memory card come in that way and no other. A
link that cannot be listed when Megler starts is left out, and the log has a
line for each. A handle comes from the folder's object:
`IStorageFolderHandleAccess::Create` for a file by its name,
`IStorageItemHandleAccess::Create` for a folder itself. A path is walked a
folder at a time (`TryGetItemAsync`) and the folders found are kept for half a
minute, so the next file of a folder costs one call. A file or an empty folder
is removed by marking it for deletion through a handle, where the file system
itself refuses a folder that holds something (the broker's `DeleteAsync` would
take the contents with it; it is the fallback, for a folder seen empty). A
rename in a folder is `RenameAsync`, a file to another folder `MoveAsync`; a
folder to another folder is tried through its handle and is `EXDEV` where
Windows will not. The app's threads have stacks of the app's choosing and no
COM apartment, so every call is done on one of up to four threads of the
host's own, in the multithreaded apartment, while the caller waits.

*Proven:* with a stand-in for the shell that opens folders of the tests'
scratch folder with the plain calls (`core/tests/hk_storage_tests.c`), on both
hosts, through the C library's functions as an app binds them: the five names
of the storage, the standard folders and each link once in a listing, files
and folders in a link, renames in a link, between links and refused across,
the app's own part where it was, a link that is only to be read, two threads
at once. And the same tests with the shell's own `StorageHost.cpp` in the
stand-in's place, on a PC (`hk_tests --storage-windows`): Windows.Storage's
real objects, but a test program is no app container and no broker stands in
between. *Expected:* that the broker hands an app on a phone what a PC's
Windows.Storage hands a program there. A phone was seen to give a file handle
that reads, writes, seeks, says its size and deletes, and a folder handle that
lists; everything else -- a handle for the attributes alone, a folder deleted
or renamed through its handle, what a folder's name asked for as a file is
answered with, a picked folder kept across starts -- has not been asked of a
phone. What asks is `megler_storage_check`: in every link the layer does,
through its own C library, what an app does to files, in a folder of its own
(`megler-storage-check`) that it removes again, and says each step and which
way Windows answered. **Settings › Check the Folders** runs it, and so does a
package built with `tools/build-appx.sh arm --auto storage`, into
`megler-auto.log`.

## The network

An app's sockets are the phone's own: Winsock 2, which an app container may
use (the package declares `internetClient`, `internetClientServer` and
`privateNetworkClientServer`, which together are what Android's INTERNET
permission allows). Three layers:

- **The host** (`host/hk_host.h`, "sockets"; Winsock in `hk_host_win32.c`,
  BSD sockets in `hk_host_posix.c`) has sockets, the system's resolver and the
  list of interfaces in neutral terms of its own: its own families, options,
  flags and errors, an address structure of its own. A host socket never
  blocks.
- **bionic** (`bionic/hk_net.c`: sockets; `bionic/hk_netdb.c`: names,
  addresses, interfaces) is what an app links against. It turns Linux's ABI
  into the host's and back: the numbers (`AF_INET6` is 10 on Linux and 23 on
  Windows, `SOL_SOCKET` 1 and 0xFFFF, the `SO_*`, `IPPROTO_*`, `TCP_*` and
  `MSG_*` options and flags), the structures (`sockaddr_in6`, `msghdr`,
  `linger`, a `timeval` where Windows takes milliseconds, `ip_mreqn`,
  `group_req`, `ifreq`), and the errors as Linux's errno. `struct addrinfo`
  is bionic's, whose order is BSD's -- `ai_canonname` before `ai_addr`, the
  same as Windows', the other way round from glibc's. An option with no
  meaning on the host succeeds and is logged once.
- **The descriptor table** (`bionic/hk_libc_io.c`): a socket is a descriptor
  like a file, so `read`, `write`, `close`, `dup`, `fcntl` (`O_NONBLOCK`,
  `FD_CLOEXEC`), `ioctl` (`FIONREAD`, `FIONBIO`, the `SIOCGIF*` questions)
  and `fstat` (`S_IFSOCK`) work on it.

Windows is made to answer as Linux does where Android code depends on it.
libcore makes every socket an IPv6 one and reaches IPv4 addresses through it
as `::ffff:a.b.c.d`, which works because the host turns `IPV6_V6ONLY` off
(Linux's default, not Windows'). A UDP socket is not told of an earlier
datagram's ICMP refusal, as Windows otherwise would on the next receive. A
refused connection to the machine's own address is refused at once (Windows
retransmits the opening segment twice first, a second or more). Linux's
`SO_REUSEADDR` is kept and not given its Windows meaning (which lets a second
socket take a port another listens on). A failed connection's error is said
once. A datagram larger than the buffer is cut short and said so with
`MSG_TRUNC`, where Windows calls it an error.

**Names.** `getaddrinfo` and `android_getaddrinfofornet` (libcore's
`InetAddress` goes through the latter) ask the host's resolver (`GetAddrInfoW`)
for names. Numbers, and `localhost` -- 127.0.0.1, as Android's hosts file has
it -- need no network. With no type asked for, every address comes once as a
datagram entry and once as a stream entry, as bionic gives them.
`getnameinfo`, the `gethostby*` calls, `inet_pton`/`inet_ntop` (IPv6 written
as bionic writes it), services and protocols by name are there too.

**Interfaces** are the host's (`GetAdaptersAddresses`) under the names an
Android phone gives them: `lo`, `wlan0`, `eth0`, `rmnet0`, each with a link
entry (`AF_PACKET`) and its addresses, as Linux's `getifaddrs` lists them;
`java.net.NetworkInterface` reads them through `getifaddrs`,
`if_nametoindex` and `SIOCGIFFLAGS`. Interfaces that are down and the host's
own tunnels are left out.

**The app's own sockets.** `AF_UNIX` is implemented in the process, in
`hk_net.c`: `socketpair` of stream, datagram and seqpacket sockets, and names
(paths or abstract) bound, listened on and connected to. Nothing outside the
app could be reached on a phone anyway. Libraries make such a pair as a
wake-up channel, and libcore makes one to stand in for a socket it closes
under a blocked thread.

### Waiting on sockets with everything else

poll, select and epoll are given a mix: a socket and the pipe an engine's
loop is woken by, a socket and an eventfd, sockets and files. On Windows
`WSAPoll` takes sockets only -- and on the Windows these phones have, before
Windows 10 2004, it never reports a connection attempt that failed -- and
nothing of Windows' waits on the layer's pipes and eventfds, which are not
handles. So the wait is not the host's at all. Every descriptor of the layer
already waited one way: under the descriptor table's lock, on the one
condition that every change of a pipe or an eventfd is announced on. Sockets
were made to fit that:

- Each host socket has an event that Winsock sets on each of its network
  events (`WSAEventSelect`), and a thread-pool wait watches the event. When it
  fires, the events are taken (`WSAEnumNetworkEvents`), what they say that
  cannot be asked later is kept (how a connection attempt ended, that the
  other side closed), and the table's condition is announced, as when a pipe
  is written to. On Linux one thread waits on all host sockets at once,
  edge-triggered (`epoll` with `EPOLLET`), and announces likewise.
- A waiter looks at what holds under the lock -- for a host socket, `select()`
  with no wait on Windows, `poll()` on Linux -- and then waits on the
  condition. The announcement takes the same lock, so a change that comes
  after the look wakes the waiter and none is missed, whichever kind of
  descriptor changed.
- Winsock's events are edges, and room to send (`FD_WRITE`) is announced only
  after a send that found none. So a send that took part of what it was given
  is asked again with the rest until Winsock has taken all of it or refused
  some: then the edge is armed.

This costs one more thread hop per change than a native wait, wakes every
waiter of the process on every change (as a pipe's write always did), and
needs no thread of the layer's own on Windows. Blocking calls -- `accept`,
`connect`, `recv`, `send` on a socket without `O_NONBLOCK` -- wait the same
way, so `SO_RCVTIMEO` and `SO_SNDTIMEO` are the limits of that wait, and a
signal sent to the thread ends it. That last matters for Java: libcore
closes a socket that another thread is blocked on by putting a dead pair in
its place (`dup2`) and sending that thread signal 34
(`AsynchronousCloseMonitor`), whose handler does not ask for `SA_RESTART`; the
blocked call must end with `EINTR`, and `ServerSocket.close()` then ends
`accept()` with "Socket closed". Signals above 32 are kept in a word of their
own for that. poll, select and epoll end with `EINTR` when any handler ran,
as Linux's do; the looper's own waits go on.

*Proven:* on this PC (the Win32 host) and under qemu (Linux), by
`core/tests/hk_net_tests.c` over loopback, through the C library as an app
calls it -- TCP over IPv4 and over IPv6 reached as `::ffff:127.0.0.1`,
blocking and not, refusal, accept with a timeout, 6 MB with a blocked and with
a polling writer, shutdown and the end of a stream, UDP, options, AF_UNIX,
poll, select and epoll each woken by a socket, a pipe and an eventfd alone,
`getaddrinfo`'s results word by word as guest code reads them, and (under
qemu) signal 34 ending a blocked accept and a blocked read; and from Java
under qemu by the test app `data` (`InetAddress`, `HttpURLConnection` against
a `ServerSocket` of the app's own, `DatagramSocket`, a `ServerSocket` closed
under its accepting thread, a `java.nio` selector woken from another thread
and then serving two channels without blocking, `NetworkInterface`). The check a phone runs
(`megler_net_check`) reaches the internet from both. *Expected:* that a phone's
Winsock in an app container does what the PC's does, loopback within the
process included (Microsoft's documents allow it within a process).
`tools/build-appx.sh arm --auto net` asks. No TLS: an https connection fails
with an `SSLException`, as it did before there were sockets.

*Seen on the phone* (the Self-test of 0.1.0.138): a connection to a closed
port on 127.0.0.1 is not refused. The SYN goes unanswered: a non-blocking
connect is still `EINPROGRESS` after 5 s, and a blocking one ended with
`ECONNREFUSED` only after about two minutes (127 s). The PC and Linux refuse
it at once. This is the phone's app-container loopback, not the layer's. The
test now tries without blocking first. On the phone it counts "no answer
within 2 s" as the phone's behaviour, says so, and leaves out the blocking
attempt. An app that probes a local port that is not open waits that long
too.

## The Java side

No Java runs. `jni/hk_jni.c` is a `JNIEnv` over classes, methods and fields that are
looked up by name; a method is either implemented in C by a driver or logs once and
returns zero. `jni/hk_jni_android.c` has what every app asks of an activity: the
package name, the data directories, the display, `Build`.

A **driver** (`drivers/`) plays the part of one kind of app's Java shell. There is
one so far: `NativeActivity`, which is what Android itself provides for apps with no
Java (`android:hasCode="false"`, or `android.app.lib_name` in the manifest). It
loads the library, calls `ANativeActivity_onCreate`, and makes the activity's
callbacks in the framework's order on a thread that stands for Android's main
thread. Apps built on `android_native_app_glue` run on top of that.

Engines whose Java shell does more (SDL, cocos2d-x, Unity, Unreal, Godot) each need
their own driver, as they have in Megler on iOS. Apps written in Java need a Java
runtime: `core/src/dvm`, a Dalvik interpreter. Until it is started nothing above
changes -- the classes are C, and an app with a native engine pays nothing for
it. Once started there is one Java world: every class the JNI layer knows has a
runtime class (one a DEX file defines, or a stand-in for a class that exists
only in C), every object is the runtime's, and there is one pending exception.
It starts either on a small java.lang of its own (the tests') or on Android 9's
libcore, whose native libraries are guest libraries like an app's, loaded by the
same linker. [JAVA.md](JAVA.md) is its design, what is proven, and the plan.

*Proven:* `native-test.apk`, a NativeActivity app written like the glue, goes
through `megler_run` under qemu: manifest, library from the APK, JNI questions,
asset, EGL, frames, touches with two fingers, focus and pause, Back, teardown.

## What the device probe is for

Several things above cannot be known from a PC. **Device checks** in the app runs
`megler_probe` and writes one line per fact:

| Line | What it decides |
|---|---|
| `can-run-guest-code`, `exec-memory` | whether `codeGeneration` is granted, and what a protection flip costs |
| `tpidruro`, `tpidrurw`, `guest-thread-pointer` | whether thread-pointer reads can be redirected |
| `arm-one-instruction`, `plt-rescue` | whether rewritten PLT entries work, and what a rescue costs |
| `arm-state` | whether ARM-mode libraries can run as they are |
| `angle`, `d3d-feature-level`, `gles*` | which OpenGL ES the phone gives, and which texture formats |

**Self-test** runs the core's tests on the phone itself, with the same guest
libraries the qemu run uses.
