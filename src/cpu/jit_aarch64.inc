/* AArch64 backend of the JIT (docs/JIT.md), included by cpu.c after
 * jit_common.inc. Same structure as jit_x86_64.inc: blocks within one guest
 * page, handler calls with the interpreter's pc/next_pc/r15, inline ALU,
 * flags, memory access through the page cache and LDM/STM, same-page
 * chaining with a budget check at each link.
 *
 * Registers: x19 = cpu, w20 = budget left at the current block's entry,
 * w21 = instructions retired by earlier blocks in this call (callee-saved);
 * w0-w4 and x9-x11 are scratch, x16 holds call targets. Guest NZCV maps
 * one-to-one onto AArch64 NZCV (SUBS sets C = no borrow, as ARM does), so
 * flag-setting operations copy the host flags with MRS.
 * Encodings: Arm Architecture Reference Manual for A-profile, section C6. */

typedef struct { uint32_t *p; } emit_t;
static void ei(emit_t *e, uint32_t insn) { *e->p++ = insn; }

_Static_assert(offsetof(struct cpu, jlink_page) < 16000 && offsetof(struct cpu, next_pc) < 16000 &&
               offsetof(struct cpu, regs) < 16000, "fields the generated code addresses directly");
_Static_assert(offsetof(struct cpu, jlink_at) % 8 == 0 && offsetof(struct cpu, jlink_page) % 8 == 0, "64-bit stores");
_Static_assert(sizeof(struct tlb_entry) == 16 && offsetof(struct tlb_entry, host) == 8, "inline probe layout");
_Static_assert(TLB_SIZE == 1024u, "inline probe mask");

enum { W0 = 0, W1 = 1, W2 = 2, W3 = 3, W4 = 4, X9 = 9, X10 = 10, X11 = 11, X16 = 16, X19 = 19, W20 = 20, W21 = 21, ZR = 31 };

/* 32-bit logical-immediate encoding (immr << 16 | imms << 10), or -1. */
static int64_t bitmask_imm(uint32_t v)
{
    for (unsigned s = 0; s < 31; s++) {
        uint32_t ones = (1u << (s + 1)) - 1u;
        for (unsigned r = 0; r < 32; r++) {
            uint32_t rot = r ? (ones >> r) | (ones << (32u - r)) : ones;
            if (rot == v)
                return (int64_t)(r << 16 | s << 10);
        }
    }
    return -1;
}

static void movw(emit_t *e, unsigned rd, uint32_t imm)
{
    ei(e, 0x52800000u | (imm & 0xFFFFu) << 5 | rd);                    /* movz wd, #lo */
    if (imm >> 16)
        ei(e, 0x72A00000u | (imm >> 16) << 5 | rd);                    /* movk wd, #hi, lsl 16 */
}
static void movx(emit_t *e, unsigned rd, uint64_t imm)
{
    ei(e, 0xD2800000u | (uint32_t)(imm & 0xFFFFu) << 5 | rd);          /* movz xd */
    for (unsigned hw = 1; hw < 4; hw++)
        if ((imm >> (16 * hw)) & 0xFFFFu)
            ei(e, 0xF2800000u | hw << 21 | (uint32_t)((imm >> (16 * hw)) & 0xFFFFu) << 5 | rd); /* movk */
}
static void ldw(emit_t *e, unsigned rt, uint32_t off) { ei(e, 0xB9400000u | (off / 4u) << 10 | X19 << 5 | rt); }
static void stw(emit_t *e, unsigned rt, uint32_t off) { ei(e, 0xB9000000u | (off / 4u) << 10 | X19 << 5 | rt); }
static void stx(emit_t *e, unsigned rt, uint32_t off) { ei(e, 0xF9000000u | (off / 8u) << 10 | X19 << 5 | rt); }
static void st_imm(emit_t *e, uint32_t off, uint32_t imm) { movw(e, X9, imm); stw(e, X9, off); }
/* shifted-register data processing, 32-bit: op wd, wn, wm, shift #amt */
enum { A_ADD = 0x0B000000, A_SUB = 0x4B000000, A_ADDS = 0x2B000000, A_SUBS = 0x6B000000, A_AND = 0x0A000000,
       A_ORR = 0x2A000000, A_EOR = 0x4A000000, A_BIC = 0x0A200000, A_ORN = 0x2A200000, A_ANDS = 0x6A000000 };
static void alu(emit_t *e, uint32_t op, unsigned rd, unsigned rn, unsigned rm, unsigned shift, unsigned amt)
{
    ei(e, op | shift << 22 | rm << 16 | amt << 10 | rn << 5 | rd);
}
static void mov_r(emit_t *e, unsigned rd, unsigned rm) { alu(e, A_ORR, rd, ZR, rm, 0, 0); }

/* Guest registers cached in host registers x22..x28 while a block runs
 * (chosen per block by jit_pick_cache), loaded lazily on their first read.
 * Write-through: a write updates the host register and the register file,
 * so exits, links and handlers always see memory up to date. gl[] is the
 * translation-time set of cached registers that hold their value at the
 * current point of the code: after a handler call those are reloaded (the
 * handler may have changed any register in memory), and after a
 * conditionally executed instruction the set is restored to what it was
 * before it, since the instruction may have been skipped. Reads of a loaded
 * register cost nothing: no load, no load-use stall on an in-order core. */
static int8_t gc[16];
static bool gl[16];
static unsigned gread(emit_t *e, unsigned g, unsigned tmp)
{
    if (gc[g] < 0) {
        ldw(e, tmp, OFF_R(g));
        return tmp;
    }
    if (!gl[g]) {
        ldw(e, (unsigned)gc[g], OFF_R(g));
        gl[g] = true;
    }
    return (unsigned)gc[g];
}
static void gwrite(emit_t *e, unsigned g, unsigned src)
{
    if (gc[g] >= 0) {
        if ((unsigned)gc[g] != src)
            mov_r(e, (unsigned)gc[g], src);
        gl[g] = true;
    }
    stw(e, src, OFF_R(g));
}
static void gwrite_imm(emit_t *e, unsigned g, uint32_t imm) { movw(e, X9, imm); gwrite(e, g, X9); }
static void greload(emit_t *e)
{
    for (unsigned g = 0; g < 15; g++)
        if (gc[g] >= 0 && gl[g])
            ldw(e, (unsigned)gc[g], OFF_R(g));
}
/* op wd, wn, #imm through a logical immediate when encodable, else via w11 */
static void logic_imm(emit_t *e, uint32_t op_reg, uint32_t op_imm, unsigned rd, unsigned rn, uint32_t imm)
{
    int64_t enc = bitmask_imm(imm);
    if (enc >= 0) {
        ei(e, op_imm | (uint32_t)enc | rn << 5 | rd);
    } else {
        movw(e, X11, imm);
        alu(e, op_reg, rd, rn, X11, 0, 0);
    }
}
static void and_imm(emit_t *e, unsigned rd, unsigned rn, uint32_t imm) { logic_imm(e, A_AND, 0x12000000u, rd, rn, imm); }
static void add_imm(emit_t *e, unsigned rd, unsigned rn, uint32_t imm) /* wd = wn + imm (any 32-bit imm) */
{
    if (imm == 0) {
        if (rd != rn)
            mov_r(e, rd, rn);
    } else if (imm < 4096u) {
        ei(e, 0x11000000u | imm << 10 | rn << 5 | rd);
    } else if ((uint32_t)-imm < 4096u) {
        ei(e, 0x51000000u | ((uint32_t)-imm) << 10 | rn << 5 | rd);
    } else {
        movw(e, X11, imm);
        alu(e, A_ADD, rd, rn, X11, 0, 0);
    }
}
static void add_x_imm(emit_t *e, unsigned rd, unsigned rn, uint32_t imm) /* xd = xn + imm, imm < 2^24 */
{
    unsigned from = rn;
    if (imm >> 12) { ei(e, 0x91400000u | (imm >> 12) << 10 | from << 5 | rd); from = rd; }
    if ((imm & 0xFFFu) || from == rn) ei(e, 0x91000000u | (imm & 0xFFFu) << 10 | from << 5 | rd);
}
static void lsr_imm(emit_t *e, unsigned rd, unsigned rn, unsigned s) { ei(e, 0x53000000u | s << 16 | 31u << 10 | rn << 5 | rd); }
static void cmp_r(emit_t *e, unsigned rn, unsigned rm) { alu(e, A_SUBS, ZR, rn, rm, 0, 0); }
/* branches: the immediate is filled in by patch() */
enum { C_EQ = 0, C_NE = 1, C_HI = 8, C_LO = 3 };
static uint32_t *bcond(emit_t *e, unsigned cond) { uint32_t *at = e->p; ei(e, 0x54000000u | cond); return at; }
static uint32_t *b26(emit_t *e) { uint32_t *at = e->p; ei(e, 0x14000000u); return at; }
static uint32_t *cbnz(emit_t *e, unsigned rt) { uint32_t *at = e->p; ei(e, 0x35000000u | rt); return at; }
static void patch(uint32_t *at, const uint32_t *target)
{
    int64_t off = target - at;
    if ((*at & 0x7C000000u) == 0x14000000u)                  /* B */
        *at = (*at & 0xFC000000u) | ((uint32_t)off & 0x03FFFFFFu);
    else                                                     /* B.cond, CBZ, CBNZ */
        *at = (*at & ~(0x7FFFFu << 5)) | (((uint32_t)off & 0x7FFFFu) << 5);
}

static void jit_link(uint8_t *at, const uint8_t *target)
{
    patch((uint32_t *)at, (const uint32_t *)target);
    __builtin___clear_cache((char *)at, (char *)at + 4);
}

/* NZCV of the last flag-setting op (in host NZCV) into CPSR; nz_only keeps C and V. */
static void emit_merge_flags(emit_t *e, bool nz_only)
{
    ei(e, 0xD53B4200u | W3);                                  /* mrs x3, nzcv */
    ldw(e, W4, OFF_CPSR);
    if (nz_only) {
        and_imm(e, W3, W3, 0xC0000000u);
        and_imm(e, W4, W4, 0x3FFFFFFFu);
    } else {
        and_imm(e, W4, W4, 0x0FFFFFFFu);
    }
    alu(e, A_ORR, W4, W4, W3, 0, 0);
    stw(e, W4, OFF_CPSR);
}

static bool jit_inline(emit_t *e, const struct dentry *d)
{
    unsigned gd = E_RD(d), gn = E_RN(d), gm = E_RM(d);
    dhandler_t f = d->fn;
    if (f == h_mov_imm) { gwrite_imm(e, gd, d->aux); return true; }
    if (f == h_mov_reg) { gwrite(e, gd, gread(e, gm, W0)); return true; }
    if (f == h_mvn_reg) { alu(e, A_ORN, W0, ZR, gread(e, gm, W0), 0, 0); gwrite(e, gd, W0); return true; }
    if (f == h_add_imm) { add_imm(e, W0, gread(e, gn, W0), d->aux); gwrite(e, gd, W0); return true; }
    if (f == h_and_imm) { logic_imm(e, A_AND, 0x12000000u, W0, gread(e, gn, W0), d->aux); gwrite(e, gd, W0); return true; }
    if (f == h_orr_imm) { logic_imm(e, A_ORR, 0x32000000u, W0, gread(e, gn, W0), d->aux); gwrite(e, gd, W0); return true; }
    if (f == h_eor_imm) { logic_imm(e, A_EOR, 0x52000000u, W0, gread(e, gn, W0), d->aux); gwrite(e, gd, W0); return true; }
    if (f == h_rsb_imm) { unsigned a = gread(e, gn, W1); movw(e, W0, d->aux); alu(e, A_SUB, W0, W0, a, 0, 0); gwrite(e, gd, W0); return true; }
    static const struct { dhandler_t f; uint32_t op; bool swap; } reg_ops[] = {
        { h_add_reg, A_ADD, false }, { h_sub_reg, A_SUB, false }, { h_rsb_reg, A_SUB, true }, { h_and_reg, A_AND, false },
        { h_orr_reg, A_ORR, false }, { h_eor_reg, A_EOR, false }, { h_bic_reg, A_BIC, false },
    };
    for (unsigned i = 0; i < sizeof reg_ops / sizeof *reg_ops; i++)
        if (f == reg_ops[i].f) {
            unsigned a = gread(e, gn, W1), b = gread(e, gm, W2);
            if (reg_ops[i].swap) alu(e, reg_ops[i].op, W0, b, a, 0, 0);
            else alu(e, reg_ops[i].op, W0, a, b, 0, 0);
            gwrite(e, gd, W0);
            return true;
        }
    return false;
}

static bool jit_inline_shimm(emit_t *e, const struct dentry *d)
{
    if (d->fn != h_dp_shimm)
        return false;
    uint32_t insn = d->insn, amt = (insn >> 7) & 31u, sh = (insn >> 5) & 3u; /* 0 LSL, 1 LSR, 2 ASR */
    unsigned a = gread(e, E_RN(d), W1), b = gread(e, E_RM(d), W2);
    switch ((insn >> 21) & 15u) {
    case 0x0: alu(e, A_AND, W0, a, b, sh, amt); break;
    case 0x1: alu(e, A_EOR, W0, a, b, sh, amt); break;
    case 0x2: alu(e, A_SUB, W0, a, b, sh, amt); break;
    case 0x3: alu(e, A_ORR, W2, ZR, b, sh, amt); alu(e, A_SUB, W0, W2, a, 0, 0); break; /* op2 - rn */
    case 0x4: alu(e, A_ADD, W0, a, b, sh, amt); break;
    case 0xC: alu(e, A_ORR, W0, a, b, sh, amt); break;
    case 0xD: alu(e, A_ORR, W0, ZR, b, sh, amt); break;
    case 0xE: alu(e, A_BIC, W0, a, b, sh, amt); break;
    default: alu(e, A_ORN, W0, ZR, b, sh, amt); break;                                   /* mvn */
    }
    gwrite(e, E_RD(d), W0);
    return true;
}

static bool jit_inline_flags(emit_t *e, const struct dentry *d)
{
    dhandler_t f = d->fn;
    bool imm = f == h_cmp_imm || f == h_cmn_imm || f == h_tst_imm || f == h_subs_imm || f == h_adds_imm;
    bool reg = f == h_cmp_reg || f == h_cmn_reg || f == h_tst_reg || f == h_subs_reg || f == h_adds_reg;
    if (!imm && !reg)
        return false;
    unsigned a = gread(e, E_RN(d), W0), b;
    if (imm) { movw(e, W1, d->aux); b = W1; }
    else b = gread(e, E_RM(d), W1);
    uint32_t op;
    bool store = false, nz_only = false;
    if (f == h_cmp_imm || f == h_cmp_reg) op = A_SUBS;
    else if (f == h_subs_imm || f == h_subs_reg) { op = A_SUBS; store = true; }
    else if (f == h_cmn_imm || f == h_cmn_reg) op = A_ADDS;
    else if (f == h_adds_imm || f == h_adds_reg) { op = A_ADDS; store = true; }
    else { op = A_ANDS; nz_only = true; }
    alu(e, op, W2, a, b, 0, 0);
    if (store)
        gwrite(e, E_RD(d), W2);
    emit_merge_flags(e, nz_only);
    return true;
}

/* Probe the page cache for the address in w0. On a hit: x9 = host page,
 * w0 = offset in the page. align_mask keeps the low bits that make an
 * unaligned access miss (the handler then applies the ARMv4 rules). */
static void emit_probe(emit_t *e, uint32_t tlb_off, uint32_t align_mask, uint32_t **miss)
{
    lsr_imm(e, X9, W0, 12);
    and_imm(e, X9, X9, TLB_SIZE - 1u);
    add_x_imm(e, X10, X19, tlb_off);
    ei(e, 0x8B000000u | X9 << 16 | 4u << 10 | X10 << 5 | X9);   /* add x9, x10, x9, lsl #4 */
    ei(e, 0xB9400000u | X9 << 5 | X10);                         /* ldr w10, [x9] */
    and_imm(e, X11, W0, 0xFFFFF000u | align_mask);
    cmp_r(e, X11, X10);
    *miss = bcond(e, C_NE);
    ei(e, 0xF9400000u | 1u << 10 | X9 << 5 | X9);               /* ldr x9, [x9, #8] */
    and_imm(e, W0, W0, GP2X_PAGE_SIZE - 1u);
}

static bool emit_addr(emit_t *e, const struct dentry *d, gaddr_t ipc, int *size, bool *load)
{
    dhandler_t f = d->fn;
    unsigned gn = E_RN(d), gm = E_RM(d);
    if (f == h_ldr_imm || f == h_ldrb_imm || f == h_str_imm || f == h_strb_imm ||
        f == h_ldrh_imm || f == h_ldrsh_imm || f == h_ldrsb_imm || f == h_strh_imm) {
        add_imm(e, W0, gread(e, gn, W0), d->aux);
    } else if (f == h_ldr_lit) {
        movw(e, W0, ipc + 8u + d->aux);
    } else if (f == h_ldr_reg || f == h_ldrb_reg || f == h_str_reg || f == h_strb_reg) {
        unsigned m = gread(e, gm, W1), n = gread(e, gn, W2);
        alu(e, (d->aux & 0x100u) ? A_ADD : A_SUB, W0, n, m, 0, d->aux & 31u);
    } else if (f == h_ldrh_reg || f == h_ldrsh_reg || f == h_ldrsb_reg || f == h_strh_reg) {
        unsigned m = gread(e, gm, W1), n = gread(e, gn, W2);
        alu(e, d->aux ? A_ADD : A_SUB, W0, n, m, 0, 0);
    } else if (is_wb(f)) {
        add_imm(e, W0, gread(e, gn, W0), E_PRE(d) ? d->aux : 0u);
    } else {
        return false;
    }
    *load = f == h_ldr_imm || f == h_ldrb_imm || f == h_ldr_lit || f == h_ldr_reg || f == h_ldrb_reg ||
            f == h_ldrh_imm || f == h_ldrsh_imm || f == h_ldrsb_imm || f == h_ldrh_reg || f == h_ldrsh_reg ||
            f == h_ldrsb_reg || f == h_ldr_wb || f == h_ldrb_wb;
    *size = (f == h_ldrb_imm || f == h_strb_imm || f == h_ldrb_reg || f == h_strb_reg || f == h_ldrb_wb || f == h_strb_wb) ? MB
          : (f == h_ldrh_imm || f == h_strh_imm || f == h_ldrh_reg || f == h_strh_reg) ? MH
          : (f == h_ldrsh_imm || f == h_ldrsh_reg) ? MSH : (f == h_ldrsb_imm || f == h_ldrsb_reg) ? MSB : MW;
    return true;
}

static void emit_wb(emit_t *e, const struct dentry *d)
{
    add_imm(e, W1, gread(e, E_RN(d), W1), d->aux);
    gwrite(e, E_RN(d), W1);
}

static void emit_call(emit_t *e, const struct dentry *d, gaddr_t ipc)
{
    st_imm(e, OFF_PC, ipc);
    st_imm(e, OFF_NEXT, ipc + 4u);
    st_imm(e, OFF_R(15), ipc);                                  /* handlers that read r15 raw */
    ei(e, 0xAA1303E0u);                                         /* mov x0, x19 */
    movx(e, 1, (uint64_t)(uintptr_t)d);
    movx(e, X16, (uint64_t)(uintptr_t)d->fn);
    ei(e, 0xD63F0200u);                                         /* blr x16 */
    greload(e);                                                 /* the handler may have changed any register */
}
static uint32_t *emit_dirty_check(emit_t *e)
{
    movx(e, X9, (uint64_t)(uintptr_t)&jit_dirty);
    ei(e, 0x39400000u | X9 << 5 | X9);                          /* ldrb w9, [x9] */
    return cbnz(e, X9);
}
static uint32_t *emit_next_check(emit_t *e, gaddr_t ipc) /* branch taken if next_pc != ipc + 4 */
{
    ldw(e, X9, OFF_NEXT);
    movw(e, X10, ipc + 4u);
    cmp_r(e, X9, X10);
    return bcond(e, C_NE);
}

/* Chooses the guest registers to cache for a block: up to seven of r0..r14
 * read at least twice in its instructions (an over-approximation of the
 * block, which may end earlier at a branch). */
static void jit_pick_cache(const uint8_t *code, uint32_t max)
{
    unsigned cnt[16] = {0};
    for (uint32_t k = 0; k < max; k++) {
        uint32_t insn;
        memcpy(&insn, code + 4u * k, 4);
        cnt[insn >> 16 & 15u]++;
        cnt[insn >> 12 & 15u]++;
        cnt[insn & 15u]++;
        if (((insn >> 25) & 7u) == 4u)           /* LDM/STM: the list */
            for (unsigned i = 0; i < 16; i++)
                cnt[i] += insn >> i & 1u;
        if (((insn >> 25) & 7u) == 5u || ((insn >> 24) & 15u) == 15u)
            break;                               /* B/BL or SWI ends the block */
    }
    for (unsigned g = 0; g < 16; g++) {
        gc[g] = -1;
        gl[g] = false;
    }
    for (unsigned slot = 0; slot < 7; slot++) {
        unsigned best = 15;
        for (unsigned g = 0; g < 15; g++)
            if (gc[g] < 0 && cnt[g] >= 2 && (best == 15 || cnt[g] > cnt[best]))
                best = g;
        if (best == 15)
            break;
        gc[best] = (int8_t)(22 + slot);
    }
}

static struct jblock *jit_translate(cpu_t *c, gaddr_t pc, const uint8_t *page_host)
{
    uint32_t first = (pc & (GP2X_PAGE_SIZE - 1u)) >> 2;
    uint32_t avail = GP2X_PAGE_SIZE / 4 - first;
    uint32_t max = avail < JIT_MAX_INSNS ? avail : JIT_MAX_INSNS;
    if (jit_used + max * (JIT_INSN_BYTES + JIT_STUB_BYTES) + 256 > JIT_BUF_SIZE)
        jit_flush_all();
    if (gmem_mark_code(c->m, pc, 4) == 0)
        return NULL;
    struct jblock *b = malloc(sizeof *b + max * sizeof(struct dentry));
    if (!b)
        return NULL;
    b->pc = pc;
    b->host = page_host + first * 4;
    b->host_page = page_host;
    uint32_t *start = (uint32_t *)(jit_buf + jit_used);
    emit_t em = { start }, *e = &em;
    uint32_t *exits[JIT_MAX_INSNS][3], *zexits[JIT_MAX_INSNS], *links[JIT_MAX_INSNS];
    unsigned nexit[JIT_MAX_INSNS];
    /* entry from C: x0 = cpu, w1 = budget (>= this block's length) */
    ei(e, 0xA9800000u | (0x72u << 15) | 30u << 10 | 31u << 5 | 29u);  /* stp x29, x30, [sp, #-112]! */
    for (unsigned k = 0; k < 5; k++)                                   /* stp x19..x28 at sp + 16.. */
        ei(e, 0xA9000000u | ((2u + 2u * k) << 15) | (20u + 2u * k) << 10 | 31u << 5 | (19u + 2u * k));
    ei(e, 0xAA0003F3u);                                                /* mov x19, x0 */
    mov_r(e, W20, 1);                                                  /* mov w20, w1 */
    ei(e, 0x52800000u | W21);                                          /* mov w21, #0 */
    uint32_t *to_body = b26(e);
    uint32_t *entry2 = e->p;                                           /* entry from a linked block */
    uint32_t *cmp_n = e->p;
    ei(e, 0x7100001Fu | W20 << 5);                                     /* cmp w20, #n (patched) */
    uint32_t *to_bexit = bcond(e, C_LO);
    patch(to_body, e->p);
    jit_pick_cache(b->host, max);                                      /* cached registers load on first use */
    uint32_t n = 0;
    for (; n < max; n++) {
        gaddr_t ipc = pc + 4u * n;
        uint32_t insn;
        memcpy(&insn, b->host + 4u * n, 4);
        struct dentry *d = &b->ents[n];
        decode(d, insn);
        nexit[n] = 0;
        zexits[n] = NULL;
        links[n] = NULL;
        unsigned cond = insn >> 28;
        int kind = jit_kind(d->fn);
        if (cond == 0xF)
            continue;
        uint32_t *skip = NULL;
        bool gl_before[16];
        memcpy(gl_before, gl, sizeof gl);
        if (cond != 0xE) {
            ldw(e, X9, OFF_CPSR);
            lsr_imm(e, X9, X9, 28);
            movw(e, X10, cond_lut[cond]);
            ei(e, 0x1AC02400u | X9 << 16 | X10 << 5 | X10);            /* lsr w10, w10, w9 */
            ei(e, 0x72000000u | X10 << 5 | X10);                       /* ands w10, w10, #1 */
            skip = bcond(e, C_EQ);
        }
        bool ends = false;
        if (d->fn == h_adr) {
            gwrite_imm(e, E_RD(d), ipc + 8u + d->aux);
        } else if (kind == JK_BRANCH) {
            if (d->fn == h_bl)
                gwrite_imm(e, 14, ipc + 4u);
            st_imm(e, OFF_NEXT, ipc + d->aux);
            if (((ipc + d->aux) & ~(gaddr_t)(GP2X_PAGE_SIZE - 1u)) == (pc & ~(gaddr_t)(GP2X_PAGE_SIZE - 1u)))
                links[n] = b26(e);
            else
                zexits[n] = b26(e);
            ends = true; /* a basic block ends at every branch (QEMU); both successors are linked */
        } else if (jit_inline(e, d) || jit_inline_flags(e, d) || jit_inline_shimm(e, d)) {
            /* done inline */
        } else if (d->fn == h_generic && ((insn >> 25) & 7u) == 4u && ((insn >> 16) & 15u) != 15u &&
                   !(((insn & 0xFFFFu) >> ((insn >> 16) & 15u)) & 1u) && (insn & 0xFFFFu)) {
            /* LDM/STM: inline when the words lie in one cached page */
            unsigned rn = E_RN(d);
            uint32_t list = insn & 0xFFFFu;
            bool p = (insn >> 24) & 1u, u = (insn >> 23) & 1u, w = (insn >> 21) & 1u, load = (insn >> 20) & 1u;
            unsigned count = (unsigned)__builtin_popcount(list);
            int32_t start_off = u ? (p ? 4 : 0) : (p ? -4 * (int32_t)count : -4 * (int32_t)count + 4);
            and_imm(e, W0, gread(e, rn, W0), ~3u);
            add_imm(e, W0, W0, (uint32_t)start_off);
            and_imm(e, W1, W0, GP2X_PAGE_SIZE - 1u);
            ei(e, 0x7100001Fu | (GP2X_PAGE_SIZE - 4u * count) << 10 | W1 << 5); /* cmp w1, #room */
            uint32_t *cross = bcond(e, C_HI);
            uint32_t *miss;
            emit_probe(e, load ? (uint32_t)offsetof(struct cpu, tlb_r) : (uint32_t)offsetof(struct cpu, tlb_w), 3u, &miss);
            ei(e, 0x8B000000u | W0 << 16 | X9 << 5 | X9);              /* add x9, x9, x0 */
            uint32_t k = 0;
            bool pc_loaded = false;
            for (unsigned i = 0; i < 16; i++) {
                if (!((list >> i) & 1u))
                    continue;
                if (load) {
                    ei(e, 0xB9400000u | k << 10 | X9 << 5 | W1);       /* ldr w1, [x9, #4k] */
                    if (i == 15) { and_imm(e, W1, W1, ~3u); stw(e, W1, OFF_NEXT); pc_loaded = true; }
                    else gwrite(e, i, W1);
                } else {
                    unsigned v = W1;
                    if (i == 15) movw(e, W1, ipc + 12u);                /* STM of pc: pc + 12 */
                    else v = gread(e, i, W1);
                    ei(e, 0xB9000000u | k << 10 | X9 << 5 | v);        /* str wv, [x9, #4k] */
                }
                k++;
            }
            if (w) {
                and_imm(e, W1, gread(e, rn, W1), ~3u);
                add_imm(e, W1, W1, u ? 4u * count : 0u - 4u * count);
                gwrite(e, rn, W1);
            }
            uint32_t *done = NULL;
            if (pc_loaded) zexits[n] = b26(e);
            else done = b26(e);
            patch(cross, e->p);
            patch(miss, e->p);
            emit_call(e, d, ipc);
            exits[n][nexit[n]++] = cbnz(e, W0);
            if (load)
                exits[n][nexit[n]++] = emit_next_check(e, ipc);     /* may have loaded pc (w0 = 0) */
            else
                exits[n][nexit[n]++] = emit_dirty_check(e);         /* w0 = 0 */
            if (done)
                patch(done, e->p);
            ends = cond == 0xE && pc_loaded;
        } else if (kind == JK_MEM || kind == JK_STORE) {
            int size;
            bool load;
            uint32_t *miss = NULL, *done = NULL;
            if (emit_addr(e, d, ipc, &size, &load)) {
                uint32_t am = size == MW ? 3u : (size == MH || size == MSH) ? 1u : 0u;
                emit_probe(e, load ? (uint32_t)offsetof(struct cpu, tlb_r) : (uint32_t)offsetof(struct cpu, tlb_w), am, &miss);
                if (load) {
                    static const uint32_t ld_ops[5] = { 0xB8606800u, 0x38606800u, 0x78606800u, 0x78E06800u, 0x38E06800u };
                    ei(e, ld_ops[size] | W0 << 16 | X9 << 5 | W0);     /* ldr* w0, [x9, x0] */
                    if (is_wb(d->fn))
                        emit_wb(e, d);
                    gwrite(e, E_RD(d), W0);
                } else {
                    static const uint32_t st_ops[3] = { 0xB8206800u, 0x38206800u, 0x78206800u };
                    unsigned v = gread(e, E_RD(d), W1);
                    ei(e, st_ops[size] | W0 << 16 | X9 << 5 | v);      /* str* wv, [x9, x0] */
                    if (is_wb(d->fn))
                        emit_wb(e, d);
                }
                done = b26(e);
                patch(miss, e->p);
            }
            emit_call(e, d, ipc);
            exits[n][nexit[n]++] = cbnz(e, W0);
            if (kind == JK_STORE)
                zexits[n] = emit_dirty_check(e);
            if (done)
                patch(done, e->p);
        } else {
            emit_call(e, d, ipc);
            if (kind != JK_ALU)
                exits[n][nexit[n]++] = cbnz(e, W0);
            if (kind == JK_ANY) {
                exits[n][nexit[n]++] = emit_next_check(e, ipc);
                ends = cond == 0xE && (d->fn == h_bx || d->fn == h_mov_pc);
            }
            if (kind == JK_STORE)
                zexits[n] = emit_dirty_check(e);
        }
        if (skip) {
            patch(skip, e->p);
            memcpy(gl, gl_before, sizeof gl); /* the instruction may have been skipped */
        }
        if (ends || ((insn >> 24) & 0xFu) == 0xFu) {
            n++;
            break;
        }
    }
    if (n == 0) {
        free(b);
        return NULL;
    }
    st_imm(e, OFF_NEXT, pc + 4u * n);
    uint32_t *to_last = NULL, *fall_link = NULL;
    if (((pc + 4u * n) & (GP2X_PAGE_SIZE - 1u)) != 0) {
        fall_link = b26(e);
    } else {
        ei(e, 0x52800000u | W0);                                       /* mov w0, #0 */
        to_last = b26(e);
    }
    uint32_t *epi_jumps[3 * JIT_MAX_INSNS + 2];
    unsigned nepi = 0;
    uint32_t *stub_at[JIT_MAX_INSNS];
    for (uint32_t i = 0; i <= n; i++) {
        uint32_t *site = i < n ? links[i] : fall_link;
        if (!site)
            continue;
        uint32_t retired = i < n ? i + 1u : n;
        patch(site, e->p);
        ei(e, 0x11000000u | retired << 10 | W21 << 5 | W21);          /* add w21, w21, #retired */
        ei(e, 0x51000000u | retired << 10 | W20 << 5 | W20);          /* sub w20, w20, #retired */
        uint32_t *link_b = b26(e);
        patch(link_b, e->p);                                           /* unlinked: record and leave */
        movx(e, X9, (uint64_t)(uintptr_t)link_b);
        stx(e, X9, OFF_JLINK_AT);
        movx(e, X9, (uint64_t)(uintptr_t)page_host);
        stx(e, X9, OFF_JLINK_PAGE);
        stw(e, ZR, OFF_JREASON);
        mov_r(e, W0, W21);
        epi_jumps[nepi++] = b26(e);
    }
    patch(to_bexit, e->p);                                             /* budget exit of the second entry */
    st_imm(e, OFF_NEXT, pc);
    stw(e, ZR, OFF_JREASON);
    mov_r(e, W0, W21);
    epi_jumps[nepi++] = b26(e);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t *zstub = NULL;
        if (zexits[i]) {
            zstub = e->p;
            ei(e, 0x52800000u | W0);                                   /* mov w0, #0 */
        }
        stub_at[i] = e->p;
        stw(e, W0, OFF_JREASON);
        ei(e, 0x11000000u | (i + 1u) << 10 | W21 << 5 | W0);          /* add w0, w21, #retired */
        epi_jumps[nepi++] = b26(e);
        for (unsigned k = 0; k < nexit[i]; k++)
            patch(exits[i][k], stub_at[i]);
        if (zexits[i])
            patch(zexits[i], zstub);
    }
    if (to_last)
        patch(to_last, stub_at[n - 1]);
    uint32_t *epi = e->p;
    ldw(e, W1, OFF_NEXT);
    stw(e, W1, OFF_R(15));                                             /* R15 = next_pc */
    for (unsigned k = 0; k < 5; k++)                                   /* ldp x19..x28 */
        ei(e, 0xA9400000u | ((2u + 2u * k) << 15) | (20u + 2u * k) << 10 | 31u << 5 | (19u + 2u * k));
    ei(e, 0xA8C00000u | (14u << 15) | 30u << 10 | 31u << 5 | 29u);     /* ldp x29, x30, [sp], #112 */
    ei(e, 0xD65F03C0u);                                                /* ret */
    for (unsigned i = 0; i < nepi; i++)
        patch(epi_jumps[i], epi);
    *cmp_n |= n << 10;
    if (gmem_mark_code(c->m, pc, 4u * n) == 2 || c->tlb_code_epoch != gmem_code_epoch()) {
        for (unsigned i = 0; i < TLB_SIZE; i++)
            c->tlb_w[i].page = TLB_EMPTY;
        c->tlb_code_epoch = gmem_code_epoch();
    }
    __builtin___clear_cache((char *)start, (char *)e->p);
    jit_used += (size_t)((uint8_t *)e->p - (uint8_t *)start);
    jit_used = (jit_used + 15u) & ~(size_t)15u;
    b->n = n;
    b->code = (uint32_t (*)(cpu_t *, uint32_t))(void *)start;
    b->entry2 = (uint8_t *)entry2;
    uint32_t h = jit_bucket(pc, b->host);
    b->next = jit_hash[h];
    jit_hash[h] = b;
    return b;
}
