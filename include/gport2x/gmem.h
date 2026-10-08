/* GPort2X guest address space (the interpreter engine's memory).
 *
 * A gmem_t is one 32-bit guest virtual address space made of 4 KB pages.
 * Each page is unmapped, RAM or MMIO:
 *   - RAM pages point into a refcounted OBJECT of host memory. One object can
 *     be mapped at many guest addresses, which keeps the upper bank's aliases
 *     coherent (spec 1.5: the whole bank, the video windows, fb0 and fb1 are
 *     views of the same 32 MB).
 *   - MMIO pages call device callbacks with the offset inside the object; the
 *     MMSP2 register file uses this for TCOUNT, GPIO and the test modes
 *     (spec 4.1, 4.3, #23).
 * The layout the guest sees is the 2.4 shape of spec 1.4: mmaps grow up from
 * GUEST_MMAP_BASE and nothing is mapped above GUEST_TASK_SIZE.
 *
 * Not thread-safe: the interpreter engine runs guest threads from one host
 * thread (ARCHITECTURE.md section 4). */
#ifndef GPORT2X_GMEM_H
#define GPORT2X_GMEM_H

#include <stdio.h>
#include "gport2x/types.h"

typedef struct gmem gmem_t;
typedef struct gmem_obj gmem_obj_t;

enum {
    GMEM_PROT_NONE = 0,
    GMEM_PROT_R = 1,
    GMEM_PROT_W = 2,
    GMEM_PROT_X = 4,
    GMEM_PROT_RW = 3,
    GMEM_PROT_RWX = 7,
    GMEM_MAP_SHARED = 8, /* flag OR'd into prot: the pages stay aliased across gmem_clone (MAP_SHARED) */
};

/* MMIO callbacks. off is the byte offset inside the object; size is 1, 2 or 4.
 * Reads return the value in the low bits; writes receive it there. */
typedef struct gmem_mmio_ops {
    uint32_t (*read)(void *ctx, uint32_t off, unsigned size);
    void (*write)(void *ctx, uint32_t off, unsigned size, uint32_t value);
} gmem_mmio_ops_t;

/* Objects. The creator holds one reference and drops it with gmem_obj_release;
 * every mapped page holds another, so an object lives while it is mapped. */
gmem_obj_t *gmem_obj_ram(size_t size);  /* zero-filled host memory; size page-aligned */
gmem_obj_t *gmem_obj_mmio(size_t size, const gmem_mmio_ops_t *ops, void *ctx);
/* RAM object over existing host memory (page-aligned size); the memory is
 * not freed with the object (the native engine's instruction emulator maps
 * the process's own pages this way). */
gmem_obj_t *gmem_obj_wrap(uint8_t *host, size_t size);
gmem_obj_t *gmem_obj_ref(gmem_obj_t *obj); /* adds a reference */
void gmem_obj_release(gmem_obj_t *obj);
uint8_t *gmem_obj_host(const gmem_obj_t *obj); /* RAM objects only, else NULL */
size_t gmem_obj_size(const gmem_obj_t *obj);

gmem_t *gmem_create(void);
/* Native mode (the native engine on a 32-bit ARM host, docs/NATIVE_ENGINE.md):
 * gmem_set_native(true) makes gmem_create return native spaces, whose
 * mappings are real host mappings at the guest addresses, and makes RAM
 * objects memfds. Fails on hosts where guest addresses cannot be host ones. */
int gmem_set_native(bool on);
bool gmem_is_native(const gmem_t *m);
bool gmem_native_mode(void); /* gmem_set_native(true) was called */
gmem_t *gmem_create_mode(bool native); /* gmem_create_mode(false): an ordinary space regardless */
/* Native mode: [lo, hi) holds the harness's own mapping; guest mappings never go there. */
int gmem_native_forbid(gaddr_t lo, gaddr_t hi);
void gmem_destroy(gmem_t *m);

/* Mapping. addr, len and obj_off must be page-aligned, len > 0, and the range
 * must not wrap. Mapping over an existing mapping replaces it (MAP_FIXED). */
int gmem_map_anon(gmem_t *m, gaddr_t addr, uint32_t len, int prot);
int gmem_map_obj(gmem_t *m, gaddr_t addr, uint32_t len, int prot, gmem_obj_t *obj,
                 uint32_t obj_off);
int gmem_unmap(gmem_t *m, gaddr_t addr, uint32_t len); /* unmapped pages are ignored */
int gmem_protect(gmem_t *m, gaddr_t addr, uint32_t len, int prot); /* all pages must be mapped; keeps the shared flag */
/* fork: a new space with the same layout. Private RAM pages are copied into
 * new objects; shared pages and MMIO pages alias the same objects. */
gmem_t *gmem_clone(const gmem_t *src);

/* The lowest free range of len bytes at or above hint and below
 * GUEST_TASK_SIZE (bottom-up, spec 1.4). Returns 0 if there is none. */
gaddr_t gmem_find_free(const gmem_t *m, gaddr_t hint, uint32_t len);
/* True if every page of [addr, addr+len) is mapped with at least prot. */
bool gmem_is_mapped(const gmem_t *m, gaddr_t addr, uint32_t len, int prot);

/* Bulk access across pages (for the syscall layer). Checks R / W. */
int gmem_read(gmem_t *m, gaddr_t addr, void *dst, uint32_t len);
int gmem_write(gmem_t *m, gaddr_t addr, const void *src, uint32_t len);
int gmem_memset(gmem_t *m, gaddr_t addr, uint8_t value, uint32_t len);

/* An observer of those bulk writes (the syscall layer's stores into guest
 * memory: a read()'s data, a stat buffer ...), called after each successful
 * gmem_write (gmem_memset writes through it). One per process; the call
 * capture sets it around a syscall. NULL removes it. */
typedef void (*gmem_write_observer_fn)(void *ctx, gmem_t *m, gaddr_t addr, const void *src, uint32_t len);
void gmem_set_write_observer(gmem_write_observer_fn fn, void *ctx);

/* Sized access for the CPU. The address is used exactly as given (the CPU
 * applies the ARMv4 alignment rules before calling); an access that crosses
 * a page is served bytewise. Checks R for loads, W for stores, X for fetches. */
int gmem_ld8(gmem_t *m, gaddr_t addr, uint32_t *out);
int gmem_ld16(gmem_t *m, gaddr_t addr, uint32_t *out);
int gmem_ld32(gmem_t *m, gaddr_t addr, uint32_t *out);
int gmem_st8(gmem_t *m, gaddr_t addr, uint32_t value);
int gmem_st16(gmem_t *m, gaddr_t addr, uint32_t value);
int gmem_st32(gmem_t *m, gaddr_t addr, uint32_t value);
int gmem_fetch32(gmem_t *m, gaddr_t addr, uint32_t *out);

/* Host pointer to [addr, addr+len) if it is one contiguous span of RAM with
 * at least prot (same object, consecutive pages), else NULL. */
uint8_t *gmem_host_span(gmem_t *m, gaddr_t addr, uint32_t len, int prot);
/* Fast-path support for the CPU's page cache: the host memory of the RAM
 * page holding addr if the page is mapped with at least prot (NULL for an
 * MMIO, unmapped or insufficiently protected page, which the sized accessors
 * then handle), and a generation stamp that changes whenever any mapping or
 * protection of any space changes, so cached pointers are revalidated with
 * one comparison. */
uint8_t *gmem_page_host(gmem_t *m, gaddr_t addr, int prot);
uint64_t gmem_generation(const gmem_t *m);
/* Translated-code tracking for the JIT (docs/JIT.md). gmem_mark_code records
 * [addr, addr+len), inside one RAM page, as translated: 0 = not markable
 * (MMIO, unmapped, or flagged for the interpreter after repeated rewrites),
 * 1 = the page was already marked, 2 = newly marked (gmem_code_epoch() then
 * changes and cached write pointers must be dropped). A write through gmem
 * that touches a translated word, and the release of the object, clears the
 * page's mark and calls the hook with the page's host address; writes to the
 * page's other words only take the checked path. */
int gmem_mark_code(gmem_t *m, gaddr_t addr, uint32_t len);
uint64_t gmem_code_epoch(void);
void gmem_set_code_hook(void (*fn)(const uint8_t *host_page));

/* Diagnostics. */
typedef struct gmem_fault {
    gaddr_t addr; /* the first byte that could not be accessed */
    int prot;     /* the access that failed (GMEM_PROT_R/W/X) */
} gmem_fault_t;
const gmem_fault_t *gmem_last_fault(const gmem_t *m);

typedef struct gmem_region {
    gaddr_t start, end; /* [start, end) */
    int prot;         /* GMEM_PROT_* bits */
    bool shared;      /* GMEM_MAP_SHARED */
    const gmem_obj_t *obj;
    uint32_t obj_off; /* object offset of the first page */
    bool mmio;
} gmem_region_t;
/* The maximal region of identically mapped consecutive pages containing addr.
 * Returns GP_ERR_FAULT if addr is unmapped. */
int gmem_region_at(const gmem_t *m, gaddr_t addr, gmem_region_t *out);
void gmem_dump(const gmem_t *m, FILE *out); /* one line per region */
/* Bytes of RAM objects currently alive (the budget of spec 1.6). */
size_t gmem_resident_bytes(const gmem_t *m);

#endif /* GPORT2X_GMEM_H */
