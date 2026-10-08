/* GPort2X guest address space: see include/gport2x/gmem.h.
 *
 * Layout: a two-level page table. l1[1024] entries each cover 4 MB and point
 * to an l2 array of 1024 page entries, allocated on first use, so an address
 * space with a few dozen mappings costs a few hundred KB. */
#include "gport2x/gmem.h"

#include <stdlib.h>
#include <string.h>
#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>
#endif
#if defined(__linux__) && UINTPTR_MAX == 0xFFFFFFFFu
#define GMEM_NATIVE_POSSIBLE 1 /* a 32-bit host: guest addresses can be host addresses */
#endif
#include "gport2x/log.h"

#define L1_COUNT 1024u
#define L2_COUNT 1024u
#define L1_INDEX(a) ((uint32_t)(a) >> (GP2X_PAGE_SHIFT + 10u))
#define L2_INDEX(a) (((uint32_t)(a) >> GP2X_PAGE_SHIFT) & (L2_COUNT - 1u))
#define PAGE_OFF(a) ((uint32_t)(a) & (GP2X_PAGE_SIZE - 1u))

struct gmem_obj {
    uint8_t *host;              /* RAM objects */
    const gmem_mmio_ops_t *ops; /* MMIO objects */
    void *ctx;
    size_t size;
    unsigned refs; /* the creator's reference plus one per mapped page */
    uint8_t *codebits; /* per page: CODE_MARK, the invalidation count, CODE_NOJIT; NULL until first marked */
    uint32_t **codemap; /* per marked page: a bitmap of its translated words (128 bytes), or NULL */
    bool external;      /* host memory owned by someone else (gmem_obj_wrap) */
    int fd;             /* native mode: the memfd behind a RAM object, else -1 */
};

/* Pages holding translated code (the JIT, docs/JIT.md). A marked page is never
 * handed out for writing by gmem_page_host, so every guest store to it takes
 * the checked path. A store that touches a translated word clears the mark and
 * calls the hook so the JIT drops the page's blocks (QEMU's write-protect
 * scheme, done in software); a store to the page's data words only (code and
 * data share pages in the game's RWX segment) is let through, as with QEMU's
 * per-page code bitmap. A page invalidated too often is flagged CODE_NOJIT and
 * left to the interpreter. */
#define CODE_MARK 0x01u
#define CODE_COUNT_SHIFT 1
#define CODE_COUNT_MAX 8u
#define CODE_NOJIT 0x80u
static void (*code_hook)(const uint8_t *host_page);
static uint64_t code_epoch = 1;

void gmem_set_code_hook(void (*fn)(const uint8_t *host_page)) { code_hook = fn; }
uint64_t gmem_code_epoch(void) { return code_epoch; }

static void code_written(gmem_obj_t *obj, uint32_t page_index)
{
    free(obj->codemap[page_index]);
    obj->codemap[page_index] = NULL;
    uint8_t b = obj->codebits[page_index];
    unsigned count = ((b >> CODE_COUNT_SHIFT) & 0x3Fu) + 1u;
    b = (uint8_t)((b & CODE_NOJIT) | ((count > 0x3Fu ? 0x3Fu : count) << CODE_COUNT_SHIFT));
    if (count > CODE_COUNT_MAX)
        b |= CODE_NOJIT;
    obj->codebits[page_index] = b;
    if (code_hook)
        code_hook(obj->host + (size_t)page_index * GP2X_PAGE_SIZE);
}

/* Called before host memory of [off, off+len) in obj is written. */
static inline void code_check(gmem_obj_t *obj, uint32_t off, uint32_t len)
{
    if (!obj->codebits || !len)
        return;
    for (uint32_t pg = off / GP2X_PAGE_SIZE; pg <= (off + len - 1u) / GP2X_PAGE_SIZE; pg++) {
        if (!(obj->codebits[pg] & CODE_MARK))
            continue;
        uint32_t lo = off > pg * GP2X_PAGE_SIZE ? off - pg * GP2X_PAGE_SIZE : 0;
        uint32_t hi = off + len - pg * GP2X_PAGE_SIZE;
        if (hi > GP2X_PAGE_SIZE)
            hi = GP2X_PAGE_SIZE;
        const uint32_t *map = obj->codemap[pg];
        bool hit = !map;
        for (uint32_t w = lo / 4u; !hit && w <= (hi - 1u) / 4u; w++)
            hit = (map[w / 32u] >> (w % 32u)) & 1u;
        if (hit)
            code_written(obj, pg);
    }
}

typedef struct page {
    gmem_obj_t *obj; /* NULL = unmapped */
    uint32_t off;    /* offset of this page inside obj */
    uint8_t prot;
} page_t;

struct gmem {
    page_t *l2[L1_COUNT];
    size_t resident; /* bytes of RAM objects alive */
    gmem_fault_t fault;
    uint64_t gen; /* the global stamp at this space's last mapping change */
    bool native;  /* mappings are real host mappings at the guest addresses (the native engine) */
};

/* ---------------------------------------------------------------- native mode
 *
 * The native engine (docs/NATIVE_ENGINE.md) runs guest code in the harness
 * process, so a guest address is a host address. A native space keeps the
 * same page bookkeeping, but every mapping is also a real host mapping at
 * the guest address: anonymous memory privately, RAM objects (memfds in
 * native mode) shared or copy-on-write, and an unmapped range is put back to
 * PROT_NONE so the harness's own allocations never land in the guest's
 * range. The page table then points at the guest addresses themselves, so
 * gmem_read/write and the syscall layer work unchanged. */
static bool native_default; /* gmem_create makes native spaces; RAM objects are memfds */

#ifndef GMEM_NATIVE_POSSIBLE
int gmem_native_forbid(gaddr_t lo, gaddr_t hi) { (void)lo; (void)hi; return GP_ERR_INVAL; }
#endif

int gmem_set_native(bool on)
{
#ifdef GMEM_NATIVE_POSSIBLE
    native_default = on;
    return GP_OK;
#else
    return on ? GP_ERR_INVAL : GP_OK;
#endif
}

bool gmem_is_native(const gmem_t *m) { return m && m->native; }
bool gmem_native_mode(void) { return native_default; }

#ifdef GMEM_NATIVE_POSSIBLE
static int host_prot(int prot)
{
    return ((prot & GMEM_PROT_R) ? PROT_READ : 0) | ((prot & GMEM_PROT_W) ? PROT_WRITE : 0) |
           ((prot & GMEM_PROT_X) ? PROT_EXEC : 0);
}

/* Host mappings inside the guest range that the guest must never replace
 * (a dynamically linked harness's loader lands there on arm64 kernels). */
static struct { gaddr_t lo, hi; } forbidden[16];
static int nforbidden;

int gmem_native_forbid(gaddr_t lo, gaddr_t hi)
{
    if (nforbidden == 16)
        return GP_ERR_NOMEM;
    forbidden[nforbidden].lo = lo;
    forbidden[nforbidden].hi = hi;
    nforbidden++;
    return GP_OK;
}

static int native_map(gaddr_t addr, uint32_t len, int prot, int fd, uint32_t off, bool shared)
{
    for (int i = 0; i < nforbidden; i++)
        if ((uint64_t)addr < forbidden[i].hi && (uint64_t)addr + len > forbidden[i].lo) {
            gp_warn("native mapping %08x+%x would replace the harness's own mapping %08x..%08x", addr, len,
                    forbidden[i].lo, forbidden[i].hi);
            return GP_ERR_NOMEM;
        }
    int flags = MAP_FIXED | (fd >= 0 ? (shared ? MAP_SHARED : MAP_PRIVATE) : (MAP_PRIVATE | MAP_ANONYMOUS));
    void *want = (void *)(uintptr_t)addr;
    void *p = mmap(want, len, host_prot(prot), flags, fd, (off_t)off);
    if (p != want) {
        gp_warn("native mmap of %08x+%x failed", addr, len);
        return GP_ERR_NOMEM;
    }
    return GP_OK;
}

/* Which native space owns each host page. Spaces of one process share the
 * host's address space (execve loads the new image before the old space is
 * destroyed, often at the same addresses), so a space releases only the
 * pages it still owns. */
static const gmem_t *native_owner[1u << (32 - GP2X_PAGE_SHIFT)];

static void native_own(const gmem_t *m, gaddr_t addr, uint32_t len)
{
    for (uint64_t a = addr; a < (uint64_t)addr + len; a += GP2X_PAGE_SIZE)
        native_owner[a >> GP2X_PAGE_SHIFT] = m;
}

/* Returns the pages of [addr, addr+len) that m owns to the inaccessible reservation. */
static void native_release(const gmem_t *m, gaddr_t addr, uint32_t len)
{
    uint64_t end = (uint64_t)addr + len;
    for (uint64_t a = addr; a < end;) {
        if (native_owner[a >> GP2X_PAGE_SHIFT] != m) {
            a += GP2X_PAGE_SIZE;
            continue;
        }
        uint64_t b = a;
        while (b < end && native_owner[b >> GP2X_PAGE_SHIFT] == m) {
            native_owner[b >> GP2X_PAGE_SHIFT] = NULL;
            b += GP2X_PAGE_SIZE;
        }
        mmap((void *)(uintptr_t)a, (size_t)(b - a), PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        a = b;
    }
}
#endif

/* One counter for every space, so a (space, stamp) pair is never reused even
 * if a destroyed space's address is. */
static uint64_t gen_counter;
static void touch(gmem_t *m) { m->gen = ++gen_counter; }

/* ---------------------------------------------------------------- objects */

static size_t ram_bytes_alive; /* RAM objects are shared between spaces; count globally */

gmem_obj_t *gmem_obj_ram(size_t size)
{
    if (size == 0 || (size & (GP2X_PAGE_SIZE - 1u)) != 0)
        return NULL;
    gmem_obj_t *obj = calloc(1, sizeof *obj);
    if (!obj)
        return NULL;
    obj->fd = -1;
#ifdef GMEM_NATIVE_POSSIBLE
    if (native_default) { /* a memfd, so the object can be mapped shared or copy-on-write at guest addresses */
        obj->fd = memfd_create("gport2x", 0);
        void *h = MAP_FAILED;
        if (obj->fd >= 0 && ftruncate(obj->fd, (off_t)size) == 0)
            h = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, obj->fd, 0);
        if (h == MAP_FAILED) {
            if (obj->fd >= 0)
                close(obj->fd);
            free(obj);
            return NULL;
        }
        obj->host = h;
        obj->size = size;
        obj->refs = 1;
        ram_bytes_alive += size;
        return obj;
    }
#endif
    obj->host = calloc(1, size);
    if (!obj->host) {
        free(obj);
        return NULL;
    }
    obj->size = size;
    obj->refs = 1;
    ram_bytes_alive += size;
    return obj;
}

gmem_obj_t *gmem_obj_wrap(uint8_t *host, size_t size)
{
    if (!host || size == 0 || (size & (GP2X_PAGE_SIZE - 1u)) != 0)
        return NULL;
    gmem_obj_t *obj = calloc(1, sizeof *obj);
    if (!obj)
        return NULL;
    obj->host = host;
    obj->size = size;
    obj->refs = 1;
    obj->external = true;
    obj->fd = -1;
    return obj;
}

gmem_obj_t *gmem_obj_mmio(size_t size, const gmem_mmio_ops_t *ops, void *ctx)
{
    if (size == 0 || (size & (GP2X_PAGE_SIZE - 1u)) != 0 || !ops)
        return NULL;
    gmem_obj_t *obj = calloc(1, sizeof *obj);
    if (!obj)
        return NULL;
    obj->ops = ops;
    obj->ctx = ctx;
    obj->size = size;
    obj->refs = 1;
    obj->fd = -1;
    return obj;
}

static void obj_retain(gmem_obj_t *obj)
{
    obj->refs++;
}

gmem_obj_t *gmem_obj_ref(gmem_obj_t *obj)
{
    obj->refs++;
    return obj;
}

void gmem_obj_release(gmem_obj_t *obj)
{
    if (!obj)
        return;
    if (--obj->refs != 0)
        return;
    if (obj->host) {
        if (obj->codebits)
            for (uint32_t pg = 0; pg < obj->size / GP2X_PAGE_SIZE; pg++)
                if ((obj->codebits[pg] & CODE_MARK) && code_hook)
                    code_hook(obj->host + (size_t)pg * GP2X_PAGE_SIZE);
        if (!obj->external) {
            ram_bytes_alive -= obj->size;
#ifdef GMEM_NATIVE_POSSIBLE
            if (obj->fd >= 0) {
                munmap(obj->host, obj->size);
                close(obj->fd);
            } else
#endif
                free(obj->host);
        }
    }
    if (obj->codemap)
        for (uint32_t pg = 0; pg < obj->size / GP2X_PAGE_SIZE; pg++)
            free(obj->codemap[pg]);
    free(obj->codemap);
    free(obj->codebits);
    free(obj);
}

uint8_t *gmem_obj_host(const gmem_obj_t *obj)
{
    return obj ? obj->host : NULL;
}

size_t gmem_obj_size(const gmem_obj_t *obj)
{
    return obj ? obj->size : 0;
}

/* ---------------------------------------------------------------- spaces */

gmem_t *gmem_create_mode(bool native)
{
    gmem_t *m = calloc(1, sizeof(gmem_t));
    if (m) {
        touch(m);
        m->native = native;
    }
    return m;
}

gmem_t *gmem_create(void) { return gmem_create_mode(native_default); }

void gmem_destroy(gmem_t *m)
{
    if (!m)
        return;
    for (uint32_t i = 0; i < L1_COUNT; i++) {
        page_t *l2 = m->l2[i];
        if (!l2)
            continue;
        for (uint32_t j = 0; j < L2_COUNT; j++)
            if (l2[j].obj) {
#ifdef GMEM_NATIVE_POSSIBLE
                if (m->native)
                    native_release(m, (gaddr_t)((i * L2_COUNT + j) << GP2X_PAGE_SHIFT), GP2X_PAGE_SIZE);
#endif
                gmem_obj_release(l2[j].obj);
            }
        free(l2);
    }
    free(m);
}

static page_t *page_at(const gmem_t *m, gaddr_t addr)
{
    page_t *l2 = m->l2[L1_INDEX(addr)];
    return l2 ? &l2[L2_INDEX(addr)] : NULL;
}

static page_t *page_ensure(gmem_t *m, gaddr_t addr)
{
    page_t **slot = &m->l2[L1_INDEX(addr)];
    if (!*slot) {
        *slot = calloc(L2_COUNT, sizeof(page_t));
        if (!*slot)
            return NULL;
    }
    return &(*slot)[L2_INDEX(addr)];
}

static bool range_ok(gaddr_t addr, uint32_t len)
{
    return len != 0 && (addr & (GP2X_PAGE_SIZE - 1u)) == 0 && (len & (GP2X_PAGE_SIZE - 1u)) == 0 &&
           (uint64_t)addr + len <= 0x100000000ull;
}

static void page_clear(gmem_t *m, page_t *p)
{
    (void)m;
    if (p->obj) {
        gmem_obj_release(p->obj);
        p->obj = NULL;
    }
    p->off = 0;
    p->prot = 0;
}

static int book_map(gmem_t *m, gaddr_t addr, uint32_t len, int prot, gmem_obj_t *obj, uint32_t obj_off);

int gmem_map_obj(gmem_t *m, gaddr_t addr, uint32_t len, int prot, gmem_obj_t *obj, uint32_t obj_off)
{
#ifdef GMEM_NATIVE_POSSIBLE
    if (m && m->native && obj) {
        if (!range_ok(addr, len) || (obj_off & (GP2X_PAGE_SIZE - 1u)) != 0 || (uint64_t)obj_off + len > obj->size)
            return GP_ERR_INVAL;
        if (obj->fd < 0) {
            gp_warn("native mapping at %08x: object has no memfd (MMIO or wrapped memory)", addr);
            return GP_ERR_INVAL;
        }
        int r = native_map(addr, len, prot, obj->fd, obj_off, (prot & GMEM_MAP_SHARED) != 0);
        if (r != GP_OK)
            return r;
        native_own(m, addr, len);
        gmem_obj_t *w = gmem_obj_wrap((uint8_t *)(uintptr_t)addr, len); /* host address == guest address */
        if (!w)
            return GP_ERR_NOMEM;
        r = book_map(m, addr, len, prot, w, 0);
        gmem_obj_release(w);
        return r;
    }
#endif
    return book_map(m, addr, len, prot, obj, obj_off);
}

static int book_map(gmem_t *m, gaddr_t addr, uint32_t len, int prot, gmem_obj_t *obj, uint32_t obj_off)
{
    if (!m || !obj || !range_ok(addr, len) || (obj_off & (GP2X_PAGE_SIZE - 1u)) != 0 ||
        (uint64_t)obj_off + len > obj->size)
        return GP_ERR_INVAL;
    /* Make sure every l2 array exists before touching pages, so the mapping is all-or-nothing. */
    for (uint64_t a = addr; a < (uint64_t)addr + len; a += GP2X_PAGE_SIZE)
        if (!page_ensure(m, (gaddr_t)a))
            return GP_ERR_NOMEM;
    for (uint64_t a = addr; a < (uint64_t)addr + len; a += GP2X_PAGE_SIZE) {
        page_t *p = page_at(m, (gaddr_t)a);
        page_clear(m, p);
        obj_retain(obj);
        p->obj = obj;
        p->off = obj_off + (uint32_t)(a - addr);
        p->prot = (uint8_t)(prot & (GMEM_PROT_RWX | GMEM_MAP_SHARED));
    }
    touch(m);
    return GP_OK;
}

int gmem_map_anon(gmem_t *m, gaddr_t addr, uint32_t len, int prot)
{
    if (!m || !range_ok(addr, len))
        return GP_ERR_INVAL;
#ifdef GMEM_NATIVE_POSSIBLE
    if (m->native) {
        int r = native_map(addr, len, prot, -1, 0, false);
        if (r != GP_OK)
            return r;
        native_own(m, addr, len);
        gmem_obj_t *w = gmem_obj_wrap((uint8_t *)(uintptr_t)addr, len);
        if (!w)
            return GP_ERR_NOMEM;
        r = book_map(m, addr, len, prot, w, 0);
        gmem_obj_release(w);
        return r;
    }
#endif
    gmem_obj_t *obj = gmem_obj_ram(len);
    if (!obj)
        return GP_ERR_NOMEM;
    int r = gmem_map_obj(m, addr, len, prot, obj, 0);
    gmem_obj_release(obj); /* the pages now hold the references */
    return r;
}

int gmem_unmap(gmem_t *m, gaddr_t addr, uint32_t len)
{
    if (!m || !range_ok(addr, len))
        return GP_ERR_INVAL;
#ifdef GMEM_NATIVE_POSSIBLE
    if (m->native)
        native_release(m, addr, len);
#endif
    for (uint64_t a = addr; a < (uint64_t)addr + len; a += GP2X_PAGE_SIZE) {
        page_t *p = page_at(m, (gaddr_t)a);
        if (p)
            page_clear(m, p);
    }
    touch(m);
    return GP_OK;
}

int gmem_protect(gmem_t *m, gaddr_t addr, uint32_t len, int prot)
{
    if (!m || !range_ok(addr, len))
        return GP_ERR_INVAL;
    for (uint64_t a = addr; a < (uint64_t)addr + len; a += GP2X_PAGE_SIZE) {
        page_t *p = page_at(m, (gaddr_t)a);
        if (!p || !p->obj) {
            m->fault.addr = (gaddr_t)a;
            m->fault.prot = prot;
            return GP_ERR_FAULT;
        }
    }
#ifdef GMEM_NATIVE_POSSIBLE
    if (m->native && mprotect((void *)(uintptr_t)addr, len, host_prot(prot)) != 0)
        return GP_ERR_INVAL;
#endif
    for (uint64_t a = addr; a < (uint64_t)addr + len; a += GP2X_PAGE_SIZE) {
        page_t *p = page_at(m, (gaddr_t)a);
        p->prot = (uint8_t)((prot & GMEM_PROT_RWX) | (p->prot & GMEM_MAP_SHARED));
    }
    touch(m);
    return GP_OK;
}

gmem_t *gmem_clone(const gmem_t *src)
{
    if (src && src->native) { /* the native engine forks for real; the kernel copies the space */
        gp_warn("gmem_clone of a native space");
        return NULL;
    }
    gmem_t *dst = gmem_create();
    if (!dst)
        return NULL;
    uint64_t a = 0;
    while (a < 0x100000000ull) {
        const page_t *p = page_at(src, (gaddr_t)a);
        if (!p || !p->obj) {
            /* skip whole empty l2 arrays quickly */
            if (!src->l2[L1_INDEX(a)])
                a = (a | (0x400000ull - 1)) + 1;
            else
                a += GP2X_PAGE_SIZE;
            continue;
        }
        /* extend the run: same object, consecutive offsets, same flags */
        uint64_t end = a + GP2X_PAGE_SIZE;
        while (end < 0x100000000ull) {
            const page_t *q = page_at(src, (gaddr_t)end);
            if (!q || q->obj != p->obj || q->off != p->off + (uint32_t)(end - a) || q->prot != p->prot)
                break;
            end += GP2X_PAGE_SIZE;
        }
        uint32_t len = (uint32_t)(end - a);
        int r;
        if ((p->prot & GMEM_MAP_SHARED) || !p->obj->host) {
            r = gmem_map_obj(dst, (gaddr_t)a, len, p->prot, p->obj, p->off);
        } else {
            gmem_obj_t *copy = gmem_obj_ram(len);
            if (!copy) {
                gmem_destroy(dst);
                return NULL;
            }
            memcpy(copy->host, p->obj->host + p->off, len);
            r = gmem_map_obj(dst, (gaddr_t)a, len, p->prot, copy, 0);
            gmem_obj_release(copy);
        }
        if (r != GP_OK) {
            gmem_destroy(dst);
            return NULL;
        }
        a = end;
    }
    return dst;
}

static bool page_mapped(const gmem_t *m, gaddr_t addr)
{
    const page_t *p = page_at(m, addr);
    return p && p->obj;
}

gaddr_t gmem_find_free(const gmem_t *m, gaddr_t hint, uint32_t len)
{
    if (!m || len == 0)
        return 0;
    uint64_t need = GP2X_PAGE_ALIGN_UP((uint64_t)len);
    uint64_t start = GP2X_PAGE_ALIGN_UP((uint64_t)hint);
    if (start == 0)
        start = GP2X_PAGE_SIZE; /* never hand out the NULL page */
    while (start + need <= GUEST_TASK_SIZE) {
        uint64_t a = start;
        for (; a < start + need; a += GP2X_PAGE_SIZE) {
            if (page_mapped(m, (gaddr_t)a))
                break;
#ifdef GMEM_NATIVE_POSSIBLE
            bool harness = false;
            for (int i = 0; m->native && i < nforbidden; i++)
                if (a >= forbidden[i].lo && a < forbidden[i].hi)
                    harness = true;
            if (harness)
                break;
#endif
        }
        if (a == start + need)
            return (gaddr_t)start;
        start = a + GP2X_PAGE_SIZE; /* skip past the mapped page */
    }
    return 0;
}

bool gmem_is_mapped(const gmem_t *m, gaddr_t addr, uint32_t len, int prot)
{
    if (!m || len == 0 || (uint64_t)addr + len > 0x100000000ull)
        return false;
    for (uint64_t a = addr & GP2X_PAGE_MASK; a < (uint64_t)addr + len; a += GP2X_PAGE_SIZE) {
        const page_t *p = page_at(m, (gaddr_t)a);
        if (!p || !p->obj || (p->prot & prot) != prot)
            return false;
    }
    return true;
}

/* ---------------------------------------------------------------- access */

static page_t *access_page(gmem_t *m, gaddr_t addr, int prot)
{
    page_t *p = page_at(m, addr);
    if (!p || !p->obj || (p->prot & prot) != prot) {
        m->fault.addr = addr;
        m->fault.prot = prot;
        return NULL;
    }
    return p;
}

static int mmio_read_bytes(page_t *p, uint32_t off, uint8_t *dst, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        dst[i] = (uint8_t)p->obj->ops->read(p->obj->ctx, off + i, 1);
    return GP_OK;
}

static int mmio_write_bytes(page_t *p, uint32_t off, const uint8_t *src, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        p->obj->ops->write(p->obj->ctx, off + i, 1, src[i]);
    return GP_OK;
}

int gmem_read(gmem_t *m, gaddr_t addr, void *dst, uint32_t len)
{
    if (!m || (len && !dst) || (uint64_t)addr + len > 0x100000000ull)
        return GP_ERR_INVAL;
    uint8_t *out = dst;
    while (len) {
        page_t *p = access_page(m, addr, GMEM_PROT_R);
        if (!p)
            return GP_ERR_FAULT;
        uint32_t in_page = GP2X_PAGE_SIZE - PAGE_OFF(addr);
        uint32_t n = len < in_page ? len : in_page;
        uint32_t off = p->off + PAGE_OFF(addr);
        if (p->obj->host)
            memcpy(out, p->obj->host + off, n);
        else
            mmio_read_bytes(p, off, out, n);
        addr += n;
        out += n;
        len -= n;
    }
    return GP_OK;
}

int gmem_write(gmem_t *m, gaddr_t addr, const void *src, uint32_t len)
{
    if (!m || (len && !src) || (uint64_t)addr + len > 0x100000000ull)
        return GP_ERR_INVAL;
    const uint8_t *in = src;
    while (len) {
        page_t *p = access_page(m, addr, GMEM_PROT_W);
        if (!p)
            return GP_ERR_FAULT;
        uint32_t in_page = GP2X_PAGE_SIZE - PAGE_OFF(addr);
        uint32_t n = len < in_page ? len : in_page;
        uint32_t off = p->off + PAGE_OFF(addr);
        if (p->obj->host) {
            code_check(p->obj, off, n);
            memcpy(p->obj->host + off, in, n);
        } else {
            mmio_write_bytes(p, off, in, n);
        }
        addr += n;
        in += n;
        len -= n;
    }
    return GP_OK;
}

int gmem_memset(gmem_t *m, gaddr_t addr, uint8_t value, uint32_t len)
{
    uint8_t chunk[256];
    memset(chunk, value, sizeof chunk);
    while (len) {
        uint32_t n = len < sizeof chunk ? len : (uint32_t)sizeof chunk;
        int r = gmem_write(m, addr, chunk, n);
        if (r != GP_OK)
            return r;
        addr += n;
        len -= n;
    }
    return GP_OK;
}

/* Sized loads and stores. A value that crosses a page is served through the
 * bulk path; everything else is one host access or one MMIO callback. */
static int load_sized(gmem_t *m, gaddr_t addr, unsigned size, int prot, uint32_t *out)
{
    if (PAGE_OFF(addr) + size > GP2X_PAGE_SIZE) {
        uint8_t buf[4] = {0};
        if (prot == GMEM_PROT_X) { /* fetches across a page are checked per page */
            for (unsigned i = 0; i < size; i++) {
                uint32_t b;
                int r = load_sized(m, addr + i, 1, prot, &b);
                if (r != GP_OK)
                    return r;
                buf[i] = (uint8_t)b;
            }
        } else {
            int r = gmem_read(m, addr, buf, size);
            if (r != GP_OK)
                return r;
        }
        *out = (uint32_t)buf[0] | (uint32_t)buf[1] << 8 | (uint32_t)buf[2] << 16 | (uint32_t)buf[3] << 24;
        return GP_OK;
    }
    page_t *p = access_page(m, addr, prot);
    if (!p)
        return GP_ERR_FAULT;
    uint32_t off = p->off + PAGE_OFF(addr);
    if (p->obj->host) {
        const uint8_t *h = p->obj->host + off;
        switch (size) {
        case 1: *out = h[0]; break;
        case 2: *out = (uint32_t)h[0] | (uint32_t)h[1] << 8; break;
        default: *out = (uint32_t)h[0] | (uint32_t)h[1] << 8 | (uint32_t)h[2] << 16 | (uint32_t)h[3] << 24; break;
        }
    } else {
        *out = p->obj->ops->read(p->obj->ctx, off, size);
        if (size == 1)
            *out &= 0xFFu;
        else if (size == 2)
            *out &= 0xFFFFu;
    }
    return GP_OK;
}

static int store_sized(gmem_t *m, gaddr_t addr, unsigned size, uint32_t value)
{
    if (PAGE_OFF(addr) + size > GP2X_PAGE_SIZE) {
        uint8_t buf[4] = {(uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24)};
        return gmem_write(m, addr, buf, size);
    }
    page_t *p = access_page(m, addr, GMEM_PROT_W);
    if (!p)
        return GP_ERR_FAULT;
    uint32_t off = p->off + PAGE_OFF(addr);
    if (p->obj->host) {
        code_check(p->obj, off, size);
        uint8_t *h = p->obj->host + off;
        h[0] = (uint8_t)value;
        if (size >= 2)
            h[1] = (uint8_t)(value >> 8);
        if (size == 4) {
            h[2] = (uint8_t)(value >> 16);
            h[3] = (uint8_t)(value >> 24);
        }
    } else {
        p->obj->ops->write(p->obj->ctx, off, size, value);
    }
    return GP_OK;
}

int gmem_ld8(gmem_t *m, gaddr_t addr, uint32_t *out) { return load_sized(m, addr, 1, GMEM_PROT_R, out); }
int gmem_ld16(gmem_t *m, gaddr_t addr, uint32_t *out) { return load_sized(m, addr, 2, GMEM_PROT_R, out); }
int gmem_ld32(gmem_t *m, gaddr_t addr, uint32_t *out) { return load_sized(m, addr, 4, GMEM_PROT_R, out); }
int gmem_fetch32(gmem_t *m, gaddr_t addr, uint32_t *out) { return load_sized(m, addr, 4, GMEM_PROT_X, out); }

uint8_t *gmem_page_host(gmem_t *m, gaddr_t addr, int prot)
{
    page_t *p = m ? page_at(m, addr) : NULL;
    if (!p || !p->obj || (p->prot & prot) != prot || !p->obj->host)
        return NULL;
    if ((prot & GMEM_PROT_W) && p->obj->codebits && (p->obj->codebits[p->off / GP2X_PAGE_SIZE] & CODE_MARK))
        return NULL; /* translated code: writes go through the checked path */
    return p->obj->host + p->off;
}

int gmem_mark_code(gmem_t *m, gaddr_t addr, uint32_t len)
{
    page_t *p = m ? page_at(m, addr) : NULL;
    if (!p || !p->obj || !p->obj->host || !len || PAGE_OFF(addr) + len > GP2X_PAGE_SIZE)
        return 0;
    gmem_obj_t *obj = p->obj;
    uint32_t pages = (uint32_t)(obj->size / GP2X_PAGE_SIZE);
    if (!obj->codebits) {
        obj->codebits = calloc(pages, 1);
        obj->codemap = calloc(pages, sizeof *obj->codemap);
        if (!obj->codebits || !obj->codemap) {
            free(obj->codebits);
            free(obj->codemap);
            obj->codebits = NULL;
            obj->codemap = NULL;
            return 0;
        }
    }
    uint32_t pg = p->off / GP2X_PAGE_SIZE;
    uint8_t *b = &obj->codebits[pg];
    if (*b & CODE_NOJIT)
        return 0;
    if (!obj->codemap[pg] && !(obj->codemap[pg] = calloc(GP2X_PAGE_SIZE / 32u, 1)))
        return 0;
    uint32_t *map = obj->codemap[pg];
    for (uint32_t w = PAGE_OFF(addr) / 4u; w <= (PAGE_OFF(addr) + len - 1u) / 4u; w++)
        map[w / 32u] |= 1u << (w % 32u);
    if (!(*b & CODE_MARK)) {
        *b |= CODE_MARK;
        code_epoch++; /* every cpu drops write-cache entries on its next run */
        return 2;
    }
    return 1;
}

uint64_t gmem_generation(const gmem_t *m) { return m ? m->gen : 0; }
int gmem_st8(gmem_t *m, gaddr_t addr, uint32_t value) { return store_sized(m, addr, 1, value); }
int gmem_st16(gmem_t *m, gaddr_t addr, uint32_t value) { return store_sized(m, addr, 2, value); }
int gmem_st32(gmem_t *m, gaddr_t addr, uint32_t value) { return store_sized(m, addr, 4, value); }

uint8_t *gmem_host_span(gmem_t *m, gaddr_t addr, uint32_t len, int prot)
{
    if (!m || len == 0 || (uint64_t)addr + len > 0x100000000ull)
        return NULL;
    page_t *first = access_page(m, addr, prot);
    if (!first || !first->obj->host)
        return NULL;
    uint32_t expect_off = first->off;
    for (uint64_t a = addr & GP2X_PAGE_MASK; a < (uint64_t)addr + len; a += GP2X_PAGE_SIZE) {
        page_t *p = access_page(m, (gaddr_t)a, prot);
        if (!p || p->obj != first->obj || p->off != expect_off)
            return NULL;
        expect_off += GP2X_PAGE_SIZE;
    }
    if (prot & GMEM_PROT_W) /* the caller may write through the pointer */
        code_check(first->obj, first->off + PAGE_OFF(addr), len);
    return first->obj->host + first->off + PAGE_OFF(addr);
}

/* ---------------------------------------------------------------- diagnostics */

const gmem_fault_t *gmem_last_fault(const gmem_t *m)
{
    return m ? &m->fault : NULL;
}

static bool same_mapping(const page_t *a, const page_t *b, uint32_t pages_apart)
{
    return a->obj == b->obj && a->prot == b->prot && b->off == a->off + pages_apart * GP2X_PAGE_SIZE;
}

int gmem_region_at(const gmem_t *m, gaddr_t addr, gmem_region_t *out)
{
    if (!m || !out)
        return GP_ERR_INVAL;
    const page_t *p = page_at(m, addr);
    if (!p || !p->obj)
        return GP_ERR_FAULT;
    gaddr_t page = addr & GP2X_PAGE_MASK;
    uint64_t start = page, end = (uint64_t)page + GP2X_PAGE_SIZE;
    while (start >= GP2X_PAGE_SIZE) {
        const page_t *q = page_at(m, (gaddr_t)(start - GP2X_PAGE_SIZE));
        if (!q || !q->obj || !same_mapping(q, p, (uint32_t)((page - (start - GP2X_PAGE_SIZE)) >> GP2X_PAGE_SHIFT)))
            break;
        start -= GP2X_PAGE_SIZE;
    }
    while (end < 0x100000000ull) {
        const page_t *q = page_at(m, (gaddr_t)end);
        if (!q || !q->obj || !same_mapping(p, q, (uint32_t)((end - page) >> GP2X_PAGE_SHIFT)))
            break;
        end += GP2X_PAGE_SIZE;
    }
    const page_t *sp = page_at(m, (gaddr_t)start);
    out->start = (gaddr_t)start;
    out->end = (gaddr_t)end; /* 0 if the region runs to the top of the space */
    out->prot = sp->prot & GMEM_PROT_RWX;
    out->shared = (sp->prot & GMEM_MAP_SHARED) != 0;
    out->obj = sp->obj;
    out->obj_off = sp->off;
    out->mmio = sp->obj->host == NULL;
    return GP_OK;
}

void gmem_dump(const gmem_t *m, FILE *out)
{
    if (!m || !out)
        return;
    uint64_t a = 0;
    while (a < 0x100000000ull) {
        gmem_region_t r;
        if (gmem_region_at(m, (gaddr_t)a, &r) != GP_OK) {
            a += GP2X_PAGE_SIZE;
            continue;
        }
        uint64_t end = r.end ? r.end : 0x100000000ull;
        fprintf(out, "%08x-%08llx %c%c%c %s obj=%p+0x%x\n", r.start, (unsigned long long)end,
                (r.prot & GMEM_PROT_R) ? 'r' : '-', (r.prot & GMEM_PROT_W) ? 'w' : '-',
                (r.prot & GMEM_PROT_X) ? 'x' : '-', r.mmio ? "mmio" : "ram ", (const void *)r.obj, r.obj_off);
        a = end;
    }
}

size_t gmem_resident_bytes(const gmem_t *m)
{
    (void)m;
    return ram_bytes_alive;
}
