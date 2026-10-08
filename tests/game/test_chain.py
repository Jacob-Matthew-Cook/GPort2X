#!/usr/bin/env python3
"""The genuine launch chain (spec 7.3, P3, P4): /bin/sh runs Payback.gpe from
the card, the stub mounts a tmpfs, decompresses the game to
/mnt/tmp/Payback_tmp (10,377,888 bytes) and execs it; the game must then
reach the same 301 flips as the oracle. Needs GPORT2X_FIRMWARE_DIR and
GPORT2X_CARD_IMAGE; skips (77) otherwise."""
import hashlib, os, re, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.environ.get('BUILD', 'build/host')
BIN = os.path.join(BUILD, 'gport2x')
missing = [v for v in ('GPORT2X_FIRMWARE_DIR', 'GPORT2X_CARD_IMAGE') if not os.environ.get(v)]
if missing:
    print('skip: unset', ', '.join(missing))
    sys.exit(77)
expected = {}
for line in open(os.path.join(HERE, 'menu_flips_0_300.sha256')):
    k, h = line.split()
    expected[int(k)] = h
tmp = tempfile.mkdtemp(prefix='gport2x-chain-')
frames = os.path.join(tmp, 'frames')
scratch = os.path.join(tmp, 'scratch')
cmd = [BIN, '--firmware', os.environ['GPORT2X_FIRMWARE_DIR'], '--card', os.environ['GPORT2X_CARD_IMAGE'],
       '--cwd', '/mnt/sd/Payback', '--test', '--flips', str(len(expected)), '--dump', frames,
       '--scratch', scratch, '--keep-scratch', '--log', 'info', '--trace', 'syscall',
       '/bin/sh', '/mnt/sd/Payback/Payback.gpe']
r = subprocess.run(cmd, capture_output=True, text=True, timeout=1800)
log = r.stderr
failures = []
def check(cond, msg):
    if not cond:
        failures.append(msg)
        print('FAIL:', msg)
check(re.search(r'execve\((\./|/mnt/sd/Payback/)Payback, \[\./Payback\]', log) is not None, 'sh runs ./Payback (the stub)')
check('mount: tmpfs on /mnt/tmp' in log, 'the stub mounts a tmpfs on /mnt/tmp')
check(re.search(r'umount\("/mnt/tmp".*= -22', log) is not None, 'umount of the unmounted /mnt/tmp returns EINVAL')
check(re.search(r'execve\(/mnt/tmp/Payback_tmp, \[\./Payback\]', log) is not None, 'the stub execs /mnt/tmp/Payback_tmp as ./Payback')
sizes = []
for root, dirs, files in os.walk(scratch):
    for f in files:
        if f == 'Payback_tmp':
            sizes.append(os.path.getsize(os.path.join(root, f)))
check(sizes == [10377888], f'Payback_tmp is 10,377,888 bytes (got {sizes})')
check('flip limit' in log, f'reached the flip limit (exit {r.returncode})')
bad = []
for k, h in expected.items():
    p = os.path.join(frames, f'frame_{k:05d}.ppm')
    got = hashlib.sha256(open(p, 'rb').read()).hexdigest() if os.path.exists(p) else 'missing'
    if got != h:
        bad.append(k)
check(not bad, f'{len(bad)} flips differ from the oracle: {bad[:10]}')
if failures:
    print(log[-3000:])
subprocess.run(['rm', '-rf', tmp])
if failures:
    sys.exit(1)
print(f'genuine chain: stub, tmpfs, {sizes[0]}-byte image, exec; all {len(expected)} flips identical to the oracle')
