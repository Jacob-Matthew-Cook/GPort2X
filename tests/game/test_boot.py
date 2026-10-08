#!/usr/bin/env python3
"""Whole-game regression (spec P4, 8.2, 8.4 gate 2): boot the user's game
under the interpreter in test mode and compare the first 301 flips with the
native oracle's frames by SHA-256 (tests/game/menu_flips_0_300.sha256 holds
the hashes of the oracle's frames, which were byte-identical on 2026-10-07).
No game data is stored here: the test needs
  GPORT2X_FIRMWARE_DIR  the firmware rootfs dump
  GPORT2X_CARD_IMAGE    the card image
  GPORT2X_GAME_ELF      the decompressed game executable (injected as /mnt/tmp/Payback_tmp)
and skips (77) when any is unset."""
import hashlib, os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.environ.get('BUILD', 'build/host')
BIN = os.path.join(BUILD, 'gport2x')
need = ['GPORT2X_FIRMWARE_DIR', 'GPORT2X_CARD_IMAGE', 'GPORT2X_GAME_ELF']
missing = [v for v in need if not os.environ.get(v)]
if missing:
    print('skip: unset', ', '.join(missing))
    sys.exit(77)
if not os.path.exists(BIN):
    print('gport2x not built')
    sys.exit(1)
expected = {}
for line in open(os.path.join(HERE, 'menu_flips_0_300.sha256')):
    k, h = line.split()
    expected[int(k)] = h
tmp = tempfile.mkdtemp(prefix='gport2x-boot-')
frames = os.path.join(tmp, 'frames')
cmd = [BIN, '--firmware', os.environ['GPORT2X_FIRMWARE_DIR'], '--card', os.environ['GPORT2X_CARD_IMAGE'],
       '--inject', os.environ['GPORT2X_GAME_ELF'] + ':/mnt/tmp/Payback_tmp', '--cwd', '/mnt/sd/Payback',
       '--argv0', './Payback', '--test', '--flips', str(len(expected)), '--dump', frames, '--log', 'warn',
       '/mnt/tmp/Payback_tmp']
r = subprocess.run(cmd, capture_output=True, text=True, timeout=1200)
print(r.stderr[-2000:])
bad = []
for k, h in expected.items():
    p = os.path.join(frames, f'frame_{k:05d}.ppm')
    if not os.path.exists(p):
        bad.append((k, 'missing'))
        continue
    got = hashlib.sha256(open(p, 'rb').read()).hexdigest()
    if got != h:
        bad.append((k, got[:12]))
subprocess.run(['rm', '-rf', tmp])
if r.returncode != 0:
    print('gport2x exit code', r.returncode)
if bad:
    print(f'{len(bad)} of {len(expected)} flips differ from the oracle:', bad[:10])
    sys.exit(1)
print(f'all {len(expected)} flips identical to the oracle')
