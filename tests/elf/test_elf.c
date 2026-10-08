/* ELF loader tests: a synthetic ELF32 ARM image built in memory, the execve
 * probe (ELF / "#!" / neither), ET_DYN relocation by base, and the initial
 * stack layout. With GPORT2X_GAME_ELF set, the user's own game image is loaded
 * and checked against the spec's layout facts (spec 1.4); otherwise that part
 * is skipped. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gport2x/elf.h"

static int failures;
#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) {                                                          \
            failures++;                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
        }                                                                       \
    } while (0)

static void w16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* Builds a two-segment ARM ELF: text (R+X) with the headers inside it, and
 * data (R+W) whose .bss extends past its file bytes. type is ET_EXEC or ET_DYN. */
static size_t build_elf(uint8_t *buf, uint16_t type, uint32_t text_vaddr, uint32_t data_vaddr, uint32_t eflags)
{
    memset(buf, 0, 0x3000);
    memcpy(buf, "\x7f" "ELF", 4);
    buf[4] = 1; /* ELFCLASS32 */
    buf[5] = 1; /* ELFDATA2LSB */
    buf[6] = 1;
    w16(buf + 16, type);
    w16(buf + 18, 40); /* EM_ARM */
    w32(buf + 20, 1);
    w32(buf + 24, text_vaddr + 0x100); /* e_entry */
    w32(buf + 28, 52);                 /* e_phoff */
    w32(buf + 36, eflags);
    w16(buf + 40, 52);
    w16(buf + 42, 32); /* e_phentsize */
    w16(buf + 44, 2);  /* e_phnum */
    uint8_t *ph = buf + 52;
    /* PT_LOAD text: file [0, 0x200) at text_vaddr, R+X */
    w32(ph + 0, 1);
    w32(ph + 4, 0);
    w32(ph + 8, text_vaddr);
    w32(ph + 12, text_vaddr);
    w32(ph + 16, 0x200);
    w32(ph + 20, 0x200);
    w32(ph + 24, 5);
    w32(ph + 28, 0x1000);
    ph += 32;
    /* PT_LOAD data: file [0x1200, 0x1210) at data_vaddr, memsz 0x1000 (bss follows), R+W */
    w32(ph + 0, 1);
    w32(ph + 4, 0x1200);
    w32(ph + 8, data_vaddr);
    w32(ph + 12, data_vaddr);
    w32(ph + 16, 0x10);
    w32(ph + 20, 0x1000);
    w32(ph + 24, 6);
    w32(ph + 28, 0x1000);
    /* Recognisable contents. */
    for (uint32_t i = 0x100; i < 0x200; i++)
        buf[i] = (uint8_t)i;
    for (uint32_t i = 0; i < 0x10; i++)
        buf[0x1200 + i] = (uint8_t)(0xA0 + i);
    return 0x1300;
}

static void test_probe(void)
{
    uint8_t buf[0x3000];
    elf_probe_t p;
    size_t n = build_elf(buf, 2, 0x8000, 0x9200, 0);
    CHECK(elf_probe(buf, n, &p) == GP_OK && p.kind == ELF_KIND_ELF && p.oabi && !p.is_dynamic);
    n = build_elf(buf, 3, 0, 0x1200, 0x05000000); /* EABI5 flags */
    CHECK(elf_probe(buf, n, &p) == GP_OK && p.kind == ELF_KIND_ELF && !p.oabi && p.is_dynamic);
    const char *script = "#!/bin/sh -e\necho hi\n";
    CHECK(elf_probe((const uint8_t *)script, strlen(script), &p) == GP_OK && p.kind == ELF_KIND_SCRIPT);
    CHECK(strcmp(p.interp, "/bin/sh") == 0 && strcmp(p.interp_arg, "-e") == 0);
    const char *nobang = "#bin/sh\n"; /* spec 7.3: no '!' means ENOEXEC, which the launch chain relies on */
    CHECK(elf_probe((const uint8_t *)nobang, strlen(nobang), &p) == GP_OK && p.kind == ELF_KIND_NONE);
    const char *junk = "\x7f" "ELX";
    CHECK(elf_probe((const uint8_t *)junk, 4, &p) == GP_OK && p.kind == ELF_KIND_NONE);
    buf[18] = 3; /* EM_386 */
    n = build_elf(buf, 2, 0x8000, 0x9200, 0);
    buf[18] = 3;
    CHECK(elf_probe(buf, n, &p) == GP_OK && p.kind == ELF_KIND_NONE);
}

static void test_load_exec(void)
{
    uint8_t buf[0x3000];
    size_t n = build_elf(buf, 2, 0x8000, 0x9200, 0);
    gmem_t *m = gmem_create();
    elf_image_t img;
    CHECK(elf_load(m, buf, n, 0, &img) == GP_OK);
    CHECK(img.entry == 0x8100 && img.oabi && !img.is_dynamic && !img.has_interp);
    CHECK(img.load_base == 0x8000 && img.load_end == 0xB000); /* data 0x9200+0x1000 -> page 0xB000 */
    CHECK(img.phdr_addr == 0x8034 && img.phent == 32 && img.phnum == 2);
    /* Protections per p_flags. */
    CHECK(gmem_is_mapped(m, 0x8000, 0x1000, GMEM_PROT_R | GMEM_PROT_X));
    CHECK(!gmem_is_mapped(m, 0x8000, 0x1000, GMEM_PROT_W));
    CHECK(gmem_is_mapped(m, 0x9000, 0x2000, GMEM_PROT_RW));
    CHECK(!gmem_is_mapped(m, 0x9000, 0x1000, GMEM_PROT_X));
    CHECK(!gmem_is_mapped(m, 0xB000, 0x1000, GMEM_PROT_R));
    /* Contents: text bytes, data bytes, zeroed bss. */
    uint32_t v;
    CHECK(gmem_ld8(m, 0x8100, &v) == GP_OK && v == 0x00);
    CHECK(gmem_ld8(m, 0x81FF, &v) == GP_OK && v == 0xFF);
    CHECK(gmem_fetch32(m, 0x8000, &v) == GP_OK && v == 0x464C457Fu); /* "\x7fELF" is in the text segment */
    CHECK(gmem_ld8(m, 0x9200, &v) == GP_OK && v == 0xA0);
    CHECK(gmem_ld8(m, 0x920F, &v) == GP_OK && v == 0xAF);
    CHECK(gmem_ld8(m, 0x9210, &v) == GP_OK && v == 0);
    CHECK(gmem_ld32(m, 0xA1FC, &v) == GP_OK && v == 0);
    gmem_destroy(m);
}

static void test_load_dyn(void)
{
    uint8_t buf[0x3000];
    size_t n = build_elf(buf, 3, 0, 0x1200, 0);
    gmem_t *m = gmem_create();
    elf_image_t img;
    CHECK(elf_load(m, buf, n, 0x40000000, &img) == GP_OK);
    CHECK(img.is_dynamic && img.entry == 0x40000100 && img.load_base == 0x40000000 && img.load_end == 0x40003000);
    CHECK(img.phdr_addr == 0x40000034);
    uint32_t v;
    CHECK(gmem_ld8(m, 0x40001200, &v) == GP_OK && v == 0xA0);
    /* Malformed: a segment past the end of the file. */
    uint8_t *ph = buf + 52 + 32;
    ph[16] = 0xFF; ph[17] = 0xFF; /* filesz huge */
    gmem_t *m2 = gmem_create();
    CHECK(elf_load(m2, buf, n, 0x40000000, &img) == GP_ERR_FORMAT);
    gmem_destroy(m2);
    gmem_destroy(m);
}

static uint32_t rd32g(gmem_t *m, gaddr_t a)
{
    uint32_t v = 0xDEADDEAD;
    gmem_ld32(m, a, &v);
    return v;
}

static int gstrcmp(gmem_t *m, gaddr_t a, const char *s)
{
    char buf[64] = {0};
    gmem_read(m, a, buf, (uint32_t)strlen(s) + 1);
    return strcmp(buf, s);
}

static void test_stack(void)
{
    uint8_t buf[0x3000];
    size_t n = build_elf(buf, 2, 0x8000, 0x9200, 0);
    gmem_t *m = gmem_create();
    elf_image_t img;
    CHECK(elf_load(m, buf, n, 0, &img) == GP_OK);
    const char *argv[] = {"/mnt/sd/Payback/Payback", "arg1", NULL};
    const char *envp[] = {"HOME=/root", "PATH=/bin", NULL};
    gaddr_t sp = 0;
    CHECK(elf_setup_stack(m, &img, NULL, argv[0], 2, argv, envp, elf_default_auxinfo(), GUEST_STACK_TOP, 0x800000, &sp) == GP_OK);
    CHECK(sp != 0 && (sp & 7) == 0 && sp < GUEST_STACK_TOP && sp > GUEST_STACK_TOP - 0x1000);
    CHECK(gmem_is_mapped(m, GUEST_STACK_TOP - 0x800000, 0x800000, GMEM_PROT_RW));
    CHECK(rd32g(m, sp) == 2); /* argc */
    gaddr_t a0 = rd32g(m, sp + 4), a1 = rd32g(m, sp + 8);
    CHECK(gstrcmp(m, a0, argv[0]) == 0 && gstrcmp(m, a1, "arg1") == 0 && a0 > sp && a1 > a0);
    CHECK(rd32g(m, sp + 12) == 0); /* argv terminator */
    gaddr_t e0 = rd32g(m, sp + 16), e1 = rd32g(m, sp + 20);
    CHECK(gstrcmp(m, e0, "HOME=/root") == 0 && gstrcmp(m, e1, "PATH=/bin") == 0);
    CHECK(rd32g(m, sp + 24) == 0); /* envp terminator */
    /* auxv: type/value pairs until AT_NULL, with the entries the game's libc needs. */
    gaddr_t aux = sp + 28;
    uint32_t seen_entry = 0, seen_phdr = 0, seen_pagesz = 0, seen_platform = 0, seen_null = 0, seen_hwcap = 0;
    for (int i = 0; i < 20; i++) {
        uint32_t t = rd32g(m, aux + (uint32_t)i * 8), val = rd32g(m, aux + (uint32_t)i * 8 + 4);
        if (t == 9) seen_entry = val;
        if (t == 3) seen_phdr = val;
        if (t == 6) seen_pagesz = val;
        if (t == 15) seen_platform = val;
        if (t == 16) seen_hwcap = val;
        if (t == 0) { seen_null = 1; break; }
    }
    CHECK(seen_entry == 0x8100 && seen_phdr == 0x8034 && seen_pagesz == 4096 && seen_null);
    CHECK(rd32g(m, aux + 14 * 8) == 0 && rd32g(m, aux + 14 * 8 + 4) == 0); /* AT_NULL is the 15th pair */
    CHECK(rd32g(m, aux + 13 * 8) == 15);                                       /* ...after AT_PLATFORM */
    CHECK(seen_hwcap == 0x17); /* swp, half, thumb, fast_mult: an ARM920T */
    CHECK(seen_platform && gstrcmp(m, seen_platform, "v4l") == 0);
    /* Bad arguments are refused. */
    CHECK(elf_setup_stack(m, &img, NULL, argv[0], 2, argv, envp, elf_default_auxinfo(), GUEST_STACK_TOP + 0x100, 0x1000, &sp) == GP_ERR_INVAL);
    gmem_destroy(m);
}

static int test_real_game(void)
{
    const char *path = getenv("GPORT2X_GAME_ELF");
    if (!path || !*path)
        return 77;
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)len);
    CHECK(buf && fread(buf, 1, (size_t)len, f) == (size_t)len);
    fclose(f);
    elf_probe_t p;
    CHECK(elf_probe(buf, (size_t)len, &p) == GP_OK && p.kind == ELF_KIND_ELF && p.oabi && !p.is_dynamic);
    gmem_t *m = gmem_create();
    elf_image_t img;
    CHECK(elf_load(m, buf, (size_t)len, 0, &img) == GP_OK);
    /* Spec 1.4: the first PT_LOAD starts at 0x8000, .bss ends at 0xF23440, brk starts at 0xF24000. */
    CHECK(img.load_base == 0x8000);
    CHECK(img.load_end == 0x00F24000);
    CHECK(gmem_is_mapped(m, 0x8000, 0x1000, GMEM_PROT_R | GMEM_PROT_X));
    CHECK(gmem_is_mapped(m, 0x9F1000, 0x1000, GMEM_PROT_RW));
    CHECK(!gmem_is_mapped(m, 0x05000000, 0x1000, GMEM_PROT_R)); /* stays unmapped on purpose */
    uint32_t v;
    CHECK(gmem_fetch32(m, img.entry, &v) == GP_OK);
    printf("real game: entry=%08x load=[%08x,%08x) phdr=%08x oabi=%d\n", img.entry, img.load_base, img.load_end, img.phdr_addr, img.oabi);
    gmem_destroy(m);
    free(buf);
    return 0;
}

int main(void)
{
    test_probe();
    test_load_exec();
    test_load_dyn();
    test_stack();
    int real = test_real_game();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("elf: ok");
    if (real == 77)
        puts("real-game check skipped: GPORT2X_GAME_ELF unset");
    return 0;
}
