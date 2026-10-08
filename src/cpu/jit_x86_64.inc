/* x86-64 backend of the JIT (docs/JIT.md), included by cpu.c after jit_common.inc. */

/* ---- emitter */

typedef struct { uint8_t *p; } emit_t;
static void e8(emit_t *e, uint8_t v) { *e->p++ = v; }
static void e32(emit_t *e, uint32_t v) { memcpy(e->p, &v, 4); e->p += 4; }
static void e64(emit_t *e, uint64_t v) { memcpy(e->p, &v, 8); e->p += 8; }
/* reg: 0 = eax, 1 = ecx; [rbx + disp32] */
static void ld_reg(emit_t *e, unsigned reg, uint32_t off) { e8(e, 0x8B); e8(e, (uint8_t)(0x83 | reg << 3)); e32(e, off); }
static void st_reg(emit_t *e, unsigned reg, uint32_t off) { e8(e, 0x89); e8(e, (uint8_t)(0x83 | reg << 3)); e32(e, off); }
static void st_imm(emit_t *e, uint32_t off, uint32_t imm) { e8(e, 0xC7); e8(e, 0x83); e32(e, off); e32(e, imm); }
static void alu_eax_mem(emit_t *e, uint8_t opc, uint32_t off) { e8(e, opc); e8(e, 0x83); e32(e, off); } /* op eax, [rbx+off] */
static void alu_eax_imm(emit_t *e, uint8_t opc, uint32_t imm) { e8(e, opc); e32(e, imm); }             /* op eax, imm32 */
static uint8_t *jcc32(emit_t *e, uint8_t cc) { e8(e, 0x0F); e8(e, cc); uint8_t *at = e->p; e32(e, 0); return at; }
static uint8_t *jmp32(emit_t *e) { e8(e, 0xE9); uint8_t *at = e->p; e32(e, 0); return at; }
static void patch(uint8_t *at, const uint8_t *target) { int32_t rel = (int32_t)(target - (at + 4)); memcpy(at, &rel, 4); }


/* Inline forms; returns false if the handler has none. */
static bool jit_inline(emit_t *e, const struct dentry *d)
{
    uint32_t rd = OFF_R(E_RD(d)), rn = OFF_R(E_RN(d)), rm = OFF_R(E_RM(d));
    dhandler_t f = d->fn;
    if (f == h_mov_imm) { st_imm(e, rd, d->aux); return true; }
    if (f == h_mov_reg) { ld_reg(e, 0, rm); st_reg(e, 0, rd); return true; }
    if (f == h_mvn_reg) { ld_reg(e, 0, rm); e8(e, 0xF7); e8(e, 0xD0); st_reg(e, 0, rd); return true; }
    static const struct { dhandler_t f; uint8_t opc; } imm_ops[] = {
        { h_add_imm, 0x05 }, { h_and_imm, 0x25 }, { h_orr_imm, 0x0D }, { h_eor_imm, 0x35 },
    };
    for (unsigned i = 0; i < sizeof imm_ops / sizeof *imm_ops; i++)
        if (f == imm_ops[i].f) { ld_reg(e, 0, rn); alu_eax_imm(e, imm_ops[i].opc, d->aux); st_reg(e, 0, rd); return true; }
    if (f == h_rsb_imm) { e8(e, 0xB8); e32(e, d->aux); alu_eax_mem(e, 0x2B, rn); st_reg(e, 0, rd); return true; }
    static const struct { dhandler_t f; uint8_t opc; } reg_ops[] = {
        { h_add_reg, 0x03 }, { h_sub_reg, 0x2B }, { h_and_reg, 0x23 }, { h_orr_reg, 0x0B }, { h_eor_reg, 0x33 },
    };
    for (unsigned i = 0; i < sizeof reg_ops / sizeof *reg_ops; i++)
        if (f == reg_ops[i].f) { ld_reg(e, 0, rn); alu_eax_mem(e, reg_ops[i].opc, rm); st_reg(e, 0, rd); return true; }
    if (f == h_rsb_reg) { ld_reg(e, 0, rm); alu_eax_mem(e, 0x2B, rn); st_reg(e, 0, rd); return true; }
    if (f == h_bic_reg) { ld_reg(e, 1, rm); e8(e, 0xF7); e8(e, 0xD1); ld_reg(e, 0, rn); e8(e, 0x21); e8(e, 0xC8); st_reg(e, 0, rd); return true; }
    if (f == h_adr) { return false; } /* needs the instruction's pc: emitted by the caller */
    return false;
}


/* ---- inline memory access: probe the cpu's page cache, call the handler on a miss
 *
 * eax = guest address. The cache entry index is (addr >> 12) & (TLB_SIZE-1);
 * the entry matches when its page equals addr & ~0xFFF with the alignment
 * bits kept (so an unaligned word or halfword never matches and takes the
 * handler, which applies the ARMv4 rotation or forced alignment). */
_Static_assert(sizeof(struct tlb_entry) == 16 && offsetof(struct tlb_entry, host) == 8, "inline probe layout");
_Static_assert(TLB_SIZE == 1024u, "inline probe mask");
static void emit_probe(emit_t *e, uint32_t tlb_off, uint32_t align_mask, uint8_t **miss)
{
    e8(e, 0x89); e8(e, 0xC1);                               /* mov ecx, eax */
    e8(e, 0xC1); e8(e, 0xE9); e8(e, 0x0C);                  /* shr ecx, 12 */
    e8(e, 0x81); e8(e, 0xE1); e32(e, TLB_SIZE - 1u);        /* and ecx, 1023 */
    e8(e, 0xC1); e8(e, 0xE1); e8(e, 0x04);                  /* shl ecx, 4 */
    e8(e, 0x89); e8(e, 0xC2);                               /* mov edx, eax */
    e8(e, 0x81); e8(e, 0xE2); e32(e, 0xFFFFF000u | align_mask); /* and edx, mask */
    e8(e, 0x3B); e8(e, 0x94); e8(e, 0x0B); e32(e, tlb_off); /* cmp edx, [rbx+rcx+tlb] */
    *miss = jcc32(e, 0x85);                                 /* jne miss */
    e8(e, 0x48); e8(e, 0x8B); e8(e, 0x94); e8(e, 0x0B); e32(e, tlb_off + 8u); /* mov rdx, [rbx+rcx+tlb+8] */
    e8(e, 0x25); e32(e, GP2X_PAGE_SIZE - 1u);               /* and eax, 0xFFF */
}

/* Address of a pre-indexed, no-writeback access into eax; false if not handled. */
static bool emit_addr(emit_t *e, const struct dentry *d, gaddr_t ipc, int *size, bool *load)
{
    dhandler_t f = d->fn;
    uint32_t rn = OFF_R(E_RN(d)), rm = OFF_R(E_RM(d));
    if (f == h_ldr_imm || f == h_ldrb_imm || f == h_str_imm || f == h_strb_imm ||
        f == h_ldrh_imm || f == h_ldrsh_imm || f == h_ldrsb_imm || f == h_strh_imm) {
        ld_reg(e, 0, rn);
        if (d->aux)
            alu_eax_imm(e, 0x05, d->aux);
    } else if (f == h_ldr_lit) {
        e8(e, 0xB8); e32(e, ipc + 8u + d->aux);
    } else if (f == h_ldr_reg || f == h_ldrb_reg || f == h_str_reg || f == h_strb_reg) {
        ld_reg(e, 0, rm);
        if (d->aux & 31u) { e8(e, 0xC1); e8(e, 0xE0); e8(e, (uint8_t)(d->aux & 31u)); } /* shl eax, n */
        if (d->aux & 0x100u) {
            alu_eax_mem(e, 0x03, rn);                                      /* add eax, [rn] */
        } else {
            ld_reg(e, 1, rn);
            e8(e, 0x29); e8(e, 0xC1);                                      /* sub ecx, eax */
            e8(e, 0x89); e8(e, 0xC8);                                      /* mov eax, ecx */
        }
    } else if (f == h_ldrh_reg || f == h_ldrsh_reg || f == h_ldrsb_reg || f == h_strh_reg) {
        ld_reg(e, 0, rn);
        alu_eax_mem(e, d->aux ? 0x03 : 0x2B, rm);                          /* add/sub eax, [rm] */
    } else if (f == h_ldr_wb || f == h_ldrb_wb || f == h_str_wb || f == h_strb_wb) {
        ld_reg(e, 0, rn);
        if (E_PRE(d) && d->aux)
            alu_eax_imm(e, 0x05, d->aux);                                  /* pre-indexed */
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

/* Shifted-register ALU without flags (h_dp_shimm). */
static bool jit_inline_shimm(emit_t *e, const struct dentry *d)
{
    if (d->fn != h_dp_shimm)
        return false;
    uint32_t insn = d->insn, rd = OFF_R(E_RD(d)), rn = OFF_R(E_RN(d)), amt = (insn >> 7) & 31u;
    ld_reg(e, 0, OFF_R(E_RM(d)));
    static const uint8_t shift_modrm[3] = { 0xE0, 0xE8, 0xF8 };      /* shl, shr, sar eax */
    e8(e, 0xC1); e8(e, shift_modrm[(insn >> 5) & 3u]); e8(e, (uint8_t)amt);
    switch ((insn >> 21) & 15u) {
    case 0x0: alu_eax_mem(e, 0x23, rn); break;                       /* and */
    case 0x1: alu_eax_mem(e, 0x33, rn); break;                       /* xor */
    case 0x2: ld_reg(e, 1, rn); e8(e, 0x29); e8(e, 0xC1); e8(e, 0x89); e8(e, 0xC8); break; /* rn - op2 */
    case 0x3: alu_eax_mem(e, 0x2B, rn); break;                       /* op2 - rn */
    case 0x4: alu_eax_mem(e, 0x03, rn); break;                       /* add */
    case 0xC: alu_eax_mem(e, 0x0B, rn); break;                       /* or */
    case 0xD: break;                                                 /* mov */
    case 0xE: e8(e, 0xF7); e8(e, 0xD0); alu_eax_mem(e, 0x23, rn); break; /* bic */
    default: e8(e, 0xF7); e8(e, 0xD0); break;                        /* mvn */
    }
    st_reg(e, 0, rd);
    return true;
}

/* LDM/STM whose base is not in the list and whose words lie in one cached
 * page: plain host loads and stores. Returns the miss jump (the caller emits
 * the handler call there) or NULL if this form is not inlined; *pc_loaded is
 * set for an LDM that loads pc (next_pc then holds the target). */
static uint8_t *jit_inline_ldm_stm(emit_t *e, const struct dentry *d, gaddr_t ipc, uint8_t **done, bool *pc_loaded,
                                   uint8_t **zexit)
{
    uint32_t insn = d->insn;
    if (d->fn != h_generic || ((insn >> 25) & 7u) != 4u)
        return NULL;
    unsigned rn = E_RN(d);
    uint32_t list = insn & 0xFFFFu;
    bool p = (insn >> 24) & 1u, u = (insn >> 23) & 1u, w = (insn >> 21) & 1u, load = (insn >> 20) & 1u;
    unsigned count = (unsigned)__builtin_popcount(list);
    if (!count || rn == 15 || (list >> rn) & 1u)
        return NULL;
    int32_t start_off = u ? (p ? 4 : 0) : (p ? -4 * (int32_t)count : -4 * (int32_t)count + 4);
    ld_reg(e, 0, OFF_R(rn));
    e8(e, 0x83); e8(e, 0xE0); e8(e, 0xFC);                   /* and eax, ~3 */
    if (start_off)
        alu_eax_imm(e, 0x05, (uint32_t)start_off);
    e8(e, 0x89); e8(e, 0xC1);                                /* mov ecx, eax */
    e8(e, 0x81); e8(e, 0xE1); e32(e, GP2X_PAGE_SIZE - 1u);   /* and ecx, 0xFFF */
    e8(e, 0x81); e8(e, 0xF9); e32(e, GP2X_PAGE_SIZE - 4u * count); /* cmp ecx, room */
    uint8_t *cross = jcc32(e, 0x87);                         /* ja: crosses a page */
    uint8_t *miss;
    emit_probe(e, load ? (uint32_t)offsetof(struct cpu, tlb_r) : (uint32_t)offsetof(struct cpu, tlb_w), 3u, &miss);
    uint32_t k = 0;
    *pc_loaded = false;
    for (unsigned i = 0; i < 16; i++) {
        if (!((list >> i) & 1u))
            continue;
        if (load) {
            e8(e, 0x8B); e8(e, 0x8C); e8(e, 0x02); e32(e, 4u * k); /* mov ecx, [rdx+rax+4k] */
            if (i == 15) {
                e8(e, 0x83); e8(e, 0xE1); e8(e, 0xFC);              /* and ecx, ~3 */
                st_reg(e, 1, OFF_NEXT);
                *pc_loaded = true;
            } else {
                st_reg(e, 1, OFF_R(i));
            }
        } else {
            if (i == 15) { e8(e, 0xB9); e32(e, ipc + 12u); }        /* STM of pc: pc + 12 */
            else ld_reg(e, 1, OFF_R(i));
            e8(e, 0x89); e8(e, 0x8C); e8(e, 0x02); e32(e, 4u * k);  /* mov [rdx+rax+4k], ecx */
        }
        k++;
    }
    if (w) {
        ld_reg(e, 1, OFF_R(rn));
        e8(e, 0x83); e8(e, 0xE1); e8(e, 0xFC);                      /* and ecx, ~3 */
        e8(e, 0x81); e8(e, 0xC1); e32(e, u ? 4u * count : 0u - 4u * count); /* add ecx, +-4n */
        st_reg(e, 1, OFF_R(rn));
    }
    if (*pc_loaded) {          /* leave with next_pc = the loaded target */
        *zexit = jmp32(e);
        *done = NULL;
    } else {
        *done = jmp32(e);
    }
    patch(cross, e->p);        /* both misses: the handler call the caller emits next */
    patch(miss, e->p);
    return e->p;
}

/* Flags from the host's after an arithmetic op: cl = N, ch = Z, dl = C, dh = V
 * (carry_is_borrow: C = !CF, for subtraction), then merged into CPSR. */
static void emit_capture_nzcv(emit_t *e, bool carry_is_borrow)
{
    e8(e, 0x0F); e8(e, 0x98); e8(e, 0xC1);                       /* sets cl */
    e8(e, 0x0F); e8(e, 0x94); e8(e, 0xC5);                       /* setz ch */
    e8(e, 0x0F); e8(e, carry_is_borrow ? 0x93 : 0x92); e8(e, 0xC2); /* setnc / setc dl */
    e8(e, 0x0F); e8(e, 0x90); e8(e, 0xC6);                       /* seto dh */
}
static void emit_merge_nzcv(emit_t *e)
{
    e8(e, 0x0F); e8(e, 0xB6); e8(e, 0xC1); /* movzx eax, cl */
    e8(e, 0x01); e8(e, 0xC0);              /* add eax, eax */
    e8(e, 0x0F); e8(e, 0xB6); e8(e, 0xCD); /* movzx ecx, ch */
    e8(e, 0x09); e8(e, 0xC8);              /* or eax, ecx */
    e8(e, 0x01); e8(e, 0xC0);
    e8(e, 0x0F); e8(e, 0xB6); e8(e, 0xCA); /* movzx ecx, dl */
    e8(e, 0x09); e8(e, 0xC8);
    e8(e, 0x01); e8(e, 0xC0);
    e8(e, 0x0F); e8(e, 0xB6); e8(e, 0xCE); /* movzx ecx, dh */
    e8(e, 0x09); e8(e, 0xC8);
    e8(e, 0xC1); e8(e, 0xE0); e8(e, 0x1C); /* shl eax, 28 */
    ld_reg(e, 1, OFF_CPSR);
    e8(e, 0x81); e8(e, 0xE1); e32(e, 0x0FFFFFFFu); /* and ecx, 0x0FFFFFFF */
    e8(e, 0x09); e8(e, 0xC1);              /* or ecx, eax */
    st_reg(e, 1, OFF_CPSR);
}
/* N and Z only (TST): C and V are kept. */
static void emit_merge_nz(emit_t *e)
{
    e8(e, 0x0F); e8(e, 0xB6); e8(e, 0xC1); /* movzx eax, cl */
    e8(e, 0x01); e8(e, 0xC0);
    e8(e, 0x0F); e8(e, 0xB6); e8(e, 0xCD); /* movzx ecx, ch */
    e8(e, 0x09); e8(e, 0xC8);
    e8(e, 0xC1); e8(e, 0xE0); e8(e, 0x1E); /* shl eax, 30 */
    ld_reg(e, 1, OFF_CPSR);
    e8(e, 0x81); e8(e, 0xE1); e32(e, 0x3FFFFFFFu);
    e8(e, 0x09); e8(e, 0xC1);
    st_reg(e, 1, OFF_CPSR);
}
/* CMP/CMN/TST/SUBS/ADDS with an immediate or an unshifted register. */
static bool jit_inline_flags(emit_t *e, const struct dentry *d)
{
    dhandler_t f = d->fn;
    uint32_t rd = OFF_R(E_RD(d)), rn = OFF_R(E_RN(d)), rm = OFF_R(E_RM(d));
    bool imm = f == h_cmp_imm || f == h_cmn_imm || f == h_tst_imm || f == h_subs_imm || f == h_adds_imm;
    bool reg = f == h_cmp_reg || f == h_cmn_reg || f == h_tst_reg || f == h_subs_reg || f == h_adds_reg;
    if (!imm && !reg)
        return false;
    ld_reg(e, 0, rn);
    uint8_t op_imm, op_reg;
    bool borrow = false, store = false, nz_only = false;
    if (f == h_cmp_imm || f == h_cmp_reg) { op_imm = 0x3D; op_reg = 0x3B; borrow = true; }      /* cmp */
    else if (f == h_subs_imm || f == h_subs_reg) { op_imm = 0x2D; op_reg = 0x2B; borrow = true; store = true; } /* sub */
    else if (f == h_cmn_imm || f == h_cmn_reg) { op_imm = 0x05; op_reg = 0x03; }                 /* add, result dropped */
    else if (f == h_adds_imm || f == h_adds_reg) { op_imm = 0x05; op_reg = 0x03; store = true; }
    else { op_imm = 0xA9; op_reg = 0x85; nz_only = true; }                                       /* test */
    if (imm)
        alu_eax_imm(e, op_imm, d->aux);
    else if (op_reg == 0x85) { ld_reg(e, 1, rm); e8(e, 0x85); e8(e, 0xC8); } /* test eax, ecx */
    else
        alu_eax_mem(e, op_reg, rm);
    emit_capture_nzcv(e, borrow);
    if (store)
        st_reg(e, 0, rd);
    if (nz_only)
        emit_merge_nz(e);
    else
        emit_merge_nzcv(e);
    return true;
}

static void emit_call(emit_t *e, const struct dentry *d)
{
    e8(e, 0x48); e8(e, 0x89); e8(e, 0xDF);                    /* mov rdi, rbx */
    e8(e, 0x48); e8(e, 0xBE); e64(e, (uint64_t)(uintptr_t)d); /* mov rsi, entry */
    e8(e, 0x48); e8(e, 0xB8); e64(e, (uint64_t)(uintptr_t)d->fn); /* mov rax, handler */
    e8(e, 0xFF); e8(e, 0xD0);                                 /* call rax */
}

static struct jblock *jit_translate(cpu_t *c, gaddr_t pc, const uint8_t *page_host)
{
    uint32_t first = (pc & (GP2X_PAGE_SIZE - 1u)) >> 2;
    uint32_t avail = GP2X_PAGE_SIZE / 4 - first;
    uint32_t max = avail < JIT_MAX_INSNS ? avail : JIT_MAX_INSNS;
    if (jit_used + max * (JIT_INSN_BYTES + JIT_STUB_BYTES) + 64 > JIT_BUF_SIZE)
        jit_flush_all();
    if (gmem_mark_code(c->m, pc, 4) == 0) /* refused pages are not translated */
        return NULL;
    struct jblock *b = malloc(sizeof *b + max * sizeof(struct dentry));
    if (!b)
        return NULL;
    b->pc = pc;
    b->host = page_host + first * 4;
    b->host_page = page_host;
    uint8_t *start = jit_buf + jit_used;
    emit_t em = { start }, *e = &em;
    uint8_t *exits[JIT_MAX_INSNS][3];      /* sites jumping to stub i (reason in eax) */
    uint8_t *zexits[JIT_MAX_INSNS];        /* sites jumping to stub i with reason 0 */
    unsigned nexit[JIT_MAX_INSNS];
    e8(e, 0x53);                           /* push rbx */
    e8(e, 0x41); e8(e, 0x54);              /* push r12 */
    e8(e, 0x41); e8(e, 0x55);              /* push r13 */
    e8(e, 0x48); e8(e, 0x89); e8(e, 0xFB); /* mov rbx, rdi */
    e8(e, 0x41); e8(e, 0x89); e8(e, 0xF4); /* mov r12d, esi */
    e8(e, 0x45); e8(e, 0x31); e8(e, 0xED); /* xor r13d, r13d */
    uint8_t *to_body = jmp32(e);
    uint8_t *entry2 = e->p;
    e8(e, 0x41); e8(e, 0x81); e8(e, 0xFC); /* cmp r12d, n */
    uint8_t *n_at = e->p;
    e32(e, 0);
    uint8_t *to_bexit = jcc32(e, 0x82);    /* jb budget exit */
    patch(to_body, e->p);
    uint8_t *links[JIT_MAX_INSNS + 1];     /* per instruction: a link site (or NULL); [n] = fall-through */
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
        if (cond == 0xF) { /* never executes on ARMv4: counts only */
            continue;
        }
        uint8_t *skip = NULL;
        if (cond != 0xE) {
            ld_reg(e, 0, OFF_CPSR);
            e8(e, 0xC1); e8(e, 0xE8); e8(e, 0x1C);  /* shr eax, 28 */
            e8(e, 0xB9); e32(e, cond_lut[cond]);    /* mov ecx, lut */
            e8(e, 0x0F); e8(e, 0xA3); e8(e, 0xC1);  /* bt ecx, eax */
            skip = jcc32(e, 0x83);                  /* jnc */
        }
        bool ends = false;
        if (d->fn == h_adr) {
            st_imm(e, OFF_R(E_RD(d)), ipc + 8u + d->aux);
        } else if (kind == JK_BRANCH) {
            if (d->fn == h_bl)
                st_imm(e, OFF_R(14), ipc + 4u);
            st_imm(e, OFF_NEXT, ipc + d->aux);
            if (((ipc + d->aux) & ~(gaddr_t)(GP2X_PAGE_SIZE - 1u)) == (pc & ~(gaddr_t)(GP2X_PAGE_SIZE - 1u)))
                links[n] = jmp32(e); /* to this instruction's link stub */
            else
                zexits[n] = jmp32(e);
            ends = true; /* a basic block ends at every branch (QEMU); both successors are linked */
        } else if (jit_inline(e, d) || jit_inline_flags(e, d) || jit_inline_shimm(e, d)) {
            /* done inline */
        } else if (d->fn == h_generic && ((insn >> 25) & 7u) == 4u && ((insn >> 16) & 15u) != 15u &&
                   !(((insn & 0xFFFFu) >> ((insn >> 16) & 15u)) & 1u) && (insn & 0xFFFFu)) {
            /* LDM/STM: inline when the words lie in one cached page, else the handler */
            uint8_t *done = NULL;
            bool pc_loaded = false;
            bool is_load = (insn >> 20) & 1u;
            jit_inline_ldm_stm(e, d, ipc, &done, &pc_loaded, &zexits[n]);
            /* slow path: page crossing or cache miss */
            st_imm(e, OFF_PC, ipc);
            st_imm(e, OFF_NEXT, ipc + 4u);
            st_imm(e, OFF_R(15), ipc);
            emit_call(e, d);
            e8(e, 0x85); e8(e, 0xC0);
            exits[n][nexit[n]++] = jcc32(e, 0x85);
            if (is_load) {                         /* may have loaded pc */
                e8(e, 0x81); e8(e, 0xBB); e32(e, OFF_NEXT); e32(e, ipc + 4u);
                exits[n][nexit[n]++] = jcc32(e, 0x85);
            } else {
                e8(e, 0x48); e8(e, 0xB8); e64(e, (uint64_t)(uintptr_t)&jit_dirty);
                e8(e, 0x80); e8(e, 0x38); e8(e, 0x00);
                exits[n][nexit[n]++] = jcc32(e, 0x85); /* eax is 0 here */
            }
            if (done)
                patch(done, e->p);
            ends = cond == 0xE && pc_loaded;
        } else if (kind == JK_MEM || kind == JK_STORE) {
            int size;
            bool load;
            uint8_t *miss = NULL, *done = NULL;
            if (emit_addr(e, d, ipc, &size, &load)) {
                uint32_t am = size == MW ? 3u : (size == MH || size == MSH) ? 1u : 0u;
                emit_probe(e, load ? (uint32_t)offsetof(struct cpu, tlb_r) : (uint32_t)offsetof(struct cpu, tlb_w), am, &miss);
                if (load) {
                    switch (size) {
                    case MW:  e8(e, 0x8B); e8(e, 0x04); e8(e, 0x02); break;               /* mov eax, [rdx+rax] */
                    case MB:  e8(e, 0x0F); e8(e, 0xB6); e8(e, 0x04); e8(e, 0x02); break; /* movzx eax, byte */
                    case MH:  e8(e, 0x0F); e8(e, 0xB7); e8(e, 0x04); e8(e, 0x02); break; /* movzx eax, word */
                    case MSH: e8(e, 0x0F); e8(e, 0xBF); e8(e, 0x04); e8(e, 0x02); break; /* movsx eax, word */
                    default:  e8(e, 0x0F); e8(e, 0xBE); e8(e, 0x04); e8(e, 0x02); break; /* movsx eax, byte */
                    }
                    if (is_wb(d->fn)) {            /* base update before the destination, as the handler */
                        ld_reg(e, 1, OFF_R(E_RN(d)));
                        e8(e, 0x81); e8(e, 0xC1); e32(e, d->aux);  /* add ecx, aux */
                        st_reg(e, 1, OFF_R(E_RN(d)));
                    }
                    st_reg(e, 0, OFF_R(E_RD(d)));
                } else {
                    ld_reg(e, 1, OFF_R(E_RD(d)));
                    switch (size) {
                    case MW: e8(e, 0x89); e8(e, 0x0C); e8(e, 0x02); break;               /* mov [rdx+rax], ecx */
                    case MB: e8(e, 0x88); e8(e, 0x0C); e8(e, 0x02); break;               /* mov [rdx+rax], cl */
                    default: e8(e, 0x66); e8(e, 0x89); e8(e, 0x0C); e8(e, 0x02); break;  /* mov [rdx+rax], cx */
                    }
                    if (is_wb(d->fn)) {
                        ld_reg(e, 1, OFF_R(E_RN(d)));
                        e8(e, 0x81); e8(e, 0xC1); e32(e, d->aux);
                        st_reg(e, 1, OFF_R(E_RN(d)));
                    }
                }
                done = jmp32(e);
                patch(miss, e->p);
            }
            st_imm(e, OFF_PC, ipc);
            st_imm(e, OFF_NEXT, ipc + 4u);
            st_imm(e, OFF_R(15), ipc);
            emit_call(e, d);
            e8(e, 0x85); e8(e, 0xC0);              /* test eax, eax */
            exits[n][nexit[n]++] = jcc32(e, 0x85);
            if (kind == JK_STORE) {
                e8(e, 0x48); e8(e, 0xB8); e64(e, (uint64_t)(uintptr_t)&jit_dirty);
                e8(e, 0x80); e8(e, 0x38); e8(e, 0x00);
                zexits[n] = jcc32(e, 0x85);
            }
            if (done)
                patch(done, e->p);
        } else {
            st_imm(e, OFF_PC, ipc);
            st_imm(e, OFF_NEXT, ipc + 4u);
            st_imm(e, OFF_R(15), ipc); /* handlers that read r15 raw see the interpreter's value */
            emit_call(e, d);
            if (kind != JK_ALU) {
                e8(e, 0x85); e8(e, 0xC0);           /* test eax, eax */
                exits[n][nexit[n]++] = jcc32(e, 0x85); /* jnz stub */
            }
            if (kind == JK_ANY) {                   /* eax is 0 here */
                e8(e, 0x81); e8(e, 0xBB); e32(e, OFF_NEXT); e32(e, ipc + 4u); /* cmp [next_pc], pc+4 */
                exits[n][nexit[n]++] = jcc32(e, 0x85);
                ends = cond == 0xE && (d->fn == h_bx || d->fn == h_mov_pc);
            }
            if (kind == JK_STORE) {
                e8(e, 0x48); e8(e, 0xB8); e64(e, (uint64_t)(uintptr_t)&jit_dirty); /* mov rax, &jit_dirty */
                e8(e, 0x80); e8(e, 0x38); e8(e, 0x00);                              /* cmp byte [rax], 0 */
                uint8_t *z = jcc32(e, 0x85);
                zexits[n] = z;
            }
        }
        if (skip)
            patch(skip, e->p);
        if (ends || ((insn >> 24) & 0xFu) == 0xFu) { /* unconditional transfer, or SWI */
            n++;
            break;
        }
    }
    if (n == 0) {
        free(b);
        return NULL;
    }
    /* fall-through end: next_pc = the instruction after the block */
    st_imm(e, OFF_NEXT, pc + 4u * n);
    uint8_t *to_last = NULL, *fall_link = NULL;
    if (((pc + 4u * n) & (GP2X_PAGE_SIZE - 1u)) != 0) {
        fall_link = jmp32(e);
    } else {
        e8(e, 0x31); e8(e, 0xC0);              /* xor eax, eax */
        to_last = jmp32(e);
    }
    uint8_t *epilogue_jumps[3 * JIT_MAX_INSNS + 2];
    unsigned nepi = 0;
    uint8_t *stub_at[JIT_MAX_INSNS];
    /* link stubs: account the instructions retired, then jump on (patched later) */
    for (uint32_t i = 0; i <= n; i++) {
        uint8_t *site = i < n ? links[i] : fall_link;
        if (!site)
            continue;
        uint32_t retired = i < n ? i + 1u : n;
        patch(site, e->p);
        e8(e, 0x41); e8(e, 0x81); e8(e, 0xC5); e32(e, retired); /* add r13d, retired */
        e8(e, 0x41); e8(e, 0x81); e8(e, 0xEC); e32(e, retired); /* sub r12d, retired */
        uint8_t *link_rel = jmp32(e);
        patch(link_rel, e->p);                                  /* unlinked: fall into the recorder */
        e8(e, 0x48); e8(e, 0xB8); e64(e, (uint64_t)(uintptr_t)link_rel); /* mov rax, &rel32 */
        e8(e, 0x48); e8(e, 0x89); e8(e, 0x83); e32(e, OFF_JLINK_AT);       /* mov [rbx+jlink_at], rax */
        e8(e, 0x48); e8(e, 0xB8); e64(e, (uint64_t)(uintptr_t)page_host);
        e8(e, 0x48); e8(e, 0x89); e8(e, 0x83); e32(e, OFF_JLINK_PAGE);
        e8(e, 0x31); e8(e, 0xC0);                               /* xor eax, eax */
        st_reg(e, 0, OFF_JREASON);
        e8(e, 0x44); e8(e, 0x89); e8(e, 0xE8);                  /* mov eax, r13d */
        epilogue_jumps[nepi++] = jmp32(e);
    }
    /* budget exit of the second entry: nothing retired in this block */
    patch(to_bexit, e->p);
    st_imm(e, OFF_NEXT, pc);
    e8(e, 0x31); e8(e, 0xC0);
    st_reg(e, 0, OFF_JREASON);
    e8(e, 0x44); e8(e, 0x89); e8(e, 0xE8);                      /* mov eax, r13d */
    epilogue_jumps[nepi++] = jmp32(e);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *zstub = NULL;
        if (zexits[i]) {
            zstub = e->p;
            e8(e, 0x31); e8(e, 0xC0);          /* xor eax, eax, then fall into the stub */
        }
        stub_at[i] = e->p;
        st_reg(e, 0, OFF_JREASON);
        e8(e, 0x41); e8(e, 0x8D); e8(e, 0x85); e32(e, i + 1u); /* lea eax, [r13 + retired] */
        epilogue_jumps[nepi++] = jmp32(e);
        for (unsigned k = 0; k < nexit[i]; k++)
            patch(exits[i][k], stub_at[i]);
        if (zexits[i])
            patch(zexits[i], zstub);
    }
    if (to_last)
        patch(to_last, stub_at[n - 1]);
    uint8_t *epi = e->p;
    ld_reg(e, 1, OFF_NEXT);                    /* R15 = next_pc */
    st_reg(e, 1, OFF_R(15));
    e8(e, 0x41); e8(e, 0x5D);                  /* pop r13 */
    e8(e, 0x41); e8(e, 0x5C);                  /* pop r12 */
    e8(e, 0x5B);                               /* pop rbx */
    e8(e, 0xC3);                               /* ret */
    for (unsigned i = 0; i < nepi; i++)
        patch(epilogue_jumps[i], epi);
    memcpy(n_at, &n, 4);
    /* record the translated words; the page may sit in our own write cache */
    if (gmem_mark_code(c->m, pc, 4u * n) == 2 || c->tlb_code_epoch != gmem_code_epoch()) {
        for (unsigned i = 0; i < TLB_SIZE; i++)
            c->tlb_w[i].page = TLB_EMPTY;
        c->tlb_code_epoch = gmem_code_epoch();
    }
    jit_used += (size_t)(e->p - start);
    jit_used = (jit_used + 15u) & ~(size_t)15u;
    b->n = n;
    b->code = (uint32_t (*)(cpu_t *, uint32_t))(void *)start;
    b->entry2 = entry2;
    uint32_t h = jit_bucket(pc, b->host);
    b->next = jit_hash[h];
    jit_hash[h] = b;
    return b;
}

static void jit_link(uint8_t *at, const uint8_t *target) { patch(at, target); }
