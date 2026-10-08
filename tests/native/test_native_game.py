#!/usr/bin/env python3
"""The native engine on the user's game (docs/NATIVE_ENGINE.md). Runs only on
a build that has the engine (make armhf-native, on an AArch64 kernel with
32-bit compat support) and skips (77) elsewhere. Two runs:

1. The decompressed game injected, as in test_boot, to 302 flips.
2. The genuine chain (/bin/sh Payback.gpe: the stub's shell commands, the
   tmpfs mount, the exec of the decompressed game, LinuxThreads, changeclock
   through sh) to 61 flips; the game runs in a child process here.

Frames: the native engine counts every change of the scanout address, from
start-up, so init's presentation of the cleared fb1 page is its flip 0. The
interpreter's flip rule ignores scanout writes until the game first reads
TCOUNT, a read the native engine cannot see (the register file is plain
shared memory), so native flip k+1 is the oracle's flip k. Through oracle
flip 206 (init, the logos) every frame depends only on the frame count and
must be identical (tests/game/menu_flips_0_300.sha256); from 207 on the
intro's frames follow the clock (the oracle shows some for two flips), which
is real time here and the test mode's fixed step there, so they are counted,
not required.

Both runs: exit status 0 at the flip limit with the interpreter's report,
audio reaching the DAC (in run 2 from the game's own process: the shared
device model), no process of GPort2X's group left behind and its temporary
scratch directory removed. Needs GPORT2X_FIRMWARE_DIR, GPORT2X_CARD_IMAGE
and GPORT2X_GAME_ELF."""
import hashlib, os, re, shutil, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.environ.get('BUILD', 'build/host')
BIN = os.path.abspath(os.path.join(BUILD, 'gport2x'))
need = ['GPORT2X_FIRMWARE_DIR', 'GPORT2X_CARD_IMAGE', 'GPORT2X_GAME_ELF']
missing = [v for v in need if not os.environ.get(v)]
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

oracle = [line.split()[1] for line in open(os.path.join(HERE, '..', 'game', 'menu_flips_0_300.sha256'))]
FRAME_EXACT = 207  # oracle flips 0..206 depend on the frame count alone
failures = []

def check(cond, msg):
    if not cond:
        failures.append(msg)
        print('FAIL:', msg)
    return cond

def group_members(pgid):
    """Live (not zombie) processes of the process group pgid."""
    out = []
    for e in os.listdir('/proc'):
        if not e.isdigit():
            continue
        try:
            stat = open(f'/proc/{e}/stat').read()
        except OSError:
            continue
        fields = stat[stat.rindex(')') + 2:].split()
        if int(fields[2]) == pgid and fields[0] not in 'ZX':
            out.append(int(e))
    return out

def run(name, flips, program):
    tmp = tempfile.mkdtemp(prefix='gport2x-native-')
    tmpdir = os.path.join(tmp, 'tmpdir')  # TMPDIR: where GPort2X puts its scratch directory
    os.mkdir(tmpdir)
    frames = os.path.join(tmp, 'frames')
    cmd = [BIN, '--engine', 'native', '--firmware', os.environ['GPORT2X_FIRMWARE_DIR'],
           '--card', os.environ['GPORT2X_CARD_IMAGE'], '--cwd', '/mnt/sd/Payback', '--test',
           '--flips', str(flips), '--dump', frames, '--log', 'info'] + program
    t0 = time.time()
    p = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True,
                         env=dict(os.environ, TMPDIR=tmpdir))
    try:
        log = p.communicate(timeout=1500)[1]
    except subprocess.TimeoutExpired:
        p.kill()
        log = p.communicate()[1]
        check(False, f'{name}: no exit within 1500 s')
    secs = time.time() - t0
    # GPort2X leads its own process group (setpgid at start-up)
    left = group_members(p.pid)
    for _ in range(20):
        if not left:
            break
        time.sleep(0.1)
        left = group_members(p.pid)
    check(p.returncode == 0, f'{name}: exit status {p.returncode}, expected 0 at the flip limit')
    m = re.search(r'stopped: flip limit; (\d+) flips', log)
    check(m is not None and int(m.group(1)) == flips, f'{name}: the report says {m.group(0) if m else "nothing"}')
    a = re.search(r'audio: (\d+) bytes to the DAC, (\d+) underruns', log)
    check(a is not None and int(a.group(1)) > 0, f'{name}: no audio reached the DAC')
    check(not left, f'{name}: processes of its group left behind: {left}')
    check(not os.listdir(tmpdir), f'{name}: scratch left in TMPDIR: {os.listdir(tmpdir)}')
    clones = re.findall(r'clone: pid \d+ flags (0x[0-9a-f]+)', log)
    check('0xf00' in clones and clones.count('0xf21') >= 3,
          f'{name}: LinuxThreads did not start its manager and threads (clones {clones})')
    got = []
    for k in range(flips):
        path = os.path.join(frames, f'frame_{k:05d}.ppm')
        got.append(hashlib.sha256(open(path, 'rb').read()).hexdigest() if os.path.exists(path) else None)
    strict = min(flips - 1, FRAME_EXACT)
    bad = [k for k in range(strict) if got[k + 1] != oracle[k]]
    check(not bad, f'{name}: native flips k+1 differ from the oracle\'s k for k in {bad[:10]} ({len(bad)} of {strict})')
    timed = sum(got[k + 1] == oracle[k] for k in range(strict, flips - 1))
    shutil.rmtree(tmp, ignore_errors=True)
    print(f'{name}: {flips} flips in {secs:.0f} s; frames 1..{strict} = oracle 0..{strict - 1}'
          + (f', clock-driven {strict}..{flips - 2}: {timed} of {flips - 1 - strict} equal' if flips - 1 > strict else '')
          + f'; audio {a.group(1) if a else 0} bytes, {a.group(2) if a else "?"} underruns; {len(clones)} clones')
    return log

run('injected game', 302, ['--inject', os.environ['GPORT2X_GAME_ELF'] + ':/mnt/tmp/Payback_tmp',
                           '--argv0', './Payback', '/mnt/tmp/Payback_tmp'])
log = run('genuine chain', 61, ['/bin/sh', '/mnt/sd/Payback/Payback.gpe'])
check(re.search(r'mount: tmpfs on /mnt/tmp', log) is not None, 'genuine chain: the stub did not mount its tmpfs')
check(re.search(r'exec pid \d+: /mnt/tmp/Payback_tmp', log) is not None, 'genuine chain: the stub did not exec the game')
check(re.search(r'exec pid \d+: /mnt/sd/Payback/changeclock', log) is not None,
      'genuine chain: changeclock did not run through sh')
if failures:  # last, where the runner shows them
    print(f'{len(failures)} failed:', *failures, sep='\n  ')
sys.exit(1 if failures else 0)
