/* Copy-on-write overlay (spec 6.4): reads come from lower unless upper has
 * the entry or a whiteout; writes copy the file up into upper. Whiteouts
 * are upper files named ".wh.<name>". statfs is lower's, so the card's
 * geometry (protection check C5) is unchanged by saves. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "gport2x/fs.h"
#include "gport2x/log.h"

#define WH_PREFIX ".wh."

typedef struct overlay {
    fs_backend_t *lower, *upper;
    uint64_t gen; /* bumps on every mutation; invalidates the readdir cache */
    char cache_rel[FS_PATH_MAX];
    uint64_t cache_gen;
    fs_dirent_t *cache;
    size_t cache_n;
    bool cache_valid;
} overlay_t;

static void split(const char *rel, char *dir, char *base)
{
    const char *slash = strrchr(rel, '/');
    if (!slash) {
        dir[0] = 0;
        snprintf(base, FS_NAME_MAX + 1, "%s", rel);
    } else {
        size_t n = (size_t)(slash - rel);
        memcpy(dir, rel, n);
        dir[n] = 0;
        snprintf(base, FS_NAME_MAX + 1, "%s", slash + 1);
    }
}

static void wh_path(const char *rel, char *out)
{
    char dir[FS_PATH_MAX], base[FS_NAME_MAX + 1];
    split(rel, dir, base);
    int n = *dir ? snprintf(out, FS_PATH_MAX, "%s/" WH_PREFIX "%s", dir, base)
                 : snprintf(out, FS_PATH_MAX, WH_PREFIX "%s", base);
    if (n >= FS_PATH_MAX)
        out[FS_PATH_MAX - 1] = 0; /* over-long names cannot exist in the guest namespace */
}

static bool has_wh_component(const char *rel)
{
    const char *p = rel;
    while (*p) {
        if (!strncmp(p, WH_PREFIX, 4))
            return true;
        const char *e = strchr(p, '/');
        if (!e)
            break;
        p = e + 1;
    }
    return false;
}

static bool upper_has(overlay_t *o, const char *rel, fs_stat_t *st)
{
    fs_stat_t tmp;
    return o->upper->ops->stat(o->upper, rel, st ? st : &tmp) == 0;
}

/* True if rel or any ancestor is whited out in upper. */
static bool whited(overlay_t *o, const char *rel)
{
    char prefix[FS_PATH_MAX];
    const char *p = rel;
    while (*p) {
        const char *e = strchr(p, '/');
        size_t plen = e ? (size_t)(e - rel) : strlen(rel);
        memcpy(prefix, rel, plen);
        prefix[plen] = 0;
        char wh[FS_PATH_MAX];
        wh_path(prefix, wh);
        if (upper_has(o, wh, NULL))
            return true;
        if (!e)
            break;
        p = e + 1;
    }
    return false;
}

static int merged_stat(overlay_t *o, const char *rel, fs_stat_t *st, bool *in_upper)
{
    if (has_wh_component(rel))
        return -ENOENT;
    if (*rel && whited(o, rel))
        return -ENOENT;
    if (upper_has(o, rel, st)) {
        if (in_upper)
            *in_upper = true;
        return 0;
    }
    if (in_upper)
        *in_upper = false;
    return o->lower->ops->stat(o->lower, rel, st);
}

static int ov_stat(fs_backend_t *b, const char *rel, fs_stat_t *st)
{
    int r = merged_stat(b->priv, rel, st, NULL);
    if (r == 0)
        st->dev = b->dev;
    return r;
}

static int ov_readlink(fs_backend_t *b, const char *rel, char *buf, size_t n)
{
    overlay_t *o = b->priv;
    fs_stat_t st;
    bool up;
    int r = merged_stat(o, rel, &st, &up);
    if (r < 0)
        return r;
    fs_backend_t *src = up ? o->upper : o->lower;
    return src->ops->readlink ? src->ops->readlink(src, rel, buf, n) : -EINVAL;
}

/* Creates the ancestors of rel in upper (they exist in the merged view). */
static int upper_mkparents(overlay_t *o, const char *rel)
{
    char prefix[FS_PATH_MAX];
    const char *p = rel;
    for (;;) {
        const char *e = strchr(p, '/');
        if (!e)
            return 0;
        size_t plen = (size_t)(e - rel);
        memcpy(prefix, rel, plen);
        prefix[plen] = 0;
        fs_stat_t st;
        if (!upper_has(o, prefix, &st)) {
            int r = o->upper->ops->mkdir(o->upper, prefix, 0755);
            if (r < 0 && r != -EEXIST)
                return r;
        }
        p = e + 1;
    }
}

static int remove_whiteout(overlay_t *o, const char *rel)
{
    char wh[FS_PATH_MAX];
    wh_path(rel, wh);
    if (upper_has(o, wh, NULL))
        return o->upper->ops->unlink(o->upper, wh);
    return 0;
}

static int add_whiteout(overlay_t *o, const char *rel)
{
    char wh[FS_PATH_MAX];
    wh_path(rel, wh);
    int r = upper_mkparents(o, rel);
    if (r < 0)
        return r;
    fs_file_t *f;
    r = o->upper->ops->open(o->upper, wh, GUEST_O_WRONLY | GUEST_O_CREAT | GUEST_O_TRUNC, 0644, &f);
    if (r < 0)
        return r;
    fs_file_unref(f);
    return 0;
}

static int copy_up(overlay_t *o, const char *rel, bool content)
{
    int r = upper_mkparents(o, rel);
    if (r < 0)
        return r;
    fs_stat_t st;
    r = o->lower->ops->stat(o->lower, rel, &st);
    if (r < 0)
        return r;
    if (S_ISDIR(st.mode)) {
        r = o->upper->ops->mkdir(o->upper, rel, 0755);
        return r == -EEXIST ? 0 : r;
    }
    fs_file_t *dst;
    r = o->upper->ops->open(o->upper, rel, GUEST_O_WRONLY | GUEST_O_CREAT | GUEST_O_TRUNC, st.mode & 07777, &dst);
    if (r < 0)
        return r;
    if (content) {
        fs_file_t *src;
        r = o->lower->ops->open(o->lower, rel, GUEST_O_RDONLY, 0, &src);
        if (r < 0) {
            fs_file_unref(dst);
            return r;
        }
        uint8_t buf[65536];
        int64_t off = 0;
        for (;;) {
            int64_t got = src->ops->read(src, off, buf, sizeof buf);
            if (got < 0) {
                r = (int)got;
                break;
            }
            if (got == 0)
                break;
            int64_t put = dst->ops->write(dst, off, buf, (size_t)got);
            if (put != got) {
                r = put < 0 ? (int)put : -EIO;
                break;
            }
            off += got;
        }
        fs_file_unref(src);
    }
    fs_file_unref(dst);
    gp_trace(GP_TRACE_FS, "overlay: copy-up %s%s", rel, content ? "" : " (empty)");
    return r;
}

static int ov_open(fs_backend_t *b, const char *rel, int gflags, uint32_t mode, fs_file_t **out)
{
    overlay_t *o = b->priv;
    if (has_wh_component(rel))
        return -ENOENT;
    int acc = gflags & GUEST_O_ACCMODE;
    bool writing = acc != GUEST_O_RDONLY || (gflags & GUEST_O_TRUNC);
    bool is_whited = *rel && whited(o, rel);
    fs_stat_t st;
    if (!is_whited && upper_has(o, rel, &st)) {
        int r = o->upper->ops->open(o->upper, rel, gflags, mode, out);
        if (r == 0)
            (*out)->backend = b;
        return r;
    }
    if (!is_whited && o->lower->ops->stat(o->lower, rel, &st) == 0) {
        if (!writing) {
            int r = o->lower->ops->open(o->lower, rel, gflags, mode, out);
            if (r == 0)
                (*out)->backend = b;
            return r;
        }
        int r = copy_up(o, rel, !(gflags & GUEST_O_TRUNC));
        if (r < 0)
            return r;
        o->gen++;
        r = o->upper->ops->open(o->upper, rel, gflags & ~GUEST_O_CREAT, mode, out);
        if (r == 0)
            (*out)->backend = b;
        return r;
    }
    if (!(gflags & GUEST_O_CREAT))
        return -ENOENT;
    int r = upper_mkparents(o, rel);
    if (r < 0)
        return r;
    r = remove_whiteout(o, rel);
    if (r < 0)
        return r;
    o->gen++;
    r = o->upper->ops->open(o->upper, rel, gflags, mode, out);
    if (r == 0)
        (*out)->backend = b;
    return r;
}

/* Builds the merged listing of rel: lower order first (entries replaced by
 * upper keep their position), then upper-only entries in upper order. */
static int build_listing(overlay_t *o, const char *rel)
{
    free(o->cache);
    o->cache = NULL;
    o->cache_n = 0;
    o->cache_valid = false;
    fs_dirent_t *up = NULL;
    size_t nup = 0, capup = 0;
    bool *emitted = NULL;
    fs_stat_t st;
    bool upper_dir = upper_has(o, rel, &st) && S_ISDIR(st.mode);
    bool lower_dir = o->lower->ops->stat(o->lower, rel, &st) == 0 && S_ISDIR(st.mode);
    if (*rel && whited(o, rel))
        lower_dir = false;
    if (!upper_dir && !lower_dir)
        return -ENOENT;
    if (upper_dir) {
        uint64_t cookie = 0;
        fs_dirent_t d;
        int r;
        while ((r = o->upper->ops->readdir(o->upper, rel, cookie, &d)) == 1) {
            cookie = d.off;
            if (nup == capup) {
                capup = capup ? capup * 2 : 32;
                fs_dirent_t *n = realloc(up, capup * sizeof *n);
                if (!n) {
                    free(up);
                    return -ENOMEM;
                }
                up = n;
            }
            up[nup++] = d;
        }
        if (r < 0) {
            free(up);
            return r;
        }
    }
    emitted = calloc(nup ? nup : 1, sizeof *emitted);
    size_t cap = 0;
    #define PUSH(ent) do { \
        if (o->cache_n == cap) { cap = cap ? cap * 2 : 32; \
            fs_dirent_t *n_ = realloc(o->cache, cap * sizeof *n_); \
            if (!n_) { free(up); free(emitted); return -ENOMEM; } o->cache = n_; } \
        o->cache[o->cache_n] = (ent); o->cache[o->cache_n].off = o->cache_n + 1; o->cache_n++; } while (0)
    if (lower_dir) {
        uint64_t cookie = 0;
        fs_dirent_t d;
        int r;
        while ((r = o->lower->ops->readdir(o->lower, rel, cookie, &d)) == 1) {
            cookie = d.off;
            bool dot = !strcmp(d.name, ".") || !strcmp(d.name, "..");
            if (!dot) {
                bool wh = false;
                size_t j;
                for (j = 0; j < nup; j++) {
                    if (!strncmp(up[j].name, WH_PREFIX, 4) && !strcmp(up[j].name + 4, d.name)) {
                        wh = true;
                        break;
                    }
                }
                if (wh)
                    continue;
                for (j = 0; j < nup; j++) {
                    if (!strcmp(up[j].name, d.name)) {
                        emitted[j] = true;
                        PUSH(up[j]);
                        break;
                    }
                }
                if (j < nup)
                    continue;
            } else {
                for (size_t j = 0; j < nup; j++)
                    if (!strcmp(up[j].name, d.name))
                        emitted[j] = true;
            }
            PUSH(d);
        }
        if (r < 0) {
            free(up);
            free(emitted);
            return r;
        }
    }
    for (size_t j = 0; j < nup; j++) {
        if (emitted[j] || !strncmp(up[j].name, WH_PREFIX, 4))
            continue;
        bool dot = !strcmp(up[j].name, ".") || !strcmp(up[j].name, "..");
        if (dot && lower_dir)
            continue;
        PUSH(up[j]);
    }
    #undef PUSH
    free(up);
    free(emitted);
    snprintf(o->cache_rel, sizeof o->cache_rel, "%s", rel);
    o->cache_gen = o->gen;
    o->cache_valid = true;
    return 0;
}

static int ov_readdir(fs_backend_t *b, const char *rel, uint64_t cookie, fs_dirent_t *out)
{
    overlay_t *o = b->priv;
    if (!o->cache_valid || o->cache_gen != o->gen || strcmp(o->cache_rel, rel)) {
        int r = build_listing(o, rel);
        if (r < 0)
            return r;
    }
    if (cookie >= o->cache_n)
        return 0;
    *out = o->cache[cookie];
    return 1;
}

static int ov_unlink(fs_backend_t *b, const char *rel)
{
    overlay_t *o = b->priv;
    fs_stat_t st;
    bool up;
    int r = merged_stat(o, rel, &st, &up);
    if (r < 0)
        return r;
    if (S_ISDIR(st.mode))
        return -EISDIR;
    o->gen++;
    if (up) {
        r = o->upper->ops->unlink(o->upper, rel);
        if (r < 0)
            return r;
    }
    if (o->lower->ops->stat(o->lower, rel, &st) == 0)
        return add_whiteout(o, rel);
    return 0;
}

static int ov_mkdir(fs_backend_t *b, const char *rel, uint32_t mode)
{
    overlay_t *o = b->priv;
    fs_stat_t st;
    if (merged_stat(o, rel, &st, NULL) == 0)
        return -EEXIST;
    char dir[FS_PATH_MAX], base[FS_NAME_MAX + 1];
    split(rel, dir, base);
    if (merged_stat(o, dir, &st, NULL) < 0)
        return -ENOENT;
    if (!S_ISDIR(st.mode))
        return -ENOTDIR;
    int r = upper_mkparents(o, rel);
    if (r < 0)
        return r;
    r = remove_whiteout(o, rel);
    if (r < 0)
        return r;
    o->gen++;
    return o->upper->ops->mkdir(o->upper, rel, mode);
}

static int ov_rmdir(fs_backend_t *b, const char *rel)
{
    overlay_t *o = b->priv;
    fs_stat_t st;
    bool up;
    int r = merged_stat(o, rel, &st, &up);
    if (r < 0)
        return r;
    if (!S_ISDIR(st.mode))
        return -ENOTDIR;
    fs_dirent_t d;
    uint64_t cookie = 0;
    while ((r = ov_readdir(b, rel, cookie, &d)) == 1) {
        cookie = d.off;
        if (strcmp(d.name, ".") && strcmp(d.name, ".."))
            return -ENOTEMPTY;
    }
    o->gen++;
    if (up) {
        /* remove leftover whiteouts inside, then the directory */
        cookie = 0;
        while ((r = o->upper->ops->readdir(o->upper, rel, cookie, &d)) == 1) {
            cookie = d.off;
            if (!strncmp(d.name, WH_PREFIX, 4)) {
                char p[FS_PATH_MAX];
                snprintf(p, sizeof p, "%s/%s", rel, d.name);
                o->upper->ops->unlink(o->upper, p);
                cookie = 0;
            }
        }
        r = o->upper->ops->rmdir(o->upper, rel);
        if (r < 0)
            return r;
    }
    if (o->lower->ops->stat(o->lower, rel, &st) == 0)
        return add_whiteout(o, rel);
    return 0;
}

static int ov_rename(fs_backend_t *b, const char *from, const char *to)
{
    overlay_t *o = b->priv;
    fs_stat_t st;
    bool up;
    int r = merged_stat(o, from, &st, &up);
    if (r < 0)
        return r;
    if (S_ISDIR(st.mode))
        return -EXDEV; /* directory renames are not needed by the guest (2.4 vfat: allowed, but unused) */
    if (!up) {
        r = copy_up(o, from, true);
        if (r < 0)
            return r;
    }
    r = upper_mkparents(o, to);
    if (r < 0)
        return r;
    o->gen++;
    r = o->upper->ops->rename(o->upper, from, to);
    if (r < 0)
        return r;
    remove_whiteout(o, to);
    if (o->lower->ops->stat(o->lower, from, &st) == 0)
        add_whiteout(o, from);
    return 0;
}

static int ov_truncate(fs_backend_t *b, const char *rel, int64_t size)
{
    overlay_t *o = b->priv;
    fs_stat_t st;
    bool up;
    int r = merged_stat(o, rel, &st, &up);
    if (r < 0)
        return r;
    if (!up) {
        r = copy_up(o, rel, true);
        if (r < 0)
            return r;
    }
    o->gen++;
    return o->upper->ops->truncate(o->upper, rel, size);
}

static int ov_chmod(fs_backend_t *b, const char *rel, uint32_t mode)
{
    overlay_t *o = b->priv;
    fs_stat_t st;
    bool up;
    int r = merged_stat(o, rel, &st, &up);
    if (r < 0)
        return r;
    if (!up) {
        r = copy_up(o, rel, true);
        if (r < 0)
            return r;
        o->gen++;
    }
    return o->upper->ops->chmod ? o->upper->ops->chmod(o->upper, rel, mode) : 0;
}

static int ov_statfs(fs_backend_t *b, const char *rel, fs_statfs_t *out)
{
    overlay_t *o = b->priv;
    if (o->lower->ops->statfs)
        return o->lower->ops->statfs(o->lower, "", out);
    (void)rel;
    memset(out, 0, sizeof *out);
    out->bsize = out->frsize = 4096;
    out->namelen = 255;
    return 0;
}

static void ov_destroy(fs_backend_t *b)
{
    overlay_t *o = b->priv;
    if (o->lower->ops->destroy)
        o->lower->ops->destroy(o->lower);
    if (o->upper->ops->destroy)
        o->upper->ops->destroy(o->upper);
    free(o->cache);
    free(o);
    free(b);
}

static const fs_backend_ops_t overlay_ops = {
    .stat = ov_stat, .readlink = ov_readlink, .open = ov_open, .readdir = ov_readdir, .unlink = ov_unlink,
    .mkdir = ov_mkdir, .rmdir = ov_rmdir, .rename = ov_rename, .truncate = ov_truncate, .chmod = ov_chmod, .statfs = ov_statfs,
    .destroy = ov_destroy,
};

fs_backend_t *fs_backend_overlay(fs_backend_t *lower, fs_backend_t *upper, uint64_t dev)
{
    if (!lower || !upper || upper->readonly)
        return NULL;
    fs_backend_t *b = calloc(1, sizeof *b);
    overlay_t *o = calloc(1, sizeof *o);
    if (!b || !o) {
        free(b);
        free(o);
        return NULL;
    }
    o->lower = lower;
    o->upper = upper;
    b->ops = &overlay_ops;
    b->dev = dev;
    b->priv = o;
    return b;
}
