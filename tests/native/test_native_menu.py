#!/usr/bin/env python3
"""The firmware menu chain under the native engine (test_menu's path): the
stock gp2xmenu boots with --boot --disable-autorun (its boot script pipes
mount into grep, runs modprobe/insmod/rmmod/ifconfig), renders with the FPA
code of its libraries emulated on SIGILL, and a timed pad script walks
Game -> SD card -> Payback -> Payback.gpe and launches it: execvp gets
ENOEXEC, the menu falls back to /bin/sh, the stub mounts a tmpfs, writes the
10,377,888-byte image and execs it, and the game flips to the limit; exit
status 0 and no process of GPort2X's group left.

Real clock: the presses start at GPORT2X_MENU_READY_MS (default 30,000 ms),
well after the menu appears (about 13 s in an emulated arm64 VM, a second or
two on a handheld), and leave 25 s for the SD card script that highlighting
Game starts. Runs only on a build with the native engine (make
armhf-native) and skips (77) elsewhere. Needs GPORT2X_FIRMWARE_DIR and
GPORT2X_CARD_IMAGE."""
import os, re, shutil, subprocess, sys, tempfile, time

BUILD = os.environ.get('BUILD', 'build/host')
BIN = os.path.abspath(os.path.join(BUILD, 'gport2x'))
missing = [v for v in ('GPORT2X_FIRMWARE_DIR', 'GPORT2X_CARD_IMAGE') if not os.environ.get(v)]
if missing:
    print('skip: unset', ', '.join(missing))
    sys.exit(77)
if not os.path.exists(BIN):
    print('gport2x not built')
    sys.exit(1)
probe = subprocess.run([BIN, '--engine', 'native', '--log', 'error', '/nonexistent'],
                       capture_output=True, text=True, timeout=120)
if 'needs the armhf build' in probe.stderr:
    print('skip: this build has no native engine (make armhf-native, on an AArch64 kernel)')
    sys.exit(77)

t0 = int(os.environ.get('GPORT2X_MENU_READY_MS', '30000'))
# Highlighting Game makes the menu run its SD card script (mount | grep,
# modprobe, insmod, ...), seconds in an emulated VM, during which presses are
# lost: the next press waits 25 s.
presses = [(0, 'RIGHT'), (25000, 'B'), (30000, 'B'), (35000, 'B'), (40000, 'DOWN'), (45000, 'B')]
script = ','.join(f'{t0 + t}:{k},{t0 + t + 300}:-' for t, k in presses)
tmp = tempfile.mkdtemp(prefix='gport2x-native-menu-')
snaps = os.path.join(tmp, 'snaps')
scratch = os.path.join(tmp, 'scratch')
cmd = [BIN, '--engine', 'native', '--firmware', os.environ['GPORT2X_FIRMWARE_DIR'],
       '--card', os.environ['GPORT2X_CARD_IMAGE'], '--cwd', '/usr/gp2x', '--clock', 'real',
       '--dump', snaps, '--snapshot-ms', '1000', '--pad-time', script, '--flips', '30',
       '--scratch', scratch, '--keep-scratch', '--log', 'info', '--trace', 'syscall',
       '--env', 'PATH=/sbin:/usr/sbin:/usr/local/sbin:/bin:/usr/bin:/usr/local/bin',
       '--env', 'LD_LIBRARY_PATH=./:/lib:/usr/local/lib:/usr/lib', '--env', 'HOME=/', '--env', 'TERM=linux',
       '/usr/gp2x/gp2xmenu', '--boot', '--disable-autorun']
start = time.time()
p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
try:
    log = p.communicate(timeout=t0 / 1000 + 600)[1]
except subprocess.TimeoutExpired:
    p.kill()  # SIGTERM would go to the guest
    log = p.communicate()[1]
secs = time.time() - start
failures = []

def check(cond, msg):
    if not cond:
        failures.append(msg)
        print('FAIL:', msg)

def group_members(pgid):
    out = []
    for e in os.listdir('/proc'):
        if e.isdigit():
            try:
                stat = open(f'/proc/{e}/stat').read()
            except OSError:
                continue
            fields = stat[stat.rindex(')') + 2:].split()
            if int(fields[2]) == pgid and fields[0] not in 'ZX':
                out.append(int(e))
    return out

blank = lambda path: all(b == 0 for b in open(path, 'rb').read()[15:15 + 60000:7])
before = [os.path.join(snaps, f) for f in sorted(os.listdir(snaps)) if f.startswith('snap_')][:t0 // 1000] \
    if os.path.isdir(snaps) else []
check(before and not all(blank(s) for s in before), 'the menu renders before the presses start')
check(re.search(r'execve\((\./|/mnt/sd/Payback/)Payback\.gpe, .*\n.*= -8 \(Exec format error\)', log) is not None,
      'execve of Payback.gpe returns ENOEXEC')
check(re.search(r'Exec format error\)\n(?:[^\n]*\n){0,12}?[^\n]*exec pid \d+: /bin/sh', log) is not None,
      'the menu falls back to /bin/sh')
check('mount: tmpfs on /mnt/tmp' in log, 'the stub mounts a tmpfs')
sizes = [os.path.getsize(os.path.join(d, f)) for d, _, fs in os.walk(scratch) for f in fs if f == 'Payback_tmp']
check(sizes == [10377888], f'Payback_tmp is 10,377,888 bytes (got {sizes})')
check(re.search(r'exec pid \d+: /mnt/tmp/Payback_tmp \(OABI\)', log) is not None, 'the game image is exec\'d')
check(re.search(r'stopped: flip limit; 30 flips', log) is not None and p.returncode == 0,
      f'the game flips to the limit and GPort2X exits 0 (exit {p.returncode})')
time.sleep(0.5)
left = group_members(p.pid)
check(not left, f'processes of GPort2X\'s group left behind: {left}')
shutil.rmtree(tmp, ignore_errors=True)
if failures:  # the log's tail, then the failures last (the runner shows the last lines)
    print('\n'.join(l for l in log.splitlines() if '[gport2x syscall]' not in l)[-2500:])
    print(f'{len(failures)} failed after {secs:.0f} s (exit {p.returncode}):', *failures, sep='\n  ')
    sys.exit(1)
print(f'firmware menu (native, {secs:.0f} s): renders, launches Payback.gpe through sh and the stub; the game flips')
