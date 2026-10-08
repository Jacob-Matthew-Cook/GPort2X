# GPort2X architecture

GPort2X runs a user's own GP2X software, read-only, on a modern host. The
behaviour it implements is specified in the project's harness specification
(`HARNESS_SPEC.md`, referenced below as "spec N"); this document is the
design that implements it. Nothing here is derived from any game, decompilation
or firmware code (see `LEGAL.md`).

## 1. Two engines, one environment

The guest is ARM OABI user-mode code (spec 1.1, 1.2). It needs an execution
engine plus an environment (syscalls, files, devices, hardware registers). The
environment is written once; the engine is pluggable:

| engine | runs where | how guest code executes | status |
|---|---|---|---|
| **interp** | any host (x86-64 dev box, aarch64 handheld) | an ARMv4T interpreter with exact ARMv4 semantics (spec 5): rotated unaligned loads, `STR pc` = pc+12, SWP, undefined-instruction hooks | milestone 1 |
| **native** | armhf build on an aarch64 kernel with COMPAT (spec 1.3, 8.3) | the CPU runs the guest directly; `svc` is caught by a seccomp instruction-pointer filter and SIGSYS, SWP/FPA by SIGILL | milestone 3 (`docs/NATIVE_ENGINE.md`) |

Both engines deliver the same events to the environment:

| event | interp | native |
|---|---|---|
| `svc #imm` | interpreter hook | SIGSYS from the seccomp filter |
| undefined instruction (SWP on ARMv8, FPA) | decoder hook | SIGILL |
| data / prefetch abort | gmem fault | SIGSEGV |
| MMIO access to the register file (TCOUNT, GPIO, test modes) | gmem MMIO page | PROT_NONE page + SIGSEGV (spec 8.2) |
| flip | cacheflush `svc 0x9F0002` from the environment | same |

## 2. Modules and layering

Dependencies point downward only. Each module has one public header under
`include/gport2x/` and its code under `src/<module>/`.

| layer | module | header | responsibility | spec |
|---|---|---|---|---|
| 0 | `types` | `types.h` | guest address types, layout constants, error codes | 1.4, 1.5 |
| 0 | `log` | `log.h` | leveled logging, syscall/register trace switches | 8, #23 |
| 1 | `gmem` | `gmem.h` | guest address space: pages, protections, refcounted physical objects (the 32 MB upper bank, the 64 KB register file), MMIO pages, 2.4-style free-area search | 1.4, 1.5, 3.2 |
| 1 | `host` | `host.h` | host backends: display (PPM dumps / SDL2), audio (WAV / SDL2), input (scripted / SDL2), clock (deterministic / real) | 4.4, 3.4, 8.2 |
| 1 | `card` | `card.h` | the card: FAT16 image reader (read-only) or a directory, with genuine vfat semantics: sizes, FAT mtimes, readdir order, statfs geometry | 6.2, 6.4, #8 |
| 2 | `elf` | `elf.h` | ELF32 ARM loader: ET_EXEC/ET_DYN, PT_LOAD at fixed VAs, brk, initial stack with argv/envp/auxv | 1.1, 1.4, #1 |
| 2 | `cpu` | `cpu.h` | the ARMv4T interpreter (one `cpu_t` per guest thread), hooks for svc/undef/abort | 1.2, 5 |
| 2 | `dev` | `dev.h` | `/dev/mem` physical model, MMSP2 register file (TCOUNT, GPIO, DPC, MLC, latched memory), fb0/fb1, dsp, mixer, mmuhack, GPIO/batt | 3, 4 |
| 2 | `fs` | `fs.h` | the guest path namespace: firmware root RO, card RO + copy-on-write overlay, tmpfs, `/proc` subset; the guest fd table | 6.4, 7.5, #7, #22 |
| 3 | `sys` | `sys.h` | OABI/EABI syscall decode, argument marshalling, struct translation (OABI stat64, old mmap, time, getrlimit, sigaction), dispatch to fs/dev/proc | 2 |
| 3 | `proc` | `proc.h` | guest processes and LinuxThreads threads (interp: a scheduler over `cpu_t`s; native: real clone), fork/exec re-exec, signals | 1.3, 1.4, #4, #5, #17 |
| 4 | `engine` | `engine.h` | the engine interface and the two implementations | 1.2, 1.3 |
| 5 | `main` | — | the `gport2x` command line, configuration, test modes | #23 |

## 3. Guest memory model (interp)

`gmem` is a sparse two-level page table over the 4 GB guest space (4 KB
pages). A page is unmapped, RAM or MMIO. RAM pages point into a refcounted
**object**; one object can be mapped at many guest addresses, which is how the
upper bank's aliases (the whole bank, the nine video windows, fb0, fb1; spec
3.2) stay coherent. MMIO pages call device callbacks with the offset inside
the object, which is how TCOUNT, GPIO and the test modes are implemented
without a background thread (spec 4.1 describes the native mechanism; the
interpreter sees every access directly).

The interpreter keeps a per-`cpu_t` page cache over this table (a software
TLB: direct-mapped arrays of guest page -> host pointer for reads, writes and
fetches, RAM pages only). `gmem` stamps every space with a global generation
that changes on any map, unmap or protection change, and `cpu_run`
revalidates the cache against it on entry; mappings only change between runs,
in the syscall layer, so nothing inside a run can go stale. MMIO pages are
never cached, which keeps the register file's side effects (TCOUNT, the flip
on a 0x2914 write) on the checked path. On x86-64 a JIT (docs/JIT.md) compiles
guest blocks on top of the same caches; it tracks translated words per page
in gmem so stores to code invalidate exactly the affected blocks.

The guest layout is the 2.4 shape of spec 1.4: ELF at 0x8000, brk after
`.bss`, anonymous mmaps bottom-up from 0x40000000, the initial stack just
below 0xC0000000, LinuxThreads stacks at the hints the guest asks for.
0x05000000 and 0x06000000 stay unmapped on purpose (spec 1.4).

The native engine maps the same addresses with host `mmap(MAP_FIXED)`, using
memfd objects for the two physical ranges (spec 1.5).

### 3.1 The flip event and the clock

The display event used for frame counting and dumps is the guest's write of
the RGB layer's even-field scanout address (+0x2912 low half, then +0x2914
high half) once the frame clock (TCOUNT) has been read. That is exactly the
`cacheflush`-then-program sequence of the game's flip routine; it excludes
the presentation of the cleared pages during hardware init and the overlay
flush of the loading screen, so the numbering matches the native oracle's
(spec 8.2). The cacheflush trap is still reported in the trace.

Clock modes (`gpdev_config.clock_mode`): `REAL` follows CLOCK_MONOTONIC and
`nanosleep` sleeps; `STEP` advances TCOUNT by a fixed step (7,680 counts =
32 game ticks) per 32-bit read and `nanosleep` returns at once; `MAINSTEP`
counts only the main thread's reads, which keeps the main thread's tick
sequence identical to the oracle (which runs without the audio thread) while
the real LinuxThreads audio thread still sees time pass. Virtual time, which
paces the OSS device's drain, derives from TCOUNT in the step modes; when
every task waits, the scheduler lets virtual time pass (the "idle" advance).

## 4. Process model

* **interp**: one `cpu_t` per guest thread, all sharing one `gmem`. `clone`
  with `CLONE_VM` creates a thread; the scheduler switches threads at
  syscalls and after a quantum of instructions, so LinuxThreads' manager and
  the audio feed thread run as on hardware. `fork` copies the address space
  (copy-on-write at the object level is milestone 2; milestone 1 copies).
  `execve` loads the new ELF into a fresh `gmem` in the same harness process.
* **native**: real `clone`/`fork`/`execve` (the harness re-execs itself for a
  guest exec; `#!` and `-ENOEXEC` handling per spec 1.3 and 7.3).

## 5. Verification (executable checks only)

* `gmem`, `card`, `elf`, `dev`: unit tests with exact expected values from the
  spec (genuine geometry, layouts, offsets).
* `cpu`: differential tests against Unicorn (same registers, memory and code
  → same final state), plus hand-written cases for ARMv4-only semantics that
  Unicorn's ARMv5 model may not share (rotated loads, `STR pc` offset).
* `sys`: guest test programs (assembled with `arm-linux-gnueabi-as`) that must
  print the same output under `qemu-arm` and under GPort2X.
* whole game: flip-aligned frame dumps compared with the native oracle
  (spec 8.2); dumps use the oracle's conventions (`frame_%05d.ppm`, P6
  320×240) so the existing comparison tools work unchanged.
* deterministic test modes (spec #23): TCOUNT +32 game ticks (7,680 raw
  counts) per read, `time()` = 0, virtual-time sleeps, syscall and
  register-access traces.

## 6. Milestones

1. **Boot the game under interp on x86** (done 2026-10-07): every B service
   in spec 9; the genuine launch chain (`Payback.gpe` → busybox `sh` → the
   stub → tmpfs → decompression → `exec`) reaches the main menu, and flips
   0..300 are byte-identical to the oracle (`tests/game/`).
2. Gameplay fidelity: replays and missions identical to the oracle (spec 8.4),
   audio (A services), RAM budget check.
3. Native engine on an aarch64 VM (`docs/NATIVE_ENGINE.md`).
4. Firmware menu chain with FPA emulation (M services; spec 7). In progress
   (2026-10-07): `rc.sysinit` boots under the harness (busybox `mount`,
   `devfsd`, `mknod`, `hostname`, `irqbattery`), the stock `gp2xmenu`
   renders through SDL fbcon (P1), a host-time pad script drives it
   (`--pad-time`; P2: the explorer lists the card root), and its autorun
   launches the game through the stub (init → menu → stub → game). Open:
   launching a `.gpe` from the explorer, the battery/TV-out devices, FPA
   arithmetic fidelity (long double, not NWFPE's SoftFloat).
