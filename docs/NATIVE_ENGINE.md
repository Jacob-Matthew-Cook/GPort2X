# The native engine (milestone 3)

**Status (2026-10-08):** implemented. It runs Payback on an Anbernic
RG353M (RK3566, ROCKNIX) from the PortMaster-style port at the game's full
pace (25.4 fps, 39.4 ms frames set by the 2.4 timer ticks), with sound,
through the genuine launch chain, the intro, the menus and gameplay; the
interpreter with its JIT reached 8 fps there. `make test-native` checks it
on an arm64 machine (section 8). Not yet: the deterministic test modes (a
`PROT_NONE` register page), so frames are compared with the oracle only
where the game's frames do not depend on timing.

The interpreter engine runs the guest's ARM code in software. The native
engine runs it on the host CPU: an **armhf build** of GPort2X on an AArch64
kernel with 32-bit compat support (spec 1.3, 8.3), where the guest's
instructions execute directly and only the events the environment must see
are trapped. Everything above the engine (`fs`, `dev`, `sys`, the process
model's policy) is shared with the interpreter; what changes is how guest
code runs, how memory is mapped and how events arrive.

## As built

Sections 1-7 below are the plan the engine was built from. Where the result
differs:

* **Address space.** The harness is an armhf binary linked at 0xE0000000,
  above the guest range: static for tests, dynamic with SDL2 for handhelds.
  `native_prepare` reserves every free gap of [0x8000, 0xC0000000) and
  records what is already mapped there (the SDL build's dynamic loader and
  libraries land inside the range) as forbidden to the guest. gmem's native
  mode creates real mappings at guest addresses, with memfd objects for the
  shared ones and a per-page owner table, so a forked child's `exec` never
  unmaps what the new image just mapped.
* **Syscalls.** The seccomp filter traps exactly the calls whose instruction
  pointer is in [0x8000, 0xC0000000) (the low 32 bits are compared), so the
  harness and its libraries run unfiltered and a host program it started
  itself could trap: GPort2X never runs a host shell after the filter is in
  (scratch removal uses `nftw`). SIGSYS hands the saved registers to the
  shared `sys` layer.
* **SWP, FPA.** SIGILL and SIGBUS go to `aemu`, which emulates one
  instruction: SWP as a host atomic exchange, FPA and the rest through the
  interpreter on host memory.
* **Threads.** Guest threads (LinuxThreads' `clone(CLONE_VM)`) are host
  threads, with the `sys` layer's task model kept as the reference: one big
  lock (robust, process-shared, in a shared mapping) serialises the syscall
  layer; a blocking call parks the task and waits outside the lock, polling
  every 0.5 ms; SIGRTMIN+4 interrupts a thread for a signal.
* **Processes.** `fork`, `vfork` and `clone` without `CLONE_VM` are host
  forks. A child gets `PR_SET_PDEATHSIG` and stays in GPort2X's process
  group. `wait4`, `waitpid`, `kill` and `getppid` go to the host, and guest
  pids are host pids. Pipes are host pipes, made non-blocking on the host so
  that an empty pipe parks the reader outside the lock while the writer runs.
  `execve` stays in-process: the image is replaced, the harness state kept.
* **Devices.** The device model (`gpdev`) lives in a shared mapping, so every
  guest process sees one DSP ring, one mixer and one register file. Only
  GPort2X's own process (the front end) drains the DAC and talks to SDL. The
  register file is a memfd, which a clock thread updates every 400 µs
  (TCOUNT, the PLL status bit). A flip is a change of the scanout address
  seen by that thread on its 4 ms device tick.
* **Front end.** SDL runs on a thread of its own that never takes the lock (a
  display update may block behind a hidden window), and pulls the audio
  straight from the DSP ring in its callback.
* **Ending.** A stop (the front end's quit combination, or `--flips`) and the
  exit of the first guest process both SIGKILL every other process of the
  group, print the interpreter's report (flips, audio) and remove the
  temporary scratch directory. The exit codes are the interpreter's: 0 at the
  flip limit, 125 on a stop request, the guest's own status otherwise.
  SIGTERM and the other host signals go to the guest, as on the console, so
  a supervisor must use SIGKILL.
* **Flip numbering.** The native engine counts every scanout change from
  start-up. The interpreter's flip rule ignores scanout writes until the
  game first reads TCOUNT, a read the native engine cannot see. So init's
  presentation of the cleared fb1 page is native flip 0, and for Payback,
  native flip k+1 is the interpreter's (and the oracle's) flip k.
* **Unaligned accesses** are left to the compat kernel: the OPEN-13 census
  found none in the game, the menu or the firmware userland.

## 1. What the host must provide

| requirement | why | check at start-up |
|---|---|---|
| AArch64 kernel with `CONFIG_COMPAT` | runs the armhf harness and the OABI guest | `uname -m` is `aarch64`, the armhf binary starts |
| `vm.mmap_min_addr` ≤ 0x8000 | the game's first PT_LOAD is at 0x8000 (spec 1.4) | read `/proc/sys/vm/mmap_min_addr`; refuse with a message otherwise |
| seccomp filters (`SECCOMP_SET_MODE_FILTER`) with `SECCOMP_RET_TRAP` | the syscall trap | `prctl(PR_GET_SECCOMP)` probe |
| `memfd_create` | the shared physical objects | probe |
| personality `ADDR_COMPAT_LAYOUT` / `ADDR_NO_RANDOMIZE` | the 2.4 bottom-up layout (spec 1.4) | `personality()` |

The CI stand-in is `qemu-system-aarch64 -cpu cortex-a53` with a distro
kernel; qemu-user cannot host the engine (no seccomp for the guest, no
LinuxThreads clones).

## 2. Address space

The harness process *is* the guest process. Its own code, data and stack
must stay out of the guest's view: the armhf harness is linked to load at
0xC0000000 or above is impossible (user space ends there), so the harness
is built position-independent and relocated by the loader to the top of
the compat address space (`ADDR_COMPAT_LAYOUT` puts the stack and libraries
high); at start-up it records every mapping of its own from `/proc/self/maps`
and never lets the guest map over them. The guest layout of spec 1.4 is
then built with `mmap(MAP_FIXED)`:

* PT_LOAD segments at their virtual addresses, with `PROT_READ|PROT_WRITE|
  PROT_EXEC` for the first (the game stores into its own text);
* the brk area after `.bss`, grown with `mmap(MAP_FIXED)` on `brk`;
* the initial stack just below 0xC0000000, grown on SIGSEGV below it;
* anonymous mappings bottom-up from 0x40000000 (the engine keeps its own
  free-range search, as `gmem_find_free` does, because the host's allocator
  would not give the 2.4 shape);
* the two physical objects as `memfd`s: 32 MB for the upper bank and 64 KB
  for the register file, mapped `MAP_SHARED` for every `/dev/mem`, fb0 and
  fb1 window so the aliases stay coherent and `changeclock` sees the same
  registers from its own process (spec 1.5).

`gmem` becomes a thin bookkeeping layer over the host mappings (what is
mapped where, with which protection and object), used by `fs_file_mmap`,
`/proc/self/maps` and the fault handler; the interpreter's page table is
not used.

## 3. Events

| event | mechanism | handler |
|---|---|---|
| `svc` (OABI `0x900000+N`, ARM-private `0x9F000N`, EABI `svc 0`) | a seccomp filter that allows the harness's own syscalls (those made from the harness's code range, identified by the instruction pointer) and returns `SECCOMP_RET_TRAP` for everything else; the kernel delivers SIGSYS with the register state | `sys_dispatch` on the saved `ucontext` registers; the result is written to r0 before `sigreturn`. The OABI immediate is read from the instruction word at `pc - 4` (the filter cannot see it) |
| SWP/SWPB (undefined on AArch64 compat without `CONFIG_SWP_EMULATE`) | SIGILL | decode the instruction, perform the atomic exchange with `ldrex/strex` loops, advance pc (spec 5.2) |
| FPA instructions (firmware userland) | SIGILL | the interpreter's FPA code, operating on the saved registers and an FPA register file kept per thread |
| data abort | SIGSEGV | stack growth (spec 1.4); MMIO emulation for the register pages mapped `PROT_NONE` in the test modes (spec 8.2); otherwise the guest's own SIGSEGV |
| unaligned access | the compat kernel's alignment handling | nothing: `/proc/cpu/alignment` is set to the rotate-load behaviour of the GP2X kernel at start-up (OPEN-14), or the accesses are fixed up in the SIGBUS handler with ARMv4 semantics |
| flip | `svc 0x9F0002` through the SIGSYS path, then the scanout register write seen by the MMIO handler of the register page, or by the display thread sampling the shared register file | as the interpreter: `gpdev_cacheflush`, the scanout rule |
| TCOUNT, GPIO | at normal speed the register page is plain shared memory: a harness thread rewrites TCOUNT every 250..500 µs and the GPIO words on input (spec 4.1); in the test modes the first register page is `PROT_NONE` and every access is emulated on SIGSEGV | `gpdev_reg_read/write` |

The signal handlers run on an alternate stack (`sigaltstack`) that lives in
the harness's reserved range, so they never touch guest memory.

## 4. Processes and threads

The guest's tasks are real tasks:

* `clone(0xF00)` is passed to the kernel after the child's SIGSYS/SIGILL
  handlers are inherited (they are: `CLONE_SIGHAND`); each new task installs
  its own alternate signal stack and FPA state on first use;
* `fork` is a real fork (the shared `memfd` objects stay shared, private
  mappings are copied on write by the kernel for free);
* `execve` re-executes the harness binary with the guest program as its
  argument and the environment state (mount table, open descriptors that
  are not close-on-exec, the virtual pid namespace) handed over through an
  inherited descriptor; the `#!` and `-ENOEXEC` rules are the same code as
  the interpreter's;
* `wait4`, `kill`, `getpid` and the LinuxThreads signals 32..34 go to the
  kernel. Guest pids are host pids inside a pid namespace when the host
  allows one (`CLONE_NEWPID`), so the guest sees small, stable numbers;
  otherwise the host's.

Signals the guest installs are kept by the harness: `rt_sigaction` records
the guest handler, and the harness installs its own trampoline handler for
that signal; on delivery it builds the guest frame (the same code as the
interpreter's `setup_frame`) on the guest stack and returns into the
guest handler; the guest's restorer or the vectors-page trampoline reaches
`sigreturn` through the SIGSYS path, which restores from the saved frame.
SIGSYS, SIGILL, SIGSEGV and SIGBUS are never deliverable to the guest's own
handlers except as the faults the guest would have seen (a SIGSEGV that the
engine cannot fix up becomes the guest's SIGSEGV).

## 5. The syscall filter

The filter is built once per exec:

```
if (instruction_pointer is inside the harness's own text)   ALLOW
if (nr is one the harness never uses from guest context)   TRAP
TRAP
```

Everything the guest does goes through the trap, including calls the
compat kernel could serve directly (the trap costs about 1 µs; the game
makes ~200 syscalls per frame at most, which is negligible). The harness's
own calls from the handlers are allowed by the instruction-pointer rule.

## 6. What is shared with the interpreter and what is new

| shared | new for native |
|---|---|
| `card`, `fs`, `dev` (the model, the devices), `sys` (decode, marshalling), the signal frame layout, the process policy (pids, wait, exit status) | the mapping layer (MAP_FIXED, memfd), the seccomp filter, the SIGSYS/SIGILL/SIGSEGV handlers, the TCOUNT/GPIO writer thread, the SWP emulation, the exec hand-over, the per-task alternate stacks |

`cpu` is not used; `gmem` is replaced by the mapping layer behind the same
API where `fs`/`sys` need it (`gmem_read/write` become plain memcpy with
fault catching).

## 7. Order of work

1. `make armhf` builds (the code is portable already; `-m32`-style flags
   come from the cross compiler).
2. An aarch64 VM with COMPAT (the CI stand-in) that runs the interpreter
   build first (`tests/sys`, the game boot), so the environment is proven
   before the engine changes.
3. The mapping layer and the ELF load with MAP_FIXED; run the guest test
   programs with the seccomp trap and no SWP/FPA.
4. SWP on SIGILL; LinuxThreads (the game boots).
5. FPA on SIGILL (the firmware menu).
6. The test modes (PROT_NONE register page) and the same regression gates
   as the interpreter: flips 0..300 and mission 2 identical to the oracle.

## 8. Tests

`make test-native` builds `armhf-native` and runs the unit tests, `tests/sys`
and `tests/native` on it. It needs an AArch64 kernel with 32-bit compat and
`arm-linux-gnueabihf-gcc`. `tools/test_native_remote.sh` syncs the tree to
such a machine over ssh and runs it there, with that machine's
`~/.gport2x-test-env` giving the paths of the user's images. The machine can
be an emulated arm64 VM (`qemu-system-aarch64 -M virt -cpu max`, Ubuntu
24.04 arm64). There, each game run takes about a minute.

| test | checks |
|---|---|
| `tests/sys/test_sys.py` | the guest syscall programs (files, memory, processes; EABI and OABI) under both engines, byte for byte against the oracle: qemu-arm, or the host kernel itself where it runs ARM programs |
| `tests/native/test_native_game.py` | the injected game to 302 flips and the genuine chain (shell, stub, tmpfs, exec, LinuxThreads, changeclock) to 61 flips: native flip k+1 identical to the oracle's flip k, exit 0 at the flip limit with the report, audio reaching the DAC (in the chain from the game's own process), no process of the group left, the scratch directory removed |
| `tests/native/test_native_menu.py` | the firmware menu (its boot script's pipes, FPA in its libraries) renders, a timed pad script launches Payback.gpe through the `execvp` fallback and the stub, and the game flips to the limit |

