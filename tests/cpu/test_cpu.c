/* Interpreter tests with hand-encoded instructions (ARM ARM, ARMv4T): the
 * semantics the spec singles out (rotated unaligned loads, STR pc = pc+12,
 * register-shift PC = pc+12, SWP, BX to Thumb, undefined and abort reporting)
 * plus the ordinary instruction classes with exact expected results.
 * The random differential test against Unicorn (test_difftest.py) covers the
 * broad instruction space; this file pins the ARMv4-specific corners. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gport2x/cpu.h"

static int failures;
#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            failures++;                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
        }                                                                       \
    } while (0)

/* Encoders (cond = AL unless given). */
#define AL 0xEu
static uint32_t dp_imm(unsigned cond, unsigned op, unsigned s, unsigned rn, unsigned rd, unsigned imm8, unsigned rot)
{
    return cond << 28 | 1u << 25 | op << 21 | s << 20 | rn << 16 | rd << 12 | rot << 8 | imm8;
}
static uint32_t dp_reg(unsigned op, unsigned s, unsigned rn, unsigned rd, unsigned rm, unsigned type, unsigned amount)
{
    return AL << 28 | op << 21 | s << 20 | rn << 16 | rd << 12 | amount << 7 | type << 5 | rm;
}
static uint32_t dp_regshift(unsigned op, unsigned s, unsigned rn, unsigned rd, unsigned rm, unsigned type, unsigned rs)
{
    return AL << 28 | op << 21 | s << 20 | rn << 16 | rd << 12 | rs << 8 | type << 5 | 1u << 4 | rm;
}
static uint32_t ldst(unsigned l, unsigned b, unsigned p, unsigned u, unsigned w, unsigned rn, unsigned rd, unsigned imm12)
{
    return AL << 28 | 1u << 26 | p << 24 | u << 23 | b << 22 | w << 21 | l << 20 | rn << 16 | rd << 12 | imm12;
}
static uint32_t ldsth(unsigned l, unsigned kind, unsigned p, unsigned u, unsigned w, unsigned rn, unsigned rd, unsigned imm8)
{
    return AL << 28 | p << 24 | u << 23 | 1u << 22 | w << 21 | l << 20 | rn << 16 | rd << 12 | (imm8 >> 4) << 8 | 1u << 7 |
           kind << 5 | 1u << 4 | (imm8 & 0xFu);
}
static uint32_t ldstm(unsigned l, unsigned p, unsigned u, unsigned w, unsigned rn, unsigned list)
{
    return AL << 28 | 1u << 27 | p << 24 | u << 23 | w << 21 | l << 20 | rn << 16 | list;
}
static uint32_t branch(unsigned link, int32_t words) { return AL << 28 | 5u << 25 | link << 24 | ((uint32_t)words & 0xFFFFFFu); }
static uint32_t bx(unsigned rm) { return 0xE12FFF10u | rm; }
static uint32_t swi(uint32_t imm) { return 0xEF000000u | imm; }
static uint32_t mul(unsigned s, unsigned rd, unsigned rm, unsigned rs) { return AL << 28 | s << 20 | rd << 16 | rs << 8 | 9u << 4 | rm; }
static uint32_t mull(unsigned sgn, unsigned acc, unsigned s, unsigned rdhi, unsigned rdlo, unsigned rm, unsigned rs)
{
    return AL << 28 | 1u << 23 | sgn << 22 | acc << 21 | s << 20 | rdhi << 16 | rdlo << 12 | rs << 8 | 9u << 4 | rm;
}
static uint32_t swp(unsigned b, unsigned rn, unsigned rd, unsigned rm) { return AL << 28 | 1u << 24 | b << 22 | rn << 16 | rd << 12 | 9u << 4 | rm; }
static uint32_t mrs(unsigned rd) { return 0xE10F0000u | rd << 12; }
static uint32_t msr_f_imm(unsigned imm8, unsigned rot) { return 0xE328F000u | rot << 8 | imm8; }

enum { OP_AND, OP_EOR, OP_SUB, OP_RSB, OP_ADD, OP_ADC, OP_SBC, OP_RSC, OP_TST, OP_TEQ, OP_CMP, OP_CMN, OP_ORR, OP_MOV, OP_BIC, OP_MVN };
enum { LSL, LSR, ASR, ROR };

#define CODE 0x10000u
#define DATA 0x20000u

static gmem_t *m;
static cpu_t *c;
static gaddr_t pc_fill;

static void reset(void)
{
    if (c)
        cpu_destroy(c);
    if (m)
        gmem_destroy(m);
    m = gmem_create();
    gmem_map_anon(m, CODE, 0x1000, GMEM_PROT_RWX);
    gmem_map_anon(m, DATA, 0x1000, GMEM_PROT_RW);
    c = cpu_create(m);
    cpu_regs(c)->r[15] = CODE;
    cpu_regs(c)->r[10] = DATA + 0x100; /* r10 points into the data page */
    pc_fill = CODE;
}

static void emit(uint32_t insn)
{
    gmem_st32(m, pc_fill, insn);
    pc_fill += 4;
}

static uint32_t reg_(unsigned n) { return cpu_regs(c)->r[n]; }
static bool flag(uint32_t f) { return (cpu_regs(c)->cpsr & f) != 0; }
static uint32_t data32(uint32_t off) { uint32_t v = 0; gmem_ld32(m, DATA + off, &v); return v; }

static void test_arith_flags(void)
{
    reset();
    emit(dp_imm(AL, OP_MOV, 0, 0, 0, 1, 0));            /* mov r0, #1 */
    emit(dp_imm(AL, OP_ADD, 0, 0, 1, 2, 0));            /* add r1, r0, #2 */
    emit(dp_imm(AL, OP_SUB, 1, 1, 2, 3, 0));            /* subs r2, r1, #3 -> 0, Z C */
    emit(dp_imm(AL, OP_MOV, 0, 0, 3, 0x7F, 4));         /* mov r3, #0x7F000000 */
    emit(dp_reg(OP_ADD, 1, 3, 4, 3, LSL, 0));           /* adds r4, r3, r3 -> 0xFE000000, N V, no C */
    emit(dp_imm(AL, OP_CMP, 1, 0, 0, 2, 0));            /* cmp r0, #2 -> N, no C */
    emit(dp_imm(0x1, OP_MOV, 0, 0, 5, 0x55, 0));        /* movne r5, #0x55 (executes) */
    emit(dp_imm(0x0, OP_MOV, 0, 0, 6, 0x66, 0));        /* moveq r6, #0x66 (skipped) */
    emit(dp_imm(AL, OP_RSB, 1, 0, 7, 0, 0));            /* rsbs r7, r0, #0 -> -1, N, no C */
    emit(dp_imm(AL, OP_MVN, 0, 0, 8, 0, 0));            /* mvn r8, #0 -> 0xFFFFFFFF */
    emit(dp_imm(AL, OP_ADD, 1, 8, 9, 1, 0));            /* adds r9, r8, #1 -> 0, Z C, no V */
    emit(swi(0x900001));
    cpu_stop_info_t info;
    CHECK(cpu_run(c, 0, &info) == CPU_STOP_SVC);
    CHECK(info.svc_imm == 0x900001 && info.pc == CODE + 12 * 4 && reg_(15) == CODE + 12 * 4);
    CHECK(reg_(0) == 1 && reg_(1) == 3 && reg_(2) == 0 && reg_(3) == 0x7F000000u && reg_(4) == 0xFE000000u);
    CHECK(reg_(5) == 0x55 && reg_(6) == 0 && reg_(7) == 0xFFFFFFFFu && reg_(8) == 0xFFFFFFFFu && reg_(9) == 0);
    CHECK(flag(CPSR_Z) && flag(CPSR_C) && !flag(CPSR_N) && !flag(CPSR_V));
    CHECK(cpu_insn_count(c) == 12);
    /* Flag snapshots mid-sequence. */
    reset();
    emit(dp_imm(AL, OP_MOV, 0, 0, 3, 0x7F, 4));
    emit(dp_reg(OP_ADD, 1, 3, 4, 3, LSL, 0));
    CHECK(cpu_run(c, 2, NULL) == CPU_STOP_LIMIT);
    CHECK(flag(CPSR_N) && flag(CPSR_V) && !flag(CPSR_C) && !flag(CPSR_Z));
    reset();
    emit(dp_imm(AL, OP_MOV, 0, 0, 0, 1, 0));
    emit(dp_imm(AL, OP_CMP, 1, 0, 0, 2, 0));
    CHECK(cpu_run(c, 2, NULL) == CPU_STOP_LIMIT);
    CHECK(flag(CPSR_N) && !flag(CPSR_C) && !flag(CPSR_Z) && !flag(CPSR_V));
}

static void test_shifter(void)
{
    reset();
    cpu_regs(c)->r[1] = 0x80000001u;
    cpu_regs(c)->r[2] = 33;
    emit(dp_reg(OP_MOV, 1, 0, 0, 1, LSR, 0));       /* movs r0, r1, lsr #32 -> 0, C = bit31 */
    emit(dp_reg(OP_MOV, 1, 0, 3, 1, ROR, 0));       /* movs r3, r1, rrx (C=1) -> 0xC0000000, C = bit0 = 1 */
    emit(dp_reg(OP_MOV, 1, 0, 4, 1, ASR, 0));       /* movs r4, r1, asr #32 -> 0xFFFFFFFF, C = 1 */
    emit(dp_regshift(OP_MOV, 1, 0, 5, 1, LSL, 2));  /* movs r5, r1, lsl r2 (33) -> 0, C = 0 */
    emit(dp_regshift(OP_MOV, 0, 0, 6, 15, LSL, 9)); /* mov r6, pc, lsl r9 (r9 = 0): pc reads + 12 */
    emit(dp_reg(OP_MOV, 0, 0, 7, 15, LSL, 0));      /* mov r7, pc: + 8 */
    emit(dp_reg(OP_MOV, 1, 0, 8, 1, ROR, 4));       /* movs r8, r1, ror #4 -> 0x18000000, C = bit3 = 0 */
    CHECK(cpu_run(c, 7, NULL) == CPU_STOP_LIMIT);
    CHECK(reg_(0) == 0);
    CHECK(reg_(3) == 0xC0000000u && reg_(4) == 0xFFFFFFFFu && reg_(5) == 0);
    CHECK(reg_(6) == CODE + 4 * 4 + 12);
    CHECK(reg_(7) == CODE + 5 * 4 + 8);
    CHECK(reg_(8) == 0x18000000u && !flag(CPSR_C));
}

static void test_loads_stores(void)
{
    reset();
    gmem_st32(m, DATA + 0x100, 0x44332211u);
    emit(ldst(1, 0, 1, 1, 0, 10, 0, 1));      /* ldr r0, [r10, #1]: unaligned, rotated -> 0x11443322 */
    emit(ldst(1, 0, 1, 1, 0, 10, 1, 2));      /* ldr r1, [r10, #2] -> 0x22114433 */
    emit(ldst(1, 1, 1, 1, 0, 10, 2, 3));      /* ldrb r2, [r10, #3] -> 0x44 */
    emit(ldst(0, 0, 1, 1, 0, 10, 15, 0x10));  /* str pc, [r10, #0x10] -> pc + 12 */
    emit(ldst(0, 0, 1, 1, 0, 10, 15, 0x17));  /* str pc, [r10, #0x17]: forced aligned to 0x14 */
    emit(ldsth(1, 2, 1, 1, 0, 10, 3, 0x1));   /* ldrsb r3, [r10, #1] -> 0x22 */
    emit(ldsth(1, 3, 1, 1, 0, 10, 4, 0x2));   /* ldrsh r4, [r10, #2] -> 0x4433 */
    emit(ldsth(1, 1, 1, 1, 0, 10, 5, 0x0));   /* ldrh r5, [r10] -> 0x2211 */
    emit(ldst(1, 0, 0, 1, 0, 10, 6, 4));      /* ldr r6, [r10], #4: post-indexed writeback */
    emit(ldst(0, 1, 1, 0, 1, 10, 2, 8));      /* strb r2, [r10, #-8]! : pre-indexed writeback */
    CHECK(cpu_run(c, 10, NULL) == CPU_STOP_LIMIT);
    CHECK(reg_(0) == 0x11443322u && reg_(1) == 0x22114433u && reg_(2) == 0x44);
    CHECK(data32(0x110) == CODE + 3 * 4 + 12);
    CHECK(data32(0x114) == CODE + 4 * 4 + 12);
    CHECK(reg_(3) == 0x22 && reg_(4) == 0x4433 && reg_(5) == 0x2211);
    CHECK(reg_(6) == 0x44332211u && reg_(10) == DATA + 0x104 - 8);
    CHECK((data32(0xFC) & 0xFF) == 0x44);
    CHECK(cpu_unaligned_count(c) == 3); /* two rotated ldr, one forced-aligned str */
    /* Sign-extension of negative values. */
    reset();
    gmem_st32(m, DATA + 0x100, 0x8000FF80u);
    emit(ldsth(1, 2, 1, 1, 0, 10, 0, 0)); /* ldrsb r0 -> 0xFFFFFF80 */
    emit(ldsth(1, 3, 1, 1, 0, 10, 1, 2)); /* ldrsh r1, [r10, #2] -> 0xFFFF8000 */
    emit(ldsth(0, 1, 1, 1, 0, 10, 0, 4)); /* strh r0, [r10, #4] -> 0xFF80 */
    CHECK(cpu_run(c, 3, NULL) == CPU_STOP_LIMIT);
    CHECK(reg_(0) == 0xFFFFFF80u && reg_(1) == 0xFFFF8000u && (data32(0x104) & 0xFFFF) == 0xFF80);
}

static void test_multiple(void)
{
    reset();
    for (unsigned i = 0; i < 4; i++)
        cpu_regs(c)->r[i] = 0x11111111u * (i + 1);
    emit(ldstm(0, 0, 1, 1, 10, 0x000Fu)); /* stmia r10!, {r0-r3} */
    emit(ldstm(1, 1, 0, 1, 10, 0x03C0u)); /* ldmdb r10!, {r6-r9} */
    emit(ldstm(0, 1, 1, 0, 10, 0x8400u)); /* stmib r10, {r10, pc}: r10 stored, then pc + 12 */
    CHECK(cpu_run(c, 3, NULL) == CPU_STOP_LIMIT);
    CHECK(data32(0x100) == 0x11111111u && data32(0x10C) == 0x44444444u);
    CHECK(reg_(6) == 0x11111111u && reg_(9) == 0x44444444u && reg_(10) == DATA + 0x100);
    CHECK(data32(0x104) == DATA + 0x100 && data32(0x108) == CODE + 2 * 4 + 12);
    /* LDM with pc in the list branches; STM with writeback stores the old base when it is first. */
    reset();
    gmem_st32(m, DATA + 0x100, 0xAAAA);       /* lowest register first: r4 */
    gmem_st32(m, DATA + 0x104, CODE + 0x100); /* then pc */
    emit(ldstm(1, 0, 1, 0, 10, 0x8010u)); /* ldmia r10, {r4, pc} */
    gmem_st32(m, CODE + 0x100, swi(0x900000));
    cpu_stop_info_t info;
    CHECK(cpu_run(c, 0, &info) == CPU_STOP_SVC && info.pc == CODE + 0x104 && reg_(4) == 0xAAAA);
    /* the same with the data page already cached (the JIT's inline path) */
    reset();
    gmem_st32(m, DATA + 0x100, 0xBBBB);
    gmem_st32(m, DATA + 0x104, CODE + 0x100);
    emit(ldst(1, 0, 1, 1, 0, 10, 5, 0));      /* ldr r5, [r10]: caches the page */
    emit(ldstm(1, 0, 1, 1, 10, 0x8010u));     /* ldmia r10!, {r4, pc} */
    gmem_st32(m, CODE + 0x100, swi(0x900001));
    CHECK(cpu_run(c, 0, &info) == CPU_STOP_SVC && info.pc == CODE + 0x104 && reg_(4) == 0xBBBB &&
          reg_(5) == 0xBBBB && reg_(10) == DATA + 0x108 && cpu_insn_count(c) == 3);
    reset();
    emit(ldstm(0, 0, 1, 1, 10, 0x0C00u)); /* stmia r10!, {r10, r11}: first in list -> original r10 */
    CHECK(cpu_run(c, 1, NULL) == CPU_STOP_LIMIT);
    CHECK(data32(0x100) == DATA + 0x100 && reg_(10) == DATA + 0x108);
}

static void test_branches_and_stops(void)
{
    reset();
    emit(branch(1, 0));                     /* bl: target = pc + 8 + 0, the mov below */
    emit(swi(1));                           /* skipped */
    emit(dp_imm(AL, OP_MOV, 0, 0, 0, 7, 0)); /* mov r0, #7 */
    emit(dp_reg(OP_MOV, 0, 0, 15, 14, LSL, 0)); /* mov pc, lr -> the swi */
    cpu_stop_info_t info;
    CHECK(cpu_run(c, 0, &info) == CPU_STOP_SVC && info.svc_imm == 1 && reg_(0) == 7 && reg_(14) == CODE + 4);
    /* BX to a Thumb address is reported, not executed. */
    reset();
    cpu_regs(c)->r[1] = CODE + 0x101;
    emit(bx(1));
    CHECK(cpu_run(c, 0, &info) == CPU_STOP_THUMB && info.pc == CODE + 0x101 && reg_(15) == CODE);
    /* The explicit undefined instruction. */
    reset();
    emit(0xE7F000F0u);
    CHECK(cpu_run(c, 0, &info) == CPU_STOP_UNDEF && info.pc == CODE && info.insn == 0xE7F000F0u && reg_(15) == CODE);
    /* A coprocessor other than the FPA is undefined. */
    reset();
    emit(0xEE100310u); /* mrc p3, 0, r0, c0, c0, 0 */
    CHECK(cpu_run(c, 0, &info) == CPU_STOP_UNDEF);
    /* FPA: FLT, STF.D, ADF with a constant, FIX, CMF, DVF.S, STF.S, SFM/LFM round trip. */
    reset();
    cpu_regs(c)->r[1] = 5;
    cpu_regs(c)->r[3] = DATA;
    emit(0xEE001190u); /* flt.d f0, r1 */
    emit(0xED838100u); /* stf.d f0, [r3] */
    emit(0xEE08110Fu); /* adf.e f1, f0, #10 */
    emit(0xEE102111u); /* fix r2, f1 */
    emit(0xEE91F11Fu); /* cmf f1, #10 -> greater: C only */
    emit(0xEE40210Au); /* dvf.s f2, f0, #2 -> 2.5 */
    emit(0xED832102u); /* stf.s f2, [r3, #8] */
    emit(0xED830208u); /* sfm f0, 4, [r3, #0x20] */
    emit(0xED934208u); /* lfm f4, 4, [r3, #0x20] */
    emit(0xED83D104u); /* stf.d f5, [r3, #16] (f5 == f1 == 15.0) */
    emit(0xEE90F11Fu); /* cmf f0, #10 -> less: N */
    emit(0xEF000000u); /* svc to stop */
    CHECK(cpu_run(c, 0, &info) == CPU_STOP_SVC);
    {
        uint32_t lo, hi, s32, dlo, dhi;
        gmem_ld32(m, DATA, &hi); gmem_ld32(m, DATA + 4, &lo);
        CHECK(hi == 0x40140000u && lo == 0); /* stf.d of 5.0: FPA doubles store the high word first */
        CHECK(reg_(2) == 15); /* fix of 15.0 */
        gmem_ld32(m, DATA + 8, &s32);
        CHECK(s32 == 0x40200000u); /* stf.s of 2.5 */
        gmem_ld32(m, DATA + 16, &dhi); gmem_ld32(m, DATA + 20, &dlo);
        CHECK(dhi == 0x402E0000u && dlo == 0); /* lfm/sfm round trip, stf.d of 15.0 */
        CHECK((cpu_regs(c)->cpsr & 0xF0000000u) == CPSR_N); /* cmf 5 < 10 sets N */
    }
    /* Data abort: registers untouched, exact address, pc at the instruction. */
    reset();
    cpu_regs(c)->r[0] = 0x1234;
    emit(ldst(1, 0, 1, 1, 0, 10, 0, 0xF00)); /* ldr r0, [r10, #0xF00] -> 0x21000, unmapped */
    CHECK(cpu_run(c, 0, &info) == CPU_STOP_DATA_ABORT);
    CHECK(info.fault_addr == DATA + 0x1000 && info.fault_prot == GMEM_PROT_R && info.pc == CODE && reg_(0) == 0x1234 && reg_(15) == CODE);
    /* Post-indexed writeback is not performed on an abort (base restored). */
    reset();
    emit(ldst(0, 0, 0, 1, 0, 10, 0, 4)); /* str r0, [r10], #4 with r10 pointing at an unmapped page */
    cpu_regs(c)->r[10] = DATA + 0x1000;
    CHECK(cpu_run(c, 0, &info) == CPU_STOP_DATA_ABORT && reg_(10) == DATA + 0x1000 && info.fault_prot == GMEM_PROT_W);
    /* Prefetch abort. */
    reset();
    cpu_regs(c)->r[15] = CODE + 0x1000;
    CHECK(cpu_run(c, 0, &info) == CPU_STOP_PREFETCH_ABORT && info.fault_addr == CODE + 0x1000);
    /* Breakpoint: stops before the instruction, resumes past it. */
    reset();
    emit(dp_imm(AL, OP_MOV, 0, 0, 0, 1, 0));
    emit(dp_imm(AL, OP_MOV, 0, 0, 1, 2, 0));
    emit(swi(0));
    cpu_add_breakpoint(c, CODE + 4);
    CHECK(cpu_run(c, 0, &info) == CPU_STOP_BREAKPOINT && info.pc == CODE + 4 && reg_(0) == 1 && reg_(1) == 0);
    CHECK(cpu_run(c, 0, &info) == CPU_STOP_SVC && reg_(1) == 2);
    /* The NV condition never executes (and is not an ARMv5 unconditional op). */
    reset();
    emit(dp_imm(0xF, OP_MOV, 0, 0, 0, 9, 0));
    emit(swi(0));
    CHECK(cpu_run(c, 0, &info) == CPU_STOP_SVC && reg_(0) == 0 && cpu_insn_count(c) == 2);
}

static void test_multiply_swap_status(void)
{
    reset();
    cpu_regs(c)->r[1] = 0xFFFFFFFFu; /* -1 */
    cpu_regs(c)->r[2] = 7;
    cpu_regs(c)->r[3] = 100;
    emit(mul(1, 0, 1, 2));             /* muls r0, r1, r2 -> -7, N */
    emit(mull(0, 0, 0, 5, 4, 1, 2));   /* umull r4, r5, r1, r2 -> 0x6FFFFFFF9 */
    emit(mull(1, 0, 0, 7, 6, 1, 2));   /* smull r6, r7, r1, r2 -> -7 */
    emit(mull(1, 1, 0, 7, 6, 3, 2));   /* smlal r6, r7, r3, r2 -> -7 + 700 = 693 */
    CHECK(cpu_run(c, 4, NULL) == CPU_STOP_LIMIT);
    CHECK(reg_(0) == 0xFFFFFFF9u && flag(CPSR_N));
    CHECK(reg_(4) == 0xFFFFFFF9u && reg_(5) == 6);
    CHECK(reg_(6) == 693 && reg_(7) == 0);
    /* SWP exchanges; SWPB swaps a byte. */
    reset();
    gmem_st32(m, DATA + 0x100, 0xCAFEBABEu);
    cpu_regs(c)->r[1] = 0x11223344u;
    emit(swp(0, 10, 0, 1));
    emit(swp(1, 10, 2, 1));
    CHECK(cpu_run(c, 2, NULL) == CPU_STOP_LIMIT);
    CHECK(reg_(0) == 0xCAFEBABEu && reg_(2) == 0x44 && data32(0x100) == 0x11223344u);
    /* MSR writes only the flags; MRS reads user mode. */
    reset();
    emit(msr_f_imm(0xF0, 4)); /* msr cpsr_f, #0xF0000000 */
    emit(mrs(0));
    emit(0xE321F0DFu);        /* msr cpsr_c, #0xDF: control field, ignored in user mode */
    emit(mrs(1));
    CHECK(cpu_run(c, 4, NULL) == CPU_STOP_LIMIT);
    CHECK(reg_(0) == 0xF0000010u && reg_(1) == 0xF0000010u);
}

/* A counted loop run in small slices, under the JIT and the interpreter:
 * results and instruction counts must be identical (the JIT chains the loop
 * block to itself and must stop exactly at each budget). */
static void test_loop_budgets(void)
{
    for (int jit = 1; jit >= 0; jit--) {
        cpu_set_jit(jit != 0);
        for (unsigned slice = 1; slice <= 9; slice += 4) {
            reset();
            emit(dp_imm(AL, OP_MOV, 0, 0, 0, 0, 0));      /* mov r0, #0 */
            emit(dp_imm(AL, OP_MOV, 0, 0, 1, 0, 0));      /* mov r1, #0 */
            emit(dp_imm(AL, OP_ADD, 0, 0, 0, 1, 0));      /* loop: add r0, r0, #1 */
            emit(dp_reg(OP_ADD, 0, 1, 1, 0, LSL, 0));     /* add r1, r1, r0 */
            emit(ldst(0, 0, 1, 1, 0, 10, 1, 0));          /* str r1, [r10] */
            emit(dp_imm(AL, OP_CMP, 1, 0, 0, 250, 0));    /* cmp r0, #250 */
            emit((branch(0, -6) & 0x0FFFFFFFu) | 0x10000000u); /* bne loop */
            emit(swi(0));
            cpu_stop_info_t info;
            unsigned runs = 0;
            enum cpu_stop st;
            while ((st = cpu_run(c, slice, &info)) == CPU_STOP_LIMIT && runs < 100000)
                runs++;
            CHECK(st == CPU_STOP_SVC);
            CHECK(reg_(0) == 250 && reg_(1) == 250u * 251u / 2u && data32(0x100) == 250u * 251u / 2u);
            CHECK(cpu_insn_count(c) == 2u + 250u * 5u + 1u);
            CHECK(runs == (2u + 250u * 5u + 1u - 1u) / slice); /* every slice but the last retired exactly `slice` */
        }
    }
    cpu_set_jit(true);
}

int main(void)
{
    setenv("GPORT2X_JIT_HOT", "0", 1); /* compile every block at once: these programs run each block once */
    test_arith_flags();
    test_shifter();
    test_loads_stores();
    test_multiple();
    test_branches_and_stops();
    test_multiply_swap_status();
    test_loop_budgets();
    cpu_destroy(c);
    gmem_destroy(m);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("cpu: ok");
    return 0;
}
