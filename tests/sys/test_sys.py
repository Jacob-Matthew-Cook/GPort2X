#!/usr/bin/env python3
"""Syscall layer tests (spec 8.1 gate 1): guest programs in ARM assembly are
run under an oracle (EABI) and under GPort2X in both the EABI and the OABI
encodings; stdout and the exit code must match byte for byte. The oracle is
qemu-arm, or the host kernel itself where it runs 32-bit ARM programs (an
AArch64 kernel with compat support). GPort2X runs them with the interpreter
and, on a build where it is available (make armhf-native), with the native
engine too. Each program runs in its own temporary directory, which is the
guest root under GPort2X (--root) and the cwd under the oracle. Skips (77)
without a cross assembler/linker or an oracle."""
import os, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.environ.get('BUILD', 'build/host')
BIN = os.path.abspath(os.path.join(BUILD, 'gport2x'))
for prefix in ('arm-linux-gnueabi-', 'arm-linux-gnueabihf-'):
    AS, LD = prefix + 'as', prefix + 'ld'
    if shutil.which(AS) and shutil.which(LD):
        break
else:
    print('skip: no arm-linux-gnueabi(hf)-as/ld')
    sys.exit(77)
if not os.path.exists(BIN):
    print('gport2x not built')
    sys.exit(1)
work = tempfile.mkdtemp(prefix='gport2x-sys-')
failures = []

def build(src, oabi):
    name = os.path.basename(src)[:-2] + ('-oabi' if oabi else '-eabi')
    obj = os.path.join(work, name + '.o')
    exe = os.path.join(work, name)
    cmd = [AS, '-march=armv4t', '-I', os.path.join(HERE, 'guest'), '-o', obj, src]
    if oabi:
        cmd += ['--defsym', 'OABI=1']
    subprocess.run(cmd, check=True)
    # at 0x10000: the host's mmap_min_addr (0x8000 at most for the game) must
    # admit the first page under the native engine
    subprocess.run([LD, '-static', '-Ttext=0x10000', '-o', exe, obj], check=True)
    return exe

def run_oracle(exe):
    d = tempfile.mkdtemp(prefix='q-', dir=work)
    os.mkdir(os.path.join(d, 'w'))
    r = subprocess.run(ORACLE + [exe], cwd=os.path.join(d, 'w'), capture_output=True, timeout=60)
    return r.stdout, r.returncode

def run_harness(exe, engine):
    d = tempfile.mkdtemp(prefix='h-', dir=work)
    os.mkdir(os.path.join(d, 'w'))  # the program's cwd: only its own files appear in listings
    shutil.copy(exe, os.path.join(d, 'prog'))
    r = subprocess.run([BIN, '--engine', engine, '--root', d, '--cwd', '/w', '--log', 'warn', '/prog'],
                       capture_output=True, timeout=120)
    return r.stdout, r.returncode, r.stderr.decode(errors='replace')

progs = {}
for prog in ('t_files', 't_mem', 't_proc'):
    src = os.path.join(HERE, 'guest', prog + '.S')
    progs[prog] = (build(src, False), build(src, True))

# the oracle: qemu-arm, else the host kernel when it runs ARM programs itself
ORACLE = None
if shutil.which('qemu-arm'):
    ORACLE, oracle_name = ['qemu-arm'], 'qemu'
else:
    try:
        subprocess.run([progs['t_mem'][0]], capture_output=True, timeout=60)
        ORACLE, oracle_name = [], 'kernel'
    except OSError:
        pass
if ORACLE is None:
    print('skip: no qemu-arm, and the host does not run 32-bit ARM programs')
    shutil.rmtree(work, ignore_errors=True)
    sys.exit(77)

engines = ['interp']
probe = run_harness(progs['t_mem'][0], 'native')
if 'needs the armhf build' not in probe[2]:
    engines.append('native')
print(f'oracle: {oracle_name}; engines: {", ".join(engines)}')

for prog, (eabi, oabi) in progs.items():
    q_out, q_rc = run_oracle(eabi)
    for engine, abi, exe in [(e, a, x) for e in engines for a, x in (('eabi', eabi), ('oabi', oabi))]:
        h_out, h_rc, h_err = run_harness(exe, engine)
        label = f'{engine} {abi}'
        if (h_out, h_rc) != (q_out, q_rc):
            failures.append(f'{prog} {label}')
            print(f'FAIL {prog} ({label}): {oracle_name} rc={q_rc} harness rc={h_rc}')
            ql, hl = q_out.decode(errors='replace').splitlines(), h_out.decode(errors='replace').splitlines()
            for i in range(max(len(ql), len(hl))):
                a = ql[i] if i < len(ql) else '<none>'
                b = hl[i] if i < len(hl) else '<none>'
                if a != b:
                    print(f'  line {i}: {oracle_name} {a!r}  harness {b!r}')
            if h_err.strip():
                print('  harness stderr:', h_err.strip()[-600:])
        else:
            print(f'ok   {prog} ({label}): {len(q_out.splitlines())} lines, rc {q_rc}')
shutil.rmtree(work, ignore_errors=True)
sys.exit(1 if failures else 0)
