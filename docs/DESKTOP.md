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
  the TEB's 64. A read at another absolute place of the segment is left, and
  said: all of Android 9's libcrypto's are data among its code.
- **An access of `%fs` through a register on x86_64**, the offset in a
  register: Go's runtime reaches its g so (`mov runtime.tls_g(%rip), %r14;
  mov %fs:(%r14), %r14` after every call to its assembly; its stores of g,
  `mov %rdi, %fs:(%rax)`), and C of the initial-exec model its variables
  (`mov x@gottpoff(%rip), %rax; mov %fs:(%rax), %eax`). The instruction and
  whole instructions next to it, as a system call's below, become a jump to
  a piece of the library's island that makes the access on the guest's
  thread pointer: a register the instruction does not use is kept in a third
  TEB slot (the patcher's word: not the stack, a goroutine's has nothing to
  spare), loaded with the thread pointer, the operand's base added by `lea`,
  the instruction made with that register as its base, the register given
  back -- no flag and no register it does not write changes. An interrupted
  thread finds the word as it was (`mg_thread_interrupt` keeps it across
  what it runs there). Only in a library that says it reaches the segment
  so -- `DF_STATIC_TLS`, or Go's build ID note -- since elsewhere such bytes
  have been data (libcrypto's ChaCha constant reads as `and %dh,
  %fs:(%rbx)`).
- **`syscall` in x86_64 code of an app's own** (Go's runtime makes every
  system call so, anti-tamper libraries some): a trap would put Windows'
  exception frame over the 128 bytes below rsp that System V code may keep
  there, so the instruction and whole instructions next to it become a jump
  to a piece of the library's island, which jumps on to the core's
  `mg_x86_syscall_entry` with the guest's thread pointer in r11 and the way
  back in rcx (the two registers `syscall` itself overwrites). The entry
  makes the call by the ABI's number with every other register, the flags
  and the XSAVE state kept, on the thread's own stack below its red zone, or,
  when the code runs on a stack of its own (a goroutine's), on a stack of the
  thread's for system calls (slot 15 of its guest block) that the TEB is told
  while it is used. An instruction taken with it that has an operand relative
  to rip is given the displacement that reaches the same place from the
  piece. A syscall with no such place around it (a jump into it, a jump or a
  return on both sides) is left and said.
- **ARM64** (`arm64/mg_patch_arm64.c`): `mrs xN, tpidr_el0` becomes
  `ldr xN, [x18, #0x1480+8*slot]` (x18 is the TEB, and Android's compiler
  leaves it alone), the canary following through the guest's own block --
  which every thread is given before its first call into guest code
  (`mg_code_entry`): a Java thread's first native method found the slot empty
  otherwise; `svc #0` becomes a branch to a piece of the library's island that
  leads the call out to the core (below, "ARM64's system calls"). TLS
  descriptors (`R_AARCH64_TLSDESC`, ARM64's compilers' way to dynamic TLS)
  are given one resolver that returns the offset in the layer's static block.
- **System calls by number**: `syscall()` and ARM64's traps take the ABI's
  own numbers, made 32-bit ARM's (`bionic/mg_syscall_abi.c`, its table made
  by `tools/syscall-numbers.py` from the NDK's headers), with the arguments
  ARM's passes otherwise (a 64-bit offset in one register, mmap's in bytes).
- **The vDSO** that Go's runtime reads its clocks through
  (`AT_SYSINFO_EHDR`, `bionic/mg_libc_misc.c`): an image of each processor's
  class and machine with its kernel's names and version --
  `__vdso_clock_gettime` and its kin of LINUX_2.6 on x86_64 and x86,
  `__kernel_clock_gettime` of LINUX_2.6.39 on ARM64 -- each the core's libc
  function of that name.

A native method is called with the processor's convention (`jni/mg_jni.c`,
System V's six and eight registers, AAPCS64's eight and eight, x86's stack);
a fault's and a stopped thread's registers are each processor's; a guest
thread is interrupted where it runs (what a signal does) by its context, the
x64 one below the 128 bytes System V code may keep under its stack pointer --
but not when Windows says it stopped the thread in the kernel
(`CONTEXT_SERVICE_ACTIVE`): inside NtContinue, on its way back from the
interruption before, its pc is guest code's already and the registers kept
for a caller are not yet its own. Sent on from there, an x64 thread went back
with rbx, rsi and rdi of the code that called NtContinue (x86 code had its
own); it is told again a millisecond later instead. The collector stops a
thread with SuspendThread too (`mg_thread_suspend`) and takes its registers,
and its stack from the stack pointer up, for all it holds -- a JNI reference
that native code keeps in a register is the object's address. Found inside
NtContinue, an x64 thread was given to it with the pc and stack pointer it
goes back to and rbx, rsi and rdi of NtContinue's caller, its own lying in
the context NtContinue was given, below that stack pointer. So a stop that
finds `CONTEXT_SERVICE_ACTIVE` at a pc that is not just past a `syscall` (or
`int 0x2e`) -- where a thread in an ordinary system call is found, with its
own registers -- lets the thread go and stops it again 10 us later; x86's
registers there are WOW64's copy, its own, and are taken as they are. So are
ARM64's and the phone's ARM32's, not looked at: no device has said yet what
Windows gives there. The test that says it (`test_stop_signalled`) runs on
every ABI -- the phone's Self-test with it, on the Thumb-2 and the ARM
library -- each spinner with a mark in every register `mg_thread_regs` has
but ARM64's x18, which holds the TEB and is left alone. While
the guest's handler runs, the registers of the code it interrupted are kept
in the thread's record alone (`mg_thread.was`, on the host's heap), and the
landing and the handler use those their callers do not keep -- rax, rcx,
rdx and r8-r11 on x64, eax, ecx and edx on x86 -- so a stop of a thread in
an interruption gives them too (`mg_thread_regs.was`), and the collector
takes them as it takes the thread's own. On Linux the kernel's frame of the
signal holds them, on the thread's stack above the handler's.

Memory an app gives back with madvise is decommitted, to be committed again
at its first touch (`bionic/mg_libc_mem.c`). A goroutine's stack is a piece
of Go's heap, whose free pages Go's scavenger gives back so and soon hands
out again as a stack: the goroutine then touches such a page first itself,
its stack pointer on it, and Windows lays a fault's records right below the
stack pointer -- on that page, which it cannot write; the process ended before
any handler ran. Go says nothing when it uses a page again, and
`GODEBUG=harddecommit`, which would have it map them first, never reaches a
library of Go's on Android: its runtime starts with an argv of "gojni" and no
environment. So madvise made by number from a library of Go's (its build ID
note, `mg_ld_lib_is_go`; the call's place, `mg_bionic_syscall_site`) commits
the pages again at once -- zero, their memory the system's, their commit kept
-- while everyone else's (Chromium's PartitionAlloc, V8) are decommitted as
before. The tests' POSIX host takes its faults on a stack of their own
(sigaltstack), which Windows has no counterpart of.

### ARM64's system calls, its stacks, and x18

An `svc #0` of an app's ARM64 code was a UDF, and every system call of a
library's own a fault the core's handler made the call at. On Windows a fault
is an exception, and it is dispatched on the faulting thread's own stack:
KiUserExceptionDispatcher lays the CONTEXT (0x390 bytes) and the exception's
record below sp, RtlDispatchException takes 0x2a0 bytes more, then come the
vectored handlers and the core's fault handler, whose frame is 5.6 KB -- some
8 KB for each call -- and ntdll's code reads the TEB through x18 before any
handler of the core's has run. On a Snapdragon X Elite (Windows 11 build
26340, GitHub issue 4) Telegram 13.2.0, Via and Spotify ended in ntdll's
RtlDispatchException as they started, at +0x34 (its first store into its new
frame: no stack was left under the exception's records) and at +0xA0 (`ldr
x8, [x18, #0x60]`: x18 was not the TEB); the loader had made the 119 system
calls of Telegram's libtmessages.49.so traps. x64's are led out to the core
with no exception (`syscall`, above), and the same apps ran there.

So ARM64's are led out too (`arm64/mg_patch_arm64.c`): each `svc #0` becomes a
branch to a piece of the library's island, which puts x16 and x30 below the
guest's sp, loads the guest's thread pointer, and calls the core's
`mg_arm64_syscall_entry`. The entry takes a frame of 848 bytes below that, on
the thread's own stack -- x0..x30 (x18 read, never written), the guest's sp,
NZCV, FPSR, FPCR and q0..q31 whole, since a function of the core's keeps only
the lower halves of v8..v15 -- and makes the call as the fault handler made it
(`mg_bionic_syscall`: the number in x8, the arguments in x0..x5, the svc's
place as the site, -errno in x0 and the guest's errno as it was; a signal's
handler that the call runs on the thread runs inside it, as inside the trap's
handler); then every register but x0 is as the kernel keeps them, and the
thread goes on after the svc. Where the guest's sp is on a stack of its own (a
goroutine's), the call is made on the thread's stack for system calls (256 KB,
slot 15 of its guest block), the TEB told it while it is used, as on x86_64.
The entry keeps the guest's sp and its way back on that call's frame, not in
the thread's block: a signal's handler that runs inside the call may make a
call of its own on the same thread. Where no island is in a branch's reach
(128 MB), or a thread has no stack for system calls (the piece then goes to a
UDF of its own), the call is the trap it was; the flag `trap-svc` makes every
site a trap that is not answered, as before. The loader says, for each
library: `ld: <library>: N system calls of its own led out to the core, M left
traps`.

**Stacks.** A Java thread's stack is at least 1 MB on the 64-bit processors,
ART's own default, and 160 KB on 32-bit ARM, where the phones' memory is why
it is small (`mg_dvm_art.c`). On Windows ARM64 a guest thread (pthread_create)
and a Java thread get 64 KB more than they ask for (`MG_STACK_FAULT_ROOM`): a
fault or a call left a trap at the bottom of a stack an app sized for its own
code still finds room there for Windows' exception frame and the core's
handler.

**What the reporter's machine is asked.** On Windows ARM64 alone, two things
said in the log, once each:

- a fault of guest code with less than 64 KB between its sp and the bottom of
  its thread's stack: `fault: thread <id> faulted at <library>+0x<offset>
  with <n> KB of its stack left below sp (<m> KB in all)`, once a thread;
- a thread stopped (the collector's `mg_thread_suspend`, a signal's
  `mg_thread_interrupt`) with x18 other than its TEB, recorded at its start:
  `x18: thread <id> was stopped at <library>+0x<offset> with x18 <value>, not
  its TEB <teb> (<n> such stops so far)`, once a pc. Nothing is said while a
  thread is stopped: the table is said once no thread is (the last
  `mg_thread_resume`, the end of an interruption). The pc names the code that
  wrote x18, or code after it.

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

**Calls take turns** on the desktop as on the phone (`mg_egl.c`,
"turns"): ANGLE serves all the contexts of a display with one state manager,
so once two threads have contexts of one display current, every EGL and GLES
call holds that display's lock and makes its thread's context current again
when another thread's was last. The thunks that take the turn are machine code
made at run time, one body for each processor and an entry per function: the
function is called with the caller's stack as it was (on x86 and x86_64 the
body takes the caller's return address and puts its own in its place; on
ARM64 it calls with the caller's stack pointer, as 32-bit ARM's does), the
argument and result registers kept around `turn_enter` and `turn_leave`.
Without them, Chromium's threads drew in the WebView's display at once and
ANGLE ended in `glDrawArrays`, marking a freed framebuffer's attachments.

**The WebView** is Android's System WebView of the processor
(`tools/webview-fetch.sh x86_64|x86|arm64-v8a`: the phone's release, Chromium
128, built for that processor, from the same AOSP commit), drawn through the
draw functor on the graphics processor as on the phone. Its PartitionAlloc
reserves pools of 16 GB with no access; mappings of 4 GB and more are the
app's like any other since `MG_ALIGN_UP` keeps a 64-bit size's upper half.

## Google's apps

A desktop package brings Google Play services, Google Play Store, Google
Services Framework and microG as the phone's does (`tools/build-appx.sh
<arch> --java --bitgapps ZIP --gms APK --play-store APK --microg APK`), and the
shell offers them the same way: the library lists Google Play services and
Play Store, the first start asks whether Google Play is wanted and installs
both as apps of their own, and microG is published for the apps set to it.
What differs:

- **Google Play services of the processor**: Google's variant for it
  (APKMirror's `x86 + x86_64` for x64, `x86` for x86, `arm64-v8a +
  armeabi-v7a` for ARM64; [PER-APP-PACKAGES.md](PER-APP-PACKAGES.md), "Google's
  APKs for each processor"). `tools/bitgapps-check.py` refuses an APK whose
  native code is not wholly the package's processor's. The x86 builds Google
  makes are for Android 7 and later (or 12), which deflate their DEX files:
  each app inflates them once into its `dex` folder (103 MB).
- **The WebView**: Google's sign-in is Google Play services' MinuteMaid, a web
  page in a WebView, so the package's Android root must have the processor's
  (`tools/webview-fetch.sh <abi>` before building); `build-appx.sh` refuses
  Google's apps without it. Released desktop packages had none -- `build/webview`
  held 32-bit ARM's alone -- and Play Store's Sign In came back at once ("this
  phone's Android root has no WebView package").
- **The certificate of the apps' packages**: desktop Windows installs Google
  Play services' and Play Store's packages only once `LocalState\signing\
  MeglerApps.cer` is trusted for the machine (0x800b0109 until then), which an
  administrator does once (`Import-Certificate ... Cert:\LocalMachine\
  TrustedPeople`). Windows opens no certificate an app hands it, so an app's
  page offers **Copy the Command** (that command, for PowerShell run as
  administrator) and **Show the Certificate** (its folder in File Explorer,
  where it opens with Install Certificate); Google Play's installs wait for the
  user meanwhile, and the Library says so. A test identity (MeglerTest) has a
  certificate of its own.

## The window

Megler's window on a PC opens phone-shaped (400 x 760) and is the user's to
resize, maximize or put in full screen; the running app's surface follows it,
as an Android app's window follows a freeform window on a Chromebook or in
Samsung DeX (`ui/PlayerScreen.cpp`, docs/JAVA.md "The screen resized and
turned").

- **Resizing.** The surface follows the window once its size has held for a
  poll of the player (250 to 500 ms): while it is dragged the app's last
  picture is scaled into the window, undistorted, and touches map onto it;
  then the app is told the new size (`megler_set_surface`). A Java app keeps
  its density, as Android keeps a window's density when it is resized, so its
  layout gets more room rather than larger text, and it has Android's
  configuration change: an activity is relaunched only for a change of
  orientation, or of a size its resources are made for (`w600dp`,
  `sw720dp`...), else its views are laid out again where they are. A window
  moved to a screen of another scale gives the app that screen's density.
- **A game's surface** is the window's pixels within the Render Size setting
  (the render cap, 1280 by default), however large the window grows: the
  swap chain scales it up to the window. Its window is made again at the new
  size (docs/ARCHITECTURE.md, "Graphics").
- **An app that asks for an orientation.** Its window is turned when it is
  not of that shape -- as wide as it was tall and as tall as it was wide
  (`ApplicationView::TryResizeView`), as Chrome OS opens and turns an Android
  app's window -- once for each wish the app has, never a window in full
  screen. A window that is not turned, or that the user makes the other shape
  again, gives the app a surface of its shape in its middle -- the window's
  proportions turned the other way, as tall as the window for an app that
  asks for portrait, as wide for one that asks for landscape -- letterboxed in
  black, as Android shows such an app on a large screen. A game, which asks
  for landscape, so opens in a landscape window.
- **The rotation** an app is told is the display's own (`Display.getRotation`,
  0 on a PC's monitor); a tablet turned turns the window too, the ways the app
  allows (`AutoRotationPreferences`).
- **An Xbox** has the whole television, which never changes size: an app
  that asks for portrait gets the strip in the middle, and nothing else is
  turned. **Windows RT**'s tablets are desktop Windows: the same as a PC's,
  with the tablet's turning as above.

## Proven

On this PC (x64, Windows 11 26300), 2026-11-05 and 06:

- `tools/build-desktop.sh x64 tests`, the core in `megler-core.so` loaded into
  a Windows x64 process, with the x86_64 guest test libraries: 5371 checks, 0
  failed (now and then one under load: the herd test's timing check of a few
  milliseconds). The GL tests go through the adapters to stand-ins of ANGLE's
  convention, floats and all. ELF64 with RELA, APS2 and RELR relocations,
  IFUNC, 90 patched thread-pointer and canary reads in one library,
  initial-exec and global-dynamic thread-local variables, PLT calls, JNI with
  every kind of argument, threads, a thread interrupted twenty times where it
  was, setjmp, system calls by number, the vDSO, an app from its APK through
  NativeActivity; and an app's own `syscall` led out to the core in six places
  of the seven t_raw_syscall has (the seventh, with a jump into it, left):
  every register but rax, the flags with DF set, xmm0, xmm15, ymm1's upper
  half and the red zone as they were, six arguments, a stack of the caller's
  own, four threads at once. And its accesses of `%fs` through a register, 19
  of t_fs_access's 20 led out to pieces (the 20th, a jump to it and nothing to
  move around it, left): Go's forms -- its reload of g, its stores, rt0_go's --
  and C's initial-exec ones through the GOT, an index too, on two threads; the
  flags, every register the access does not write and the red zone as they
  were; and a thread interrupted two hundred times in the middle of them by a
  handler that makes them too (with the patcher's word not kept across an
  interruption, its registers came back changed: t_fs_access 32, on Linux as
  well; with a thread sent on from inside NtContinue, on Windows alone: 37 of
  7200 calls of `mg_tests --load` with 8 processes at once, and 0 of 7200
  since it is not). And a goroutine's stack in a heap of a library of Go's
  (libt_go.so), given back by number as Go's runtime gives it back and run on
  again with a 64 KB frame written bottom first: committed still, zero, not a
  page taken back (without that, the process ended there). And a thread that
  some 10,000 signals a run keep sending back through NtContinue, stopped
  20000 times as the collector stops one (test_stop_signalled): its own
  registers at every stop in its loop -- 0 of 190,544 in twelve runs, where
  the same build with the look at its context switched off gave 980 of
  189,534 rbx, rsi and rdi of NtContinue's caller. And a thread stopped 200
  times as the collector stops one while the handler of a signal sent to it
  waits, spinning in guest code or in sigsuspend (test_stop_in_handler):
  every register the signal found it with among what the collector takes,
  at every stop -- 0 of 3200 stops short in four runs, where the same build
  with the interrupted code's registers left out of the stop gave 3200 of
  3200 without rax, rcx, rdx and r8-r11 (rbx, rbp, rsi, rdi and r12-r15 were
  on the stack, saved by the functions the handler runs in).
- `tools/build-desktop.sh x86 tests` (through WOW64): 5362 checks, 0 failed
  (now and then 1 under load: the same timing check), the GL tests through
  `__stdcall` stand-ins; the signalled thread stopped as the collector stops
  one had its own registers with no look at its context (0 of some 26,000
  stops in its loop); a thread stopped while its signal's handler waits, 0
  of 3200 stops short, where 3200 of 3200 lacked eax, ecx and edx with the
  interrupted code's registers left out.
- The registers a function keeps for its caller, kept across signals on
  every ABI (t_signal_regs: a thread spinning with a mark in each, Android's
  and Windows' both, sent 300 signals, each some microseconds after its
  handler ran for the one before, so that the next finds it on its way back
  from the landing): none came back changed on x64, x86, Linux x86_64, ARM
  (Thumb-2 and ARM mode, qemu-arm) and ARM64 (qemu-aarch64) -- 5339, 5330,
  5337, 5451 and 5328 checks with it. With x64's check of a thread stopped in
  the kernel skipped, 344 of 2000 `mg_tests --load` calls with 8 processes at
  once had rbx 0 and rsi and rdi addresses on the thread's stack, from 442
  threads sent on from inside NtContinue; 0 of 2000 with it, 492 left there.
- A library of Go's: a c-shared one for android/amd64 that Go 1.27.1 made
  here (with the NDK's clang for cgo; not in the repository), loaded by
  `mg_tests --load` on x64 and on WSL's Linux: its 468 accesses of `%fs`
  through a register and its 40 system calls all led out, its runtime started
  on a thread of its own (runtime.main ran, tls_g 16, the vDSO taken), and its
  exports called from the loading thread (`MG_TESTS_LOAD_CALL`: cgocallback,
  needm) -- 1000 goroutines allocating and sending, the collector run, the
  sums right: 20 of 20 runs on both, and 20 of 20 of five such rounds with
  `debug.FreeOSMemory()` between them, which put each round's goroutines on
  pages given back (and 3 of 3 runs again on x64 with everything merged).
  Before, none of 10 of those ended well on either: the process ended at a
  goroutine's first write, `movb $0,0x17(%rsp)` after `sub $0x10038,%rsp` (a
  second-chance access violation on Windows, exit 139 on Linux).
- `tools/build-tests.sh win` (MSVC's x64 build of the tests): 5023 checks, 0
  failed, and 5087 on Android's libcore, 0 failed; `tools/build-tests.sh
  linux64` (x86_64 on WSL's Linux): 5369 checks, 0 failed, and 5453 on
  Android's libcore, 0 failed, the registers a signal found a thread with on
  its stack while the handler waits, an app's own `syscall` and accesses of
  `%fs` through a register led out there too, and a goroutine's first touch
  of its own stack's page that libc's madvise gave back repaired on the
  handler's stack of its own (its bundle tests, which expect 32-bit ARM's
  split kept, fail there where the test app's bundles are built: an x86_64
  build keeps the processor's own); `tools/abi-check.py`: 0 differences on all
  four ABIs; the phone's core compiles for ARM32 with MSVC, and its tests
  under qemu-arm (`tools/build-tests.sh arm`) give 5463 checks, 0 failed: the
  signalled thread stopped as the collector stops one had its own r0-r12 and
  lr at every stop in its loop, Thumb-2 and ARM (0 of 65,659 in the four
  libraries' runs on libcore and without), where a host that reported r4 and
  lr changed at every 97th stop failed it (165 of 16,273 stops in the loop,
  and 165 of 16,202).
- `tools/build-tests.sh arm64`: ARM64's core and tests as one static program
  of the NDK's bionic, under qemu-aarch64 in WSL with the POSIX host: 5624
  checks, 0 failed, and 5773 checks, 0 failed on Android's libcore with
  arm64-v8a's natives (2026-11-08, branch arm64-svc-islands). ELF64, the
  patched TPIDR_EL0 reads (through the island there, bionic's own thread
  pointer being the host's), TLS descriptors, svc led out to the core
  (libt_svc.so, t_svc.c: 1310 calls through its three pieces; results, -errno
  and the guest's errno kept; x1..x30, the flags, FPSR, FPCR and q0..q31
  whole as they were; rt_sigprocmask by svc letting a held signal through,
  its handler's own call by svc inside it; 500 calls on goroutines' stacks of
  two kilobytes with no memory below, made on the threads' stacks for system
  calls, nothing written below sp but the piece's 16 bytes; the piece's trap
  where a thread has no stack for system calls; trap-svc's traps -- and an
  entry that changed x13 on its way out was caught, 113 from the two tests
  that keep registers, and one that took its way back from the thread's
  block after the call, as x86_64's and ARM32's entries do, went back from
  the call the handler's call was made inside to that call's place and
  ended at the mark in x30), AAPCS64's native calls, faults; the signalled
  thread's own x0-x28 (but x18), fp and lr at every stop in its loop (0 of
  63,814), and a failure where the host reported x19 and lr changed at every
  97th stop (160 of 16,044). (A run of the tests once hung in t_threads, every
  thread asleep on a futex: a lost wakeup in `mg_pthread.c`'s mutex_acquire,
  shared code; fixed since, and t_mutex_stress checks it.)
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
- ARM64 builds -- the core's ELF, the tests' program, ANGLE, the package; on
  Windows it is not run: there is no ARM64 machine here.
- The APKs of a Downloads folder, 2026-11-06: each started for 30 seconds in
  a test package of its own (one Megler process an app, its first pictures
  taken), on x64 the 144 that have x86-64 code or none, on x86 the 153
  that have x86 code or none. On the last builds (x64 923, x86 924, made from
  apk-fixes 0415d152; what came after it changes tests and documents alone):
  on x64 135 started, of which 124 still ran after their 30 seconds, 5 ended
  themselves with status 0 as they do on a phone (a sound recorder,
  ACBridge's System.exit, Accuweather's widget app, the two test videos), 4
  were closed by a Back from outside the test, and 2 ended on their own
  errors (below); on x86 145 started, 134 still running, the same 5 ended
  themselves, 4 closed by a Back, 2 on their own errors; no fault on either.
  RUTUBE plays its films on both. What did not start does not on a phone
  either: VirtualHere's server archives (not APKs), a widget with no
  activity, Android's framework-res, Google Play Store without Google Play
  services, and on x64 a WhatsApp bundle whose x86-64 split is damaged in the
  bundle itself (95% of it zeros; its x86 split runs). What the runs found
  is mended in the branch apk-fixes, a commit each: an app's own `int 0x80`
  on x86 (Asphalt 9, WhatsApp, Discord, MAX ended at their start), the
  guest's malloc at bionic's alignment and of jemalloc's sizes (x86-64's
  16-byte SSE stores faulted in exteraGram, Spotify, Google Maps and others;
  MaxVibe wrote into a malloc(0) and corrupted Windows' heap), a library page
  that writes itself (Titanium Backup stood still), SDL apps run as Java
  (Minecraft: Story Mode), fonts, styles and colours of newer API levels
  (Apple Music, Niagara), a view in Compose's AndroidView drawn again when it
  changes (RUTUBE's films were black), BluetoothManager's adapter
  (VectorWatch), and the log written out when an app ends itself. What is the
  apps' own: XDurakLite (LSPlant hooks ART, which is not here), Tekken Mobile
  (INCA AppGuard ends it), Fake Device Test (reads /proc/cpuinfo into a
  buffer of 10000 characters until it is told -1, which sixteen cores'
  cpuinfo never tells), com.example.app (Flutter for arm64 alone), Titanium
  Backup (asks for root, and Megler starts no programs), Minecraft: Story
  Mode (wants its OBB), Aurora Store (restarts itself: below). The suites
  with desktop-release merged into apk-fixes (26b772a0): x64 5484 checks, x86
  5472, Linux x86_64 5482 and 5566 on libcore, ARM under qemu 5553, ARM64
  under qemu 5451 and 5535 on libcore, 0 failed each; the MSVC build 5118 and
  5182 on libcore, 0 failed (0415d152, the same but for tests of guest code).
- Google's apps in the x64 package (2026-11-07, 0.1.0.942 under the test
  identity MeglerTest, branch desktop-gms): Google Play services 26.41.31
  (`040800`, x86_64), Play Store 53.7.23, microG 0.3.17. The library offered
  both, the first start asked, and once the user had trusted the test
  identity's certificate Windows's deployment engine installed both (options
  0x1); Update All made them again for the next build without copying their
  APKs. Play Store's own package started with Google's own Google Play
  services, whose x86_64 libraries loaded under the core (`libgmscore.so`,
  `libconscrypt_gmscore_jni.so`, its security provider installed,
  `libleveldbjni.so`); the check-in succeeded
  (`android.googleapis.com/checkin`); Play Store showed its page for no
  account; Sign In opened Google's sign-in page in the x86_64 WebView, drawn
  with OpenGL ES 3 through ANGLE; the user signed in with an account of theirs
  through its two-step verification, and Play Store showed its home page.
  The user installed Gmail from it: Play Store handed the bundle to its own
  Megler (`meglertest:install`, opened in that package by name), which
  unpacked it (a base and three splits), installed it as an app of its own in
  2 s, and Play Store started it. The microG test app, as an app of its own
  set to microG, ran with microG's and passed ("PASS isolated GmsCore
  broker..."), microG's x86_64 `libconscrypt_gmscore_jni.so` loaded. In the
  x86 package (0.1.0.943, MeglerTestX86) the x86 WebView ran the WebView test
  app (pages, the JavaScript bridge, a redirect, input). Seen on the way: Google Play
  services asks again and again for a module it may not download
  (`Appsearch.optional`, "logged 6000 times" in two minutes), and some of
  Chromium's TLS handshakes were reset (`net_error -101`) on this PC's
  network.

In developer mode the first start after the stage folder was registered again
over an earlier build is refused the package's files ("core: not loaded: ...
access denied"); the next start is not. Registering it a second time before
the first start mends it too: the second registration gives the rebuilt
folder the package's access.

## What is left

1. **OpenGL ES**: ARM64's ANGLE is built and not run. On x86_64 and x86 a
   function of ANGLE's that none of the NDK's headers declares (an extension
   of ANGLE's own) has no adapter and is "no such function", where the phone
   gives it.
2. **Go on x86_64**: its g, its system calls, its clock and its goroutines'
   stacks on pages its scavenger gave back are the core's now, and a
   library's exports run (above). A fault in Go's code is not given to its
   handler off 32-bit ARM (`mg_signal_fault`): a nil dereference ends the
   process instead of panicking. What Go's heap gives back stays committed:
   the commit limit counts that heap at its largest.
3. **`int 0x80` in x86 code of an app's own** is made a system call at the
   access violation Windows gives at it (`mg_core.c`): every one is a fault,
   slow for code that makes many (none of the apps seen does).
4. **WebView on ARM64**: fetched and packed as x86_64's and x86's are, and not
   run on an ARM64 device.
5. **ARM64 on a device**: what is Windows' alone -- the TPIDR_EL0 reads
   patched through x18, x18 left alone by the guest's code (Android 9's
   prebuilt libcrypto among others), the context of a faulting, stopped or
   interrupted thread; and of the system calls led out, the own stack told by
   the TEB, the TEB told the stack for system calls while a goroutine's call
   is made there, and a call left a trap dispatched by Windows. The rest runs
   under qemu-aarch64 (above). The first such machine, GitHub issue 4's, ended
   in RtlDispatchException with x18 not the TEB at +0xA0, which leading the
   calls out does not mend by itself (a thread takes far fewer exceptions,
   but one with x18 so still ends at its next): the two lines above ("What
   the reporter's machine is asked") are to find out where it goes bad.
6. **Ping in a desktop app container**: Windows gives an app container no
   ICMP echo at all -- IcmpCreateFile fails with ERROR_ACCESS_DENIED whatever
   the package's capabilities (the NSI service turns away the echo's
   registration, and `\\.\Nsi` any process of Low integrity) -- so the ping
   socket is refused with EACCES, as Linux refuses one, and isReachable goes
   on to its TCP connection. A real echo would take a helper outside the
   container.
7. **An app that restarts itself** -- the library ProcessPhoenix, which
   Aurora Store calls after its first-start pages: its activity in a process
   of its own starts the app's first activity again and ends its process with
   Runtime.exit. A Megler process runs one app, once, so the app ends there,
   on the phone as here, and is started again by hand. Windows 10 1803 and
   later can restart a packaged app (CoreApplication.RequestRestartAsync);
   Windows 10 Mobile cannot.
8. **A system call made inside one led out, on x86_64 and 32-bit ARM**: their
   entries (`mg_x86_syscall_entry`, `mg_stub_svc_entry`) keep the guest's
   stack pointer and way back in the thread's block across the call, where a
   signal's handler that runs inside it (one let through by the call, one the
   thread sent itself) and makes a call of its own the same way would write
   them again, and the first call would go back to the second's place.
   ARM64's keeps them on its frame. Go's runtime, whose signals' handlers do
   make such calls, has its preemption's signal dropped here (tgkill by
   number does nothing), which keeps it from happening there so far.
