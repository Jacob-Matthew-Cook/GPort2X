/* gmem unit tests: mapping, protection, aliasing, MMIO, free-area search,
 * page-crossing access, host spans and the resident-byte count. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gport2x/gmem.h"

static int failures;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            failures++;                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                           \
    } while (0)

/* An MMIO object that records the last access and serves a counter. */
typedef struct {
    uint32_t last_off, last_size, last_value, reads, writes, counter;
} mmio_log_t;

static uint32_t mmio_read(void *ctx, uint32_t off, unsigned size)
{
    mmio_log_t *l = ctx;
    l->reads++;
    l->last_off = off;
    l->last_size = size;
    if (off == 0x0A00 && size == 4) /* a TCOUNT-like counter (spec 4.3) */
        return l->counter += 7680;
    return 0x11223344u & (size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu);
}

static void mmio_write(void *ctx, uint32_t off, unsigned size, uint32_t value)
{
    mmio_log_t *l = ctx;
    l->writes++;
    l->last_off = off;
    l->last_size = size;
    l->last_value = value;
}

static void test_map_and_access(void)
{
    gmem_t *m = gmem_create();
    CHECK(m);
    CHECK(gmem_map_anon(m, 0x8000, 0x3000, GMEM_PROT_RWX) == GP_OK);
    CHECK(gmem_is_mapped(m, 0x8000, 0x3000, GMEM_PROT_RWX));
    CHECK(!gmem_is_mapped(m, 0x7000, 0x1000, GMEM_PROT_R));

    uint32_t v = 1;
    CHECK(gmem_ld32(m, 0x8000, &v) == GP_OK && v == 0); /* zero-filled */
    CHECK(gmem_st32(m, 0x8000, 0xDEADBEEFu) == GP_OK);
    CHECK(gmem_ld32(m, 0x8000, &v) == GP_OK && v == 0xDEADBEEFu);
    CHECK(gmem_ld8(m, 0x8001, &v) == GP_OK && v == 0xBE); /* little-endian */
    CHECK(gmem_ld16(m, 0x8002, &v) == GP_OK && v == 0xDEAD);
    CHECK(gmem_fetch32(m, 0x8000, &v) == GP_OK && v == 0xDEADBEEFu);

    /* Unmapped and protection faults, with the fault record. */
    CHECK(gmem_ld32(m, 0x7FFC, &v) == GP_ERR_FAULT);
    CHECK(gmem_last_fault(m)->addr == 0x7FFC && gmem_last_fault(m)->prot == GMEM_PROT_R);
    CHECK(gmem_protect(m, 0x9000, 0x1000, GMEM_PROT_R) == GP_OK);
    CHECK(gmem_st8(m, 0x9000, 1) == GP_ERR_FAULT);
    CHECK(gmem_last_fault(m)->addr == 0x9000 && gmem_last_fault(m)->prot == GMEM_PROT_W);
    CHECK(gmem_ld8(m, 0x9000, &v) == GP_OK);
    CHECK(gmem_fetch32(m, 0x9000, &v) == GP_ERR_FAULT); /* no X */
    CHECK(gmem_protect(m, 0x7000, 0x1000, GMEM_PROT_R) == GP_ERR_FAULT); /* not mapped */

    /* A store that crosses into a read-only page faults there. */
    CHECK(gmem_st32(m, 0x8FFE, 0xA1B2C3D4u) == GP_ERR_FAULT);
    CHECK(gmem_last_fault(m)->addr == 0x9000 && gmem_last_fault(m)->prot == GMEM_PROT_W);
    CHECK(gmem_protect(m, 0x9000, 0x1000, GMEM_PROT_RW) == GP_OK);
    /* A 32-bit access that crosses a page boundary is served bytewise. */
    CHECK(gmem_st32(m, 0x8FFE, 0xA1B2C3D4u) == GP_OK);
    CHECK(gmem_ld32(m, 0x8FFE, &v) == GP_OK && v == 0xA1B2C3D4u);
    CHECK(gmem_ld16(m, 0x9000, &v) == GP_OK && v == 0xA1B2);

    /* Bulk access across pages. */
    uint8_t buf[0x2000], back[0x2000];
    for (size_t i = 0; i < sizeof buf; i++)
        buf[i] = (uint8_t)(i * 7);
    CHECK(gmem_write(m, 0x8100, buf, sizeof buf) == GP_OK);
    CHECK(gmem_read(m, 0x8100, back, sizeof back) == GP_OK);
    CHECK(memcmp(buf, back, sizeof buf) == 0);
    CHECK(gmem_write(m, 0xA800, buf, 0x1000) == GP_ERR_FAULT); /* runs off the mapping */
    CHECK(gmem_memset(m, 0xA000, 0x5A, 0x1000) == GP_OK);
    CHECK(gmem_ld8(m, 0xAFFF, &v) == GP_OK && v == 0x5A);

    /* Partial unmap keeps the other pages. */
    CHECK(gmem_unmap(m, 0x9000, 0x1000) == GP_OK);
    CHECK(gmem_ld32(m, 0x9000, &v) == GP_ERR_FAULT);
    CHECK(gmem_ld32(m, 0x8000, &v) == GP_OK && v == 0xDEADBEEFu);
    CHECK(gmem_ld8(m, 0xA000, &v) == GP_OK && v == 0x5A);
    gmem_destroy(m);
}

static void test_alias_and_objects(void)
{
    gmem_t *m = gmem_create();
    gmem_obj_t *bank = gmem_obj_ram(0x4000);
    CHECK(bank && gmem_obj_size(bank) == 0x4000);
    /* Two views of the same object, like fb0 inside the upper bank (spec 1.5). */
    CHECK(gmem_map_obj(m, 0x40000000, 0x4000, GMEM_PROT_RW, bank, 0) == GP_OK);
    CHECK(gmem_map_obj(m, 0x40800000, 0x1000, GMEM_PROT_RW, bank, 0x2000) == GP_OK);
    CHECK(gmem_st32(m, 0x40002004, 0x12345678u) == GP_OK);
    uint32_t v = 0;
    CHECK(gmem_ld32(m, 0x40800004, &v) == GP_OK && v == 0x12345678u);
    CHECK(gmem_obj_host(bank)[0x2004] == 0x78);
    /* A host span inside one object is contiguous; across objects it is not. */
    CHECK(gmem_host_span(m, 0x40000100, 0x3F00, GMEM_PROT_RW) == gmem_obj_host(bank) + 0x100);
    CHECK(gmem_host_span(m, 0x40003000, 0x2000, GMEM_PROT_RW) == NULL); /* runs past the mapping */
    CHECK(gmem_map_anon(m, 0x40004000, 0x1000, GMEM_PROT_RW) == GP_OK);
    CHECK(gmem_host_span(m, 0x40003000, 0x2000, GMEM_PROT_RW) == NULL); /* different objects */
    CHECK(gmem_host_span(m, 0x40800000, 0x1000, GMEM_PROT_RWX) == NULL); /* no X */
    /* Bad arguments. */
    CHECK(gmem_map_obj(m, 0x40000000, 0x8000, GMEM_PROT_RW, bank, 0) == GP_ERR_INVAL); /* past the object */
    CHECK(gmem_map_obj(m, 0x40000100, 0x1000, GMEM_PROT_RW, bank, 0) == GP_ERR_INVAL); /* unaligned */
    CHECK(gmem_map_anon(m, 0xFFFFF000, 0x2000, GMEM_PROT_RW) == GP_ERR_INVAL);         /* wraps */
    /* The region view coalesces identical consecutive pages. */
    gmem_region_t r;
    CHECK(gmem_region_at(m, 0x40001234, &r) == GP_OK);
    CHECK(r.start == 0x40000000 && r.end == 0x40004000 && r.prot == GMEM_PROT_RW && r.obj == bank && !r.mmio);
    CHECK(gmem_region_at(m, 0x40004000, &r) == GP_OK && r.start == 0x40004000 && r.end == 0x40005000);
    CHECK(gmem_region_at(m, 0x40005000, &r) == GP_ERR_FAULT);
    /* Resident accounting follows object lifetime, not the mapping count. */
    size_t before = gmem_resident_bytes(m);
    gmem_obj_release(bank);                     /* creator's reference; still mapped */
    CHECK(gmem_resident_bytes(m) == before);
    CHECK(gmem_unmap(m, 0x40000000, 0x4000) == GP_OK);
    CHECK(gmem_resident_bytes(m) == before); /* the alias still holds it */
    CHECK(gmem_unmap(m, 0x40800000, 0x1000) == GP_OK);
    CHECK(gmem_resident_bytes(m) == before - 0x4000);
    gmem_destroy(m);
}

static void test_mmio(void)
{
    gmem_t *m = gmem_create();
    static const gmem_mmio_ops_t ops = {mmio_read, mmio_write};
    mmio_log_t log = {0};
    gmem_obj_t *regs = gmem_obj_mmio(GP2X_REGS_SIZE, &ops, &log);
    CHECK(regs && gmem_obj_host(regs) == NULL);
    /* The register file mapped where the game maps it (spec 3.2: offset 0xC0000000, 64 KB). */
    CHECK(gmem_map_obj(m, 0x40801000, GP2X_REGS_SIZE, GMEM_PROT_RW, regs, 0) == GP_OK);
    uint32_t v = 0;
    CHECK(gmem_ld32(m, 0x40801000 + 0x0A00, &v) == GP_OK && v == 7680);
    CHECK(gmem_ld32(m, 0x40801000 + 0x0A00, &v) == GP_OK && v == 15360);
    CHECK(log.last_off == 0x0A00 && log.last_size == 4);
    CHECK(gmem_ld16(m, 0x40801000 + 0x1198, &v) == GP_OK && v == 0x3344 && log.last_size == 2);
    CHECK(gmem_ld8(m, 0x40801000 + 0x1199, &v) == GP_OK && v == 0x44 && log.last_off == 0x1199);
    CHECK(gmem_st16(m, 0x40801000 + 0x2912, 0xABCD) == GP_OK);
    CHECK(log.writes == 1 && log.last_off == 0x2912 && log.last_size == 2 && log.last_value == 0xABCD);
    /* Bulk access to MMIO goes through the callbacks bytewise. */
    uint8_t b[3] = {1, 2, 3};
    CHECK(gmem_write(m, 0x40801000 + 0x2800, b, 3) == GP_OK && log.writes == 4);
    CHECK(gmem_host_span(m, 0x40801000, 0x100, GMEM_PROT_R) == NULL); /* MMIO has no host span */
    gmem_region_t r;
    CHECK(gmem_region_at(m, 0x40801000, &r) == GP_OK && r.mmio && r.end == 0x40811000);
    gmem_obj_release(regs);
    gmem_destroy(m);
}

static void test_find_free(void)
{
    gmem_t *m = gmem_create();
    CHECK(gmem_find_free(m, 0, 0x1000) == GP2X_PAGE_SIZE); /* never the NULL page */
    CHECK(gmem_find_free(m, GUEST_MMAP_BASE, 0x10000) == GUEST_MMAP_BASE);
    CHECK(gmem_map_anon(m, GUEST_MMAP_BASE, 0x10000, GMEM_PROT_RW) == GP_OK);
    CHECK(gmem_find_free(m, GUEST_MMAP_BASE, 0x1000) == GUEST_MMAP_BASE + 0x10000); /* bottom-up */
    CHECK(gmem_map_anon(m, GUEST_MMAP_BASE + 0x11000, 0x1000, GMEM_PROT_RW) == GP_OK);
    CHECK(gmem_find_free(m, GUEST_MMAP_BASE, 0x1000) == GUEST_MMAP_BASE + 0x10000);  /* fits the hole */
    CHECK(gmem_find_free(m, GUEST_MMAP_BASE, 0x2000) == GUEST_MMAP_BASE + 0x12000);  /* skips the hole */
    CHECK(gmem_find_free(m, GUEST_MMAP_BASE + 0x123, 0x800) == GUEST_MMAP_BASE + 0x10000); /* hint and length are page-aligned up; one page fits the hole */
    CHECK(gmem_find_free(m, GUEST_TASK_SIZE - 0x1000, 0x2000) == 0); /* nothing above the task size */
    CHECK(gmem_find_free(m, 0xBFFFF000, 0x1000) == 0xBFFFF000);
    gmem_destroy(m);
}

/* The CPU's page cache support: host pointers for RAM pages with the asked
 * protection only, and a generation stamp that moves on every mapping change. */
static void test_page_host_and_generation(void)
{
    gmem_t *m = gmem_create();
    uint64_t g0 = gmem_generation(m);
    CHECK(g0 != 0);
    CHECK(gmem_map_anon(m, 0x8000, 0x2000, GMEM_PROT_RW) == GP_OK);
    uint64_t g1 = gmem_generation(m);
    CHECK(g1 != g0);
    uint8_t *h = gmem_page_host(m, 0x8010, GMEM_PROT_R); /* the page's host memory, whatever the offset */
    CHECK(h != NULL && gmem_page_host(m, 0x8FFC, GMEM_PROT_W) == h);
    CHECK(gmem_page_host(m, 0x9000, GMEM_PROT_R) == h + 0x1000);  /* same object, next page */
    CHECK(gmem_page_host(m, 0x8000, GMEM_PROT_X) == NULL);         /* not executable */
    CHECK(gmem_page_host(m, 0x7000, GMEM_PROT_R) == NULL);         /* unmapped */
    CHECK(gmem_st32(m, 0x8010, 0x11223344u) == GP_OK && memcmp(h + 0x10, "\x44\x33\x22\x11", 4) == 0);
    CHECK(gmem_protect(m, 0x8000, 0x1000, GMEM_PROT_R | GMEM_PROT_X) == GP_OK);
    CHECK(gmem_generation(m) != g1);
    CHECK(gmem_page_host(m, 0x8000, GMEM_PROT_X) == h && gmem_page_host(m, 0x8000, GMEM_PROT_W) == NULL);
    uint64_t g2 = gmem_generation(m);
    CHECK(gmem_unmap(m, 0x9000, 0x1000) == GP_OK && gmem_generation(m) != g2);
    CHECK(gmem_page_host(m, 0x9000, GMEM_PROT_R) == NULL);
    /* MMIO pages never have host memory. */
    static const gmem_mmio_ops_t ops = {mmio_read, mmio_write};
    mmio_log_t log = {0};
    gmem_obj_t *regs = gmem_obj_mmio(GP2X_REGS_SIZE, &ops, &log);
    CHECK(gmem_map_obj(m, 0x40801000, GP2X_REGS_SIZE, GMEM_PROT_RW, regs, 0) == GP_OK);
    CHECK(gmem_page_host(m, 0x40801A00, GMEM_PROT_R) == NULL);
    /* A second space has its own, different stamp (one global counter). */
    gmem_t *m2 = gmem_create();
    CHECK(gmem_generation(m2) != gmem_generation(m));
    gmem_destroy(m2);
    gmem_obj_release(regs);
    gmem_destroy(m);
}

static int hook_calls;
static const uint8_t *hook_page;
static void code_hook_fn(const uint8_t *host_page) { hook_calls++; hook_page = host_page; }

/* Translated-code marks (the JIT's invalidation, docs/JIT.md). */
static void test_code_marks(void)
{
    gmem_set_code_hook(code_hook_fn);
    gmem_t *m = gmem_create();
    CHECK(gmem_map_anon(m, 0x8000, 0x2000, GMEM_PROT_RWX) == GP_OK);
    uint8_t *h = gmem_page_host(m, 0x8000, GMEM_PROT_R);
    uint64_t ep = gmem_code_epoch();
    CHECK(gmem_mark_code(m, 0x8100, 16) == 2 && gmem_code_epoch() != ep);  /* words 0x8100..0x810F */
    CHECK(gmem_mark_code(m, 0x8200, 4) == 1);                              /* same page, already marked */
    CHECK(gmem_page_host(m, 0x8000, GMEM_PROT_W) == NULL);                 /* no write pointer for a code page */
    CHECK(gmem_page_host(m, 0x8000, GMEM_PROT_R) == h);
    CHECK(gmem_page_host(m, 0x9000, GMEM_PROT_W) != NULL);                 /* the next page is unaffected */
    hook_calls = 0;
    CHECK(gmem_st32(m, 0x8110, 1) == GP_OK && hook_calls == 0);            /* a data word: no invalidation */
    CHECK(gmem_st8(m, 0x80FF, 1) == GP_OK && hook_calls == 0);
    CHECK(gmem_st16(m, 0x810E, 7) == GP_OK && hook_calls == 1 && hook_page == h); /* touches a code word */
    CHECK(gmem_page_host(m, 0x8000, GMEM_PROT_W) == h);                    /* unmarked: writable again */
    CHECK(gmem_st32(m, 0x8100, 2) == GP_OK && hook_calls == 1);
    /* bulk writes and writable host spans check too */
    CHECK(gmem_mark_code(m, 0x8100, 4) == 2);
    uint8_t buf[8] = {0};
    CHECK(gmem_write(m, 0x80FC, buf, 8) == GP_OK && hook_calls == 2);
    CHECK(gmem_mark_code(m, 0x8100, 4) == 2);
    CHECK(gmem_host_span(m, 0x8000, 0x200, GMEM_PROT_W) == h && hook_calls == 3);
    /* rewritten too often: left to the interpreter */
    int refused = 0;
    for (int i = 0; i < 20 && !refused; i++) {
        if (gmem_mark_code(m, 0x8100, 4) == 0)
            refused = 1;
        else
            gmem_st32(m, 0x8100, (uint32_t)i);
    }
    CHECK(refused);
    /* releasing the object reports its marked pages */
    CHECK(gmem_mark_code(m, 0x9000, 4) == 2);
    int before = hook_calls;
    gmem_destroy(m);
    CHECK(hook_calls == before + 1);
    gmem_set_code_hook(NULL);
}

int main(void)
{
    test_map_and_access();
    test_page_host_and_generation();
    test_code_marks();
    test_alias_and_objects();
    test_mmio();
    test_find_free();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("gmem: ok");
    return 0;
}
