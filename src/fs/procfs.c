/* The /proc subset (spec 6.4, 7.4, #22): mounts, sys/kernel/..., cpuinfo,
 * version, and per-process exe/cwd/cmdline/status/maps from the process
 * module. "self" is resolved by the VFS from the calling context. File
 * contents are generated at open and served from a buffer; sizes report 0
 * as a 2.4 proc file does. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "gport2x/fs.h"

typedef struct procfs {
    fs_t *fs;
    const fs_procinfo_ops_t *ops;
    void *ctx;
    char version[160];   /* /proc/sys/kernel/version: the uts version string */
    char osrelease[64];  /* /proc/sys/kernel/osrelease */
} procfs_t;

enum node_kind { N_NONE, N_DIR, N_FILE, N_LINK };

/* Classifies rel; pid_out is set for per-process nodes; leaf names the file. */
static enum node_kind classify(procfs_t *p, const char *rel, int *pid_out, const char **leaf)
{
    *pid_out = 0;
    *leaf = "";
    if (!*rel)
        return N_DIR;
    if (!strcmp(rel, "mounts") || !strcmp(rel, "cpuinfo") || !strcmp(rel, "version") || !strcmp(rel, "meminfo") ||
        !strcmp(rel, "uptime")) {
        *leaf = rel;
        return N_FILE;
    }
    if (!strcmp(rel, "self"))
        return N_LINK;
    if (!strcmp(rel, "sys") || !strcmp(rel, "sys/kernel"))
        return N_DIR;
    if (!strncmp(rel, "sys/kernel/", 11)) {
        const char *k = rel + 11;
        if (!strcmp(k, "version") || !strcmp(k, "osrelease") || !strcmp(k, "ngroups_max") || !strcmp(k, "rtsig-max") ||
            !strcmp(k, "pid_max") || !strcmp(k, "ostype") || !strcmp(k, "hostname")) {
            *leaf = k;
            return N_FILE;
        }
        return N_NONE;
    }
    /* <pid>[/...] */
    char *end;
    long pid = strtol(rel, &end, 10);
    if (end == rel || pid <= 0)
        return N_NONE;
    if (p->ops && p->ops->pids) {
        int pids[256];
        int n = p->ops->pids(p->ctx, pids, 256);
        bool found = false;
        for (int i = 0; i < n; i++)
            if (pids[i] == pid)
                found = true;
        if (!found)
            return N_NONE;
    }
    *pid_out = (int)pid;
    if (!*end)
        return N_DIR;
    if (*end != '/')
        return N_NONE;
    const char *sub = end + 1;
    if (!strcmp(sub, "exe") || !strcmp(sub, "cwd") || !strcmp(sub, "root"))
        return N_LINK;
    if (!strcmp(sub, "cmdline") || !strcmp(sub, "status") || !strcmp(sub, "maps") || !strcmp(sub, "stat")) {
        *leaf = sub;
        return N_FILE;
    }
    return N_NONE;
}

static uint64_t name_hash(const char *s)
{
    uint64_t h = 1469598103934665603ull;
    for (; *s; s++)
        h = (h ^ (uint8_t)*s) * 1099511628211ull;
    return (h & 0x7FFFFFFF) | 0x10000;
}

static int pf_stat(fs_backend_t *b, const char *rel, fs_stat_t *st)
{
    int pid;
    const char *leaf;
    enum node_kind k = classify(b->priv, rel, &pid, &leaf);
    if (k == N_NONE)
        return -ENOENT;
    memset(st, 0, sizeof *st);
    st->dev = b->dev;
    st->ino = *rel ? name_hash(rel) : 1;
    st->nlink = 1;
    st->blksize = 1024;
    st->mode = k == N_DIR ? (S_IFDIR | 0555) : k == N_LINK ? (S_IFLNK | 0777) : (S_IFREG | 0444);
    return 0;
}

static int pf_readlink(fs_backend_t *b, const char *rel, char *buf, size_t n)
{
    procfs_t *p = b->priv;
    int pid;
    const char *leaf;
    if (classify(p, rel, &pid, &leaf) != N_LINK)
        return -EINVAL;
    if (!strcmp(rel, "self")) {
        const fs_ctx_t *c = fs_current_ctx();
        return snprintf(buf, n, "%d", c ? c->pid : 1);
    }
    const char *sub = strchr(rel, '/') + 1;
    char tmp[FS_PATH_MAX];
    int r;
    if (!strcmp(sub, "exe"))
        r = p->ops && p->ops->exe ? p->ops->exe(p->ctx, pid, tmp, sizeof tmp) : -ENOENT;
    else if (!strcmp(sub, "cwd"))
        r = p->ops && p->ops->cwd ? p->ops->cwd(p->ctx, pid, tmp, sizeof tmp) : -ENOENT;
    else
        r = snprintf(tmp, sizeof tmp, "/");
    if (r < 0)
        return r;
    size_t len = strlen(tmp);
    if (len > n)
        len = n;
    memcpy(buf, tmp, len);
    return (int)len;
}

/* ---- generated files --------------------------------------------------- */

typedef struct procfile {
    char *data;
    size_t len;
} procfile_t;

static int64_t pfile_read(fs_file_t *f, int64_t off, void *buf, size_t n)
{
    procfile_t *pf = f->priv;
    if (off < 0 || (size_t)off >= pf->len)
        return 0;
    size_t avail = pf->len - (size_t)off;
    if (n > avail)
        n = avail;
    memcpy(buf, pf->data + off, n);
    return (int64_t)n;
}

static int pfile_fstat(fs_file_t *f, fs_stat_t *st)
{
    memset(st, 0, sizeof *st);
    st->dev = f->backend->dev;
    st->ino = name_hash(f->path);
    st->mode = S_IFREG | 0444;
    st->nlink = 1;
    st->blksize = 1024;
    return 0;
}

static void pfile_release(fs_file_t *f)
{
    procfile_t *pf = f->priv;
    free(pf->data);
    free(pf);
}

static const fs_file_ops_t procfile_ops = { .read = pfile_read, .fstat = pfile_fstat, .release = pfile_release };

static int generate(procfs_t *p, const char *rel, int pid, const char *leaf, char **data, size_t *len)
{
    size_t cap = 65536;
    char *buf = malloc(cap);
    if (!buf)
        return -ENOMEM;
    int n = 0;
    if (!strcmp(rel, "mounts")) {
        n = fs_mounts_text(p->fs, buf, cap);
    } else if (!strcmp(rel, "cpuinfo")) {
        n = snprintf(buf, cap,
                     "Processor\t: ARM920Tid(wb) rev 0 (v4l)\n"
                     "BogoMIPS\t: 99.73\n"
                     "Features\t: swp half thumb \n"
                     "CPU implementer\t: 0x41\n"
                     "CPU architecture: 4T\n"
                     "CPU variant\t: 0x1\n"
                     "CPU part\t: 0x920\n"
                     "CPU revision\t: 0\n"
                     "Cache type\t: write-back\n"
                     "Cache clean\t: cp15 c7 ops\n"
                     "Cache lockdown\t: format A\n"
                     "Cache format\t: Harvard\n"
                     "I size\t\t: 16384\nI assoc\t\t: 64\nI line length\t: 32\nI sets\t\t: 8\n"
                     "D size\t\t: 16384\nD assoc\t\t: 64\nD line length\t: 32\nD sets\t\t: 8\n"
                     "\nHardware\t: MMSP2\nRevision\t: 0000\nSerial\t\t: 0000000000000000\n");
    } else if (!strcmp(rel, "version")) {
        n = snprintf(buf, cap, "Linux version %s (root@gp2x) (gcc version 3.4.3) %s\n", p->osrelease, p->version);
    } else if (!strcmp(rel, "meminfo")) {
        n = snprintf(buf, cap, "MemTotal:        30224 kB\nMemFree:         12000 kB\nBuffers:           0 kB\nCached:          8000 kB\n");
    } else if (!strcmp(rel, "uptime")) {
        n = snprintf(buf, cap, "100.00 90.00\n");
    } else if (!strncmp(rel, "sys/kernel/", 11)) {
        if (!strcmp(leaf, "version"))
            n = snprintf(buf, cap, "%s\n", p->version);
        else if (!strcmp(leaf, "osrelease"))
            n = snprintf(buf, cap, "%s\n", p->osrelease);
        else if (!strcmp(leaf, "ngroups_max"))
            n = snprintf(buf, cap, "32\n");
        else if (!strcmp(leaf, "rtsig-max"))
            n = snprintf(buf, cap, "1024\n");
        else if (!strcmp(leaf, "pid_max"))
            n = snprintf(buf, cap, "32768\n");
        else if (!strcmp(leaf, "ostype"))
            n = snprintf(buf, cap, "Linux\n");
        else if (!strcmp(leaf, "hostname"))
            n = snprintf(buf, cap, "gp2x\n");
    } else if (pid) {
        if (!strcmp(leaf, "cmdline"))
            n = p->ops && p->ops->cmdline ? p->ops->cmdline(p->ctx, pid, buf, cap) : 0;
        else if (!strcmp(leaf, "status"))
            n = p->ops && p->ops->status ? p->ops->status(p->ctx, pid, buf, cap) : 0;
        else if (!strcmp(leaf, "maps"))
            n = p->ops && p->ops->maps ? p->ops->maps(p->ctx, pid, buf, cap) : 0;
        else if (!strcmp(leaf, "stat"))
            n = snprintf(buf, cap, "%d (gport2x) R 1 %d %d 0 -1 0 0 0 0 0 0 0 0 0 16 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n", pid, pid, pid);
    }
    if (n < 0) {
        free(buf);
        return n;
    }
    *data = buf;
    *len = (size_t)n;
    return 0;
}

static int pf_open(fs_backend_t *b, const char *rel, int gflags, uint32_t mode, fs_file_t **out)
{
    (void)mode;
    procfs_t *p = b->priv;
    int pid;
    const char *leaf;
    enum node_kind k = classify(p, rel, &pid, &leaf);
    if (k == N_NONE)
        return -ENOENT;
    if (k != N_FILE)
        return -EISDIR;
    if ((gflags & GUEST_O_ACCMODE) != GUEST_O_RDONLY)
        return -EACCES;
    procfile_t *pf = calloc(1, sizeof *pf);
    if (!pf)
        return -ENOMEM;
    int r = generate(p, rel, pid, leaf, &pf->data, &pf->len);
    if (r < 0) {
        free(pf);
        return r;
    }
    fs_file_t *f = fs_file_new(&procfile_ops, gflags);
    if (!f) {
        free(pf->data);
        free(pf);
        return -ENOMEM;
    }
    f->priv = pf;
    f->backend = b;
    *out = f;
    return 0;
}

static int emit(fs_dirent_t *out, const char *name, uint8_t type, uint64_t idx)
{
    memset(out, 0, sizeof *out);
    snprintf(out->name, sizeof out->name, "%s", name);
    out->ino = name_hash(name);
    out->type = type;
    out->off = idx + 1;
    return 1;
}

static int pf_readdir(fs_backend_t *b, const char *rel, uint64_t cookie, fs_dirent_t *out)
{
    procfs_t *p = b->priv;
    int pid;
    const char *leaf;
    enum node_kind k = classify(p, rel, &pid, &leaf);
    if (k == N_NONE)
        return -ENOENT;
    if (k != N_DIR)
        return -ENOTDIR;
    static const char *const root_names[] = { ".", "..", "mounts", "sys", "self", "cpuinfo", "version", "meminfo", "uptime" };
    static const uint8_t root_types[] = { GUEST_DT_DIR, GUEST_DT_DIR, GUEST_DT_REG, GUEST_DT_DIR, GUEST_DT_LNK, GUEST_DT_REG, GUEST_DT_REG, GUEST_DT_REG, GUEST_DT_REG };
    static const char *const kernel_names[] = { ".", "..", "version", "osrelease", "ngroups_max", "rtsig-max", "pid_max", "ostype", "hostname" };
    static const char *const pid_names[] = { ".", "..", "exe", "cwd", "root", "cmdline", "status", "maps", "stat" };
    static const uint8_t pid_types[] = { GUEST_DT_DIR, GUEST_DT_DIR, GUEST_DT_LNK, GUEST_DT_LNK, GUEST_DT_LNK, GUEST_DT_REG, GUEST_DT_REG, GUEST_DT_REG, GUEST_DT_REG };
    if (!*rel) {
        size_t nfixed = sizeof root_names / sizeof *root_names;
        if (cookie < nfixed)
            return emit(out, root_names[cookie], root_types[cookie], cookie);
        int pids[256];
        int n = p->ops && p->ops->pids ? p->ops->pids(p->ctx, pids, 256) : 0;
        size_t i = (size_t)cookie - nfixed;
        if ((int)i >= n)
            return 0;
        char name[16];
        snprintf(name, sizeof name, "%d", pids[i]);
        return emit(out, name, GUEST_DT_DIR, cookie);
    }
    if (!strcmp(rel, "sys")) {
        static const char *const names[] = { ".", "..", "kernel" };
        if (cookie >= 3)
            return 0;
        return emit(out, names[cookie], GUEST_DT_DIR, cookie);
    }
    if (!strcmp(rel, "sys/kernel")) {
        if (cookie >= sizeof kernel_names / sizeof *kernel_names)
            return 0;
        return emit(out, kernel_names[cookie], cookie < 2 ? GUEST_DT_DIR : GUEST_DT_REG, cookie);
    }
    if (cookie >= sizeof pid_names / sizeof *pid_names)
        return 0;
    return emit(out, pid_names[cookie], pid_types[cookie], cookie);
}

static int pf_statfs(fs_backend_t *b, const char *rel, fs_statfs_t *out)
{
    (void)b; (void)rel;
    memset(out, 0, sizeof *out);
    out->type = 0x9fa0; /* PROC_SUPER_MAGIC */
    out->bsize = out->frsize = 4096;
    out->namelen = 255;
    return 0;
}

static void pf_destroy(fs_backend_t *b)
{
    free(b->priv);
    free(b);
}

static const fs_backend_ops_t procfs_ops = {
    .stat = pf_stat, .readlink = pf_readlink, .open = pf_open, .readdir = pf_readdir, .statfs = pf_statfs,
    .destroy = pf_destroy,
};

fs_backend_t *fs_backend_proc(fs_t *fs, const fs_procinfo_ops_t *ops, void *ctx, const char *kernel_version)
{
    fs_backend_t *b = calloc(1, sizeof *b);
    procfs_t *p = calloc(1, sizeof *p);
    if (!b || !p) {
        free(b);
        free(p);
        return NULL;
    }
    p->fs = fs;
    p->ops = ops;
    p->ctx = ctx;
    /* kernel_version: "release|uts version", e.g. "2.4.25|#1 Mon Jan 1 00:00:00 KST 2007" */
    const char *bar = kernel_version ? strchr(kernel_version, '|') : NULL;
    if (bar) {
        snprintf(p->osrelease, sizeof p->osrelease, "%.*s", (int)(bar - kernel_version), kernel_version);
        snprintf(p->version, sizeof p->version, "%s", bar + 1);
    } else {
        snprintf(p->osrelease, sizeof p->osrelease, "%s", kernel_version ? kernel_version : "2.4.25");
        snprintf(p->version, sizeof p->version, "#1 Mon Jan 1 00:00:00 KST 2007");
    }
    b->ops = &procfs_ops;
    b->dev = 3;
    b->readonly = true;
    b->priv = p;
    return b;
}
