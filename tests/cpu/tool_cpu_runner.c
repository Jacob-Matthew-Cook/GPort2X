/* Differential-test helper: runs one case file through the interpreter and
 * prints the final state. Used by test_difftest.py, which runs the same case
 * under Unicorn and compares.
 *
 * Case file (all little-endian u32 unless noted):
 *   magic 0x47504331 ("GPC1")
 *   r0..r15, cpsr
 *   code_addr, code_len, code bytes
 *   data_addr, data_len, data bytes
 *   max_insns
 * Output: "stop N pc X", "regs r0..r15", "cpsr X", "insns N", "data hex". */
#include <stdio.h>
#include <stdlib.h>

#include "gport2x/cpu.h"

static uint32_t rd32(FILE *f)
{
    unsigned char b[4];
    if (fread(b, 1, 4, f) != 4)
        exit(2);
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f)
        return 2;
    if (rd32(f) != 0x47504331u)
        return 2;
    cpu_regs_t init;
    for (int i = 0; i < 16; i++)
        init.r[i] = rd32(f);
    init.cpsr = rd32(f);
    uint32_t code_addr = rd32(f), code_len = rd32(f);
    unsigned char *code = malloc(code_len ? code_len : 1);
    if (fread(code, 1, code_len, f) != code_len)
        return 2;
    uint32_t data_addr = rd32(f), data_len = rd32(f);
    unsigned char *data = malloc(data_len ? data_len : 1);
    if (fread(data, 1, data_len, f) != data_len)
        return 2;
    uint32_t max_insns = rd32(f);
    fclose(f);

    gmem_t *m = gmem_create();
    gmem_map_anon(m, code_addr & GP2X_PAGE_MASK, GP2X_PAGE_ALIGN_UP(code_len + (code_addr & 0xFFF)), GMEM_PROT_RWX);
    gmem_map_anon(m, data_addr & GP2X_PAGE_MASK, GP2X_PAGE_ALIGN_UP(data_len + (data_addr & 0xFFF)), GMEM_PROT_RW);
    gmem_write(m, code_addr, code, code_len);
    gmem_write(m, data_addr, data, data_len);
    cpu_t *c = cpu_create(m);
    *cpu_regs(c) = init;
    cpu_stop_info_t info;
    enum cpu_stop st = cpu_run(c, max_insns, &info);
    const cpu_regs_t *r = cpu_regs(c);
    printf("stop %d pc %08x\n", (int)st, info.pc);
    printf("regs");
    for (int i = 0; i < 16; i++)
        printf(" %08x", r->r[i]);
    printf("\ncpsr %08x\ninsns %llu\ndata ", r->cpsr, (unsigned long long)cpu_insn_count(c));
    gmem_read(m, data_addr, data, data_len);
    for (uint32_t i = 0; i < data_len; i++)
        printf("%02x", data[i]);
    printf("\n");
    return 0;
}
