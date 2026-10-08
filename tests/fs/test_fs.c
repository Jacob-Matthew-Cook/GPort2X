/* fs module tests: path normalisation and symlink resolution, host-directory
 * backends (read-only and writable), the copy-on-write overlay (copy-up,
 * whiteouts, merged listings, lower statfs), /proc, /dev, descriptor tables
 * and pipes. Builds its own host tree in a temporary directory. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gport2x/fs.h"
#include "gport2x/log.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char tmproot[256];

static void host_write(const char *rel, const char *content)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", tmproot, rel);
    FILE *f = fopen(p, "wb");
    if (!f) { perror(p); exit(2); }
    fputs(content, f);
    fclose(f);
}

static void host_mkdir(const char *rel)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", tmproot, rel);
    mkdir(p, 0755);
}

static void host_symlink(const char *target, const char *rel)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", tmproot, rel);
    if (symlink(target, p) < 0) { perror(p); exit(2); }
}

static int read_all(fs_t *fs, fs_ctx_t *ctx, const char *path, char *buf, size_t n)
{
    uint8_t *d;
    size_t len;
    int r = fs_read_file(fs, ctx, path, &d, &len);
    if (r < 0)
        return r;
    if (len >= n)
        len = n - 1;
    memcpy(buf, d, len);
    buf[len] = 0;
    free(d);
    return (int)len;
}

static int listing(fs_t *fs, fs_ctx_t *ctx, const char *path, char *out, size_t n)
{
    fs_file_t *f;
    int r = fs_open(fs, ctx, path, GUEST_O_RDONLY | GUEST_O_DIRECTORY, 0, &f);
    if (r < 0)
        return r;
    out[0] = 0;
    fs_dirent_t d;
    while ((r = fs_file_readdir(f, &d)) == 1) {
        strncat(out, d.name, n - strlen(out) - 2);
        strncat(out, " ", n - strlen(out) - 2);
    }
    fs_file_unref(f);
    return r;
}

static int pi_exe(void *c, int pid, char *buf, size_t n) { (void)c; return pid == 7 ? snprintf(buf, n, "/mnt/tmp/Payback_tmp") : -ESRCH; }
static int pi_cmdline(void *c, int pid, char *buf, size_t n) { (void)c; (void)pid; memcpy(buf, "./Payback\0", 10); (void)n; return 10; }
static int pi_status(void *c, int pid, char *buf, size_t n) { (void)c; return snprintf(buf, n, "Name:\tPayback\nPid:\t%d\n", pid); }
static int pi_cwd(void *c, int pid, char *buf, size_t n) { (void)c; (void)pid; return snprintf(buf, n, "/mnt/sd/Payback"); }
static int pi_maps(void *c, int pid, char *buf, size_t n) { (void)c; (void)pid; return snprintf(buf, n, "00008000-009e9000 rwxp 00000000 00:00 0\n"); }
static int pi_pids(void *c, int *out, int max) { (void)c; (void)max; out[0] = 1; out[1] = 7; return 2; }

int main(void)
{
    gp_log_init_from_env();
    snprintf(tmproot, sizeof tmproot, "/tmp/gport2x-fs-XXXXXX");
    if (!mkdtemp(tmproot)) { perror("mkdtemp"); return 2; }
    /* firmware-like read-only root */
    host_mkdir("root"); host_mkdir("root/bin"); host_mkdir("root/lib"); host_mkdir("root/etc"); host_mkdir("root/mnt");
    host_mkdir("root/mnt/sd"); host_mkdir("root/proc"); host_mkdir("root/dev"); host_mkdir("root/tmp"); host_mkdir("root/usr");
    host_write("root/bin/sh", "#!/bin/busybox\n");
    host_write("root/lib/libc.so.6", "LIBC");
    host_symlink("libc.so.6", "root/lib/libc-2.2.5.so.link");
    host_symlink("/lib/libc.so.6", "root/etc/abslink");
    host_symlink("../lib", "root/usr/liblink");
    host_symlink("loop2", "root/etc/loop1");
    host_symlink("loop1", "root/etc/loop2");
    host_write("root/etc/fstab", "none /tmp tmpfs size=5M 0 0\n");
    /* card-like lower */
    host_mkdir("card"); host_mkdir("card/Payback"); host_mkdir("card/Payback/Data"); host_mkdir("card/Payback/Data/Config");
    host_mkdir("card/Payback/Data/Replays");
    host_write("card/Payback/Payback", "STUB");
    { char q[512]; snprintf(q, sizeof q, "%s/card/Payback/Payback", tmproot); chmod(q, 0755); } /* vfat reports 0755 */
    host_write("card/Payback/Data/Config/game.ini", "volume=50\n");
    host_write("card/Payback/Data/Replays/old.rec", "OLD");
    host_write("card/autorun.gpu", "#!/bin/bash\n");
    host_mkdir("saves"); host_mkdir("scratch"); host_mkdir("tmpfs");

    char p[512];
    fs_t *fs = fs_create();
    snprintf(p, sizeof p, "%s/root", tmproot);
    fs_backend_t *root_lower = fs_backend_hostdir(p, true, 0x1F03);
    snprintf(p, sizeof p, "%s/scratch", tmproot);
    fs_backend_t *root_upper = fs_backend_hostdir(p, false, 0x1F03);
    CHECK(fs_mount(fs, "/mnt/sd", fs_backend_hostdir(p, true, 1), "x", "y", "z") == -EINVAL, "root must be mounted first");
    CHECK(fs_mount(fs, "/", fs_backend_overlay(root_lower, root_upper, 0x1F03), "/dev/root", "yaffs", "rw,sync,noatime") == 0, "mount /");
    snprintf(p, sizeof p, "%s/card", tmproot);
    fs_backend_t *card_lower = fs_backend_hostdir(p, true, 0xF101);
    fs_statfs_t geom = { .type = 0x4D44, .bsize = 32768, .frsize = 32768, .blocks = 15625, .bfree = 7247, .bavail = 7247, .namelen = 260 };
    fs_hostdir_set_statfs(card_lower, &geom);
    snprintf(p, sizeof p, "%s/saves", tmproot);
    fs_backend_t *saves = fs_backend_hostdir(p, false, 0xF101);
    CHECK(fs_mount(fs, "/mnt/sd", fs_backend_overlay(card_lower, saves, 0xF101), "/dev/mmcsd/disc0/part1", "vfat", "rw,sync,noatime,iocharset=utf8") == 0, "mount card");
    static const fs_procinfo_ops_t pi = { pi_exe, pi_cmdline, pi_status, pi_cwd, pi_maps, pi_pids };
    CHECK(fs_mount(fs, "/proc", fs_backend_proc(fs, &pi, NULL, "2.4.25|#1 Mon Jan 1 00:00:00 KST 2007"), "none", "proc", "rw") == 0, "mount proc");
    CHECK(fs_mount(fs, "/dev", fs_backend_devfs(), "none", "devfs", "rw") == 0, "mount dev");
    snprintf(p, sizeof p, "%s/tmpfs", tmproot);
    CHECK(fs_mount(fs, "/tmp", fs_backend_hostdir(p, false, 9), "none", "tmpfs", "rw") == 0, "mount tmp");

    fs_ctx_t ctx;
    fs_ctx_init(&ctx, 7, "/mnt/sd/Payback");
    char out[FS_PATH_MAX];

    /* normalisation */
    fs_normalize(&ctx, "Data/../Data/./Maps//0.lmd", out); CHECK(!strcmp(out, "/mnt/sd/Payback/Data/Maps/0.lmd"), "normalize relative: %s", out);
    fs_normalize(&ctx, "/../a/b/../c/", out); CHECK(!strcmp(out, "/a/c"), "normalize absolute: %s", out);
    fs_normalize(&ctx, "..", out); CHECK(!strcmp(out, "/mnt/sd"), "normalize ..: %s", out);
    fs_normalize(&ctx, "/", out); CHECK(!strcmp(out, "/"), "normalize /: %s", out);
    fs_normalize(&ctx, "../../../../..", out); CHECK(!strcmp(out, "/"), "normalize beyond root: %s", out);

    /* symlink resolution across the mount table (host symlinks never followed by the host) */
    CHECK(fs_resolve(fs, &ctx, "/lib/libc-2.2.5.so.link", true, out) == 0 && !strcmp(out, "/lib/libc.so.6"), "relative symlink: %s", out);
    CHECK(fs_resolve(fs, &ctx, "/etc/abslink", true, out) == 0 && !strcmp(out, "/lib/libc.so.6"), "absolute symlink: %s", out);
    CHECK(fs_resolve(fs, &ctx, "/usr/liblink/libc.so.6", true, out) == 0 && !strcmp(out, "/lib/libc.so.6"), "symlink in the middle: %s", out);
    CHECK(fs_resolve(fs, &ctx, "/etc/loop1", true, out) == -ELOOP, "symlink loop");
    CHECK(fs_resolve(fs, &ctx, "/etc/abslink", false, out) == 0 && !strcmp(out, "/etc/abslink"), "nofollow keeps the link");
    char buf[256];
    int n = fs_readlink(fs, &ctx, "/etc/abslink", buf, sizeof buf);
    CHECK(n == 14 && !strncmp(buf, "/lib/libc.so.6", 14), "readlink");
    CHECK(fs_readlink(fs, &ctx, "/etc/fstab", buf, sizeof buf) == -EINVAL, "readlink of a file");
    CHECK(fs_resolve(fs, &ctx, "/etc/fstab/x", true, out) == -ENOTDIR, "file in the middle");
    CHECK(fs_resolve(fs, &ctx, "/nonexistent/x", true, out) == -ENOENT, "missing dir in the middle");

    /* /proc/self */
    n = fs_readlink(fs, &ctx, "/proc/self", buf, sizeof buf); CHECK(n == 1 && buf[0] == '7', "/proc/self -> pid");
    n = fs_readlink(fs, &ctx, "/proc/self/exe", buf, sizeof buf); CHECK(n == 20 && !strncmp(buf, "/mnt/tmp/Payback_tmp", 20), "/proc/self/exe");
    n = fs_readlink(fs, &ctx, "/proc/self/cwd", buf, sizeof buf); CHECK(n == 15 && !strncmp(buf, "/mnt/sd/Payback", 15), "/proc/self/cwd");
    CHECK(read_all(fs, &ctx, "/proc/sys/kernel/version", buf, sizeof buf) > 0 && !strcmp(buf, "#1 Mon Jan 1 00:00:00 KST 2007\n"), "kernel/version: %s", buf);
    CHECK(read_all(fs, &ctx, "/proc/sys/kernel/osrelease", buf, sizeof buf) > 0 && !strcmp(buf, "2.4.25\n"), "osrelease: %s", buf);
    CHECK(read_all(fs, &ctx, "/proc/self/cmdline", buf, sizeof buf) == 10 && !strcmp(buf, "./Payback"), "cmdline");
    CHECK(read_all(fs, &ctx, "/proc/7/status", buf, sizeof buf) > 0 && strstr(buf, "Pid:\t7"), "status");
    CHECK(read_all(fs, &ctx, "/proc/mounts", buf, sizeof buf) > 0, "mounts readable");
    CHECK(strstr(buf, "/dev/mmcsd/disc0/part1 /mnt/sd vfat rw,sync,noatime,iocharset=utf8 0 0\n") && strstr(buf, "/dev/root / yaffs rw,sync,noatime 0 0\n"), "mounts content:\n%s", buf);
    fs_stat_t st;
    CHECK(fs_stat(fs, &ctx, "/proc/99", true, &st) == -ENOENT, "unknown pid");
    CHECK(fs_stat(fs, &ctx, "/proc/self", false, &st) == 0 && S_ISLNK(st.mode), "lstat /proc/self is a symlink");
    CHECK(fs_stat(fs, &ctx, "/proc/self", true, &st) == 0 && S_ISDIR(st.mode), "stat /proc/self is the pid dir");
    CHECK(listing(fs, &ctx, "/proc", out, sizeof out) == 0 && strstr(out, ". .. mounts sys self") && strstr(out, " 1 7 "), "/proc listing: %s", out);

    /* statvfs-style mount scan: stat each mount point, dev ids distinct per mount */
    fs_stat_t s_root, s_sd, s_tmp, s_proc, s_file;
    CHECK(fs_stat(fs, &ctx, "/", true, &s_root) == 0 && fs_stat(fs, &ctx, "/mnt/sd", true, &s_sd) == 0 && fs_stat(fs, &ctx, "/tmp", true, &s_tmp) == 0 && fs_stat(fs, &ctx, "/proc", true, &s_proc) == 0, "mount points stat");
    CHECK(fs_stat(fs, &ctx, "Payback", true, &s_file) == 0 && s_file.dev == s_sd.dev && s_file.dev == 0xF101 && s_file.dev != s_root.dev, "st_dev matches the mount");
    CHECK(s_root.dev == 0x1F03 && s_tmp.dev == 9 && s_proc.dev == 3, "mount devs %llx %llx %llx", (unsigned long long)s_root.dev, (unsigned long long)s_tmp.dev, (unsigned long long)s_proc.dev);
    fs_statfs_t sf;
    CHECK(fs_statfs(fs, &ctx, "Payback", &sf) == 0 && sf.frsize == 32768 && sf.blocks == 15625 && sf.type == 0x4D44 && sf.namelen == 260, "statfs through the overlay is lower's: %u %u", sf.frsize, sf.blocks);

    /* read-only firmware root via overlay: reads, writes copy up, new files land in upper */
    CHECK(read_all(fs, &ctx, "/bin/sh", buf, sizeof buf) == 15 && !strcmp(buf, "#!/bin/busybox\n"), "read firmware file");
    fs_file_t *f;
    CHECK(fs_open(fs, &ctx, "/etc/fstab", GUEST_O_WRONLY | GUEST_O_APPEND, 0, &f) == 0, "open lower file for append (copy-up)");
    CHECK(fs_file_write(f, "x\n", 2) == 2, "append write");
    fs_file_unref(f);
    CHECK(read_all(fs, &ctx, "/etc/fstab", buf, sizeof buf) == 30 && !strcmp(buf, "none /tmp tmpfs size=5M 0 0\nx\n"), "copy-up preserved content: %s", buf);
    snprintf(p, sizeof p, "%s/root/etc/fstab", tmproot);
    struct stat hs; CHECK(stat(p, &hs) == 0 && hs.st_size == 28, "lower untouched");
    snprintf(p, sizeof p, "%s/scratch/etc/fstab", tmproot);
    CHECK(stat(p, &hs) == 0 && hs.st_size == 30, "upper has the copy");
    CHECK(fs_mkdir(fs, &ctx, "/mnt/tmp", 0755) == 0, "mkdir on the overlay root");
    CHECK(fs_mkdir(fs, &ctx, "/mnt/tmp", 0755) == -EEXIST, "mkdir existing");
    CHECK(fs_mkdir(fs, &ctx, "/mnt", 0755) == -EEXIST, "mkdir existing lower dir");
    CHECK(fs_stat(fs, &ctx, "/mnt/tmp", true, &st) == 0 && S_ISDIR(st.mode), "new dir visible");
    CHECK(listing(fs, &ctx, "/mnt", out, sizeof out) == 0 && !strcmp(out, ". .. sd tmp "), "merged listing lower then upper: '%s'", out);
    CHECK(fs_open(fs, &ctx, "/usr/gp2x/common.ini", GUEST_O_WRONLY | GUEST_O_CREAT | GUEST_O_TRUNC, 0644, &f) == -ENOENT, "create needs the parent");
    CHECK(fs_mkdir(fs, &ctx, "/usr/gp2x", 0755) == 0 && fs_open(fs, &ctx, "/usr/gp2x/common.ini", GUEST_O_WRONLY | GUEST_O_CREAT | GUEST_O_TRUNC, 0644, &f) == 0, "create in a new upper dir");
    CHECK(fs_file_write(f, "[a]\n", 4) == 4, "write new file"); fs_file_unref(f);
    CHECK(fs_stat(fs, &ctx, "/usr/gp2x/common.ini", true, &st) == 0 && st.size == 4 && S_ISREG(st.mode) && st.uid == 0, "new file stat");

    /* card overlay: game writes, whiteouts, order */
    CHECK(fs_open(fs, &ctx, "Data/Config/game.ini", GUEST_O_RDWR, 0, &f) == 0, "open config RDWR copies up");
    CHECK(fs_file_lseek(f, 0, 2) == 10 && fs_file_write(f, "x=1\n", 4) == 4, "write at end"); fs_file_unref(f);
    CHECK(read_all(fs, &ctx, "Data/Config/game.ini", buf, sizeof buf) == 14, "config has both parts");
    CHECK(fs_open(fs, &ctx, "Data/Replays/new.rec", GUEST_O_WRONLY | GUEST_O_CREAT | GUEST_O_TRUNC, 0666, &f) == 0, "create replay"); fs_file_write(f, "NEW", 3); fs_file_unref(f);
    CHECK(listing(fs, &ctx, "Data/Replays", out, sizeof out) == 0 && !strcmp(out, ". .. old.rec new.rec "), "replay listing: '%s'", out);
    CHECK(fs_unlink(fs, &ctx, "Data/Replays/old.rec") == 0, "unlink lower replay (whiteout)");
    CHECK(fs_stat(fs, &ctx, "Data/Replays/old.rec", true, &st) == -ENOENT, "whited out");
    CHECK(listing(fs, &ctx, "Data/Replays", out, sizeof out) == 0 && !strcmp(out, ". .. new.rec "), "listing after whiteout: '%s'", out);
    CHECK(fs_stat(fs, &ctx, "Data/Replays/.wh.old.rec", true, &st) == -ENOENT, "whiteout file hidden");
    CHECK(fs_open(fs, &ctx, "Data/Replays/old.rec", GUEST_O_WRONLY | GUEST_O_CREAT, 0666, &f) == 0, "recreate a whited-out name"); fs_file_write(f, "AGAIN", 5); fs_file_unref(f);
    CHECK(read_all(fs, &ctx, "Data/Replays/old.rec", buf, sizeof buf) == 5 && !strcmp(buf, "AGAIN"), "recreated content");
    CHECK(fs_unlink(fs, &ctx, "Data/Replays/new.rec") == 0 && fs_stat(fs, &ctx, "Data/Replays/new.rec", true, &st) == -ENOENT, "unlink upper-only");
    CHECK(fs_rename(fs, &ctx, "Data/Replays/old.rec", "Data/Replays/renamed.rec") == 0, "rename");
    CHECK(listing(fs, &ctx, "Data/Replays", out, sizeof out) == 0 && !strcmp(out, ". .. renamed.rec "), "listing after rename: '%s'", out);
    CHECK(fs_rename(fs, &ctx, "Data/Replays/renamed.rec", "/tmp/x.rec") == -EXDEV, "cross-mount rename");
    CHECK(fs_stat(fs, &ctx, "Payback", true, &st) == 0 && st.size == 4, "untouched lower file stat (size)");
    CHECK(fs_unlink(fs, &ctx, "Data") == -EISDIR, "unlink a dir");
    CHECK(fs_rmdir(fs, &ctx, "Data") == -ENOTEMPTY, "rmdir non-empty");
    CHECK(fs_mkdir(fs, &ctx, "Data/New", 0777) == 0 && fs_rmdir(fs, &ctx, "Data/New") == 0 && fs_stat(fs, &ctx, "Data/New", true, &st) == -ENOENT, "mkdir+rmdir upper dir");
    CHECK(fs_rmdir(fs, &ctx, "Data/Replays") == -ENOTEMPTY, "rmdir with whiteouts and files");
    CHECK(fs_access(fs, &ctx, "Payback", 1) == 0 && fs_access(fs, &ctx, "Payback", 4) == 0 && fs_access(fs, &ctx, "nothere", 0) == -ENOENT, "access");
    CHECK(fs_chdir(fs, &ctx, "Data") == 0 && !strcmp(ctx.cwd, "/mnt/sd/Payback/Data") && fs_chdir(fs, &ctx, "Config/game.ini") == -ENOTDIR && fs_chdir(fs, &ctx, "..") == 0 && !strcmp(ctx.cwd, "/mnt/sd/Payback"), "chdir");
    CHECK(fs_open(fs, &ctx, "Data", GUEST_O_WRONLY, 0, &f) == -EISDIR, "open dir for writing");
    CHECK(fs_open(fs, &ctx, "Payback", GUEST_O_RDONLY | GUEST_O_DIRECTORY, 0, &f) == -ENOTDIR, "O_DIRECTORY on a file");
    CHECK(fs_open(fs, &ctx, "Payback", GUEST_O_WRONLY | GUEST_O_CREAT | GUEST_O_EXCL, 0, &f) == -EEXIST, "O_EXCL existing");
    /* a lower file opened RDONLY then also opened for writing: the reader keeps the lower content (as a copy-up unlinks-and-replaces on a real overlay) */
    CHECK(fs_truncate(fs, &ctx, "Data/Config/game.ini", 3) == 0 && fs_stat(fs, &ctx, "Data/Config/game.ini", true, &st) == 0 && st.size == 3, "truncate");

    /* /dev */
    CHECK(fs_stat(fs, &ctx, "/dev/null", true, &st) == 0 && S_ISCHR(st.mode) && st.rdev == 0x103, "/dev/null node");
    CHECK(fs_open(fs, &ctx, "/dev/null", GUEST_O_RDWR, 0, &f) == 0 && fs_file_read(f, buf, 10) == 0 && fs_file_write(f, "abc", 3) == 3, "/dev/null io"); fs_file_unref(f);
    CHECK(fs_open(fs, &ctx, "/dev/zero", GUEST_O_RDONLY, 0, &f) == 0 && fs_file_read(f, buf, 4) == 4 && !buf[0] && !buf[3], "/dev/zero"); fs_file_unref(f);
    CHECK(fs_stat(fs, &ctx, "/dev/vc/0", true, &st) == 0 && S_ISCHR(st.mode) && fs_stat(fs, &ctx, "/dev/vc", true, &st) == 0 && S_ISDIR(st.mode), "implicit /dev/vc dir");
    CHECK(fs_stat(fs, &ctx, "/dev/pts", true, &st) == 0 && S_ISDIR(st.mode), "/dev/pts");
    CHECK(fs_open(fs, &ctx, "/dev/console", GUEST_O_RDWR, 0, &f) == 0 && fs_file_ioctl(f, 0x4700, NULL, 0) == -ENOTTY, "console unknown ioctl -> ENOTTY"); fs_file_unref(f);
    CHECK(fs_open(fs, &ctx, "/dev/tty", GUEST_O_RDWR, 0, &f) == 0, "/dev/tty is the console"); if (f) { uint8_t tio[36]; gmem_t *gm = gmem_create(); gmem_map_anon(gm, 0x1000, 0x1000, GMEM_PROT_RW); CHECK(fs_file_ioctl(f, 0x5401, gm, 0x1000) == 0 && gmem_read(gm, 0x1000, tio, 36) == GP_OK && tio[17] == 3, "TCGETS gives a termios (VINTR ^C)"); gmem_destroy(gm); fs_file_unref(f); }
    CHECK(fs_stat(fs, &ctx, "/dev/tty1", true, &st) == 0 && S_ISCHR(st.mode) && st.rdev == 0x401, "/dev/tty1 exists");
    CHECK(fs_stat(fs, &ctx, "/dev/mem", true, &st) == -ENOENT, "unregistered device absent");
    CHECK(listing(fs, &ctx, "/dev", out, sizeof out) == 0 && strstr(out, "null zero") && strstr(out, " vc ") && !strstr(out, "vc/0"), "/dev listing: %s", out);

    /* descriptor tables */
    fs_fdtable_t *t = fs_fdt_create();
    fs_file_t *r, *w;
    CHECK(fs_pipe_create(&r, &w) == 0, "pipe");
    int fdr = fs_fdt_install(t, r, false, 0), fdw = fs_fdt_install(t, w, true, 0);
    CHECK(fdr == 0 && fdw == 1, "fds allocated lowest first: %d %d", fdr, fdw);
    fs_file_unref(r); fs_file_unref(w);
    CHECK(fs_fdt_get(t, 0) == r && fs_fdt_get(t, 2) == NULL && fs_fdt_get(t, -1) == NULL, "fdt get");
    CHECK(fs_fdt_dup2(t, 0, 5) == 5 && fs_fdt_get(t, 5) == r && r->refs == 2, "dup2 shares the description (refs %d)", r->refs);
    CHECK(fs_fdt_install(t, r, false, 0) == 2, "next free fd is 2");
    CHECK(fs_fdt_get_cloexec(t, 1) == 1 && fs_fdt_get_cloexec(t, 5) == 0, "cloexec flags");
    fs_fdtable_t *child = fs_fdt_clone(t);
    CHECK(fs_fdt_get(child, 5) == r && fs_fdt_get_cloexec(child, 1) == 1, "clone copies fds and flags");
    fs_fdt_close_on_exec(child);
    CHECK(fs_fdt_get(child, 1) == NULL && fs_fdt_get(child, 0) == r, "close-on-exec");
    CHECK(fs_fdt_close(t, 9) == -EBADF && fs_fdt_close(t, 5) == 0 && fs_fdt_get(t, 5) == NULL, "close");
    /* pipe semantics: EAGAIN when empty with a writer, EOF when the writer is gone, atomic writes */
    CHECK(fs_file_read(r, buf, 10) == -EAGAIN, "empty pipe would block");
    CHECK(fs_file_write(w, "hello", 5) == 5 && fs_file_read(r, buf, 3) == 3 && !strncmp(buf, "hel", 3) && fs_file_read(r, buf, 10) == 2, "pipe data");
    char big[5000]; memset(big, 'z', sizeof big);
    CHECK(fs_file_write(w, big, 4096) == 4096 && fs_file_write(w, "x", 1) == -EAGAIN, "pipe full");
    CHECK(fs_file_poll(w, GUEST_POLLOUT) == 0 && fs_file_poll(r, GUEST_POLLIN) == GUEST_POLLIN, "poll full pipe");
    CHECK(fs_file_read(r, big, 5000) == 4096, "drain");
    CHECK(fs_file_write(w, big, 5000) == 4096, "large write is partial");
    fs_file_read(r, big, 5000);
    CHECK(fs_file_lseek(r, 0, 0) == -ESPIPE, "pipes are not seekable");
    fs_fdt_close(t, 1); /* the write end's only fd in t; child still holds none (closed on exec) */
    CHECK(fs_file_read(r, buf, 10) == 0, "EOF after the writer closes");
    CHECK(fs_file_poll(r, GUEST_POLLIN) & GUEST_POLLHUP, "POLLHUP after the writer closes");
    fs_fdt_unref(child);
    fs_fdt_unref(t);

    /* tmpfs mount, mounts text and umount */
    CHECK(fs_open(fs, &ctx, "/tmp/log", GUEST_O_WRONLY | GUEST_O_CREAT, 0644, &f) == 0, "tmp create"); fs_file_unref(f);
    CHECK(fs_umount(fs, "/nothing") == -EINVAL && fs_umount(fs, "/") == -EBUSY, "umount errors");
    snprintf(p, sizeof p, "%s/tmpfs", tmproot);
    CHECK(fs_mount(fs, "/mnt/tmp", fs_backend_hostdir(p, false, 10), "none", "tmpfs", "rw,size=16M") == 0, "mount /mnt/tmp");
    CHECK(read_all(fs, &ctx, "/proc/mounts", buf, sizeof buf) > 0 && strstr(buf, "none /mnt/tmp tmpfs rw,size=16M 0 0\n"), "new mount in /proc/mounts");
    CHECK(fs_stat(fs, &ctx, "/mnt/tmp/log", true, &st) == 0, "the same host dir shows through the new mount");
    CHECK(fs_umount(fs, "/mnt/tmp") == 0 && fs_stat(fs, &ctx, "/mnt/tmp/log", true, &st) == -ENOENT && fs_stat(fs, &ctx, "/mnt/tmp", true, &st) == 0, "umount reveals the directory");

    fs_destroy(fs);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmproot);
    if (system(cmd) != 0)
        printf("cleanup failed\n");
    printf("%s: %d failure(s)\n", __FILE__, failures);
    return failures ? 1 : 0;
}
