#!/usr/bin/env python3
"""The firmware menu chain (spec 7, P1-P3): the stock gp2xmenu boots with
--boot --disable-autorun (the irqbattery exit-1 path), renders (P1), a timed
pad script walks Game -> SD card -> Payback -> Payback.gpe and launches it
(P2, P3): the menu's execvp gets ENOEXEC, falls back to /bin/sh, the stub
mounts a tmpfs, writes the 10,377,888-byte image and execs it, and the game
flips. Real clock, so timings are generous. Needs GPORT2X_FIRMWARE_DIR and
GPORT2X_CARD_IMAGE; skips (77) otherwise."""
import os, re, subprocess, sys, tempfile

BUILD = os.environ.get('BUILD', 'build/host')
BIN = os.path.join(BUILD, 'gport2x')
missing = [v for v in ('GPORT2X_FIRMWARE_DIR', 'GPORT2X_CARD_IMAGE') if not os.environ.get(v)]
if missing:
    print('skip: unset', ', '.join(missing))
    sys.exit(77)
tmp = tempfile.mkdtemp(prefix='gport2x-menu-')
snaps = os.path.join(tmp, 'snaps')
scratch = os.path.join(tmp, 'scratch')
script = '3000:RIGHT,3300:-,5000:B,5300:-,8000:B,8300:-,11000:B,11300:-,13500:DOWN,13800:-,15500:B,15800:-'
cmd = [BIN, '--firmware', os.environ['GPORT2X_FIRMWARE_DIR'], '--card', os.environ['GPORT2X_CARD_IMAGE'],
       '--cwd', '/usr/gp2x', '--clock', 'real', '--dump', snaps, '--snapshot-ms', '1000', '--pad-time', script,
       '--flips', '30', '--scratch', scratch, '--keep-scratch', '--log', 'info', '--trace', 'syscall',
       '--env', 'PATH=/sbin:/usr/sbin:/usr/local/sbin:/bin:/usr/bin:/usr/local/bin',
       '--env', 'LD_LIBRARY_PATH=./:/lib:/usr/local/lib:/usr/lib', '--env', 'HOME=/', '--env', 'TERM=linux',
       '/usr/gp2x/gp2xmenu', '--boot', '--disable-autorun']
try:
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    log, code = r.stderr, r.returncode
except subprocess.TimeoutExpired as e:
    log, code = (e.stderr or b'').decode(errors='replace') if isinstance(e.stderr, bytes) else (e.stderr or ''), 'timeout'
failures = []
def check(cond, msg):
    if not cond:
        failures.append(msg)
        print('FAIL:', msg)
# P1: a snapshot taken after SDL init is non-blank
blank = lambda p: all(b == 0 for b in open(p, 'rb').read()[15:15 + 60000:7])
later = [os.path.join(snaps, f) for f in sorted(os.listdir(snaps)) if f.startswith('snap_')][3:8] if os.path.isdir(snaps) else []
check(later and not all(blank(p) for p in later), 'the menu renders (a non-blank snapshot after 3 s)')
# P2: the SD root listing (10 entries from the card: ., .., Tools, Payback, autorun.gpu, game, music, movie, ebook, photo)
check(re.search(r'chdir\("/mnt/sd", [^\n]*= 0\n(?:[^\n]*\n){0,8}?[^\n]*getdents64\(\w+, \w+, \w+, \w+\) = 0x128', log) is not None, 'the Game section lists the card root (getdents64 = 0x128)')
check('open("/mnt/sd/Payback/Payback.png"' in log, 'the entry icon Payback.png is loaded')
# P3: execvp -> ENOEXEC -> /bin/sh fallback -> stub -> tmpfs -> exec
check(re.search(r'execve\((\./|/mnt/sd/Payback/)Payback\.gpe, .*\n.*= -8 \(Exec format error\)', log) is not None, 'execve of Payback.gpe returns ENOEXEC')
check(re.search(r'Exec format error\)\n(?:[^\n]*\n){0,12}?[^\n]*exec pid \d+: /bin/sh', log) is not None, 'the menu falls back to /bin/sh')
check('mount: tmpfs on /mnt/tmp' in log, 'the stub mounts a tmpfs')
sizes = [os.path.getsize(os.path.join(d, f)) for d, _, fs in os.walk(scratch) for f in fs if f == 'Payback_tmp']
check(sizes == [10377888], f'Payback_tmp is 10,377,888 bytes (got {sizes})')
check(re.search(r'exec pid \d+: /mnt/tmp/Payback_tmp \(OABI\)', log) is not None, 'the game image is exec\'d')
check('flip limit' in log, f'the game flips (exit {code})')
if failures:
    print(log[-2500:])
subprocess.run(['rm', '-rf', tmp])
if failures:
    sys.exit(1)
print('firmware menu: renders, lists the card, launches Payback.gpe through sh and the stub; the game flips')
