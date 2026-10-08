/* A host directory as a guest backend (the firmware rootfs read-only, the
 * private tmpfs mounts and overlay uppers writable). The guest never sees
 * host paths: every rel path is joined under the configured root, symlinks
 * are reported to the VFS (which resolves them in guest space) and never
 * followed by the host. uid/gid are reported as 0. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

#include "gport2x/fs.h"

typedef struct hostdir {
    char root[FS_PATH_MAX];
    fs_statfs_t sfs;
    bool have_sfs;
    bool sorted;
} hostdir_t;

static int join(const fs_backend_t *b, const char *rel, char *out)
{
    const hostdir_t *h = b->priv;
    int n = *rel ? snprintf(out, FS_PATH_MAX, "%s/%s", h->root, rel) : snprintf(out, FS_PATH_MAX, "%s", h->root);
    return n >= FS_PATH_MAX ? -ENAMETOOLONG : 0;
}

static void map_stat(const fs_backend_t *b, const struct stat *hs, fs_stat_t *st)
{
    memset(st, 0, sizeof *st);
    st->dev = b->dev;
    st->ino = hs->st_ino;
    st->mode = hs->st_mode;
    st->nlink = (uint32_t)hs->st_nlink;
    st->rdev = hs->st_rdev;
    st->size = hs->st_size;
    st->blksize = 4096;
    st->blocks = (uint64_t)hs->st_blocks;
    st->atime = hs->st_atime;
    st->mtime = hs->st_mtime;
    st->ctime = hs->st_ctime;
}

static int hd_stat(fs_backend_t *b, const char *rel, fs_stat_t *st)
{
    char p[FS_PATH_MAX];
    int r = join(b, rel, p);
    if (r < 0)
        return r;
    struct stat hs;
    if (lstat(p, &hs) < 0)
        return -errno;
    map_stat(b, &hs, st);
    return 0;
}

static int hd_readlink(fs_backend_t *b, const char *rel, char *buf, size_t n)
{
    char p[FS_PATH_MAX];
    int r = join(b, rel, p);
    if (r < 0)
        return r;
    ssize_t k = readlink(p, buf, n);
    return k < 0 ? -errno : (int)k;
}

/* ---- regular files ----------------------------------------------------- */

static int64_t hf_read(fs_file_t *f, int64_t off, void *buf, size_t n)
{
    ssize_t r = pread((int)(intptr_t)f->priv, buf, n, (off_t)off);
    return r < 0 ? -errno : r;
}

static int64_t hf_write(fs_file_t *f, int64_t off, const void *buf, size_t n)
{
    ssize_t r = pwrite((int)(intptr_t)f->priv, buf, n, (off_t)off);
    return r < 0 ? -errno : r;
}

static int hf_fstat(fs_file_t *f, fs_stat_t *st)
{
    struct stat hs;
    if (fstat((int)(intptr_t)f->priv, &hs) < 0)
        return -errno;
    map_stat(f->backend, &hs, st);
    return 0;
}

static int hf_ftruncate(fs_file_t *f, int64_t size)
{
    return ftruncate((int)(intptr_t)f->priv, (off_t)size) < 0 ? -errno : 0;
}

static int hf_fsync(fs_file_t *f)
{
    return fsync((int)(intptr_t)f->priv) < 0 ? -errno : 0;
}

static void hf_release(fs_file_t *f)
{
    close((int)(intptr_t)f->priv);
}

static const fs_file_ops_t hostfile_ops = {
    .read = hf_read, .write = hf_write, .fstat = hf_fstat, .ftruncate = hf_ftruncate, .fsync = hf_fsync,
    .release = hf_release,
};

static int hd_open(fs_backend_t *b, const char *rel, int gflags, uint32_t mode, fs_file_t **out)
{
    char p[FS_PATH_MAX];
    int r = join(b, rel, p);
    if (r < 0)
        return r;
    int acc = gflags & GUEST_O_ACCMODE;
    if (b->readonly && (acc != GUEST_O_RDONLY || (gflags & (GUEST_O_CREAT | GUEST_O_TRUNC))))
        return -EROFS;
    int hflags = (acc == GUEST_O_RDONLY ? O_RDONLY : acc == GUEST_O_WRONLY ? O_WRONLY : O_RDWR) | O_CLOEXEC | O_NOFOLLOW;
    if (gflags & GUEST_O_CREAT)
        hflags |= O_CREAT;
    if (gflags & GUEST_O_EXCL)
        hflags |= O_EXCL;
    if (gflags & GUEST_O_TRUNC)
        hflags |= O_TRUNC;
    int fd = open(p, hflags, (mode_t)(mode ? mode : 0644));
    if (fd < 0)
        return -errno;
    struct stat hs;
    if (fstat(fd, &hs) < 0) {
        close(fd);
        return -errno;
    }
    if (!S_ISREG(hs.st_mode)) {
        close(fd);
        return S_ISDIR(hs.st_mode) ? -EISDIR : -ENXIO; /* host device nodes are never exposed */
    }
    fs_file_t *f = fs_file_new(&hostfile_ops, gflags);
    if (!f) {
        close(fd);
        return -ENOMEM;
    }
    f->priv = (void *)(intptr_t)fd;
    f->backend = b;
    *out = f;
    return 0;
}

/* "." and ".." first, then the entries sorted by name: a deterministic
 * order, where the host's hash order would vary between runs (a vfat lists
 * its slots in creation order; sorting is the stable approximation for an
 * upper directory). The cookie is the entry index. */
static int name_cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int hd_readdir(fs_backend_t *b, const char *rel, uint64_t cookie, fs_dirent_t *out)
{
    char p[FS_PATH_MAX];
    int r = join(b, rel, p);
    if (r < 0)
        return r;
    if (cookie < 2) {
        struct stat hs;
        if (stat(p, &hs) < 0)
            return -errno;
        if (!S_ISDIR(hs.st_mode))
            return -ENOTDIR;
        memset(out, 0, sizeof *out);
        strcpy(out->name, cookie == 0 ? "." : "..");
        out->ino = hs.st_ino;
        out->type = GUEST_DT_DIR;
        out->off = cookie + 1;
        return 1;
    }
    DIR *d = opendir(p);
    if (!d)
        return -errno;
    char **names = NULL;
    size_t n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 64;
            names = realloc(names, cap * sizeof *names);
        }
        names[n++] = strdup(de->d_name);
    }
    closedir(d);
    if (((hostdir_t *)b->priv)->sorted)
        qsort(names, n, sizeof *names, name_cmp);
    uint64_t idx = cookie - 2;
    r = 0;
    if (idx < n) {
        char q[FS_PATH_MAX + 300];
        struct stat hs;
        snprintf(q, sizeof q, "%s/%s", p, names[idx]);
        memset(out, 0, sizeof *out);
        snprintf(out->name, sizeof out->name, "%s", names[idx]);
        if (lstat(q, &hs) == 0) {
            out->ino = hs.st_ino;
            out->type = S_ISDIR(hs.st_mode) ? GUEST_DT_DIR : S_ISLNK(hs.st_mode) ? GUEST_DT_LNK : S_ISCHR(hs.st_mode) ? GUEST_DT_CHR
                      : S_ISBLK(hs.st_mode) ? GUEST_DT_BLK : S_ISFIFO(hs.st_mode) ? GUEST_DT_FIFO : GUEST_DT_REG;
        } else {
            out->type = GUEST_DT_UNKNOWN;
        }
        out->off = cookie + 1;
        r = 1;
    }
    for (size_t i = 0; i < n; i++)
        free(names[i]);
    free(names);
    return r;
}

static int hd_unlink(fs_backend_t *b, const char *rel)
{
    char p[FS_PATH_MAX];
    int r = join(b, rel, p);
    if (r < 0)
        return r;
    if (b->readonly)
        return -EROFS;
    return unlink(p) < 0 ? -errno : 0;
}

static int hd_mkdir(fs_backend_t *b, const char *rel, uint32_t mode)
{
    char p[FS_PATH_MAX];
    int r = join(b, rel, p);
    if (r < 0)
        return r;
    if (b->readonly)
        return -EROFS;
    return mkdir(p, (mode_t)(mode ? mode : 0755)) < 0 ? -errno : 0;
}

static int hd_rmdir(fs_backend_t *b, const char *rel)
{
    char p[FS_PATH_MAX];
    int r = join(b, rel, p);
    if (r < 0)
        return r;
    if (b->readonly)
        return -EROFS;
    return rmdir(p) < 0 ? -errno : 0;
}

static int hd_rename(fs_backend_t *b, const char *from, const char *to)
{
    char p[FS_PATH_MAX], q[FS_PATH_MAX];
    int r = join(b, from, p);
    if (r < 0)
        return r;
    r = join(b, to, q);
    if (r < 0)
        return r;
    if (b->readonly)
        return -EROFS;
    return rename(p, q) < 0 ? -errno : 0;
}

static int hd_truncate(fs_backend_t *b, const char *rel, int64_t size)
{
    char p[FS_PATH_MAX];
    int r = join(b, rel, p);
    if (r < 0)
        return r;
    if (b->readonly)
        return -EROFS;
    return truncate(p, (off_t)size) < 0 ? -errno : 0;
}

static int hd_chmod(fs_backend_t *b, const char *rel, uint32_t mode)
{
    char p[FS_PATH_MAX];
    int r = join(b, rel, p);
    if (r < 0)
        return r;
    if (b->readonly)
        return -EROFS;
    return chmod(p, (mode_t)mode) < 0 ? -errno : 0;
}

static int hd_statfs(fs_backend_t *b, const char *rel, fs_statfs_t *out)
{
    hostdir_t *h = b->priv;
    if (h->have_sfs) {
        *out = h->sfs;
        return 0;
    }
    char p[FS_PATH_MAX];
    int r = join(b, rel, p);
    if (r < 0)
        return r;
    struct statfs hs;
    if (statfs(p, &hs) < 0)
        return -errno;
    memset(out, 0, sizeof *out);
    out->type = (uint32_t)hs.f_type;
    out->bsize = (uint32_t)hs.f_bsize;
    out->frsize = (uint32_t)hs.f_frsize;
    out->blocks = (uint32_t)hs.f_blocks;
    out->bfree = (uint32_t)hs.f_bfree;
    out->bavail = (uint32_t)hs.f_bavail;
    out->files = (uint32_t)hs.f_files;
    out->ffree = (uint32_t)hs.f_ffree;
    out->namelen = (uint32_t)hs.f_namelen;
    return 0;
}

static void hd_destroy(fs_backend_t *b)
{
    free(b->priv);
    free(b);
}

static const fs_backend_ops_t hostdir_ops = {
    .stat = hd_stat, .readlink = hd_readlink, .open = hd_open, .readdir = hd_readdir, .unlink = hd_unlink,
    .mkdir = hd_mkdir, .rmdir = hd_rmdir, .rename = hd_rename, .truncate = hd_truncate, .chmod = hd_chmod, .statfs = hd_statfs,
    .destroy = hd_destroy,
};

fs_backend_t *fs_backend_hostdir(const char *host_path, bool readonly, uint64_t dev)
{
    fs_backend_t *b = calloc(1, sizeof *b);
    hostdir_t *h = calloc(1, sizeof *h);
    if (!b || !h) {
        free(b);
        free(h);
        return NULL;
    }
    snprintf(h->root, sizeof h->root, "%s", host_path);
    size_t n = strlen(h->root);
    while (n > 1 && h->root[n - 1] == '/')
        h->root[--n] = 0;
    h->sorted = true;
    b->ops = &hostdir_ops;
    b->dev = dev;
    b->readonly = readonly;
    b->priv = h;
    return b;
}

void fs_hostdir_set_sorted(fs_backend_t *b, bool sorted)
{
    ((hostdir_t *)b->priv)->sorted = sorted;
}

void fs_hostdir_set_statfs(fs_backend_t *b, const fs_statfs_t *values)
{
    hostdir_t *h = b->priv;
    h->sfs = *values;
    h->have_sfs = true;
}
