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
| `tools/build-desktop.sh x64\|arm64\|x86 tests` | builds the core's ELF and Megler.exe's half, and runs the tests where the processor is this machine's |
| `host/win32/mg_core_loader.c` | loads `megler-core.so`: its segments, its relocations (`RELATIVE`, `GLOB_DAT`, `JUMP_SLOT`, the absolute one), its imports bound by name to Megler.exe's functions, its initialisers. It refuses what the core is built not to have (packed relocations, a TLS segment, a processor not its own) |
| `host/win32/mg_crt_bridge.c` | the C library under Android's names and convention; where the two differ beyond the convention (`long` in `strtol`, `lround`, `scalbln`, `time` and `struct tm`; 32-bit `wchar_t` in `wcslen`; a comparator of the core's for `qsort`), the guest's way |
| `crt/mg_crt_format.c` | printf, scanf and their kin, the core's own: their arguments are Android's, which Windows' functions cannot read. A floating-point conversion alone goes to Windows' `snprintf`, one value at a time |
| `crt/mg_crt_setjmp.c` | setjmp for the collector, which spills its registers before scanning its own stack; `__emutls_get_address` |
| `host/win32/mg_host_base.c` | what the host layer calls of `mg_base.c` (`mg_alloc`, `mg_log`), on Megler.exe's side; its log lines go to the core's log once the core is loaded |
| `tools/desktop-stubs.py` | from the ELF's own symbols: a stub in Megler.exe for each function the core exports, jumping through a slot the loader fills, and the table of Megler.exe's functions the core imports |

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

*Proven* (x64, this PC, 2026-11-05): `tools/build-desktop.sh x64 tests` runs
the core's tests in `megler-core.so` loaded into a Windows x64 process: 5005
checks, 0 failed -- the host layer, files and the shared storage, sockets and
poll/epoll over loopback, the Dalvik runtime on its own java.lang, the Java
side of drawing, OpenGL ES through its stand-in, the decoders' and cameras'
stand-ins, bundles and APKs. Guest code does not run yet in this build.

## What is left, in order

1. Guest code on x86_64: ELF64 and its relocations in `ld/mg_ld.c`; the
   instructions that cannot run here (`%fs`-relative loads of bionic's thread
   pointer and stack canary, `syscall`); the fault handler and a thread's
   interruption for x64's registers; a native method's call for System V.
2. bionic's structures for the 64-bit ABIs: the core's own definitions of
   them were written for 32-bit ARM (pointers as `u32`, `time_t` as `i32`), and
   are checked against the NDK's headers.
3. libcore's native libraries for each ABI (`tools/android9-natives.sh <abi>`).
4. The shell and its package for each processor, with ANGLE.
5. x86 and ARM64 the same way.
