#!/usr/bin/env python3
"""The dynamic loader run as a command (`ld.so PROGRAM`, as some programs'
launch scripts do): a position-independent ELF executed directly is placed at
ELF_ET_DYN_BASE (0x80000000, 2/3 of TASK_SIZE) as Linux 2.4 does, loads the
program and runs it. Needs GPORT2X_FIRMWARE_DIR; skips (77) otherwise."""
import os, re, subprocess, sys

BUILD = os.environ.get('BUILD', 'build/host')
BIN = os.path.join(BUILD, 'gport2x')
if not os.environ.get('GPORT2X_FIRMWARE_DIR'):
    print('skip: unset GPORT2X_FIRMWARE_DIR')
    sys.exit(77)
r = subprocess.run([BIN, '--firmware', os.environ['GPORT2X_FIRMWARE_DIR'], '--log', 'info', '--cwd', '/',
                    '/lib/ld-linux.so.2', '/bin/echo', 'hello-from-ldso'], capture_output=True, text=True, timeout=300)
fail = []
m = re.search(r'exec pid \d+: /lib/ld-linux.so.2 \(OABI\) entry ([0-9a-f]+)', r.stderr)
if not m or not 0x80000000 <= int(m.group(1), 16) < 0x80100000:
    fail.append(f'ld.so not loaded at 0x80000000 (entry {m.group(1) if m else "none"})')
if r.stdout.strip() != 'hello-from-ldso' or r.returncode != 0:
    fail.append(f'the program did not run (exit {r.returncode}, output {r.stdout!r})')
for f in fail:
    print('FAIL:', f)
if fail:
    print(r.stderr[-2000:])
    sys.exit(1)
print('ld.so as a command: loaded at 0x80000000, ran /bin/echo')
