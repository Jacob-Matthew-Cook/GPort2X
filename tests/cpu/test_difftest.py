#!/usr/bin/env python3
"""Differential test: random ARM instruction sequences run under the GPort2X
interpreter (tools/cpu/tool_cpu_runner) and under Unicorn (ARM926EJ-S), with
the final registers, flags and data memory compared.

The generated instructions stay inside what both agree on for ARMv4T code:
data processing (all three operand forms), multiplies, aligned loads and
stores with immediate offsets and writeback, LDM/STM in all four modes, SWP,
MRS/MSR, and conditional execution. Rotated unaligned loads, STR pc and
register-shift PC offsets are ARMv4-specific and are covered by test_cpu.c.

Exits 77 (skip) if Unicorn or the runner is unavailable."""
import os
import random
import struct
import subprocess
import sys

# A Python with Unicorn (pip install unicorn); GPORT2X_UNICORN_PYTHON names one
# in a virtualenv, else the current interpreter is used.
VENV_PY = os.environ.get("GPORT2X_UNICORN_PYTHON", "")
BUILD = os.environ.get("BUILD", "build/host")
RUNNER = os.path.join(BUILD, "tools/cpu/tool_cpu_runner")
CODE, DATA = 0x10000, 0x20000
BASE = DATA + 0x100          # r10: a fixed base inside the data page
CASES = int(os.environ.get("DIFFTEST_CASES", "400"))
SEED = int(os.environ.get("DIFFTEST_SEED", "1"))
RUN_ENV = dict(os.environ, GPORT2X_JIT_HOT="0")  # main() runs every case under the JIT (compiling at once) and the interpreter

if VENV_PY and sys.executable != VENV_PY and os.path.exists(VENV_PY):
    os.execv(VENV_PY, [VENV_PY] + sys.argv)   # re-run under the venv that has Unicorn
try:
    import unicorn
    from unicorn import arm_const as A
except ImportError:
    print("unicorn not available; skipping")
    sys.exit(77)
if not os.path.exists(RUNNER):
    print(f"{RUNNER} not built; skipping")
    sys.exit(77)

SCRATCH = [r for r in range(0, 10)] + [12, 13, 14]   # never r10 (base), r11 (temp base), r15

def dp_imm(cond, op, s, rn, rd, imm8, rot):
    return cond << 28 | 1 << 25 | op << 21 | s << 20 | rn << 16 | rd << 12 | rot << 8 | imm8

def dp_reg(cond, op, s, rn, rd, rm, typ, amount):
    return cond << 28 | op << 21 | s << 20 | rn << 16 | rd << 12 | amount << 7 | typ << 5 | rm

def dp_regshift(cond, op, s, rn, rd, rm, typ, rs):
    return cond << 28 | op << 21 | s << 20 | rn << 16 | rd << 12 | rs << 8 | typ << 5 | 1 << 4 | rm

def ldst(cond, l, b, p, u, w, rn, rd, imm12):
    return cond << 28 | 1 << 26 | p << 24 | u << 23 | b << 22 | w << 21 | l << 20 | rn << 16 | rd << 12 | imm12

def ldsth(cond, l, kind, p, u, w, rn, rd, imm8):
    return (cond << 28 | p << 24 | u << 23 | 1 << 22 | w << 21 | l << 20 | rn << 16 | rd << 12
            | (imm8 >> 4) << 8 | 1 << 7 | kind << 5 | 1 << 4 | (imm8 & 0xF))

def ldstm(cond, l, p, u, w, rn, lst):
    return cond << 28 | 1 << 27 | p << 24 | u << 23 | w << 21 | l << 20 | rn << 16 | lst

def gen_case(rng):
    """Returns (code_words, data_bytes, regs). Only r10/r11 are used as bases."""
    code = []
    def rcond():
        return rng.choice([0xE] * 6 + list(range(0, 14)))
    for _ in range(rng.randint(10, 48)):
        k = rng.random()
        if k < 0.35:                          # data processing
            op = rng.randrange(16)
            s = 1 if op in (8, 9, 10, 11) else rng.randrange(2)
            # SBZ fields: Rd of TST/TEQ/CMP/CMN and Rn of MOV/MVN. Compilers never set
            # them and QEMU's decoder (Unicorn) rejects the encoding when they are set.
            rn = 0 if op in (13, 15) else rng.choice(SCRATCH + [15])
            rd = 0 if op in (8, 9, 10, 11) else rng.choice(SCRATCH)
            form = rng.randrange(3)
            if form == 0:
                code.append(dp_imm(rcond(), op, s, rn, rd, rng.randrange(256), rng.randrange(16)))
            elif form == 1:
                code.append(dp_reg(rcond(), op, s, rn, rd, rng.choice(SCRATCH + [15]), rng.randrange(4), rng.randrange(32)))
            else:                             # register shift: no pc operands (pc+12 differs from Unicorn's model)
                code.append(dp_regshift(rcond(), op, s, 0 if op in (13, 15) else rng.choice(SCRATCH), rd, rng.choice(SCRATCH), rng.randrange(4), rng.choice(SCRATCH)))
        elif k < 0.45:                        # multiply
            rd, rm, rs, rn = rng.choice(SCRATCH), rng.choice(SCRATCH), rng.choice(SCRATCH), rng.choice(SCRATCH)
            if rd == rm:
                rm = (rm + 1) % 10
            if rng.random() < 0.5:
                code.append(0xE << 28 | rng.randrange(2) << 21 | rng.randrange(2) << 20 | rd << 16 | rn << 12 | rs << 8 | 9 << 4 | rm)
            else:
                lo = rng.choice(SCRATCH)
                hi = rng.choice([r for r in SCRATCH if r != lo])
                rm = rng.choice([r for r in SCRATCH if r not in (lo, hi)])
                code.append(0xE << 28 | 1 << 23 | rng.randrange(2) << 22 | rng.randrange(2) << 21 | rng.randrange(2) << 20 | hi << 16 | lo << 12 | rs << 8 | 9 << 4 | rm)
        elif k < 0.70:                        # aligned load/store, immediate offset from r10, no writeback
            l, b = rng.randrange(2), rng.randrange(2)
            u = rng.randrange(2)
            off = rng.randrange(0, 0xF0, 1 if b else 4)
            rd = rng.choice(SCRATCH)
            if rng.random() < 0.5:
                code.append(ldst(rcond(), l, b, 1, u, 0, 10, rd, off))
            else:                             # halfword / signed forms
                kind = rng.choice([1, 2, 3]) if l else 1
                off = rng.randrange(0, 0xF0, 1 if kind == 2 else 2)
                code.append(ldsth(rcond(), l, kind, 1, u, 0, 10, rd, off))
        elif k < 0.80:                        # writeback forms on r11, re-based each time
            code.append(dp_imm(0xE, 4, 0, 10, 11, 0x40, 0))      # add r11, r10, #0x40
            l, b = rng.randrange(2), rng.randrange(2)
            p, u, w = rng.randrange(2), rng.randrange(2), 1
            off = rng.randrange(0, 0x30, 4)
            rd = rng.choice(SCRATCH)
            code.append(ldst(0xE, l, b, p, u, w, 11, rd, off))
        elif k < 0.92:                        # LDM/STM on r11
            code.append(dp_imm(0xE, 4, 0, 10, 11, 0x40, 0))
            lst = rng.randrange(1, 0x10000) & 0x73FF            # never r10, r11, r15
            if lst == 0:
                lst = 1
            code.append(ldstm(rcond(), rng.randrange(2), rng.randrange(2), rng.randrange(2), rng.randrange(2), 11, lst))
        elif k < 0.96:                        # SWP / SWPB on r11
            code.append(dp_imm(0xE, 4, 0, 10, 11, 0x40, 0))
            rd, rm = rng.choice(SCRATCH), rng.choice(SCRATCH)
            code.append(0xE << 28 | 1 << 24 | rng.randrange(2) << 22 | 11 << 16 | rd << 12 | 9 << 4 | rm)
        else:                                 # MSR flags / MRS
            if rng.random() < 0.5:
                # msr cpsr_f, #imm8 ror 8: NZCV only. Bit 27 is Q on Unicorn's ARM926 (ARMv5E);
                # the ARM920T has no Q and ignores the write (mrs would then read 0 there)
                code.append(0xE328F000 | 4 << 8 | (rng.randrange(256) & 0xF0))
            else:
                code.append(0xE10F0000 | rng.choice(SCRATCH) << 12)   # mrs rd, cpsr
    code.append(0xEF000000)                                          # swi 0: stop
    data = bytes(rng.randrange(256) for _ in range(0x400))
    regs = [rng.randrange(1 << 32) for _ in range(16)]
    regs[10] = BASE
    regs[11] = BASE + 0x40
    regs[15] = CODE
    cpsr = 0x10 | (rng.randrange(16) << 28)
    return code, data, regs, cpsr

def run_gport2x(code, data, regs, cpsr, path):
    blob = struct.pack("<I", 0x47504331) + struct.pack("<16I", *regs) + struct.pack("<I", cpsr)
    cb = b"".join(struct.pack("<I", w) for w in code)
    blob += struct.pack("<II", CODE, len(cb)) + cb + struct.pack("<II", DATA, len(data)) + data + struct.pack("<I", 100000)
    with open(path, "wb") as f:
        f.write(blob)
    out = subprocess.run([RUNNER, path], capture_output=True, text=True, timeout=30, env=RUN_ENV)
    if out.returncode != 0:
        return None
    res = {}
    for line in out.stdout.splitlines():
        k, _, v = line.partition(" ")
        res[k] = v
    return res

def run_unicorn(code, data, regs, cpsr):
    uc = unicorn.Uc(unicorn.UC_ARCH_ARM, unicorn.UC_MODE_ARM)
    try:
        uc.ctl_set_cpu_model(A.UC_CPU_ARM_926)
    except Exception:
        pass
    uc.mem_map(CODE, 0x1000, unicorn.UC_PROT_ALL)
    uc.mem_map(DATA, 0x1000, unicorn.UC_PROT_READ | unicorn.UC_PROT_WRITE)
    cb = b"".join(struct.pack("<I", w) for w in code)
    uc.mem_write(CODE, cb)
    uc.mem_write(DATA, data)
    regnames = [A.UC_ARM_REG_R0, A.UC_ARM_REG_R1, A.UC_ARM_REG_R2, A.UC_ARM_REG_R3, A.UC_ARM_REG_R4, A.UC_ARM_REG_R5,
                A.UC_ARM_REG_R6, A.UC_ARM_REG_R7, A.UC_ARM_REG_R8, A.UC_ARM_REG_R9, A.UC_ARM_REG_R10, A.UC_ARM_REG_R11,
                A.UC_ARM_REG_R12, A.UC_ARM_REG_R13, A.UC_ARM_REG_R14, A.UC_ARM_REG_R15]
    uc.reg_write(A.UC_ARM_REG_CPSR, cpsr)
    for i, r in enumerate(regnames):
        if i != 15:
            uc.reg_write(r, regs[i])
    stopped = {"swi": False}
    def on_intr(u, intno, _):
        stopped["swi"] = True
        u.emu_stop()
    uc.hook_add(unicorn.UC_HOOK_INTR, on_intr)
    try:
        uc.emu_start(CODE, CODE + len(cb), count=100000)
    except unicorn.UcError as e:
        return None, f"unicorn error {e}"
    out = [uc.reg_read(r) for r in regnames]
    return {"regs": out, "cpsr": uc.reg_read(A.UC_ARM_REG_CPSR), "data": uc.mem_read(DATA, len(data)), "swi": stopped["swi"]}, None

def main():
    rng = random.Random(SEED)
    tmp = os.path.join(os.environ.get("TMPDIR", "/tmp"), f"gport2x_difftest_{os.getpid()}.bin")
    bad = 0
    skipped = 0
    for n in range(CASES):
        code, data, regs, cpsr = gen_case(rng)
        ref, err = run_unicorn(code, data, regs, cpsr)
        if ref is None or not ref["swi"]:
            skipped += 1      # the reference faulted or did not reach the swi; not a comparable case
            continue
        RUN_ENV["GPORT2X_JIT"] = "0"
        interp = run_gport2x(code, data, regs, cpsr, tmp)
        RUN_ENV["GPORT2X_JIT"] = "1"
        got = run_gport2x(code, data, regs, cpsr, tmp)
        if got is not None and interp != got:   # JIT and interpreter: identical state and instruction count
            print(f"case {n}: JIT differs from the interpreter: {interp} vs {got}"); bad += 1; continue
        if got is None:
            print(f"case {n}: runner failed"); bad += 1; continue
        gregs = [int(x, 16) for x in got["regs"].split()]
        gcpsr = int(got["cpsr"], 16)
        diffs = []
        if got["stop"].split()[0] != "2":
            diffs.append(f"stop={got['stop']} (expected svc)")
        for i in range(15):
            if gregs[i] != ref["regs"][i]:
                diffs.append(f"r{i}: gport2x={gregs[i]:08x} unicorn={ref['regs'][i]:08x}")
        if (gcpsr >> 28) != (ref["cpsr"] >> 28):
            diffs.append(f"flags: gport2x={gcpsr >> 28:x} unicorn={ref['cpsr'] >> 28:x}")
        if bytes.fromhex(got["data"]) != bytes(ref["data"]):
            diffs.append("data memory differs")
        if diffs:
            bad += 1
            print(f"case {n}: MISMATCH")
            for d in diffs[:8]:
                print("   ", d)
            print("    code:", " ".join(f"{w:08x}" for w in code))
            if bad >= 5:
                break
    if os.path.exists(tmp):
        os.unlink(tmp)
    print(f"difftest: {CASES - skipped - bad} agree, {bad} mismatch, {skipped} not comparable")
    sys.exit(1 if bad else 0)

if __name__ == "__main__":
    main()
