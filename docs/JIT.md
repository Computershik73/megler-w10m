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
interpreted. The benchmark (`tools/dvm-bench.sh arm`) gives the speed under qemu, interpreted and compiled; the
phone's Self-test gives the phone's.

## Where it stands

The first stage is in `core/src/dvm/mg_dvm_jit.c`: moves and constants, int arithmetic and logic in every form (division
and remainder with the divisor checked), the shifts' counts masked, `if-*`, `if-*z` and `goto`, `array-length`, `aget`
and `aput` of ints, bytes, chars, shorts and booleans and `aget-object`, each with the interpreter's checks; `iget` and
`iput` of every width of a field resolved by then (the object checked for null and for having the slot), `sget` and
`sput` of a class initialised by then (the slot's address a constant of the code). A method is
compiled at the 1000th jump back it takes; a loop is entered only when it is compiled whole, from its head to its jump
back: a loop whose body calls out would go in and out at every turn, which in the benchmark made Math.min's and
Integer.valueOf's loops twice as slow. Such loops, and every method with no loop compiled whole, stay the interpreter's.

Under qemu-arm (ns a turn, interpreted then compiled): int arithmetic 131 to 5, aget and aput of an int[256] 228 to 17,
String.indexOf 80 to 3; the items whose loops call out are as they were. The tests (`t.Jit`, mg_dvm_tests.c) run every
compiled instruction and hand-back, and the whole main suite passes with every loop compiled (MEGLER_JIT_THRESHOLD=1).

Next: calls compiled as calls into the interpreter so that loops which call out can be compiled whole, long arithmetic,
`packed-switch`.
