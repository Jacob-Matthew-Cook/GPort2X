/* GPort2X ARMv4T interpreter: see include/gport2x/cpu.h.
 *
 * Encodings and semantics follow the ARM Architecture Reference Manual
 * (ARMv4T edition, chapters A3-A5). The file is organised by instruction
 * class: shifter operands, data processing, multiply, loads and stores,
 * load/store multiple, branches, status registers, SWP, SWI. */
#include "gport2x/cpu.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fenv.h>
#include <stddef.h>
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
#include <sys/mman.h>
#define GPORT2X_JIT 1
#endif

#include "gport2x/log.h"

#define TLB_SIZE 1024u   /* data pages, direct-mapped by page number */
#define ICACHE_SIZE 64u  /* decoded instruction pages */
#define JCACHE_SIZE 4096u /* per-cpu direct-mapped cache of JIT blocks */

/* Decoded-instruction cache. An entry holds an instruction word, the handler
 * chosen for it and a precomputed operand; the handler depends on the word
 * alone (never on its address), so an entry is correct wherever that word is
 * met. cpu_run compares the word in memory with the entry's on every
 * execution and redecodes on a mismatch, so code that is rewritten, loaded or
 * remapped can never run stale. fn == NULL marks a never-used entry. */
struct cpu;
struct dentry;
typedef enum cpu_stop (*dhandler_t)(struct cpu *c, const struct dentry *e);
struct dentry {
    dhandler_t fn;
    uint32_t insn;
    uint32_t aux;
};
struct dpage {
    gaddr_t page;          /* guest page base, or 1 when empty */
    const uint8_t *host;   /* its host memory */
    uint8_t nojit;         /* the JIT declined this page: interpret it */
    struct dentry e[GP2X_PAGE_SIZE / 4];
};

struct cpu {
    gmem_t *m;
    cpu_regs_t regs;
    uint64_t insns;
    uint64_t unaligned;
    volatile bool stop_requested;
    gaddr_t *bps;
    size_t nbps;
    gaddr_t resume_pc; /* a breakpoint at this pc is skipped once (the run that stopped there resumes) */
    bool have_resume_pc;
    /* FPA (coprocessors 1 and 2) register images: the 12-byte extended
     * format the FPA stores in memory (word 0: sign bit 31, exponent bits
     * 14..0; word 1: mantissa high with the integer bit at 31; word 2:
     * mantissa low). LFM/SFM move these verbatim; LDF/STF convert. */
    uint32_t fpa[8][3];
    uint32_t fpsr, fpcr;
    /* Per-instruction state. */
    gaddr_t pc;      /* address of the instruction being executed */
    gaddr_t next_pc; /* where execution continues; branches change it */
    cpu_stop_info_t stop;
    uint32_t jreason;            /* the JIT's stop reason */
    uint8_t *jlink_at;           /* set by an unlinked link stub: the branch to patch */
    const uint8_t *jlink_page;   /* and the host page of the block it belongs to */
    /* Page cache (a software TLB): guest page -> host memory, for RAM pages
     * only, so MMIO keeps its side effects on the slow path. Entries are
     * valid while the space's generation stamp is unchanged; cpu_run
     * revalidates on entry, and mappings only change between runs (in the
     * syscall layer). page = TLB_EMPTY marks an empty entry. */
    struct tlb_entry { gaddr_t page; uint8_t *host; } tlb_r[TLB_SIZE], tlb_w[TLB_SIZE];
    struct dpage *dcache; /* ICACHE_SIZE decoded code pages, allocated lazily by the OS (calloc) */
    gmem_t *tlb_m;
    uint64_t tlb_gen;
    uint64_t tlb_code_epoch;
    /* JIT state (jit_*.inc); jreason and jlink_* are near the start of the
     * struct, where AArch64 load/store offsets reach them */
    uint64_t jepoch;
    struct jblock *jcache[JCACHE_SIZE];
    uint8_t jhot[JCACHE_SIZE]; /* executions seen at a block start before it is translated (hashed by pc) */
    cpu_watch_fn watch;        /* cpu_set_watch: NULL on the fast path */
    void *watch_ctx;
};

_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "the page cache reads guest words with host loads");

/* An empty cache entry. No access address masked for comparison (page bits
 * plus the low alignment bits, see the JIT's inline probe) can equal it. */
#define TLB_EMPTY 0xFFFFFFFFu

static void tlb_flush(cpu_t *c)
{
    for (unsigned i = 0; i < TLB_SIZE; i++)
        c->tlb_r[i].page = c->tlb_w[i].page = TLB_EMPTY;
    if (c->dcache)
        for (unsigned i = 0; i < ICACHE_SIZE; i++)
            c->dcache[i].page = 1; /* entries stay: each is valid for its instruction word anywhere */
}

static inline void tlb_sync(cpu_t *c)
{
    uint64_t g = gmem_generation(c->m);
    if (c->tlb_m != c->m || c->tlb_gen != g) {
        tlb_flush(c);
        c->tlb_m = c->m;
        c->tlb_gen = g;
    }
    if (c->tlb_code_epoch != gmem_code_epoch()) { /* a page became translated code: no cached write pointers */
        for (unsigned i = 0; i < TLB_SIZE; i++)
            c->tlb_w[i].page = TLB_EMPTY;
        c->tlb_code_epoch = gmem_code_epoch();
    }
}

/* Host pointer for addr if its page is cached or cacheable RAM with prot;
 * NULL sends the access down the checked slow path (MMIO, faults). The
 * callers never cross a page: the ARMv4 alignment rules have been applied. */
static inline uint8_t *tlb_lookup(cpu_t *c, struct tlb_entry *t, gaddr_t addr, int prot)
{
    gaddr_t pg = addr & ~(gaddr_t)(GP2X_PAGE_SIZE - 1u);
    struct tlb_entry *e = &t[(addr >> GP2X_PAGE_SHIFT) & (TLB_SIZE - 1u)];
    if (e->page != pg) {
        uint8_t *h = gmem_page_host(c->m, addr, prot);
        if (!h)
            return NULL;
        e->page = pg;
        e->host = h;
    }
    return e->host + (addr & (GP2X_PAGE_SIZE - 1u));
}

static inline int watched(cpu_t *c, int rc, gaddr_t addr, unsigned size, uint32_t v, bool write)
{
    if (__builtin_expect(c->watch != NULL, 0) && rc == GP_OK)
        c->watch(c->watch_ctx, addr, size, v, write);
    return rc;
}

static inline int cpu_ld32(cpu_t *c, gaddr_t addr, uint32_t *out)
{
    const uint8_t *h = tlb_lookup(c, c->tlb_r, addr, GMEM_PROT_R);
    int rc = GP_OK;
    if (h) memcpy(out, h, 4); else rc = gmem_ld32(c->m, addr, out);
    return watched(c, rc, addr, 4, *out, false);
}
static inline int cpu_ld16(cpu_t *c, gaddr_t addr, uint32_t *out)
{
    const uint8_t *h = tlb_lookup(c, c->tlb_r, addr, GMEM_PROT_R);
    int rc = GP_OK;
    if (h) { uint16_t v; memcpy(&v, h, 2); *out = v; } else rc = gmem_ld16(c->m, addr, out);
    return watched(c, rc, addr, 2, *out, false);
}
static inline int cpu_ld8(cpu_t *c, gaddr_t addr, uint32_t *out)
{
    const uint8_t *h = tlb_lookup(c, c->tlb_r, addr, GMEM_PROT_R);
    int rc = GP_OK;
    if (h) *out = *h; else rc = gmem_ld8(c->m, addr, out);
    return watched(c, rc, addr, 1, *out, false);
}
static inline int cpu_st32(cpu_t *c, gaddr_t addr, uint32_t v)
{
    uint8_t *h = tlb_lookup(c, c->tlb_w, addr, GMEM_PROT_W);
    int rc = GP_OK;
    if (h) memcpy(h, &v, 4); else rc = gmem_st32(c->m, addr, v);
    return watched(c, rc, addr, 4, v, true);
}
static inline int cpu_st16(cpu_t *c, gaddr_t addr, uint32_t v)
{
    uint8_t *h = tlb_lookup(c, c->tlb_w, addr, GMEM_PROT_W);
    int rc = GP_OK;
    if (h) { uint16_t x = (uint16_t)v; memcpy(h, &x, 2); } else rc = gmem_st16(c->m, addr, v);
    return watched(c, rc, addr, 2, v & 0xFFFFu, true);
}
static inline int cpu_st8(cpu_t *c, gaddr_t addr, uint32_t v)
{
    uint8_t *h = tlb_lookup(c, c->tlb_w, addr, GMEM_PROT_W);
    int rc = GP_OK;
    if (h) *h = (uint8_t)v; else rc = gmem_st8(c->m, addr, v);
    return watched(c, rc, addr, 1, v & 0xFFu, true);
}

void cpu_set_watch(cpu_t *c, cpu_watch_fn fn, void *ctx)
{
    c->watch = fn;
    c->watch_ctx = ctx;
}

/* cond_lut[cond] bit f is set when condition cond passes with flags f =
 * NZCV (bits 3..0). Built once by cpu_create. */
static uint16_t cond_lut[16];
static void build_cond_lut(void)
{
    for (unsigned cond = 0; cond < 16; cond++) {
        uint16_t bits = 0;
        for (unsigned f = 0; f < 16; f++) {
            bool n = f & 8, z = f & 4, cf = f & 2, v = f & 1, pass;
            switch (cond) {
            case 0x0: pass = z; break;
            case 0x1: pass = !z; break;
            case 0x2: pass = cf; break;
            case 0x3: pass = !cf; break;
            case 0x4: pass = n; break;
            case 0x5: pass = !n; break;
            case 0x6: pass = v; break;
            case 0x7: pass = !v; break;
            case 0x8: pass = cf && !z; break;
            case 0x9: pass = !cf || z; break;
            case 0xA: pass = n == v; break;
            case 0xB: pass = n != v; break;
            case 0xC: pass = !z && n == v; break;
            case 0xD: pass = z || n != v; break;
            case 0xE: pass = true; break;
            default: pass = false; break; /* 0xF: unconditional space, undefined on ARMv4 */
            }
            if (pass)
                bits |= (uint16_t)(1u << f);
        }
        cond_lut[cond] = bits;
    }
}

#define R(c, n) ((c)->regs.r[(n)])
#define CPSR(c) ((c)->regs.cpsr)
#define BIT(x, n) (((x) >> (n)) & 1u)
#define BITS(x, hi, lo) (((x) >> (lo)) & ((1u << ((hi) - (lo) + 1)) - 1u))

/* ---------------------------------------------------------------- lifecycle */

cpu_t *cpu_create(gmem_t *m)
{
    if (!m)
        return NULL;
    cpu_t *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->m = m;
    c->regs.cpsr = CPSR_MODE_USR;
    c->dcache = calloc(ICACHE_SIZE, sizeof *c->dcache);
    if (!c->dcache) {
        free(c);
        return NULL;
    }
    if (!cond_lut[0xE]) {
        build_cond_lut();
        const char *j = getenv("GPORT2X_JIT"); /* GPORT2X_JIT=0: interpreter only */
        if (j && !strcmp(j, "0"))
            cpu_set_jit(false);
    }
    tlb_flush(c);
    c->tlb_m = m;
    c->tlb_gen = gmem_generation(m);
    return c;
}

void cpu_destroy(cpu_t *c)
{
    if (!c)
        return;
    free(c->bps);
    free(c->dcache);
    free(c);
}

gmem_t *cpu_gmem(cpu_t *c) { return c ? c->m : NULL; }
cpu_regs_t *cpu_regs(cpu_t *c) { return c ? &c->regs : NULL; }
uint64_t cpu_insn_count(const cpu_t *c) { return c ? c->insns : 0; }
uint64_t cpu_unaligned_count(const cpu_t *c) { return c ? c->unaligned : 0; }
void cpu_request_stop(cpu_t *c)
{
    if (c)
        c->stop_requested = true;
}

int cpu_add_breakpoint(cpu_t *c, gaddr_t pc)
{
    if (!c)
        return GP_ERR_INVAL;
    for (size_t i = 0; i < c->nbps; i++)
        if (c->bps[i] == pc)
            return GP_ERR_EXIST;
    gaddr_t *n = realloc(c->bps, (c->nbps + 1) * sizeof *n);
    if (!n)
        return GP_ERR_NOMEM;
    c->bps = n;
    c->bps[c->nbps++] = pc;
    return GP_OK;
}

int cpu_remove_breakpoint(cpu_t *c, gaddr_t pc)
{
    if (!c)
        return GP_ERR_INVAL;
    for (size_t i = 0; i < c->nbps; i++)
        if (c->bps[i] == pc) {
            c->bps[i] = c->bps[--c->nbps];
            return GP_OK;
        }
    return GP_ERR_INVAL;
}

static bool at_breakpoint(const cpu_t *c, gaddr_t pc)
{
    for (size_t i = 0; i < c->nbps; i++)
        if (c->bps[i] == pc)
            return true;
    return false;
}

/* ---------------------------------------------------------------- flags */

static inline void set_nz(cpu_t *c, uint32_t res)
{
    CPSR(c) = (CPSR(c) & ~(CPSR_N | CPSR_Z)) | (res & CPSR_N) | (res == 0 ? CPSR_Z : 0);
}

static inline void set_c(cpu_t *c, bool carry)
{
    CPSR(c) = (CPSR(c) & ~CPSR_C) | (carry ? CPSR_C : 0);
}

/* a + b + cin with full flags (ADD, ADC, and the subtractions as a + ~b + cin). */
static inline uint32_t add_flags(cpu_t *c, uint32_t a, uint32_t b, uint32_t cin, bool set)
{
    uint64_t sum = (uint64_t)a + b + cin;
    uint32_t res = (uint32_t)sum;
    if (set) {
        set_nz(c, res);
        bool carry = (sum >> 32) != 0;
        bool overflow = (((a ^ res) & (b ^ res)) >> 31) != 0;
        CPSR(c) = (CPSR(c) & ~(CPSR_C | CPSR_V)) | (carry ? CPSR_C : 0) | (overflow ? CPSR_V : 0);
    }
    return res;
}

/* ---------------------------------------------------------------- operands */

/* Register read with the PC offset rule: PC reads as the instruction address
 * + 8, or + 12 for a register-specified shift and for stores of PC (spec 5.4). */
static inline uint32_t reg(const cpu_t *c, unsigned n, uint32_t pc_offset)
{
    return n == 15 ? c->pc + pc_offset : R(c, n);
}

/* Shifter operand for data processing (ARM ARM A5.1). Returns the value and
 * the shifter carry-out. */
static inline uint32_t shifter_operand(cpu_t *c, uint32_t insn, bool *carry_out)
{
    uint32_t cflag = (CPSR(c) & CPSR_C) ? 1u : 0u;
    if (BIT(insn, 25)) { /* 32-bit immediate: imm8 rotated right by 2 x rot */
        uint32_t imm = insn & 0xFFu, rot = BITS(insn, 11, 8) * 2u;
        uint32_t v = rot ? (imm >> rot) | (imm << (32u - rot)) : imm;
        *carry_out = rot ? (v >> 31) != 0 : cflag != 0;
        return v;
    }
    unsigned rm = insn & 0xFu, type = BITS(insn, 6, 5);
    if ((insn & 0xFF0u) == 0) { /* Rm with no shift (LSL #0): the common register form */
        *carry_out = cflag;
        return reg(c, rm, 8);
    }
    uint32_t amount;
    uint32_t v;
    bool by_register = BIT(insn, 4);
    if (by_register) {
        amount = R(c, BITS(insn, 11, 8)) & 0xFFu; /* Rs = PC is unpredictable; read r15 as is */
        v = reg(c, rm, 12);
    } else {
        amount = BITS(insn, 11, 7);
        v = reg(c, rm, 8);
    }
    switch (type) {
    case 0: /* LSL */
        if (amount == 0) { *carry_out = cflag; return v; }
        if (amount < 32) { *carry_out = BIT(v, 32 - amount); return v << amount; }
        *carry_out = amount == 32 ? BIT(v, 0) : 0;
        return 0;
    case 1: /* LSR */
        if (!by_register && amount == 0) amount = 32; /* LSR #0 encodes LSR #32 */
        if (amount == 0) { *carry_out = cflag; return v; }
        if (amount < 32) { *carry_out = BIT(v, amount - 1); return v >> amount; }
        *carry_out = amount == 32 ? BIT(v, 31) : 0;
        return 0;
    case 2: /* ASR */
        if (!by_register && amount == 0) amount = 32;
        if (amount == 0) { *carry_out = cflag; return v; }
        if (amount < 32) { *carry_out = BIT(v, amount - 1); return (uint32_t)((int32_t)v >> amount); }
        *carry_out = BIT(v, 31);
        return BIT(v, 31) ? 0xFFFFFFFFu : 0u;
    default: /* ROR, and RRX for an immediate amount of 0 */
        if (!by_register && amount == 0) { /* RRX */
            *carry_out = BIT(v, 0);
            return (cflag << 31) | (v >> 1);
        }
        if (amount == 0) { *carry_out = cflag; return v; }
        amount &= 31u;
        if (amount == 0) { *carry_out = BIT(v, 31); return v; }
        *carry_out = BIT(v, amount - 1);
        return (v >> amount) | (v << (32u - amount));
    }
}

/* ---------------------------------------------------------------- memory with ARMv4 alignment rules */

static inline void note_unaligned(cpu_t *c, gaddr_t addr, const char *what)
{
    c->unaligned++;
    gp_trace(GP_TRACE_UNALIGNED, "pc=%08x %s addr=%08x", c->pc, what, addr);
}

static int ld_word(cpu_t *c, gaddr_t addr, uint32_t *out)
{
    uint32_t v;
    int rc = cpu_ld32(c, addr & ~3u, &v);
    if (rc != GP_OK)
        return rc;
    unsigned rot = (addr & 3u) * 8u;
    if (rot) { /* ARMv4 rotated load */
        note_unaligned(c, addr, "ldr");
        v = (v >> rot) | (v << (32u - rot));
    }
    *out = v;
    return GP_OK;
}

static int st_word(cpu_t *c, gaddr_t addr, uint32_t v)
{
    if (addr & 3u)
        note_unaligned(c, addr, "str");
    return cpu_st32(c, addr & ~3u, v);
}

static int ld_half(cpu_t *c, gaddr_t addr, uint32_t *out)
{
    if (addr & 1u)
        note_unaligned(c, addr, "ldrh");
    return cpu_ld16(c, addr & ~1u, out);
}

static int st_half(cpu_t *c, gaddr_t addr, uint32_t v)
{
    if (addr & 1u)
        note_unaligned(c, addr, "strh");
    return cpu_st16(c, addr & ~1u, v & 0xFFFFu);
}

/* ---------------------------------------------------------------- stop helpers */

static enum cpu_stop stop_with(cpu_t *c, enum cpu_stop reason, uint32_t insn)
{
    c->stop.reason = reason;
    c->stop.pc = c->pc;
    c->stop.insn = insn;
    c->next_pc = c->pc; /* the instruction did not complete */
    return reason;
}

static enum cpu_stop data_abort(cpu_t *c, uint32_t insn)
{
    const gmem_fault_t *f = gmem_last_fault(c->m);
    c->stop.fault_addr = f->addr;
    c->stop.fault_prot = f->prot;
    gp_trace(GP_TRACE_CPU, "data abort pc=%08x insn=%08x addr=%08x", c->pc, insn, f->addr);
    return stop_with(c, CPU_STOP_DATA_ABORT, insn);
}

static enum cpu_stop undefined(cpu_t *c, uint32_t insn)
{
    gp_trace(GP_TRACE_CPU, "undefined instruction pc=%08x insn=%08x", c->pc, insn);
    return stop_with(c, CPU_STOP_UNDEF, insn);
}

static enum cpu_stop exec_fpa_transfer(cpu_t *c, uint32_t insn);
static enum cpu_stop exec_fpa_cp1(cpu_t *c, uint32_t insn);

/* Writes a branch target reached through a register (BX, LDR pc, LDM pc,
 * data processing with Rd = pc). ARMv4 ARM state ignores bits [1:0]; only BX
 * can select Thumb, which the spec rules out (1.2). */
static enum cpu_stop branch_to(cpu_t *c, uint32_t target, bool interwork, uint32_t insn)
{
    if (interwork && (target & 1u)) {
        c->stop.reason = CPU_STOP_THUMB;
        c->stop.pc = target;
        c->stop.insn = insn;
        c->next_pc = c->pc;
        return CPU_STOP_THUMB;
    }
    c->next_pc = target & ~3u;
    return CPU_STOP_NONE;
}

/* ---------------------------------------------------------------- data processing */

static enum cpu_stop exec_data_processing(cpu_t *c, uint32_t insn)
{
    unsigned opcode = BITS(insn, 24, 21), rn = BITS(insn, 19, 16), rd = BITS(insn, 15, 12);
    bool s = BIT(insn, 20);
    bool carry;
    uint32_t op2 = shifter_operand(c, insn, &carry);
    bool shift_by_reg = !BIT(insn, 25) && BIT(insn, 4);
    uint32_t a = reg(c, rn, shift_by_reg ? 12 : 8);
    uint32_t cin = (CPSR(c) & CPSR_C) ? 1u : 0u;
    uint32_t res;
    bool write = true, logical = false;
    /* S with Rd = pc restores CPSR from SPSR on a privileged core; in user mode
     * there is no SPSR, so the flags are left alone and it acts as a branch. */
    bool setflags = s && rd != 15;
    switch (opcode) {
    case 0x0: res = a & op2; logical = true; break;                       /* AND */
    case 0x1: res = a ^ op2; logical = true; break;                       /* EOR */
    case 0x2: res = add_flags(c, a, ~op2, 1, setflags); break;            /* SUB */
    case 0x3: res = add_flags(c, op2, ~a, 1, setflags); break;            /* RSB */
    case 0x4: res = add_flags(c, a, op2, 0, setflags); break;             /* ADD */
    case 0x5: res = add_flags(c, a, op2, cin, setflags); break;           /* ADC */
    case 0x6: res = add_flags(c, a, ~op2, cin, setflags); break;          /* SBC */
    case 0x7: res = add_flags(c, op2, ~a, cin, setflags); break;          /* RSC */
    case 0x8: res = a & op2; logical = true; write = false; break;        /* TST */
    case 0x9: res = a ^ op2; logical = true; write = false; break;        /* TEQ */
    case 0xA: res = add_flags(c, a, ~op2, 1, true); write = false; break; /* CMP */
    case 0xB: res = add_flags(c, a, op2, 0, true); write = false; break;  /* CMN */
    case 0xC: res = a | op2; logical = true; break;                       /* ORR */
    case 0xD: res = op2; logical = true; break;                           /* MOV */
    case 0xE: res = a & ~op2; logical = true; break;                      /* BIC */
    default:  res = ~op2; logical = true; break;                          /* MVN */
    }
    if (logical && (setflags || !write)) {
        set_nz(c, res);
        set_c(c, carry);
    }
    if (!write)
        return CPU_STOP_NONE;
    if (rd == 15)
        return branch_to(c, res, false, insn);
    R(c, rd) = res;
    return CPU_STOP_NONE;
}

/* ---------------------------------------------------------------- multiply */

static enum cpu_stop exec_multiply(cpu_t *c, uint32_t insn)
{
    unsigned rd = BITS(insn, 19, 16), rn = BITS(insn, 15, 12), rs = BITS(insn, 11, 8), rm = insn & 0xFu;
    bool s = BIT(insn, 20);
    if (BIT(insn, 23)) { /* UMULL/UMLAL/SMULL/SMLAL: RdHi = rd, RdLo = rn */
        bool is_signed = BIT(insn, 22), accumulate = BIT(insn, 21);
        uint64_t prod = is_signed ? (uint64_t)((int64_t)(int32_t)R(c, rm) * (int64_t)(int32_t)R(c, rs))
                                  : (uint64_t)R(c, rm) * (uint64_t)R(c, rs);
        if (accumulate)
            prod += ((uint64_t)R(c, rd) << 32) | R(c, rn);
        R(c, rn) = (uint32_t)prod;
        R(c, rd) = (uint32_t)(prod >> 32);
        if (s) { /* N and Z from the 64-bit result; C and V are unpredictable and left alone */
            CPSR(c) = (CPSR(c) & ~(CPSR_N | CPSR_Z)) | ((prod >> 63) ? CPSR_N : 0) | (prod == 0 ? CPSR_Z : 0);
        }
        return CPU_STOP_NONE;
    }
    if (BITS(insn, 24, 22) != 0)
        return undefined(c, insn);
    uint32_t res = R(c, rm) * R(c, rs);
    if (BIT(insn, 21)) /* MLA */
        res += R(c, rn);
    R(c, rd) = res;
    if (s)
        set_nz(c, res); /* C is unpredictable on ARMv4 and left unchanged */
    return CPU_STOP_NONE;
}

/* ---------------------------------------------------------------- loads and stores */

/* Computes the address of a single load or store and performs the base
 * writeback (after a successful access, so an abort restores the base). */
typedef struct addr_mode {
    gaddr_t address;   /* the address accessed */
    bool writeback;    /* Rn := wb_value after the access */
    uint32_t wb_value;
    unsigned rn;
} addr_mode_t;

static inline void compute_address(cpu_t *c, uint32_t insn, uint32_t offset, addr_mode_t *am)
{
    bool p = BIT(insn, 24), u = BIT(insn, 23), w = BIT(insn, 21);
    am->rn = BITS(insn, 19, 16);
    uint32_t base = reg(c, am->rn, 8);
    uint32_t offset_addr = u ? base + offset : base - offset;
    if (p) {
        am->address = offset_addr;
        am->writeback = w;
    } else { /* post-indexed: always writes back */
        am->address = base;
        am->writeback = true;
    }
    am->wb_value = offset_addr;
    if (am->rn == 15)
        am->writeback = false; /* writeback to pc is unpredictable; ignore it */
}

static enum cpu_stop exec_load_store(cpu_t *c, uint32_t insn)
{
    uint32_t offset;
    if (BIT(insn, 25)) { /* register offset, shifted by an immediate */
        if (BIT(insn, 4))
            return undefined(c, insn);
        bool unused;
        uint32_t shift_insn = insn & ~(1u << 25); /* reuse the shifter: immediate shift form */
        offset = shifter_operand(c, shift_insn, &unused);
    } else {
        offset = insn & 0xFFFu;
    }
    addr_mode_t am;
    compute_address(c, insn, offset, &am);
    unsigned rd = BITS(insn, 15, 12);
    bool byte = BIT(insn, 22), load = BIT(insn, 20);
    int rc;
    if (load) {
        uint32_t v;
        rc = byte ? cpu_ld8(c, am.address, &v) : ld_word(c, am.address, &v);
        if (rc != GP_OK)
            return data_abort(c, insn);
        if (am.writeback)
            R(c, am.rn) = am.wb_value;
        if (rd == 15)
            return branch_to(c, v, false, insn);
        R(c, rd) = v;
    } else {
        uint32_t v = reg(c, rd, 12); /* a store of pc stores pc + 12 (spec 5.4) */
        rc = byte ? cpu_st8(c, am.address, v & 0xFFu) : st_word(c, am.address, v);
        if (rc != GP_OK)
            return data_abort(c, insn);
        if (am.writeback)
            R(c, am.rn) = am.wb_value;
    }
    return CPU_STOP_NONE;
}

/* LDRH/STRH/LDRSB/LDRSH (bits 7 and 4 set, bits 6:5 not 00). */
static enum cpu_stop exec_load_store_half(cpu_t *c, uint32_t insn)
{
    uint32_t offset = BIT(insn, 22) ? (BITS(insn, 11, 8) << 4) | (insn & 0xFu) : reg(c, insn & 0xFu, 8);
    addr_mode_t am;
    compute_address(c, insn, offset, &am);
    unsigned rd = BITS(insn, 15, 12), kind = BITS(insn, 6, 5);
    bool load = BIT(insn, 20);
    uint32_t v;
    int rc;
    if (!load) {
        if (kind != 1) /* STRD / LDRD are ARMv5 */
            return undefined(c, insn);
        rc = st_half(c, am.address, reg(c, rd, 12));
        if (rc != GP_OK)
            return data_abort(c, insn);
        if (am.writeback)
            R(c, am.rn) = am.wb_value;
        return CPU_STOP_NONE;
    }
    switch (kind) {
    case 1: rc = ld_half(c, am.address, &v); break;                                    /* LDRH */
    case 2: rc = cpu_ld8(c, am.address, &v); v = (uint32_t)(int32_t)(int8_t)v; break; /* LDRSB */
    default: rc = ld_half(c, am.address, &v); v = (uint32_t)(int32_t)(int16_t)v; break;  /* LDRSH */
    }
    if (rc != GP_OK)
        return data_abort(c, insn);
    if (am.writeback)
        R(c, am.rn) = am.wb_value;
    R(c, rd) = v;
    return CPU_STOP_NONE;
}

/* ---------------------------------------------------------------- load/store multiple */

static enum cpu_stop exec_load_store_multiple(cpu_t *c, uint32_t insn)
{
    bool p = BIT(insn, 24), u = BIT(insn, 23), w = BIT(insn, 21), load = BIT(insn, 20);
    unsigned rn = BITS(insn, 19, 16);
    uint32_t list = insn & 0xFFFFu;
    unsigned count = 0;
    for (uint32_t l = list; l; l >>= 1)
        count += l & 1u;
    if (count == 0)
        return undefined(c, insn); /* an empty list is unpredictable; refuse it loudly */
    if (R(c, rn) & 3u) /* ignored on ARMv4; a SIGBUS on ARMv8 (spec 5.3) */
        note_unaligned(c, R(c, rn), load ? "ldm" : "stm");
    uint32_t base = R(c, rn) & ~3u; /* bits [1:0] are ignored */
    uint32_t start = u ? (p ? base + 4 : base) : (p ? base - 4 * count : base - 4 * count + 4);
    uint32_t wb = u ? base + 4 * count : base - 4 * count;
    int rc;
    if (load) {
        uint32_t values[16];
        uint32_t a = start;
        for (unsigned i = 0, k = 0; i < 16; i++) {
            if (!BIT(list, i))
                continue;
            rc = cpu_ld32(c, a, &values[k++]); /* all loads first: an abort leaves the registers alone */
            if (rc != GP_OK)
                return data_abort(c, insn);
            a += 4;
        }
        if (w && !BIT(list, rn))
            R(c, rn) = wb;
        enum cpu_stop st = CPU_STOP_NONE;
        for (unsigned i = 0, k = 0; i < 16; i++) {
            if (!BIT(list, i))
                continue;
            if (i == 15)
                st = branch_to(c, values[k++], false, insn);
            else
                R(c, i) = values[k++];
        }
        return st;
    }
    if (!gmem_is_mapped(c->m, start, 4 * count, GMEM_PROT_W)) {
        /* Find the exact faulting word for the report. */
        for (uint32_t a = start; a < start + 4 * count; a += 4)
            if (!gmem_is_mapped(c->m, a, 4, GMEM_PROT_W)) {
                (void)cpu_st32(c, a, 0); /* records the fault */
                break;
            }
        return data_abort(c, insn);
    }
    uint32_t a = start;
    bool first = true;
    for (unsigned i = 0; i < 16; i++) {
        if (!BIT(list, i))
            continue;
        uint32_t v;
        if (i == 15)
            v = c->pc + 12; /* STM of pc stores pc + 12 (spec 5.4) */
        else if (i == rn && w && !first)
            v = wb; /* the base after the first register sees its written-back value */
        else
            v = R(c, i);
        cpu_st32(c, a, v);
        a += 4;
        first = false;
    }
    if (w)
        R(c, rn) = wb;
    return CPU_STOP_NONE;
}

/* ---------------------------------------------------------------- status registers, BX, SWP */

static enum cpu_stop exec_msr(cpu_t *c, uint32_t insn)
{
    uint32_t value;
    if (BIT(insn, 25)) {
        bool unused;
        value = shifter_operand(c, insn, &unused);
    } else {
        value = R(c, insn & 0xFu);
    }
    /* Only the flags field is writable in user mode; the control, extension
     * and status fields (and the SPSR, R = 1) are ignored there. */
    if (!BIT(insn, 22) && BIT(insn, 19))
        CPSR(c) = (CPSR(c) & 0x0FFFFFFFu) | (value & 0xF0000000u);
    return CPU_STOP_NONE;
}

static enum cpu_stop exec_swap(cpu_t *c, uint32_t insn)
{
    unsigned rn = BITS(insn, 19, 16), rd = BITS(insn, 15, 12), rm = insn & 0xFu;
    bool byte = BIT(insn, 22);
    gaddr_t addr = R(c, rn);
    uint32_t old;
    int rc = byte ? cpu_ld8(c, addr, &old) : ld_word(c, addr, &old);
    if (rc != GP_OK)
        return data_abort(c, insn);
    rc = byte ? cpu_st8(c, addr, R(c, rm) & 0xFFu) : st_word(c, addr, R(c, rm));
    if (rc != GP_OK)
        return data_abort(c, insn);
    R(c, rd) = old;
    return CPU_STOP_NONE;
}

/* ---------------------------------------------------------------- dispatch */

static inline bool condition_passed(uint32_t cpsr, unsigned cond)
{
    return (cond_lut[cond] >> (cpsr >> 28)) & 1u;
}

static enum cpu_stop execute(cpu_t *c, uint32_t insn)
{
    switch (BITS(insn, 27, 25)) {
    case 0x0:
        if ((insn & 0x90u) == 0x90u) { /* bits 7 and 4 set: multiply, SWP or halfword transfer */
            if (BITS(insn, 6, 5) == 0) {
                if (BITS(insn, 27, 24) == 0x0)
                    return exec_multiply(c, insn);
                if (BITS(insn, 27, 23) == 0x2 && BITS(insn, 21, 20) == 0)
                    return exec_swap(c, insn);
                return undefined(c, insn);
            }
            return exec_load_store_half(c, insn);
        }
        if (BITS(insn, 24, 23) == 0x2 && !BIT(insn, 20)) { /* miscellaneous: MRS, MSR, BX */
            if ((insn & 0x0FBF0FFFu) == 0x010F0000u) { /* MRS Rd, CPSR (or SPSR, which user mode lacks) */
                R(c, BITS(insn, 15, 12)) = CPSR(c);
                return CPU_STOP_NONE;
            }
            if ((insn & 0x0FB0FFF0u) == 0x0120F000u) /* MSR CPSR_f, Rm */
                return exec_msr(c, insn);
            if ((insn & 0x0FFFFFF0u) == 0x012FFF10u) /* BX Rm */
                return branch_to(c, R(c, insn & 0xFu), true, insn);
            return undefined(c, insn); /* CLZ, BKPT, BLX (ARMv5) and the rest */
        }
        return exec_data_processing(c, insn);
    case 0x1:
        if (BITS(insn, 24, 23) == 0x2 && !BIT(insn, 20)) {
            if ((insn & 0x0FB0F000u) == 0x0320F000u) /* MSR CPSR_f, #imm */
                return exec_msr(c, insn);
            return undefined(c, insn);
        }
        return exec_data_processing(c, insn);
    case 0x2:
    case 0x3:
        return exec_load_store(c, insn);
    case 0x4:
        return exec_load_store_multiple(c, insn);
    case 0x5: { /* B / BL */
        uint32_t offset = (uint32_t)((int32_t)(insn << 8) >> 6); /* sign-extended imm24 x 4 */
        if (BIT(insn, 24))
            R(c, 14) = c->pc + 4;
        c->next_pc = c->pc + 8 + offset;
        return CPU_STOP_NONE;
    }
    case 0x6:
        return exec_fpa_transfer(c, insn); /* LDC / STC: FPA LDF/STF (cp1) and LFM/SFM (cp2) */
    default:
        if (BIT(insn, 24)) { /* SWI */
            c->stop.reason = CPU_STOP_SVC;
            c->stop.pc = c->pc + 4;
            c->stop.insn = insn;
            c->stop.svc_imm = insn & 0xFFFFFFu;
            c->next_pc = c->pc + 4;
            return CPU_STOP_SVC;
        }
        return exec_fpa_cp1(c, insn); /* CDP / MCR / MRC: FPA data operations and transfers */
    }
}

/* ---------------------------------------------------------------- FPA loads and stores
 *
 * Extended-format conversions are done on the bit patterns (no host float
 * types), so every host gives the same results. Round to nearest even. */

typedef struct fx80 {
    uint16_t sign_exp; /* bit 15 sign, bits 14..0 biased exponent (bias 16383) */
    uint64_t mant;     /* explicit integer bit at 63 */
} fx80_t;

static void fx80_from_words(const uint32_t w[3], fx80_t *x)
{
    x->sign_exp = (uint16_t)(((w[0] >> 31) << 15) | (w[0] & 0x7FFFu));
    x->mant = ((uint64_t)w[1] << 32) | w[2];
}

static void fx80_to_words(const fx80_t *x, uint32_t w[3])
{
    w[0] = ((uint32_t)(x->sign_exp >> 15) << 31) | (x->sign_exp & 0x7FFFu);
    w[1] = (uint32_t)(x->mant >> 32);
    w[2] = (uint32_t)x->mant;
}

static fx80_t fx80_from_f32(uint32_t f)
{
    fx80_t x;
    uint32_t sign = f >> 31, exp = (f >> 23) & 0xFF, frac = f & 0x7FFFFFu;
    if (exp == 0xFF) { /* inf / nan */
        x.sign_exp = (uint16_t)((sign << 15) | 0x7FFF);
        x.mant = frac ? (0xC000000000000000ull | ((uint64_t)frac << 40)) : 0x8000000000000000ull;
        return x;
    }
    if (exp == 0) {
        if (frac == 0) { x.sign_exp = (uint16_t)(sign << 15); x.mant = 0; return x; }
        int shift = 0;
        while (!(frac & 0x800000u)) { frac <<= 1; shift++; } /* normalise the subnormal */
        exp = (uint32_t)(1 - shift);
        x.sign_exp = (uint16_t)((sign << 15) | ((exp - 127 + 16383) & 0x7FFF));
        x.mant = (uint64_t)frac << 40;
        return x;
    }
    x.sign_exp = (uint16_t)((sign << 15) | (exp - 127 + 16383));
    x.mant = ((uint64_t)(frac | 0x800000u)) << 40;
    return x;
}

static fx80_t fx80_from_f64(uint64_t f)
{
    fx80_t x;
    uint32_t sign = (uint32_t)(f >> 63), exp = (uint32_t)((f >> 52) & 0x7FF);
    uint64_t frac = f & 0xFFFFFFFFFFFFFull;
    if (exp == 0x7FF) {
        x.sign_exp = (uint16_t)((sign << 15) | 0x7FFF);
        x.mant = frac ? (0xC000000000000000ull | (frac << 11)) : 0x8000000000000000ull;
        return x;
    }
    if (exp == 0) {
        if (frac == 0) { x.sign_exp = (uint16_t)(sign << 15); x.mant = 0; return x; }
        int shift = 0;
        while (!(frac & (1ull << 52))) { frac <<= 1; shift++; }
        exp = (uint32_t)(1 - shift);
        x.sign_exp = (uint16_t)((sign << 15) | ((exp - 1023 + 16383) & 0x7FFF));
        x.mant = frac << 11;
        return x;
    }
    x.sign_exp = (uint16_t)((sign << 15) | (exp - 1023 + 16383));
    x.mant = (frac | (1ull << 52)) << 11;
    return x;
}

/* Rounds an extended value to a format with `fbits` fraction bits and
 * exponent bias `bias`, max biased exponent `emax` (all ones = inf/nan). */
static uint64_t fx80_round_to(const fx80_t *x, int fbits, int bias, int emax)
{
    uint64_t sign = x->sign_exp >> 15;
    int exp = x->sign_exp & 0x7FFF;
    uint64_t mant = x->mant;
    if (exp == 0x7FFF) { /* inf / nan */
        uint64_t frac = (mant & 0x7FFFFFFFFFFFFFFFull) ? (mant >> (63 - fbits)) & ((1ull << fbits) - 1) : 0;
        if ((mant & 0x7FFFFFFFFFFFFFFFull) && !frac) frac = 1ull << (fbits - 1); /* keep it a nan */
        return (sign << (fbits + (emax == 0xFF ? 8 : 11))) | ((uint64_t)emax << fbits) | frac;
    }
    if (mant == 0)
        return sign << (fbits + (emax == 0xFF ? 8 : 11));
    while (!(mant & 0x8000000000000000ull)) { mant <<= 1; exp--; } /* unnormal: normalise */
    int e = exp - 16383 + bias; /* target biased exponent */
    int shift = 63 - fbits;      /* bits to drop for a normal number */
    if (e <= 0) {                /* subnormal in the target: drop more */
        shift += 1 - e;
        e = 0;
        if (shift > 64) shift = 64;
    }
    uint64_t kept = shift >= 64 ? 0 : mant >> shift;
    uint64_t rem = shift >= 64 ? mant : (shift == 0 ? 0 : mant & ((1ull << shift) - 1));
    uint64_t half = shift == 0 ? 0 : 1ull << (shift - 1);
    if (shift > 0 && (rem > half || (rem == half && (kept & 1)))) {
        kept++;
        if (e == 0 && (kept >> fbits)) e = 1;             /* rounded up into the normal range */
        else if (e > 0 && (kept >> (fbits + 1))) { kept >>= 1; e++; } /* mantissa overflow */
    } else if (e == 0 && (kept >> fbits)) {
        e = 1;
    }
    if (e >= emax) /* overflow -> infinity */
        return (sign << (fbits + (emax == 0xFF ? 8 : 11))) | ((uint64_t)emax << fbits);
    uint64_t frac = kept & ((1ull << fbits) - 1);
    return (sign << (fbits + (emax == 0xFF ? 8 : 11))) | ((uint64_t)e << fbits) | frac;
}

static enum cpu_stop exec_fpa_transfer(cpu_t *c, uint32_t insn)
{
    uint32_t cp = BITS(insn, 11, 8);
    if (cp != 1 && cp != 2)
        return undefined(c, insn);
    bool P = BIT(insn, 24), U = BIT(insn, 23), N = BIT(insn, 22), W = BIT(insn, 21), L = BIT(insn, 20);
    uint32_t rn = BITS(insn, 19, 16), fd = BITS(insn, 14, 12);
    uint32_t offset = (insn & 0xFFu) * 4;
    uint32_t base = R(c, rn) + (rn == 15 ? 8 : 0);
    uint32_t addr = P ? (U ? base + offset : base - offset) : base;
    uint32_t words[12];
    uint32_t count;
    unsigned prec = (N << 1) | BIT(insn, 15); /* LDF/STF: 0 S, 1 D, 2 E, 3 P;  LFM/SFM: 0 = 4 regs, else count */
    if (cp == 2) {
        count = prec == 0 ? 4 : prec;
        count *= 3;
    } else {
        count = prec == 0 ? 1 : prec == 1 ? 2 : 3;
    }
    if (L) {
        for (uint32_t i = 0; i < count; i++)
            if (ld_word(c, addr + i * 4, &words[i]) != GP_OK)
                return data_abort(c, insn);
        if (cp == 2) {
            for (uint32_t i = 0; i < count / 3; i++)
                memcpy(c->fpa[(fd + i) & 7], &words[i * 3], 12);
        } else {
            fx80_t x;
            if (prec == 0) x = fx80_from_f32(words[0]);
            else if (prec == 1) x = fx80_from_f64(((uint64_t)words[0] << 32) | words[1]);
            else { fx80_from_words(words, &x); } /* extended (packed decimal is treated the same) */
            fx80_to_words(&x, c->fpa[fd]);
        }
    } else {
        if (cp == 2) {
            for (uint32_t i = 0; i < count / 3; i++)
                memcpy(&words[i * 3], c->fpa[(fd + i) & 7], 12);
        } else {
            fx80_t x;
            fx80_from_words(c->fpa[fd], &x);
            if (prec == 0) words[0] = (uint32_t)fx80_round_to(&x, 23, 127, 0xFF);
            else if (prec == 1) { uint64_t d = fx80_round_to(&x, 52, 1023, 0x7FF); words[0] = (uint32_t)(d >> 32); words[1] = (uint32_t)d; }
            else fx80_to_words(&x, words);
        }
        for (uint32_t i = 0; i < count; i++)
            if (st_word(c, addr + i * 4, words[i]) != GP_OK)
                return data_abort(c, insn);
    }
    if (W || !P) { /* writeback (post-indexed always writes back) */
        uint32_t nb = U ? base + offset : base - offset;
        if (rn != 15)
            R(c, rn) = nb;
    }
    return CPU_STOP_NONE;
}

/* ---------------------------------------------------------------- FPA arithmetic
 *
 * Coprocessor 1 data operations (CPDO) and register transfers (CPRT), as
 * the kernel's NWFPE emulator provides them to user programs (spec 5.6).
 * Values are computed in the host's long double (exact for the 64-bit
 * mantissa on x86-64; binary128 on AArch64, which rounds once more for
 * extended results) and rounded to the destination precision with the
 * instruction's rounding mode. */

static long double fx80_to_ld(const fx80_t *x)
{
    int sign = x->sign_exp >> 15;
    int exp = x->sign_exp & 0x7FFF;
    if (exp == 0x7FFF) {
        if (x->mant & 0x7FFFFFFFFFFFFFFFull)
            return NAN;
        return sign ? -INFINITY : INFINITY;
    }
    if (x->mant == 0)
        return sign ? -0.0L : 0.0L;
    long double v = ldexpl((long double)x->mant, exp - 16383 - 63);
    return sign ? -v : v;
}

static fx80_t ld_to_fx80(long double v)
{
    fx80_t x;
    int sign = signbit(v) ? 1 : 0;
    if (isnan(v)) {
        x.sign_exp = (uint16_t)((sign << 15) | 0x7FFF);
        x.mant = 0xC000000000000000ull;
        return x;
    }
    if (isinf(v)) {
        x.sign_exp = (uint16_t)((sign << 15) | 0x7FFF);
        x.mant = 0x8000000000000000ull;
        return x;
    }
    if (v == 0) {
        x.sign_exp = (uint16_t)(sign << 15);
        x.mant = 0;
        return x;
    }
    int e;
    long double m = frexpl(fabsl(v), &e); /* [0.5, 1) x 2^e */
    long double scaled = ldexpl(m, 64);
    uint64_t mant = (uint64_t)scaled;
    long double frac = scaled - (long double)mant;
    if (frac > 0.5L || (frac == 0.5L && (mant & 1))) { /* nearest even (only matters beyond 64 bits) */
        if (++mant == 0) {
            mant = 0x8000000000000000ull;
            e++;
        }
    }
    int exp = e - 1 + 16383;
    if (exp >= 0x7FFF) {
        x.sign_exp = (uint16_t)((sign << 15) | 0x7FFF);
        x.mant = 0x8000000000000000ull;
        return x;
    }
    if (exp <= 0) { /* denormal in extended: shift the mantissa down */
        int sh = 1 - exp;
        mant = sh >= 64 ? 0 : mant >> sh;
        exp = 0;
    }
    x.sign_exp = (uint16_t)((sign << 15) | exp);
    x.mant = mant;
    return x;
}

static int fpa_host_round(unsigned mode)
{
    switch (mode) {
    case 1: return FE_UPWARD;
    case 2: return FE_DOWNWARD;
    case 3: return FE_TOWARDZERO;
    default: return FE_TONEAREST;
    }
}

/* Rounds v to the destination precision (0 S, 1 D, 2 E) in the given mode. */
static long double fpa_round_prec(long double v, unsigned prec, unsigned mode)
{
    int old = fegetround();
    fesetround(fpa_host_round(mode));
    long double r;
    if (prec == 0) {
        volatile float f = (float)v;
        r = f;
    } else if (prec == 1) {
        volatile double d = (double)v;
        r = d;
    } else {
        r = v;
    }
    fesetround(old);
    return r;
}

static long double fpa_operand(cpu_t *c, uint32_t insn, unsigned fm_field)
{
    static const long double consts[8] = { 0.0L, 1.0L, 2.0L, 3.0L, 4.0L, 5.0L, 0.5L, 10.0L };
    if (BIT(insn, 3))
        return consts[fm_field & 7];
    fx80_t x;
    fx80_from_words(c->fpa[fm_field & 7], &x);
    return fx80_to_ld(x.mant || !(x.sign_exp & 0x7FFF) ? &x : &x);
}

static void fpa_set(cpu_t *c, unsigned fd, long double v)
{
    fx80_t x = ld_to_fx80(v);
    fx80_to_words(&x, c->fpa[fd & 7]);
}

static enum cpu_stop exec_fpa_cpdo(cpu_t *c, uint32_t insn)
{
    unsigned opcode = BITS(insn, 23, 20) | (BIT(insn, 15) << 4);
    unsigned fn = BITS(insn, 18, 16), fd = BITS(insn, 14, 12), fm = BITS(insn, 2, 0);
    unsigned prec = (BIT(insn, 19) << 1) | BIT(insn, 7);
    unsigned mode = BITS(insn, 6, 5);
    if (prec == 3)
        return undefined(c, insn); /* packed decimal */
    long double a = 0, b = fpa_operand(c, insn, fm), r;
    if (!(opcode & 0x10)) {
        fx80_t x;
        fx80_from_words(c->fpa[fn], &x);
        a = fx80_to_ld(&x);
    }
    int old = fegetround();
    fesetround(fpa_host_round(mode));
    switch (opcode) {
    case 0x00: r = a + b; break;                 /* ADF */
    case 0x01: r = a * b; break;                 /* MUF */
    case 0x02: r = a - b; break;                 /* SUF */
    case 0x03: r = b - a; break;                 /* RSF */
    case 0x04: r = a / b; break;                 /* DVF */
    case 0x05: r = b / a; break;                 /* RDF */
    case 0x06: r = powl(a, b); break;            /* POW */
    case 0x07: r = powl(b, a); break;            /* RPW */
    case 0x08: r = remainderl(a, b); break;      /* RMF */
    case 0x09: r = a * b; break;                 /* FML (fast) */
    case 0x0A: r = a / b; break;                 /* FDV */
    case 0x0B: r = b / a; break;                 /* FRD */
    case 0x0C: r = atan2l(b, a); break;          /* POL */
    case 0x10: r = b; break;                     /* MVF */
    case 0x11: r = -b; break;                    /* MNF */
    case 0x12: r = fabsl(b); break;              /* ABS */
    case 0x13: r = rintl(b); break;              /* RND (to integral, current mode) */
    case 0x14: r = sqrtl(b); break;              /* SQT */
    case 0x15: r = log10l(b); break;             /* LOG */
    case 0x16: r = logl(b); break;               /* LGN */
    case 0x17: r = expl(b); break;               /* EXP */
    case 0x18: r = sinl(b); break;               /* SIN */
    case 0x19: r = cosl(b); break;               /* COS */
    case 0x1A: r = tanl(b); break;               /* TAN */
    case 0x1B: r = asinl(b); break;              /* ASN */
    case 0x1C: r = acosl(b); break;              /* ACS */
    case 0x1D: r = atanl(b); break;              /* ATN */
    case 0x1E: r = rintl(b); break;              /* URD */
    case 0x1F: r = b; break;                     /* NRM */
    default:
        fesetround(old);
        return undefined(c, insn);
    }
    fesetround(old);
    fpa_set(c, fd, fpa_round_prec(r, prec, mode));
    return CPU_STOP_NONE;
}

static enum cpu_stop exec_fpa_cprt(cpu_t *c, uint32_t insn)
{
    unsigned opcode = BITS(insn, 23, 20);
    unsigned fn = BITS(insn, 18, 16), rd = BITS(insn, 15, 12), fm = BITS(insn, 2, 0);
    unsigned prec = (BIT(insn, 19) << 1) | BIT(insn, 7);
    unsigned mode = BITS(insn, 6, 5);
    switch (opcode) {
    case 0x0: { /* FLT Fn, Rd: integer to float */
        long double v = (long double)(int32_t)R(c, rd);
        fpa_set(c, fn, fpa_round_prec(v, prec == 3 ? 2 : prec, mode));
        return CPU_STOP_NONE;
    }
    case 0x1: { /* FIX Rd, Fm: float to integer in the rounding mode */
        long double v = fpa_operand(c, insn, fm);
        int32_t out;
        if (isnan(v))
            out = INT32_MIN;
        else {
            int old = fegetround();
            fesetround(fpa_host_round(mode));
            long double r = rintl(v);
            fesetround(old);
            if (r >= 2147483648.0L) out = INT32_MAX;
            else if (r <= -2147483649.0L) out = INT32_MIN;
            else out = (int32_t)r;
        }
        if (rd == 15)
            return undefined(c, insn);
        R(c, rd) = (uint32_t)out;
        return CPU_STOP_NONE;
    }
    case 0x2: c->fpsr = R(c, rd); return CPU_STOP_NONE;              /* WFS */
    case 0x3: R(c, rd) = (c->fpsr & 0x00FFFFFFu) | 0x81000000u; return CPU_STOP_NONE; /* RFS: system id 0x81 */
    case 0x4: c->fpcr = R(c, rd); return CPU_STOP_NONE;              /* WFC */
    case 0x5: R(c, rd) = c->fpcr; return CPU_STOP_NONE;              /* RFC */
    case 0x9: case 0xB: case 0xD: case 0xF: {                        /* CMF, CNF, CMFE, CNFE */
        fx80_t x;
        fx80_from_words(c->fpa[fn], &x);
        long double a = fx80_to_ld(&x), b = fpa_operand(c, insn, fm);
        if (opcode == 0xB || opcode == 0xF)
            b = -b;
        uint32_t flags;
        if (isnan(a) || isnan(b))
            flags = CPSR_C | CPSR_V;
        else if (a < b)
            flags = CPSR_N;
        else if (a == b)
            flags = CPSR_Z | CPSR_C;
        else
            flags = CPSR_C;
        CPSR(c) = (CPSR(c) & 0x0FFFFFFFu) | flags;
        return CPU_STOP_NONE;
    }
    default:
        return undefined(c, insn);
    }
}

static enum cpu_stop exec_fpa_cp1(cpu_t *c, uint32_t insn)
{
    if (BITS(insn, 11, 8) != 1)
        return undefined(c, insn);
    return BIT(insn, 4) ? exec_fpa_cprt(c, insn) : exec_fpa_cpdo(c, insn);
}


/* ---------------------------------------------------------------- decoded handlers
 *
 * Specialised forms of the commonest instructions. Each is only chosen by
 * decode() when its preconditions hold (no pc operand or destination, no S
 * bit unless named, pre-indexed without writeback), so it needs none of the
 * generic checks; everything else runs through execute(). Conditions are
 * checked by cpu_run before the handler. */
#define E_RD(e) (((e)->insn >> 12) & 15u)
#define E_RN(e) (((e)->insn >> 16) & 15u)
#define E_RM(e) ((e)->insn & 15u)

static enum cpu_stop h_generic(cpu_t *c, const struct dentry *e) { return execute(c, e->insn); }

/* data processing, immediate operand (aux = the operand, pre-negated or
 * pre-inverted where that turns the op into another) */
static enum cpu_stop h_mov_imm(cpu_t *c, const struct dentry *e) { R(c, E_RD(e)) = e->aux; return CPU_STOP_NONE; }
static enum cpu_stop h_add_imm(cpu_t *c, const struct dentry *e) { R(c, E_RD(e)) = R(c, E_RN(e)) + e->aux; return CPU_STOP_NONE; }
static enum cpu_stop h_rsb_imm(cpu_t *c, const struct dentry *e) { R(c, E_RD(e)) = e->aux - R(c, E_RN(e)); return CPU_STOP_NONE; }
static enum cpu_stop h_and_imm(cpu_t *c, const struct dentry *e) { R(c, E_RD(e)) = R(c, E_RN(e)) & e->aux; return CPU_STOP_NONE; }
static enum cpu_stop h_orr_imm(cpu_t *c, const struct dentry *e) { R(c, E_RD(e)) = R(c, E_RN(e)) | e->aux; return CPU_STOP_NONE; }
static enum cpu_stop h_eor_imm(cpu_t *c, const struct dentry *e) { R(c, E_RD(e)) = R(c, E_RN(e)) ^ e->aux; return CPU_STOP_NONE; }
static enum cpu_stop h_cmp_imm(cpu_t *c, const struct dentry *e) { add_flags(c, R(c, E_RN(e)), ~e->aux, 1, true); return CPU_STOP_NONE; }
static enum cpu_stop h_cmn_imm(cpu_t *c, const struct dentry *e) { add_flags(c, R(c, E_RN(e)), e->aux, 0, true); return CPU_STOP_NONE; }
static enum cpu_stop h_tst_imm(cpu_t *c, const struct dentry *e) { set_nz(c, R(c, E_RN(e)) & e->aux); return CPU_STOP_NONE; } /* rot 0: C kept */
static enum cpu_stop h_subs_imm(cpu_t *c, const struct dentry *e) { R(c, E_RD(e)) = add_flags(c, R(c, E_RN(e)), ~e->aux, 1, true); return CPU_STOP_NONE; }
static enum cpu_stop h_adds_imm(cpu_t *c, const struct dentry *e) { R(c, E_RD(e)) = add_flags(c, R(c, E_RN(e)), e->aux, 0, true); return CPU_STOP_NONE; }

/* data processing, register operand without shift */
static enum cpu_stop h_mov_reg(cpu_t *c, const struct dentry *e) { R(c, E_RD(e)) = R(c, E_RM(e)); return CPU_STOP_NONE; }
static enum cpu_stop h_mvn_reg(cpu_t *c, const struct dentry *e) { R(c, E_RD(e)) = ~R(c, E_RM(e)); return CPU_STOP_NONE; }
static enum cpu_stop h_add_reg(cpu_t *c, const struct dentry *e) { R(c, E_RD(e)) = R(c, E_RN(e)) + R(c, E_RM(e)); return CPU_STOP_NONE; }
static enum cpu_stop h_sub_reg(cpu_t *c, const struct dentry *e) { R(c, E_RD(e)) = R(c, E_RN(e)) - R(c, E_RM(e)); return CPU_STOP_NONE; }
static enum cpu_stop h_rsb_reg(cpu_t *c, const struct dentry *e) { R(c, E_RD(e)) = R(c, E_RM(e)) - R(c, E_RN(e)); return CPU_STOP_NONE; }
static enum cpu_stop h_and_reg(cpu_t *c, const struct dentry *e) { R(c, E_RD(e)) = R(c, E_RN(e)) & R(c, E_RM(e)); return CPU_STOP_NONE; }
static enum cpu_stop h_orr_reg(cpu_t *c, const struct dentry *e) { R(c, E_RD(e)) = R(c, E_RN(e)) | R(c, E_RM(e)); return CPU_STOP_NONE; }
static enum cpu_stop h_eor_reg(cpu_t *c, const struct dentry *e) { R(c, E_RD(e)) = R(c, E_RN(e)) ^ R(c, E_RM(e)); return CPU_STOP_NONE; }
static enum cpu_stop h_bic_reg(cpu_t *c, const struct dentry *e) { R(c, E_RD(e)) = R(c, E_RN(e)) & ~R(c, E_RM(e)); return CPU_STOP_NONE; }
static enum cpu_stop h_cmp_reg(cpu_t *c, const struct dentry *e) { add_flags(c, R(c, E_RN(e)), ~R(c, E_RM(e)), 1, true); return CPU_STOP_NONE; }
static enum cpu_stop h_cmn_reg(cpu_t *c, const struct dentry *e) { add_flags(c, R(c, E_RN(e)), R(c, E_RM(e)), 0, true); return CPU_STOP_NONE; }
static enum cpu_stop h_tst_reg(cpu_t *c, const struct dentry *e) { set_nz(c, R(c, E_RN(e)) & R(c, E_RM(e))); return CPU_STOP_NONE; }
static enum cpu_stop h_subs_reg(cpu_t *c, const struct dentry *e) { R(c, E_RD(e)) = add_flags(c, R(c, E_RN(e)), ~R(c, E_RM(e)), 1, true); return CPU_STOP_NONE; }
static enum cpu_stop h_adds_reg(cpu_t *c, const struct dentry *e) { R(c, E_RD(e)) = add_flags(c, R(c, E_RN(e)), R(c, E_RM(e)), 0, true); return CPU_STOP_NONE; }
static enum cpu_stop h_movs_reg(cpu_t *c, const struct dentry *e) { uint32_t v = R(c, E_RM(e)); R(c, E_RD(e)) = v; set_nz(c, v); return CPU_STOP_NONE; }

/* data processing, register shifted by an immediate 1..31 (LSL, LSR, ASR), no S */
static enum cpu_stop h_dp_shimm(cpu_t *c, const struct dentry *e)
{
    uint32_t insn = e->insn, v = R(c, E_RM(e)), amt = (insn >> 7) & 31u, op2;
    switch ((insn >> 5) & 3u) {
    case 0: op2 = v << amt; break;
    case 1: op2 = v >> amt; break;
    default: op2 = (uint32_t)((int32_t)v >> amt); break;
    }
    uint32_t a = R(c, E_RN(e)), res;
    switch ((insn >> 21) & 15u) {
    case 0x0: res = a & op2; break;
    case 0x1: res = a ^ op2; break;
    case 0x2: res = a - op2; break;
    case 0x3: res = op2 - a; break;
    case 0x4: res = a + op2; break;
    case 0xC: res = a | op2; break;
    case 0xD: res = op2; break;
    case 0xE: res = a & ~op2; break;
    default: res = ~op2; break; /* MVN */
    }
    R(c, E_RD(e)) = res;
    return CPU_STOP_NONE;
}

/* single loads and stores, pre-indexed without writeback (aux = signed offset) */
static enum cpu_stop h_ldr_imm(cpu_t *c, const struct dentry *e)
{
    uint32_t v;
    if (ld_word(c, R(c, E_RN(e)) + e->aux, &v) != GP_OK)
        return data_abort(c, e->insn);
    R(c, E_RD(e)) = v;
    return CPU_STOP_NONE;
}
static enum cpu_stop h_ldr_lit(cpu_t *c, const struct dentry *e) /* ldr rd, [pc, #off] */
{
    uint32_t v;
    if (ld_word(c, c->pc + 8 + e->aux, &v) != GP_OK)
        return data_abort(c, e->insn);
    R(c, E_RD(e)) = v;
    return CPU_STOP_NONE;
}
static enum cpu_stop h_ldrb_imm(cpu_t *c, const struct dentry *e)
{
    uint32_t v;
    if (cpu_ld8(c, R(c, E_RN(e)) + e->aux, &v) != GP_OK)
        return data_abort(c, e->insn);
    R(c, E_RD(e)) = v;
    return CPU_STOP_NONE;
}
static enum cpu_stop h_str_imm(cpu_t *c, const struct dentry *e)
{
    if (st_word(c, R(c, E_RN(e)) + e->aux, R(c, E_RD(e))) != GP_OK)
        return data_abort(c, e->insn);
    return CPU_STOP_NONE;
}
static enum cpu_stop h_strb_imm(cpu_t *c, const struct dentry *e)
{
    if (cpu_st8(c, R(c, E_RN(e)) + e->aux, R(c, E_RD(e)) & 0xFFu) != GP_OK)
        return data_abort(c, e->insn);
    return CPU_STOP_NONE;
}
/* register offset, LSL #0..31 (aux = amount | up << 8) */
static inline uint32_t reg_addr(cpu_t *c, const struct dentry *e)
{
    uint32_t off = R(c, E_RM(e)) << (e->aux & 31u);
    return (e->aux & 0x100u) ? R(c, E_RN(e)) + off : R(c, E_RN(e)) - off;
}
static enum cpu_stop h_ldr_reg(cpu_t *c, const struct dentry *e)
{
    uint32_t v;
    if (ld_word(c, reg_addr(c, e), &v) != GP_OK)
        return data_abort(c, e->insn);
    R(c, E_RD(e)) = v;
    return CPU_STOP_NONE;
}
static enum cpu_stop h_ldrb_reg(cpu_t *c, const struct dentry *e)
{
    uint32_t v;
    if (cpu_ld8(c, reg_addr(c, e), &v) != GP_OK)
        return data_abort(c, e->insn);
    R(c, E_RD(e)) = v;
    return CPU_STOP_NONE;
}
static enum cpu_stop h_str_reg(cpu_t *c, const struct dentry *e)
{
    if (st_word(c, reg_addr(c, e), R(c, E_RD(e))) != GP_OK)
        return data_abort(c, e->insn);
    return CPU_STOP_NONE;
}
static enum cpu_stop h_strb_reg(cpu_t *c, const struct dentry *e)
{
    if (cpu_st8(c, reg_addr(c, e), R(c, E_RD(e)) & 0xFFu) != GP_OK)
        return data_abort(c, e->insn);
    return CPU_STOP_NONE;
}
/* halfword and signed transfers, immediate offset, pre-indexed without writeback */
static enum cpu_stop h_ldrh_imm(cpu_t *c, const struct dentry *e)
{
    uint32_t v;
    if (ld_half(c, R(c, E_RN(e)) + e->aux, &v) != GP_OK)
        return data_abort(c, e->insn);
    R(c, E_RD(e)) = v;
    return CPU_STOP_NONE;
}
static enum cpu_stop h_ldrsh_imm(cpu_t *c, const struct dentry *e)
{
    uint32_t v;
    if (ld_half(c, R(c, E_RN(e)) + e->aux, &v) != GP_OK)
        return data_abort(c, e->insn);
    R(c, E_RD(e)) = (uint32_t)(int32_t)(int16_t)v;
    return CPU_STOP_NONE;
}
static enum cpu_stop h_ldrsb_imm(cpu_t *c, const struct dentry *e)
{
    uint32_t v;
    if (cpu_ld8(c, R(c, E_RN(e)) + e->aux, &v) != GP_OK)
        return data_abort(c, e->insn);
    R(c, E_RD(e)) = (uint32_t)(int32_t)(int8_t)v;
    return CPU_STOP_NONE;
}
static enum cpu_stop h_strh_imm(cpu_t *c, const struct dentry *e)
{
    if (st_half(c, R(c, E_RN(e)) + e->aux, R(c, E_RD(e))) != GP_OK)
        return data_abort(c, e->insn);
    return CPU_STOP_NONE;
}
/* branches (aux = offset + 8) */
static enum cpu_stop h_b(cpu_t *c, const struct dentry *e) { c->next_pc = c->pc + e->aux; return CPU_STOP_NONE; }
static enum cpu_stop h_bl(cpu_t *c, const struct dentry *e)
{
    R(c, 14) = c->pc + 4;
    c->next_pc = c->pc + e->aux;
    return CPU_STOP_NONE;
}


/* ---- second round, from a histogram of the forms still taking h_generic */

/* single loads and stores that write the base back: pre-indexed with W, or
 * post-indexed (aux = signed offset; bit 24 of the word = P). The base is
 * written only after the access succeeds, as in exec_load_store. */
#define E_PRE(e) (((e)->insn >> 24) & 1u)
static inline gaddr_t wb_addr(cpu_t *c, const struct dentry *e) { return E_PRE(e) ? R(c, E_RN(e)) + e->aux : R(c, E_RN(e)); }
static enum cpu_stop h_ldr_wb(cpu_t *c, const struct dentry *e)
{
    uint32_t v;
    if (ld_word(c, wb_addr(c, e), &v) != GP_OK)
        return data_abort(c, e->insn);
    R(c, E_RN(e)) += e->aux;
    R(c, E_RD(e)) = v;
    return CPU_STOP_NONE;
}
static enum cpu_stop h_ldrb_wb(cpu_t *c, const struct dentry *e)
{
    uint32_t v;
    if (cpu_ld8(c, wb_addr(c, e), &v) != GP_OK)
        return data_abort(c, e->insn);
    R(c, E_RN(e)) += e->aux;
    R(c, E_RD(e)) = v;
    return CPU_STOP_NONE;
}
static enum cpu_stop h_str_wb(cpu_t *c, const struct dentry *e)
{
    if (st_word(c, wb_addr(c, e), R(c, E_RD(e))) != GP_OK)
        return data_abort(c, e->insn);
    R(c, E_RN(e)) += e->aux;
    return CPU_STOP_NONE;
}
static enum cpu_stop h_strb_wb(cpu_t *c, const struct dentry *e)
{
    if (cpu_st8(c, wb_addr(c, e), R(c, E_RD(e)) & 0xFFu) != GP_OK)
        return data_abort(c, e->insn);
    R(c, E_RN(e)) += e->aux;
    return CPU_STOP_NONE;
}
/* halfword transfers, register offset, pre-indexed without writeback (aux = up) */
static inline gaddr_t hreg_addr(cpu_t *c, const struct dentry *e) { return e->aux ? R(c, E_RN(e)) + R(c, E_RM(e)) : R(c, E_RN(e)) - R(c, E_RM(e)); }
static enum cpu_stop h_ldrh_reg(cpu_t *c, const struct dentry *e)
{
    uint32_t v;
    if (ld_half(c, hreg_addr(c, e), &v) != GP_OK)
        return data_abort(c, e->insn);
    R(c, E_RD(e)) = v;
    return CPU_STOP_NONE;
}
static enum cpu_stop h_ldrsh_reg(cpu_t *c, const struct dentry *e)
{
    uint32_t v;
    if (ld_half(c, hreg_addr(c, e), &v) != GP_OK)
        return data_abort(c, e->insn);
    R(c, E_RD(e)) = (uint32_t)(int32_t)(int16_t)v;
    return CPU_STOP_NONE;
}
static enum cpu_stop h_ldrsb_reg(cpu_t *c, const struct dentry *e)
{
    uint32_t v;
    if (cpu_ld8(c, hreg_addr(c, e), &v) != GP_OK)
        return data_abort(c, e->insn);
    R(c, E_RD(e)) = (uint32_t)(int32_t)(int8_t)v;
    return CPU_STOP_NONE;
}
static enum cpu_stop h_strh_reg(cpu_t *c, const struct dentry *e)
{
    if (st_half(c, hreg_addr(c, e), R(c, E_RD(e))) != GP_OK)
        return data_abort(c, e->insn);
    return CPU_STOP_NONE;
}
/* LDR with a register offset where Rn or Rd is pc (jump tables: ldr pc, [pc, r0, lsl #2]) */
static enum cpu_stop h_ldr_reg_pc(cpu_t *c, const struct dentry *e)
{
    uint32_t off = R(c, E_RM(e)) << (e->aux & 31u);
    uint32_t base = E_RN(e) == 15 ? c->pc + 8 : R(c, E_RN(e));
    uint32_t v;
    if (ld_word(c, (e->aux & 0x100u) ? base + off : base - off, &v) != GP_OK)
        return data_abort(c, e->insn);
    if (E_RD(e) == 15)
        return branch_to(c, v, false, e->insn);
    R(c, E_RD(e)) = v;
    return CPU_STOP_NONE;
}
static enum cpu_stop h_bx(cpu_t *c, const struct dentry *e) { return branch_to(c, R(c, E_RM(e)), true, e->insn); }
static enum cpu_stop h_mov_pc(cpu_t *c, const struct dentry *e) { return branch_to(c, R(c, E_RM(e)), false, e->insn); }
static enum cpu_stop h_adr(cpu_t *c, const struct dentry *e) { R(c, E_RD(e)) = c->pc + 8 + e->aux; return CPU_STOP_NONE; }
static enum cpu_stop h_tst_imm_c(cpu_t *c, const struct dentry *e) /* rotated immediate: C = bit 31 of it */
{
    set_nz(c, R(c, E_RN(e)) & e->aux);
    set_c(c, e->aux >> 31);
    return CPU_STOP_NONE;
}
static enum cpu_stop h_mul(cpu_t *c, const struct dentry *e) /* MUL/MLA, same as exec_multiply */
{
    uint32_t insn = e->insn, res = R(c, insn & 15u) * R(c, (insn >> 8) & 15u);
    if ((insn >> 21) & 1u)
        res += R(c, (insn >> 12) & 15u);
    R(c, (insn >> 16) & 15u) = res;
    if ((insn >> 20) & 1u)
        set_nz(c, res);
    return CPU_STOP_NONE;
}
/* data processing, register shifted by a register, no S, no pc operands */
static enum cpu_stop h_dp_shreg(cpu_t *c, const struct dentry *e)
{
    uint32_t insn = e->insn, v = R(c, E_RM(e)), amt = R(c, (insn >> 8) & 15u) & 0xFFu, op2;
    switch ((insn >> 5) & 3u) {
    case 0: op2 = amt >= 32 ? 0 : v << amt; break;
    case 1: op2 = amt >= 32 ? 0 : v >> amt; break;
    case 2: op2 = amt >= 32 ? (uint32_t)((int32_t)v >> 31) : (uint32_t)((int32_t)v >> amt); break;
    default: amt &= 31u; op2 = amt ? (v >> amt) | (v << (32u - amt)) : v; break;
    }
    uint32_t a = R(c, E_RN(e)), res;
    switch ((insn >> 21) & 15u) {
    case 0x0: res = a & op2; break;
    case 0x1: res = a ^ op2; break;
    case 0x2: res = a - op2; break;
    case 0x3: res = op2 - a; break;
    case 0x4: res = a + op2; break;
    case 0xC: res = a | op2; break;
    case 0xD: res = op2; break;
    case 0xE: res = a & ~op2; break;
    default: res = ~op2; break;
    }
    R(c, E_RD(e)) = res;
    return CPU_STOP_NONE;
}

/* Second-round forms; returns true when it chose a handler. */
static bool decode_more(struct dentry *e, uint32_t insn)
{
    unsigned cls = (insn >> 25) & 7u, op = (insn >> 21) & 15u, s = (insn >> 20) & 1u;
    unsigned rd = (insn >> 12) & 15u, rn = (insn >> 16) & 15u, rm = insn & 15u;
    bool p = (insn >> 24) & 1u, u = (insn >> 23) & 1u, w = (insn >> 21) & 1u, l = (insn >> 20) & 1u;
    bool misc = ((insn >> 23) & 3u) == 2u && !s;
    if (cls == 2 && (!p || w) && rn != 15 && rd != 15 && !(l && rn == rd)) {
        uint32_t off = insn & 0xFFFu;
        e->aux = u ? off : 0u - off;
        bool b = (insn >> 22) & 1u;
        e->fn = l ? (b ? h_ldrb_wb : h_ldr_wb) : (b ? h_strb_wb : h_str_wb);
        return true;
    }
    if (cls == 3 && !(insn & 0x10u) && p && !w && l && !((insn >> 22) & 1u) && ((insn >> 5) & 3u) == 0 && rm != 15 &&
        (rn == 15 || rd == 15)) {
        e->aux = ((insn >> 7) & 31u) | (u ? 0x100u : 0u);
        e->fn = h_ldr_reg_pc;
        return true;
    }
    if (cls == 0 && (insn & 0x90u) == 0x90u) {
        unsigned kind = (insn >> 5) & 3u;
        if (kind == 0) {
            if (((insn >> 22) & 0x3Fu) == 0) { /* MUL/MLA */
                e->fn = h_mul;
                return true;
            }
            return false;
        }
        if (!p || w || ((insn >> 22) & 1u) || rn == 15 || rd == 15 || rm == 15 || ((insn >> 8) & 15u) != 0)
            return false;
        e->aux = u;
        if (!l) {
            if (kind != 1)
                return false;
            e->fn = h_strh_reg;
        } else {
            e->fn = kind == 1 ? h_ldrh_reg : kind == 2 ? h_ldrsb_reg : h_ldrsh_reg;
        }
        return true;
    }
    if (cls == 0 && (insn & 0x0FFFFFF0u) == 0x012FFF10u && rm != 15) {
        e->fn = h_bx;
        return true;
    }
    if (cls == 0 && !misc && op == 0xD && !s && rd == 15 && (insn & 0xFF0u) == 0 && rm != 15) {
        e->fn = h_mov_pc;
        return true;
    }
    if (cls == 0 && !misc && !s && (insn & 0x10u) && !(insn & 0x80u) && rd != 15 && rm != 15 &&
        ((insn >> 8) & 15u) != 15 && (op <= 0x4 || op >= 0xC) && (op == 0xD || op == 0xF || rn != 15)) {
        e->fn = h_dp_shreg;
        return true;
    }
    if (cls == 1 && !misc) {
        uint32_t imm = insn & 0xFFu, rot = ((insn >> 8) & 15u) * 2u;
        imm = rot ? (imm >> rot) | (imm << (32u - rot)) : imm;
        if (!s && rn == 15 && rd != 15 && (op == 0x4 || op == 0x2)) {
            e->aux = op == 0x4 ? imm : 0u - imm;
            e->fn = h_adr;
            return true;
        }
        if (s && op == 0x8 && rot != 0 && rn != 15) {
            e->aux = imm;
            e->fn = h_tst_imm_c;
            return true;
        }
    }
    return false;
}

static void decode_first(struct dentry *e, uint32_t insn)
{
    e->insn = insn;
    e->fn = h_generic;
    e->aux = 0;
    unsigned cls = (insn >> 25) & 7u, op = (insn >> 21) & 15u, s = (insn >> 20) & 1u;
    unsigned rd = (insn >> 12) & 15u, rn = (insn >> 16) & 15u, rm = insn & 15u;
    bool p = (insn >> 24) & 1u, u = (insn >> 23) & 1u, w = (insn >> 21) & 1u, l = (insn >> 20) & 1u;
    bool misc = ((insn >> 23) & 3u) == 2u && !s; /* MRS/MSR/BX space (TST..CMN without S) */
    bool uses_rn = op != 0xD && op != 0xF;
    switch (cls) {
    case 1: { /* data processing, immediate */
        if (misc)
            return;
        uint32_t imm = insn & 0xFFu, rot = ((insn >> 8) & 15u) * 2u;
        imm = rot ? (imm >> rot) | (imm << (32u - rot)) : imm;
        if (uses_rn && rn == 15)
            return;
        if (!s) {
            if (rd == 15)
                return;
            switch (op) {
            case 0xD: e->aux = imm; e->fn = h_mov_imm; return;
            case 0xF: e->aux = ~imm; e->fn = h_mov_imm; return;
            case 0x4: e->aux = imm; e->fn = h_add_imm; return;
            case 0x2: e->aux = 0u - imm; e->fn = h_add_imm; return;
            case 0x3: e->aux = imm; e->fn = h_rsb_imm; return;
            case 0x0: e->aux = imm; e->fn = h_and_imm; return;
            case 0xE: e->aux = ~imm; e->fn = h_and_imm; return;
            case 0xC: e->aux = imm; e->fn = h_orr_imm; return;
            case 0x1: e->aux = imm; e->fn = h_eor_imm; return;
            default: return;
            }
        }
        e->aux = imm;
        switch (op) {
        case 0xA: e->fn = h_cmp_imm; return;
        case 0xB: e->fn = h_cmn_imm; return;
        case 0x8: if (rot == 0) e->fn = h_tst_imm; return;
        case 0x2: if (rd != 15) e->fn = h_subs_imm; return;
        case 0x4: if (rd != 15) e->fn = h_adds_imm; return;
        default: return;
        }
    }
    case 0: {
        if ((insn & 0x90u) == 0x90u) { /* multiply, SWP, halfword transfers */
            unsigned kind = (insn >> 5) & 3u;
            if (kind == 0 || !p || w || !((insn >> 22) & 1u) || rn == 15 || rd == 15)
                return;
            uint32_t off = ((insn >> 4) & 0xF0u) | (insn & 0xFu);
            e->aux = u ? off : 0u - off;
            if (!l)
                e->fn = kind == 1 ? h_strh_imm : h_generic; /* LDRD/STRD are ARMv5: generic (undefined) */
            else
                e->fn = kind == 1 ? h_ldrh_imm : kind == 2 ? h_ldrsb_imm : h_ldrsh_imm;
            return;
        }
        if (misc || (insn & 0x10u) || rm == 15 || (uses_rn && rn == 15))
            return; /* register-specified shifts and pc operands: generic */
        if ((insn & 0xFF0u) == 0) { /* register, no shift */
            if (!s) {
                if (rd == 15)
                    return;
                switch (op) {
                case 0xD: e->fn = h_mov_reg; return;
                case 0xF: e->fn = h_mvn_reg; return;
                case 0x4: e->fn = h_add_reg; return;
                case 0x2: e->fn = h_sub_reg; return;
                case 0x3: e->fn = h_rsb_reg; return;
                case 0x0: e->fn = h_and_reg; return;
                case 0xC: e->fn = h_orr_reg; return;
                case 0x1: e->fn = h_eor_reg; return;
                case 0xE: e->fn = h_bic_reg; return;
                default: return;
                }
            }
            switch (op) {
            case 0xA: e->fn = h_cmp_reg; return;
            case 0xB: e->fn = h_cmn_reg; return;
            case 0x8: e->fn = h_tst_reg; return;
            case 0x2: if (rd != 15) e->fn = h_subs_reg; return;
            case 0x4: if (rd != 15) e->fn = h_adds_reg; return;
            case 0xD: if (rd != 15) e->fn = h_movs_reg; return;
            default: return;
            }
        }
        unsigned type = (insn >> 5) & 3u, amt = (insn >> 7) & 31u;
        if (s || rd == 15 || type == 3 || amt == 0)
            return;
        if (op <= 0x4 || op >= 0xC)
            e->fn = h_dp_shimm;
        return;
    }
    case 2: { /* LDR/STR, immediate offset */
        if (!p || w || rd == 15)
            return;
        uint32_t off = insn & 0xFFFu;
        e->aux = u ? off : 0u - off;
        bool b = (insn >> 22) & 1u;
        if (rn == 15) {
            if (l && !b)
                e->fn = h_ldr_lit;
            return;
        }
        e->fn = l ? (b ? h_ldrb_imm : h_ldr_imm) : (b ? h_strb_imm : h_str_imm);
        return;
    }
    case 3: { /* LDR/STR, register offset */
        if ((insn & 0x10u) || !p || w || rd == 15 || rn == 15 || rm == 15 || ((insn >> 5) & 3u) != 0)
            return;
        e->aux = ((insn >> 7) & 31u) | (u ? 0x100u : 0u);
        bool b = (insn >> 22) & 1u;
        e->fn = l ? (b ? h_ldrb_reg : h_ldr_reg) : (b ? h_strb_reg : h_str_reg);
        return;
    }
    case 5:
        e->aux = (uint32_t)((int32_t)(insn << 8) >> 6) + 8u;
        e->fn = ((insn >> 24) & 1u) ? h_bl : h_b;
        return;
    default:
        return;
    }
}

static void decode(struct dentry *e, uint32_t insn)
{
    decode_first(e, insn);
    if (e->fn == h_generic)
        (void)decode_more(e, insn);
}

#ifdef GPORT2X_JIT
#include "jit_common.inc"
#if defined(__x86_64__)
#include "jit_x86_64.inc"
#else
#include "jit_aarch64.inc"
#endif
#include "jit_dispatch.inc"
void cpu_set_jit(bool on) { jit_on = on; }
bool cpu_jit_available(void) { return true; }
#else
void cpu_set_jit(bool on) { (void)on; }
bool cpu_jit_available(void) { return false; }
#endif

enum cpu_stop cpu_run(cpu_t *c, uint64_t max_insns, cpu_stop_info_t *info)
{
    if (!c)
        return CPU_STOP_HALT;
    memset(&c->stop, 0, sizeof c->stop);
    tlb_sync(c);
    uint64_t budget = max_insns ? max_insns : UINT64_MAX;
    enum cpu_stop reason = CPU_STOP_NONE;
#ifdef GPORT2X_JIT
    struct { uint8_t *at; const uint8_t *page; uint64_t epoch; gaddr_t pc; } link = { NULL, NULL, 0, 0 };
    bool jit_tail = false;
    bool jit_try = true; /* look for a block only where one can start: run start, after a branch or a block */
#endif
    while (budget) {
        if (c->stop_requested) {
            c->stop_requested = false;
            reason = CPU_STOP_HALT;
            c->stop.pc = R(c, 15);
            break;
        }
        gaddr_t pc = R(c, 15) & ~3u;
        c->pc = pc;
        if (c->nbps && at_breakpoint(c, pc) && !(c->have_resume_pc && c->resume_pc == pc)) {
            reason = CPU_STOP_BREAKPOINT;
            c->stop.pc = pc;
            c->resume_pc = pc;
            c->have_resume_pc = true;
            break;
        }
        c->have_resume_pc = false;
        uint32_t insn;
        const struct dentry *e;
        struct dentry tmp;
        struct dpage *dp = &c->dcache[(pc >> GP2X_PAGE_SHIFT) & (ICACHE_SIZE - 1u)];
        gaddr_t base = pc & ~(gaddr_t)(GP2X_PAGE_SIZE - 1u);
        if (dp->page != base) {
            const uint8_t *h = gmem_page_host(c->m, pc, GMEM_PROT_X);
            if (h) {
                dp->page = base;
                dp->host = h;
                dp->nojit = 0;
            } else {
                dp = NULL;
            }
        }
#ifdef GPORT2X_JIT
        if (dp && jit_on && !c->nbps && !jit_tail && jit_try) {
            struct jblock *jb = jit_get(c, dp, pc);
            if (jb && jb->n > budget)
                jit_tail = true; /* interpret the rest of this run: no new blocks at budget-split addresses */
            if (jb && jb->n <= budget) {
                if (link.at && link.epoch == jit_epoch && link.pc == pc && jb->host_page == link.page)
                    jit_link(link.at, jb->entry2); /* chain the block that exited here to this one */
                link.at = NULL;
                jit_dirty = 0;
                c->jlink_at = NULL;
                uint32_t k = jb->code(c, budget > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)budget);
                if (c->jlink_at) {
                    link.at = c->jlink_at;
                    link.page = c->jlink_page;
                    link.epoch = jit_epoch;
                    link.pc = R(c, 15);
                }
                budget -= k;
                c->insns += k;
                if (c->jreason != CPU_STOP_NONE) {
                    reason = (enum cpu_stop)c->jreason;
                    break;
                }
                continue;
            }
        }
#endif
        budget--;
        if (dp) {
            uint32_t off = pc & (GP2X_PAGE_SIZE - 1u);
            memcpy(&insn, dp->host + off, 4);
            struct dentry *de = &dp->e[off >> 2];
            if (de->insn != insn || !de->fn)
                decode(de, insn);
            e = de;
        } else { /* executable page without host memory (never for the game): checked fetch */
            if (gmem_fetch32(c->m, pc, &insn) != GP_OK) {
                reason = CPU_STOP_PREFETCH_ABORT;
                c->stop.pc = pc;
                c->stop.fault_addr = pc;
                c->stop.fault_prot = GMEM_PROT_X;
                break;
            }
            decode(&tmp, insn);
            e = &tmp;
        }
        c->next_pc = pc + 4;
        c->insns++;
        if (condition_passed(CPSR(c), insn >> 28))
            reason = e->fn(c, e);
#ifdef GPORT2X_JIT
        jit_try = c->next_pc != pc + 4u;
#endif
        R(c, 15) = c->next_pc;
        if (reason != CPU_STOP_NONE)
            break;
    }
    if (reason == CPU_STOP_NONE) {
        reason = CPU_STOP_LIMIT;
        c->stop.pc = R(c, 15);
    }
    c->stop.reason = reason;
    if (info)
        *info = c->stop;
    return reason;
}
