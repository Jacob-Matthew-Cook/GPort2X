#!/usr/bin/env python3
"""--exit-at-menu under the native engine (tests/game/test_quit.py runs the
game's QUIT with it under the interpreter): the firmware's shell starts the
menu the two ways a program's end can,

1. from a child process, as the card's .gpe script does (`./gp2xmenu`, then
   more script): the child exits with status 0 instead of becoming the menu
   and the script goes on (here to `exit 3`, GPort2X's exit status);
2. with `exec` from GPort2X's own process, the first guest process: it exits
   with status 0, and so does GPort2X;

and neither starts the menu. Runs only on a build with the native engine
(make armhf-native) and skips (77) elsewhere. Needs GPORT2X_FIRMWARE_DIR."""
import os, re, subprocess, sys, tempfile

BUILD = os.environ.get('BUILD', 'build/host')
BIN = os.path.abspath(os.path.join(BUILD, 'gport2x'))
if not os.environ.get('GPORT2X_FIRMWARE_DIR'):
    print('skip: unset GPORT2X_FIRMWARE_DIR')
    sys.exit(77)
if not os.path.exists(BIN):
    print('gport2x not built')
    sys.exit(1)
probe = subprocess.run([BIN, '--engine', 'native', '--log', 'error', '/nonexistent'],
                       capture_output=True, text=True, timeout=120)
if 'needs the armhf build' in probe.stderr:
    print('skip: this build has no native engine (make armhf-native, on an AArch64 kernel)')
    sys.exit(77)

failures = []
def check(cond, msg):
    if not cond:
        failures.append(msg)
        print('FAIL:', msg)

for name, script, want_rc in (('child', 'cd /usr/gp2x; ./gp2xmenu; exit 3', 3),
                              ('exec', 'cd /usr/gp2x && exec ./gp2xmenu', 0)):
    with tempfile.TemporaryDirectory(prefix='gport2x-native-eam-') as scratch:
        cmd = [BIN, '--engine', 'native', '--firmware', os.environ['GPORT2X_FIRMWARE_DIR'], '--exit-at-menu',
               '--scratch', scratch, '--log', 'info', '--cwd', '/', '/bin/sh', '-c', script]
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        except subprocess.TimeoutExpired:  # the menu started after all
            check(False, f'{name}: GPort2X did not end')
            continue
    log = r.stderr
    check(re.search(r'execve\(/usr/gp2x/gp2xmenu\): exits with status 0 instead', log) is not None,
          f'{name}: the menu\'s execve exits instead')
    check(re.search(r'exec pid \d+: /usr/gp2x/gp2xmenu', log) is None, f'{name}: the menu does not start')
    check(r.returncode == want_rc, f'{name}: GPort2X exits {want_rc} (got {r.returncode})')
    if failures:
        print(log[-3000:])
        sys.exit(1)
print('--exit-at-menu, native: the menu\'s execve exits with status 0, from a child and from the first process')
