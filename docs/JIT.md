# A JIT for the Dalvik interpreter

The plan for compiling an app's hot bytecode to Thumb-2, written before the code. What is built so far is said at the
end; this file follows the work.

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
  a few Thumb-2 instructions; each it does not becomes an exit: "go on in the interpreter at this pc". A method whose
  every instruction is an exit is not kept.
- **Entries.** The code can be entered at pc 0 and at every branch target (a loop's head): the interpreter, at a jump
  back (where it already looks at `g_dvm.slow`, SAFEPOINT), finds the method's code and jumps in at the target. Entering
  at a loop's head is on-stack replacement for free: the frame is the state.
- **Exits.** An exit stores the pc to go on at and returns. The interpreter reloads `pc` and continues its loop.
- **The calling convention.** `int code(mg_dvm_reg *regs, const void *entry, u32 *pc_out, mg_dvm_thread *t)`: r4 holds
  `regs` throughout, r5 the thread, r6 `pc_out`. Returns 0 (go on at `*pc_out`).
- **Watching.** While `g_dvm.slow` is set (a trace, a step limit, the profile) nothing compiled runs: the watched loop
  never enters code, and compiled code checks the flag at each jump back and exits there.
- **Memory.** Code goes in executable pages of its own (the package has rescap:codeGeneration; the patcher writes code
  the same way), flushed with FlushInstructionCache on Windows and `__builtin___clear_cache` under Linux.
- **Turning it off.** The flag `no-jit` (a file in LocalState\flags) keeps everything interpreted; `trace-jit` says each
  method compiled, its size, and how often each of its exits is taken.

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

Next: what the phone's logs say is handed back most.
