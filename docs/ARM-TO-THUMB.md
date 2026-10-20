# If ARM-mode code cannot run: translating it to Thumb-2

Not written. This is the design to start from if the device probe's `arm-state` line
says a thread does not survive in ARM state (see ARCHITECTURE.md, "ARM mode"). If it
says threads do survive, none of this is needed and ARM libraries run as they are.

On the one phone tried so far (a Snapdragon 820 on build 15254) threads do survive,
and the ARM-mode test library passes there. So this is for another phone, if one
ever says otherwise.

## What has to hold

Guest code is not emulated here; it is the process's own code, with real registers
and a real stack, calling and being called by Thumb code and by the host. A
translation has to keep all of that true:

- every register, flag and stack slot is the guest's own, at every instruction
  boundary, so nothing needs spilling at calls in or out;
- return addresses and function pointers remain things other code can branch to;
- C++ exceptions still unwind: the unwinder looks return addresses up in
  `.ARM.exidx` and compares them with ranges and landing pads in the LSDA, all of
  which are addresses in the original layout.

The last point rules out the usual dynamic translator with a code cache: return
addresses in a cache are not in any unwind table.

## The shape: same size, same place

Thumb-2 was designed to have an equivalent for nearly every ARM instruction, and a
Thumb-2 instruction is at most four bytes, the size of every ARM one. So an ARM
function can be rewritten **in place**: each instruction becomes four bytes of
Thumb-2 at the same address (one 32-bit instruction, or a 16-bit one and a `nop`).
The function keeps its address, its size, and the offset of every instruction in
it. Then:

- a return address pushed by a rewritten `bl` is the original address with the
  Thumb bit set, which is what the unwinder expects of Thumb code (it strips the
  bit for lookups, and libgcc's and LLVM's `_Unwind_SetIP` both carry the frame's
  Thumb bit over to the landing pad);
- LSDA ranges and landing pads are offsets from the function's start: unchanged;
- jump tables of offsets or of branches are unchanged;
- literal pools stay where they are, and stay data.

An instruction with no four-byte equivalent becomes a `b.w` to the library's island,
where it is done in as many instructions as it needs, followed by a `b.w` back to
the next slot. Those are:

| ARM | Thumb-2 |
|---|---|
| conditional execution of a non-branch (`addeq`, `ldrne`, `popgt`) | island: `it` + the instruction, or a branch around it. (A conditional branch is `b<cc>.w` in place.) |
| a PC-relative read or address (`ldr r0, [pc, #n]`, `add r0, pc, r1`) | PC is +8 in ARM and +4 (word-aligned) in Thumb: in place with the offset adjusted where it still fits, island otherwise |
| a register shifted by a register as an operand | island: shift into a scratch, then the operation |
| `rsc` | island: `mvn` + `adc` |
| an immediate that wraps around bit 31 | island: materialise it |
| addressing modes Thumb-2 lacks (negative or scaled register offsets with write-back, `ldm`/`stm` IB and DA) | island |
| SP or PC where Thumb-2 forbids it | island, through a scratch |

VFP and NEON instructions map by a fixed change of the top bits.

A scratch register for an island is whichever register the instruction does not
use, pushed and popped around it.

`b.w` reaches 16 MB. A library whose ARM text is larger than about 32 MB cannot
have every instruction within reach of an island on either side; the far ones need
another answer (an 8-byte long jump that consumes the next slot, where that slot is
provably not a branch target).

## Pointers to rewritten code need the Thumb bit

A pointer to a rewritten function with its low bit clear, when called, enters ARM
state at an address that now holds Thumb code. Every source of such pointers has to
set the bit:

- dynamic symbols: the loader hands them out, so it sets it;
- relocations whose target is a rewritten instruction (`R_ARM_RELATIVE`, `ABS32`,
  `GLOB_DAT`, `JUMP_SLOT`: vtables, callback tables): the loader sets it;
- addresses computed in code (`adr`, or `ldr r0, =label - . - 8` then
  `add r0, pc, r0`): the rewriter resolves the constant, and when the target is the
  start of a rewritten function, makes the result odd.

What is left is a function whose address is taken only in a way the rewriter could
not follow. For those, the first four bytes of each function can be chosen so that
they are an UNDEFINED instruction when decoded as ARM (the same trick as the PLT
entries, the other way round), and the fault handler resumes at the same address in
Thumb state.

## Knowing code from data

Rewriting in place needs to know, for every word of an ARM function, whether it is
an instruction or data. Stripped libraries carry no `$a`/`$t`/`$d` mapping symbols.
What they do carry:

- `.ARM.exidx`: the start of every function that has unwind information, in order;
- the mode of every function in `.dynsym` and of every relocation target (the low
  bit);
- direct calls: `bl` from ARM code goes to ARM code, `blx` to Thumb, and the other
  way round from Thumb.

From those, recursive descent through each ARM function finds its instructions;
words it never reaches that a PC-relative load refers to are literals. The hard
case is a computed jump (`add pc, pc, r0, lsl #2` and the tables after it), which
needs the compiler's idioms recognised to find its targets and its table.

The asymmetry to respect: calling data code corrupts the data silently, while
missing some code only leaves it in ARM state. So only what descent proves to be
code is rewritten, and the rest is reported.

## Testing it

Under qemu-arm an ARM function runs natively, so the test for the translator is
differential: run each function of a real ARM library (the OpenSSL build already
used by the loader's tests has 3885) natively and translated, on the same inputs,
and compare results and memory. `mg_tests --load` is the place to hang it.
