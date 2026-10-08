/* The /dev tree (spec 3.1): registered device nodes with their open
 * functions, implicit directories, symlinks, and the generic devices
 * (null, zero, urandom, the console ttys). */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "gport2x/fs.h"

typedef struct devnode {
    char rel[256];
    uint32_t mode;
    uint64_t rdev;
    fs_devopen_fn open;
    void *ctx;
    char target[256];
} devnode_t;

typedef struct devfs {
    devnode_t *nodes;
    size_t n, cap;
    FILE *console;
} devfs_t;

static devnode_t *find(devfs_t *d, const char *rel)
{
    for (size_t i = 0; i < d->n; i++)
        if (!strcmp(d->nodes[i].rel, rel))
            return &d->nodes[i];
    return NULL;
}

static devnode_t *add(devfs_t *d, const char *rel)
{
    devnode_t *n = find(d, rel);
    if (n)
        return n;
    if (d->n == d->cap) {
        size_t ncap = d->cap ? d->cap * 2 : 32;
        devnode_t *nn = realloc(d->nodes, ncap * sizeof *nn);
        if (!nn)
            return NULL;
        d->nodes = nn;
        d->cap = ncap;
    }
    n = &d->nodes[d->n++];
    memset(n, 0, sizeof *n);
    snprintf(n->rel, sizeof n->rel, "%s", rel);
    return n;
}

static int ensure_parents(devfs_t *d, const char *rel)
{
    char prefix[256];
    const char *p = rel;
    for (;;) {
        const char *e = strchr(p, '/');
        if (!e)
            return 0;
        size_t plen = (size_t)(e - rel);
        memcpy(prefix, rel, plen);
        prefix[plen] = 0;
        devnode_t *n = find(d, prefix);
        if (!n) {
            n = add(d, prefix);
            if (!n)
                return -ENOMEM;
            n->mode = S_IFDIR | 0755;
        }
        p = e + 1;
    }
}

int fs_devfs_register(fs_backend_t *devfs, const char *rel, uint32_t mode, uint64_t rdev, fs_devopen_fn open, void *ctx)
{
    devfs_t *d = devfs->priv;
    int r = ensure_parents(d, rel);
    if (r < 0)
        return r;
    devnode_t *n = add(d, rel);
    if (!n)
        return -ENOMEM;
    n->mode = mode;
    n->rdev = rdev;
    n->open = open;
    n->ctx = ctx;
    n->target[0] = 0;
    return 0;
}

int fs_devfs_symlink(fs_backend_t *devfs, const char *rel, const char *target)
{
    devfs_t *d = devfs->priv;
    int r = ensure_parents(d, rel);
    if (r < 0)
        return r;
    devnode_t *n = add(d, rel);
    if (!n)
        return -ENOMEM;
    n->mode = S_IFLNK | 0777;
    snprintf(n->target, sizeof n->target, "%s", target);
    return 0;
}

static int df_stat(fs_backend_t *b, const char *rel, fs_stat_t *st)
{
    devfs_t *d = b->priv;
    memset(st, 0, sizeof *st);
    st->dev = b->dev;
    st->nlink = 1;
    st->blksize = 4096;
    if (!*rel) {
        st->mode = S_IFDIR | 0755;
        st->ino = 1;
        return 0;
    }
    devnode_t *n = find(d, rel);
    if (!n)
        return -ENOENT;
    st->mode = n->mode;
    st->rdev = n->rdev;
    st->ino = (uint64_t)(n - d->nodes) + 2;
    return 0;
}

static int df_readlink(fs_backend_t *b, const char *rel, char *buf, size_t n)
{
    devnode_t *node = find(b->priv, rel);
    if (!node || !S_ISLNK(node->mode))
        return -EINVAL;
    size_t len = strlen(node->target);
    if (len > n)
        len = n;
    memcpy(buf, node->target, len);
    return (int)len;
}

static int df_open(fs_backend_t *b, const char *rel, int gflags, uint32_t mode, fs_file_t **out)
{
    (void)mode;
    devnode_t *n = find(b->priv, rel);
    if (!n)
        return -ENOENT;
    if (S_ISDIR(n->mode))
        return -EISDIR;
    if (!n->open)
        return -ENXIO;
    int r = n->open(n->ctx, gflags, out);
    if (r == 0)
        (*out)->backend = b;
    return r;
}

static int df_readdir(fs_backend_t *b, const char *rel, uint64_t cookie, fs_dirent_t *out)
{
    devfs_t *d = b->priv;
    if (*rel) {
        devnode_t *n = find(d, rel);
        if (!n)
            return -ENOENT;
        if (!S_ISDIR(n->mode))
            return -ENOTDIR;
    }
    memset(out, 0, sizeof *out);
    if (cookie < 2) {
        strcpy(out->name, cookie ? ".." : ".");
        out->ino = 1;
        out->type = GUEST_DT_DIR;
        out->off = cookie + 1;
        return 1;
    }
    size_t plen = strlen(rel);
    uint64_t idx = 2;
    for (size_t i = 0; i < d->n; i++) {
        const char *r = d->nodes[i].rel;
        if (plen) {
            if (strncmp(r, rel, plen) || r[plen] != '/')
                continue;
            r += plen + 1;
        }
        if (strchr(r, '/'))
            continue;
        if (idx++ < cookie)
            continue;
        snprintf(out->name, sizeof out->name, "%s", r);
        out->ino = i + 2;
        uint32_t m = d->nodes[i].mode;
        out->type = S_ISDIR(m) ? GUEST_DT_DIR : S_ISLNK(m) ? GUEST_DT_LNK : S_ISCHR(m) ? GUEST_DT_CHR : S_ISBLK(m) ? GUEST_DT_BLK : GUEST_DT_REG;
        out->off = idx;
        return 1;
    }
    return 0;
}

static int df_statfs(fs_backend_t *b, const char *rel, fs_statfs_t *out)
{
    (void)b; (void)rel;
    memset(out, 0, sizeof *out);
    out->type = 0x1373; /* DEVFS_SUPER_MAGIC */
    out->bsize = out->frsize = 4096;
    out->namelen = 255;
    return 0;
}

static void df_destroy(fs_backend_t *b)
{
    devfs_t *d = b->priv;
    free(d->nodes);
    free(d);
    free(b);
}

static const fs_backend_ops_t devfs_ops = {
    .stat = df_stat, .readlink = df_readlink, .open = df_open, .readdir = df_readdir, .statfs = df_statfs,
    .destroy = df_destroy,
};

/* ---- generic devices --------------------------------------------------- */

static int chr_fstat(fs_file_t *f, fs_stat_t *st)
{
    memset(st, 0, sizeof *st);
    st->dev = f->backend ? f->backend->dev : 0;
    st->mode = S_IFCHR | 0666;
    st->nlink = 1;
    st->blksize = 4096;
    st->rdev = (uint64_t)(uintptr_t)f->priv;
    return 0;
}

static int64_t null_read(fs_file_t *f, int64_t off, void *buf, size_t n) { (void)f; (void)off; (void)buf; (void)n; return 0; }
static int64_t null_write(fs_file_t *f, int64_t off, const void *buf, size_t n) { (void)f; (void)off; (void)buf; return (int64_t)n; }
static int64_t zero_read(fs_file_t *f, int64_t off, void *buf, size_t n) { (void)f; (void)off; memset(buf, 0, n); return (int64_t)n; }
static int64_t urandom_read(fs_file_t *f, int64_t off, void *buf, size_t n)
{
    (void)f; (void)off;
    static uint64_t s = 0x9E3779B97F4A7C15ull;
    uint8_t *d = buf;
    for (size_t i = 0; i < n; i++) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        d[i] = (uint8_t)s;
    }
    return (int64_t)n;
}

static const fs_file_ops_t null_ops = { .read = null_read, .write = null_write, .fstat = chr_fstat };
static const fs_file_ops_t zero_ops = { .read = zero_read, .write = null_write, .fstat = chr_fstat };
static const fs_file_ops_t urandom_ops = { .read = urandom_read, .write = null_write, .fstat = chr_fstat };

typedef struct console_ctx {
    devfs_t *d;
    uint64_t rdev;
} console_ctx_t;

static int64_t console_write(fs_file_t *f, int64_t off, const void *buf, size_t n)
{
    (void)off;
    console_ctx_t *c = f->priv;
    FILE *out = c->d->console ? c->d->console : stderr;
    fwrite(buf, 1, n, out);
    fflush(out);
    return (int64_t)n;
}

/* No keyboard is attached: a read waits (EAGAIN on a non-blocking fd), as a
 * tty with no input does; the harness has no console input source yet. */
static int64_t console_read(fs_file_t *f, int64_t off, void *buf, size_t n) { (void)f; (void)off; (void)buf; (void)n; return -EAGAIN; }

static int console_fstat(fs_file_t *f, fs_stat_t *st)
{
    console_ctx_t *c = f->priv;
    memset(st, 0, sizeof *st);
    st->dev = f->backend ? f->backend->dev : 0;
    st->mode = S_IFCHR | 0600;
    st->nlink = 1;
    st->blksize = 1024;
    st->rdev = c->rdev;
    return 0;
}

/* The console ttys: the stdio tcgetattr gets ENOTTY as in the trace (spec
 * 2.2). The virtual-terminal and keyboard ioctls SDL 1.2's fbcon driver
 * issues on /dev/tty0 are accepted with the state of a single text console
 * (OPEN-20: the exact sequence is captured by the trace). */
static int console_ioctl(fs_file_t *f, uint32_t req, gmem_t *m, gaddr_t arg)
{
    (void)f;
    bool vt = true; /* every console node is the one text console (OPEN-20) */
    switch (req) {
    case 0x5401: { /* TCGETS: the kernel termios of a text console (36 bytes) */
        if (!vt) return -ENOTTY;
        uint8_t tio[36];
        memset(tio, 0, sizeof tio);
        uint32_t iflag = 0x0500 /* ICRNL | IXON */, oflag = 0x0005 /* OPOST | ONLCR */,
                 cflag = 0x00BF /* B38400 | CS8 | CREAD | HUPCL */, lflag = 0x8A3B /* ISIG ICANON ECHO ECHOE ECHOK ECHOCTL ECHOKE IEXTEN */;
        memcpy(tio + 0, &iflag, 4); memcpy(tio + 4, &oflag, 4); memcpy(tio + 8, &cflag, 4); memcpy(tio + 12, &lflag, 4);
        static const uint8_t cc[19] = { 3, 28, 127, 21, 4, 0, 1, 0, 17, 19, 26, 0, 18, 15, 23, 22, 0, 0, 0 };
        memcpy(tio + 17, cc, 19);
        return gmem_write(m, arg, tio, sizeof tio) == GP_OK ? 0 : -EFAULT;
    }
    case 0x5402: case 0x5403: case 0x5404: /* TCSETS* */
        return vt ? 0 : -ENOTTY;
    case 0x5603: { /* VT_GETSTATE: struct vt_stat { u16 v_active, v_signal, v_state } */
        if (!vt) return -ENOTTY;
        uint8_t st[6] = { 1, 0, 0, 0, 3, 0 };
        return gmem_write(m, arg, st, 6) == GP_OK ? 0 : -EFAULT;
    }
    case 0x5600: /* VT_OPENQRY: the next free VT */
        if (!vt) return -ENOTTY;
        return gmem_st32(m, arg, 2) == GP_OK ? 0 : -EFAULT;
    case 0x5601: { /* VT_GETMODE: struct vt_mode { char mode, waitv; short relsig, acqsig, frsig } */
        if (!vt) return -ENOTTY;
        uint8_t vm[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        return gmem_write(m, arg, vm, 8) == GP_OK ? 0 : -EFAULT;
    }
    case 0x5602: /* VT_SETMODE */
    case 0x5606: /* VT_ACTIVATE */
    case 0x5607: /* VT_WAITACTIVE */
    case 0x5605: /* VT_RELDISP */
    case 0x4B3A: /* KDSETMODE (KD_TEXT / KD_GRAPHICS) */
    case 0x4B45: /* KDSKBMODE */
    case 0x4B47: /* KDSKBENT */
    case 0x4B4B: /* KDSKBLED */
    case 0x4B51: /* KDSETLED? (KDSKBMETA) */
        return vt ? 0 : -ENOTTY;
    case 0x4B3B: /* KDGETMODE */
        return vt ? (gmem_st32(m, arg, 0) == GP_OK ? 0 : -EFAULT) : -ENOTTY;
    case 0x4B44: /* KDGKBMODE: K_XLATE */
        return vt ? (gmem_st32(m, arg, 1) == GP_OK ? 0 : -EFAULT) : -ENOTTY;
    case 0x4B4A: /* KDGKBLED */
        return vt ? (gmem_st32(m, arg, 0) == GP_OK ? 0 : -EFAULT) : -ENOTTY;
    case 0x5413: { /* TIOCGWINSZ: 53 x 40 on the 320x240 console */
        uint8_t ws[8] = { 40, 0, 53, 0, 0, 0, 0, 0 };
        return gmem_write(m, arg, ws, 8) == GP_OK ? 0 : -EFAULT;
    }
    case 0x540E: case 0x5422: /* TIOCSCTTY, TIOCNOTTY */
        return 0;
    default:
        return -ENOTTY;
    }
}

static int console_poll(fs_file_t *f, int events) { (void)f; return events & GUEST_POLLOUT; }
static void console_release(fs_file_t *f) { free(f->priv); }

static const fs_file_ops_t console_ops = {
    .read = console_read, .write = console_write, .fstat = console_fstat, .ioctl = console_ioctl, .poll = console_poll,
    .release = console_release,
};

static int generic_open(void *ctx, int gflags, fs_file_t **out)
{
    const fs_file_ops_t *ops = ctx;
    fs_file_t *f = fs_file_new(ops, gflags);
    if (!f)
        return -ENOMEM;
    *out = f;
    return 0;
}

typedef struct console_open_ctx {
    devfs_t *d;
    uint64_t rdev;
} console_open_ctx_t;

static int console_open(void *ctx, int gflags, fs_file_t **out)
{
    console_open_ctx_t *oc = ctx;
    fs_file_t *f = fs_file_new(&console_ops, gflags);
    console_ctx_t *c = calloc(1, sizeof *c);
    if (!f || !c) {
        free(f);
        free(c);
        return -ENOMEM;
    }
    c->d = oc->d;
    c->rdev = oc->rdev;
    f->priv = c;
    *out = f;
    return 0;
}

#define MKDEV(ma, mi) (((uint64_t)(ma) << 8) | (mi))

fs_backend_t *fs_backend_devfs(void)
{
    fs_backend_t *b = calloc(1, sizeof *b);
    devfs_t *d = calloc(1, sizeof *d);
    if (!b || !d) {
        free(b);
        free(d);
        return NULL;
    }
    b->ops = &devfs_ops;
    b->dev = 4;
    b->readonly = false; /* devices open for writing; the tree itself has no mutation ops */
    b->priv = d;
    d->console = stderr;
    fs_devfs_register(b, "null", S_IFCHR | 0666, MKDEV(1, 3), generic_open, (void *)&null_ops);
    fs_devfs_register(b, "zero", S_IFCHR | 0666, MKDEV(1, 5), generic_open, (void *)&zero_ops);
    fs_devfs_register(b, "urandom", S_IFCHR | 0666, MKDEV(1, 9), generic_open, (void *)&urandom_ops);
    fs_devfs_register(b, "random", S_IFCHR | 0666, MKDEV(1, 8), generic_open, (void *)&urandom_ops);
    /* The console and the virtual terminals (devfs vc/N and the compat ttyN):
     * one text console whose ioctls SDL's fbcon driver needs succeed. */
    static console_open_ctx_t con[11];
    con[0] = (console_open_ctx_t){ d, MKDEV(4, 0) };
    fs_devfs_register(b, "console", S_IFCHR | 0600, MKDEV(5, 1), console_open, &con[0]);
    /* /dev/tty: init's children inherit the console as their controlling
     * terminal on the device (the menu fopen()s it without checking). */
    con[9] = (console_open_ctx_t){ d, MKDEV(5, 0) };
    fs_devfs_register(b, "tty", S_IFCHR | 0666, MKDEV(5, 0), console_open, &con[9]);
    for (int n = 0; n <= 8; n++) {
        char name[16];
        con[n + 1] = (console_open_ctx_t){ d, MKDEV(4, (unsigned)n) };
        snprintf(name, sizeof name, "tty%d", n);
        fs_devfs_register(b, name, S_IFCHR | 0600, MKDEV(4, (unsigned)n), console_open, &con[n + 1]);
        snprintf(name, sizeof name, "vc/%d", n);
        fs_devfs_register(b, name, S_IFCHR | 0600, MKDEV(4, (unsigned)n), console_open, &con[n + 1]);
    }
    fs_devfs_register(b, "pts", S_IFDIR | 0755, 0, NULL, NULL);
    fs_devfs_register(b, "shm", S_IFDIR | 0755, 0, NULL, NULL);
    return b;
}

void fs_devfs_set_console(fs_backend_t *devfs, FILE *out)
{
    devfs_t *d = devfs->priv;
    d->console = out;
}
