# GPort2X

A GP2X user-mode compatibility harness: it runs a user's own GP2X software
(a firmware dump, a card image and the programs on it) read-only on a modern
host, by reproducing the Linux 2.4 / MMSP2 user-space environment of the
console. It is implemented from a behavioural specification and public
documentation only (see `LEGAL.md`).

Status (2026-10-07): the interpreter engine boots the game on the card from
the genuine launch chain (`Payback.gpe` → busybox `sh` → the launcher stub,
which mounts a tmpfs, decompresses and `exec`s the game), through LinuxThreads,
the copy-protection checks and the intro to the main menu. In the deterministic
test mode the first 301 flips are byte-identical to the project's native x86
oracle (`tests/game/test_boot.py`, `test_chain.py`), as are STORY mission 2's
menu navigation, start and first ~1,100 flips of play (`test_mission2.py`).
The firmware itself boots too: `/etc/rc.d/rc.sysinit` runs its busybox
commands, `irqbattery` and the stock `gp2xmenu`, which renders through SDL's
fbcon driver, navigates under a timed pad script and launches the game from
its Game section (`test_menu.py`); QUIT returns to the menu (`test_quit.py`).
The native engine (`--engine native`, an armhf build on an AArch64 kernel
with 32-bit compat support: `docs/NATIVE_ENGINE.md`) runs the game's own
ARM code on the host CPU and plays Payback at the console's full pace on an
Anbernic RG353M (RK3566, ROCKNIX), with sound, from a PortMaster-style port
(`port/`, `tools/make_port.sh`); it runs the same chains, the firmware menu
included (`make test-native`). `docs/STATUS.md` has the state against every
section of the specification.

## Build and test

```sh
make            # host build: build/host/gport2x, libgport2x.a, tests
make test       # unit tests, the Unicorn CPU differential test, the card tests
make aarch64    # cross-compile check (aarch64-linux-gnu-gcc)
make test-aarch64  # the suite on the aarch64 build under qemu-user
```

The native engine needs an AArch64 kernel that runs 32-bit ARM programs (an
arm64 VM under `qemu-system-aarch64` will do) with `arm-linux-gnueabihf-gcc`:

```sh
make armhf-native  # build/armhf/gport2x: static, linked at 0xE0000000, above the guest
make test-native   # the unit tests, tests/sys and tests/native on it
tools/test_native_remote.sh -i KEY -p 2222 user@host  # sync and run make test-native over ssh
```

Tests that need the user's own images read these variables and skip
otherwise: `GPORT2X_FIRMWARE_DIR`, `GPORT2X_CARD_IMAGE`, `GPORT2X_GAME_ELF`
(and `GPORT2X_SAVES_CONFIG` for the mission tests). `tools/test_native_remote.sh`
takes them from `~/.gport2x-test-env` on the remote machine.

The PortMaster package (`dist/gport2x.zip`, no game or firmware data) is built
from an aarch64 SDL2 build and the native engine with SDL2 (`make
armhf-native-sdl SDL_SYSROOT=...`, see the Makefile):

```sh
tools/make_port.sh BINARY_AARCH64 BINARY_ARMHF_NATIVE
```

## Running

Boot the game the way the console does (the menu's `execvp` fallback runs the
`.gpe` script with `/bin/sh`):

```sh
build/host/gport2x --firmware FIRMWARE_ROOTFS --card CARD.img \
  --cwd /mnt/sd/Payback /bin/sh /mnt/sd/Payback/Payback.gpe
```

Boot the firmware (init script, menu) with the real clock, a screenshot every
second and a timed pad script (milliseconds):

```sh
build/host/gport2x --firmware FW --card CARD.img --cwd / --clock real \
  --dump snaps --snapshot-ms 1000 --pad-time "3000:DOWN,3300:-,4500:B,4800:-" \
  /etc/rc.d/rc.sysinit
```

Deterministic test run with frame dumps (`frame_%05d.ppm`, 320×240 P6), the
audio as WAV, scripted input, stopping after 300 flips:

```sh
build/host/gport2x --firmware FW --card CARD.img --cwd /mnt/sd/Payback \
  --test --flips 300 --dump frames --wav out.wav --pad "40:DOWN,44:-,80:B,84:-" \
  /bin/sh /mnt/sd/Payback/Payback.gpe
```

Play it in a window (needs SDL2 at build time; keyboard: arrows, Enter =
START, Space = SELECT, Q/W = L/R, Z/X/A/S = A/B/X/Y, -/+ = volume, C = stick
push, Esc = quit):

```sh
build/host/gport2x --firmware FW --card CARD.img --cwd /mnt/sd/Payback --sdl 3 \
  /bin/sh /mnt/sd/Payback/Payback.gpe
```

The same natively, fullscreen, on a handheld (the armhf build with SDL2):

```sh
gport2x.armhf --engine native --fullscreen --firmware FW --card CARD.img \
  --cwd /mnt/sd/Payback /bin/sh /mnt/sd/Payback/Payback.gpe
```

Record real calls of chosen functions (interpreter only; one JSON line per
call: entry registers, the bytes it read before writing them, the bytes it
wrote, its syscalls and results, r0/r1), e.g. to check a reimplementation on
the program's own data:

```sh
build/host/gport2x ... --capture 0x123e8,0xae2c8 --capture-out calls.jsonl --capture-exe Payback ...
```

`--help` lists every option. `--trace syscall,mmio,fs,dev` (or
`GPORT2X_TRACE`) prints the guest's activity; `--log debug` the harness's.

## Layout

| directory | contents |
|---|---|
| `include/gport2x/` | one public header per module |
| `src/gmem` | guest address space: pages, refcounted objects, MMIO |
| `src/elf` | ELF32 ARM loader (OABI ET_EXEC, ET_DYN interpreters, auxv) |
| `src/cpu` | ARMv4T interpreter with ARMv4 memory semantics, SWP, FPA transfers |
| `src/card` | FAT16/32 reader with Linux 2.4 vfat semantics |
| `src/fs` | the guest namespace: mounts, host dirs, overlay, /proc, /dev, fds, pipes |
| `src/dev` | the GP2X physical model, MMSP2 registers, /dev/mem, fb, dsp, mixer |
| `src/proc` | tasks, LinuxThreads clones, fork/exec/wait, signals, the scheduler |
| `src/sys` | syscall decode, OABI/EABI marshalling, dispatch |
| `src/native` | the native engine: guest code on the host CPU, seccomp syscall traps, host processes and threads |
| `src/aemu` | the native engine's one-instruction emulator (SWP, FPA, on SIGILL/SIGBUS) |
| `src/main` | the command line, the SDL2 front end |
| `port/`, `tools/` | the PortMaster port and its packaging; the remote native test runner |
| `tests/` | per-module tests; `tests/README.md` explains the rules |
| `docs/ARCHITECTURE.md` | the design |

The behavioural specification this implements lives in the main project as
`docs/HARNESS_SPEC.md`; section numbers in comments refer to it.

## License

GPort2X is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 2 of the License, or (at your option) any later
version (SPDX: `GPL-2.0-or-later`). See `LICENSE`. It contains no game,
firmware or decompilation code or data (`LEGAL.md`); you supply your own.

