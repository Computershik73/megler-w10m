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
| file descriptors | pipes, eventfd, epoll | this layer's own table; pipes and eventfds are implemented here |
| paths | `/data/data/<pkg>`, `/sdcard`, `/proc/cpuinfo` | a mount table onto the app's folder under `LocalState`; the `/proc` and `/sys` files code reads are made up |
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
