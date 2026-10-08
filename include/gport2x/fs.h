/* GPort2X guest filesystem namespace (spec 6.4, 7.5, #7, #22).
 *
 * The guest sees only the GP2X filesystem: a mount table of backends that
 * never falls through to host paths. Backends: a host directory (read-only
 * or writable: the firmware rootfs, the private tmpfs mounts), the card
 * image (card.h), a copy-on-write overlay (card + saves, rootfs + scratch),
 * the /proc subset and /dev nodes registered by the device module.
 *
 * Every operation is path-based: the VFS normalises the guest path against
 * the caller's cwd, resolves symlinks component by component through the
 * mount table, and hands the remainder to the backend that owns the longest
 * matching mount point. Open files (fs_file_t) are refcounted descriptions
 * shared by dup'd descriptors and across fork, as in the kernel.
 *
 * Blocking: file operations never block the host. One that would block
 * returns -EAGAIN even on a blocking descriptor; the syscall layer parks the
 * guest thread and retries (spec 3.4 blocking /dev/dsp writes, pipes).
 *
 * Results are 0/positive or a negative errno (Linux values; the guest is
 * Linux/ARM so they pass through unchanged). */
#ifndef GPORT2X_FS_H
#define GPORT2X_FS_H

#include <stdio.h>

#include "gport2x/card.h"
#include "gport2x/gmem.h"
#include "gport2x/types.h"

#define FS_PATH_MAX 4096
#define FS_NAME_MAX 255

/* Guest open(2) flags (asm-arm fcntl.h; they differ from x86 for
 * O_DIRECTORY, O_NOFOLLOW, O_LARGEFILE and O_DIRECT). */
enum {
    GUEST_O_ACCMODE = 00000003,
    GUEST_O_RDONLY = 00000000,
    GUEST_O_WRONLY = 00000001,
    GUEST_O_RDWR = 00000002,
    GUEST_O_CREAT = 00000100,
    GUEST_O_EXCL = 00000200,
    GUEST_O_NOCTTY = 00000400,
    GUEST_O_TRUNC = 00001000,
    GUEST_O_APPEND = 00002000,
    GUEST_O_NONBLOCK = 00004000,
    GUEST_O_SYNC = 00010000,
    GUEST_O_DIRECTORY = 00040000,
    GUEST_O_NOFOLLOW = 00100000,
    GUEST_O_DIRECT = 00200000,
    GUEST_O_LARGEFILE = 00400000,
};

typedef struct fs fs_t;
typedef struct fs_backend fs_backend_t;
typedef struct fs_file fs_file_t;
typedef struct fs_fdtable fs_fdtable_t;

typedef struct fs_stat {
    uint64_t dev, ino, rdev;
    uint32_t mode, nlink, uid, gid;
    int64_t size;
    uint32_t blksize;
    uint64_t blocks;
    int64_t atime, mtime, ctime;
} fs_stat_t;

typedef struct fs_statfs {
    uint32_t type, bsize, frsize, blocks, bfree, bavail, files, ffree, namelen;
} fs_statfs_t;

typedef struct fs_dirent {
    char name[FS_NAME_MAX + 1];
    uint64_t ino;
    uint64_t off;  /* the cookie after this entry */
    uint8_t type;  /* DT_* */
} fs_dirent_t;

/* The calling process's view: its cwd, pid (for /proc/self) and umask. */
typedef struct fs_ctx {
    int pid;
    unsigned umask;
    char cwd[FS_PATH_MAX];
} fs_ctx_t;

/* ---- backends ---------------------------------------------------------- */

/* Paths given to a backend are relative to its mount root, normalised, with
 * no leading '/', no "." or ".." components; "" is the root itself. lstat
 * semantics: the VFS follows symlinks. */
typedef struct fs_backend_ops {
    int (*stat)(fs_backend_t *b, const char *rel, fs_stat_t *st);
    int (*readlink)(fs_backend_t *b, const char *rel, char *buf, size_t n); /* returns length */
    int (*open)(fs_backend_t *b, const char *rel, int gflags, uint32_t mode, fs_file_t **out);
    int (*readdir)(fs_backend_t *b, const char *rel, uint64_t cookie, fs_dirent_t *out); /* 1 / 0 / -errno */
    int (*unlink)(fs_backend_t *b, const char *rel);
    int (*mkdir)(fs_backend_t *b, const char *rel, uint32_t mode);
    int (*rmdir)(fs_backend_t *b, const char *rel);
    int (*rename)(fs_backend_t *b, const char *from, const char *to);
    int (*truncate)(fs_backend_t *b, const char *rel, int64_t size);
    int (*chmod)(fs_backend_t *b, const char *rel, uint32_t mode);
    int (*statfs)(fs_backend_t *b, const char *rel, fs_statfs_t *out);
    void (*destroy)(fs_backend_t *b);
} fs_backend_ops_t;

struct fs_backend {
    const fs_backend_ops_t *ops;
    uint64_t dev;   /* st_dev reported for everything on this backend */
    bool readonly;
    void *priv;
};

typedef struct fs_file_ops {
    int64_t (*read)(fs_file_t *f, int64_t off, void *buf, size_t n);
    int64_t (*write)(fs_file_t *f, int64_t off, const void *buf, size_t n);
    int (*fstat)(fs_file_t *f, fs_stat_t *st);
    int (*readdir)(fs_file_t *f, uint64_t cookie, fs_dirent_t *out);
    int (*ioctl)(fs_file_t *f, uint32_t req, gmem_t *m, gaddr_t arg);
    /* Returns the object to map and the object offset for file offset off;
     * the caller maps len bytes. Regular files are served by the VFS. */
    int (*mmap)(fs_file_t *f, uint64_t off, uint32_t len, int prot, bool shared, gmem_obj_t **obj, uint32_t *obj_off);
    int (*ftruncate)(fs_file_t *f, int64_t size);
    int (*fsync)(fs_file_t *f);
    int (*poll)(fs_file_t *f, int events); /* returns revents */
    void (*release)(fs_file_t *f);
} fs_file_ops_t;

struct fs_file {
    const fs_file_ops_t *ops;
    int refs;
    int gflags;       /* GUEST_O_* access mode and status flags (O_APPEND, O_NONBLOCK) */
    int64_t pos;
    bool is_dir;
    bool seekable;
    fs_backend_t *backend;
    char path[FS_PATH_MAX]; /* absolute guest path (for /proc/self/fd and diagnostics) */
    void *priv;
};

fs_file_t *fs_file_new(const fs_file_ops_t *ops, int gflags);
fs_file_t *fs_file_ref(fs_file_t *f);
void fs_file_unref(fs_file_t *f);

/* ---- the namespace ----------------------------------------------------- */

fs_t *fs_create(void);
void fs_destroy(fs_t *fs);

/* Mounts b at target (an absolute path; "/" first). source, fstype and
 * options are what /proc/mounts shows. The VFS owns b afterwards. */
int fs_mount(fs_t *fs, const char *target, fs_backend_t *b, const char *source, const char *fstype, const char *options);
int fs_umount(fs_t *fs, const char *target); /* -EINVAL if not a mount point (2.4) */
/* Writes the /proc/mounts text. Returns the length. */
int fs_mounts_text(fs_t *fs, char *buf, size_t n);

void fs_ctx_init(fs_ctx_t *ctx, int pid, const char *cwd);
/* The ctx of the VFS call in progress (for backends: /proc/self). */
const fs_ctx_t *fs_current_ctx(void);

/* Normalises path against ctx->cwd without touching the mount table:
 * absolute, no "." / ".." / "//". Returns the length or -ENAMETOOLONG. */
int fs_normalize(const fs_ctx_t *ctx, const char *path, char *out);
/* Resolves symlinks too (the last component only when follow). */
int fs_resolve(fs_t *fs, const fs_ctx_t *ctx, const char *path, bool follow, char *out);

int fs_open(fs_t *fs, fs_ctx_t *ctx, const char *path, int gflags, uint32_t mode, fs_file_t **out);
int fs_stat(fs_t *fs, fs_ctx_t *ctx, const char *path, bool follow, fs_stat_t *out);
int fs_readlink(fs_t *fs, fs_ctx_t *ctx, const char *path, char *buf, size_t n);
int fs_access(fs_t *fs, fs_ctx_t *ctx, const char *path, int mode);
int fs_unlink(fs_t *fs, fs_ctx_t *ctx, const char *path);
int fs_mkdir(fs_t *fs, fs_ctx_t *ctx, const char *path, uint32_t mode);
int fs_rmdir(fs_t *fs, fs_ctx_t *ctx, const char *path);
int fs_rename(fs_t *fs, fs_ctx_t *ctx, const char *from, const char *to);
int fs_truncate(fs_t *fs, fs_ctx_t *ctx, const char *path, int64_t size);
int fs_chmod(fs_t *fs, fs_ctx_t *ctx, const char *path, uint32_t mode);
int fs_statfs(fs_t *fs, fs_ctx_t *ctx, const char *path, fs_statfs_t *out);
int fs_chdir(fs_t *fs, fs_ctx_t *ctx, const char *path);
/* Reads a whole file into a malloc'd buffer (for execve and the loader). */
int fs_read_file(fs_t *fs, fs_ctx_t *ctx, const char *path, uint8_t **data, size_t *len);

/* File operations (pos-based ones update f->pos). */
int64_t fs_file_read(fs_file_t *f, void *buf, size_t n);
int64_t fs_file_write(fs_file_t *f, const void *buf, size_t n);
int64_t fs_file_pread(fs_file_t *f, int64_t off, void *buf, size_t n);
int64_t fs_file_pwrite(fs_file_t *f, int64_t off, const void *buf, size_t n);
int64_t fs_file_lseek(fs_file_t *f, int64_t off, int whence);
int fs_file_fstat(fs_file_t *f, fs_stat_t *st);
int fs_file_readdir(fs_file_t *f, fs_dirent_t *out); /* uses and advances f->pos as the cookie */
int fs_file_ioctl(fs_file_t *f, uint32_t req, gmem_t *m, gaddr_t arg);
int fs_file_mmap(fs_file_t *f, uint64_t off, uint32_t len, int prot, bool shared, gmem_obj_t **obj, uint32_t *obj_off);
int fs_file_ftruncate(fs_file_t *f, int64_t size);
int fs_file_fsync(fs_file_t *f);
int fs_file_poll(fs_file_t *f, int events);

/* ---- descriptor tables ------------------------------------------------- */

fs_fdtable_t *fs_fdt_create(void);
fs_fdtable_t *fs_fdt_ref(fs_fdtable_t *t);  /* CLONE_FILES */
fs_fdtable_t *fs_fdt_clone(fs_fdtable_t *t); /* fork: new table, same open files */
void fs_fdt_unref(fs_fdtable_t *t);
fs_file_t *fs_fdt_get(fs_fdtable_t *t, int fd); /* borrowed; NULL if closed */
int fs_fdt_install(fs_fdtable_t *t, fs_file_t *f, bool cloexec, int minfd); /* takes a ref; returns the fd */
int fs_fdt_close(fs_fdtable_t *t, int fd);
int fs_fdt_dup2(fs_fdtable_t *t, int oldfd, int newfd);
int fs_fdt_get_cloexec(fs_fdtable_t *t, int fd);
int fs_fdt_set_cloexec(fs_fdtable_t *t, int fd, bool cloexec);
void fs_fdt_close_on_exec(fs_fdtable_t *t);
int fs_fdt_max(const fs_fdtable_t *t);

/* ---- host stdio ---------------------------------------------------------- */

/* A guest file that passes through to a host descriptor (the harness's own
 * stdin/stdout/stderr for guest fds 0..2). Reads and writes go straight
 * through; fstat reports a character device; ioctls are ENOTTY. */
fs_file_t *fs_file_hostfd(int host_fd, int gflags);
fs_file_t *fs_file_hostfd_owned(int host_fd, int gflags); /* closes the descriptor on release; polls it */

/* ---- pipes ------------------------------------------------------------- */

int fs_pipe_create(fs_file_t **rd, fs_file_t **wr);

/* ---- backends ---------------------------------------------------------- */

/* A host directory. Guest uid/gid are reported as 0. */
fs_backend_t *fs_backend_hostdir(const char *host_path, bool readonly, uint64_t dev);
/* Fixed statfs values to report (e.g. the firmware's yaffs or a tmpfs size). */
void fs_hostdir_set_statfs(fs_backend_t *b, const fs_statfs_t *values);
/* Listing order: sorted by name (the default: deterministic, the stable
 * approximation of a vfat's slot order for a writable upper) or the host's. */
void fs_hostdir_set_sorted(fs_backend_t *b, bool sorted);
/* The card image, read-only, with its genuine semantics. The backend owns card. */
fs_backend_t *fs_backend_card(card_t *card);
/* Copy-on-write: reads from lower unless upper has the entry (or a
 * whiteout); writes copy up into upper. statfs comes from lower. The overlay
 * owns both backends. */
fs_backend_t *fs_backend_overlay(fs_backend_t *lower, fs_backend_t *upper, uint64_t dev);

/* The /proc subset. The process module supplies the facts. */
typedef struct fs_procinfo_ops {
    int (*exe)(void *ctx, int pid, char *buf, size_t n);     /* -ESRCH if no such pid */
    int (*cmdline)(void *ctx, int pid, char *buf, size_t n); /* NUL-separated; returns the length */
    int (*status)(void *ctx, int pid, char *buf, size_t n);  /* the /proc/pid/status text */
    int (*cwd)(void *ctx, int pid, char *buf, size_t n);     /* the process's cwd */
    int (*maps)(void *ctx, int pid, char *buf, size_t n);    /* the /proc/pid/maps text */
    int (*pids)(void *ctx, int *out, int max);               /* the live pids, for readdir */
} fs_procinfo_ops_t;
fs_backend_t *fs_backend_proc(fs_t *fs, const fs_procinfo_ops_t *ops, void *ctx, const char *kernel_version);

/* The /dev tree. Nodes are registered by path (e.g. "sound/dsp"); open
 * calls the node's function to create the file. Directories appear as
 * needed. The generic nodes (null, zero, tty, console, tty0, vc/0, urandom,
 * pts/) are registered by fs_backend_devfs itself. */
typedef int (*fs_devopen_fn)(void *ctx, int gflags, fs_file_t **out);
fs_backend_t *fs_backend_devfs(void);
/* Where console device writes go (default stderr). */
void fs_devfs_set_console(fs_backend_t *devfs, FILE *out);
int fs_devfs_register(fs_backend_t *devfs, const char *rel, uint32_t mode, uint64_t rdev, fs_devopen_fn open, void *ctx);
int fs_devfs_symlink(fs_backend_t *devfs, const char *rel, const char *target);

/* Guest-side constants the sys layer needs. */
enum { GUEST_DT_UNKNOWN = 0, GUEST_DT_FIFO = 1, GUEST_DT_CHR = 2, GUEST_DT_DIR = 4, GUEST_DT_BLK = 6, GUEST_DT_REG = 8, GUEST_DT_LNK = 10, GUEST_DT_SOCK = 12 };
enum { GUEST_POLLIN = 1, GUEST_POLLPRI = 2, GUEST_POLLOUT = 4, GUEST_POLLERR = 8, GUEST_POLLHUP = 16, GUEST_POLLNVAL = 32 };

#endif /* GPORT2X_FS_H */
