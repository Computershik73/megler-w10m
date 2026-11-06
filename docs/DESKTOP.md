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
  otherwise; `svc #0` becomes a UDF the fault handler does the call for. TLS
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
registers there are WOW64's copy, its own, and are taken as they are. While
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

## Proven

On this PC (x64, Windows 11 26300), 2026-11-05 and 06:

- `tools/build-desktop.sh x64 tests`, the core in `megler-core.so` loaded into
  a Windows x64 process, with the x86_64 guest test libraries: 5363 checks, 3
  failed -- the three float checks of the GLES stand-in (below); now and then
  one or two more under load, the herd test's timing check. ELF64 with
  RELA, APS2 and RELR relocations, IFUNC, 90 patched thread-pointer and canary
  reads in one library, initial-exec and global-dynamic thread-local
  variables, PLT calls, JNI with every kind of argument, threads, a thread
  interrupted twenty times where it was, setjmp, system calls by number, the
  vDSO, an app from its APK through NativeActivity; and an app's own
  `syscall` led out to the core in six places of the seven t_raw_syscall has
  (the seventh, with a jump into it, left): every register but rax, the flags
  with DF set, xmm0, xmm15, ymm1's upper half and the red zone as they were,
  six arguments, a stack of the caller's own, four threads at once. And its
  accesses of `%fs` through a register, 19 of t_fs_access's 20 led out to
  pieces (the 20th, a jump to it and nothing to move around it, left): Go's
  forms -- its reload of g, its stores, rt0_go's -- and C's initial-exec ones
  through the GOT, an index too, on two threads; the flags, every register
  the access does not write and the red zone as they were; and a thread
  interrupted two hundred times in the middle of them by a handler that makes
  them too (with the patcher's word not kept across an interruption, its
  registers came back changed: t_fs_access 32, on Linux as well; with a
  thread sent on from inside NtContinue, on Windows alone: 37 of 7200 calls
  of `mg_tests --load` with 8 processes at once, and 0 of 7200 since it is
  not). And a goroutine's stack in a heap of a library of Go's (libt_go.so),
  given back by number as Go's runtime gives it back and run on again with a
  64 KB frame written bottom first: committed still, zero, not a page taken
  back (without that, the process ended there). And a thread that some
  10,000 signals a run keep sending back through NtContinue, stopped 20000
  times as the collector stops one (test_stop_signalled): its own registers
  at every stop in its loop -- 0 of 190,544 in twelve runs, where the same
  build with the look at its context switched off gave 980 of 189,534 rbx,
  rsi and rdi of NtContinue's caller. And a thread stopped 200 times as the
  collector stops one while the handler of a signal sent to it waits,
  spinning in guest code or in sigsuspend (test_stop_in_handler): every
  register the signal found it with among what the collector takes, at every
  stop -- 0 of 3200 stops short in four runs, where the same build with the
  interrupted code's registers left out of the stop gave 3200 of 3200
  without rax, rcx, rdx and r8-r11 (rbx, rbp, rsi, rdi and r12-r15 were on
  the stack, saved by the functions the handler runs in).
- `tools/build-desktop.sh x86 tests` (through WOW64): 5354 checks, 0 failed
  (now and then 1 under load: a timing check of a few milliseconds); the
  signalled thread stopped as the collector stops one had its own registers
  with no look at its context (0 of some 26,000 stops in its loop); a thread
  stopped while its signal's handler waits, 0 of 3200 stops short, where
  3200 of 3200 lacked eax, ecx and edx with the interrupted code's registers
  left out.
- A library of Go's: a c-shared one for android/amd64 that Go 1.27.1 made
  here (with the NDK's clang for cgo; not in the repository), loaded by
  `mg_tests --load` on x64 and on WSL's Linux: its 468 accesses of `%fs`
  through a register and its 40 system calls all led out, its runtime started
  on a thread of its own (runtime.main ran, tls_g 16, the vDSO taken), and its
  exports called from the loading thread (`MG_TESTS_LOAD_CALL`: cgocallback,
  needm) -- 1000 goroutines allocating and sending, the collector run, the
  sums right: 20 of 20 runs on both, and 20 of 20 of five such rounds with
  `debug.FreeOSMemory()` between them, which put each round's goroutines on
  pages given back. Before, none of 10 of those ended well on either: the
  process ended at a goroutine's first write, `movb $0,0x17(%rsp)` after
  `sub $0x10038,%rsp` (a second-chance access violation on Windows, exit 139
  on Linux).
- `tools/build-tests.sh win` (MSVC's x64 build of the tests): 5023 checks, 0
  failed, and 5087 on Android's libcore, 0 failed; `tools/build-tests.sh
  linux64` (x86_64 on WSL's Linux): 5361 checks, 3 failed (the float checks
  above), the registers a signal found a thread with on its stack while the
  handler waits, an app's own `syscall` and accesses of `%fs` through a
  register led out there too, and a goroutine's first touch of its own
  stack's page that libc's madvise gave back repaired on the handler's stack
  of its own; `tools/abi-check.py`: 0 differences on all four ABIs; the phone's
  core compiles for ARM32 with MSVC, 148 of 148, and its tests under qemu-arm
  (`tools/build-tests.sh arm`) give 5443 checks, 0 failed.
- `tools/build-tests.sh arm64`: ARM64's core and tests as one static program
  of the NDK's bionic, under qemu-aarch64 in WSL with the POSIX host: 5320
  checks, 3 failed, and 5404 checks, 3 failed on Android's libcore with
  arm64-v8a's natives (the GLES stand-in's float checks, as on x64). ELF64,
  the patched TPIDR_EL0 reads (through the island there, bionic's own thread
  pointer being the host's), TLS descriptors, svc traps, AAPCS64's native
  calls, faults.
- The x64 and x86 packages (`tools/build-appx.sh x64 --java --auto java`),
  registered from their stage folder in developer mode and started: the core
  loads, "megler core on win32-x64"; Android 9's libcore runs its natives
  through the runtime's tests (the x86 package's Self-test: 2437 checks, 3
  failed, none of them the port's); and the Java test app hello.apk starts,
  draws (its first picture after 3.8 s on x64, 0.8 s on x86, through Direct2D
  and Direct3D 11.1), counts presses of its button and goes through onPause,
  onStop and onDestroy on Back, status 0.
- ARM64 builds -- the core's ELF, the tests' program, the package; on
  Windows it is not run: there is no ARM64 machine here.

In developer mode the first start after the stage folder was registered again
over an earlier build is refused the package's files ("core: not loaded: ...
access denied"); the next start is not.

## What is left

1. **OpenGL ES.** ANGLE for desktop Windows on each processor, and a layer of
   adapters in front of it: ANGLE's functions are of Windows' convention
   (Microsoft's x64 one, `__stdcall` on x86), guest code's of Android's, and
   only ARM64's are the same. Every GL and EGL entry point needs one, made from
   the NDK's GLES headers; the float adapters of `ndk/mg_egl.c` are 32-bit
   ARM's (softfp) and pass x86_64's and ARM64's floats wrong (the three
   checks above).
2. **Go on x86_64**: its g, its system calls, its clock and its goroutines'
   stacks on pages its scavenger gave back are the core's now, and a
   library's exports run (above). A fault in Go's code is not given to its
   handler off 32-bit ARM (`mg_signal_fault`): a nil dereference ends the
   process instead of panicking. What Go's heap gives back stays committed:
   the commit limit counts that heap at its largest.
3. **`int 0x80` in x86 code of an app's own** is counted, not rewritten (x86
   keeps nothing below its stack pointer: a trap would do there).
4. **WebView**: Android's System WebView for x86_64 and arm64 (the phone's is
   32-bit ARM's).
5. **ARM64 on a device**: what is Windows' alone -- the TPIDR_EL0 reads
   patched through x18, x18 left alone by the guest's code (Android 9's
   prebuilt libcrypto among others), the context of a faulting, stopped or
   interrupted thread. The rest runs under qemu-aarch64 (above).
6. **Ping in a desktop app container**: Windows gives an app container no
   ICMP echo at all -- IcmpCreateFile fails with ERROR_ACCESS_DENIED whatever
   the package's capabilities (the NSI service turns away the echo's
   registration, and `\\.\Nsi` any process of Low integrity) -- so the ping
   socket is refused with EACCES, as Linux refuses one, and isReachable goes
   on to its TCP connection. A real echo would take a helper outside the
   container.
