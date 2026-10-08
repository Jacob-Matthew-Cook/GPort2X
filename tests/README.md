# Tests

`make test` builds everything and runs `tests/run_tests.sh`, which runs:

* `tests/<module>/test_*.c`: compiled against `libgport2x.a`; exit 0 = pass.
* `tests/<module>/test_*.sh` and `test_*.py`: scripts; exit 0 = pass, 77 = skip.

Rules:

* Every behaviour test has an **exact expected value** taken from the spec or
  from an oracle, so a wrong implementation fails. Tautological tests are not
  tests.
* Oracles: `qemu-arm` runs EABI ARM guest test programs (assembled with
  `arm-linux-gnueabi-as`/`ld`); the same program must behave identically under
  GPort2X. Unicorn (`pip install unicorn`; a virtualenv's Python can be named
  with `GPORT2X_UNICORN_PYTHON`) is the CPU oracle for the interpreter. A script that needs an absent oracle exits 77.
* Tests that need the user's own images read `GPORT2X_GAME_ELF`,
  `GPORT2X_CARD_IMAGE`, `GPORT2X_FIRMWARE_DIR` or `GPORT2X_SAVES_CONFIG` (a
  `Data/Config` directory with a 100% profile, for the mission test; it must
  be the pristine snapshot the oracle reference was made from, because the
  game and the oracle rewrite `Payback.ini` and `Slot*.ini` as they run) and
  exit 77 when unset. Only synthetic data and SHA-256 lists of oracle frames are
  committed (LEGAL.md rule 5).
* `tests/game/`: whole-game gates against the native oracle, by flip count
  (`frame_%05d.ppm`): boot to the main menu (`test_boot`, `test_chain`), the
  firmware menu chain (`test_menu`), mission 2 (`test_mission2`) and the
  RAMPAGE replay (`test_rampage`). Pad scripts use the oracle's syntax; on the
  full card the intro ends at flip ~1890, so menu presses start at 1900.
* `tests/sys/`: guest programs in ARM assembly run under `qemu-arm` and under
  GPort2X (EABI and OABI); output and exit codes must match.
* Keep tests small and fast; the whole suite should run in well under a minute
  without the oracles.

## State diffs against the oracle

The frame gates stop where audio-driven overlays start. The gate for a
replay or a mission is a state diff: `tools/harness/statediff_run.py` in the
decomp repository runs the native oracle and the harness with the same pad
script, dumps both `.data/.bss` images at one flip (`--state-flip N
--state-dump FILE` here, `PB_STATE_FLIP`/`PB_STATE_DUMP` there) and
classifies every differing run. Run it from the decomp repository with the
same environment variables as the tests:

```bash
python3 tools/harness/statediff_run.py --flip 2990 --pad "1900:DOWN,1904:-,...,2190:B,2194:-" --out /tmp/sd_replay0
```

Exit status 0 means every difference is audio-thread, thread-library, voice
or sound-handle state; anything else is listed and is a simulation
difference to investigate. `--dump` adds a frame-by-frame comparison and
reports the first differing flip. As of 2026-10-07 all 7 replays (flip
2,990) and STORY missions 1-6 (flip 3,400, `--saves-config` pointing at the
pristine Config snapshot) pass.
