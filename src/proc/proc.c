/* Guest tasks, address spaces, signals, fork/clone/exec/exit/wait and the
 * scheduler. See proc.h and task.h. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "task.h"

#define SIGBIT(s) (1ull << ((s) - 1))
#define SIG_UNSTOPPABLE (SIGBIT(GSIGKILL) | SIGBIT(GSIGSTOP))

static uint64_t host_monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void gsys_config_default(gsys_config_t *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->quantum = 20000;
    cfg->uname_release = "2.4.25";
    cfg->uname_version = "#1 Mon Jan 1 00:00:00 KST 2007";
    cfg->uname_machine = "armv4tl";
    cfg->hostname = "gp2x";
    cfg->hwcap = elf_default_auxinfo()->hwcap;
    cfg->platform = elf_default_auxinfo()->platform;
}

/* ---- shared objects ---------------------------------------------------- */

static gmm_t *gmm_new(gsys_t *s)
{
    gmm_t *mm = calloc(1, sizeof *mm);
    if (!mm)
        return NULL;
    mm->refs = 1;
    mm->mem = gmem_create();
    if (!mm->mem) {
        free(mm);
        return NULL;
    }
    if (!gmem_is_native(mm->mem)) /* natively 0xFFFF0000 is the host kernel's vectors page */
        gmem_map_obj(mm->mem, TRAMPOLINE_PAGE, GP2X_PAGE_SIZE, GMEM_PROT_R | GMEM_PROT_X | GMEM_MAP_SHARED, s->trampoline, 0);
    return mm;
}

static void gmm_unref(gmm_t *mm)
{
    if (!mm || --mm->refs > 0)
        return;
    gmem_destroy(mm->mem);
    free(mm);
}

static gfsctx_t *gfsctx_new(const fs_ctx_t *from)
{
    gfsctx_t *f = calloc(1, sizeof *f);
    if (!f)
        return NULL;
    f->refs = 1;
    if (from)
        f->ctx = *from;
    return f;
}

static void gfsctx_unref(gfsctx_t *f)
{
    if (f && --f->refs == 0)
        free(f);
}

static gsighand_t *gsighand_new(const gsighand_t *from)
{
    gsighand_t *h = calloc(1, sizeof *h);
    if (!h)
        return NULL;
    h->refs = 1;
    if (from)
        memcpy(h->act, from->act, sizeof h->act);
    return h;
}

static void gsighand_unref(gsighand_t *h)
{
    if (h && --h->refs == 0)
        free(h);
}

/* ---- system ------------------------------------------------------------ */

static const uint32_t trampoline_code[] = {
    0xEF900077, /* 0x500: svc 0x900077  (OABI sigreturn) */
    0xEF9000AD, /* 0x504: svc 0x9000AD  (OABI rt_sigreturn) */
    0xE3A07077, /* 0x508: mov r7, #119 */
    0xEF000000, /* 0x50C: svc 0         (EABI sigreturn) */
    0xE3A070AD, /* 0x510: mov r7, #173 */
    0xEF000000, /* 0x514: svc 0         (EABI rt_sigreturn) */
};

gsys_t *gsys_create(const gsys_config_t *cfg)
{
    gsys_t *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->cfg = *cfg;
    if (!s->cfg.quantum)
        s->cfg.quantum = 20000;
    s->fs = cfg->fs;
    s->dev = cfg->dev;
    s->next_pid = GSYS_FIRST_PID;
    snprintf(s->hostname, sizeof s->hostname, "%s", cfg->hostname ? cfg->hostname : "gp2x");
    s->host_t0_ns = host_monotonic_ns();
    s->trampoline = gmem_obj_ram(GP2X_PAGE_SIZE);
    if (!s->trampoline) {
        free(s);
        return NULL;
    }
    memcpy(gmem_obj_host(s->trampoline) + 0x500, trampoline_code, sizeof trampoline_code);
    if (capture_create(s) != 0) {
        gmem_obj_release(s->trampoline);
        free(s);
        return NULL;
    }
    return s;
}

static void task_free(gtask_t *t)
{
    if (t->cpu)
        cpu_destroy(t->cpu);
    gmm_unref(t->mm);
    fs_fdt_unref(t->fdt);
    gfsctx_unref(t->fs);
    gsighand_unref(t->sighand);
    free(t->frames);
    free(t);
}

void gsys_destroy(gsys_t *s)
{
    if (!s)
        return;
    capture_destroy(s);
    while (s->tasks) {
        gtask_t *t = s->tasks;
        s->tasks = t->next;
        task_free(t);
    }
    gmem_obj_release(s->trampoline);
    free(s);
}

gtask_t *gsys_find_task(gsys_t *s, int pid)
{
    for (gtask_t *t = s->tasks; t; t = t->next)
        if (t->pid == pid && t->state != TASK_DEAD)
            return t;
    return NULL;
}

int gtask_pid(const gtask_t *t) { return t->pid; }
uint64_t gsys_insn_count(const gsys_t *s) { return s->total_insns; }

uint64_t gsys_insn_count_now(const gsys_t *s)
{
    uint64_t n = s->total_insns;
    if (s->current && s->current->cpu)
        n += cpu_insn_count(s->current->cpu) - s->slice_start;
    return n;
}
int gsys_task_count(const gsys_t *s) { return s->ntasks; }
uint64_t gsys_syscall_count(const gsys_t *s) { return s->syscalls; }
void gsys_request_stop(gsys_t *s) { s->stop_requested = true; }

uint64_t gsys_now_ns(gsys_t *s)
{
    if (s->dev && !gpdev_sleeps_are_real(s->dev))
        return gpdev_virtual_ns(s->dev);
    return host_monotonic_ns() - s->host_t0_ns;
}

uint64_t gsys_epoch_ns(gsys_t *s)
{
    if (s->cfg.time_fixed)
        return (uint64_t)s->cfg.time_value * 1000000000ull + gsys_now_ns(s);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int64_t gsys_epoch_seconds(gsys_t *s)
{
    if (s->cfg.time_fixed)
        return s->cfg.time_value;
    return (int64_t)(gsys_epoch_ns(s) / 1000000000ull);
}

static gtask_t *task_new(gsys_t *s)
{
    gtask_t *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->sys = s;
    t->pid = s->next_pid++;
    t->ppid = GSYS_INIT_PID;
    t->pgid = t->sid = t->pid;
    t->state = TASK_RUNNING;
    t->is_main = true;
    for (int i = 0; i < RLIM_NLIMITS; i++)
        t->rlim[i].cur = t->rlim[i].max = GUEST_RLIM_INFINITY;
    t->rlim[3].cur = DEFAULT_STACK_LIMIT; /* RLIMIT_STACK */
    t->rlim[7].cur = t->rlim[7].max = 1024; /* RLIMIT_NOFILE */
    t->rlim[6].cur = t->rlim[6].max = 256;  /* RLIMIT_NPROC */
    /* append (scheduling order = creation order) */
    gtask_t **pp = &s->tasks;
    while (*pp)
        pp = &(*pp)->next;
    *pp = t;
    s->ntasks++;
    return t;
}

/* ---- /proc facts ------------------------------------------------------- */

static gsys_t *procinfo_sys; /* set by gsys_spawn; one system per harness process */

static int pi_exe(void *ctx, int pid, char *buf, size_t n)
{
    (void)ctx;
    gtask_t *t = procinfo_sys ? gsys_find_task(procinfo_sys, pid) : NULL;
    if (!t || !t->mm)
        return -ESRCH;
    return snprintf(buf, n, "%s", t->mm->exe);
}

static int pi_cwd(void *ctx, int pid, char *buf, size_t n)
{
    (void)ctx;
    gtask_t *t = procinfo_sys ? gsys_find_task(procinfo_sys, pid) : NULL;
    if (!t || !t->fs)
        return -ESRCH;
    return snprintf(buf, n, "%s", t->fs->ctx.cwd);
}

static int pi_cmdline(void *ctx, int pid, char *buf, size_t n)
{
    (void)ctx;
    gtask_t *t = procinfo_sys ? gsys_find_task(procinfo_sys, pid) : NULL;
    if (!t)
        return -ESRCH;
    size_t len = t->cmdline_len < n ? t->cmdline_len : n;
    memcpy(buf, t->cmdline, len);
    return (int)len;
}

static int pi_status(void *ctx, int pid, char *buf, size_t n)
{
    (void)ctx;
    gtask_t *t = procinfo_sys ? gsys_find_task(procinfo_sys, pid) : NULL;
    if (!t)
        return -ESRCH;
    return snprintf(buf, n,
                    "Name:\t%s\nState:\t%c (%s)\nPid:\t%d\nPPid:\t%d\nUid:\t0\t0\t0\t0\nGid:\t0\t0\t0\t0\n"
                    "VmSize:\t%8zu kB\nSigPnd:\t%016llx\nSigBlk:\t%016llx\n",
                    t->comm, t->state == TASK_RUNNING ? 'R' : t->state == TASK_BLOCKED ? 'S' : 'Z',
                    t->state == TASK_RUNNING ? "running" : t->state == TASK_BLOCKED ? "sleeping" : "zombie", t->pid,
                    t->ppid, t->mm ? gmem_resident_bytes(t->mm->mem) / 1024 : 0, (unsigned long long)t->sigpending,
                    (unsigned long long)t->sigblocked);
}

static int pi_maps(void *ctx, int pid, char *buf, size_t n)
{
    (void)ctx;
    gtask_t *t = procinfo_sys ? gsys_find_task(procinfo_sys, pid) : NULL;
    if (!t || !t->mm)
        return -ESRCH;
    size_t len = 0;
    uint64_t a = 0;
    while (a < 0x100000000ull && len + 80 < n) {
        gmem_region_t r;
        if (gmem_region_at(t->mm->mem, (gaddr_t)a, &r) != GP_OK) {
            a += GP2X_PAGE_SIZE;
            if ((a & 0x3FFFFF) == 0)
                a = (a + 0x400000 - GP2X_PAGE_SIZE); /* coarse skip inside empty l2 ranges is not visible; keep simple */
            continue;
        }
        len += (size_t)snprintf(buf + len, n - len, "%08x-%08x %c%c%c%c %08x 00:00 0\n", r.start, r.end,
                                r.prot & GMEM_PROT_R ? 'r' : '-', r.prot & GMEM_PROT_W ? 'w' : '-',
                                r.prot & GMEM_PROT_X ? 'x' : '-', r.shared ? 's' : 'p', r.obj_off);
        if (r.end == 0)
            break; /* the region runs to the top of the space */
        a = r.end;
    }
    return (int)len;
}

static int pi_pids(void *ctx, int *out, int max)
{
    (void)ctx;
    int n = 0;
    if (max > 0)
        out[n++] = GSYS_INIT_PID;
    if (!procinfo_sys)
        return n;
    for (gtask_t *t = procinfo_sys->tasks; t && n < max; t = t->next)
        if (t->state != TASK_DEAD)
            out[n++] = t->pid;
    return n;
}

static const fs_procinfo_ops_t procinfo_ops = { pi_exe, pi_cmdline, pi_status, pi_cwd, pi_maps, pi_pids };
const fs_procinfo_ops_t *gsys_procinfo_ops(void) { return &procinfo_ops; }

/* ---- blocking ---------------------------------------------------------- */

void gtask_block(gtask_t *t, enum block_kind kind)
{
    t->state = TASK_BLOCKED;
    t->block = kind;
}

void gtask_set_deadline(gtask_t *t, uint64_t abs_ns)
{
    t->deadline_ns = abs_ns;
    t->has_deadline = true;
}

/* ---- exec -------------------------------------------------------------- */

static void set_cmdline(gtask_t *t, char **argv, const char *comm_from)
{
    t->cmdline_len = 0;
    for (int i = 0; argv[i]; i++) {
        size_t l = strlen(argv[i]) + 1;
        if (t->cmdline_len + l > sizeof t->cmdline)
            break;
        memcpy(t->cmdline + t->cmdline_len, argv[i], l);
        t->cmdline_len += l;
    }
    const char *base = strrchr(comm_from, '/');
    base = base ? base + 1 : comm_from;
    snprintf(t->comm, sizeof t->comm, "%s", base);
}

static int load_image(gtask_t *t, const uint8_t *data, size_t len, const char *path, char **argv, char **envp, int depth);

static int exec_script(gtask_t *t, const elf_probe_t *probe, const char *path, char **argv, char **envp, int depth)
{
    /* argv = interp [arg] path argv[1..] (binfmt_script) */
    int argc = 0;
    while (argv[argc])
        argc++;
    char **nargv = calloc((size_t)argc + 4, sizeof *nargv);
    if (!nargv)
        return -ENOMEM;
    int n = 0;
    nargv[n++] = (char *)probe->interp;
    if (probe->interp_arg[0])
        nargv[n++] = (char *)probe->interp_arg;
    nargv[n++] = (char *)path;
    for (int i = 1; i < argc; i++)
        nargv[n++] = argv[i];
    nargv[n] = NULL;
    uint8_t *data;
    size_t len;
    int r = fs_read_file(t->sys->fs, &t->fs->ctx, probe->interp, &data, &len);
    if (r < 0) {
        free(nargv);
        return r;
    }
    r = load_image(t, data, len, probe->interp, nargv, envp, depth + 1);
    free(data);
    free(nargv);
    return r;
}

static int load_image(gtask_t *t, const uint8_t *data, size_t len, const char *path, char **argv, char **envp, int depth)
{
    gsys_t *s = t->sys;
    elf_probe_t probe;
    if (elf_probe(data, len, &probe) != GP_OK || probe.kind == ELF_KIND_NONE)
        return -ENOEXEC;
    if (probe.kind == ELF_KIND_SCRIPT) {
        if (depth >= 4)
            return -ELOOP;
        return exec_script(t, &probe, path, argv, envp, depth);
    }
    gmm_t *mm = gmm_new(s);
    if (!mm)
        return -ENOMEM;
    elf_image_t img, interp_img;
    int r = elf_load(mm->mem, data, len, 0, &img);
    if (r != GP_OK) {
        gmm_unref(mm);
        return r == GP_ERR_NOMEM ? -ENOMEM : -ENOEXEC;
    }
    bool has_interp = img.has_interp;
    if (has_interp) {
        uint8_t *idata;
        size_t ilen;
        r = fs_read_file(s->fs, &t->fs->ctx, img.interp, &idata, &ilen);
        if (r < 0) {
            gp_error("exec %s: interpreter %s: %s", path, img.interp, strerror(-r));
            gmm_unref(mm);
            return r == -ENOENT ? -ENOENT : -ELIBBAD;
        }
        gaddr_t base = gmem_find_free(mm->mem, GUEST_MMAP_BASE, 1u << 20);
        r = elf_load(mm->mem, idata, ilen, base, &interp_img);
        free(idata);
        if (r != GP_OK) {
            gmm_unref(mm);
            return -ELIBBAD;
        }
    }
    mm->brk_start = mm->brk = img.load_end;
    mm->stack_top = GUEST_STACK_TOP;
    mm->stack_low = GUEST_STACK_TOP - INITIAL_STACK_BYTES;
    int argc = 0;
    while (argv[argc])
        argc++;
    elf_auxinfo_t aux = *elf_default_auxinfo();
    if (s->cfg.hwcap)
        aux.hwcap = s->cfg.hwcap;
    if (s->cfg.platform)
        aux.platform = s->cfg.platform;
    gaddr_t sp;
    r = elf_setup_stack(mm->mem, &img, has_interp ? &interp_img : NULL, path, argc, (const char *const *)argv,
                        (const char *const *)envp, &aux, mm->stack_top, INITIAL_STACK_BYTES, &sp);
    if (r != GP_OK) {
        gmm_unref(mm);
        return -E2BIG;
    }
    char abs[FS_PATH_MAX];
    if (fs_resolve(s->fs, &t->fs->ctx, path, true, abs) == 0)
        snprintf(mm->exe, sizeof mm->exe, "%s", abs);
    else
        snprintf(mm->exe, sizeof mm->exe, "%s", path);

    /* point of no return: replace the task's image */
    gmm_unref(t->mm);
    t->mm = mm;
    if (t->cpu)
        cpu_destroy(t->cpu);
    t->cpu = cpu_create(mm->mem);
    cpu_regs_t *regs = cpu_regs(t->cpu);
    memset(regs, 0, sizeof *regs);
    regs->r[13] = sp;
    regs->r[15] = has_interp ? interp_img.entry : img.entry;
    regs->cpsr = CPSR_MODE_USR;
    fs_fdt_close_on_exec(t->fdt);
    gsighand_t *nh = gsighand_new(NULL);
    for (int i = 1; i <= NSIG_GUEST; i++)
        if (t->sighand->act[i].handler == 1)
            nh->act[i].handler = 1; /* SIG_IGN survives exec */
    gsighand_unref(t->sighand);
    t->sighand = nh;
    t->nframes = 0;
    t->altstack_sp = 0;
    t->altstack_size = 0;
    set_cmdline(t, argv, path);
    gp_info("exec pid %d: %s (%s%s) entry %08x brk %08x sp %08x", t->pid, mm->exe, img.oabi ? "OABI" : "EABI",
            has_interp ? ", dynamic" : "", regs->r[15], mm->brk, sp);
    return 0;
}

int gtask_execve(gtask_t *t, const char *path, char **argv, char **envp)
{
    gsys_t *s = t->sys;
    fs_stat_t st;
    int r = fs_stat(s->fs, &t->fs->ctx, path, true, &st);
    if (r < 0)
        return r;
    if (S_ISDIR(st.mode))
        return -EACCES;
    if (!S_ISREG(st.mode) || !(st.mode & 0111))
        return -EACCES;
    uint8_t *data;
    size_t len;
    r = fs_read_file(s->fs, &t->fs->ctx, path, &data, &len);
    if (r < 0)
        return r;
    r = load_image(t, data, len, path, argv, envp, 0);
    free(data);
    return r;
}

int gsys_spawn(gsys_t *s, const char *path, const char *const *argv, const char *const *envp, const char *cwd, gtask_t **out)
{
    gtask_t *t = task_new(s);
    if (!t)
        return -ENOMEM;
    t->mm = gmm_new(s);
    t->fdt = fs_fdt_create();
    t->fs = gfsctx_new(NULL);
    t->sighand = gsighand_new(NULL);
    if (!t->mm || !t->fdt || !t->fs || !t->sighand)
        return -ENOMEM;
    fs_ctx_init(&t->fs->ctx, t->pid, cwd);
    /* stdin, stdout, stderr pass through to the harness's own (guest
     * programs started from the firmware open /dev/tty themselves) */
    for (int fd = 0; fd < 3; fd++) {
        fs_file_t *f = fs_file_hostfd(fd, fd == 0 ? GUEST_O_RDONLY : GUEST_O_WRONLY);
        if (f) {
            fs_fdt_install(t->fdt, f, false, fd);
            fs_file_unref(f);
        }
    }
    procinfo_sys = s;
    int r = gtask_execve(t, path, (char **)argv, (char **)envp);
    if (r < 0) {
        t->state = TASK_DEAD;
        return r;
    }
    if (!s->first)
        s->first = t;
    if (out)
        *out = t;
    return 0;
}

/* ---- fork / clone ------------------------------------------------------ */

static gtask_t *task_dup(gtask_t *t, uint32_t flags)
{
    gsys_t *s = t->sys;
    gtask_t *c = task_new(s);
    if (!c)
        return NULL;
    c->ppid = (flags & 0x8000 /* CLONE_PARENT */) ? t->ppid : t->pid;
    c->pgid = t->pgid;
    c->sid = t->sid;
    if (flags & 0x100 /* CLONE_VM */) {
        c->mm = t->mm;
        c->mm->refs++;
        c->is_main = false;
    } else {
        gmm_t *mm = calloc(1, sizeof *mm);
        if (!mm)
            return NULL;
        *mm = *t->mm;
        mm->refs = 1;
        mm->mem = gmem_clone(t->mm->mem);
        if (!mm->mem) {
            free(mm);
            return NULL;
        }
        c->mm = mm;
        c->is_main = true;
    }
    if (flags & 0x400 /* CLONE_FILES */)
        c->fdt = fs_fdt_ref(t->fdt);
    else
        c->fdt = fs_fdt_clone(t->fdt);
    if (flags & 0x200 /* CLONE_FS */) {
        c->fs = t->fs;
        c->fs->refs++;
    } else {
        c->fs = gfsctx_new(&t->fs->ctx);
    }
    if (flags & 0x800 /* CLONE_SIGHAND */) {
        c->sighand = t->sighand;
        c->sighand->refs++;
    } else {
        c->sighand = gsighand_new(t->sighand);
    }
    c->sigblocked = t->sigblocked;
    c->sigpending = 0;
    if (t->nframes) {
        c->frames = malloc((size_t)t->nframes * sizeof *c->frames);
        memcpy(c->frames, t->frames, (size_t)t->nframes * sizeof *c->frames);
        c->nframes = c->frames_cap = t->nframes;
    }
    c->altstack_sp = t->altstack_sp;
    c->altstack_size = t->altstack_size;
    memcpy(c->rlim, t->rlim, sizeof c->rlim);
    memcpy(c->comm, t->comm, sizeof c->comm);
    memcpy(c->cmdline, t->cmdline, sizeof c->cmdline);
    c->cmdline_len = t->cmdline_len;
    c->tls = t->tls;
    c->cpu = cpu_create(c->mm->mem);
    *cpu_regs(c->cpu) = *cpu_regs(t->cpu);
    cpu_regs(c->cpu)->r[0] = 0;
    return c;
}

int gtask_fork(gtask_t *t, bool vfork, gtask_t **child)
{
    (void)vfork;
    gtask_t *c = task_dup(t, 0);
    if (!c)
        return -ENOMEM;
    c->exit_signal = GSIGCHLD;
    gp_info("fork: pid %d -> child %d", t->pid, c->pid);
    *child = c;
    return 0;
}

int gtask_clone_thread(gtask_t *t, uint32_t flags, gaddr_t child_sp, gtask_t **child)
{
    gtask_t *c = task_dup(t, flags);
    if (!c)
        return -ENOMEM;
    c->exit_signal = (int)(flags & 0xFF);
    if (child_sp)
        cpu_regs(c->cpu)->r[13] = child_sp;
    gp_info("clone: pid %d flags %#x -> child %d (sp %08x, exit signal %d)", t->pid, flags, c->pid, child_sp,
            c->exit_signal);
    *child = c;
    return 0;
}

/* ---- exit / wait ------------------------------------------------------- */

static void task_release_resources(gtask_t *t)
{
    if (t->fdt) {
        fs_fdt_unref(t->fdt);
        t->fdt = NULL;
    }
    gfsctx_unref(t->fs);
    t->fs = NULL;
    gsighand_unref(t->sighand);
    t->sighand = NULL;
    if (t->cpu) {
        cpu_destroy(t->cpu);
        t->cpu = NULL;
    }
    gmm_unref(t->mm);
    t->mm = NULL;
}

void gtask_exit(gtask_t *t, int wait_status)
{
    gsys_t *s = t->sys;
    if (t->state == TASK_ZOMBIE || t->state == TASK_DEAD)
        return;
    gp_info("exit: pid %d status %#x (%llu insns)", t->pid, wait_status, (unsigned long long)t->insns_run);
    t->exit_code = wait_status;
    t->state = TASK_ZOMBIE;
    t->block = BLK_NONE;
    task_release_resources(t);
    if (t == s->first) {
        s->first_exited = true;
        s->first_status = wait_status;
    }
    /* children are adopted by init, which reaps zombies at once */
    for (gtask_t *c = s->tasks; c; c = c->next) {
        if (c->ppid == t->pid && c != t) {
            c->ppid = GSYS_INIT_PID;
            if (c->state == TASK_ZOMBIE)
                c->state = TASK_DEAD;
        }
    }
    gtask_t *parent = gsys_find_task(s, t->ppid);
    if (!parent || t->ppid == GSYS_INIT_PID) {
        t->state = TASK_DEAD;
        return;
    }
    if (t->exit_signal)
        gtask_send_signal(parent, t->exit_signal, t->pid);
}

static bool wait_matches(const gtask_t *parent, const gtask_t *c, int pid, int options)
{
    if (c->ppid != parent->pid || c == parent)
        return false;
    if (pid > 0 && c->pid != pid)
        return false;
    if (pid == 0 && c->pgid != parent->pgid)
        return false;
    if (pid < -1 && c->pgid != -pid)
        return false;
    bool is_clone = c->exit_signal != GSIGCHLD;
    if (options & 0x40000000) /* __WALL */
        return true;
    if (options & 0x80000000u) /* __WCLONE */
        return is_clone;
    return !is_clone;
}

int gtask_wait4(gtask_t *t, int pid, int options, gaddr_t status_ptr, gaddr_t rusage_ptr)
{
    gsys_t *s = t->sys;
    bool any = false;
    for (gtask_t *c = s->tasks; c; c = c->next) {
        if (c->state == TASK_DEAD || !wait_matches(t, c, pid, options))
            continue;
        any = true;
        if (c->state != TASK_ZOMBIE)
            continue;
        if (status_ptr && gmem_st32(t->mm->mem, status_ptr, (uint32_t)c->exit_code) != GP_OK)
            return -EFAULT;
        if (rusage_ptr)
            gmem_memset(t->mm->mem, rusage_ptr, 0, 72);
        c->state = TASK_DEAD;
        c->reaped = true;
        gp_info("wait4: pid %d reaped %d (status %#x)", t->pid, c->pid, c->exit_code);
        t->has_deadline = false;
        return c->pid;
    }
    if (!any)
        return -ECHILD;
    if (options & 1 /* WNOHANG */)
        return 0;
    t->wait_pid = pid;
    t->wait_options = options;
    t->wait_status_ptr = status_ptr;
    t->wait_rusage_ptr = rusage_ptr;
    gtask_block(t, BLK_WAIT);
    return 1;
}

/* ---- signals ----------------------------------------------------------- */

static bool default_ignored(int sig)
{
    return sig == GSIGCHLD || sig == GSIGURG || sig == GSIGWINCH || sig == GSIGCONT || sig == GSIGSTOP ||
           sig == GSIGTSTP || sig == GSIGTTIN || sig == GSIGTTOU;
}

int gtask_send_signal(gtask_t *t, int sig, int from_pid)
{
    (void)from_pid;
    if (sig <= 0 || sig > NSIG_GUEST)
        return -EINVAL;
    if (t->state == TASK_ZOMBIE || t->state == TASK_DEAD)
        return -ESRCH;
    if (sig == GSIGKILL) {
        gp_info("SIGKILL -> pid %d", t->pid);
        gtask_exit(t, GSIGKILL);
        return 0;
    }
    /* an ignored signal is discarded at once, as the kernel does */
    const gsigact_t *a = &t->sighand->act[sig];
    if (a->handler == 1 || (a->handler == 0 && default_ignored(sig)))
        return 0;
    t->sigpending |= SIGBIT(sig);
    gp_trace(GP_TRACE_SYSCALL, "signal %d -> pid %d (pending %016llx blocked %016llx)", sig, t->pid,
             (unsigned long long)t->sigpending, (unsigned long long)t->sigblocked);
    return 0;
}

int gsys_kill(gsys_t *s, gtask_t *from, int pid, int sig)
{
    if (sig < 0 || sig > NSIG_GUEST)
        return -EINVAL;
    int found = 0, r = 0;
    for (gtask_t *t = s->tasks; t; t = t->next) {
        if (t->state == TASK_DEAD || t->state == TASK_ZOMBIE)
            continue;
        bool match = pid > 0 ? t->pid == pid : pid == 0 ? t->pgid == from->pgid : pid == -1 ? t != from : t->pgid == -pid;
        if (!match)
            continue;
        found++;
        if (sig)
            r = gtask_send_signal(t, sig, from->pid);
    }
    return found ? r : -ESRCH;
}

bool gtask_signal_deliverable(const gtask_t *t)
{
    return (t->sigpending & ~t->sigblocked) != 0 || (t->sigpending & SIG_UNSTOPPABLE);
}

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

/* Pushes a signal frame and enters the handler. */
static int setup_frame(gtask_t *t, int sig, gsigact_t *act)
{
    cpu_regs_t *regs = cpu_regs(t->cpu);
    uint64_t oldmask = t->block == BLK_SUSPEND ? t->suspend_oldmask : t->sigblocked;
    /* an interrupted blocking syscall */
    if (t->state == TASK_BLOCKED) {
        switch (t->block) {
        case BLK_SYSCALL:
        case BLK_WAIT:
        case BLK_POLL:
            if (act->flags & GSA_RESTART)
                regs->r[15] -= 4; /* re-execute the svc after the handler */
            else
                regs->r[0] = (uint32_t)-EINTR;
            break;
        case BLK_SLEEP:
        case BLK_SUSPEND:
        default:
            regs->r[0] = (uint32_t)-EINTR;
            break;
        }
        t->state = TASK_RUNNING;
        t->block = BLK_NONE;
        t->has_deadline = false;
    }
    if (t->nframes == t->frames_cap) {
        t->frames_cap = t->frames_cap ? t->frames_cap * 2 : 8;
        t->frames = realloc(t->frames, (size_t)t->frames_cap * sizeof *t->frames);
    }
    saved_frame_t *sf = &t->frames[t->nframes++];
    sf->regs = *regs;
    sf->sigmask = oldmask;

    gaddr_t sp = regs->r[13];
    if ((act->flags & GSA_ONSTACK) && t->altstack_size &&
        !(sp > t->altstack_sp && sp <= t->altstack_sp + t->altstack_size))
        sp = t->altstack_sp + t->altstack_size;
    enum { INFO = 128, UC = 232, RET = 8, FRAME = INFO + UC + RET };
    sp = (sp - FRAME) & ~7u;
    uint8_t frame[FRAME];
    memset(frame, 0, sizeof frame);
    put32(frame + 0, (uint32_t)sig);
    put32(frame + 8, 0); /* si_code: SI_USER */
    if (sig == GSIGSEGV || sig == GSIGBUS || sig == GSIGILL || sig == GSIGFPE) {
        put32(frame + 8, 1);
        put32(frame + 12, t->wait_status_ptr); /* si_addr: the fault address stashed by the scheduler */
    }
    uint8_t *uc = frame + INFO;
    put32(uc + 8, t->altstack_sp);
    put32(uc + 12, (uint32_t)t->altstack_flags);
    put32(uc + 16, t->altstack_size);
    uint8_t *mc = uc + 20;
    put32(mc + 8, (uint32_t)oldmask);
    for (int i = 0; i < 16; i++)
        put32(mc + 12 + i * 4, regs->r[i]);
    put32(mc + 76, regs->cpsr);
    put32(uc + 104, (uint32_t)oldmask);
    put32(uc + 108, (uint32_t)(oldmask >> 32));
    if (gmem_write(t->mm->mem, sp, frame, sizeof frame) != GP_OK) {
        gp_error("pid %d: cannot write the signal frame at %08x; killing", t->pid, sp);
        t->nframes--;
        gtask_exit(t, GSIGSEGV);
        return -EFAULT;
    }
    regs->r[0] = (uint32_t)sig;
    regs->r[1] = sp;
    regs->r[2] = sp + INFO;
    regs->r[13] = sp;
    regs->r[14] = (act->flags & GSA_RESTORER) ? act->restorer
                  : (act->flags & GSA_SIGINFO) ? TRAMPOLINE_RT_SIGRETURN : TRAMPOLINE_SIGRETURN;
    regs->r[15] = act->handler;
    t->sigblocked |= act->mask;
    if (!(act->flags & GSA_NODEFER))
        t->sigblocked |= SIGBIT(sig);
    t->sigblocked &= ~SIG_UNSTOPPABLE;
    if (act->flags & GSA_RESETHAND)
        act->handler = 0;
    gp_trace(GP_TRACE_SYSCALL, "pid %d: signal %d handler %08x frame %08x", t->pid, sig, regs->r[15], sp);
    return 0;
}

int gtask_sigreturn(gtask_t *t)
{
    if (t->nframes == 0)
        return -EINVAL;
    saved_frame_t *sf = &t->frames[--t->nframes];
    *cpu_regs(t->cpu) = sf->regs;
    t->sigblocked = sf->sigmask & ~SIG_UNSTOPPABLE;
    return 0;
}

/* Delivers one pending, unblocked signal (handler, default action or
 * nothing). Returns true if the task's state changed. */
static bool deliver_signals(gtask_t *t);
bool gtask_deliver_signals(gtask_t *t) { return deliver_signals(t); }

static bool deliver_signals(gtask_t *t)
{
    if (!t->sigpending || t->state == TASK_ZOMBIE || t->state == TASK_DEAD)
        return false;
    for (int sig = 1; sig <= NSIG_GUEST; sig++) {
        uint64_t bit = SIGBIT(sig);
        if (!(t->sigpending & bit))
            continue;
        if ((t->sigblocked & bit) && !(bit & SIG_UNSTOPPABLE))
            continue;
        t->sigpending &= ~bit;
        gsigact_t *act = &t->sighand->act[sig];
        if (act->handler == 1)
            continue;
        if (act->handler == 0) {
            if (default_ignored(sig))
                continue;
            gp_warn("pid %d (%s): killed by signal %d at pc %08x lr %08x", t->pid, t->comm, sig,
                    cpu_regs(t->cpu)->r[15], cpu_regs(t->cpu)->r[14]);
            gtask_exit(t, sig);
            return true;
        }
        setup_frame(t, sig, act);
        return true;
    }
    return false;
}

/* ---- memory map operations --------------------------------------------- */

static int guest_prot_to_gmem(int prot)
{
    int g = 0;
    if (prot & GUEST_PROT_READ)
        g |= GMEM_PROT_R | GMEM_PROT_X; /* the ARMv4 MMU has no execute-never: readable is executable */
    if (prot & GUEST_PROT_WRITE)
        g |= GMEM_PROT_R | GMEM_PROT_W | GMEM_PROT_X; /* and no write-only: writable is readable */
    if (prot & GUEST_PROT_EXEC)
        g |= GMEM_PROT_R | GMEM_PROT_X;
    return g;
}

int gmm_brk(gtask_t *t, gaddr_t newbrk, gaddr_t *out)
{
    gmm_t *mm = t->mm;
    *out = mm->brk;
    if (newbrk == 0 || newbrk < mm->brk_start)
        return 0;
    gaddr_t old_end = GP2X_PAGE_ALIGN_UP(mm->brk), new_end = GP2X_PAGE_ALIGN_UP(newbrk);
    if (new_end > old_end) {
        if (gmem_find_free(mm->mem, old_end, new_end - old_end) != old_end)
            return 0; /* something is in the way: the old brk stays */
        if (gmem_map_anon(mm->mem, old_end, new_end - old_end, GMEM_PROT_RWX) != GP_OK)
            return 0;
    } else if (new_end < old_end) {
        gmem_unmap(mm->mem, new_end, old_end - new_end);
    }
    mm->brk = newbrk;
    *out = newbrk;
    return 0;
}

int gmm_grow_stack(gtask_t *t, gaddr_t fault_addr)
{
    gmm_t *mm = t->mm;
    uint32_t limit = t->rlim[3].cur == GUEST_RLIM_INFINITY ? (64u << 20) : t->rlim[3].cur;
    if (fault_addr >= mm->stack_low || fault_addr < mm->stack_top - limit)
        return -EFAULT;
    gaddr_t new_low = fault_addr & GP2X_PAGE_MASK;
    if (gmem_find_free(mm->mem, new_low, mm->stack_low - new_low) != new_low)
        return -EFAULT;
    if (gmem_map_anon(mm->mem, new_low, mm->stack_low - new_low, GMEM_PROT_RWX) != GP_OK)
        return -ENOMEM;
    gp_trace(GP_TRACE_CPU, "pid %d: stack grown to %08x", t->pid, new_low);
    mm->stack_low = new_low;
    return 0;
}

int gmm_mmap(gtask_t *t, gaddr_t addr, uint32_t len, int prot, int flags, int fd, uint64_t off, gaddr_t *out)
{
    gmm_t *mm = t->mm;
    if (len == 0 || (off & (GP2X_PAGE_SIZE - 1)))
        return -EINVAL;
    uint64_t alen = GP2X_PAGE_ALIGN_UP((uint64_t)len);
    if (alen > 0xFFFFF000ull)
        return -ENOMEM;
    len = (uint32_t)alen;
    bool shared = (flags & GUEST_MAP_SHARED) != 0;
    int gprot = guest_prot_to_gmem(prot) | (shared ? GMEM_MAP_SHARED : 0);
    if (flags & GUEST_MAP_FIXED) {
        if (addr & (GP2X_PAGE_SIZE - 1))
            return -EINVAL;
        if ((uint64_t)addr + len > GUEST_TASK_SIZE)
            return -EINVAL;
    } else {
        gaddr_t hint = addr & GP2X_PAGE_MASK;
        if (hint && hint >= GUEST_LOAD_BASE && (uint64_t)hint + len <= GUEST_TASK_SIZE &&
            gmem_find_free(mm->mem, hint, len) == hint)
            addr = hint;
        else
            addr = gmem_find_free(mm->mem, GUEST_MMAP_BASE, len);
        if (!addr)
            return -ENOMEM;
    }
    int r;
    if (flags & GUEST_MAP_ANONYMOUS) {
        r = gmem_map_anon(mm->mem, addr, len, gprot);
    } else {
        fs_file_t *f = fs_fdt_get(t->fdt, fd);
        if (!f)
            return -EBADF;
        gmem_obj_t *obj;
        uint32_t obj_off;
        r = fs_file_mmap(f, off, len, gprot & GMEM_PROT_RWX, shared, &obj, &obj_off);
        if (r < 0)
            return r;
        r = gmem_map_obj(mm->mem, addr, len, gprot, obj, obj_off);
        gmem_obj_release(obj);
    }
    if (r != GP_OK)
        return r == GP_ERR_NOMEM ? -ENOMEM : -EINVAL;
    *out = addr;
    return 0;
}

int gmm_munmap(gtask_t *t, gaddr_t addr, uint32_t len)
{
    if ((addr & (GP2X_PAGE_SIZE - 1)) || len == 0)
        return -EINVAL;
    uint64_t alen = GP2X_PAGE_ALIGN_UP((uint64_t)len);
    if ((uint64_t)addr + alen > 0x100000000ull)
        return -EINVAL;
    gmem_unmap(t->mm->mem, addr, (uint32_t)alen);
    return 0;
}

int gmm_mprotect(gtask_t *t, gaddr_t addr, uint32_t len, int prot)
{
    if ((addr & (GP2X_PAGE_SIZE - 1)))
        return -EINVAL;
    if (len == 0)
        return 0;
    uint64_t alen = GP2X_PAGE_ALIGN_UP((uint64_t)len);
    if ((uint64_t)addr + alen > 0x100000000ull)
        return -ENOMEM;
    int r = gmem_protect(t->mm->mem, addr, (uint32_t)alen, guest_prot_to_gmem(prot));
    return r == GP_OK ? 0 : -ENOMEM;
}

int gmm_mremap(gtask_t *t, gaddr_t old_addr, uint32_t old_len, uint32_t new_len, int flags, gaddr_t new_addr, gaddr_t *out)
{
    gmem_t *m = t->mm->mem;
    (void)new_addr;
    if ((old_addr & (GP2X_PAGE_SIZE - 1)) || new_len == 0)
        return -EINVAL;
    old_len = GP2X_PAGE_ALIGN_UP(old_len);
    new_len = GP2X_PAGE_ALIGN_UP(new_len);
    if (!gmem_is_mapped(m, old_addr, old_len ? old_len : 1, 0))
        return -EFAULT;
    gmem_region_t reg;
    gmem_region_at(m, old_addr, &reg);
    int prot = reg.prot | (reg.shared ? GMEM_MAP_SHARED : 0);
    if (new_len <= old_len) {
        if (new_len < old_len)
            gmem_unmap(m, old_addr + new_len, old_len - new_len);
        *out = old_addr;
        return 0;
    }
    uint32_t extra = new_len - old_len;
    if (gmem_find_free(m, old_addr + old_len, extra) == old_addr + old_len) {
        if (gmem_map_anon(m, old_addr + old_len, extra, prot) != GP_OK)
            return -ENOMEM;
        *out = old_addr;
        return 0;
    }
    if (!(flags & 1 /* MREMAP_MAYMOVE */))
        return -ENOMEM;
    gaddr_t dst = gmem_find_free(m, GUEST_MMAP_BASE, new_len);
    if (!dst)
        return -ENOMEM;
    gmem_obj_t *obj = gmem_obj_ram(new_len);
    if (!obj)
        return -ENOMEM;
    if (gmem_read(m, old_addr, gmem_obj_host(obj), old_len) != GP_OK) {
        gmem_obj_release(obj);
        return -EFAULT;
    }
    int r = gmem_map_obj(m, dst, new_len, prot, obj, 0);
    gmem_obj_release(obj);
    if (r != GP_OK)
        return -ENOMEM;
    gmem_unmap(m, old_addr, old_len);
    *out = dst;
    return 0;
}

/* ---- scheduler --------------------------------------------------------- */

static void raise_fault(gtask_t *t, int sig, gaddr_t addr, const char *what, const cpu_stop_info_t *info)
{
    cpu_regs_t *r = cpu_regs(t->cpu);
    gp_warn("pid %d (%s): %s at pc %08x (insn %08x) addr %08x lr %08x sp %08x -> signal %d", t->pid, t->comm,
            what, info->pc, info->insn, addr, r->r[14], r->r[13], sig);
    if (gp_log_trace_enabled(GP_TRACE_CPU)) {
        for (int i = 0; i < 16; i += 4)
            gp_trace(GP_TRACE_CPU, "  r%-2d %08x r%-2d %08x r%-2d %08x r%-2d %08x", i, r->r[i], i + 1, r->r[i + 1],
                     i + 2, r->r[i + 2], i + 3, r->r[i + 3]);
    }
    t->wait_status_ptr = addr; /* si_addr for the frame */
    t->sigpending |= SIGBIT(sig);
    /* a fault signal blocked or ignored kills the task, as the kernel forces it */
    const gsigact_t *a = &t->sighand->act[sig];
    if (a->handler == 1 || (t->sigblocked & SIGBIT(sig)))
        gtask_exit(t, sig);
}

static void reap_dead(gsys_t *s)
{
    gtask_t **pp = &s->tasks;
    while (*pp) {
        gtask_t *t = *pp;
        if (t->state == TASK_DEAD) {
            *pp = t->next;
            if (t == s->first)
                s->first = NULL; /* keep first_status */
            task_free(t);
            s->ntasks--;
        } else {
            pp = &t->next;
        }
    }
}

gtask_t *gsys_current_task(const gsys_t *s) { return s->current; }

int gtask_read_mem(gtask_t *t, gaddr_t addr, void *buf, uint32_t len)
{
    if (!t || !t->mm)
        return -ESRCH;
    return gmem_read(t->mm->mem, addr, buf, len) == GP_OK ? 0 : -EFAULT;
}

static void run_slice(gsys_t *s, gtask_t *t)
{
    cpu_stop_info_t info;
    t->slice_only_slept = false;
    s->current = t;
    gpdev_set_current_thread(s->dev, t->is_main);
    if (s->cap)
        capture_before_slice(s, t);
    uint64_t before = cpu_insn_count(t->cpu);
    s->slice_start = before;
    enum cpu_stop stop = cpu_run(t->cpu, s->cfg.quantum, &info);
    uint64_t ran = cpu_insn_count(t->cpu) - before;
    t->insns_run += ran;
    s->total_insns += ran;
    s->slice_start = cpu_insn_count(t->cpu);
    switch (stop) {
    case CPU_STOP_LIMIT:
    case CPU_STOP_HALT:
    case CPU_STOP_NONE:
        break;
    case CPU_STOP_SVC:
        s->syscalls++;
        if (t->cap_rec)
            capture_svc(t, info.svc_imm, false);
        sys_dispatch(t, info.svc_imm);
        if (t->cap_rec)
            capture_svc(t, info.svc_imm, true);
        break;
    case CPU_STOP_UNDEF:
        raise_fault(t, GSIGILL, info.pc, "undefined instruction", &info);
        break;
    case CPU_STOP_THUMB:
        raise_fault(t, GSIGILL, info.pc, "Thumb entry", &info);
        break;
    case CPU_STOP_DATA_ABORT:
        if (gmm_grow_stack(t, info.fault_addr) == 0)
            break;
        raise_fault(t, GSIGSEGV, info.fault_addr, "data abort", &info);
        break;
    case CPU_STOP_PREFETCH_ABORT:
        raise_fault(t, GSIGSEGV, info.pc, "prefetch abort", &info);
        break;
    case CPU_STOP_BREAKPOINT:
        if (s->cap)
            capture_breakpoint(s, t, info.pc);
        break;
    }
    if (s->cap)
        capture_after_slice(s, t);
    s->current = NULL;
}

enum gsys_stop gsys_run(gsys_t *s, int *wait_status)
{
    enum gsys_stop reason = GSYS_STOP_ALL_EXITED;
    uint64_t idle_rounds = 0;
    for (;;) {
        if (s->stop_requested) {
            reason = GSYS_STOP_REQUESTED;
            break;
        }
        if (s->cfg.max_insns && s->total_insns >= s->cfg.max_insns) {
            reason = GSYS_STOP_INSNS;
            break;
        }
        if (s->cfg.max_flips && s->dev && gpdev_flip_count(s->dev) >= s->cfg.max_flips) {
            reason = GSYS_STOP_FLIPS;
            break;
        }
        if (s->dev)
            gpdev_tick(s->dev); /* the DAC drains while tasks wait: wake a blocked writer in time */
        bool any_alive = false, any_ran = false, have_deadline = false, all_slept = true;
        uint64_t earliest = UINT64_MAX, min_sleep = UINT64_MAX;
        for (gtask_t *t = s->tasks; t; t = t->next) {
            if (t->state == TASK_ZOMBIE || t->state == TASK_DEAD)
                continue;
            any_alive = true;
            if (t->state == TASK_BLOCKED) {
                deliver_signals(t);
                if (t->state == TASK_BLOCKED) {
                    if (t->block != BLK_SUSPEND)
                        sys_retry(t);
                    if (t->state == TASK_BLOCKED) {
                        if (t->has_deadline) {
                            have_deadline = true;
                            if (t->deadline_ns < earliest)
                                earliest = t->deadline_ns;
                        }
                        continue;
                    }
                }
                if (t->state != TASK_RUNNING)
                    continue;
            }
            deliver_signals(t);
            if (t->state != TASK_RUNNING)
                continue;
            if (s->dev)
                gpdev_tick(s->dev);
            run_slice(s, t);
            any_ran = true;
            if (t->slice_only_slept) {
                if (t->sleep_req_ns < min_sleep)
                    min_sleep = t->sleep_req_ns;
            } else {
                all_slept = false;
            }
            if (t->state == TASK_RUNNING)
                deliver_signals(t);
        }
        reap_dead(s);
        if (!any_alive)
            break;
        if (any_ran) {
            idle_rounds = 0;
            /* every running task only slept: nothing will move the timer, so
             * let the shortest requested sleep's worth of virtual time pass */
            if (all_slept && s->dev && !gpdev_sleeps_are_real(s->dev))
                gpdev_idle_advance_ns(s->dev, min_sleep ? min_sleep : 1000);
            continue;
        }
        /* idle: every live task waits. Let time pass. */
        idle_rounds++;
        uint64_t now = gsys_now_ns(s);
        uint64_t step = have_deadline ? (earliest > now ? earliest - now : 0) : 1000000ull;
        if (s->dev) { /* the next time the DAC frees a fragment, if a writer may be waiting for one */
            uint64_t dsp = gpdev_dsp_next_space_ns(s->dev);
            if (dsp < step)
                step = dsp;
        }
        if (step > 50000000ull)
            step = 50000000ull;
        if (!s->dev || gpdev_sleeps_are_real(s->dev)) {
            if (step) {
                struct timespec ts = { (time_t)(step / 1000000000ull), (long)(step % 1000000000ull) };
                nanosleep(&ts, NULL);
            }
        } else {
            gpdev_idle_advance_ns(s->dev, step);
        }
        if (idle_rounds > 20000) { /* about 20 s of waiting with no progress */
            gp_error("deadlock: %d task(s) waiting, nothing can wake them", s->ntasks);
            for (gtask_t *t = s->tasks; t; t = t->next)
                if (t->state == TASK_BLOCKED)
                    gp_error("  pid %d (%s) blocked kind %d svc %#x pc %08x", t->pid, t->comm, t->block, t->pending_svc,
                             cpu_regs(t->cpu)->r[15]);
            reason = GSYS_STOP_DEADLOCK;
            break;
        }
    }
    if (wait_status)
        *wait_status = s->first_exited ? s->first_status : -1;
    return reason;
}
