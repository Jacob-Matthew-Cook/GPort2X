/* JIT, the parts shared by the backends (docs/JIT.md), included by cpu.c.
 * The backends (jit_x86_64.inc, jit_aarch64.inc) define jit_translate and
 * jit_link; jit_dispatch.inc then defines jit_get.
 *
 * A block is a run of guest instructions inside one guest page, keyed by its
 * guest pc and the host address of its first instruction (so a remapped or
 * reloaded page never matches an old block). Each instruction becomes either
 * inline host code (register moves, ALU operations without flags, constant
 * loads, branches) or a direct call to its decoded handler, followed by the
 * checks the interpreter makes: a nonzero stop reason or a changed next_pc
 * ends the block. The block returns how many guest instructions it retired
 * (including one that stopped), leaving R15 = next_pc and c->jreason, so
 * instruction counts, budgets and stop states equal the interpreter's.
 *
 * Invalidation follows QEMU's user-mode scheme in software: translating marks
 * the page (gmem_mark_code); marked pages never enter a write cache, so any
 * store to them reaches gmem, which calls jit_invalidate_page. A block that
 * stores re-checks jit_dirty and leaves if its store invalidated anything.
 *
 * Chaining (QEMU's goto_tb, dynarmic's LinkBlock): a direct branch or the
 * fall-through whose target lies in the same guest page leaves through a link
 * stub. Unlinked, the stub records itself in c->jlink_at and exits; cpu_run
 * patches it to the target block's second entry once that block exists. Both
 * blocks then share a host page, so an invalidation kills them together and
 * no link ever points at a dead block from a live one. Register use inside
 * generated code: rbx = cpu, r12d = budget left at the current block's entry,
 * r13d = instructions retired by earlier blocks in this call (x86-64; the
 * AArch64 backend uses x19, w20 and w21 the same way). The second
 * entry returns to C before running a block longer than the budget left, so
 * counts stay exact. When the code buffer fills, everything is dropped
 * (dynarmic's policy) between blocks. */

#define JIT_BUF_SIZE (32u << 20)
#define JIT_HASH_SIZE 65536u
#define JIT_MAX_INSNS 16u
#define JIT_HOT_THRESHOLD 16u /* executions of a block start before it is compiled */
#define JIT_INSN_BYTES 640u   /* upper bound of host code per guest instruction */
#define JIT_STUB_BYTES 160u

struct jblock {
    gaddr_t pc;
    const uint8_t *host;       /* host address of the first instruction */
    const uint8_t *host_page;  /* its page, for invalidation */
    uint32_t n;                /* guest instructions */
    uint32_t (*code)(cpu_t *c, uint32_t budget); /* entry from C */
    uint8_t *entry2;           /* entry from a linked block: checks the budget left in r12d */
    struct jblock *next;       /* hash chain, or the dead list */
    struct dentry ents[];
};

static bool jit_on = true;
static uint8_t *jit_buf;
static size_t jit_used;
static struct jblock **jit_hash;
static struct jblock *jit_dead;
static uint64_t jit_epoch = 1;
static volatile uint8_t jit_dirty;
static bool jit_broken; /* the buffer could not be allocated: interpreter only */
static unsigned jit_hot = JIT_HOT_THRESHOLD; /* GPORT2X_JIT_HOT overrides (the tests use 0: compile at once) */

static void jit_free_list(struct jblock *b)
{
    while (b) {
        struct jblock *n = b->next;
        free(b);
        b = n;
    }
}

static void jit_flush_all(void)
{
    for (uint32_t i = 0; i < JIT_HASH_SIZE; i++) {
        jit_free_list(jit_hash[i]);
        jit_hash[i] = NULL;
    }
    jit_free_list(jit_dead);
    jit_dead = NULL;
    jit_used = 0;
    jit_epoch++;
}

static void jit_invalidate_page(const uint8_t *host_page)
{
    if (!jit_hash)
        return;
    for (uint32_t i = 0; i < JIT_HASH_SIZE; i++) {
        struct jblock **pp = &jit_hash[i];
        while (*pp) {
            struct jblock *b = *pp;
            if (b->host_page == host_page) {
                *pp = b->next;
                b->next = jit_dead;
                jit_dead = b;
            } else {
                pp = &b->next;
            }
        }
    }
    jit_epoch++;
    jit_dirty = 1;
}

static bool jit_init(void)
{
    if (jit_buf)
        return true;
    if (jit_broken)
        return false;
    void *p = mmap(NULL, JIT_BUF_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    jit_hash = calloc(JIT_HASH_SIZE, sizeof *jit_hash);
    if (p == MAP_FAILED || !jit_hash) {
        jit_broken = true; /* e.g. a kernel that forbids writable-executable memory */
        if (p != MAP_FAILED)
            munmap(p, JIT_BUF_SIZE);
        free(jit_hash);
        jit_hash = NULL;
        return false;
    }
    jit_buf = p;
    gmem_set_code_hook(jit_invalidate_page);
    const char *h = getenv("GPORT2X_JIT_HOT");
    if (h && *h)
        jit_hot = (unsigned)strtoul(h, NULL, 10) > 255u ? 255u : (unsigned)strtoul(h, NULL, 10);
    return true;
}

static inline uint32_t jit_bucket(gaddr_t pc, const uint8_t *host)
{
    return ((pc >> 2) ^ (uint32_t)((uintptr_t)host >> 4)) & (JIT_HASH_SIZE - 1u);
}

#define OFF_R(n) ((uint32_t)(offsetof(struct cpu, regs) + offsetof(cpu_regs_t, r) + 4u * (n)))
#define OFF_CPSR ((uint32_t)(offsetof(struct cpu, regs) + offsetof(cpu_regs_t, cpsr)))
#define OFF_PC ((uint32_t)offsetof(struct cpu, pc))
#define OFF_NEXT ((uint32_t)offsetof(struct cpu, next_pc))
#define OFF_JREASON ((uint32_t)offsetof(struct cpu, jreason))
#define OFF_JLINK_AT ((uint32_t)offsetof(struct cpu, jlink_at))
#define OFF_JLINK_PAGE ((uint32_t)offsetof(struct cpu, jlink_page))

enum { JK_ALU, JK_MEM, JK_STORE, JK_BRANCH, JK_ANY };
static int jit_kind(dhandler_t fn)
{
    if (fn == h_str_imm || fn == h_strb_imm || fn == h_str_reg || fn == h_strb_reg || fn == h_strh_imm ||
        fn == h_strh_reg || fn == h_str_wb || fn == h_strb_wb)
        return JK_STORE;
    if (fn == h_ldr_imm || fn == h_ldr_lit || fn == h_ldrb_imm || fn == h_ldr_reg || fn == h_ldrb_reg ||
        fn == h_ldrh_imm || fn == h_ldrsh_imm || fn == h_ldrsb_imm || fn == h_ldr_wb || fn == h_ldrb_wb ||
        fn == h_ldrh_reg || fn == h_ldrsh_reg || fn == h_ldrsb_reg)
        return JK_MEM;
    if (fn == h_b || fn == h_bl)
        return JK_BRANCH;
    if (fn == h_generic || fn == h_bx || fn == h_mov_pc || fn == h_ldr_reg_pc)
        return JK_ANY;
    return JK_ALU;
}

enum { MW, MB, MH, MSH, MSB };
static bool is_wb(dhandler_t f) { return f == h_ldr_wb || f == h_ldrb_wb || f == h_str_wb || f == h_strb_wb; }
