/* The VFS core: mount table, path resolution, open files, descriptor tables
 * and pipes. See fs.h. */
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gport2x/fs.h"
#include "gport2x/log.h"

typedef struct fs_mount {
    char target[FS_PATH_MAX];
    size_t tlen;
    fs_backend_t *b;
    char source[128], fstype[32], options[128];
    struct fs_mount *next;
} fs_mount_t;

struct fs {
    fs_mount_t *mounts; /* in mount order; the latest mount of a target wins */
};

static const fs_ctx_t *cur_ctx;
const fs_ctx_t *fs_current_ctx(void) { return cur_ctx; }

/* ---- files ------------------------------------------------------------- */

fs_file_t *fs_file_new(const fs_file_ops_t *ops, int gflags)
{
    fs_file_t *f = calloc(1, sizeof *f);
    if (!f)
        return NULL;
    f->ops = ops;
    f->refs = 1;
    f->gflags = gflags;
    f->seekable = true;
    return f;
}

fs_file_t *fs_file_ref(fs_file_t *f)
{
    f->refs++;
    return f;
}

void fs_file_unref(fs_file_t *f)
{
    if (!f || --f->refs > 0)
        return;
    if (f->ops->release)
        f->ops->release(f);
    free(f);
}

/* ---- namespace --------------------------------------------------------- */

fs_t *fs_create(void)
{
    return calloc(1, sizeof(fs_t));
}

void fs_destroy(fs_t *fs)
{
    if (!fs)
        return;
    while (fs->mounts) {
        fs_mount_t *m = fs->mounts;
        fs->mounts = m->next;
        if (m->b && m->b->ops->destroy)
            m->b->ops->destroy(m->b);
        free(m);
    }
    free(fs);
}

void fs_ctx_init(fs_ctx_t *ctx, int pid, const char *cwd)
{
    memset(ctx, 0, sizeof *ctx);
    ctx->pid = pid;
    ctx->umask = 022;
    snprintf(ctx->cwd, sizeof ctx->cwd, "%s", cwd && *cwd ? cwd : "/");
}

/* Appends path to the absolute, normalised base (used when base is "/"
 * or the cwd), collapsing ".", ".." and repeated slashes. */
static int normalize_from(const char *base, const char *path, char *out)
{
    char buf[FS_PATH_MAX];
    size_t n;
    if (path[0] == '/') {
        buf[0] = '/';
        n = 1;
    } else {
        n = strlen(base);
        if (n >= sizeof buf)
            return -ENAMETOOLONG;
        memcpy(buf, base, n + 1);
        if (n > 1 && buf[n - 1] == '/')
            buf[--n] = 0;
    }
    const char *p = path;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *e = strchr(p, '/');
        size_t clen = e ? (size_t)(e - p) : strlen(p);
        if (clen == 1 && p[0] == '.') {
            /* skip */
        } else if (clen == 2 && p[0] == '.' && p[1] == '.') {
            while (n > 1 && buf[n - 1] != '/')
                n--;
            if (n > 1)
                n--;
            buf[n] = 0;
        } else {
            if (clen > FS_NAME_MAX)
                return -ENAMETOOLONG;
            if (n + 1 + clen >= sizeof buf)
                return -ENAMETOOLONG;
            if (n > 1 || buf[n - 1] != '/')
                buf[n++] = '/';
            memcpy(buf + n, p, clen);
            n += clen;
            buf[n] = 0;
        }
        p += clen;
    }
    if (n == 0) {
        buf[0] = '/';
        n = 1;
    }
    buf[n] = 0;
    memcpy(out, buf, n + 1);
    return (int)n;
}

int fs_normalize(const fs_ctx_t *ctx, const char *path, char *out)
{
    return normalize_from(ctx->cwd, path, out);
}

static int mount_add(fs_t *fs, fs_mount_t *m)
{
    m->next = NULL;
    fs_mount_t **pp = &fs->mounts;
    while (*pp)
        pp = &(*pp)->next;
    *pp = m;
    return 0;
}

int fs_mount(fs_t *fs, const char *target, fs_backend_t *b, const char *source, const char *fstype, const char *options)
{
    if (!b || !target || target[0] != '/')
        return -EINVAL;
    fs_mount_t *m = calloc(1, sizeof *m);
    if (!m)
        return -ENOMEM;
    int n = normalize_from("/", target, m->target);
    if (n < 0) {
        free(m);
        return n;
    }
    m->tlen = (size_t)n;
    m->b = b;
    snprintf(m->source, sizeof m->source, "%s", source ? source : "none");
    snprintf(m->fstype, sizeof m->fstype, "%s", fstype ? fstype : "none");
    snprintf(m->options, sizeof m->options, "%s", options ? options : "rw");
    if (!fs->mounts && m->tlen != 1) {
        free(m);
        return -EINVAL; /* "/" must come first */
    }
    gp_trace(GP_TRACE_FS, "mount %s on %s type %s (%s)", m->source, m->target, m->fstype, m->options);
    return mount_add(fs, m);
}

int fs_umount(fs_t *fs, const char *target)
{
    char abs[FS_PATH_MAX];
    if (normalize_from("/", target, abs) < 0)
        return -EINVAL;
    fs_mount_t **pp = &fs->mounts, **found = NULL;
    for (; *pp; pp = &(*pp)->next)
        if (!strcmp((*pp)->target, abs))
            found = pp; /* the latest wins */
    if (!found)
        return -EINVAL;
    if ((*found)->tlen == 1)
        return -EBUSY;
    fs_mount_t *m = *found;
    *found = m->next;
    gp_trace(GP_TRACE_FS, "umount %s", m->target);
    if (m->b->ops->destroy)
        m->b->ops->destroy(m->b);
    free(m);
    return 0;
}

int fs_mounts_text(fs_t *fs, char *buf, size_t n)
{
    size_t len = 0;
    for (fs_mount_t *m = fs->mounts; m; m = m->next) {
        int k = snprintf(buf + len, len < n ? n - len : 0, "%s %s %s %s 0 0\n", m->source, m->target, m->fstype,
                         m->options);
        if (k < 0)
            return -EIO;
        len += (size_t)k;
    }
    return (int)len;
}

/* The mount owning abs, and the path relative to it. */
static fs_mount_t *mount_for(fs_t *fs, const char *abs, const char **rel)
{
    fs_mount_t *best = NULL;
    for (fs_mount_t *m = fs->mounts; m; m = m->next) {
        bool match;
        if (m->tlen == 1)
            match = true;
        else
            match = !strncmp(abs, m->target, m->tlen) && (abs[m->tlen] == 0 || abs[m->tlen] == '/');
        if (match && (!best || m->tlen >= best->tlen))
            best = m;
    }
    if (!best)
        return NULL;
    const char *r = abs + best->tlen;
    while (*r == '/')
        r++;
    *rel = r;
    return best;
}

static bool is_proc_self(const char *abs, const char **rest)
{
    if (strncmp(abs, "/proc/self", 10))
        return false;
    if (abs[10] == 0 || abs[10] == '/') {
        *rest = abs + 10;
        return true;
    }
    return false;
}

/* lstat of an absolute, lexically normalised path. */
static int stat_nofollow(fs_t *fs, const char *abs, fs_stat_t *st, fs_mount_t **mp, const char **relp)
{
    const char *rest;
    if (is_proc_self(abs, &rest) && !*rest) {
        memset(st, 0, sizeof *st);
        st->mode = S_IFLNK | 0777;
        st->nlink = 1;
        st->ino = 0xFFFE;
        if (mp)
            *mp = NULL;
        return 0;
    }
    const char *rel;
    fs_mount_t *m = mount_for(fs, abs, &rel);
    if (!m)
        return -ENOENT;
    int r = m->b->ops->stat(m->b, rel, st);
    if (r < 0)
        return r;
    st->dev = m->b->dev;
    if (mp)
        *mp = m;
    if (relp)
        *relp = rel;
    return 0;
}

static int readlink_nofollow(fs_t *fs, const char *abs, char *buf, size_t n)
{
    const char *rest;
    if (is_proc_self(abs, &rest) && !*rest) {
        int k = snprintf(buf, n, "%d", cur_ctx ? cur_ctx->pid : 1);
        return k;
    }
    const char *rel;
    fs_mount_t *m = mount_for(fs, abs, &rel);
    if (!m)
        return -ENOENT;
    if (!m->b->ops->readlink)
        return -EINVAL;
    return m->b->ops->readlink(m->b, rel, buf, n);
}

int fs_resolve(fs_t *fs, const fs_ctx_t *ctx, const char *path, bool follow, char *out)
{
    char cur[FS_PATH_MAX];
    int r = normalize_from(ctx->cwd, path, cur);
    if (r < 0)
        return r;
    int loops = 0;
    for (;;) {
        bool restarted = false;
        const char *s = cur + 1;
        while (*s) {
            const char *e = strchr(s, '/');
            size_t plen = e ? (size_t)(e - cur) : strlen(cur);
            char prefix[FS_PATH_MAX];
            memcpy(prefix, cur, plen);
            prefix[plen] = 0;
            bool last = !e;
            fs_stat_t st;
            r = stat_nofollow(fs, prefix, &st, NULL, NULL);
            if (r < 0) {
                if (last)
                    break; /* a missing final component is the caller's business */
                return r == -ENOENT ? -ENOENT : r;
            }
            if (S_ISLNK(st.mode) && (!last || follow)) {
                if (++loops > 40)
                    return -ELOOP;
                char target[FS_PATH_MAX];
                int tl = readlink_nofollow(fs, prefix, target, sizeof target - 1);
                if (tl < 0)
                    return tl;
                target[tl] = 0;
                /* dir(prefix) + target + rest */
                char next[FS_PATH_MAX], joined[FS_PATH_MAX];
                size_t dl = plen;
                while (dl > 1 && cur[dl - 1] != '/')
                    dl--;
                if (dl > 1)
                    dl--; /* drop the slash */
                char dir[FS_PATH_MAX];
                memcpy(dir, cur, dl);
                dir[dl] = 0;
                if (dl == 0)
                    strcpy(dir, "/");
                const char *rest = e ? e : "";
                if (snprintf(joined, sizeof joined, "%s%s", target, rest) >= (int)sizeof joined)
                    return -ENAMETOOLONG;
                r = normalize_from(target[0] == '/' ? "/" : dir, joined, next);
                if (r < 0)
                    return r;
                memcpy(cur, next, (size_t)r + 1);
                restarted = true;
                break;
            }
            if (!last && !S_ISDIR(st.mode))
                return -ENOTDIR;
            s = e ? e + 1 : s + strlen(s);
        }
        if (!restarted)
            break;
    }
    strcpy(out, cur);
    return 0;
}

/* ---- directory files (generic over every backend) ---------------------- */

static int dir_readdir(fs_file_t *f, uint64_t cookie, fs_dirent_t *out)
{
    fs_backend_t *b = f->backend;
    return b->ops->readdir(b, (const char *)f->priv, cookie, out);
}

static int dir_fstat(fs_file_t *f, fs_stat_t *st)
{
    fs_backend_t *b = f->backend;
    int r = b->ops->stat(b, (const char *)f->priv, st);
    if (r == 0)
        st->dev = b->dev;
    return r;
}

static int64_t dir_read(fs_file_t *f, int64_t off, void *buf, size_t n)
{
    (void)f; (void)off; (void)buf; (void)n;
    return -EISDIR;
}

static int dir_fsync(fs_file_t *f) { (void)f; return 0; }

static void dir_release(fs_file_t *f) { free(f->priv); }

static const fs_file_ops_t dir_ops = {
    .read = dir_read, .fstat = dir_fstat, .readdir = dir_readdir, .fsync = dir_fsync, .release = dir_release,
};

/* ---- operations -------------------------------------------------------- */

#define ENTER(ctx) (cur_ctx = (ctx))

static int parent_of(const char *rel, char *out)
{
    const char *slash = strrchr(rel, '/');
    if (!slash) {
        out[0] = 0;
        return 0;
    }
    size_t n = (size_t)(slash - rel);
    memcpy(out, rel, n);
    out[n] = 0;
    return 0;
}

int fs_open(fs_t *fs, fs_ctx_t *ctx, const char *path, int gflags, uint32_t mode, fs_file_t **out)
{
    ENTER(ctx);
    *out = NULL;
    char abs[FS_PATH_MAX];
    int r = fs_resolve(fs, ctx, path, !(gflags & GUEST_O_NOFOLLOW), abs);
    if (r < 0)
        return r;
    const char *rel;
    fs_mount_t *m = mount_for(fs, abs, &rel);
    if (!m)
        return -ENOENT;
    fs_backend_t *b = m->b;
    fs_stat_t st;
    bool exists = b->ops->stat(b, rel, &st) == 0;
    int acc = gflags & GUEST_O_ACCMODE;
    if (exists) {
        if (S_ISLNK(st.mode))
            return -ELOOP;
        if ((gflags & GUEST_O_CREAT) && (gflags & GUEST_O_EXCL))
            return -EEXIST;
        if (S_ISDIR(st.mode)) {
            if (acc != GUEST_O_RDONLY || (gflags & GUEST_O_TRUNC))
                return -EISDIR;
            fs_file_t *f = fs_file_new(&dir_ops, gflags);
            if (!f)
                return -ENOMEM;
            f->is_dir = true;
            f->backend = b;
            f->priv = strdup(rel);
            snprintf(f->path, sizeof f->path, "%s", abs);
            *out = f;
            gp_trace(GP_TRACE_FS, "open %s (dir)", abs);
            return 0;
        }
        if (gflags & GUEST_O_DIRECTORY)
            return -ENOTDIR;
        if (b->readonly && (acc != GUEST_O_RDONLY || (gflags & GUEST_O_TRUNC)))
            return -EROFS;
    } else {
        if (!(gflags & GUEST_O_CREAT))
            return -ENOENT;
        if (b->readonly)
            return -EROFS;
        char parent[FS_PATH_MAX];
        parent_of(rel, parent);
        fs_stat_t pst;
        if (b->ops->stat(b, parent, &pst) < 0)
            return -ENOENT;
        if (!S_ISDIR(pst.mode))
            return -ENOTDIR;
    }
    if (!b->ops->open)
        return -ENODEV;
    r = b->ops->open(b, rel, gflags, mode & ~ctx->umask & 07777, out);
    if (r < 0) {
        gp_trace(GP_TRACE_FS, "open %s flags %#o: %s", abs, gflags, strerror(-r));
        return r;
    }
    (*out)->backend = b;
    (*out)->gflags = gflags;
    snprintf((*out)->path, sizeof (*out)->path, "%s", abs);
    gp_trace(GP_TRACE_FS, "open %s flags %#o ok", abs, gflags);
    return 0;
}

int fs_stat(fs_t *fs, fs_ctx_t *ctx, const char *path, bool follow, fs_stat_t *out)
{
    ENTER(ctx);
    char abs[FS_PATH_MAX];
    int r = fs_resolve(fs, ctx, path, follow, abs);
    if (r < 0)
        return r;
    r = stat_nofollow(fs, abs, out, NULL, NULL);
    gp_trace(GP_TRACE_FS, "%s %s: %s", follow ? "stat" : "lstat", abs, r ? strerror(-r) : "ok");
    return r;
}

int fs_readlink(fs_t *fs, fs_ctx_t *ctx, const char *path, char *buf, size_t n)
{
    ENTER(ctx);
    char abs[FS_PATH_MAX];
    int r = fs_resolve(fs, ctx, path, false, abs);
    if (r < 0)
        return r;
    fs_stat_t st;
    r = stat_nofollow(fs, abs, &st, NULL, NULL);
    if (r < 0)
        return r;
    if (!S_ISLNK(st.mode))
        return -EINVAL;
    return readlink_nofollow(fs, abs, buf, n);
}

int fs_access(fs_t *fs, fs_ctx_t *ctx, const char *path, int mode)
{
    ENTER(ctx);
    char abs[FS_PATH_MAX];
    int r = fs_resolve(fs, ctx, path, true, abs);
    if (r < 0)
        return r;
    fs_stat_t st;
    fs_mount_t *m;
    r = stat_nofollow(fs, abs, &st, &m, NULL);
    if (r < 0)
        return r;
    if ((mode & 2) && m && m->b->readonly)
        return -EROFS;
    if ((mode & 1) && !S_ISDIR(st.mode) && !(st.mode & 0111))
        return -EACCES;
    return 0;
}

static int mutate_lookup(fs_t *fs, fs_ctx_t *ctx, const char *path, bool follow, fs_mount_t **mp, char *relbuf)
{
    ENTER(ctx);
    char abs[FS_PATH_MAX];
    int r = fs_resolve(fs, ctx, path, follow, abs);
    if (r < 0)
        return r;
    const char *rel;
    fs_mount_t *m = mount_for(fs, abs, &rel);
    if (!m)
        return -ENOENT;
    if (!*rel)
        return -EBUSY; /* the mount root itself */
    if (m->b->readonly)
        return -EROFS;
    *mp = m;
    snprintf(relbuf, FS_PATH_MAX, "%s", rel);
    return 0;
}

int fs_unlink(fs_t *fs, fs_ctx_t *ctx, const char *path)
{
    fs_mount_t *m;
    char rel[FS_PATH_MAX];
    int r = mutate_lookup(fs, ctx, path, false, &m, rel);
    if (r < 0)
        return r;
    if (!m->b->ops->unlink)
        return -EPERM;
    r = m->b->ops->unlink(m->b, rel);
    gp_trace(GP_TRACE_FS, "unlink %s: %s", path, r ? strerror(-r) : "ok");
    return r;
}

int fs_mkdir(fs_t *fs, fs_ctx_t *ctx, const char *path, uint32_t mode)
{
    fs_mount_t *m;
    char rel[FS_PATH_MAX];
    int r = mutate_lookup(fs, ctx, path, true, &m, rel);
    if (r < 0)
        return r == -EBUSY ? -EEXIST : r;
    if (!m->b->ops->mkdir)
        return -EPERM;
    r = m->b->ops->mkdir(m->b, rel, mode & ~ctx->umask & 07777);
    gp_trace(GP_TRACE_FS, "mkdir %s: %s", path, r ? strerror(-r) : "ok");
    return r;
}

int fs_rmdir(fs_t *fs, fs_ctx_t *ctx, const char *path)
{
    fs_mount_t *m;
    char rel[FS_PATH_MAX];
    int r = mutate_lookup(fs, ctx, path, false, &m, rel);
    if (r < 0)
        return r;
    if (!m->b->ops->rmdir)
        return -EPERM;
    return m->b->ops->rmdir(m->b, rel);
}

int fs_rename(fs_t *fs, fs_ctx_t *ctx, const char *from, const char *to)
{
    fs_mount_t *m1, *m2;
    char rel1[FS_PATH_MAX], rel2[FS_PATH_MAX];
    int r = mutate_lookup(fs, ctx, from, false, &m1, rel1);
    if (r < 0)
        return r;
    r = mutate_lookup(fs, ctx, to, false, &m2, rel2);
    if (r < 0)
        return r;
    if (m1 != m2)
        return -EXDEV;
    if (!m1->b->ops->rename)
        return -EPERM;
    return m1->b->ops->rename(m1->b, rel1, rel2);
}

int fs_truncate(fs_t *fs, fs_ctx_t *ctx, const char *path, int64_t size)
{
    fs_mount_t *m;
    char rel[FS_PATH_MAX];
    int r = mutate_lookup(fs, ctx, path, true, &m, rel);
    if (r < 0)
        return r;
    if (!m->b->ops->truncate)
        return -EPERM;
    return m->b->ops->truncate(m->b, rel, size);
}

int fs_chmod(fs_t *fs, fs_ctx_t *ctx, const char *path, uint32_t mode)
{
    fs_mount_t *m;
    char rel[FS_PATH_MAX];
    int r = mutate_lookup(fs, ctx, path, true, &m, rel);
    if (r == -EBUSY)
        return 0; /* the mount root: nothing stores it */
    if (r < 0)
        return r;
    if (!m->b->ops->chmod)
        return 0; /* backends without modes (devfs, proc) accept it */
    return m->b->ops->chmod(m->b, rel, mode & 07777);
}

int fs_statfs(fs_t *fs, fs_ctx_t *ctx, const char *path, fs_statfs_t *out)
{
    ENTER(ctx);
    char abs[FS_PATH_MAX];
    int r = fs_resolve(fs, ctx, path, true, abs);
    if (r < 0)
        return r;
    fs_stat_t st;
    fs_mount_t *m;
    const char *rel;
    r = stat_nofollow(fs, abs, &st, &m, &rel);
    if (r < 0)
        return r;
    memset(out, 0, sizeof *out);
    if (!m || !m->b->ops->statfs) {
        out->bsize = out->frsize = 4096;
        out->namelen = 255;
        return 0;
    }
    return m->b->ops->statfs(m->b, rel, out);
}

int fs_chdir(fs_t *fs, fs_ctx_t *ctx, const char *path)
{
    ENTER(ctx);
    char abs[FS_PATH_MAX];
    int r = fs_resolve(fs, ctx, path, true, abs);
    if (r < 0)
        return r;
    fs_stat_t st;
    r = stat_nofollow(fs, abs, &st, NULL, NULL);
    if (r < 0)
        return r;
    if (!S_ISDIR(st.mode))
        return -ENOTDIR;
    snprintf(ctx->cwd, sizeof ctx->cwd, "%s", abs);
    return 0;
}

int fs_read_file(fs_t *fs, fs_ctx_t *ctx, const char *path, uint8_t **data, size_t *len)
{
    fs_file_t *f;
    int r = fs_open(fs, ctx, path, GUEST_O_RDONLY, 0, &f);
    if (r < 0)
        return r;
    if (f->is_dir) {
        fs_file_unref(f);
        return -EISDIR;
    }
    fs_stat_t st;
    size_t cap = 0;
    if (fs_file_fstat(f, &st) == 0 && st.size > 0)
        cap = (size_t)st.size;
    if (cap == 0)
        cap = 4096;
    uint8_t *buf = malloc(cap + 1);
    if (!buf) {
        fs_file_unref(f);
        return -ENOMEM;
    }
    size_t n = 0;
    for (;;) {
        if (n == cap) {
            cap *= 2;
            uint8_t *nb = realloc(buf, cap + 1);
            if (!nb) {
                free(buf);
                fs_file_unref(f);
                return -ENOMEM;
            }
            buf = nb;
        }
        int64_t got = fs_file_read(f, buf + n, cap - n);
        if (got < 0) {
            free(buf);
            fs_file_unref(f);
            return (int)got;
        }
        if (got == 0)
            break;
        n += (size_t)got;
    }
    fs_file_unref(f);
    buf[n] = 0;
    *data = buf;
    *len = n;
    return 0;
}

/* ---- file operations --------------------------------------------------- */

int64_t fs_file_read(fs_file_t *f, void *buf, size_t n)
{
    if (f->is_dir)
        return -EISDIR;
    if ((f->gflags & GUEST_O_ACCMODE) == GUEST_O_WRONLY)
        return -EBADF;
    if (!f->ops->read)
        return -EINVAL;
    int64_t r = f->ops->read(f, f->pos, buf, n);
    if (r > 0 && f->seekable)
        f->pos += r;
    return r;
}

int64_t fs_file_write(fs_file_t *f, const void *buf, size_t n)
{
    if (f->is_dir)
        return -EISDIR;
    if ((f->gflags & GUEST_O_ACCMODE) == GUEST_O_RDONLY)
        return -EBADF;
    if (!f->ops->write)
        return -EINVAL;
    if ((f->gflags & GUEST_O_APPEND) && f->seekable && f->ops->fstat) {
        fs_stat_t st;
        if (f->ops->fstat(f, &st) == 0)
            f->pos = st.size;
    }
    int64_t r = f->ops->write(f, f->pos, buf, n);
    if (r > 0 && f->seekable)
        f->pos += r;
    return r;
}

int64_t fs_file_pread(fs_file_t *f, int64_t off, void *buf, size_t n)
{
    if (!f->seekable)
        return -ESPIPE;
    if (f->is_dir)
        return -EISDIR;
    if ((f->gflags & GUEST_O_ACCMODE) == GUEST_O_WRONLY)
        return -EBADF;
    if (!f->ops->read)
        return -EINVAL;
    return f->ops->read(f, off, buf, n);
}

int64_t fs_file_pwrite(fs_file_t *f, int64_t off, const void *buf, size_t n)
{
    if (!f->seekable)
        return -ESPIPE;
    if ((f->gflags & GUEST_O_ACCMODE) == GUEST_O_RDONLY)
        return -EBADF;
    if (!f->ops->write)
        return -EINVAL;
    return f->ops->write(f, off, buf, n);
}

int64_t fs_file_lseek(fs_file_t *f, int64_t off, int whence)
{
    if (!f->seekable)
        return -ESPIPE;
    int64_t base;
    switch (whence) {
    case 0: base = 0; break;
    case 1: base = f->pos; break;
    case 2: {
        fs_stat_t st;
        if (!f->ops->fstat || f->ops->fstat(f, &st) < 0)
            return -EINVAL;
        base = st.size;
        break;
    }
    default: return -EINVAL;
    }
    if (base + off < 0)
        return -EINVAL;
    f->pos = base + off;
    return f->pos;
}

int fs_file_fstat(fs_file_t *f, fs_stat_t *st)
{
    if (!f->ops->fstat)
        return -EINVAL;
    int r = f->ops->fstat(f, st);
    if (r == 0 && f->backend)
        st->dev = f->backend->dev;
    return r;
}

int fs_file_readdir(fs_file_t *f, fs_dirent_t *out)
{
    if (!f->is_dir || !f->ops->readdir)
        return -ENOTDIR;
    int r = f->ops->readdir(f, (uint64_t)f->pos, out);
    if (r == 1)
        f->pos = (int64_t)out->off;
    return r;
}

int fs_file_ioctl(fs_file_t *f, uint32_t req, gmem_t *m, gaddr_t arg)
{
    if (!f->ops->ioctl)
        return -ENOTTY;
    return f->ops->ioctl(f, req, m, arg);
}

int fs_file_mmap(fs_file_t *f, uint64_t off, uint32_t len, int prot, bool shared, gmem_obj_t **obj, uint32_t *obj_off)
{
    if (f->ops->mmap)
        return f->ops->mmap(f, off, len, prot, shared, obj, obj_off);
    if (f->is_dir || !f->ops->read)
        return -ENODEV;
    if ((f->gflags & GUEST_O_ACCMODE) == GUEST_O_WRONLY)
        return -EACCES;
    if (shared && (prot & GMEM_PROT_W) && (f->gflags & GUEST_O_ACCMODE) == GUEST_O_RDONLY)
        return -EACCES;
    if (shared && (prot & GMEM_PROT_W))
        gp_warn("mmap: MAP_SHARED writable mapping of %s served as a private copy", f->path);
    uint32_t size = GP2X_PAGE_ALIGN_UP(len);
    gmem_obj_t *o = gmem_obj_ram(size);
    if (!o)
        return -ENOMEM;
    uint8_t *host = gmem_obj_host(o);
    size_t done = 0;
    while (done < len) {
        int64_t got = f->ops->read(f, (int64_t)(off + done), host + done, len - done);
        if (got < 0) {
            gmem_obj_release(o);
            return (int)got;
        }
        if (got == 0)
            break;
        done += (size_t)got;
    }
    *obj = o;
    *obj_off = 0;
    return 0;
}

int fs_file_ftruncate(fs_file_t *f, int64_t size)
{
    if ((f->gflags & GUEST_O_ACCMODE) == GUEST_O_RDONLY)
        return -EINVAL;
    if (!f->ops->ftruncate)
        return -EINVAL;
    return f->ops->ftruncate(f, size);
}

int fs_file_fsync(fs_file_t *f)
{
    return f->ops->fsync ? f->ops->fsync(f) : 0;
}

int fs_file_poll(fs_file_t *f, int events)
{
    if (f->ops->poll)
        return f->ops->poll(f, events);
    return events & (GUEST_POLLIN | GUEST_POLLOUT);
}

/* ---- descriptor tables ------------------------------------------------- */

#define FS_FD_MAX 1024

typedef struct fd_entry {
    fs_file_t *f;
    bool cloexec;
} fd_entry_t;

struct fs_fdtable {
    int refs;
    int cap;
    fd_entry_t *e;
};

fs_fdtable_t *fs_fdt_create(void)
{
    fs_fdtable_t *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->refs = 1;
    t->cap = 64;
    t->e = calloc((size_t)t->cap, sizeof *t->e);
    if (!t->e) {
        free(t);
        return NULL;
    }
    return t;
}

fs_fdtable_t *fs_fdt_ref(fs_fdtable_t *t)
{
    t->refs++;
    return t;
}

fs_fdtable_t *fs_fdt_clone(fs_fdtable_t *t)
{
    fs_fdtable_t *n = fs_fdt_create();
    if (!n)
        return NULL;
    for (int fd = 0; fd < t->cap; fd++) {
        if (!t->e[fd].f)
            continue;
        if (fs_fdt_install(n, t->e[fd].f, t->e[fd].cloexec, fd) != fd) {
            fs_fdt_unref(n);
            return NULL;
        }
    }
    return n;
}

void fs_fdt_unref(fs_fdtable_t *t)
{
    if (!t || --t->refs > 0)
        return;
    for (int fd = 0; fd < t->cap; fd++)
        if (t->e[fd].f)
            fs_file_unref(t->e[fd].f);
    free(t->e);
    free(t);
}

fs_file_t *fs_fdt_get(fs_fdtable_t *t, int fd)
{
    if (fd < 0 || fd >= t->cap)
        return NULL;
    return t->e[fd].f;
}

static int fdt_grow(fs_fdtable_t *t, int need)
{
    if (need < t->cap)
        return 0;
    if (need >= FS_FD_MAX)
        return -EMFILE;
    int ncap = t->cap;
    while (ncap <= need)
        ncap *= 2;
    if (ncap > FS_FD_MAX)
        ncap = FS_FD_MAX;
    fd_entry_t *ne = realloc(t->e, (size_t)ncap * sizeof *ne);
    if (!ne)
        return -ENOMEM;
    memset(ne + t->cap, 0, (size_t)(ncap - t->cap) * sizeof *ne);
    t->e = ne;
    t->cap = ncap;
    return 0;
}

int fs_fdt_install(fs_fdtable_t *t, fs_file_t *f, bool cloexec, int minfd)
{
    if (minfd < 0)
        minfd = 0;
    for (int fd = minfd; fd < FS_FD_MAX; fd++) {
        int r = fdt_grow(t, fd);
        if (r < 0)
            return r;
        if (!t->e[fd].f) {
            t->e[fd].f = fs_file_ref(f);
            t->e[fd].cloexec = cloexec;
            return fd;
        }
    }
    return -EMFILE;
}

int fs_fdt_close(fs_fdtable_t *t, int fd)
{
    if (fd < 0 || fd >= t->cap || !t->e[fd].f)
        return -EBADF;
    fs_file_unref(t->e[fd].f);
    t->e[fd].f = NULL;
    t->e[fd].cloexec = false;
    return 0;
}

int fs_fdt_dup2(fs_fdtable_t *t, int oldfd, int newfd)
{
    fs_file_t *f = fs_fdt_get(t, oldfd);
    if (!f)
        return -EBADF;
    if (newfd < 0 || newfd >= FS_FD_MAX)
        return -EBADF;
    if (oldfd == newfd)
        return newfd;
    int r = fdt_grow(t, newfd);
    if (r < 0)
        return r;
    if (t->e[newfd].f)
        fs_file_unref(t->e[newfd].f);
    t->e[newfd].f = fs_file_ref(f);
    t->e[newfd].cloexec = false;
    return newfd;
}

int fs_fdt_get_cloexec(fs_fdtable_t *t, int fd)
{
    if (!fs_fdt_get(t, fd))
        return -EBADF;
    return t->e[fd].cloexec ? 1 : 0;
}

int fs_fdt_set_cloexec(fs_fdtable_t *t, int fd, bool cloexec)
{
    if (!fs_fdt_get(t, fd))
        return -EBADF;
    t->e[fd].cloexec = cloexec;
    return 0;
}

void fs_fdt_close_on_exec(fs_fdtable_t *t)
{
    for (int fd = 0; fd < t->cap; fd++)
        if (t->e[fd].f && t->e[fd].cloexec)
            fs_fdt_close(t, fd);
}

int fs_fdt_max(const fs_fdtable_t *t)
{
    (void)t;
    return FS_FD_MAX;
}

/* ---- host stdio ---------------------------------------------------------- */

static int64_t hostfd_read(fs_file_t *f, int64_t off, void *buf, size_t n)
{
    (void)off;
    ssize_t r = read((int)(intptr_t)f->priv, buf, n);
    return r < 0 ? -errno : r;
}

static int64_t hostfd_write(fs_file_t *f, int64_t off, const void *buf, size_t n)
{
    (void)off;
    size_t done = 0;
    const uint8_t *p = buf;
    while (done < n) {
        ssize_t r = write((int)(intptr_t)f->priv, p + done, n - done);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return done ? (int64_t)done : -errno;
        }
        done += (size_t)r;
    }
    return (int64_t)done;
}

static int hostfd_fstat(fs_file_t *f, fs_stat_t *st)
{
    memset(st, 0, sizeof *st);
    st->mode = S_IFCHR | 0600;
    st->nlink = 1;
    st->blksize = 1024;
    st->rdev = (5u << 8) | 1u;
    st->ino = 1000 + (uint64_t)(intptr_t)f->priv;
    return 0;
}

static int hostfd_ioctl(fs_file_t *f, uint32_t req, gmem_t *m, gaddr_t arg)
{
    (void)f; (void)req; (void)m; (void)arg;
    return -ENOTTY;
}

static int hostfd_poll(fs_file_t *f, int events)
{
    (void)f;
    return events & GUEST_POLLOUT; /* no host input is polled; reads block the harness */
}

static const fs_file_ops_t hostfd_ops = { .read = hostfd_read, .write = hostfd_write, .fstat = hostfd_fstat, .ioctl = hostfd_ioctl, .poll = hostfd_poll };

/* An owned host descriptor (the native engine's pipes): readiness from the
 * host's poll, a FIFO in fstat, closed when the last guest reference goes. */
static int hostfd_owned_poll(fs_file_t *f, int events)
{
    struct pollfd p = { (int)(intptr_t)f->priv, 0, 0 };
    if (events & GUEST_POLLIN) p.events |= POLLIN;
    if (events & GUEST_POLLOUT) p.events |= POLLOUT;
    if (poll(&p, 1, 0) <= 0)
        return 0;
    int r = 0;
    if (p.revents & POLLIN) r |= GUEST_POLLIN;
    if (p.revents & POLLOUT) r |= GUEST_POLLOUT;
    if (p.revents & POLLHUP) r |= GUEST_POLLHUP;
    if (p.revents & POLLERR) r |= GUEST_POLLERR;
    return r;
}
static int hostfd_owned_fstat(fs_file_t *f, fs_stat_t *st)
{
    int r = hostfd_fstat(f, st);
    st->mode = S_IFIFO | 0600;
    st->rdev = 0;
    return r;
}
static void hostfd_owned_release(fs_file_t *f) { close((int)(intptr_t)f->priv); }
static const fs_file_ops_t hostfd_owned_ops = { .read = hostfd_read, .write = hostfd_write, .fstat = hostfd_owned_fstat,
                                                .ioctl = hostfd_ioctl, .poll = hostfd_owned_poll, .release = hostfd_owned_release };

fs_file_t *fs_file_hostfd_owned(int host_fd, int gflags)
{
    fs_file_t *f = fs_file_new(&hostfd_owned_ops, gflags);
    if (!f)
        return NULL;
    f->priv = (void *)(intptr_t)host_fd;
    f->seekable = false;
    snprintf(f->path, sizeof f->path, "pipe:[%d]", host_fd);
    return f;
}

fs_file_t *fs_file_hostfd(int host_fd, int gflags)
{
    fs_file_t *f = fs_file_new(&hostfd_ops, gflags);
    if (!f)
        return NULL;
    f->priv = (void *)(intptr_t)host_fd;
    f->seekable = false;
    snprintf(f->path, sizeof f->path, "host:%d", host_fd);
    return f;
}

/* ---- pipes (2.4: one page of buffer, PIPE_BUF 4096) -------------------- */

#define PIPE_SIZE 4096

typedef struct pipe_buf {
    uint8_t data[PIPE_SIZE];
    size_t head, len;
    int readers, writers;
} pipe_buf_t;

static int64_t pipe_read(fs_file_t *f, int64_t off, void *buf, size_t n)
{
    (void)off;
    pipe_buf_t *p = f->priv;
    if (p->len == 0)
        return p->writers ? -EAGAIN : 0;
    if (n > p->len)
        n = p->len;
    uint8_t *dst = buf;
    for (size_t i = 0; i < n; i++)
        dst[i] = p->data[(p->head + i) % PIPE_SIZE];
    p->head = (p->head + n) % PIPE_SIZE;
    p->len -= n;
    return (int64_t)n;
}

static int64_t pipe_write(fs_file_t *f, int64_t off, const void *buf, size_t n)
{
    (void)off;
    pipe_buf_t *p = f->priv;
    if (!p->readers)
        return -EPIPE;
    size_t room = PIPE_SIZE - p->len;
    if (n <= PIPE_SIZE) {
        if (room < n)
            return -EAGAIN; /* atomic: wait for space for the whole write */
    } else if (room == 0) {
        return -EAGAIN;
    }
    if (n > room)
        n = room;
    const uint8_t *src = buf;
    for (size_t i = 0; i < n; i++)
        p->data[(p->head + p->len + i) % PIPE_SIZE] = src[i];
    p->len += n;
    return (int64_t)n;
}

static int pipe_fstat(fs_file_t *f, fs_stat_t *st)
{
    (void)f;
    memset(st, 0, sizeof *st);
    st->mode = S_IFIFO | 0600;
    st->nlink = 1;
    st->blksize = PIPE_SIZE;
    return 0;
}

static int pipe_poll(fs_file_t *f, int events)
{
    pipe_buf_t *p = f->priv;
    int rev = 0;
    if ((f->gflags & GUEST_O_ACCMODE) == GUEST_O_RDONLY) {
        if (p->len)
            rev |= GUEST_POLLIN;
        if (!p->writers)
            rev |= GUEST_POLLHUP;
    } else {
        if (p->len < PIPE_SIZE)
            rev |= GUEST_POLLOUT;
        if (!p->readers)
            rev |= GUEST_POLLERR;
    }
    return rev & (events | GUEST_POLLHUP | GUEST_POLLERR);
}

static void pipe_release(fs_file_t *f)
{
    pipe_buf_t *p = f->priv;
    if ((f->gflags & GUEST_O_ACCMODE) == GUEST_O_RDONLY)
        p->readers--;
    else
        p->writers--;
    if (!p->readers && !p->writers)
        free(p);
}

static const fs_file_ops_t pipe_rd_ops = { .read = pipe_read, .fstat = pipe_fstat, .poll = pipe_poll, .release = pipe_release };
static const fs_file_ops_t pipe_wr_ops = { .write = pipe_write, .fstat = pipe_fstat, .poll = pipe_poll, .release = pipe_release };

int fs_pipe_create(fs_file_t **rd, fs_file_t **wr)
{
    pipe_buf_t *p = calloc(1, sizeof *p);
    if (!p)
        return -ENOMEM;
    fs_file_t *r = fs_file_new(&pipe_rd_ops, GUEST_O_RDONLY);
    fs_file_t *w = fs_file_new(&pipe_wr_ops, GUEST_O_WRONLY);
    if (!r || !w) {
        free(r);
        free(w);
        free(p);
        return -ENOMEM;
    }
    r->priv = w->priv = p;
    r->seekable = w->seekable = false;
    p->readers = p->writers = 1;
    snprintf(r->path, sizeof r->path, "pipe:");
    snprintf(w->path, sizeof w->path, "pipe:");
    *rd = r;
    *wr = w;
    return 0;
}
