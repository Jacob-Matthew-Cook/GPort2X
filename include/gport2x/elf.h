/* GPort2X ELF32 ARM loader (spec 1.1, 1.4, #1).
 *
 * Loads OABI (and EABI test) executables at their fixed virtual addresses,
 * honouring PT_LOAD protections, sets the program break after .bss, records
 * PT_INTERP for dynamic programs, and builds the initial stack (argc, argv,
 * envp, auxv) in the 2.4 shape of spec 1.4. The file is given as a buffer;
 * the fs layer reads it from the guest namespace. */
#ifndef GPORT2X_ELF_H
#define GPORT2X_ELF_H

#include "gport2x/gmem.h"
#include "gport2x/types.h"

enum elf_kind {
    ELF_KIND_NONE = 0, /* neither ELF nor a script: execve returns -ENOEXEC (spec 1.3, 7.3) */
    ELF_KIND_ELF,      /* a loadable ELF32 ARM executable */
    ELF_KIND_SCRIPT,   /* starts with "#!"; interp holds the interpreter path */
};

typedef struct elf_probe {
    enum elf_kind kind;
    bool oabi;        /* e_flags EABI version 0 (spec 1.1) */
    bool is_dynamic;  /* ET_DYN */
    char interp[256]; /* PT_INTERP path, or the "#!" interpreter (first word) */
    char interp_arg[256]; /* the optional "#!" argument */
} elf_probe_t;

typedef struct elf_image {
    gaddr_t entry;     /* e_entry (plus the base for ET_DYN) */
    gaddr_t load_base; /* lowest page of any PT_LOAD */
    gaddr_t load_end;  /* first page after the highest PT_LOAD: the initial brk */
    gaddr_t phdr_addr; /* AT_PHDR */
    uint16_t phent;    /* AT_PHENT */
    uint16_t phnum;    /* AT_PHNUM */
    bool oabi;
    bool is_dynamic;
    bool has_interp;
    char interp[256];
} elf_image_t;

/* Classifies a file for execve without loading it (spec 1.3). */
int elf_probe(const uint8_t *file, size_t len, elf_probe_t *out);

/* Loads the file's PT_LOAD segments into m. ET_DYN images are placed at
 * dyn_base (ignored for ET_EXEC). Pages shared by two segments keep both
 * contents and the union of their protections. Returns GP_ERR_FORMAT for a
 * malformed or non-ARM file. */
int elf_load(gmem_t *m, const uint8_t *file, size_t len, gaddr_t dyn_base, elf_image_t *out);

/* Auxiliary-vector facts the kernel would report (spec 1.4; OPEN-4). */
typedef struct elf_auxinfo {
    uint32_t hwcap;       /* AT_HWCAP */
    const char *platform; /* AT_PLATFORM string, e.g. "v4l" */
    uint32_t clktck;      /* AT_CLKTCK (100 on 2.4) */
    uint32_t uid, euid, gid, egid;
} elf_auxinfo_t;

/* Maps a stack of stack_size bytes ending at stack_top (RW) and writes the
 * initial frame: strings (execfn, envp, argv, platform), then auxv, envp,
 * argv and argc at the returned, 8-byte-aligned sp. interp may be NULL;
 * when given, AT_BASE is its load base. */
int elf_setup_stack(gmem_t *m, const elf_image_t *img, const elf_image_t *interp, const char *execfn,
                    int argc, const char *const *argv, const char *const *envp, const elf_auxinfo_t *aux,
                    gaddr_t stack_top, uint32_t stack_size, gaddr_t *sp_out);

/* Default auxiliary facts for a GP2X (spec OPEN-4: best current values). */
const elf_auxinfo_t *elf_default_auxinfo(void);

#endif /* GPORT2X_ELF_H */
