#!/usr/bin/env python3
"""QUIT returns to the menu (spec P5): on the genuine chain, the pad script
selects QUIT in the main menu and confirms; the game exits cleanly (its
hardware init runs again before the atexit unmapping, so the C4 zeroing has
no effect: HARNESS_SPEC section 12), /bin/sh continues the .gpe script and
starts gp2xmenu, whose autorun ([main] autorun in common.ini, on by default,
checked at every start) launches the game again.

Then the same with --exit-at-menu, as the PortMaster port's launchers run it:
the .gpe script's gp2xmenu exits with status 0 instead of starting, so the
script and the run end there, with status 0, long before the flip limit.
Needs GPORT2X_FIRMWARE_DIR and GPORT2X_CARD_IMAGE; skips (77) otherwise."""
import os, re, subprocess, sys

BUILD = os.environ.get('BUILD', 'build/host')
BIN = os.path.join(BUILD, 'gport2x')
missing = [v for v in ('GPORT2X_FIRMWARE_DIR', 'GPORT2X_CARD_IMAGE') if not os.environ.get(v)]
if missing:
    print('skip: unset', ', '.join(missing))
    sys.exit(77)
script = '1900:DOWN,1904:-,1940:DOWN,1944:-,1980:DOWN,1984:-,2020:DOWN,2024:-,2060:B,2064:-,2120:UP,2124:-,2160:B,2164:-'

def run(*extra):
    cmd = [BIN, '--firmware', os.environ['GPORT2X_FIRMWARE_DIR'], '--card', os.environ['GPORT2X_CARD_IMAGE'],
           '--cwd', '/mnt/sd/Payback', '--test', '--flips', '2400', '--pad', script, '--log', 'info', *extra,
           '/bin/sh', '/mnt/sd/Payback/Payback.gpe']
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=1800)
    return r.returncode, r.stderr

failures = []
def check(cond, msg):
    if not cond:
        failures.append(msg)
        print('FAIL:', msg)

def game_quits(log, what):
    """The game exits cleanly after QUIT; returns the log after its exit."""
    m = re.search(r'exec pid (\d+): /mnt/tmp/Payback_tmp', log)
    check(m is not None, f'{what}: the game is exec\'d by the stub')
    pid = m.group(1) if m else '0'
    exit_m = re.search(r'exit: pid %s status (0x[0-9a-f]+|\d+)' % pid, log)
    check(exit_m is not None and exit_m.group(1) in ('0', '0x0'),
          f'{what}: the game exits cleanly after QUIT (got {exit_m.group(1) if exit_m else "no exit"})')
    return log[exit_m.end():] if exit_m else ''

rc, log = run()
after = game_quits(log, 'genuine')
check('/bin/sync' in after and 'exec pid' in after and '/usr/gp2x/gp2xmenu' in after,
      'genuine: the .gpe script runs sync and gp2xmenu after the game')
check(re.search(r'exec pid \d+: /mnt/tmp/Payback_tmp', after) is not None, 'genuine: the menu\'s autorun starts the game again')
check('flip limit' in log, f'genuine: the run reaches its flip limit (exit {rc})')
if failures:
    print(log[-3000:])
    sys.exit(1)

rc, log = run('--exit-at-menu')
after = game_quits(log, '--exit-at-menu')
check('/bin/sync' in after, '--exit-at-menu: the .gpe script runs sync after the game')
check(re.search(r'execve\(/usr/gp2x/gp2xmenu\): exits with status 0 instead', after) is not None,
      '--exit-at-menu: the menu\'s execve exits instead')
check(re.search(r'exec pid \d+: /usr/gp2x/gp2xmenu', after) is None, '--exit-at-menu: the menu does not start')
check(re.search(r'exec pid \d+: /mnt/tmp/Payback_tmp', after) is None, '--exit-at-menu: the game does not start again')
check('stopped: all tasks exited' in log and rc == 0, f'--exit-at-menu: the run ends there with status 0 (exit {rc})')
if failures:
    print(log[-3000:])
    sys.exit(1)
print('QUIT: clean exit, .gpe script relaunches the menu, autorun restarts the game; '
      'with --exit-at-menu the run ends there, status 0')
