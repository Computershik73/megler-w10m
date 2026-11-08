# A JIT for the Dalvik interpreter

The plan for compiling an app's hot bytecode to Thumb-2, written before the code, and then to A64, x86-64 and x86 for the
PCs. What is built so far is said at the end; this file follows the work.

## Why

Java apps spend most of their time in the interpreter (`core/src/dvm/mg_dvm_interp_loop.h`). On the HP Elite x3 it runs
5 to 17 million Dalvik instructions a second (docs/JAVA.md, "the interpreter's benchmark"): every instruction is a
fetch, a `switch` (MSVC has no computed goto), loads and stores of the frame's registers in memory, and the loop's
bookkeeping. A plain loop of int arithmetic costs some 9 ns a turn on a Ryzen and 136 ns under qemu; the same loop as
machine code is a few cycles.

## What makes it simpler than a JIT usually is

- **The collector needs no safepoints.** It stops every other thread from outside (`mg_thread_suspend`, mg_dvm_gc.c),
  reads its registers and stacks and every frame's registers conservatively, and moves nothing. Compiled code may hold
  references in machine registers and never polls for the collector.
- **The frame stays the interpreter's.** Compiled code works on the frame's own registers (`f->regs`), loading and
  storing them as the interpreter does. At any instruction it can stop and hand the frame back: the interpreter goes on
  from that `pc` with nothing to rebuild. So what compiled code does not do -- a call, a return, an exception, an
  allocation, a class not yet initialised, anything rare -- it leaves to the interpreter by returning to it.
- **One thread at a time per frame**, and frames are never moved.

## The shape

- **What is compiled.** A method, whole, the first time its back-edge count passes a threshold (loops are what is hot;
  a method called often but without loops gains less and is left for later). Each instruction the compiler knows becomes
  a few of the processor's (Thumb-2's, A64's, x86's); each it does not becomes an exit: "go on in the interpreter at this
  pc". A method whose every instruction is an exit is not kept.
- **Entries.** The code can be entered at pc 0 and at every branch target (a loop's head): the interpreter, at a jump
  back (where it already looks at `g_dvm.slow`, SAFEPOINT), finds the method's code and jumps in at the target. Entering
  at a loop's head is on-stack replacement for free: the frame is the state.
- **Exits.** An exit stores the pc to go on at and returns. The interpreter reloads `pc` and continues its loop.
- **The calling convention.** `int code(mg_dvm_jit_ctx *ctx, const void *entry)`: the context holds the frame's
  registers, the address of `g_dvm.slow`, the pc to go on at, the thread, the frame, and the last call's result; a
  callee-saved register holds the frame's registers throughout (r4 on Thumb-2, x19 on A64, rbx and ebx on x86-64 and
  x86), another the context (r5, x20, r12, esi). Returns 0 (go on at `ctx->pc`), 1 (an exception pending, thrown at
  `ctx->pc`) or 2 (returned, the value in `ctx->result`).
- **Watching.** While `g_dvm.slow` is set (a trace, a step limit, the profile) nothing compiled runs: the watched loop
  never enters code, and compiled code checks the flag at each jump back and exits there.
- **Memory.** Code goes in executable pages of its own (the package has rescap:codeGeneration; the patcher writes code
  the same way), flushed with FlushInstructionCache on Windows and `__builtin___clear_cache` under Linux.
- **Turning it off.** The flag `no-jit` (a file in LocalState\flags) keeps everything interpreted; `trace-jit` says each
  method compiled, its size, and how often each of its exits is taken.

## The back ends

`core/src/dvm/mg_dvm_jit.c` is the front end, which every processor shares: the walk over a method's bytecode, which
instructions are compiled and which handed back, the decisions that need the runtime (a field resolved, a class
initialised, a call done in place or the quick way, a switch's table), branches and their targets, the code's memory and
its publishing. A back end writes each instruction's code (its `emit_*` functions) and the branches:
`mg_dvm_jit_thumb2.h` for 32-bit ARM, `mg_dvm_jit_a64.h` for ARM64, `mg_dvm_jit_x86.h` for x86-64 and x86 (`MG_DVM_JIT`
says where there is one). A branch takes as many of the back end's units as it says for its condition: halfwords, words,
or x86's bytes. The split changed nothing on the phone: the 3842 methods the ARM32 suites compile with the threshold at
1 came out byte for byte as before it (the pointers movw and movt load aside, which another build has elsewhere).

**A64** (desktop Windows on ARM64, and the arm64 tests under qemu-aarch64) follows AAPCS64: x19 holds the frame's
registers, x20 the context and x21 the slow flag's address, all three callee-saved; x29 and x30 make a frame record;
x0-x3 and x9-x11 hold values and d0, d1 floating point, nothing kept in them from one instruction to the next, and never
a reference in a vector register (the collector reads x0-x28, fp and lr of a thread it stops). x18 is never touched: it
is the TEB on Windows, and code that changes it crashes Windows' exception dispatch (GitHub issue #4). A register of the
frame is 64 bits there: an int is computed in a W register, whose write clears the upper half, and stored whole, so that
if-eq and if-eqz, which compare whole registers as the interpreter does (a reference is 64 bits), find it as the
interpreter keeps it; a long or a double is its two registers' low halves, each zero-extended. Java's semantics come
with the instructions: W and X arithmetic wraps; LSLV, LSRV and ASRV take the count modulo 32 or 64; SDIV gives
MIN_VALUE for MIN_VALUE / -1, and MSUB the remainder, 0; FCVTZS rounds toward zero, saturates and makes NaN 0, so that
d2i, f2i, d2l and f2l, and the conversions from a long, are an instruction each, where Thumb-2 calls the runtime for a
long's; cmpl and cmpg are FCMP, CSET and CSNEG (an unordered compare satisfies LT but not MI); cmp-long is CMP, CSET and
CSNEG; if-eqz and if-nez are CBZ and CBNZ. A divisor of zero is a CBZ to the exit, and the interpreter throws; a float's
or a double's remainder is fmod's, by a call. A64 compiles all that Thumb-2 does, and a long's division, remainder and
conversions in the code.

**x86-64** (`mg_dvm_jit_x86.h`: desktop Windows' core, an ELF of Android's ABI, and the linux64 tests; not MSVC's build
of the tests, whose convention is Windows') follows System V's convention, the core's, not Windows': rbx holds the
frame's registers, r12 the context, r13 the slow flag's address, rbp a frame pointer, and r14 is pushed only to keep rsp
16-byte aligned at the calls; rax, rcx, rdx and r11 hold values, xmm0 and xmm1 floating point (SSE2, never x87), nothing
kept in them from one instruction to the next and never a reference in an xmm (the collector reads rax to r15). Neither
fs nor gs is ever named: gs is Windows' TEB on x64, and the guest's own reads of fs are the patcher's. A frame's
register is 64 bits, as on A64: a 32-bit operation's write clears the upper half, and a long or a double is its two
registers' low halves. A branch is a Jcc or a JMP with a 32-bit offset (6 or 5 bytes: the back end's units are bytes).
x86's answers that are not Java's are mended in the code: CVTTSS2SI and CVTTSD2SI give the "integer indefinite"
(MIN_VALUE) for NaN and for what is out of range, and the code, finding MIN_VALUE (a compare with 1 overflows only for
it), makes it 0 for NaN, MAX_VALUE for what is too large, and leaves it for what is too small; IDIV faults on a divisor
of zero (an exit first, as everywhere) and on MIN_VALUE / -1, so a divisor of -1 is a negation and a remainder of 0.
Shifts by CL take the count modulo 32 or 64, as Java does; cmpl and cmpg are UCOMISS or UCOMISD, which say unordered by
PF; a float's or a double's remainder is fmod's, by a call.

**x86** (the same file's 32-bit mode: desktop Windows' x86 core, i386's cdecl with Android's 16-byte stack alignment at
calls) keeps ebx the frame's registers, esi the context, edi the slow flag's address and ebp a frame pointer, and 28
bytes below them where the calls' arguments are written (the stack then 16-byte aligned at each call). A register of the
frame is 32 bits, as on the phone, so a long is the 8 bytes of its two registers: add/adc, sub/sbb, the 64-bit product
from MUL and two IMULs, SHLD/SHRD and SHL/SAR/SHR with a count of 32 and more mended (they take five bits of it),
cmp-long by the high words signed and the low ones unsigned; a double is MOVSD's 8 bytes of the two registers, a float
MOVSS's 4. What 32-bit x86 has no SSE2 instruction for -- a long's division and remainder, a long's conversions to and
from float and double -- and a float's or a double's remainder are C: the division by a call that returns the long in
edx:eax, the others by calls that read and write the frame's registers themselves, as i386's convention would return a
float in x87's st(0), which the code never touches.

## The first instructions

Moves and constants; int arithmetic and logic (`add`, `sub`, `mul`, `and`, `or`, `xor`, the shifts, their `/2addr`,
`/lit8` and `/lit16` forms; `div` and `rem` with the divisor checked, an exit when it is zero); comparisons and
branches (`if-*`, `if-*z`, `goto`); `array-length`, `aget` and `aput` of ints, bytes, chars, shorts and references
with the bounds checked (an exit when out of bounds or null, the interpreter then throws); `iget` and `iput` of a field
already resolved (an exit when the object is null); `sget` and `sput` of a class already initialised; long arithmetic
next. Calls stay exits at first: the interpreter does the call, and the code is entered again at the next jump back.

## How it is tested

The libcore and DVM tests run under qemu-arm, which executes the Thumb-2 the JIT writes. They are run with the
threshold at 0, so that every method with a loop is compiled at its first jump back, and must pass as they do
interpreted. With `QEMU_CPU=cortex-a9` (and `WSLENV=QEMU_CPU`, which hands it to WSL) qemu plays a Surface RT's
processor, which has no SDIV: the tests then take the JIT's division by a call. The benchmark (`tools/dvm-bench.sh
arm`) gives the speed under qemu, interpreted and compiled; the phone's Self-test gives the phone's.

The arm64 tests run the A64 the same way under qemu-aarch64 (`tools/build-tests.sh arm64`; MEGLER_JIT_THRESHOLD reaches
the program when WSLENV names it): the core's and Android's libcore, with the JIT's own threshold, with it at 1 and with
it at 0 (off), and `MEGLER_JIT_THRESHOLD=0 tools/dvm-bench.sh arm64` beside `tools/dvm-bench.sh arm64`. What only an
ARM64 machine says is said under "Where it stands".

x86-64 runs here natively: `tools/build-tests.sh linux64` (WSL's gcc, System V, as the core's ELF has it) and
`tools/build-desktop.sh x64 tests` (the core's ELF and Windows' host, as the package has them; `-- --dvm-android <root>`
for libcore), with MEGLER_JIT_THRESHOLD in the environment at the JIT's own, 1 and 0; `tools/dvm-bench.sh x64` the same
two ways. x86 runs here too, by WOW64: `tools/build-desktop.sh x86 tests` and `tools/dvm-bench.sh x86`.

Every arithmetic instruction is checked compiled as well as interpreted: tools/dvm-ops.py gives each of `t.Ops`' methods
a twin that does its instruction in the second turn of a loop, which the JIT compiles at the first jump back, and
`test_ops_jit` (mg_dvm_tests.c) runs the 1991 cases through the twins -- Java's answers worked out in Python: shifts by
32, 33, 63, 64 and negative counts, MIN_VALUE / -1 and % -1, NaN and out-of-range conversions to int and long, a float's
remainder, cmpl and cmpg of NaN, cmp-long's edges, divisors of zero -- and checks that each twin's loop was compiled
whole, so that none ran interpreted unnoticed. `t.Jit` adds what the twins do not reach: ints made in compiled code
compared whole with if-ne and if-nez, in the code and in the interpreter after a hand-back, and returned to an
interpreted caller (upperHalves, upperCaller); a frame of 300 registers, past A64's register pairs' reach and past
move/16's 255 (bigFrame); a negative index (readDown) and a null object (fieldOfNull) met by compiled loops.

## Where it stands

`core/src/dvm/mg_dvm_jit.c` compiles: moves and constants (`const-wide` and `const-string` of a string already made
included); int arithmetic and logic in every form (division and remainder with the divisor checked, by SDIV, or by a
call of the runtime's on a processor without it, a Surface RT's Cortex-A9; the shifts' counts masked); long arithmetic
(add, sub, mul, and, or, xor, the three shifts by any count, neg, not, `int-to-long`, `long-to-int`, `cmp-long`);
float and double arithmetic in VFP (add, sub, mul, div, neg, `cmpl` and `cmpg`, the
conversions between int, float and double, toward zero and saturated as Java says); and by calls of small functions of
the runtime's, a long's division and remainder (the divisor checked), a float's and a double's remainder, a long's
conversions to and from them; `if-*`, `if-*z`, `goto`, `packed-switch` and `sparse-switch` of up to 64
keys; `array-length`, `aget` and `aput` of ints, bytes, chars, shorts, booleans, floats, longs and doubles and `aget-object`; `iget` and `iput`
of every width of a field resolved by then, `sget` and `sput` of a class initialised by then; calls of every kind with
their `move-result`; the returns; and through `mg_dvm_jit_op` (mg_dvm_interp.c) `new-instance`, `new-array`, `const-class`, the
monitors, `aput-object` with its store check, `check-cast` and `instance-of` -- these two in the code itself when the
object is null or of the very class named. A call goes through `mg_dvm_jit_invoke` (mg_dvm_interp.c), which finds the method as the
interpreter's do_invoke does and does it as it would -- an intrinsic or a trivial body in place, a native method called,
bytecode run to its return -- in the callee's own compiled code when it has some at its start, else in a loop of the
interpreter's on the host's stack; an exception it ends with sends the frame back to the interpreter, pending, at the
invoke; where the host's stack has too little left for another loop (deep recursion through compiled code) the call is the
interpreter's, without one. A call of a trivial method (empty, a constant, a getter, a setter, a static getter: the
interpreter's "trivial methods") is done in the code itself, after a check that the object's class has that very method
in its slot (not for a final method or class, which nothing overrides); an override, a null object, an object without the
field go by `mg_dvm_jit_invoke`. So do the intrinsics that are a few instructions: String.length, isEmpty and charAt (a
null string or an index out of bounds by the call, which throws), Math.min, max and abs of ints, Math.sqrt.

Any other call is tried the quick way first (`fast_invoke`; `mg_dvm_jit_fast` in mg_dvm_interp.c): the method resolved
when the caller is compiled and the arguments' registers packed into a word, so that nothing of the invoke is decoded or
looked up again; a static or direct call, or a virtual one of a method nothing overrides, names the method itself, and its
code having a start for such calls is asked in the code before any C is called; a virtual call finds its method by the
object's class's vtable, an interface call by the class's cache. The method's code says, when it is published, whether a
call of it needs nothing more than a frame pushed and its start run (its code's `fast`: no handlers, not synchronized, no
intrinsic). What is not so -- a null object, a method not compiled at its start, instructions watched, the profile or the
call trace on, too little of the host's stack or of the frame stack's segment left -- goes the general way, nothing having
happened. Under qemu-arm a call went from 117-138 ns to 99-125 ns, HashMap.get from 2060 to about 1700, Integer.valueOf
from 740 to about 600; the flag no-fast-calls leaves every call to the general way. The log's line of the JIT counts the
calls each way.

An app that hooks methods (Xposed's XposedBridge: docs/JAVA.md, "Methods an app hooks") must not have a hooked method's
body done in place. A call of a method hooked by then is not compiled in place, nor tried the quick way when it names the
method itself, and the quick way declines a hooked method; and once the app has hooked anything (`mg_dvm_jit_hooks_on`)
every call compiled in place first loads its method's mark and goes the general way, which calls the hook, when it is set:
five instructions more, only in such an app. The code compiled before the first hook, whose calls in place do not look, is
forgotten then (the JIT keeps the list of the methods it published): its methods are interpreted until they are hot again.
It is never freed, as a thread may be inside it, and goes on there until it hands the frame back or returns. `test_hooks`
(mg_dvm_tests_hooks.c) compiles loops that call a getter in place before the first hook and after it, and hooks the getter
under the code that does it: built without the look or without the forgetting, six of its checks fail (a loop of 2000
calls saw its hook once, at its turn in the interpreter).

A method is compiled when the jumps back it takes and the calls of it made by then reach 1000. Its code is entered at the
head of a loop compiled whole -- a loop that hands back at every turn costs more than it saves -- and at its start, for
its calls, when the code there does some work before it hands anything back (four instructions in a row, or a return):
the interpreter's call (do_invoke) and the compiled one (`mg_dvm_jit_invoke`) go in there, and a compiled return ends the
call with no loop of the interpreter's. Code goes on pages of an arena of the process's: code under half a page on the
page being filled, with other methods', which is made executable once the next does not fit or 200 ms after its first
(the method interpreted meanwhile), larger code on pages of its own. The data app's 536 methods are 377 KB of code: 2.2 MB
of pages each on its own, some 480 KB shared.

Under qemu-arm (ns a turn, interpreted then compiled): int arithmetic 141 to 5, iget and iput 217 to 15, sget and sput 119
to 4, aget and aput of an int[256] 228 to 17, String.indexOf 81 to 3, String.charAt 256 to 97, String.hashCode 187 to 94,
System.arraycopy 225 to 145, Math.min, max, abs 385 to 272; nothing slower. The tests (`t.Jit`, mg_dvm_tests.c) run every
compiled instruction and hand-back, calls of each kind with their results, a callee's exception caught in the loop,
recursion 3000 deep through compiled calls, long arithmetic against C, and both switches; the whole main suite passes with
every loop compiled (MEGLER_JIT_THRESHOLD=1).

A64 (`mg_dvm_jit_a64.h`) compiles all of that too, and a long's division, remainder and conversions in the code. The
arm64 suites pass under qemu-aarch64 at the JIT's own threshold, with every loop compiled (MEGLER_JIT_THRESHOLD=1) and
with the JIT off (0): 7635 checks, and 7789 on Android's libcore, none failed. Under qemu-aarch64 (ns a turn,
interpreted then compiled; `tools/dvm-bench.sh arm64`): int arithmetic 221 to 6.4, long and double arithmetic 377 to 27,
iget and iput 413 to 19, sget and sput 215 to 4.9, aget and aput of an int[256] 395 to 24, String.charAt 320 to 9.6,
String.indexOf 139 to 3.7, Math.min, max, abs 632 to 12, invoke-static 284 to 113, HashMap.get 4706 to 1924,
Integer.valueOf 1346 to 627; an exception thrown and caught 362 to 385, within qemu's noise. What only an ARM64 machine
running Windows can say: that the code lives with Windows' x18 and its exception dispatch (nothing in it names x18 --
its registers are x0-x3, x9-x11, x16, x19-x21, x29 and x30 -- and a fault under it reaches the core's vectored handler,
which needs no unwind information, of which the code's frames have none, as the core's ELF's have none), pages made
executable by VirtualProtect and FlushInstructionCache on a processor with real caches (qemu keeps none to flush), ARM's
weaker ordering of memory between threads (qemu on an x86 PC is stronger), and its speed.

x86-64 (`mg_dvm_jit_x86.h`) compiles all that A64 does. linux64's suites pass at the JIT's own threshold, at 1 and at 0:
7666 checks, and 7820 on Android's libcore, none failed (its bundle tests' 25 known failures alike at each). Desktop
x64's pass every Dalvik check at the three thresholds (7668 checks, and 7822 on libcore); their 23 or 24 failures, the
same with the JIT off, are the guest signals' and the threads' stops, which Easy Anti-Cheat, running on this PC,
disturbs. On this PC's x64 (ns a turn, interpreted then compiled; `tools/dvm-bench.sh x64`): int arithmetic 18.4 to 1.5,
long and double arithmetic 31 to 7.6, iget and iput 30 to 3.7, sget and sput 16 to 1.2, aget and aput 29 to 4.2,
String.charAt 27 to 2.0, String.indexOf 12 to 0.9, Math.min, max, abs 48 to 2.9, invoke-static 28 to 15, HashMap.get 435
to 237, Integer.valueOf 131 to 88; an exception made, thrown and caught 548 to 591, the one item slower, as under
qemu-aarch64 (6389 to 6426).

x86 (the same back end's 32-bit mode) compiles all that Thumb-2 does, a long's division and its conversions to and from
float and double by C. Desktop x86's suites (WOW64) pass every Dalvik check at the JIT's own threshold, at 1 and at 0:
7655 checks, and 7809 on libcore; the failures besides, none of the runtime's, are timing checks, the same with the JIT
off. On this PC (ns a turn, interpreted then compiled; `tools/dvm-bench.sh x86`): int arithmetic 16.3 to 1.5, iget and
iput 27 to 4.7, sget and sput 14 to 1.3, aget and aput 28 to 4.2, String.charAt 23 to 1.9, String.indexOf 9.0 to 0.8,
Math.min, max, abs 43 to 3.5, ArrayList's iterator 104 to 54; but a long's and a double's loop only 25 to 22 (its
long-to-double is a call of C here), and what a loop does by calls of C is slower compiled than interpreted, where x86's
interpreter calls more cheaply than x64's: invoke-static 20.3 to 19.7, a final method 27 to 30, a synchronized static
method 43 to 54, an exception made, thrown and caught 489 to 631, Math.sin 83 to 97, System.arraycopy 22 to 26. Next for
x86: long-to-double in the code (SSE2 can, from the two halves), and a loop whose turns are mostly calls left to the
interpreter there.

Next: what the phone's logs say is handed back most.
