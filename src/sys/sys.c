/* Syscall decode, argument marshalling and dispatch (spec 2). OABI calls
 * arrive as svc #(0x900000 + N) with arguments in r0..r6; EABI test
 * programs as svc #0 with N in r7. Results go to r0 (negative errno on
 * failure). A call that would block parks the task and leaves the
 * registers untouched so the scheduler can retry it. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../proc/task.h"

#define SYS_BLOCK (-0x7FFF0001)   /* the handler parked the task */
#define SYS_NORETURN (-0x7FFF0002) /* r0 must not be written (exit, exec, sigreturn) */

typedef struct call {
    gtask_t *t;
    gsys_t *s;
    gmem_t *m;
    cpu_regs_t *r;
    uint32_t nr;
    uint32_t imm;
    bool eabi;
    bool retry;
} call_t;

#define A(c, i) ((c)->r->r[i])

/* Linux 2.4 timers tick at HZ = 100: a sleep or a poll timeout is rounded
 * up to whole jiffies and waits one jiffy more (sys_nanosleep's
 * timespec_to_jiffies(t) + 1), counted from the current tick, so usleep(1)
 * takes 10..20 ms on the device and ends on a tick.
 * The firmware menu's event loop is paced by exactly that. */
#define JIFFY_NS 10000000ull

/* The deadline of a sleep of ns starting at now: the timer fires on the
 * tick (jiffy boundary) n + 1 jiffies after the current one, where n is ns in
 * whole jiffies rounded up, so the sleep lasts between n and n + 1 jiffies. */
static uint64_t jiffy_deadline(uint64_t now, uint64_t ns)
{
    if (ns == 0)
        return now;
    uint64_t jiffies = (ns + JIFFY_NS - 1) / JIFFY_NS + 1;
    return (now / JIFFY_NS + jiffies) * JIFFY_NS;
}

/* ---- guest memory helpers ---------------------------------------------- */

static int cp_in(call_t *c, gaddr_t a, void *dst, uint32_t n)
{
    return gmem_read(c->m, a, dst, n) == GP_OK ? 0 : -EFAULT;
}

static int cp_out(call_t *c, gaddr_t a, const void *src, uint32_t n)
{
    return gmem_write(c->m, a, src, n) == GP_OK ? 0 : -EFAULT;
}

static int get_u32(call_t *c, gaddr_t a, uint32_t *v) { return gmem_ld32(c->m, a, v) == GP_OK ? 0 : -EFAULT; }
static int put_u32(call_t *c, gaddr_t a, uint32_t v) { return gmem_st32(c->m, a, v) == GP_OK ? 0 : -EFAULT; }

static int get_str(call_t *c, gaddr_t a, char *buf, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint32_t b;
        if (gmem_ld8(c->m, a + (gaddr_t)i, &b) != GP_OK)
            return -EFAULT;
        buf[i] = (char)b;
        if (!b)
            return (int)i;
    }
    return -ENAMETOOLONG;
}

static int get_path(call_t *c, gaddr_t a, char *buf)
{
    int r = get_str(c, a, buf, FS_PATH_MAX);
    return r < 0 ? r : 0;
}

/* NULL-terminated array of guest strings -> host array (free with free_strv). */
static int get_strv(call_t *c, gaddr_t a, char ***out)
{
    size_t cap = 16, n = 0;
    char **v = calloc(cap, sizeof *v);
    if (!v)
        return -ENOMEM;
    for (;;) {
        uint32_t p;
        if (a == 0) {
            break; /* a NULL argv/envp is an empty list */
        }
        if (get_u32(c, a, &p) < 0) {
            for (size_t i = 0; i < n; i++) free(v[i]);
            free(v);
            return -EFAULT;
        }
        if (!p)
            break;
        char *s = malloc(FS_PATH_MAX);
        if (!s || get_str(c, p, s, FS_PATH_MAX) < 0) {
            free(s);
            for (size_t i = 0; i < n; i++) free(v[i]);
            free(v);
            return -EFAULT;
        }
        if (n + 2 > cap) {
            cap *= 2;
            v = realloc(v, cap * sizeof *v);
        }
        v[n++] = s;
        if (n > 4096) {
            for (size_t i = 0; i < n; i++) free(v[i]);
            free(v);
            return -E2BIG;
        }
        a += 4;
    }
    v[n] = NULL;
    *out = v;
    return 0;
}

static void free_strv(char **v)
{
    if (!v)
        return;
    for (size_t i = 0; v[i]; i++)
        free(v[i]);
    free(v);
}

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }
static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }

static fs_file_t *fdget(call_t *c, int fd) { return fs_fdt_get(c->t->fdt, fd); }

/* A file result of -EAGAIN on a blocking descriptor parks the task. */
static int block_or(call_t *c, fs_file_t *f, int64_t r)
{
    if (r == -EAGAIN && !(f->gflags & GUEST_O_NONBLOCK)) {
        gtask_block(c->t, BLK_SYSCALL);
        return SYS_BLOCK;
    }
    return (int)r;
}

/* ---- struct writers ---------------------------------------------------- */

static int write_stat64(call_t *c, gaddr_t a, const fs_stat_t *st)
{
    uint8_t b[104];
    memset(b, 0, sizeof b);
    if (!c->eabi) { /* OABI stat64: packed to 4 bytes (sys_oabi_stat64 layout), 96 bytes */
        put64(b + 0, st->dev);
        put32(b + 12, (uint32_t)st->ino);
        put32(b + 16, st->mode);
        put32(b + 20, st->nlink);
        put32(b + 24, st->uid);
        put32(b + 28, st->gid);
        put64(b + 32, st->rdev);
        put64(b + 44, (uint64_t)st->size);
        put32(b + 52, st->blksize);
        put64(b + 56, st->blocks);
        put32(b + 64, (uint32_t)st->atime);
        put32(b + 72, (uint32_t)st->mtime);
        put32(b + 80, (uint32_t)st->ctime);
        put64(b + 88, st->ino);
        return cp_out(c, a, b, 96);
    }
    put64(b + 0, st->dev);
    put32(b + 12, (uint32_t)st->ino);
    put32(b + 16, st->mode);
    put32(b + 20, st->nlink);
    put32(b + 24, st->uid);
    put32(b + 28, st->gid);
    put64(b + 32, st->rdev);
    put64(b + 48, (uint64_t)st->size);
    put32(b + 56, st->blksize);
    put64(b + 64, st->blocks);
    put32(b + 72, (uint32_t)st->atime);
    put32(b + 80, (uint32_t)st->mtime);
    put32(b + 88, (uint32_t)st->ctime);
    put64(b + 96, st->ino);
    return cp_out(c, a, b, 104);
}

static int write_oldstat(call_t *c, gaddr_t a, const fs_stat_t *st)
{
    uint8_t b[64];
    memset(b, 0, sizeof b);
    put32(b + 0, (uint32_t)st->dev);
    put32(b + 4, (uint32_t)st->ino);
    put16(b + 8, (uint16_t)st->mode);
    put16(b + 10, (uint16_t)st->nlink);
    put16(b + 12, (uint16_t)st->uid);
    put16(b + 14, (uint16_t)st->gid);
    put32(b + 16, (uint32_t)st->rdev);
    put32(b + 20, (uint32_t)st->size);
    put32(b + 24, st->blksize);
    put32(b + 28, (uint32_t)st->blocks);
    put32(b + 32, (uint32_t)st->atime);
    put32(b + 40, (uint32_t)st->mtime);
    put32(b + 48, (uint32_t)st->ctime);
    return cp_out(c, a, b, 64);
}

static int write_statfs(call_t *c, gaddr_t a, const fs_statfs_t *sf)
{
    uint8_t b[64];
    memset(b, 0, sizeof b);
    put32(b + 0, sf->type);
    put32(b + 4, sf->bsize);
    put32(b + 8, sf->blocks);
    put32(b + 12, sf->bfree);
    put32(b + 16, sf->bavail);
    put32(b + 20, sf->files);
    put32(b + 24, sf->ffree);
    put32(b + 36, sf->namelen);
    put32(b + 40, sf->frsize);
    return cp_out(c, a, b, 64);
}

static int write_statfs64(call_t *c, gaddr_t a, uint32_t sz, const fs_statfs_t *sf)
{
    uint8_t b[88];
    memset(b, 0, sizeof b);
    put32(b + 0, sf->type);
    put32(b + 4, sf->bsize);
    put64(b + 8, sf->blocks);
    put64(b + 16, sf->bfree);
    put64(b + 24, sf->bavail);
    put64(b + 32, sf->files);
    put64(b + 40, sf->ffree);
    put32(b + 56, sf->namelen);
    put32(b + 60, sf->frsize);
    if (sz != 84 && sz != 88)
        return -EINVAL;
    return cp_out(c, a, b, sz);
}

/* ---- files ------------------------------------------------------------- */

static int do_open(call_t *c, gaddr_t pathp, int flags, uint32_t mode)
{
    char path[FS_PATH_MAX];
    int r = get_path(c, pathp, path);
    if (r < 0)
        return r;
    fs_file_t *f;
    r = fs_open(c->s->fs, &c->t->fs->ctx, path, flags, mode, &f);
    if (r < 0)
        return r;
    int fd = fs_fdt_install(c->t->fdt, f, (flags & 02000000) != 0, 0);
    fs_file_unref(f);
    return fd;
}

static int do_read(call_t *c, int fd, gaddr_t buf, uint32_t n)
{
    fs_file_t *f = fdget(c, fd);
    if (!f)
        return -EBADF;
    if (n == 0)
        return 0;
    /* A regular file read returns the full count short of EOF, as the kernel
     * does; devices and pipes return what they have. */
    uint8_t tmp[65536];
    uint32_t done = 0;
    while (done < n) {
        uint32_t chunk = n - done > sizeof tmp ? sizeof tmp : n - done;
        int64_t got = fs_file_read(f, tmp, chunk);
        if (got < 0)
            return done ? (int)done : block_or(c, f, got);
        if (got > 0 && cp_out(c, buf + done, tmp, (uint32_t)got) < 0)
            return done ? (int)done : -EFAULT;
        done += (uint32_t)got;
        if ((uint32_t)got < chunk || !f->seekable)
            break;
    }
    return (int)done;
}

static int do_write(call_t *c, int fd, gaddr_t buf, uint32_t n)
{
    fs_file_t *f = fdget(c, fd);
    if (!f)
        return -EBADF;
    if (n == 0)
        return 0;
    uint8_t tmp[65536];
    uint32_t done = 0;
    while (done < n) {
        uint32_t chunk = n - done > sizeof tmp ? sizeof tmp : n - done;
        if (cp_in(c, buf + done, tmp, chunk) < 0)
            return done ? (int)done : -EFAULT;
        int64_t put = fs_file_write(f, tmp, chunk);
        if (put == -EPIPE && !done)
            gtask_send_signal(c->t, GSIGPIPE, c->t->pid);
        if (put < 0)
            return done ? (int)done : block_or(c, f, put);
        done += (uint32_t)put;
        if ((uint32_t)put < chunk)
            break;
    }
    return (int)done;
}

static int do_pread(call_t *c, int fd, gaddr_t buf, uint32_t n, uint64_t off, bool write)
{
    fs_file_t *f = fdget(c, fd);
    if (!f)
        return -EBADF;
    uint8_t tmp[65536];
    uint32_t chunk = n > sizeof tmp ? sizeof tmp : n;
    if (write) {
        if (cp_in(c, buf, tmp, chunk) < 0)
            return -EFAULT;
        int64_t r = fs_file_pwrite(f, (int64_t)off, tmp, chunk);
        return r < 0 ? block_or(c, f, r) : (int)r;
    }
    int64_t r = fs_file_pread(f, (int64_t)off, tmp, chunk);
    if (r < 0)
        return block_or(c, f, r);
    if (r > 0 && cp_out(c, buf, tmp, (uint32_t)r) < 0)
        return -EFAULT;
    return (int)r;
}

static int do_iov(call_t *c, int fd, gaddr_t iov, uint32_t cnt, bool write)
{
    fs_file_t *f = fdget(c, fd);
    if (!f)
        return -EBADF;
    if (cnt > 1024)
        return -EINVAL;
    int64_t total = 0;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t base, len;
        if (get_u32(c, iov + i * 8, &base) < 0 || get_u32(c, iov + i * 8 + 4, &len) < 0)
            return -EFAULT;
        if (len == 0)
            continue;
        uint8_t tmp[65536];
        uint32_t chunk = len > sizeof tmp ? sizeof tmp : len;
        int64_t r;
        if (write) {
            if (cp_in(c, base, tmp, chunk) < 0)
                return total ? (int)total : -EFAULT;
            r = fs_file_write(f, tmp, chunk);
        } else {
            r = fs_file_read(f, tmp, chunk);
            if (r > 0 && cp_out(c, base, tmp, (uint32_t)r) < 0)
                return total ? (int)total : -EFAULT;
        }
        if (r < 0)
            return total ? (int)total : block_or(c, f, r);
        total += r;
        if ((uint32_t)r < chunk)
            break;
    }
    return (int)total;
}

static int do_stat(call_t *c, gaddr_t pathp, gaddr_t buf, bool follow, bool is64)
{
    char path[FS_PATH_MAX];
    int r = get_path(c, pathp, path);
    if (r < 0)
        return r;
    fs_stat_t st;
    r = fs_stat(c->s->fs, &c->t->fs->ctx, path, follow, &st);
    if (r < 0)
        return r;
    return is64 ? write_stat64(c, buf, &st) : write_oldstat(c, buf, &st);
}

static int do_fstat(call_t *c, int fd, gaddr_t buf, bool is64)
{
    fs_file_t *f = fdget(c, fd);
    if (!f)
        return -EBADF;
    fs_stat_t st;
    int r = fs_file_fstat(f, &st);
    if (r < 0)
        return r;
    return is64 ? write_stat64(c, buf, &st) : write_oldstat(c, buf, &st);
}

static int do_getdents(call_t *c, int fd, gaddr_t dirp, uint32_t count, bool is64)
{
    fs_file_t *f = fdget(c, fd);
    if (!f)
        return -EBADF;
    if (!f->is_dir)
        return -ENOTDIR;
    uint32_t written = 0;
    fs_dirent_t d;
    for (;;) {
        int64_t saved = f->pos;
        int r = fs_file_readdir(f, &d);
        if (r < 0)
            return written ? (int)written : r;
        if (r == 0)
            break;
        size_t nl = strlen(d.name);
        uint32_t reclen = is64 ? (uint32_t)((19 + nl + 1 + 7) & ~7u) : (uint32_t)((10 + nl + 2 + 3) & ~3u);
        if (written + reclen > count) {
            f->pos = saved; /* does not fit: leave it for the next call */
            if (written == 0)
                return -EINVAL;
            break;
        }
        uint8_t rec[300];
        memset(rec, 0, reclen);
        if (is64) {
            put64(rec + 0, d.ino);
            put64(rec + 8, d.off);
            put16(rec + 16, (uint16_t)reclen);
            rec[18] = d.type;
            memcpy(rec + 19, d.name, nl);
        } else {
            put32(rec + 0, (uint32_t)d.ino);
            put32(rec + 4, (uint32_t)d.off);
            put16(rec + 8, (uint16_t)reclen);
            memcpy(rec + 10, d.name, nl);
            rec[reclen - 1] = d.type;
        }
        if (cp_out(c, dirp + written, rec, reclen) < 0)
            return -EFAULT;
        written += reclen;
    }
    return (int)written;
}

static int do_fcntl(call_t *c, int fd, uint32_t cmd, uint32_t arg)
{
    fs_file_t *f = fdget(c, fd);
    if (!f)
        return -EBADF;
    switch (cmd) {
    case 0: /* F_DUPFD */
    case 1030: { /* F_DUPFD_CLOEXEC */
        if (arg >= (uint32_t)fs_fdt_max(c->t->fdt))
            return -EINVAL;
        return fs_fdt_install(c->t->fdt, f, cmd == 1030, (int)arg);
    }
    case 1: return fs_fdt_get_cloexec(c->t->fdt, fd);
    case 2: return fs_fdt_set_cloexec(c->t->fdt, fd, (arg & 1) != 0);
    case 3: return f->gflags & (GUEST_O_ACCMODE | GUEST_O_APPEND | GUEST_O_NONBLOCK | GUEST_O_SYNC | GUEST_O_LARGEFILE | GUEST_O_DIRECTORY);
    case 4:
        f->gflags = (f->gflags & ~(GUEST_O_APPEND | GUEST_O_NONBLOCK | GUEST_O_SYNC)) |
                    ((int)arg & (GUEST_O_APPEND | GUEST_O_NONBLOCK | GUEST_O_SYNC));
        return 0;
    case 5: case 6: case 7: case 12: case 13: case 14: /* locks: no contention in a single guest */
        if (cmd == 5 || cmd == 12) {
            /* F_GETLK: report unlocked (l_type = F_UNLCK = 2 at offset 0) */
            uint16_t unl = 2;
            return cp_out(c, arg, &unl, 2);
        }
        return 0;
    case 8: case 9: return 0; /* F_SETOWN / F_GETOWN */
    default: return -EINVAL;
    }
}

static int do_ioctl(call_t *c, int fd, uint32_t req, gaddr_t arg)
{
    fs_file_t *f = fdget(c, fd);
    if (!f)
        return -EBADF;
    switch (req) {
    case 0x5421: { /* FIONBIO */
        uint32_t v;
        if (get_u32(c, arg, &v) < 0)
            return -EFAULT;
        if (v) f->gflags |= GUEST_O_NONBLOCK; else f->gflags &= ~GUEST_O_NONBLOCK;
        return 0;
    }
    case 0x5452: { /* FIOCLEX / FIONCLEX via 0x5451 */
        return fs_fdt_set_cloexec(c->t->fdt, fd, true);
    }
    case 0x5450:
        return fs_fdt_set_cloexec(c->t->fdt, fd, false);
    default:
        return fs_file_ioctl(f, req, c->m, arg);
    }
}

static int do_poll(call_t *c, gaddr_t fds, uint32_t nfds, int32_t timeout_ms)
{
    if (nfds > 1024)
        return -EINVAL;
    uint8_t *buf = malloc(nfds * 8 + 1);
    if (!buf)
        return -ENOMEM;
    if (cp_in(c, fds, buf, nfds * 8) < 0) {
        free(buf);
        return -EFAULT;
    }
    int ready = 0;
    for (uint32_t i = 0; i < nfds; i++) {
        int32_t fd;
        int16_t events;
        memcpy(&fd, buf + i * 8, 4);
        memcpy(&events, buf + i * 8 + 4, 2);
        int16_t rev = 0;
        if (fd >= 0) {
            fs_file_t *f = fdget(c, fd);
            rev = f ? (int16_t)fs_file_poll(f, events | GUEST_POLLHUP | GUEST_POLLERR) : (int16_t)GUEST_POLLNVAL;
        }
        memcpy(buf + i * 8 + 6, &rev, 2);
        if (rev)
            ready++;
    }
    bool expired = false;
    if (!ready && timeout_ms != 0) {
        uint64_t now = gsys_now_ns(c->s);
        if (!c->retry || !c->t->has_deadline) {
            if (timeout_ms > 0)
                gtask_set_deadline(c->t, jiffy_deadline(now, (uint64_t)timeout_ms * 1000000ull));
            else
                c->t->has_deadline = false;
        }
        if (timeout_ms > 0 && now >= c->t->deadline_ns)
            expired = true;
        if (!expired) {
            free(buf);
            gtask_block(c->t, BLK_POLL);
            return SYS_BLOCK;
        }
    }
    int r = cp_out(c, fds, buf, nfds * 8);
    free(buf);
    c->t->has_deadline = false;
    return r < 0 ? r : ready;
}

static int do_select(call_t *c, int n, gaddr_t rp, gaddr_t wp, gaddr_t ep, gaddr_t tvp)
{
    if (n < 0 || n > 1024)
        return -EINVAL;
    uint32_t words = (uint32_t)(n + 31) / 32;
    uint32_t rs[32] = { 0 }, ws[32] = { 0 }, es[32] = { 0 };
    if (rp && cp_in(c, rp, rs, words * 4) < 0) return -EFAULT;
    if (wp && cp_in(c, wp, ws, words * 4) < 0) return -EFAULT;
    if (ep && cp_in(c, ep, es, words * 4) < 0) return -EFAULT;
    uint32_t ro[32] = { 0 }, wo[32] = { 0 }, eo[32] = { 0 };
    int ready = 0;
    for (int fd = 0; fd < n; fd++) {
        uint32_t bit = 1u << (fd & 31), w = (uint32_t)fd >> 5;
        int want = (rs[w] & bit ? GUEST_POLLIN : 0) | (ws[w] & bit ? GUEST_POLLOUT : 0) | (es[w] & bit ? GUEST_POLLPRI : 0);
        if (!want)
            continue;
        fs_file_t *f = fdget(c, fd);
        if (!f)
            return -EBADF;
        int rev = fs_file_poll(f, want | GUEST_POLLHUP | GUEST_POLLERR);
        if ((rs[w] & bit) && (rev & (GUEST_POLLIN | GUEST_POLLHUP | GUEST_POLLERR))) { ro[w] |= bit; ready++; }
        if ((ws[w] & bit) && (rev & (GUEST_POLLOUT | GUEST_POLLERR))) { wo[w] |= bit; ready++; }
        if ((es[w] & bit) && (rev & GUEST_POLLPRI)) { eo[w] |= bit; ready++; }
    }
    int32_t timeout_ms = -1;
    if (tvp) {
        uint32_t sec, usec;
        if (get_u32(c, tvp, &sec) < 0 || get_u32(c, tvp + 4, &usec) < 0)
            return -EFAULT;
        timeout_ms = (int32_t)(sec * 1000 + usec / 1000);
    }
    if (!ready && timeout_ms != 0) {
        uint64_t now = gsys_now_ns(c->s);
        if (!c->retry || !c->t->has_deadline) {
            if (timeout_ms > 0)
                gtask_set_deadline(c->t, jiffy_deadline(now, (uint64_t)timeout_ms * 1000000ull));
            else
                c->t->has_deadline = false;
        }
        if (!(timeout_ms > 0 && now >= c->t->deadline_ns)) {
            gtask_block(c->t, BLK_POLL);
            return SYS_BLOCK;
        }
    }
    c->t->has_deadline = false;
    if (rp && cp_out(c, rp, ro, words * 4) < 0) return -EFAULT;
    if (wp && cp_out(c, wp, wo, words * 4) < 0) return -EFAULT;
    if (ep && cp_out(c, ep, eo, words * 4) < 0) return -EFAULT;
    if (tvp) { put_u32(c, tvp, 0); put_u32(c, tvp + 4, 0); }
    return ready;
}

static int do_mount(call_t *c, gaddr_t srcp, gaddr_t tgtp, gaddr_t typep, uint32_t flags, gaddr_t datap)
{
    char src[FS_PATH_MAX], tgt[FS_PATH_MAX], type[64] = "", data[256] = "";
    if (get_path(c, srcp, src) < 0 || get_path(c, tgtp, tgt) < 0)
        return -EFAULT;
    if (typep && get_str(c, typep, type, sizeof type) < 0)
        return -EFAULT;
    if (datap && get_str(c, datap, data, sizeof data) < 0)
        data[0] = 0;
    char abs[FS_PATH_MAX];
    int r = fs_resolve(c->s->fs, &c->t->fs->ctx, tgt, true, abs);
    if (r < 0)
        return r;
    fs_stat_t st;
    if (fs_stat(c->s->fs, &c->t->fs->ctx, abs, true, &st) < 0)
        return -ENOENT;
    if (!S_ISDIR(st.mode))
        return -ENOTDIR;
    if (flags & 0x20 /* MS_REMOUNT */) {
        gp_info("mount: remount %s (%s): accepted as a no-op", abs, type);
        return 0;
    }
    if (!strcmp(type, "tmpfs") || !strcmp(type, "ramfs")) {
        char dir[FS_PATH_MAX];
        snprintf(dir, sizeof dir, "%s/tmpfs-XXXXXX", c->s->cfg.scratch_dir ? c->s->cfg.scratch_dir : "/tmp");
        if (!mkdtemp(dir))
            return -ENOMEM;
        fs_backend_t *b = fs_backend_hostdir(dir, false, 0x20 + (uint64_t)(c->s->syscalls & 0xFF));
        if (!b)
            return -ENOMEM;
        fs_statfs_t sf = { .type = 0x01021994, .bsize = 4096, .frsize = 4096, .blocks = 4096, .bfree = 4000, .bavail = 4000, .namelen = 255 };
        fs_hostdir_set_statfs(b, &sf);
        char opts[300];
        snprintf(opts, sizeof opts, "rw%s%s", data[0] ? "," : "", data);
        r = fs_mount(c->s->fs, abs, b, "none", "tmpfs", opts);
        gp_info("mount: tmpfs on %s (%s) -> host %s", abs, opts, dir);
        return r;
    }
    gp_info("mount: %s on %s type %s flags %#x: accepted as a no-op", src, abs, type, flags);
    return 0;
}

static int do_umount(call_t *c, gaddr_t tgtp)
{
    char tgt[FS_PATH_MAX], abs[FS_PATH_MAX];
    if (get_path(c, tgtp, tgt) < 0)
        return -EFAULT;
    int r = fs_resolve(c->s->fs, &c->t->fs->ctx, tgt, true, abs);
    if (r < 0)
        return r;
    return fs_umount(c->s->fs, abs);
}

/* ---- signals ----------------------------------------------------------- */

static int do_rt_sigaction(call_t *c, int sig, gaddr_t actp, gaddr_t oldp, uint32_t setsize, bool old_abi)
{
    if (sig < 1 || sig > NSIG_GUEST || (!old_abi && setsize != 8))
        return -EINVAL;
    gsigact_t *cur = &c->t->sighand->act[sig];
    if (oldp) {
        uint8_t b[20];
        memset(b, 0, sizeof b);
        if (old_abi) {
            put32(b + 0, cur->handler);
            put32(b + 4, (uint32_t)cur->mask);
            put32(b + 8, cur->flags);
            put32(b + 12, cur->restorer);
            if (cp_out(c, oldp, b, 16) < 0)
                return -EFAULT;
        } else {
            put32(b + 0, cur->handler);
            put32(b + 4, cur->flags);
            put32(b + 8, cur->restorer);
            put64(b + 12, cur->mask);
            if (cp_out(c, oldp, b, 20) < 0)
                return -EFAULT;
        }
    }
    if (actp) {
        if (sig == GSIGKILL || sig == GSIGSTOP)
            return -EINVAL;
        uint8_t b[20];
        gsigact_t na;
        if (old_abi) {
            if (cp_in(c, actp, b, 16) < 0)
                return -EFAULT;
            memcpy(&na.handler, b, 4);
            uint32_t m;
            memcpy(&m, b + 4, 4);
            na.mask = m;
            memcpy(&na.flags, b + 8, 4);
            memcpy(&na.restorer, b + 12, 4);
        } else {
            if (cp_in(c, actp, b, 20) < 0)
                return -EFAULT;
            memcpy(&na.handler, b, 4);
            memcpy(&na.flags, b + 4, 4);
            memcpy(&na.restorer, b + 8, 4);
            memcpy(&na.mask, b + 12, 8);
        }
        *cur = na;
        if (na.handler == 1)
            c->t->sigpending &= ~(1ull << (sig - 1));
        gp_trace(GP_TRACE_SYSCALL, "pid %d: sigaction(%d) handler %08x flags %08x restorer %08x", c->t->pid, sig,
                 na.handler, na.flags, na.restorer);
    }
    return 0;
}

static int do_sigprocmask(call_t *c, int how, gaddr_t setp, gaddr_t oldp, bool rt)
{
    uint64_t old = c->t->sigblocked;
    if (setp) {
        uint64_t set = 0;
        if (rt) {
            if (cp_in(c, setp, &set, 8) < 0)
                return -EFAULT;
        } else {
            uint32_t s32;
            if (get_u32(c, setp, &s32) < 0)
                return -EFAULT;
            set = s32;
        }
        switch (how) {
        case 0: c->t->sigblocked |= set; break;
        case 1: c->t->sigblocked &= ~set; break;
        case 2: c->t->sigblocked = set; break;
        default: return -EINVAL;
        }
        c->t->sigblocked &= ~((1ull << (GSIGKILL - 1)) | (1ull << (GSIGSTOP - 1)));
    }
    if (oldp) {
        if (rt) {
            if (cp_out(c, oldp, &old, 8) < 0)
                return -EFAULT;
        } else if (put_u32(c, oldp, (uint32_t)old) < 0) {
            return -EFAULT;
        }
    }
    return 0;
}

static int do_sigsuspend(call_t *c, uint64_t mask)
{
    if (!c->retry) {
        c->t->suspend_oldmask = c->t->sigblocked;
        c->t->sigblocked = mask & ~((1ull << (GSIGKILL - 1)) | (1ull << (GSIGSTOP - 1)));
    }
    gtask_block(c->t, BLK_SUSPEND);
    return SYS_BLOCK;
}

/* ---- time -------------------------------------------------------------- */

static int do_nanosleep(call_t *c, gaddr_t reqp, gaddr_t remp)
{
    uint32_t sec, nsec;
    if (get_u32(c, reqp, &sec) < 0 || get_u32(c, reqp + 4, &nsec) < 0)
        return -EFAULT;
    if (nsec >= 1000000000u || (int32_t)sec < 0)
        return -EINVAL;
    if (!gpdev_sleeps_are_real(c->s->dev)) {
        /* deterministic modes: time is counted, not waited for (spec #23).
         * The scheduler lets virtual time pass only if every task does
         * nothing but sleep (a guest wait gated on TCOUNT would otherwise
         * spin forever, since nobody reads the timer). */
        c->t->slice_only_slept = true;
        c->t->sleep_req_ns = (uint64_t)sec * 1000000000ull + nsec;
        return 0;
    }
    uint64_t now = gsys_now_ns(c->s);
    if (!c->retry || !c->t->has_deadline)
        gtask_set_deadline(c->t, jiffy_deadline(now, (uint64_t)sec * 1000000000ull + nsec));
    if (now >= c->t->deadline_ns) {
        c->t->has_deadline = false;
        if (remp) { put_u32(c, remp, 0); put_u32(c, remp + 4, 0); }
        return 0;
    }
    gtask_block(c->t, BLK_SLEEP);
    return SYS_BLOCK;
}

/* ---- process ----------------------------------------------------------- */

static int do_execve(call_t *c, gaddr_t pathp, gaddr_t argvp, gaddr_t envpp)
{
    char path[FS_PATH_MAX];
    int r = get_path(c, pathp, path);
    if (r < 0)
        return r;
    char **argv = NULL, **envp = NULL;
    r = get_strv(c, argvp, &argv);
    if (r < 0)
        return r;
    r = get_strv(c, envpp, &envp);
    if (r < 0) {
        free_strv(argv);
        return r;
    }
    gp_trace(GP_TRACE_SYSCALL, "pid %d: execve(%s, [%s%s])", c->t->pid, path, argv[0] ? argv[0] : "",
             argv[0] && argv[1] ? ", ..." : "");
    r = gtask_execve(c->t, path, argv, envp);
    free_strv(argv);
    free_strv(envp);
    if (r < 0)
        return r;
    c->m = c->t->mm->mem; /* the task now has a new image and cpu */
    c->r = cpu_regs(c->t->cpu);
    return SYS_NORETURN;
}

static int do_clone(call_t *c, uint32_t flags, gaddr_t newsp, gaddr_t ptid, gaddr_t tls, gaddr_t ctid)
{
    gtask_t *child;
    int r = gtask_clone_thread(c->t, flags, newsp, &child);
    if (r < 0)
        return r;
    if ((flags & 0x80000 /* CLONE_SETTLS */))
        child->tls = tls;
    if ((flags & 0x100000 /* CLONE_PARENT_SETTID */) && ptid)
        put_u32(c, ptid, (uint32_t)child->pid);
    if ((flags & 0x01000000 /* CLONE_CHILD_SETTID */) && ctid)
        gmem_st32(child->mm->mem, ctid, (uint32_t)child->pid);
    return child->pid;
}

static int do_wait4(call_t *c, int pid, gaddr_t status, int options, gaddr_t rusage)
{
    int r = gtask_wait4(c->t, pid, options, status, rusage);
    return r == 1 ? SYS_BLOCK : r;
}

static int do_uname(call_t *c, gaddr_t buf)
{
    char u[6][65];
    memset(u, 0, sizeof u);
    snprintf(u[0], 65, "Linux");
    snprintf(u[1], 65, "%s", c->s->hostname);
    snprintf(u[2], 65, "%s", c->s->cfg.uname_release);
    snprintf(u[3], 65, "%s", c->s->cfg.uname_version);
    snprintf(u[4], 65, "%s", c->s->cfg.uname_machine);
    snprintf(u[5], 65, "(none)");
    return cp_out(c, buf, u, sizeof u);
}

static int do_getrlimit(call_t *c, uint32_t res, gaddr_t buf, bool old)
{
    if (res >= RLIM_NLIMITS)
        return -EINVAL;
    uint32_t cur = c->t->rlim[res].cur, max = c->t->rlim[res].max;
    if (old) {
        if (cur == GUEST_RLIM_INFINITY) cur = 0x7FFFFFFF;
        if (max == GUEST_RLIM_INFINITY) max = 0x7FFFFFFF;
    }
    uint32_t v[2] = { cur, max };
    return cp_out(c, buf, v, 8);
}

static int do_setrlimit(call_t *c, uint32_t res, gaddr_t buf)
{
    if (res >= RLIM_NLIMITS)
        return -EINVAL;
    uint32_t v[2];
    if (cp_in(c, buf, v, 8) < 0)
        return -EFAULT;
    if (v[0] > v[1] && v[1] != GUEST_RLIM_INFINITY)
        return -EINVAL;
    c->t->rlim[res].cur = v[0];
    c->t->rlim[res].max = v[1];
    return 0;
}

/* ---- names for the trace ----------------------------------------------- */

static const char *sysname(uint32_t nr)
{
    static const struct { uint32_t nr; const char *name; } names[] = {
        {1,"exit"},{2,"fork"},{3,"read"},{4,"write"},{5,"open"},{6,"close"},{7,"waitpid"},{10,"unlink"},{11,"execve"},
        {12,"chdir"},{13,"time"},{14,"mknod"},{15,"chmod"},{19,"lseek"},{20,"getpid"},{21,"mount"},{22,"umount"},
        {24,"getuid"},{27,"alarm"},{29,"pause"},{30,"utime"},{33,"access"},{34,"nice"},{36,"sync"},{37,"kill"},
        {38,"rename"},{39,"mkdir"},{40,"rmdir"},{41,"dup"},{42,"pipe"},{43,"times"},{45,"brk"},{47,"getgid"},
        {49,"geteuid"},{50,"getegid"},{52,"umount2"},{54,"ioctl"},{55,"fcntl"},{57,"setpgid"},{60,"umask"},{63,"dup2"},
        {64,"getppid"},{65,"getpgrp"},{66,"setsid"},{67,"sigaction"},{72,"sigsuspend"},{73,"sigpending"},
        {74,"sethostname"},{75,"setrlimit"},{76,"getrlimit"},{77,"getrusage"},{78,"gettimeofday"},{85,"readlink"},
        {90,"mmap"},{91,"munmap"},{92,"truncate"},{93,"ftruncate"},{94,"fchmod"},{99,"statfs"},{100,"fstatfs"},
        {102,"socketcall"},{104,"setitimer"},{106,"stat"},{107,"lstat"},{108,"fstat"},{114,"wait4"},{116,"sysinfo"},
        {118,"fsync"},{119,"sigreturn"},{120,"clone"},{122,"uname"},{125,"mprotect"},{126,"sigprocmask"},
        {132,"getpgid"},{133,"fchdir"},{136,"personality"},{140,"_llseek"},{141,"getdents"},{142,"_newselect"},
        {144,"msync"},{145,"readv"},{146,"writev"},{147,"getsid"},{148,"fdatasync"},{149,"_sysctl"},
        {155,"sched_getparam"},{156,"sched_setscheduler"},{157,"sched_getscheduler"},{158,"sched_yield"},
        {159,"sched_get_priority_max"},{160,"sched_get_priority_min"},{162,"nanosleep"},{163,"mremap"},{168,"poll"},
        {172,"prctl"},{173,"rt_sigreturn"},{174,"rt_sigaction"},{175,"rt_sigprocmask"},{176,"rt_sigpending"},
        {179,"rt_sigsuspend"},{180,"pread64"},{181,"pwrite64"},{183,"getcwd"},{186,"sigaltstack"},{190,"vfork"},
        {191,"ugetrlimit"},{192,"mmap2"},{193,"truncate64"},{194,"ftruncate64"},{195,"stat64"},{196,"lstat64"},
        {197,"fstat64"},{199,"getuid32"},{200,"getgid32"},{201,"geteuid32"},{202,"getegid32"},{217,"getdents64"},
        {220,"madvise"},{221,"fcntl64"},{224,"gettid"},{238,"tkill"},{240,"futex"},{248,"exit_group"},
        {256,"set_tid_address"},{263,"clock_gettime"},{264,"clock_getres"},{266,"statfs64"},{267,"fstatfs64"},
        {268,"tgkill"},{269,"utimes"},{0xF0002,"cacheflush"},{0xF0005,"set_tls"},
    };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (names[i].nr == nr)
            return names[i].name;
    return "?";
}

/* ---- dispatch ---------------------------------------------------------- */

static int dispatch(call_t *c)
{
    gtask_t *t = c->t;
    gsys_t *s = c->s;
    uint32_t a0 = A(c, 0), a1 = A(c, 1), a2 = A(c, 2), a3 = A(c, 3), a4 = A(c, 4), a5 = A(c, 5);
    switch (c->nr) {
    /* files */
    case 3: return do_read(c, (int)a0, a1, a2);
    case 4: return do_write(c, (int)a0, a1, a2);
    case 5: return do_open(c, a0, (int)a1, a2);
    case 8: return do_open(c, a0, GUEST_O_WRONLY | GUEST_O_CREAT | GUEST_O_TRUNC, a1);
    case 6: return fs_fdt_close(t->fdt, (int)a0);
    case 9: return -EPERM;  /* link: vfat has none */
    case 83: return -EPERM; /* symlink */
    case 10: case 38: case 39: case 40: case 12: case 33: case 85: case 92: case 15: case 30: case 269: case 16: case 182: case 198: case 212: {
        char p[FS_PATH_MAX];
        int r = get_path(c, a0, p);
        if (r < 0)
            return r;
        switch (c->nr) {
        case 10: return fs_unlink(s->fs, &t->fs->ctx, p);
        case 38: {
            char q[FS_PATH_MAX];
            if ((r = get_path(c, a1, q)) < 0) return r;
            return fs_rename(s->fs, &t->fs->ctx, p, q);
        }
        case 39: return fs_mkdir(s->fs, &t->fs->ctx, p, a1);
        case 40: return fs_rmdir(s->fs, &t->fs->ctx, p);
        case 12: return fs_chdir(s->fs, &t->fs->ctx, p);
        case 33: return fs_access(s->fs, &t->fs->ctx, p, (int)a1);
        case 85: {
            char buf[FS_PATH_MAX];
            r = fs_readlink(s->fs, &t->fs->ctx, p, buf, sizeof buf);
            if (r < 0) return r;
            if ((uint32_t)r > a2) r = (int)a2;
            return cp_out(c, a1, buf, (uint32_t)r) < 0 ? -EFAULT : r;
        }
        case 92: return fs_truncate(s->fs, &t->fs->ctx, p, (int64_t)a1);
        case 15: return fs_chmod(s->fs, &t->fs->ctx, p, a1);
        default: { /* utime/utimes/chown: accepted, the card and tmpfs keep no such metadata */
            fs_stat_t st;
            return fs_stat(s->fs, &t->fs->ctx, p, true, &st);
        }
        }
    }
    case 193: { /* truncate64(path, [pad,] lo, hi) */
        char p[FS_PATH_MAX];
        int r = get_path(c, a0, p);
        if (r < 0) return r;
        uint64_t size = c->eabi ? ((uint64_t)a3 << 32 | a2) : ((uint64_t)a2 << 32 | a1);
        return fs_truncate(s->fs, &t->fs->ctx, p, (int64_t)size);
    }
    case 93: case 194: {
        fs_file_t *f = fdget(c, (int)a0);
        if (!f) return -EBADF;
        uint64_t size = c->nr == 93 ? a1 : c->eabi ? ((uint64_t)a3 << 32 | a2) : ((uint64_t)a2 << 32 | a1);
        return fs_file_ftruncate(f, (int64_t)size);
    }
    case 94: { fs_file_t *f = fdget(c, (int)a0); return f ? fs_chmod(s->fs, &t->fs->ctx, f->path, a1) : -EBADF; }
    case 95: case 207: return fdget(c, (int)a0) ? 0 : -EBADF; /* fchown */
    case 133: { /* fchdir */
        fs_file_t *f = fdget(c, (int)a0);
        if (!f) return -EBADF;
        return fs_chdir(s->fs, &t->fs->ctx, f->path);
    }
    case 19: { /* lseek */
        fs_file_t *f = fdget(c, (int)a0);
        if (!f) return -EBADF;
        int64_t r = fs_file_lseek(f, (int32_t)a1, (int)a2);
        if (r < 0) return (int)r;
        if (r > 0x7FFFFFFF) return -EOVERFLOW;
        return (int)r;
    }
    case 140: { /* _llseek(fd, hi, lo, result*, whence) */
        fs_file_t *f = fdget(c, (int)a0);
        if (!f) return -EBADF;
        int64_t off = (int64_t)(((uint64_t)a1 << 32) | a2);
        int64_t r = fs_file_lseek(f, off, (int)a4);
        if (r < 0) return (int)r;
        uint64_t res = (uint64_t)r;
        return cp_out(c, a3, &res, 8);
    }
    case 36: return 0; /* sync */
    case 118: case 148: { fs_file_t *f = fdget(c, (int)a0); return f ? fs_file_fsync(f) : -EBADF; }
    case 41: { fs_file_t *f = fdget(c, (int)a0); return f ? fs_fdt_install(t->fdt, f, false, 0) : -EBADF; }
    case 63: return fs_fdt_dup2(t->fdt, (int)a0, (int)a1);
    case 42: { /* pipe: the fds go to the user array */
        fs_file_t *rd, *wr;
        int r = fs_pipe_create(&rd, &wr);
        if (r < 0) return r;
        int fds[2];
        fds[0] = fs_fdt_install(t->fdt, rd, false, 0);
        fds[1] = fs_fdt_install(t->fdt, wr, false, 0);
        fs_file_unref(rd);
        fs_file_unref(wr);
        if (fds[0] < 0 || fds[1] < 0) return -EMFILE;
        return cp_out(c, a0, fds, 8);
    }
    case 54: return do_ioctl(c, (int)a0, a1, a2);
    case 55: case 221: return do_fcntl(c, (int)a0, a1, a2);
    case 60: { unsigned old = t->fs->ctx.umask; t->fs->ctx.umask = a0 & 0777; return (int)old; }
    case 99: case 266: {
        char p[FS_PATH_MAX];
        int r = get_path(c, a0, p);
        if (r < 0) return r;
        fs_statfs_t sf;
        r = fs_statfs(s->fs, &t->fs->ctx, p, &sf);
        if (r < 0) return r;
        return c->nr == 99 ? write_statfs(c, a1, &sf) : write_statfs64(c, a2, a1, &sf);
    }
    case 100: case 267: {
        fs_file_t *f = fdget(c, (int)a0);
        if (!f) return -EBADF;
        fs_statfs_t sf;
        int r = fs_statfs(s->fs, &t->fs->ctx, f->path, &sf);
        if (r < 0) return r;
        return c->nr == 100 ? write_statfs(c, a1, &sf) : write_statfs64(c, a2, a1, &sf);
    }
    case 106: return do_stat(c, a0, a1, true, false);
    case 107: return do_stat(c, a0, a1, false, false);
    case 108: return do_fstat(c, (int)a0, a1, false);
    case 195: return do_stat(c, a0, a1, true, true);
    case 196: return do_stat(c, a0, a1, false, true);
    case 197: return do_fstat(c, (int)a0, a1, true);
    case 141: return do_getdents(c, (int)a0, a1, a2, false);
    case 217: return do_getdents(c, (int)a0, a1, a2, true);
    case 145: return do_iov(c, (int)a0, a1, a2, false);
    case 146: return do_iov(c, (int)a0, a1, a2, true);
    case 180: case 181: {
        uint64_t off = c->eabi ? ((uint64_t)a5 << 32 | a4) : ((uint64_t)a4 << 32 | a3);
        return do_pread(c, (int)a0, a1, a2, off, c->nr == 181);
    }
    case 183: { /* getcwd: returns the length including the NUL */
        size_t len = strlen(t->fs->ctx.cwd) + 1;
        if (a1 < len) return a1 ? -ERANGE : -EINVAL;
        return cp_out(c, a0, t->fs->ctx.cwd, (uint32_t)len) < 0 ? -EFAULT : (int)len;
    }
    case 142: return do_select(c, (int)a0, a1, a2, a3, a4);
    case 168: return do_poll(c, a0, a1, (int32_t)a2);
    case 14: return 0;  /* mknod: the device tree already has every node */
    case 21: return do_mount(c, a0, a1, a2, a3, a4);
    case 22: case 52: return do_umount(c, a0);
    case 122: return do_uname(c, a0);
    case 74: { /* sethostname */
        if (a1 >= sizeof s->hostname) return -EINVAL;
        if (cp_in(c, a0, s->hostname, a1) < 0) return -EFAULT;
        s->hostname[a1] = 0;
        return 0;
    }
    case 144: case 220: case 150: case 151: case 152: case 153: return 0; /* msync, madvise, mlock... */

    /* memory */
    case 45: { gaddr_t out; gmm_brk(t, a0, &out); return (int)out; }
    case 90: { /* old mmap: six words at r0 */
        uint32_t w[6];
        if (cp_in(c, a0, w, 24) < 0) return -EFAULT;
        if (w[5] & (GP2X_PAGE_SIZE - 1)) return -EINVAL;
        gaddr_t out;
        int r = gmm_mmap(t, w[0], w[1], (int)w[2], (int)w[3], (int)w[4], w[5], &out);
        return r < 0 ? r : (int)out;
    }
    case 192: { gaddr_t out; int r = gmm_mmap(t, a0, a1, (int)a2, (int)a3, (int)a4, (uint64_t)a5 << 12, &out); return r < 0 ? r : (int)out; }
    case 91: return gmm_munmap(t, a0, a1);
    case 125: return gmm_mprotect(t, a0, a1, (int)a2);
    case 163: { gaddr_t out; int r = gmm_mremap(t, a0, a1, a2, (int)a3, a4, &out); return r < 0 ? r : (int)out; }

    /* time */
    case 13: { int64_t now = gsys_epoch_seconds(s); if (a0 && put_u32(c, a0, (uint32_t)now) < 0) return -EFAULT; return (int)now; }
    case 25: case 79: return 0; /* stime, settimeofday: root may, nothing depends on it */
    case 78: {
        uint64_t ns = gsys_epoch_ns(s);
        uint32_t tv[2] = { (uint32_t)(ns / 1000000000ull), (uint32_t)(ns % 1000000000ull / 1000) };
        if (a0 && cp_out(c, a0, tv, 8) < 0) return -EFAULT;
        if (a1) { uint32_t tz[2] = { 0, 0 }; if (cp_out(c, a1, tz, 8) < 0) return -EFAULT; }
        return 0;
    }
    case 263: {
        uint64_t ns = a0 == 0 ? gsys_epoch_ns(s) : gsys_now_ns(s);
        uint32_t ts[2] = { (uint32_t)(ns / 1000000000ull), (uint32_t)(ns % 1000000000ull) };
        return cp_out(c, a1, ts, 8);
    }
    case 264: { uint32_t ts[2] = { 0, 1 }; return a1 ? cp_out(c, a1, ts, 8) : 0; }
    case 262: return 0;
    case 162: return do_nanosleep(c, a0, a1);
    case 43: { /* times */
        uint32_t ticks = (uint32_t)(gsys_now_ns(s) / 10000000ull);
        uint32_t tms[4] = { (uint32_t)(t->insns_run / 2000000ull), 0, 0, 0 };
        if (a0 && cp_out(c, a0, tms, 16) < 0) return -EFAULT;
        return (int)ticks;
    }
    case 27: case 104: case 105: return 0; /* alarm, setitimer, getitimer: no timers reach the guest in the traces */
    case 116: { /* sysinfo */
        uint8_t b[64];
        memset(b, 0, sizeof b);
        put32(b + 0, (uint32_t)(gsys_now_ns(s) / 1000000000ull));
        put32(b + 16, 32u << 20); put32(b + 20, 16u << 20);
        put16(b + 40, (uint16_t)s->ntasks);
        put32(b + 52, 1);
        return cp_out(c, a0, b, 64);
    }

    /* process and identity */
    case 1: gtask_exit(t, (int)((a0 & 0xFF) << 8)); return SYS_NORETURN;
    case 248: gtask_exit(t, (int)((a0 & 0xFF) << 8)); return SYS_NORETURN;
    case 2: case 190: { gtask_t *child; int r = gtask_fork(t, c->nr == 190, &child); return r < 0 ? r : child->pid; }
    case 120: return do_clone(c, a0, a1, a2, a3, a4);
    case 11: return do_execve(c, a0, a1, a2);
    case 7: return do_wait4(c, (int)a0, a1, (int)a2, 0);
    case 114: return do_wait4(c, (int)a0, a1, (int)a2, a3);
    case 20: return t->pid;
    case 224: return t->pid;
    case 64: return t->ppid;
    case 65: return t->pgid;
    case 132: { gtask_t *o = a0 ? gsys_find_task(s, (int)a0) : t; return o ? o->pgid : -ESRCH; }
    case 147: { gtask_t *o = a0 ? gsys_find_task(s, (int)a0) : t; return o ? o->sid : -ESRCH; }
    case 57: { gtask_t *o = a0 ? gsys_find_task(s, (int)a0) : t; if (!o) return -ESRCH; o->pgid = a1 ? (int)a1 : o->pid; return 0; }
    case 66: t->pgid = t->sid = t->pid; return t->pid;
    case 24: case 47: case 49: case 50: case 199: case 200: case 201: case 202: return 0; /* root */
    case 23: case 46: case 70: case 71: case 138: case 139: case 164: case 170: case 203: case 204: case 208: case 210: case 213: case 214: case 215: case 216: return 0;
    case 80: case 205: return 0; /* getgroups: none */
    case 81: case 206: return 0;
    case 165: case 209: case 171: case 211: { /* getresuid/gid: three zeros */
        if (put_u32(c, a0, 0) < 0 || put_u32(c, a1, 0) < 0 || put_u32(c, a2, 0) < 0) return -EFAULT;
        return 0;
    }
    case 37: return gsys_kill(s, t, (int)a0, (int)a1);
    case 238: { gtask_t *o = gsys_find_task(s, (int)a0); return o ? gtask_send_signal(o, (int)a1, t->pid) : -ESRCH; }
    case 268: { gtask_t *o = gsys_find_task(s, (int)a1); return o ? gtask_send_signal(o, (int)a2, t->pid) : -ESRCH; }
    case 34: case 97: return 0;
    case 96: return 20; /* getpriority: nice 0 is reported as 20 */
    case 154: case 156: return 0;
    case 155: return put_u32(c, a1, 0);
    case 157: return 0; /* SCHED_OTHER */
    case 158: return 0; /* sched_yield: the slice ends at the syscall anyway */
    case 159: return (a0 == 1 || a0 == 2) ? 99 : 0;
    case 160: return (a0 == 1 || a0 == 2) ? 1 : 0;
    case 161: { uint32_t ts[2] = { 0, 10000000 }; return cp_out(c, a1, ts, 8); }
    case 136: return 0; /* personality: PER_LINUX */
    case 172: return 0; /* prctl */
    case 184: case 185: return 0; /* capget/capset */
    case 75: return do_setrlimit(c, a0, a1);
    case 76: return do_getrlimit(c, a0, a1, true);
    case 191: return do_getrlimit(c, a0, a1, false);
    case 77: return gmem_memset(c->m, a1, 0, 72) == GP_OK ? 0 : -EFAULT;
    case 149: return -ENOSYS; /* _sysctl: glibc falls back to /proc (spec 2.2) */
    case 128: case 129: case 167: return -ENOSYS; /* init_module, delete_module, query_module: no 2.4 kernel modules here */
    case 103: return 0;
    case 102: case 117: return -ENOSYS; /* socketcall, ipc */
    case 240: return -ENOSYS; /* futex: LinuxThreads does not use it */
    case 256: return t->pid;
    case 61: return -EPERM;
    case 111: case 88: return 0;

    /* signals */
    case 67: return do_rt_sigaction(c, (int)a0, a1, a2, 0, true);
    case 174: return do_rt_sigaction(c, (int)a0, a1, a2, a3, false);
    case 126: return do_sigprocmask(c, (int)a0, a1, a2, false);
    case 175: if (a3 != 8) return -EINVAL; return do_sigprocmask(c, (int)a0, a1, a2, true);
    case 73: return put_u32(c, a0, (uint32_t)(t->sigpending & t->sigblocked));
    case 176: { uint64_t p = t->sigpending & t->sigblocked; return cp_out(c, a0, &p, 8); }
    case 72: return do_sigsuspend(c, a2);
    case 179: { uint64_t m; if (cp_in(c, a0, &m, 8) < 0) return -EFAULT; return do_sigsuspend(c, m); }
    case 29: return do_sigsuspend(c, t->sigblocked);
    case 119: case 173:
        if (gtask_sigreturn(t) < 0) { gp_warn("pid %d: sigreturn without a frame", t->pid); gtask_exit(t, GSIGSEGV); }
        return SYS_NORETURN;
    case 186: { /* sigaltstack */
        if (a1) {
            uint32_t v[3] = { t->altstack_sp, (uint32_t)(t->altstack_size ? t->altstack_flags : 2), t->altstack_size };
            if (cp_out(c, a1, v, 12) < 0) return -EFAULT;
        }
        if (a0) {
            uint32_t v[3];
            if (cp_in(c, a0, v, 12) < 0) return -EFAULT;
            if (v[1] & 2) { t->altstack_sp = 0; t->altstack_size = 0; }
            else { if (v[2] < 2048) return -ENOMEM; t->altstack_sp = v[0]; t->altstack_size = v[2]; t->altstack_flags = 0; }
        }
        return 0;
    }
    case 177: return -ENOSYS;
    case 178: { gtask_t *o = gsys_find_task(s, (int)a0); return o ? gtask_send_signal(o, (int)a1, t->pid) : -ESRCH; }

    /* ARM private */
    case 0xF0001: gtask_send_signal(t, GSIGTRAP, t->pid); return 0;
    case 0xF0002: {
        bool flip = gpdev_cacheflush(s->dev, a0, a1);
        gp_trace(GP_TRACE_SYSCALL, "pid %d: cacheflush(%08x, %08x)%s", t->pid, a0, a1, flip ? " [flip]" : "");
        return 0;
    }
    case 0xF0003: case 0xF0004: return 0;
    case 0xF0005: t->tls = a0; return 0;
    case 0xF0006: return (int)t->tls;
    default:
        return -ENOSYS;
    }
}

static void run_call(gtask_t *t, uint32_t imm, bool retry)
{
    call_t c;
    c.t = t;
    c.s = t->sys;
    c.m = t->mm->mem;
    c.r = cpu_regs(t->cpu);
    c.imm = imm;
    c.retry = retry;
    c.eabi = imm == 0;
    if (imm == 0)
        c.nr = c.r->r[7];
    else if ((imm & 0xFF0000) == 0x9F0000)
        c.nr = 0xF0000 + (imm & 0xFFFF);
    else
        c.nr = imm - 0x900000;
    if (c.nr >= 0xF0000 && c.eabi && c.r->r[7] >= 0xF0000)
        c.nr = c.r->r[7];
    uint32_t a0 = A(&c, 0), a1 = A(&c, 1), a2 = A(&c, 2), a3 = A(&c, 3);
    char pathbuf[FS_PATH_MAX] = "";
    bool has_path = false;
    if (gp_log_trace_enabled(GP_TRACE_SYSCALL)) {
        if (c.nr == 162) { /* nanosleep: show the requested time */
            uint32_t sec = 0, nsec = 0;
            get_u32(&c, a0, &sec);
            get_u32(&c, a0 + 4, &nsec);
            snprintf(pathbuf, sizeof pathbuf, "%u.%09us", sec, nsec);
            has_path = true;
        }
        switch (c.nr) {
        case 5: case 8: case 10: case 11: case 12: case 33: case 38: case 39: case 40: case 85: case 92: case 99:
        case 106: case 107: case 195: case 196: case 193: case 266: case 21: case 22: case 52: case 14: case 15: case 30: case 269:
            has_path = get_str(&c, a0, pathbuf, sizeof pathbuf) >= 0;
            break;
        default: break;
        }
    }
    int r = dispatch(&c);
    if (r == SYS_BLOCK) {
        t->pending_svc = imm;
        if (!retry)
            gp_trace(GP_TRACE_SYSCALL, "pid %d: %s(%x, %x, %x, %x) blocks", t->pid, sysname(c.nr), a0, a1, a2, a3);
        return;
    }
    if (t->state == TASK_BLOCKED) {
        t->state = TASK_RUNNING;
        t->block = BLK_NONE;
    }
    t->has_deadline = false;
    if (r == SYS_NORETURN) {
        gp_trace(GP_TRACE_SYSCALL, "pid %d: %s(%x, %x, %x, %x) [no return]", t->pid, sysname(c.nr), a0, a1, a2, a3);
        return;
    }
    if (t->state == TASK_ZOMBIE || t->state == TASK_DEAD)
        return;
    c.r->r[0] = (uint32_t)r;
    if (r == -ENOSYS && c.nr != 149 && c.nr != 102 && c.nr != 240 && c.nr != 117 && c.nr != 128 && c.nr != 129 && c.nr != 167)
        gp_warn("pid %d: unimplemented syscall %u (%s) -> ENOSYS", t->pid, c.nr, sysname(c.nr));
    if (gp_log_trace_enabled(GP_TRACE_SYSCALL)) {
        char args[FS_PATH_MAX + 64];
        if (has_path)
            snprintf(args, sizeof args, "\"%s\", %x, %x, %x", pathbuf, a1, a2, a3);
        else
            snprintf(args, sizeof args, "%x, %x, %x, %x", a0, a1, a2, a3);
        if (r < 0 && r > -4096)
            gp_trace(GP_TRACE_SYSCALL, "pid %d: %s(%s) = -%d (%s)", t->pid, sysname(c.nr), args, -r, strerror(-r));
        else
            gp_trace(GP_TRACE_SYSCALL, "pid %d: %s(%s) = %#x", t->pid, sysname(c.nr), args, (unsigned)r);
    }
}

void sys_dispatch(gtask_t *t, uint32_t imm)
{
    run_call(t, imm, false);
}

bool sys_retry(gtask_t *t)
{
    if (t->state != TASK_BLOCKED)
        return true;
    run_call(t, t->pending_svc, true);
    return t->state != TASK_BLOCKED;
}
