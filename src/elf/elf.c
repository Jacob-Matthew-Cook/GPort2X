/* GPort2X ELF32 ARM loader: see include/gport2x/elf.h.
 * Field layouts follow the ELF specification (System V ABI, chapter 4). */
#include "gport2x/elf.h"

#include <stdlib.h>
#include <string.h>

#include "gport2x/log.h"

#define ELFCLASS32 1
#define ELFDATA2LSB 1
#define ET_EXEC 2
#define ET_DYN 3
#define EM_ARM 40
#define PT_LOAD 1
#define PT_INTERP 3
#define PF_X 1
#define PF_W 2
#define PF_R 4
#define EF_ARM_EABI_MASK 0xFF000000u

/* Auxiliary vector types (Linux uapi/linux/auxvec.h). */
#define AT_NULL 0
#define AT_PHDR 3
#define AT_PHENT 4
#define AT_PHNUM 5
#define AT_PAGESZ 6
#define AT_BASE 7
#define AT_FLAGS 8
#define AT_ENTRY 9
#define AT_UID 11
#define AT_EUID 12
#define AT_GID 13
#define AT_EGID 14
#define AT_PLATFORM 15
#define AT_HWCAP 16
#define AT_CLKTCK 17

/* ARM HWCAP bits (arch/arm uapi/asm/hwcap.h): what an ARM920T has. */
#define HWCAP_SWP 1
#define HWCAP_HALF 2
#define HWCAP_THUMB 4
#define HWCAP_FAST_MULT 16

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

typedef struct ehdr {
    uint16_t type, machine;
    uint32_t entry, phoff, flags;
    uint16_t phentsize, phnum;
} ehdr_t;

typedef struct phdr {
    uint32_t type, offset, vaddr, filesz, memsz, flags;
} phdr_t;

static int parse_ehdr(const uint8_t *f, size_t len, ehdr_t *e)
{
    if (len < 52 || memcmp(f, "\x7f" "ELF", 4) != 0)
        return GP_ERR_FORMAT;
    if (f[4] != ELFCLASS32 || f[5] != ELFDATA2LSB)
        return GP_ERR_FORMAT;
    e->type = rd16(f + 16);
    e->machine = rd16(f + 18);
    e->entry = rd32(f + 24);
    e->phoff = rd32(f + 28);
    e->flags = rd32(f + 36);
    e->phentsize = rd16(f + 42);
    e->phnum = rd16(f + 44);
    if (e->machine != EM_ARM || (e->type != ET_EXEC && e->type != ET_DYN))
        return GP_ERR_FORMAT;
    if (e->phentsize != 32 || e->phnum == 0 || (uint64_t)e->phoff + (uint64_t)e->phnum * 32 > len)
        return GP_ERR_FORMAT;
    return GP_OK;
}

static void parse_phdr(const uint8_t *f, const ehdr_t *e, unsigned i, phdr_t *p)
{
    const uint8_t *h = f + e->phoff + i * 32;
    p->type = rd32(h);
    p->offset = rd32(h + 4);
    p->vaddr = rd32(h + 8);
    p->filesz = rd32(h + 16);
    p->memsz = rd32(h + 20);
    p->flags = rd32(h + 24);
}

static void copy_bounded(char *dst, size_t dstlen, const uint8_t *src, size_t n)
{
    if (n >= dstlen)
        n = dstlen - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

int elf_probe(const uint8_t *file, size_t len, elf_probe_t *out)
{
    if (!file || !out)
        return GP_ERR_INVAL;
    memset(out, 0, sizeof *out);
    if (len >= 2 && file[0] == '#' && file[1] == '!') {
        /* "#!interp arg\n": first word = interpreter, the rest = one argument
         * (the 2.4 binfmt_script behaviour the launch chain relies on, spec 7.3). */
        size_t end = 2;
        while (end < len && file[end] != '\n' && end < 2 + 127)
            end++;
        size_t i = 2;
        while (i < end && (file[i] == ' ' || file[i] == '\t'))
            i++;
        size_t w = i;
        while (w < end && file[w] != ' ' && file[w] != '\t')
            w++;
        if (w == i)
            return GP_OK; /* "#!" with no interpreter is not executable */
        copy_bounded(out->interp, sizeof out->interp, file + i, w - i);
        while (w < end && (file[w] == ' ' || file[w] == '\t'))
            w++;
        size_t a = end;
        while (a > w && (file[a - 1] == ' ' || file[a - 1] == '\t' || file[a - 1] == '\r'))
            a--;
        if (a > w)
            copy_bounded(out->interp_arg, sizeof out->interp_arg, file + w, a - w);
        out->kind = ELF_KIND_SCRIPT;
        return GP_OK;
    }
    ehdr_t e;
    if (parse_ehdr(file, len, &e) != GP_OK)
        return GP_OK; /* not an ELF we can run: kind stays NONE (-ENOEXEC) */
    out->kind = ELF_KIND_ELF;
    out->oabi = (e.flags & EF_ARM_EABI_MASK) == 0;
    out->is_dynamic = e.type == ET_DYN;
    for (unsigned i = 0; i < e.phnum; i++) {
        phdr_t p;
        parse_phdr(file, &e, i, &p);
        if (p.type == PT_INTERP && p.filesz > 0 && (uint64_t)p.offset + p.filesz <= len)
            copy_bounded(out->interp, sizeof out->interp, file + p.offset, p.filesz - 1);
    }
    return GP_OK;
}

static int prot_of(uint32_t flags)
{
    return ((flags & PF_R) ? GMEM_PROT_R : 0) | ((flags & PF_W) ? GMEM_PROT_W : 0) |
           ((flags & PF_X) ? GMEM_PROT_X : 0);
}

/* The protection a page gets: the union of the p_flags of every PT_LOAD that
 * covers it, so a page shared by text and data keeps both (the kernel maps
 * each segment separately; the union is what the guest can rely on). */
static int page_prot(const uint8_t *file, const ehdr_t *e, uint32_t base, gaddr_t page)
{
    int prot = 0;
    for (unsigned j = 0; j < e->phnum; j++) {
        phdr_t p;
        parse_phdr(file, e, j, &p);
        if (p.type != PT_LOAD || p.memsz == 0)
            continue;
        uint64_t s = ((uint64_t)p.vaddr + base) & GP2X_PAGE_MASK;
        uint64_t en = GP2X_PAGE_ALIGN_UP((uint64_t)p.vaddr + base + p.memsz);
        if (page >= s && page < en)
            prot |= prot_of(p.flags);
    }
    return prot;
}

int elf_load(gmem_t *m, const uint8_t *file, size_t len, gaddr_t dyn_base, elf_image_t *out)
{
    if (!m || !file || !out)
        return GP_ERR_INVAL;
    memset(out, 0, sizeof *out);
    ehdr_t e;
    int rc = parse_ehdr(file, len, &e);
    if (rc != GP_OK)
        return rc;
    uint32_t base = e.type == ET_DYN ? dyn_base : 0;
    out->oabi = (e.flags & EF_ARM_EABI_MASK) == 0;
    out->is_dynamic = e.type == ET_DYN;
    out->entry = e.entry + base;
    out->phent = e.phentsize;
    out->phnum = e.phnum;
    out->load_base = 0xFFFFFFFFu;

    /* Pass 1: validate, map every segment page RW, copy file bytes, zero .bss. */
    for (unsigned i = 0; i < e.phnum; i++) {
        phdr_t p;
        parse_phdr(file, &e, i, &p);
        if (p.type == PT_INTERP) {
            if (p.filesz == 0 || (uint64_t)p.offset + p.filesz > len)
                return GP_ERR_FORMAT;
            out->has_interp = true;
            copy_bounded(out->interp, sizeof out->interp, file + p.offset, p.filesz - 1);
            continue;
        }
        if (p.type != PT_LOAD)
            continue;
        if (p.filesz > p.memsz || (uint64_t)p.offset + p.filesz > len)
            return GP_ERR_FORMAT;
        uint64_t vaddr = (uint64_t)p.vaddr + base;
        if (vaddr + p.memsz > 0x100000000ull)
            return GP_ERR_FORMAT;
        if (p.memsz == 0)
            continue;
        uint64_t pstart = vaddr & GP2X_PAGE_MASK;
        uint64_t pend = GP2X_PAGE_ALIGN_UP(vaddr + p.memsz);
        for (uint64_t a = pstart; a < pend; a += GP2X_PAGE_SIZE) {
            if (gmem_is_mapped(m, (gaddr_t)a, GP2X_PAGE_SIZE, GMEM_PROT_NONE))
                continue; /* shared with an earlier segment: keep its bytes */
            rc = gmem_map_anon(m, (gaddr_t)a, GP2X_PAGE_SIZE, GMEM_PROT_RW);
            if (rc != GP_OK)
                return rc;
        }
        if (p.filesz && gmem_write(m, (gaddr_t)vaddr, file + p.offset, p.filesz) != GP_OK)
            return GP_ERR_FAULT;
        if (p.memsz > p.filesz && gmem_memset(m, (gaddr_t)(vaddr + p.filesz), 0, p.memsz - p.filesz) != GP_OK)
            return GP_ERR_FAULT;
        if ((gaddr_t)pstart < out->load_base)
            out->load_base = (gaddr_t)pstart;
        if ((gaddr_t)pend > out->load_end)
            out->load_end = (gaddr_t)pend;
        /* AT_PHDR: the program headers as mapped by this segment, if inside it. */
        if (e.phoff >= p.offset && (uint64_t)e.phoff + (uint64_t)e.phnum * 32 <= (uint64_t)p.offset + p.filesz)
            out->phdr_addr = (gaddr_t)(vaddr + (e.phoff - p.offset));
    }
    if (out->load_base == 0xFFFFFFFFu)
        return GP_ERR_FORMAT; /* no PT_LOAD at all */

    /* Pass 2: final protections, the union of the covering segments' p_flags. */
    for (uint64_t a = out->load_base; a < out->load_end; a += GP2X_PAGE_SIZE) {
        if (!gmem_is_mapped(m, (gaddr_t)a, GP2X_PAGE_SIZE, GMEM_PROT_NONE))
            continue; /* a gap between segments */
        rc = gmem_protect(m, (gaddr_t)a, GP2X_PAGE_SIZE, page_prot(file, &e, base, (gaddr_t)a));
        if (rc != GP_OK)
            return rc;
    }

    if (out->phdr_addr == 0) {
        /* The headers were in no segment: give them a read-only page of their
         * own so AT_PHDR is valid. */
        uint32_t sz = GP2X_PAGE_ALIGN_UP((uint32_t)e.phnum * 32u);
        gaddr_t page = gmem_find_free(m, out->load_end, sz);
        if (!page || gmem_map_anon(m, page, sz, GMEM_PROT_RW) != GP_OK)
            return GP_ERR_NOMEM;
        gmem_write(m, page, file + e.phoff, (uint32_t)e.phnum * 32u);
        gmem_protect(m, page, sz, GMEM_PROT_R);
        out->phdr_addr = page;
        if (page + sz > out->load_end)
            out->load_end = page + sz;
    }
    gp_debug("elf: loaded %s image: entry=%08x load=[%08x,%08x) phdr=%08x phnum=%u interp=%s",
             out->oabi ? "OABI" : "EABI", out->entry, out->load_base, out->load_end, out->phdr_addr,
             out->phnum, out->has_interp ? out->interp : "-");
    return GP_OK;
}

const elf_auxinfo_t *elf_default_auxinfo(void)
{
    static const elf_auxinfo_t gp2x = {
        .hwcap = HWCAP_SWP | HWCAP_HALF | HWCAP_THUMB | HWCAP_FAST_MULT, /* an ARM920T */
        .platform = "v4l", /* ARMv4 little-endian, the 2.4 arch/arm ELF_PLATFORM; OPEN-4 */
        .clktck = 100,
        .uid = 0, .euid = 0, .gid = 0, .egid = 0, /* the GP2X runs everything as root; OPEN-3 */
    };
    return &gp2x;
}

/* Pushes a string onto the guest stack (growing down) and returns its address. */
static int push_string(gmem_t *m, gaddr_t *sp, const char *s, gaddr_t *addr)
{
    uint32_t n = (uint32_t)strlen(s) + 1;
    *sp -= n;
    *addr = *sp;
    return gmem_write(m, *sp, s, n);
}

static int push_word(gmem_t *m, gaddr_t *sp, uint32_t v)
{
    *sp -= 4;
    return gmem_st32(m, *sp, v);
}

int elf_setup_stack(gmem_t *m, const elf_image_t *img, const elf_image_t *interp, const char *execfn, int argc,
                    const char *const *argv, const char *const *envp, const elf_auxinfo_t *aux,
                    gaddr_t stack_top, uint32_t stack_size, gaddr_t *sp_out)
{
    if (!m || !img || !argv || !aux || !sp_out || argc < 0 || stack_size < GP2X_PAGE_SIZE ||
        (stack_top & (GP2X_PAGE_SIZE - 1u)) || (stack_size & (GP2X_PAGE_SIZE - 1u)) || stack_top < stack_size ||
        stack_top > GUEST_TASK_SIZE)
        return GP_ERR_INVAL;
    int rc = gmem_map_anon(m, stack_top - stack_size, stack_size, GMEM_PROT_RW);
    if (rc != GP_OK)
        return rc;
    int envc = 0;
    while (envp && envp[envc])
        envc++;

    /* Strings, highest first: execfn, envp, argv, platform (the 2.4 order). */
    gaddr_t sp = stack_top - 16; /* the kernel leaves the top word empty */
    gaddr_t execfn_addr = 0, platform_addr = 0;
    if ((rc = push_string(m, &sp, execfn ? execfn : argv[0], &execfn_addr)) != GP_OK)
        return rc;
    gaddr_t *env_addr = calloc((size_t)envc + 1, sizeof *env_addr);
    gaddr_t *arg_addr = calloc((size_t)argc + 1, sizeof *arg_addr);
    if (!env_addr || !arg_addr) {
        free(env_addr);
        free(arg_addr);
        return GP_ERR_NOMEM;
    }
    for (int i = envc - 1; i >= 0 && rc == GP_OK; i--)
        rc = push_string(m, &sp, envp[i], &env_addr[i]);
    for (int i = argc - 1; i >= 0 && rc == GP_OK; i--)
        rc = push_string(m, &sp, argv[i], &arg_addr[i]);
    if (rc == GP_OK)
        rc = push_string(m, &sp, aux->platform, &platform_addr);
    if (rc != GP_OK) {
        free(env_addr);
        free(arg_addr);
        return rc;
    }

    /* Word area: auxv, envp, argv, argc; placed so the final sp is 8-aligned. */
    enum { AUX_PAIRS = 15 }; /* 14 entries plus AT_NULL */
    uint32_t words = 1u + (uint32_t)argc + 1u + (uint32_t)envc + 1u + AUX_PAIRS * 2u;
    sp &= ~3u;
    if (((sp - words * 4u) & 7u) != 0)
        sp -= 4;
    const uint32_t auxv[AUX_PAIRS * 2] = {
        AT_HWCAP, aux->hwcap,  AT_PAGESZ, GP2X_PAGE_SIZE, AT_CLKTCK, aux->clktck,
        AT_PHDR,  img->phdr_addr, AT_PHENT, img->phent,   AT_PHNUM,  img->phnum,
        AT_BASE,  interp ? interp->load_base : 0,  AT_FLAGS, 0,  AT_ENTRY, img->entry,
        AT_UID,   aux->uid,  AT_EUID, aux->euid,  AT_GID, aux->gid,  AT_EGID, aux->egid,
        AT_PLATFORM, platform_addr, AT_NULL, 0,
    };
    for (int i = AUX_PAIRS * 2 - 1; i >= 0 && rc == GP_OK; i--)
        rc = push_word(m, &sp, auxv[i]);
    if (rc == GP_OK)
        rc = push_word(m, &sp, 0); /* envp terminator */
    for (int i = envc - 1; i >= 0 && rc == GP_OK; i--)
        rc = push_word(m, &sp, env_addr[i]);
    if (rc == GP_OK)
        rc = push_word(m, &sp, 0); /* argv terminator */
    for (int i = argc - 1; i >= 0 && rc == GP_OK; i--)
        rc = push_word(m, &sp, arg_addr[i]);
    if (rc == GP_OK)
        rc = push_word(m, &sp, (uint32_t)argc);
    free(env_addr);
    free(arg_addr);
    if (rc != GP_OK)
        return rc;
    *sp_out = sp;
    gp_debug("elf: initial stack sp=%08x argc=%d envc=%d execfn=%08x", sp, argc, envc, execfn_addr);
    return GP_OK;
}
