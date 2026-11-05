# Megler on desktop Windows: x64, ARM64 and x86

Megler was made for Windows 10 Mobile, a 32-bit ARM system, where the phone's
processor runs an app's `armeabi-v7a` code as it is. Desktop Windows 10 and 11
run on three processors, and Megler runs on each the Android code of that
processor: `x86_64` on x64, `arm64-v8a` on ARM64, `x86` on x86. Nothing is
emulated or translated; an app with no code for the processor cannot run
(its Java, which the Dalvik interpreter runs, can).

This document says how, and what of it is **proven**. The phone's build is
not touched by any of it: its core is still MSVC's, for 32-bit ARM, as
[ARCHITECTURE.md](ARCHITECTURE.md) describes.

## The core as an ELF

On the phone, guest code and the core share one convention but for how
floats are passed (Android's softfp), and the core bridges that by hand
(ARCHITECTURE.md, "Two ABIs in one process"). On the desktop the two
conventions are further apart:

| | Android's ABI | Windows' |
|---|---|---|
| x86_64 | System V: arguments in rdi, rsi, rdx, rcx, r8, r9 and xmm0-7; rsi, rdi, xmm6-15 the callee's to clobber | arguments in rcx, rdx, r8, r9 (or xmm0-3) by position, 32 bytes of shadow space; rsi, rdi, xmm6-15 kept |
| ARM64 | AAPCS64; a variadic double in v0-v7, `va_list` a structure | AAPCS64; a variadic double in the integer registers, `va_list` a pointer; x18 the TEB |
| x86 | cdecl, the stack 16-byte aligned at a call | cdecl, 4-byte aligned |
| everywhere | `long` 64 bits on the 64-bit ABIs, `wchar_t` 32 bits | `long` 32 bits, `wchar_t` 16 |

Every function an app's code calls -- all of bionic, the NDK, JNI, EGL, the
GLES wrappers, some 1500 of them -- and every callback the core makes into
an app would need a bridge of its own, and `long`, `va_list` and `wchar_t`
would differ in every structure and signature the core shares with apps.

So on the desktop the core is not compiled for Windows at all. It is compiled
by clang for **Android's ABI of that processor**
(`--target=x86_64-linux-android29`, `aarch64-linux-android29`,
`i686-linux-android29`) against the NDK's bionic headers, into an ELF shared
library, `megler-core.so`, and Megler.exe loads it. Its C then is of the
guest's ABI throughout, and an app's code calls the core as it calls bionic:
the same registers, the same `long`, the same `va_list`. What crosses to
Windows is named in three places:

- **the host layer** (`host/mg_host.h`): the core's one door to the operating
  system already, 160 functions and 15 kinds of callback. Each declaration is
  `MG_HOSTAPI`, each callback type `MG_HOSTCB` (`mg_base.h`): Windows'
  convention on x86_64 (`ms_abi`), a realigned stack on x86, nothing on ARM64
  (whose conventions are one for calls that are not variadic) and nothing at
  all in the phone's build;
- **Megler's API** (`include/megler.h`), which the shell calls: `MEGLER_API`
  on each of its 55 functions, `MEGLER_CB` on each of the 107 callbacks of the
  shell's that the core calls (drawing, media, cameras, storage...);
- **the C library** the core itself calls (malloc, the string functions, libm,
  `FILE`), about 150 functions: Windows' own (the UCRT), each wrapped to be
  called with Android's convention (`host/win32/mg_crt_bridge.c`).

A core function given to Windows where that convention is wanted -- a
thread's entry, a fault handler, a directory's walker, a stand-in of the
shell's in the tests -- is a type error for clang on x86_64
(`-Werror=incompatible-function-pointer-types`): 212 of them were found that
way and marked. And `tools/desktop-stubs.py` refuses a core whose imports
from Megler.exe include a function no header declares `MG_HOSTAPI`, which
would be called with the wrong convention without a word from the compiler
(the per-app packages' `mg_appx_*`, the first time).

### The pieces

| | |
|---|---|
| `tools/build-desktop.sh x64\|arm64\|x86 tests` | builds the core's ELF and Megler.exe's half, and runs the tests where the processor is this machine's, with the guest test libraries of `build/guest/<abi>` (`tools/build-guest.sh <abi>`) |
| `tools/build-desktop.sh <arch> app`, `tools/build-appx.sh x64\|arm64\|x86` | the package: Megler.exe of that processor, `megler-core.so` beside it, the Android root of the ABI (`tools/dvm-android-root.sh --abi`) |
| `host/win32/mg_core_loader.c` | loads `megler-core.so`: its segments, its relocations (`RELATIVE`, `GLOB_DAT`, `JUMP_SLOT`, the absolute one), its imports bound by name to Megler.exe's functions, its initialisers. It refuses what the core is built not to have (packed relocations, a TLS segment, a processor not its own) |
| `host/win32/mg_crt_bridge.c` | the C library under Android's names and convention; where the two differ beyond the convention (`long` in `strtol`, `lround`, `scalbln`, `time` and `struct tm`; 32-bit `wchar_t` in `wcslen`; a comparator of the core's for `qsort`), the guest's way |
| `crt/mg_crt_format.c` | printf, scanf and their kin, the core's own: their arguments are Android's, which Windows' functions cannot read. A floating-point conversion alone goes to Windows' `snprintf`, one value at a time |
| `crt/mg_crt_setjmp.c` | setjmp for the collector, which spills its registers before scanning its own stack; `__emutls_get_address` |
| `host/win32/mg_host_base.c` | what the host layer calls of `mg_base.c` (`mg_alloc`, `mg_log`), on Megler.exe's side; its log lines go to the core's log once the core is loaded |
| `tools/desktop-stubs.py` | from the ELF's own symbols: a stub in Megler.exe for each function the core exports, jumping through a slot the loader fills, and the table of Megler.exe's functions the core imports |
| `tools/abi-check.py` | what the core gives guest code -- every binding of bionic's and the NDK's names, 54 structures -- against the NDK's headers, for each of the four ABIs (0 differences) |
| `tools/build-angle.py --arch x64\|x86\|arm64` | ANGLE for the processor, from gecko-w10m's sources and object directory (below, "OpenGL ES") |
| `tools/gl-adapters.py` | `ndk/mg_gl_adapters.inc` from the NDK's EGL and GLES headers: an adapter from the guest's convention to ANGLE's for each entry point |

## Guest code of the processor's own

The guest linker (`ld/mg_ld.c`) reads ELF64 and RELA as well as ELF32 and
REL, packed relocations of both kinds (APS2, RELR), and each processor's
relocation types (`elf/mg_elf.h`). An app's libraries are those of the
processor's ABI alone, `MG_GUEST_ABI` (`MG_APK_ABI_ARM` keeps 32-bit ARM's on
the phone): the loader, the APK's analysis and a bundle's splits choose them,
and Build, the system properties and `uname` say it (`megler/Abis.java`).

What compiled Android code does that cannot run as it is here is rewritten
before the code is made executable:

- **x86_64 and x86** (`x86/mg_patch_x86.c`, with an instruction length
  decoder, `x86/mg_x86_decode.c`): bionic's thread pointer and stack guard
  are read straight from the segment, `%fs:0` and `%fs:0x28` (`%gs:0`,
  `%gs:0x14` on x86). Each such instruction is given Windows' segment and a
  slot of the TEB's TlsSlots that holds the same value on every guest thread
  -- `mov %gs:0x1480+8*slot, %rax` -- the same length, in place. The two slots
  are taken as the process starts (`.CRT$XIU`): later, XAML's DLLs have used
  the TEB's 64. A read at another place of the segment is left, and said:
  all of Android 9's libcrypto's are data among its code.
- **ARM64** (`arm64/mg_patch_arm64.c`): `mrs xN, tpidr_el0` becomes
  `ldr xN, [x18, #0x1480+8*slot]` (x18 is the TEB, and Android's compiler
  leaves it alone), the canary following through the guest's own block;
  `svc #0` becomes a UDF the fault handler does the call for.
- **System calls by number**: `syscall()` and ARM64's traps take the ABI's
  own numbers, made 32-bit ARM's (`bionic/mg_syscall_abi.c`, its table made
  by `tools/syscall-numbers.py` from the NDK's headers), with the arguments
  ARM's passes otherwise (a 64-bit offset in one register, mmap's in bytes).

A native method is called with the processor's convention (`jni/mg_jni.c`,
System V's six and eight registers, AAPCS64's eight and eight, x86's stack);
a fault's and a stopped thread's registers are each processor's; a guest
thread is interrupted where it runs (what a signal does) by its context, the
x64 one below the 128 bytes System V code may keep under its stack pointer.

### Where the two sides meet on x86

Android's x86 ABI aligns a 64-bit field at 4 bytes, MSVC's x86 at 8:
`mg_host.h`'s and `megler.h`'s structures are packed to 4 on both sides. And
the stack is 16-aligned for Android's code, 4 for Windows': what Windows calls
of the core realigns it (`MG_HOSTCB`, `MEGLER_API`).

What the core needs of the processor's ABI that Windows does not give is
taken care of at compile time:

- **Thread-local variables** are emulated (`-femulated-tls`): no ELF TLS, whose
  thread pointer (`%fs`, `TPIDR_EL0`) Windows does not keep for code of
  Android's. The bridge keeps them in fiber-local storage, freed when a thread
  ends.
- **Stack probes.** Windows hands out a thread's stack a page at a time,
  through a guard page; code built for Android moves the stack pointer by any
  amount (ARCHITECTURE.md, "a thread's stack"). The core's ELF is compiled with
  `-fstack-clash-protection`, which touches each page of a large frame in
  turn, as Windows' own code does.
- **No stack protector**: its canary is read through `%fs` (or `%gs` on x86).
- **x18** on ARM64 holds the TEB: the core is compiled `-ffixed-x18`.

The phone's build, MSVC's x64 build of the tests and the Linux builds see
none of this: the macros are empty there, `MG_CORE_ELF` and `MG_ELF_HOST` are
not defined, and the new files compile to nothing.

## OpenGL ES

**ANGLE** is the phone's, gecko-w10m's (with Megler's patches, `angle/`),
built for the processor from the sources and the object directory of
gecko-w10m on this PC: `python tools/build-angle.py --arch x64|x86|arm64`
compiles every object its build linked `libGLESv2.dll` and `libEGL.dll` from,
with the flags it used, for that processor and without Gecko -- no
`mozilla-config.h`, no mozglue allocator, the C runtime linked in, Megler's
zlib -- into `build/angle/<arch>/dist`. Still for an app container
(`ANGLE_ENABLE_WINDOWS_UWP`). The two DLLs import the system's alone
(`d3d11`, `dxgi`, `d3dcompiler_47`); `tools/build-appx.sh` stages them beside
Megler.exe, and the shell looks for those two (`shell/Platform.cpp`).

**Its functions are of Windows' convention**: the Khronos headers'
`KHRONOS_APIENTRY` is `__stdcall` on Windows, so on x86 an ANGLE function takes
its arguments off the stack itself, where Android's cdecl leaves them to the
caller; on x86_64 it is Microsoft's x64 convention. Guest code and the core
call with Android's. So on those two every function of ANGLE's is given as an
**adapter** (`ndk/mg_gl_adapters.c`): a function of the guest's convention
that calls ANGLE's, typed `MG_GLAPI`. `tools/gl-adapters.py` writes one for
each of the 1177 entry points of the NDK's headers (`mg_gl_adapters.inc`);
`mg_egl.c`'s `angle()`, where every ANGLE function is found -- for guest
imports and `dlsym`, `eglGetProcAddress`, the core's own calls and the Java
natives -- gives the adapter, so nothing gets ANGLE's own. The seven functions
that take a function of the caller's (debug callbacks, `EGL_ANDROID_blob_cache`)
are written by hand: ANGLE is given a function of the adapters' own, of
Windows' convention (`MG_GLCB`, `MG_HOSTCB`), which calls the caller's.
`eglGetProcAddress` is the core's; a name in no header has no adapter and is
"no such function". ARM64's conventions are one for calls that are not
variadic, and none of these is: ANGLE's functions are given as they are. The
float adapters of 32-bit ARM's softfp are that processor's alone.

The tests' stand-ins for ANGLE are of its convention too (`MG_GLAPI`), so the
adapters are what the GL tests go through on x86_64 and x86; on x86 the
stand-in's functions that do nothing come in one size of arguments each, as
`__stdcall` functions must.

## Proven

On this PC (x64, Windows 11 26300), 2026-11-05:

- `tools/build-desktop.sh x64 tests`, the core in `megler-core.so` loaded into
  a Windows x64 process, with the x86_64 guest test libraries: 5313 checks, 2
  failed -- t_vdso (no vDSO image off 32-bit ARM yet), in both passes of
  t_basic. The GL tests go through the adapters to stand-ins of ANGLE's
  convention, floats and all. ELF64 with RELA, APS2 and RELR
  relocations, IFUNC, 90 patched thread-pointer and canary reads in one
  library, initial-exec and global-dynamic thread-local variables, PLT calls,
  JNI with every kind of argument, threads, a thread interrupted twenty times
  where it was, setjmp, system calls by number, an app from its APK through
  NativeActivity.
- `tools/build-desktop.sh x86 tests` (through WOW64): 5322 checks, 2 failed
  (t_vdso, in both passes; a timing check of a few milliseconds fails at
  times), the GL tests through `__stdcall` stand-ins.
- `tools/build-tests.sh win` (MSVC's x64 build of the tests): 5023 checks, 0
  failed; `tools/abi-check.py`: 0 differences on all four ABIs; the phone's
  core compiles for ARM32 with MSVC, 149 of 149, and its tests under qemu-arm
  (`tools/build-tests.sh arm`) give 5429 checks, 0 failed, as before.
- The x64 and x86 packages (`tools/build-appx.sh x64 --java --auto java`),
  registered from their stage folder in developer mode and started: the core
  loads, "megler core on win32-x64"; Android 9's libcore runs its natives
  through the runtime's tests (the x86 package's Self-test: 2437 checks, 3
  failed, none of them the port's); and the Java test app hello.apk starts,
  draws (its first picture after 3.8 s on x64, 0.8 s on x86, through Direct2D
  and Direct3D 11.1), counts presses of its button and goes through onPause,
  onStop and onDestroy on Back, status 0.
- OpenGL ES, in the x64 and x86 packages with ANGLE in them
  (`--java --auto probe,play` and the flag `app-gl`): ANGLE's display made
  through the adapters -- Direct3D 11, EGL 1.5, feature level 12_1, OpenGL ES
  3.0 on the PC's GeForce -- and the Java test app gl.apk drawing as on the
  phone: a GLSurfaceView's shader triangle with a view over it, a SurfaceView
  cleared with EGL10 and GL10 in a 565 configuration, an EGL14 pbuffer read
  back, 30 frames in about half a second, when-dirty rendering, a queued
  event, a fixed size; ANGLE's program cache on.
- ARM64 builds -- the core's ELF, the tests' program, ANGLE, the package -- and
  is not run: there is no ARM64 machine here.

In developer mode the first start after the stage folder was registered again
over an earlier build is refused the package's files ("core: not loaded: ...
access denied"); the next start is not.

## What is left

1. **OpenGL ES**: ARM64's ANGLE is built and not run. On x86_64 and x86 a
   function of ANGLE's that none of the NDK's headers declares (an extension
   of ANGLE's own) has no adapter and is "no such function", where the phone
   gives it.
2. **The vDSO** for the other ABIs: Go's runtime reads its clock through it.
3. **`syscall` in x86_64 code of an app's own** is counted, not rewritten: a
   trap's fault would put Windows' exception frame over the 128 bytes below
   rsp that the code may be using; it takes a jump to an island.
4. **WebView**: Android's System WebView for x86_64 and arm64 (the phone's is
   32-bit ARM's).
5. **ARM64 on a device**: the patched TPIDR_EL0 reads, x18 left alone by the
   guest's code (Android 9's prebuilt libcrypto among others), the context of
   a stopped or interrupted thread.
6. A ping socket (IcmpCreateFile) is refused in a desktop app container.
