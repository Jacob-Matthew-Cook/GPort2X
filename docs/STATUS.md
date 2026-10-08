# Status (2026-10-08)

What GPort2X does today, against the behavioural specification it was
written from (`HARNESS_SPEC.md`, kept with the reverse-engineering project it
came out of; its section numbers are in the last column).

"Done" means an executable check passes: a unit test in `tests/`, a
differential test against qemu-arm or Unicorn, or a comparison with the
reference implementation, frame by frame (`tests/game`) or of the whole
game state at one frame (a "state diff", see [tests/README.md](../tests/README.md)).

## Summary

- **Interpreter engine:** complete. Boots the stock firmware menu and the
  game, with a deterministic test mode.
- **JIT:** x86-64 and AArch64, on by default.
- **Native engine:** complete on AArch64 kernels that run 32-bit programs;
  the game at its full pace on an Anbernic RG353M.
- **Accuracy:** boot and the menus are frame-identical to the reference;
  missions and replays keep the game state identical, with frames differing
  only where the game times itself by its audio.
- **Open:** FPA arithmetic follows long double rather than NWFPE's
  SoftFloat (library accuracy unmeasured); the deterministic test modes do
  not run under the native engine yet.

## By area

| area | state | checked by | spec |
|---|---|---|---|
| ELF loading: OABI executable at 0x8000, brk at 0x00F24000, 2.4 memory layout, stack below 0xC0000000, thread stacks | done | `tests/elf`, boot trace | 1.1, 1.4 |
| ARMv4T CPU: rotated unaligned loads, aligned stores, PC+8/+12, SWP, no Thumb | done | `tests/cpu`: hand cases and 400 random sequences against Unicorn | 1.2, 5 |
| system calls: 79 OABI calls plus EABI, OABI stat64, old mmap, signals, dirents, poll/select, mount | done, ~150 calls | `tests/sys` against qemu-arm (EABI and OABI) | 2 |
| syscall trap | done in both engines (note 1) | `tests/sys` under both, `tests/native` | 1.3 |
| memory: the 32 MB bank and 64 KB register file, aliased windows | done | `tests/dev` | 1.5, 3.2 |
| RAM use | 69.5 MB for a mission, budget 80 MB | measured (interpreter, bank included) | 1.6 |
| framebuffers fb0 and fb1 | done (fb1 at 0x03381000) | `tests/dev`, boot | 3.3 |
| OSS audio and mixer | done (note 2) | `tests/dev`, WAV output, `test_native_game` | 3.4, 3.5 |
| /dev/mmuhack, CPU clock changes | done | boot trace | 3.6, 3.7 |
| GPIO buttons, battery, TV-out chip, consoles | done for the menu's needs; some values assumed (open questions 7, 8) | `test_menu` | 3.8, 3.9 |
| MMSP2 registers: timer, GPIO, clock change, display control, latched memory; a flip is a scan-out write | done | `tests/dev`, 301 of 301 frames | 4 |
| copy protection: genuine values reported, never bypassed | done; check C4 fails, as it does on a genuine device | `tests/card` (genuine image), boot trace | 6 |
| firmware boot: rc.sysinit, irqbattery, gp2xmenu, autorun, explorer; the Game section's launch, the exec fallback to `sh`, the launcher stub and its tmpfs | done | `test_menu`, `test_chain` | 7.1-7.3 |
| the menu renders, navigates and launches; the game to frame 300 identical | done | `test_menu`, `test_boot`, `test_chain` | 7.6 P1-P4 |
| QUIT returns to the menu | done (note 3) | `test_quit` | 7.6 P5 |
| boot to the main menu: frames 0-300 identical | done | `test_boot`, `test_chain` | 8.4 gate 2 |
| replays | game state identical (note 4) | `test_rampage`, state diffs | 8.4 gate 3 |
| missions | frames identical to 3,399, state identical (note 5) | `test_mission2`, state diffs | 8.4 gate 4 |
| idle screen dim | reproduced (note 6) | state diff with frames | 8.2 |
| interpreter and JIT speed | 600 boot frames in 1.4 s on an i7-9700F (note 7) | `make test`, `make test-aarch64` | 1.2 |
| real time | no audio underruns over 3,000 frames (note 8) | `--clock real --flip-log` | 8.2 |
| unaligned accesses | none anywhere (note 9) | `--trace unaligned` | 5.3 |
| native engine: SWP and FPA instructions | done (note 10) | `tests/aemu`, native game and menu tests | 5.2, 5.6 |
| FPA floating point | transfers exact; arithmetic in long double, not NWFPE's SoftFloat | the menu runs; library accuracy unmeasured | 9 #19 |
| SDL2 front end | window, fullscreen, keyboard, controllers, audio (note 11) | by hand on the device | 9 #12 |
| arm64 test machine | an emulated arm64 VM builds and tests the native engine | `make test-native` via `tools/test_native_remote.sh` | 8.3 |
| full speed on a handheld | 25.4 fps on the RG353M, no underruns (note 12) | by hand; `--flip-log` | 8.2 |
| PortMaster port | installed and run on the RG353M (note 13) | by hand | |

## Notes

1. **Syscall trap.** The interpreter calls a hook on `svc`. The native engine
   (an armhf build on an AArch64 kernel with 32-bit support,
   [NATIVE_ENGINE.md](NATIVE_ENGINE.md)) catches the guest's calls with a
   seccomp filter and SIGSYS, and runs guest threads and processes as host
   ones.
2. **Audio.** The OSS device takes blocking writes of 8 x 512 bytes; reopening
   it after a clock change works. Under the native engine the device model is
   shared memory, so the game's process and GPort2X's own see one audio ring
   and one mixer. With SDL the audio device's callback pulls from the ring;
   `--wav` records what it plays.
3. **QUIT.** QUIT and confirm exit cleanly: the hardware set-up runs again
   before the exit unmaps it, so the protection check's zeroing has no effect
   and nothing faults. The card's `.gpe` script relaunches the menu, and
   autorun restarts the game.
4. **Replays.** All seven replays at frame 2,990 have the simulation state of
   the reference (the object pool differs only in mixer voice handles, the
   trigger tables only in sound handles, and the random-number indices are
   equal); the rest of the difference is audio-thread and thread-library
   state. RAMPAGE's frames are identical through the intro, the EXTRAS and
   REPLAYS menus and the loading screens (2,178 frames compared). Inside a
   replay, message overlays leave the screen when their speech has played
   through the audio device, which the reference (no audio thread) never
   does; in one replay a subtitle's jitter then offsets the
   cosmetic random draws (debris, puffs, glow) from frame 2,426, while the
   simulation stays identical.
5. **Missions.** Mission 2 is frame-identical to frame 3,399 (menu navigation,
   the start at ~2,304, ~1,100 frames of play), excluding the intro-to-menu
   transition (1,825-1,977, a wait on the audio stream). State diffs of all
   six story missions at frame 3,400: the simulation state is identical
   (voice handles and the audio callback's idle counter aside). Mission 2 to
   frame 10,000, with a volume key every 1,500 frames: state identical, all
   three random-number indices equal; frames identical except that window
   and 8,801-8,953, where the mission has ended and a new music track's "now
   playing" banner slides in (music plays here, not in the reference).
6. **Idle dim.** The game dims the screen after 5,161 audio buffers without a
   button press. Its audio thread runs here, so it does: story mission 1
   (last press at frame 2,260) is dimmed for frames 3,986-4,130 and
   identical to the reference before and after; the reference never dims.
   Mission frame comparisons therefore end within ~1,700 frames of the last
   press.
7. **Speed.** The JIT ([JIT.md](JIT.md)) brought 600 boot frames on an
   i7-9700F from 13.3 s (the original interpreter) and 6.1 s (interpreted
   with the caches) to 1.4 s. On AArch64 under qemu-user it takes 12.1 s
   against 44.6 s interpreted, with identical frames. On the RG353M's
   Cortex-A55 the game reaches 8 fps (its full pace is 25.4), which is why
   the native engine exists. `--no-jit` keeps the interpreter.
8. **Real time.** A RAMPAGE replay run for 3,000 frames on the real clock has
   no audio underruns (1,335 before the fixes). Sleeps and poll timeouts end
   on 2.4 jiffy ticks, so the intro runs at 40 ms and gameplay at 50 ms per
   frame (the game scales its simulation by the measured time). Gameplay
   needs 67 M guest instructions per second (83 M in the worst second).
9. **Unaligned accesses.** A census with the interpreter found no unaligned
   word, halfword or LDM/STM access over boot, the six missions (6,000
   frames), the seven replays (2,991 frames) and 300 M instructions of the
   firmware menu.
10. **Native instruction emulation.** `aemu` emulates one instruction on
    SIGILL or SIGBUS: SWP as a host atomic exchange, FPA and the rest
    through the interpreter over host memory. LinuxThreads uses SWP in the
    game and the menu, and the firmware's libraries use FPA.
11. **SDL2.** A window or fullscreen with integer scaling, the keyboard and
    game controllers by button position, a quit combination, audio pulled
    by the device's callback. Built for aarch64, and for armhf with the
    native engine, against SDL 2; in use on the RG353M (ROCKNIX: Wayland,
    PipeWire).
12. **Handheld.** Anbernic RG353M (RK3566, Cortex-A55, ROCKNIX): the native
    engine runs the genuine launch chain at the game's full pace, 25.4 fps
    (39.4 ms frames, set by the 2.4 timer ticks), with no audio underrun.
13. **PortMaster port.** `port/` and `tools/make_port.sh`: launchers for the
    game and the firmware menu with either engine, controls by button
    position, the player's firmware and card image read-only, saves in the
    port's folder. No game or firmware data is packaged ([LEGAL.md](../LEGAL.md)).

## Deviations from the specification

Recorded in its section 12: the flip rule, fb1's address, FPA transfers
being priority B, kernel-complete writes, 2.4 jiffy rounding of sleeps,
`/dev/tty` as the console, and the closing of open questions 5, 18, 19, 20
and 25.
