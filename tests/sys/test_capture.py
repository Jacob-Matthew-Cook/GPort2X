#!/usr/bin/env python3
"""Call capture (--capture, src/proc/capture.c) on a guest program whose
function f(&buf, n) reads buf[0], writes buf[0] + n to buf[1], writes three
bytes to stdout, getcwd()s into a buffer and returns buf[1]; main calls it with n = 5, then 7. Each
JSON record must hold exactly that: r0/r1 at entry, buf[0] as the only
data input (besides the stack and literal pools in its text), buf[1] as an output, the write syscall with its result 3,
getcwd with its result and the bytes it stored ("/w\\0") listed under it,
and the return value. Skips (77) without an ARM cross assembler."""
import json, os, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.environ.get('BUILD', 'build/host')
BIN = os.path.abspath(os.path.join(BUILD, 'gport2x'))
for prefix in ('arm-linux-gnueabi-', 'arm-linux-gnueabihf-'):
    AS, LD, NM = prefix + 'as', prefix + 'ld', prefix + 'nm'
    if shutil.which(AS) and shutil.which(LD):
        break
else:
    print('skip: no arm-linux-gnueabi(hf)-as/ld')
    sys.exit(77)
work = tempfile.mkdtemp(prefix='gport2x-capture-')
obj, exe = os.path.join(work, 'p.o'), os.path.join(work, 'prog')
subprocess.run([AS, '-march=armv4t', '-o', obj, os.path.join(HERE, 'guest', 't_capture.S')], check=True)
subprocess.run([LD, '-static', '-Ttext=0x10000', '-o', exe, obj], check=True)
syms = {l.split()[2]: int(l.split()[0], 16) for l in subprocess.run([NM, exe], capture_output=True, text=True).stdout.splitlines()
        if len(l.split()) == 3}
f, buf = syms['f'], syms['buf']
os.mkdir(os.path.join(work, 'w'))
out = os.path.join(work, 'cap.jsonl')
r = subprocess.run([BIN, '--root', work, '--cwd', '/w', '--log', 'warn', '--capture', hex(f), '--capture-out', out, '/prog'],
                   capture_output=True, timeout=120)
failures = []
def check(cond, msg):
    if not cond:
        failures.append(msg)
        print('FAIL:', msg)
check(r.returncode == 0 and r.stdout == b'ok\nok\n', f'the program runs (rc {r.returncode}, stdout {r.stdout!r})')
recs = [json.loads(l) for l in open(out)] if os.path.exists(out) else []
check(len(recs) == 2, f'two records (got {len(recs)})')
for rec, n in zip(recs, (5, 7)):
    v = 0x11223344 + n
    check(rec['addr'] == f and rec['complete'], f'call n={n}: complete record of f')
    check(rec['regs'][0] == buf and rec['regs'][1] == n, f'call n={n}: entry r0/r1 {rec["regs"][:2]}')
    sp = rec['regs'][13]
    # literal-pool loads from the program's text are reads too; leave them out
    reads = [(a, h) for a, h in rec['reads'] if not (sp - 0x10000 <= a < sp + 0x10000) and a >= buf]
    # the second call still reads buf[0] as an input; buf[1] is written, never read
    check(reads == [[buf, '44332211']] or reads == [(buf, '44332211')], f'call n={n}: non-stack inputs {reads}')
    writes = [(a, h) for a, h in rec['writes'] if not (sp - 0x10000 <= a < sp + 0x10000)]
    check([list(w) for w in writes] == [[buf + 4, v.to_bytes(4, 'little').hex()]], f'call n={n}: outputs {writes}')
    stack = [w for w in rec['writes'] if sp - 16 <= w[0] < sp]
    check(stack and stack[0][0] == sp - 16, f'call n={n}: the push of 4 registers below sp is a write ({stack})')
    svcs = rec['svcs']
    check(len(svcs) == 2 and svcs[0][1:4] == [4, 1, 3] and svcs[0][4] == [],
          f'call n={n}: write returning 3, no output ({svcs[:1]})')
    check(len(svcs) == 2 and svcs[1][1:4] == [183, syms['cwd'], 3] and svcs[1][4] == [[syms['cwd'], '2f7700']],
          f'call n={n}: getcwd returning 3 with its output "/w\\0" ({svcs[1:]})')
    check(rec['ret'][0] == v, f'call n={n}: returns {rec["ret"][0]:#x}, expected {v:#x}')
shutil.rmtree(work, ignore_errors=True)
if failures:
    sys.exit(1)
print(f'capture: 2 records of f with its inputs, outputs, syscall and result')
