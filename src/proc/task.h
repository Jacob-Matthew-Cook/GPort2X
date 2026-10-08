/* Internal process model shared by proc.c (tasks, scheduler, signals, fork,
 * exec, exit) and sys.c (syscall decode and marshalling). */
#ifndef GPORT2X_TASK_H
#define GPORT2X_TASK_H

#include <stdio.h>

#include "gport2x/cpu.h"
#include "gport2x/dev.h"
#include "gport2x/elf.h"
#include "gport2x/fs.h"
#include "gport2x/log.h"
#include "gport2x/proc.h"

#define GSYS_FIRST_PID 100
#define GSYS_INIT_PID 1
#define NSIG_GUEST 64
#define RLIM_NLIMITS 16
#define GUEST_RLIM_INFINITY 0xFFFFFFFFu
#define TRAMPOLINE_PAGE 0xFFFF0000u /* the kernel's vectors page: sigreturn trampolines */
#define TRAMPOLINE_SIGRETURN (TRAMPOLINE_PAGE + 0x500)
#define TRAMPOLINE_RT_SIGRETURN (TRAMPOLINE_PAGE + 0x504)
#define DEFAULT_STACK_LIMIT (8u << 20)
#define INITIAL_STACK_BYTES (128u << 10)

/* An address space (CLONE_VM). */
typedef struct gmm {
    int refs;
    gmem_t *mem;
    gaddr_t brk_start, brk;
    gaddr_t stack_top, stack_low; /* the main stack mapping [stack_low, stack_top), grown on faults */
    char exe[FS_PATH_MAX];        /* /proc/pid/exe */
} gmm_t;

/* Signal dispositions (CLONE_SIGHAND). */
typedef struct gsigact {
    gaddr_t handler; /* 0 = SIG_DFL, 1 = SIG_IGN */
    uint32_t flags;
    gaddr_t restorer;
    uint64_t mask;
} gsigact_t;

typedef struct gsighand {
    int refs;
    gsigact_t act[NSIG_GUEST + 1];
} gsighand_t;

/* cwd and umask (CLONE_FS). */
typedef struct gfsctx {
    int refs;
    fs_ctx_t ctx;
} gfsctx_t;

enum task_state { TASK_RUNNING = 0, TASK_BLOCKED, TASK_ZOMBIE, TASK_DEAD };

/* Why a task is blocked; the scheduler re-evaluates these each round. */
enum block_kind {
    BLK_NONE = 0,
    BLK_SYSCALL,  /* retry the syscall at pending_svc (its registers are untouched) */
    BLK_SLEEP,    /* nanosleep until deadline_ns (virtual or host time per clock mode) */
    BLK_SUSPEND,  /* rt_sigsuspend / sigsuspend / pause: until a deliverable signal */
    BLK_WAIT,     /* wait4: until a matching child changes state */
    BLK_POLL,     /* poll/select: until an fd is ready or deadline_ns */
};

typedef struct saved_frame {
    cpu_regs_t regs;
    uint64_t sigmask;
} saved_frame_t;

struct gtask {
    gsys_t *sys;
    int pid, ppid, pgid, sid;
    bool is_main;         /* the initial thread of a process (for the MAINSTEP clock) */
    cpu_t *cpu;
    gmm_t *mm;
    fs_fdtable_t *fdt;
    gfsctx_t *fs;
    gsighand_t *sighand;
    uint64_t sigpending, sigblocked;
    uint64_t suspend_oldmask; /* the mask to restore after sigsuspend */
    saved_frame_t *frames;    /* signal frames in flight (for sigreturn) */
    int nframes, frames_cap;
    gaddr_t altstack_sp;
    uint32_t altstack_size;
    int altstack_flags;
    enum task_state state;
    enum block_kind block;
    uint64_t deadline_ns;     /* BLK_SLEEP / BLK_POLL: virtual (or host) time */
    bool has_deadline;
    uint32_t pending_svc;     /* the svc immediate being retried */
    int wait_pid, wait_options; /* BLK_WAIT arguments */
    gaddr_t wait_status_ptr, wait_rusage_ptr;
    int exit_code;            /* wait status: (code << 8) or the signal */
    int exit_signal;          /* the parent's notification signal (clone's low byte; SIGCHLD for fork) */
    bool reaped;
    struct { uint32_t cur, max; } rlim[RLIM_NLIMITS];
    char comm[16];
    char cmdline[1024];
    size_t cmdline_len;
    uint32_t tls;
    uint64_t insns_run;
    bool slice_only_slept;   /* the last slice ended in a nanosleep that returned at once (step clock) */
    cpu_t *cap_armed;        /* capture.c: the cpu whose capture breakpoints are set */
    struct crec *cap_rec;    /* capture.c: the call being recorded */
    uint64_t sleep_req_ns;   /* its requested duration */
    gtask_t *next;
};

struct gsys {
    gsys_config_t cfg;
    fs_t *fs;
    gpdev_t *dev;
    gtask_t *tasks;
    int ntasks;
    int next_pid;
    gtask_t *first;
    gtask_t *current;         /* the task whose slice is running */
    bool first_exited;
    int first_status;
    uint64_t total_insns, syscalls;
    uint64_t slice_start; /* the current task's cpu count when its slice began (gsys_insn_count_now) */
    bool stop_requested;
    char hostname[65];
    uint64_t host_t0_ns;
    gmem_obj_t *trampoline; /* the shared vectors page */
    struct capture *cap;    /* capture.c, when cfg.capture is set */
};

/* capture.c: --capture (call records for checking reimplementations) */
int capture_create(gsys_t *s);
void capture_destroy(gsys_t *s);
void capture_before_slice(gsys_t *s, gtask_t *t);
void capture_after_slice(gsys_t *s, gtask_t *t);
bool capture_breakpoint(gsys_t *s, gtask_t *t, gaddr_t pc); /* true: a capture breakpoint, handled */
void capture_svc(gtask_t *t, uint32_t imm, bool after);

/* proc.c */
gtask_t *gsys_find_task(gsys_t *s, int pid);
void gtask_block(gtask_t *t, enum block_kind kind);
void gtask_set_deadline(gtask_t *t, uint64_t abs_ns);
uint64_t gsys_now_ns(gsys_t *s);           /* the clock sleeps and polls use (virtual or host) */
int64_t gsys_epoch_seconds(gsys_t *s);     /* time(): pinned in test mode */
uint64_t gsys_epoch_ns(gsys_t *s);
void gtask_exit(gtask_t *t, int wait_status); /* becomes a zombie (or dead if nobody waits) */
int gtask_send_signal(gtask_t *t, int sig, int from_pid);
int gsys_kill(gsys_t *s, gtask_t *from, int pid, int sig);
int gtask_fork(gtask_t *t, bool vfork, gtask_t **child);
int gtask_clone_thread(gtask_t *t, uint32_t flags, gaddr_t child_sp, gtask_t **child);
int gtask_execve(gtask_t *t, const char *path, char **argv, char **envp); /* frees nothing; argv/envp are host arrays */
int gtask_wait4(gtask_t *t, int pid, int options, gaddr_t status_ptr, gaddr_t rusage_ptr); /* -EAGAIN-style: returns 1 to block */
int gtask_sigreturn(gtask_t *t);
bool gtask_signal_deliverable(const gtask_t *t);
int gmm_brk(gtask_t *t, gaddr_t newbrk, gaddr_t *out);
int gmm_grow_stack(gtask_t *t, gaddr_t fault_addr);
int gmm_mmap(gtask_t *t, gaddr_t addr, uint32_t len, int prot, int gflags_map, int fd, uint64_t off, gaddr_t *out);
int gmm_munmap(gtask_t *t, gaddr_t addr, uint32_t len);
int gmm_mprotect(gtask_t *t, gaddr_t addr, uint32_t len, int prot);
int gmm_mremap(gtask_t *t, gaddr_t old_addr, uint32_t old_len, uint32_t new_len, int flags, gaddr_t new_addr, gaddr_t *out);

/* sys.c: performs the syscall whose svc immediate is imm (0 = EABI, number in
 * r7). Sets r0, or blocks the task (state/block set, registers untouched). */
void sys_dispatch(gtask_t *t, uint32_t imm);
bool gtask_deliver_signals(gtask_t *t); /* frames pending signals onto the task's registers */
/* Retries a blocked syscall: returns true if it completed (r0 set). */
bool sys_retry(gtask_t *t);

/* Guest mmap flags (asm-arm/mman.h). */
enum {
    GUEST_MAP_SHARED = 0x01, GUEST_MAP_PRIVATE = 0x02, GUEST_MAP_FIXED = 0x10, GUEST_MAP_ANONYMOUS = 0x20,
    GUEST_MAP_GROWSDOWN = 0x0100, GUEST_MAP_DENYWRITE = 0x0800, GUEST_MAP_EXECUTABLE = 0x1000,
    GUEST_MAP_LOCKED = 0x2000, GUEST_MAP_NORESERVE = 0x4000,
};
enum { GUEST_PROT_READ = 1, GUEST_PROT_WRITE = 2, GUEST_PROT_EXEC = 4 };

/* Guest signal numbers and sa_flags. */
enum {
    GSIGHUP = 1, GSIGINT = 2, GSIGQUIT = 3, GSIGILL = 4, GSIGTRAP = 5, GSIGABRT = 6, GSIGBUS = 7, GSIGFPE = 8,
    GSIGKILL = 9, GSIGUSR1 = 10, GSIGSEGV = 11, GSIGUSR2 = 12, GSIGPIPE = 13, GSIGALRM = 14, GSIGTERM = 15,
    GSIGCHLD = 17, GSIGCONT = 18, GSIGSTOP = 19, GSIGTSTP = 20, GSIGTTIN = 21, GSIGTTOU = 22, GSIGURG = 23,
    GSIGWINCH = 28, GSIGSYS = 31, GSIGRTMIN = 32,
};
enum {
    GSA_NOCLDSTOP = 1, GSA_NOCLDWAIT = 2, GSA_SIGINFO = 4, GSA_ONSTACK = 0x08000000, GSA_RESTART = 0x10000000,
    GSA_NODEFER = 0x40000000, GSA_RESETHAND = 0x80000000u, GSA_RESTORER = 0x04000000,
};

#endif
