/* The MMSP2's second core, the ARM940T, as programs drive it (a sound or
 * decoder loop in shared memory): DUALCTRL940 (0xC0003B48) bit 7 holds it in
 * reset and bits 0-1 choose the 16 MB bank it sees at address 0; a program
 * copies its code into the bank and releases the reset, and the core starts
 * at address 0. Modelled with the interpreter on an address space of its
 * own: the bank (only banks 2 and 3, the upper RAM, exist in the model) at 0
 * and the register file at 0xC0000000. CP15 (the protection unit and cache
 * set-up) is recorded and has no other effect; interrupts between the cores
 * are not modelled. */
#include <stdlib.h>
#include <string.h>

#include "gport2x/dual940.h"
#include "gport2x/cpu.h"
#include "gport2x/gmem.h"
#include "gport2x/log.h"

#define REG_DUALCTRL940 0x3B48
#define BANK_BYTES 0x01000000u

struct dual940 {
    gpdev_t *dev;
    gmem_t *mem;
    cpu_t *cpu;
    unsigned bank; /* the bank of the running core, 0 when stopped */
    uint32_t cp15[16];
    bool faulted;
};

dual940_t *dual940_create(gpdev_t *d)
{
    dual940_t *c = calloc(1, sizeof *c);
    if (c)
        c->dev = d;
    return c;
}

static void stop_core(dual940_t *c)
{
    if (c->cpu)
        cpu_destroy(c->cpu);
    if (c->mem)
        gmem_destroy(c->mem);
    c->cpu = NULL;
    c->mem = NULL;
    c->bank = 0;
}

void dual940_destroy(dual940_t *c)
{
    if (!c)
        return;
    stop_core(c);
    free(c);
}

static bool start_core(dual940_t *c, unsigned bank)
{
    c->mem = gmem_create_mode(false);
    if (!c->mem)
        return false;
    uint32_t off = bank * BANK_BYTES - GP2X_UPPER_BANK_PHYS;
    int prot = GMEM_PROT_R | GMEM_PROT_W | GMEM_PROT_X;
    if (gmem_map_obj(c->mem, 0, BANK_BYTES, prot, gpdev_upper_bank(c->dev), off) != GP_OK ||
        gmem_map_obj(c->mem, GP2X_REGS_PHYS, GP2X_REGS_SIZE, GMEM_PROT_R | GMEM_PROT_W, gpdev_regs(c->dev), 0) != GP_OK) {
        stop_core(c);
        return false;
    }
    c->cpu = cpu_create(c->mem);
    if (!c->cpu) {
        stop_core(c);
        return false;
    }
    cpu_regs_t *r = cpu_regs(c->cpu);
    memset(r, 0, sizeof *r);
    r->cpsr = CPSR_MODE_USR;
    c->bank = bank;
    c->faulted = false;
    gp_info("940: started on bank %u (physical %08x)", bank, bank * BANK_BYTES);
    return true;
}

/* MCR/MRC p15: record, read back. Returns false for any other instruction. */
static bool cp15(dual940_t *c, uint32_t insn)
{
    if ((insn & 0x0F000F10u) != 0x0E000F10u)
        return false;
    unsigned crn = (insn >> 16) & 15, rd = (insn >> 12) & 15;
    cpu_regs_t *r = cpu_regs(c->cpu);
    if (insn & (1u << 20)) {
        if (rd != 15)
            r->r[rd] = c->cp15[crn];
    } else {
        c->cp15[crn] = r->r[rd];
    }
    r->r[15] += 4;
    return true;
}

bool dual940_step(dual940_t *c, uint64_t budget)
{
    uint16_t ctl = gpdev_reg_peek16(c->dev, REG_DUALCTRL940);
    unsigned bank = ctl & 3;
    bool run = !(ctl & 0x80) && (bank == 2 || bank == 3);
    if (!run) {
        if (c->bank)
            gp_info("940: stopped");
        stop_core(c);
        return false;
    }
    if (c->bank != bank) {
        stop_core(c);
        if (!start_core(c, bank))
            return false;
    }
    if (c->faulted)
        return false;
    uint64_t done = 0;
    while (done < budget) {
        uint64_t before = cpu_insn_count(c->cpu);
        cpu_stop_info_t info;
        enum cpu_stop why = cpu_run(c->cpu, budget - done, &info);
        done += cpu_insn_count(c->cpu) - before;
        if (why == CPU_STOP_LIMIT || why == CPU_STOP_NONE)
            break;
        if (why == CPU_STOP_UNDEF && cp15(c, info.insn))
            continue;
        gp_warn("940: stopped by event %d at pc %08x (insn %08x, address %08x); the core halts", (int)why, info.pc,
                info.insn, info.fault_addr);
        c->faulted = true;
        break;
    }
    return true;
}
