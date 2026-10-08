/* The native engine: see include/gport2x/native.h and docs/NATIVE_ENGINE.md.
 *
 * The harness process is the guest process. The harness is linked high
 * (0xE0000000, `make armhf-native`) and the guest range [0x8000, 0xC0000000)
 * is reserved PROT_NONE at start-up; gmem's native mode turns every guest
 * mapping into a real mapping at its guest address. Guest code runs on the
 * CPU, and the events the environment must see arrive as signals:
 *
 * - SIGSYS: a seccomp filter traps every system call made from the guest
 *   range (the harness, its libraries and the kernel's pages are above it).
 *   The handler copies the saved registers into the task's register block,
 *   runs the shared syscall layer (sys_dispatch, the interpreter's code),
 *   frames pending guest signals and copies the registers back; returning
 *   resumes the guest after its svc. OABI numbers come from the svc word.
 * - SIGILL / SIGBUS: SWP, FPA and LDM/STM with an unaligned base go to the
 *   one-instruction emulator (aemu, the interpreter's semantics).
 * - SIGSEGV: stack growth below the main stack, else the guest's fault.
 *
 * Threads: each guest task sharing the address space (LinuxThreads' clone
 * with CLONE_VM) runs on its own host thread; the syscall layer's task model
 * stays the reference (pids, wait4 with __WCLONE, exit signals, signals
 * between threads), guarded by one lock that a thread holds only inside its
 * handlers and drops while it waits. A signal posted to another guest
 * thread wakes that host thread (SIGWAKE). Processes (fork) are host
 * processes; wait4 for them and kill of them go to the host.
 *
 * The register file is shared memory (gpdev_native_regs), and so is the
 * whole device model (the DAC ring, the mixer, the flips), because the game
 * runs in a child of the firmware's shell; a front-end thread in the first
 * process keeps TCOUNT current, clears the PLL status bit, reports flips when
 * the scanout address changes, drains the DAC and runs the SDL front end. */
#include "gport2x/native.h"

#if defined(__arm__) && defined(__linux__)

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#include "gport2x/aemu.h"
#include "gport2x/cpu.h"
#include "gport2x/gmem.h"
#include "gport2x/log.h"
#include "../proc/task.h"

#define GUEST_LO 0x00008000u
#define GUEST_HI 0xC0000000u
#define ALTSTACK_BYTES (1u << 20)
#define SIGWAKE (SIGRTMIN + 4)
#define POLL_NS 400000L           /* TCOUNT and the register side effects: every 400 us (spec 4.3) */
#define TICK_EVERY 10             /* the device tick (DAC, front end): every 4 ms */
#define MAX_THREADS 64

static gsys_t *nsys;
static gpdev_t *ndev;
/* One lock for every guest process and thread: process-shared (the device
 * model is shared memory across the guest's processes) and robust (a process
 * that exits holding it, as process_exit does, cannot wedge the others). */
static pthread_mutex_t *big;
static void lock_big(void)
{
    if (pthread_mutex_lock(big) == EOWNERDEAD)
        pthread_mutex_consistent(big);
}
static void unlock_big(void) { pthread_mutex_unlock(big); }
static __thread gtask_t *me;       /* this host thread's guest task */
static __thread aemu_t *myemu;
static __thread int entered;
static struct { gtask_t *t; pthread_t th; } threads[MAX_THREADS];
static int nthreads;

bool native_available(void) { return true; }

int native_prepare(void)
{
    /* Reserve every free part of the guest range; whatever the harness
     * already has there (its dynamic loader, on arm64 kernels) is recorded so
     * the guest never maps over it. */
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f)
        return -errno;
    uint32_t lo[64], hi[64];
    int n = 0;
    char line[512];
    while (fgets(line, sizeof line, f) && n < 64) {
        unsigned long a, b;
        if (sscanf(line, "%lx-%lx", &a, &b) == 2 && a < GUEST_HI && b > GUEST_LO) {
            lo[n] = (uint32_t)a;
            hi[n] = (uint32_t)b;
            n++;
        }
    }
    fclose(f);
    uint32_t cur = GUEST_LO;
    for (int i = 0; i <= n; i++) {
        uint32_t end = i < n ? (lo[i] > GUEST_LO ? lo[i] : GUEST_LO) : GUEST_HI;
        if (end > cur) {
            void *want = (void *)(uintptr_t)cur;
            void *p = mmap(want, end - cur, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE,
                           -1, 0);
            if (p != want) {
                gp_error("native engine: cannot reserve %08x..%08x of the guest range (%s)", cur, end,
                         p == MAP_FAILED ? strerror(errno) : "placed elsewhere");
                return -ENOMEM;
            }
        }
        if (i < n) {
            gp_info("native: the harness owns %08x..%08x inside the guest range; the guest will not map there", lo[i], hi[i]);
            gmem_native_forbid(lo[i], hi[i]);
            if (hi[i] > cur)
                cur = hi[i] < GUEST_HI ? hi[i] : GUEST_HI;
        }
    }
    void *lp = mmap(NULL, sizeof(pthread_mutex_t), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (lp == MAP_FAILED)
        return -ENOMEM;
    big = lp;
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setrobust(&ma, PTHREAD_MUTEX_ROBUST);
    pthread_mutex_init(big, &ma);
    pthread_mutexattr_destroy(&ma);
    if (gmem_set_native(true) != GP_OK)
        return -EINVAL;
    return 0;
}

/* ---- registers between the saved context and the task */

static void ctx_to_task(const mcontext_t *mc, gtask_t *t)
{
    cpu_regs_t *r = cpu_regs(t->cpu);
    const unsigned long *src = &mc->arm_r0; /* arm_r0..arm_pc are consecutive */
    for (int i = 0; i < 16; i++)
        r->r[i] = (uint32_t)src[i];
    r->cpsr = (uint32_t)(mc->arm_cpsr & 0xF0000000u) | CPSR_MODE_USR;
}

static void task_to_ctx(const gtask_t *t, mcontext_t *mc)
{
    const cpu_regs_t *r = cpu_regs(t->cpu);
    unsigned long *dst = &mc->arm_r0;
    for (int i = 0; i < 16; i++)
        dst[i] = r->r[i];
    mc->arm_cpsr = (mc->arm_cpsr & 0x0FFFFFC0u & ~0x20u) | (r->cpsr & 0xF0000000u) | 0x10u; /* user, ARM, flags */
}

/* ---- tasks and host threads */

static bool is_live(const gtask_t *t) { return t->state == TASK_RUNNING || t->state == TASK_BLOCKED; }

static int thread_index(const gtask_t *t)
{
    for (int i = 0; i < nthreads; i++)
        if (threads[i].t == t)
            return i;
    return -1;
}

/* SIGKILL every other process of our group (every guest process GPort2X
 * started is in it), until none is left: a dying one may have forked. */
static void kill_guest_processes(void)
{
    pid_t self = getpid(), group = getpgrp();
    for (int round = 0; round < 50; round++) {
        int found = 0;
        DIR *dir = opendir("/proc");
        if (!dir)
            return;
        struct dirent *e;
        while ((e = readdir(dir)) != NULL) {
            char *end;
            long pid = strtol(e->d_name, &end, 10);
            if (*end || pid <= 0 || pid == self)
                continue;
            char path[48], buf[512];
            snprintf(path, sizeof path, "/proc/%ld/stat", pid);
            int fd = open(path, O_RDONLY | O_CLOEXEC);
            if (fd < 0)
                continue;
            ssize_t n = read(fd, buf, sizeof buf - 1);
            close(fd);
            if (n <= 0)
                continue;
            buf[n] = 0;
            char *p = strrchr(buf, ')'), state;
            int ppid, pgrp;
            if (!p || sscanf(p + 1, " %c %d %d", &state, &ppid, &pgrp) != 3)
                continue;
            if (pgrp == group && state != 'Z' && state != 'X') {
                kill((pid_t)pid, SIGKILL);
                found++;
            }
        }
        closedir(dir);
        if (!found)
            return;
        struct timespec ts = { 0, 2000000 };
        nanosleep(&ts, NULL);
    }
}

static native_exit_fn exit_fn;
static void *exit_ctx;
static pid_t root_pid; /* GPort2X's own process: the first guest process */

void native_set_exit_hook(native_exit_fn fn, void *ctx)
{
    exit_fn = fn;
    exit_ctx = ctx;
}

static void process_exit(int st)
{
    gp_info("native: pid %d exits, status %#x", me ? me->pid : 0, st);
    if (getpid() == root_pid) { /* GPort2X ends: no guest process outlives it */
        kill_guest_processes();
        if (exit_fn)
            exit_fn(exit_ctx, "all tasks exited", st);
    }
    fflush(NULL);
    if (st & 0x7F) { /* killed by a signal: die of it, so the parent's wait4 sees the same status */
        int sig = st & 0x7F;
        struct sigaction dfl;
        memset(&dfl, 0, sizeof dfl);
        dfl.sa_handler = SIG_DFL;
        sigaction(sig, &dfl, NULL);
        sigset_t m;
        sigemptyset(&m);
        sigaddset(&m, sig);
        sigprocmask(SIG_UNBLOCK, &m, NULL);
        kill(getpid(), sig);
    }
    _exit((st & 0x7F) ? 128 + (st & 0x7F) : (st >> 8) & 0xFF);
}

/* The calling thread's task is gone: end the thread, or the process if no
 * task shares its address space any more. Called with the lock held. */
static void task_gone(gmm_t *mm)
{
    int st = me->exit_code;
    bool others = false;
    for (gtask_t *t = nsys->tasks; t; t = t->next)
        if (t != me && is_live(t) && t->mm == mm)
            others = true;
    if (!others)
        process_exit(st);
    int i = thread_index(me);
    if (i >= 0)
        threads[i] = threads[--nthreads];
    unlock_big();
    syscall(SYS_exit, 0); /* this thread only */
}

/* Frame pending signals; end the thread if the task is gone. Lock held. */
static void settle(gtask_t *t, gmm_t *mm)
{
    if (t->state == TASK_ZOMBIE || t->state == TASK_DEAD)
        task_gone(mm);
    gtask_deliver_signals(t);
    if (t->state == TASK_ZOMBIE || t->state == TASK_DEAD)
        task_gone(mm);
}

static void *thread_main(void *arg);

/* Start host threads for new guest threads, wake threads with signals to
 * take. Lock held. */
static void sync_threads(void)
{
    for (gtask_t *t = nsys->tasks; t; t = t->next) {
        if (!is_live(t) || !t->cpu || thread_index(t) >= 0)
            continue;
        if (nthreads == MAX_THREADS) {
            gp_error("native: too many guest threads");
            break;
        }
        threads[nthreads].t = t;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 256u << 10);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&threads[nthreads].th, &a, thread_main, t) != 0) {
            gp_error("native: cannot start a host thread for pid %d", t->pid);
            pthread_attr_destroy(&a);
            continue;
        }
        pthread_attr_destroy(&a);
        nthreads++;
    }
    for (int i = 0; i < nthreads; i++) {
        gtask_t *t = threads[i].t;
        if (t != me && t->state == TASK_RUNNING && (t->sigpending & ~t->sigblocked))
            pthread_kill(threads[i].th, SIGWAKE);
    }
}

/* ---- system calls the native engine performs on the host */

#define GCLONE_VM 0x00000100u
#define GWCLONE 0x80000000u
#define GWALL 0x40000000u

static uint32_t syscall_nr(uint32_t imm, const cpu_regs_t *r)
{
    if (imm == 0)
        return r->r[7];                      /* EABI */
    if (imm >= 0x900000u && imm < 0x9F0000u)
        return imm - 0x900000u;              /* OABI */
    return 0xFFFFFFFFu;                      /* ARM-private (0x9F0000+): the syscall layer's */
}

static int32_t do_fork(gtask_t *t)
{
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0)
        return -errno;
    if (pid == 0) { /* only this thread exists in the child */
        prctl(PR_SET_PDEATHSIG, SIGKILL); /* a guest process does not outlive its parent's death */
        for (gtask_t *o = nsys->tasks; o; o = o->next)
            if (o != t && o->mm == t->mm && is_live(o))
                o->state = TASK_DEAD;
        nthreads = 0;
        threads[nthreads].t = t;
        threads[nthreads].th = pthread_self();
        nthreads++;
        t->pid = getpid();
        t->ppid = getppid();
        return 0;
    }
    return pid;
}

static int32_t do_host_wait(gtask_t *t, int32_t pid, gaddr_t statusp, int32_t options, gaddr_t rusagep)
{
    for (;;) {
        int st = 0;
        pid_t r = wait4(pid, &st, options | WNOHANG, NULL);
        if (r < 0)
            return -errno;
        if (r == 0) {
            if (options & WNOHANG)
                return 0;
            if (t->sigpending & ~t->sigblocked)
                return -EINTR; /* the caller frames it (or acts on it: a kill ends the task there) */
            unlock_big();
            struct timespec ts = { 0, 1000000 };
            nanosleep(&ts, NULL);
            lock_big();
            continue;
        }
        if (statusp) {
            uint32_t v = (uint32_t)st;
            if (gmem_write(t->mm->mem, statusp, &v, 4) != GP_OK)
                return -EFAULT;
        }
        if (rusagep && gmem_memset(t->mm->mem, rusagep, 0, 72) != GP_OK)
            return -EFAULT;
        return r;
    }
}

static int32_t do_pipe(gtask_t *t, gaddr_t fdsp)
{
    int hfd[2];
    /* non-blocking on the host: an empty (full) pipe parks the task through
     * the syscall layer's -EAGAIN rule, so it waits outside the lock while
     * the process at the other end runs */
    if (pipe2(hfd, O_NONBLOCK) != 0)
        return -errno;
    fs_file_t *rd = fs_file_hostfd_owned(hfd[0], GUEST_O_RDONLY);
    fs_file_t *wr = fs_file_hostfd_owned(hfd[1], GUEST_O_WRONLY);
    if (!rd || !wr) {
        close(hfd[0]);
        close(hfd[1]);
        return -ENOMEM;
    }
    int g0 = fs_fdt_install(t->fdt, rd, false, 0);
    int g1 = g0 < 0 ? g0 : fs_fdt_install(t->fdt, wr, false, 0);
    fs_file_unref(rd);
    fs_file_unref(wr);
    if (g1 < 0) {
        if (g0 >= 0)
            fs_fdt_close(t->fdt, g0);
        return g1;
    }
    uint32_t v[2] = { (uint32_t)g0, (uint32_t)g1 };
    if (gmem_write(t->mm->mem, fdsp, v, 8) != GP_OK)
        return -EFAULT;
    return 0;
}

/* Returns true (with r0 set) if the native engine handled the call. Lock held. */
static bool native_syscall(gtask_t *t, uint32_t nr)
{
    cpu_regs_t *r = cpu_regs(t->cpu);
    int32_t res;
    switch (nr) {
    case 2:   /* fork */
    case 190: /* vfork: as fork */
        res = do_fork(t);
        break;
    case 120: /* clone: a thread is the syscall layer's (a host thread is started for it) */
        if (r->r[0] & GCLONE_VM)
            return false;
        res = do_fork(t);
        break;
    case 114: /* wait4 */
    case 7: { /* waitpid */
        int32_t pid = (int32_t)r->r[0], opt = (int32_t)r->r[2];
        if ((opt & (GWCLONE | GWALL)) || (pid > 0 && gsys_find_task(nsys, pid)))
            return false; /* a guest thread: the syscall layer's */
        res = do_host_wait(t, pid, r->r[1], opt, nr == 114 ? r->r[3] : 0);
        break;
    }
    case 37:  /* kill: a guest task here is the syscall layer's; anything else the host's */
        if ((int32_t)r->r[0] <= 0 || gsys_find_task(nsys, (int32_t)r->r[0]))
            return false;
        res = kill((pid_t)(int32_t)r->r[0], (int)r->r[1]) == 0 ? 0 : -errno;
        break;
    case 42:  /* pipe */
        res = do_pipe(t, r->r[0]);
        break;
    default:
        return false;
    }
    r->r[0] = (uint32_t)res;
    gp_trace(GP_TRACE_SYSCALL, "pid %d: native syscall %u = %d", t->pid, nr, res);
    return true;
}

/* ---- handlers */

static void on_sigsys(int sig, siginfo_t *si, void *uctx)
{
    (void)sig; (void)si;
    mcontext_t *mc = &((ucontext_t *)uctx)->uc_mcontext;
    lock_big();
    gtask_t *t = me;
    gmm_t *mm = t->mm;
    ctx_to_task(mc, t);
    uint32_t pc = cpu_regs(t->cpu)->r[15]; /* after the svc */
    uint32_t imm = *(const uint32_t *)(uintptr_t)(pc - 4u) & 0x00FFFFFFu;
    nsys->syscalls++;
    if (!native_syscall(t, syscall_nr(imm, cpu_regs(t->cpu)))) {
        sys_dispatch(t, imm);
        while (t->state == TASK_BLOCKED) { /* wait outside the lock, then retry */
            unlock_big();
            struct timespec ts = { 0, 500000 };
            nanosleep(&ts, NULL);
            lock_big();
            if (gtask_deliver_signals(t) && t->state != TASK_BLOCKED)
                break;
            sys_retry(t);
        }
        mm = t->mm ? t->mm : mm; /* execve replaces it */
    }
    settle(t, mm);
    sync_threads();
    task_to_ctx(t, mc);
    unlock_big();
}

static uint8_t *emu_page(void *ctx, gaddr_t page, int prot)
{
    (void)ctx;
    return gmem_page_host(me->mm->mem, page, prot);
}

static void on_sigill_bus(int sig, siginfo_t *si, void *uctx)
{
    mcontext_t *mc = &((ucontext_t *)uctx)->uc_mcontext;
    if ((uint32_t)mc->arm_pc >= GUEST_HI || !me) { /* the harness itself: a bug */
        fprintf(stderr, "gport2x native: harness %s at pc %08lx\n", sig == SIGILL ? "SIGILL" : "SIGBUS", mc->arm_pc);
        signal(sig, SIG_DFL);
        return;
    }
    lock_big();
    gtask_t *t = me;
    ctx_to_task(mc, t);
    cpu_regs_t *r = cpu_regs(t->cpu);
    gaddr_t fault = 0;
    enum aemu_result res = aemu_step(myemu, r->r, &r->cpsr, &fault);
    if (res != AEMU_OK) {
        gp_warn("native: pid %d %s at pc %08x addr %08x not emulated (%s)", t->pid, sig == SIGILL ? "SIGILL" : "SIGBUS",
                r->r[15], (unsigned)(uintptr_t)si->si_addr, res == AEMU_SEGV ? "fault" : "undefined");
        t->sigpending |= 1ull << ((res == AEMU_SEGV ? GSIGSEGV : (sig == SIGILL ? GSIGILL : GSIGBUS)) - 1);
        settle(t, t->mm);
    }
    task_to_ctx(t, mc);
    unlock_big();
}

static void on_sigsegv(int sig, siginfo_t *si, void *uctx)
{
    (void)sig;
    mcontext_t *mc = &((ucontext_t *)uctx)->uc_mcontext;
    uint32_t pc = (uint32_t)mc->arm_pc;
    gaddr_t addr = (gaddr_t)(uintptr_t)si->si_addr;
    if (pc >= GUEST_HI || !me) { /* the harness itself: a bug */
        fprintf(stderr, "gport2x native: harness fault at pc %08x addr %08x\n", pc, addr);
        signal(SIGSEGV, SIG_DFL);
        return;
    }
    lock_big();
    gtask_t *t = me;
    ctx_to_task(mc, t);
    if (gmm_grow_stack(t, addr) != 0) {
        gp_warn("native: pid %d data abort at pc %08x addr %08x lr %08x -> SIGSEGV", t->pid, pc, addr,
                (unsigned)mc->arm_lr);
        t->sigpending |= 1ull << (GSIGSEGV - 1);
        settle(t, t->mm);
        task_to_ctx(t, mc);
    }
    unlock_big();
}

/* A host signal for the guest (SIGCHLD, SIGPIPE, a kill from another
 * process), or SIGWAKE (another thread posted this one a signal): framed at
 * once if the guest was running here, else by the handler that was. */
static void on_forward(int sig, siginfo_t *si, void *uctx)
{
    (void)si;
    mcontext_t *mc = &((ucontext_t *)uctx)->uc_mcontext;
    gtask_t *t = me;
    if (!t || !entered)
        return;
    if (sig != SIGWAKE)
        __atomic_fetch_or(&t->sigpending, 1ull << (sig - 1), __ATOMIC_SEQ_CST);
    if ((uint32_t)mc->arm_pc < GUEST_HI) {
        lock_big();
        ctx_to_task(mc, t);
        settle(t, t->mm);
        task_to_ctx(t, mc);
        unlock_big();
    }
}

static void on_enter(int sig, siginfo_t *si, void *uctx)
{
    if (entered) { /* after its entry SIGUSR2 is an ordinary guest signal */
        on_forward(sig, si, uctx);
        return;
    }
    entered = 1;
    task_to_ctx(me, &((ucontext_t *)uctx)->uc_mcontext);
}

/* ---- set-up */

static int install(int sig, void (*fn)(int, siginfo_t *, void *))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fn;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    return sigaction(sig, &sa, NULL);
}

static bool forwarded(int sig)
{
    return sig != SIGKILL && sig != SIGSTOP && sig != SIGSYS && sig != SIGILL && sig != SIGBUS && sig != SIGSEGV;
}

/* Trap exactly the system calls made from the guest range. */
static int install_filter(void)
{
    struct sock_filter prog[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, instruction_pointer)),
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, GUEST_HI, 2, 0),
        BPF_JUMP(BPF_JMP | BPF_JGE | BPF_K, GUEST_LO, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_TRAP),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog fp = { (unsigned short)(sizeof prog / sizeof prog[0]), prog };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        return -errno;
    if (syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &fp) != 0)
        return -errno;
    return 0;
}

/* Per guest thread: an alternate signal stack and FPA state, then into the guest. */
static int thread_setup(gtask_t *t)
{
    me = t;
    myemu = aemu_create(emu_page, NULL);
    stack_t ss = { .ss_sp = malloc(ALTSTACK_BYTES), .ss_size = ALTSTACK_BYTES, .ss_flags = 0 };
    if (!myemu || !ss.ss_sp || sigaltstack(&ss, NULL) != 0)
        return -1;
    sigset_t m;
    sigemptyset(&m);
    pthread_sigmask(SIG_SETMASK, &m, NULL);
    return 0;
}

static void *thread_main(void *arg)
{
    if (thread_setup(arg) != 0) {
        gp_error("native: guest thread set-up failed");
        return NULL;
    }
    pthread_kill(pthread_self(), SIGUSR2); /* into the guest; it leaves through exit */
    return NULL;
}

/* Quit, with the lock held so no guest syscall (a fork) runs meanwhile: the
 * other guest processes first, then GPort2X itself, with the interpreter's
 * exit codes (0 at the flip limit, 125 on a stop request). */
static void quit_all(const char *why)
{
    gp_info("native: %s", why);
    kill_guest_processes();
    if (exit_fn)
        exit_fn(exit_ctx, why, -1);
    fflush(NULL);
    _exit(strcmp(why, "flip limit") ? 125 : 0);
}

/* The clock: TCOUNT and the register side effects every 400 us, the device
 * tick (draining the DAC to the front end) every 4 ms. It never waits on the
 * display, so the game's time keeps running whatever the screen does. */
static void *clock_main(void *arg)
{
    (void)arg;
    sigset_t m;
    sigfillset(&m);
    pthread_sigmask(SIG_BLOCK, &m, NULL); /* guest signals land on guest threads */
    for (unsigned n = 0;; n++) {
        bool tick = n % TICK_EVERY == 0;
        if (tick)
            lock_big();
        gpdev_native_poll(ndev, tick);
        if (tick) {
            gpdev_tick_ex(ndev, false);
            if (nsys->stop_requested)
                quit_all("stop requested");
            if (nsys->cfg.max_flips && gpdev_flip_count(ndev) >= nsys->cfg.max_flips)
                quit_all("flip limit");
            unlock_big();
        }
        struct timespec ts = { 0, POLL_NS };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

/* The front end (SDL: display, input, its events) on a thread of its own,
 * never holding the lock: a display update may block (a hidden window). */
static native_frontend_fn fe_start;
static void *fe_ctx;
static void *frontend_main(void *arg)
{
    (void)arg;
    sigset_t m;
    sigfillset(&m);
    pthread_sigmask(SIG_BLOCK, &m, NULL); /* SDL's own threads inherit this */
    if (fe_start)
        fe_start(fe_ctx);
    for (;;) {
        gpdev_run_host(ndev);
        struct timespec ts = { 0, 4000000 };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

int native_run(gsys_t *s, gtask_t *t, gpdev_t *dev, native_frontend_fn fe, void *ctx)
{
    nsys = s;
    ndev = dev;
    fe_start = fe;
    fe_ctx = ctx;
    s->current = t;
    t->pid = root_pid = getpid(); /* guest pids are host pids for processes */
    t->ppid = getppid();
    if (thread_setup(t) != 0) {
        gp_error("native engine: set-up failed");
        return -1;
    }
    threads[0].t = t;
    threads[0].th = pthread_self();
    nthreads = 1;
    if (install(SIGSYS, on_sigsys) || install(SIGILL, on_sigill_bus) || install(SIGBUS, on_sigill_bus) ||
        install(SIGSEGV, on_sigsegv) || install(SIGUSR2, on_enter) || install(SIGWAKE, on_forward)) {
        gp_error("native engine: cannot install the signal handlers");
        return -1;
    }
    for (int sig = 1; sig < 32; sig++)
        if (forwarded(sig) && sig != SIGUSR2)
            install(sig, on_forward);
    setpgid(0, 0); /* our own process group, so a quit ends every guest process */
    gpdev_set_frontend_process(ndev, getpid()); /* this process drains the shared DAC to the front end */
    pthread_t clock_th, fe_th;
    if (pthread_create(&clock_th, NULL, clock_main, NULL) != 0 || pthread_create(&fe_th, NULL, frontend_main, NULL) != 0) {
        gp_error("native engine: cannot start the clock and front-end threads");
        return -1;
    }
    int r = install_filter();
    if (r < 0) {
        gp_error("native engine: seccomp filter refused: %s", strerror(-r));
        return -1;
    }
    gp_info("native: entering the guest at %08x (sp %08x)", cpu_regs(t->cpu)->r[15], cpu_regs(t->cpu)->r[13]);
    fflush(NULL);
    pthread_kill(pthread_self(), SIGUSR2); /* returns into the guest; the guest leaves through exit */
    gp_error("native engine: the guest returned to the harness");
    return -1;
}

#else /* not a 32-bit ARM Linux build */

bool native_available(void) { return false; }
void native_set_exit_hook(native_exit_fn fn, void *ctx)
{
    (void)fn;
    (void)ctx;
}
int native_prepare(void) { return -1; }
int native_run(gsys_t *s, gtask_t *t, gpdev_t *dev, native_frontend_fn fe, void *ctx)
{
    (void)s; (void)t; (void)dev; (void)fe; (void)ctx;
    return -1;
}

#endif
