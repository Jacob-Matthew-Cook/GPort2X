/* One-instruction emulator for the native engine: see include/gport2x/aemu.h. */
#include "gport2x/aemu.h"

#include <stdlib.h>
#include <string.h>

#include "gport2x/cpu.h"
#include "gport2x/gmem.h"

#define MAX_PAGES 8 /* LDM/STM of 16 words spans at most 2 pages; FPA at most 2; fetch 1 */

struct aemu {
    aemu_page_fn page_host;
    void *ctx;
    gmem_t *m;
    cpu_t *c;
    gaddr_t mapped[MAX_PAGES];
    unsigned nmapped;
};

aemu_t *aemu_create(aemu_page_fn page_host, void *ctx)
{
    aemu_t *a = calloc(1, sizeof *a);
    if (!a || !page_host)
        goto fail;
    a->page_host = page_host;
    a->ctx = ctx;
    a->m = gmem_create_mode(false); /* pages are wrapped host memory, never native mappings */
    a->c = a->m ? cpu_create(a->m) : NULL;
    if (!a->c)
        goto fail;
    return a;
fail:
    if (a) {
        if (a->m)
            gmem_destroy(a->m);
        free(a);
    }
    return NULL;
}

void aemu_destroy(aemu_t *a)
{
    if (!a)
        return;
    cpu_destroy(a->c);
    gmem_destroy(a->m);
    free(a);
}

static bool map_page(aemu_t *a, gaddr_t page, int prot)
{
    if (a->nmapped == MAX_PAGES)
        return false;
    uint8_t *h = a->page_host(a->ctx, page, prot);
    if (!h)
        return false;
    gmem_obj_t *o = gmem_obj_wrap(h, GP2X_PAGE_SIZE);
    if (!o)
        return false;
    int r = gmem_map_obj(a->m, page, GP2X_PAGE_SIZE, prot, o, 0);
    gmem_obj_release(o); /* the mapping holds it */
    if (r != GP_OK)
        return false;
    a->mapped[a->nmapped++] = page;
    return true;
}

static void unmap_all(aemu_t *a)
{
    for (unsigned i = 0; i < a->nmapped; i++)
        gmem_unmap(a->m, a->mapped[i], GP2X_PAGE_SIZE);
    a->nmapped = 0;
}

static bool cond_passed(uint32_t cpsr, unsigned cond)
{
    bool n = cpsr >> 31 & 1, z = cpsr >> 30 & 1, c = cpsr >> 29 & 1, v = cpsr >> 28 & 1;
    switch (cond) {
    case 0x0: return z;
    case 0x1: return !z;
    case 0x2: return c;
    case 0x3: return !c;
    case 0x4: return n;
    case 0x5: return !n;
    case 0x6: return v;
    case 0x7: return !v;
    case 0x8: return c && !z;
    case 0x9: return !c || z;
    case 0xA: return n == v;
    case 0xB: return n != v;
    case 0xC: return !z && n == v;
    case 0xD: return z || n != v;
    case 0xE: return true;
    default: return false;
    }
}

/* SWP{B} Rd, Rm, [Rn]: an atomic exchange on the host location. */
static enum aemu_result do_swp(aemu_t *a, uint32_t insn, uint32_t r[16], gaddr_t *fault)
{
    unsigned rn = insn >> 16 & 15u, rd = insn >> 12 & 15u, rm = insn & 15u;
    bool byte = insn >> 22 & 1u;
    if (rn == 15 || rd == 15 || rm == 15)
        return AEMU_UNDEF; /* unpredictable */
    gaddr_t addr = r[rn];
    gaddr_t word = byte ? addr : addr & ~3u;
    uint8_t *h = a->page_host(a->ctx, word & GP2X_PAGE_MASK, GMEM_PROT_RW);
    if (!h) {
        *fault = addr;
        return AEMU_SEGV;
    }
    h += word & (GP2X_PAGE_SIZE - 1u);
    uint32_t old;
    if (byte) {
        old = __atomic_exchange_n(h, (uint8_t)r[rm], __ATOMIC_SEQ_CST);
    } else {
        old = __atomic_exchange_n((uint32_t *)(void *)h, r[rm], __ATOMIC_SEQ_CST);
        unsigned rot = (addr & 3u) * 8u; /* ARMv4: the loaded word is rotated, the store is aligned */
        if (rot)
            old = old >> rot | old << (32u - rot);
    }
    r[rd] = old;
    r[15] += 4;
    return AEMU_OK;
}

enum aemu_result aemu_step(aemu_t *a, uint32_t r[16], uint32_t *cpsr, gaddr_t *fault)
{
    gaddr_t pc = r[15] & ~3u;
    *fault = 0;
    uint8_t *code = a->page_host(a->ctx, pc & GP2X_PAGE_MASK, GMEM_PROT_R);
    if (!code) {
        *fault = pc;
        return AEMU_SEGV;
    }
    uint32_t insn;
    memcpy(&insn, code + (pc & (GP2X_PAGE_SIZE - 1u)), 4);
    if (!cond_passed(*cpsr, insn >> 28)) { /* an undefined encoding may trap even when its condition fails */
        r[15] = pc + 4;
        return AEMU_OK;
    }
    if ((insn & 0x0FB00FF0u) == 0x01000090u) /* SWP / SWPB */
        return do_swp(a, insn, r, fault);
    /* Everything else: one interpreter step, mapping each page it touches. */
    cpu_regs_t *cr = cpu_regs(a->c);
    enum aemu_result res = AEMU_UNDEF;
    if (!map_page(a, pc & GP2X_PAGE_MASK, GMEM_PROT_R | GMEM_PROT_X)) {
        *fault = pc;
        return AEMU_SEGV;
    }
    for (int attempt = 0; attempt < MAX_PAGES; attempt++) {
        memcpy(cr->r, r, sizeof cr->r);
        cr->r[15] = pc;
        cr->cpsr = (*cpsr & 0xF0000000u) | CPSR_MODE_USR;
        cpu_stop_info_t info;
        enum cpu_stop st = cpu_run(a->c, 1, &info);
        if (st == CPU_STOP_LIMIT) {
            memcpy(r, cr->r, sizeof cr->r);
            *cpsr = (*cpsr & 0x0FFFFFFFu) | (cr->cpsr & 0xF0000000u);
            res = AEMU_OK;
            break;
        }
        if (st == CPU_STOP_DATA_ABORT) {
            /* no register was written; map the page and retry */
            int prot = info.fault_prot == GMEM_PROT_W ? GMEM_PROT_RW : GMEM_PROT_R;
            if (map_page(a, info.fault_addr & GP2X_PAGE_MASK, prot))
                continue;
            *fault = info.fault_addr;
            res = AEMU_SEGV;
            break;
        }
        res = AEMU_UNDEF; /* SVC, Thumb, a truly undefined instruction */
        break;
    }
    unmap_all(a);
    return res;
}
