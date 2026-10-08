# JIT plan for the interpreter engine

Status: two backends, x86-64 (src/cpu/jit_x86_64.inc) and AArch64
(src/cpu/jit_aarch64.inc), sharing blocks, lookup and invalidation
(jit_common.inc, jit_dispatch.inc); on by default on Linux for both, and
`--no-jit` or `GPORT2X_JIT=0` selects the interpreter. The JIT exists for hosts where the
interpreter is too slow and the native engine (docs/NATIVE_ENGINE.md) cannot
run, for example Arm cores without 32-bit execution or kernels without
COMPAT.

## Where the interpreter stands

| step | 600 boot flips (i7-9700F) |
|---|---|
| original interpreter | 13.3 s |
| page cache over gmem (software TLB) | 8.0 s |
| decoded-instruction cache with specialised handlers | 5.9 s |
| JIT, call-threaded (every instruction a direct handler call, ALU inline) | 4.5 s |
| + inline memory access through the page cache, inline NZCV | 3.7 s |
| + same-page block chaining | 3.2 s |
| + word-level code marks (no refusal of pages mixing code and data) | 2.7 s |
| + inline LDM/STM, shifted ALU, halfword and writeback transfers | 1.4 s |

Every step was checked frame by frame against the previous build (600
identical frames), by the Unicorn differential test run under both engines
with identical instruction counts required, and by the full game-gate suite.

The decoded cache (src/cpu/cpu.c, `struct dpage`) keys each entry by the
instruction word and compares the word in memory on every execution, so
rewritten, loaded or remapped code can never run stale. A JIT cannot afford
that compare, which is why most of this plan is about invalidation.

## What robust JITs do, and what we take from each

* **QEMU TCG** ([Translator Internals](https://www.qemu.org/docs/master/devel/tcg.html),
  [docs/devel/tcg.rst](https://gitlab.com/qemu-project/qemu/blob/master/docs/devel/tcg.rst)):
  translation blocks are looked up by guest pc and CPU state; direct branches
  are chained by patching a jump slot after the first execution; a chained
  jump never crosses a guest page, because the mapping of the destination can
  change; user-mode QEMU write-protects every host page it has translated
  code from and, on the SEGV from a guest write, invalidates all blocks of
  that page; precise state on an exception comes from a host-pc to guest-pc
  map.
  *Taken:* block chaining only within one guest page; per-page block lists
  for invalidation; a host-pc to guest-pc map for aborts.
* **dynarmic** ([repository](https://github.com/lioncash/dynarmic),
  [overview](https://deepwiki.com/lioncash/dynarmic)), the A32/A64 JIT used by
  console emulators: decode, translate to an SSA IR, run optimisation passes,
  emit host code. Blocks end in terminals: `LinkBlock` jumps to the next
  block only if enough cycles remain, otherwise it returns to the dispatcher;
  `CallSupervisor` leaves for svc. Memory accesses are callbacks, with fastmem
  (direct host access, faults handled) falling back to page tables, then to
  callbacks. Invalidation is explicit: the embedder tells the JIT which ranges
  changed.
  *Taken:* the cycle (instruction budget) check at block links, svc and
  undefined instructions as block terminals, and the three-level memory path
  (our software TLB is the page-table level; MMIO is always a callback).
* **box86 / box64** ([dynarec overview](https://deepwiki.com/ptitSeb/box86/4-dynamic-recompiler-(dynarec)),
  [usage and tuning switches](https://github.com/ptitSeb/box86/blob/master/docs/USAGE.md)):
  "dynablocks" of guest code translated to Arm, with an interpreter for
  anything the dynarec does not handle and per-feature switches for
  correctness versus speed.
  *Taken:* the interpreter stays the reference and the fallback for every
  instruction the JIT does not cover (FPA, SWP, LDM with pc, MSR, ...), and a
  run-time switch selects the interpreter for comparison runs.

## Design (as built for x86-64)

1. **Blocks.** A block is a run of guest instructions in one page, ending at a
   branch, a write to pc, svc, an undefined or FPA instruction, or the page
   end. Key: guest pc plus the gmem generation stamp.
2. **Code generation.** One backend per host: AArch64 first (the handheld
   target), x86-64 second (the development host). Guest registers live in the
   `cpu_t` register file, loaded into host registers per block; NZCV is kept
   in a host register and written back at block exits.
3. **Memory.** Every load and store probes the software TLB inline and calls
   the existing `cpu_ld*`/`cpu_st*` helpers on a miss, so MMIO side effects
   (TCOUNT, the 0x2914 flip write) stay exact.
4. **Invalidation (the hard part).**
   * gmem records the translated words of each page in a bitmap on the
     page's object. Marked pages are never put in a write cache, so every
     guest store to them takes the checked path; a store that touches a
     translated word drops all blocks of the page and unmarks it, a store to
     the page's data words only is let through. This is QEMU's write-protect
     scheme with its per-page code bitmap, done in software. The game's RWX
     segment mixes hot code and written data in the same pages, which made
     the bitmap necessary (without it two hot pages were refused).
   * A page whose code is rewritten more than eight times is left to the
     interpreter (its decoded cache re-checks every word anyway).
   * Releasing an object drops its blocks, so reused host memory never
     matches an old block; blocks are keyed by guest pc and host address.
   * The buffer (32 MB) is flushed whole when full, between blocks.
   * A store that invalidated anything ends the running block at once.
5. **Exits and precise state.** Before every handler call the block stores
   the guest pc, next_pc and r15 as the interpreter would have them, so an
   abort, an svc or a handler that reads r15 sees the interpreter's state.
   Blocks return the number of instructions retired. A linked entry checks
   the budget left (r12d) against the block length and returns to C if it is
   short, so instruction counts are exact and the scheduler's quantum, the
   clock modes and `--insns` behave identically to the interpreter.
6. **Chaining.** Only within a guest page (QEMU's rule), patched lazily after
   the first exit (QEMU's goto_tb), with the budget check at every link
   (dynarmic's LinkBlock). Linked blocks share a page, so they are always
   invalidated together and no unlinking is needed.
7. **Verification.** The Unicorn differential test runs every case under
   both engines and requires identical state and instruction counts;
   test_cpu runs a loop in budgets of 1, 5 and 9 instructions under both;
   test_gmem covers the code marks; every game gate in `tests/game` runs
   with the JIT, and the boot and mission 2 gates also without it.

## Gameplay, and two fixes it needed

The boot figures above hid a problem that STORY mission 2 exposed: the
32 MB code buffer filled and was flushed 750 times in 3,400 flips.

* **Budget tails.** When a block was longer than the budget left in a
  scheduler slice, the loop interpreted one instruction and then looked up a
  block at the next address, so over many slices nearly every hot
  instruction became the start of its own block. Now the rest of such a
  slice (at most one block length out of 20,000 instructions) is
  interpreted without creating blocks.
* **Overlapping blocks.** A branch into the middle of a block starts a new
  block that duplicates the rest. Blocks now end at every branch, as QEMU's
  translation blocks do, with both successors chained, and are at most 16
  instructions long; the game's working set (about 31,000 blocks) then fits
  the buffer with no flush at all.

Mission 2, 3,400 flips (intro, menus, about 1,100 flips of play, about 6.4 G
guest instructions), on the i7-9700F:

| build | time | guest instructions/s |
|---|---|---|
| interpreter | 54.9 s | about 120 M |
| JIT before the two fixes | 39.9 s | about 160 M |
| JIT now | 15.9 s | about 400 M |

The game runs at 30 frames per second and gameplay costs about 1.9 M guest
instructions per frame, so real time needs about 57 M instructions/s: the
x86-64 JIT has about seven times that.

## Tiered compilation

The firmware chain starts dozens of short-lived processes (rc scripts, sh,
rm, the menu), each with private copies of its code, and compiling all of
that cost more than interpreting it. Two rules keep compilation for hot
code only:

* a block is compiled after its start has been reached 16 times
  (`GPORT2X_JIT_HOT` overrides; the CPU tests use 0 to compile at once);
* the run loop asks for a block only where one can start: at the start of
  a run, after a branch, or after another block.

On the menu workload (60 M instructions of rc.sysinit and gp2xmenu, step
clock) under qemu-aarch64 this took the JIT from 13.0 s to 5.3 s (4.5 s
interpreted; under qemu every compiled block is self-modifying code, which
real hardware does not penalise this way); gameplay speed is unchanged.
The JIT and the interpreter produce identical syscall traces on that
workload on both hosts (only `time()` results differ, being wall-clock).

## The AArch64 backend

Same structure as the x86-64 one. x19 holds the cpu, w20 and w21 the budget
left and the instructions retired; guest NZCV equals AArch64 NZCV (SUBS sets
C as "no borrow", like ARM), so flag-setting operations copy the host flags
with `mrs x3, nzcv`; shifted-register operands and register-offset loads and
stores make the shifted ALU forms and the page-cache access one instruction
each. The instruction cache is cleared after each block is emitted and after
each link is patched.

Measured under qemu-aarch64 on the development host (only the ratio means
anything there, and it predates the two fixes above): 600 boot flips in
12.1 s with the JIT against 44.6 s interpreted, frames identical. The CPU tests, the Unicorn differential test
(both engines, case by case) and the game gates pass on the aarch64 build.
Not yet measured on real Arm hardware.

## Register caching (AArch64)

Up to seven guest registers per block (chosen by how often the block reads
them) live in x22..x28, loaded on first read and written through to the
register file, so exits, links and handlers need no write-back. After a
handler call the registers already loaded are reloaded; after a
conditionally executed instruction the loaded set is restored to its state
before it. Counted in the generated code over a 600-flip boot under
qemu-aarch64: of 834 M guest-register reads, 490 M came from host registers;
loads executed fell from about 1.2 to 0.6 per guest instruction (including
74 M reloads after handler calls). The gain in time depends on the core
(an in-order A35 stalls on each load-use); it is not measurable under QEMU.

## RK3326 (Cortex-A35, 1.2 GHz): will it run in real time?

Measured with `--flip-log` on the real clock (RAMPAGE replay, x86-64, no
underruns): replay gameplay needs 67 M guest instructions/s on average and
83 M/s in its busiest second; the intro needs 43 M/s with a 173 M/s peak
while it loads. Per flip (test clock, 47,260 gameplay flips over the six
missions and seven replays): mean 2.7 M, 99th percentile 4.6 M, 99.9th
7.9 M; loading spikes reach 87 M. The x86-64 JIT reaches about 400 M/s on an
i7-9700F. A Cortex-A35 is a small in-order core; public single-thread
benchmarks put an RK3326 roughly 13 to 16 times below this desktop at
1.2 GHz, so the JIT can be expected at very roughly 25 to 30 M/s there
(somewhat more with register caching), well under the 67 M/s gameplay
needs. That is an estimate, not a measurement; it needs
checking on the device.

The native engine (docs/NATIVE_ENGINE.md) is the route to real time on
RK3326: the A35 executes 32-bit ARM code itself (the RK3326 distributions
run 32-bit emulator cores, so their kernels support compat processes), and
the game, written for a 200 MHz ARM920T, runs on a 1.2 GHz core with traps
only at syscalls and register-file accesses. The JIT remains the fallback
for cores without 32-bit execution, and its next step, keeping guest
registers in host registers within a block, matters most on in-order
cores like the A35.

## Next

1. The native engine; then run both engines on an RK3326 and measure.
2. Keep guest registers in host registers within a block, and inline the
   remaining flag-setting forms (MOVS, ANDS, shifted compares).
