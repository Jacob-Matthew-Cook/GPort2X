# GPort2X

GPort2X runs software for the **GP2X** handheld on modern Linux PCs and ARM
handhelds. You supply your own firmware dump and SD card image; GPort2X
recreates the console's Linux 2.4 user space and its MMSP2 hardware around
them, and runs the programs unchanged and read-only.

It is written from a behavioural specification and public documentation only.
It contains no game, firmware or decompiled code, and it never bypasses copy
protection: a genuine image passes the game's checks on its own
([LEGAL.md](LEGAL.md)).

## What works

- **The stock firmware** boots: `rc.sysinit`, the battery daemon and the GP2X
  menu, which renders, navigates and launches programs.
- **Payback** runs from its card through the genuine launch chain (the
  menu's `.gpe` script, the shell and the launcher stub), its copy
  protection, the intro, the menus and the missions, with sound. QUIT returns
  to the menu.
- **Deterministic test mode:** boot, the menus and mission play match an
  independent reference frame for frame (apart from moments timed by the
  audio), and the game state stays identical through all six missions and
  the seven replays (`tests/game`).
- **Full speed on a handheld:** on an Anbernic RG353M (RK3566, ROCKNIX), the
  native engine plays the game at the console's own pace, packaged as a
  PortMaster port.

[docs/STATUS.md](docs/STATUS.md) has the details and the evidence for each.

## Engines

GPort2X runs the guest's ARM code one of three ways. Everything else (files,
devices, syscalls, processes) is shared.

| engine | runs on | speed |
|---|---|---|
| interpreter with JIT (default) | x86-64 and AArch64 Linux | 600 boot frames in 1.4 s on an i7-9700F; 8 fps on an RK3566 |
| interpreter (`--no-jit`) | any Linux host | the slow, exact reference |
| native (`--engine native`) | an armhf build on an AArch64 kernel that runs 32-bit programs | the game's full 25 fps on an RK3566 |

## Build

You need Linux, GCC and GNU make. SDL2 development files are optional; with
them you also get a window and fullscreen front end.

```sh
make          # builds build/host/gport2x
make test     # unit tests, plus the game tests if your images are set (see Tests)
```

## Run

Every run needs your own files:

- `FW`: your GP2X firmware dump, the root of its filesystem (`bin/`, `lib/`,
  `usr/gp2x/`, ...).
- `CARD.img`: your SD card image (FAT16 or FAT32). It is never written:
  saves go to a separate directory (`--saves DIR`).

**Play a game in a window** (needs SDL2 at build time):

```sh
build/host/gport2x --firmware FW --card CARD.img --cwd /mnt/sd/Payback --sdl 3 \
  /bin/sh /mnt/sd/Payback/Payback.gpe
```

Keyboard: arrows, Enter = START, Space = SELECT, Q/W = L/R, Z/X/A/S = A/B/X/Y,
-/+ = volume, C = stick click, Esc = quit. Game controllers map by button
position. `--fullscreen` uses the whole display.

**Boot the firmware menu** instead, and start programs from its Game
section:

```sh
build/host/gport2x --firmware FW --card CARD.img --sdl 3 \
  --cwd /usr/gp2x /usr/gp2x/gp2xmenu --boot --disable-autorun
```

Or the whole firmware boot (`rc.sysinit`, the battery daemon, then the menu),
headless, with a screenshot every second and a pad script timed in
milliseconds:

```sh
build/host/gport2x --firmware FW --card CARD.img --cwd / --clock real \
  --dump snaps --snapshot-ms 1000 --pad-time "3000:DOWN,3300:-,4500:B,4800:-" \
  /etc/rc.d/rc.sysinit
```

**Run deterministically** for testing: a fixed clock and `time()`, scripted
input, every frame saved as `frames/frame_00000.ppm` and onwards, the audio
as WAV, stopping after 300 frames. Pad scripts list `FRAME:KEYS` presses and
`FRAME:-` releases:

```sh
build/host/gport2x --firmware FW --card CARD.img --cwd /mnt/sd/Payback \
  --test --flips 300 --dump frames --wav out.wav --pad "40:DOWN,44:-,80:B,84:-" \
  /bin/sh /mnt/sd/Payback/Payback.gpe
```

Other useful options (`--help` lists them all):

| option | does |
|---|---|
| `--saves DIR` | keep the card's saves and settings between runs |
| `--exit-at-menu` | end the run when the program goes back to the firmware menu, instead of starting the menu (for launchers) |
| `--engine native` | run guest code on the CPU (armhf build, see below) |
| `--no-jit` | interpret every instruction |
| `--trace syscall,mmio,fs,dev` | print what the guest does (also `GPORT2X_TRACE`) |
| `--log debug` | print what GPort2X does |
| `--capture ADDR,...` | record real calls of chosen functions as JSON lines (entry registers, memory read and written, syscalls, results), to check a reimplementation against the program's own data |

## Handhelds (PortMaster)

`port/` holds launchers for the game and the firmware menu with either
engine, controls by button position, and a readme for players
([port/gport2x/README.md](port/gport2x/README.md)). The package carries no
game or firmware data; players add their own firmware dump and card image.

The native engine needs an armhf toolchain (`arm-linux-gnueabihf-gcc`) and
runs on an AArch64 kernel with 32-bit support (most handheld firmwares, or an
arm64 VM). Package it from an aarch64 SDL2 build and a native-engine SDL2
build:

```sh
make armhf-native-sdl SDL_SYSROOT=...   # the native engine with SDL2 (see the Makefile)
tools/make_port.sh BINARY_AARCH64 BINARY_ARMHF_NATIVE
```

## Tests

```sh
make test           # unit tests, the CPU differential test against Unicorn, syscall tests against qemu-arm
make aarch64        # static aarch64 build
make test-aarch64   # the suite on it, under qemu-user
make test-native    # the native engine on an AArch64 machine with 32-bit support
tools/test_native_remote.sh -i KEY -p 2222 user@host   # sync and run make test-native over ssh
```

Tests that need your own images read `GPORT2X_FIRMWARE_DIR`,
`GPORT2X_CARD_IMAGE`, `GPORT2X_GAME_ELF` and `GPORT2X_SAVES_CONFIG`, and are
skipped when those are unset. [tests/README.md](tests/README.md) explains how
the tests are built.

## How it works

The guest is ARM Linux user-mode code. An engine runs its instructions and
delivers its system calls, undefined instructions and memory faults to one
shared environment: a sparse guest address space, a read-only FAT card with
Linux 2.4 vfat behaviour, a namespace of mounts and overlays, the GP2X's
devices (framebuffers, OSS audio, `/dev/mem` with the MMSP2 registers, GPIO
buttons), LinuxThreads processes and a scheduler. Host backends turn its
display, audio, input and clock into files, SDL2 or a deterministic test
clock. [docs/](docs/README.md) has the design.

| directory | contents |
|---|---|
| `src/cpu` | ARMv4T interpreter and its x86-64/AArch64 JIT |
| `src/native`, `src/aemu` | the native engine, and its one-instruction emulator for SWP and FPA |
| `src/gmem` | guest address space: pages, shared objects, MMIO |
| `src/elf` | ELF32 ARM loader |
| `src/sys` | syscall decoding and dispatch (OABI and EABI) |
| `src/proc` | processes, LinuxThreads, signals, the scheduler |
| `src/fs`, `src/card` | the guest filesystem; the FAT card reader |
| `src/dev` | GP2X devices and the MMSP2 register file |
| `src/main` | the command line and the SDL2 front end |
| `include/gport2x/` | one public header per module |
| `tests/` | per-module tests |
| `port/`, `tools/` | the PortMaster port and its packaging; test helpers |

## Legal

GPort2X runs only software you own, read-only, and never alters or skips
copy protection. Nothing from any game or firmware image, and nothing derived
from a decompilation, is in this repository. Tests that need real images read
them from your machine and skip otherwise. The full rules are in
[LEGAL.md](LEGAL.md).

## License

GPort2X is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 2 of the License, or (at your option) any later
version (`GPL-2.0-or-later`). See [LICENSE](LICENSE).
