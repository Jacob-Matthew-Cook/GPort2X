/* GPort2X guest processes (spec 1.3, 1.4, 2, #4, #5, #17, #24): the
 * interpreter engine's process model and scheduler.
 *
 * A guest "task" is what a Linux 2.4 kernel calls a task: a process, or a
 * LinuxThreads thread (clone with CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_SIGHAND
 * and no CLONE_THREAD, so it has its own pid). Tasks share refcounted
 * address spaces, descriptor tables, fs contexts and signal handlers
 * according to their clone flags. One cpu_t runs each task; the scheduler
 * round-robins runnable tasks by instruction quanta and at every syscall,
 * parks tasks whose syscall would block, delivers signals, and advances the
 * clock when every task is waiting (idle). fork copies the address space
 * (shared mappings stay aliased); execve loads the new program into a fresh
 * space, with the -ENOEXEC and "#!" rules of spec 7.3. */
#ifndef GPORT2X_PROC_H
#define GPORT2X_PROC_H

#include "gport2x/dev.h"
#include "gport2x/fs.h"
#include "gport2x/types.h"

typedef struct gsys gsys_t;
typedef struct gtask gtask_t;

typedef struct gsys_config {
    fs_t *fs;              /* the guest namespace (owned by the caller) */
    gpdev_t *dev;          /* the devices (owned by the caller) */
    bool time_fixed;       /* time()/gettimeofday pinned to time_value + virtual time (test mode) */
    int64_t time_value;
    uint64_t quantum;      /* instructions per slice (0 = default) */
    uint64_t max_insns;    /* stop after this many instructions in total (0 = unlimited) */
    uint64_t max_flips;    /* stop after this many flips (0 = unlimited) */
    const char *uname_release;  /* "2.4.25" */
    const char *uname_version;  /* "#1 ..." */
    const char *uname_machine;  /* "armv4tl" */
    const char *hostname;       /* "gp2x" */
    uint32_t hwcap;             /* AT_HWCAP */
    const char *platform;       /* AT_PLATFORM */
    bool trace_syscalls;        /* also controlled by GPORT2X_TRACE=syscall */
    const char *scratch_dir;    /* host directory for guest-created tmpfs mounts */
    /* Call capture (src/proc/capture.c): function entries to record, as
     * "ADDR[,ADDR...]", JSON lines to capture_out; only in tasks whose
     * program path contains capture_exe (NULL: any). Needs the JIT off. */
    const char *capture, *capture_out, *capture_exe;
    unsigned capture_max;       /* calls recorded per address (0 = 3) */
    unsigned capture_skip;      /* calls passed over per address before recording */
    uint64_t capture_budget;    /* instructions before a call is written as incomplete (0 = 200M) */
} gsys_config_t;

void gsys_config_default(gsys_config_t *cfg);
gsys_t *gsys_create(const gsys_config_t *cfg);
void gsys_destroy(gsys_t *s);

/* The /proc facts for fs_backend_proc. */
const fs_procinfo_ops_t *gsys_procinfo_ops(void);

/* Starts a guest program as a new process (pid >= 100). path is a guest
 * path; argv/envp are NULL-terminated; cwd is the initial directory. */
int gsys_spawn(gsys_t *s, const char *path, const char *const *argv, const char *const *envp, const char *cwd,
               gtask_t **out);

enum gsys_stop {
    GSYS_STOP_ALL_EXITED = 0, /* no task left; result = the first task's wait status */
    GSYS_STOP_INSNS,          /* max_insns reached */
    GSYS_STOP_FLIPS,          /* max_flips reached */
    GSYS_STOP_DEADLOCK,       /* every task waits for something no task can provide */
    GSYS_STOP_REQUESTED,      /* gsys_request_stop */
};

/* Runs the scheduler until a stop condition. *wait_status (may be NULL)
 * receives the first spawned task's wait4-style status (exit code << 8, or
 * the terminating signal). */
enum gsys_stop gsys_run(gsys_t *s, int *wait_status);
void gsys_request_stop(gsys_t *s);

/* The task whose slice is running (NULL between slices), and a read of a
 * task's memory for state dumps (0 or a negative errno). */
gtask_t *gsys_current_task(const gsys_t *s);
int gtask_read_mem(gtask_t *t, gaddr_t addr, void *buf, uint32_t len);

/* Statistics. */
uint64_t gsys_insn_count(const gsys_t *s);
/* Instructions retired so far including the running slice (exact inside device hooks). */
uint64_t gsys_insn_count_now(const gsys_t *s);
int gsys_task_count(const gsys_t *s);
uint64_t gsys_syscall_count(const gsys_t *s);
int gtask_pid(const gtask_t *t);

#endif /* GPORT2X_PROC_H */
