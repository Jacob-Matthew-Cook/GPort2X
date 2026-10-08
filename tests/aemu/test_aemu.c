/* Tests of the native engine's one-instruction emulator (include/gport2x/aemu.h)
 * over a host buffer standing in for the process's memory. */
#include <stdio.h>
#include <string.h>

#include "gport2x/aemu.h"
#include "gport2x/cpu.h"
#include "gport2x/gmem.h"

static int failures;
#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            failures++;                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
        }                                                                       \
    } while (0)

#define CODE 0x10000u
#define DATA 0x20000u
static _Alignas(4096) uint8_t mem[2][4096]; /* [0] = code page, [1] = data page */
static int page_calls;

static uint8_t *page_host(void *ctx, gaddr_t page, int prot)
{
    (void)ctx;
    page_calls++;
    if (page == CODE)
        return (prot & GMEM_PROT_W) ? NULL : mem[0];
    if (page == DATA)
        return mem[1];
    return NULL; /* everything else is unmapped */
}

static void put(uint32_t off, uint32_t insn) { memcpy(mem[0] + off, &insn, 4); }
static void put_data(uint32_t off, uint32_t v) { memcpy(mem[1] + off, &v, 4); }
static uint32_t data(uint32_t off) { uint32_t v; memcpy(&v, mem[1] + off, 4); return v; }

int main(void)
{
    cpu_set_jit(false);
    aemu_t *a = aemu_create(page_host, NULL);
    CHECK(a != NULL);
    uint32_t r[16] = {0}, cpsr = 0;
    gaddr_t fault;

    /* SWP r0, r1, [r2] on a lock word: atomic exchange, pc advanced */
    put(0, 0xE1020091u); /* swp r0, r1, [r2] */
    put_data(0x40, 0x11223344u);
    r[1] = 1; r[2] = DATA + 0x40; r[15] = CODE;
    CHECK(aemu_step(a, r, &cpsr, &fault) == AEMU_OK && r[0] == 0x11223344u && data(0x40) == 1 && r[15] == CODE + 4);
    /* SWPB */
    put(4, 0xE1423091u); /* swpb r3, r1, [r2] */
    put_data(0x44, 0xAABBCCDDu);
    r[2] = DATA + 0x45; r[1] = 0x77;
    CHECK(aemu_step(a, r, &cpsr, &fault) == AEMU_OK && r[3] == 0xCC && data(0x44) == 0xAABB77DDu && r[15] == CODE + 8);
    /* SWP at an unaligned address: rotated load, aligned store (ARMv4, as the interpreter) */
    put(8, 0xE1020091u);
    put_data(0x48, 0x11223344u);
    r[2] = DATA + 0x49; r[1] = 5;
    CHECK(aemu_step(a, r, &cpsr, &fault) == AEMU_OK && r[0] == 0x44112233u && data(0x48) == 5);
    /* SWP to an unmapped address: SIGSEGV at the address, registers unchanged */
    put(12, 0xE1020091u);
    r[0] = 0x5555; r[2] = 0x900000; r[15] = CODE + 12;
    CHECK(aemu_step(a, r, &cpsr, &fault) == AEMU_SEGV && fault == 0x900000 && r[0] == 0x5555 && r[15] == CODE + 12);
    /* a failed condition skips the instruction (an UNDEFINED encoding may trap anyway on ARMv8) */
    put(16, 0x01020091u); /* swpeq r0, r1, [r2] */
    cpsr = 0; r[15] = CODE + 16; r[2] = DATA + 0x40;
    CHECK(aemu_step(a, r, &cpsr, &fault) == AEMU_OK && r[0] == 0x5555 && r[15] == CODE + 20);

    /* FPA: the register file persists across steps (one aemu_t per thread) */
    put(20, 0xEE001190u); /* flt.d f0, r1 */
    put(24, 0xED820100u); /* stf.s f0, [r2] */
    r[15] = CODE + 20; r[2] = DATA + 0x80; r[1] = 1;
    CHECK(aemu_step(a, r, &cpsr, &fault) == AEMU_OK && r[15] == CODE + 24);
    CHECK(aemu_step(a, r, &cpsr, &fault) == AEMU_OK && r[15] == CODE + 28 && data(0x80) == 0x3F800000u);
    /* SFM/LFM in a prologue: sfm f4, 4, [sp, #-48]! then lfm f4, 4, [sp], #48 */
    put(28, 0xED2D420Cu); /* sfm f4, 4, [sp, #-48]! */
    put(32, 0xECBD420Cu); /* lfm f4, 4, [sp], #48 */
    r[13] = DATA + 0x400; r[15] = CODE + 28;
    CHECK(aemu_step(a, r, &cpsr, &fault) == AEMU_OK && r[13] == DATA + 0x400 - 48);
    CHECK(aemu_step(a, r, &cpsr, &fault) == AEMU_OK && r[13] == DATA + 0x400 && r[15] == CODE + 36);

    /* LDM with an unaligned base (SIGBUS on ARMv8): ARMv4 ignores bits [1:0] */
    put(36, 0xE8920003u); /* ldmia r2, {r0, r1} */
    put_data(0x100, 0xCAFE0001u);
    put_data(0x104, 0xCAFE0002u);
    r[2] = DATA + 0x102; r[15] = CODE + 36;
    CHECK(aemu_step(a, r, &cpsr, &fault) == AEMU_OK && r[0] == 0xCAFE0001u && r[1] == 0xCAFE0002u && r[15] == CODE + 40);
    /* the flags written by an emulated instruction come back; others are kept */
    put(40, 0xE3500000u); /* cmp r0, #0 */
    r[0] = 0; r[15] = CODE + 40; cpsr = 0x10000010u; /* V set, user mode bits */
    CHECK(aemu_step(a, r, &cpsr, &fault) == AEMU_OK && cpsr == (0x60000000u | 0x10u)); /* Z, C; mode kept */
    /* an access to an unmapped page faults with the exact address */
    put(44, 0xE5910000u); /* ldr r0, [r1] */
    r[1] = 0x31234; r[15] = CODE + 44; r[0] = 9;
    CHECK(aemu_step(a, r, &cpsr, &fault) == AEMU_SEGV && fault == 0x31234 && r[0] == 9);
    /* SVC is not the emulator's business */
    put(48, 0xEF900001u);
    r[15] = CODE + 48;
    CHECK(aemu_step(a, r, &cpsr, &fault) == AEMU_UNDEF && r[15] == CODE + 48);
    /* code on an unmapped page */
    r[15] = 0x50000;
    CHECK(aemu_step(a, r, &cpsr, &fault) == AEMU_SEGV && fault == 0x50000);

    aemu_destroy(a);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("aemu: ok");
    return 0;
}
