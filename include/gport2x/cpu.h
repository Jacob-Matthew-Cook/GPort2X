/* GPort2X ARMv4T interpreter (spec 1.2, 5): one cpu_t per guest thread, all
 * sharing one gmem_t.
 *
 * Semantics are those of an ARM920T in user mode (spec 5 lists where ARMv8
 * differs and why it matters):
 *   - ARM state only. A branch to Thumb (BX with bit 0 set) stops the CPU
 *     with CPU_STOP_THUMB; the spec says it never happens (1.2).
 *   - Unaligned LDR/SWP: the word at the aligned address, rotated right by
 *     8 x (addr & 3) (ARMv4 rotated load). Unaligned STR/STRH/LDRH: the low
 *     address bits are ignored (ARMv4 "forced alignment"). LDM/STM ignore
 *     bits [1:0]. Each unaligned access is reported through GP_TRACE_UNALIGNED
 *     (spec 5.3, OPEN-13).
 *   - PC reads as the instruction address + 8; a register-specified shift
 *     and a store of PC see + 12 (spec 5.4).
 *   - SWP/SWPB execute (they are undefined on ARMv8; the native engine
 *     emulates them on SIGILL, spec 5.2).
 *   - MRS/MSR: user mode; only the NZCV flags of CPSR are writable.
 *   - Coprocessor instructions (CDP/MCR/MRC/LDC/STC, i.e. FPA in the firmware
 *     userland, spec 5.6) and ARMv5+ encodings stop with CPU_STOP_UNDEF so
 *     the engine can emulate or report them.
 *   - SWI stops with CPU_STOP_SVC and the 24-bit immediate; pc already points
 *     at the next instruction, as after a kernel return.
 *   - A memory fault stops with CPU_STOP_DATA_ABORT / CPU_STOP_PREFETCH_ABORT
 *     before any register is written, with the faulting address. */
#ifndef GPORT2X_CPU_H
#define GPORT2X_CPU_H

#include "gport2x/gmem.h"
#include "gport2x/types.h"

typedef struct cpu cpu_t;

enum cpu_stop {
    CPU_STOP_NONE = 0,
    CPU_STOP_LIMIT,          /* the instruction budget ran out; resume with cpu_run */
    CPU_STOP_SVC,            /* svc #imm executed; info.svc_imm; pc = next instruction */
    CPU_STOP_UNDEF,          /* undefined or coprocessor instruction; pc = the instruction */
    CPU_STOP_DATA_ABORT,     /* data access fault; info.fault_addr; pc = the instruction */
    CPU_STOP_PREFETCH_ABORT, /* instruction fetch fault at pc */
    CPU_STOP_BREAKPOINT,     /* pc reached a breakpoint (before executing it) */
    CPU_STOP_THUMB,          /* an attempt to enter Thumb state; pc = the target */
    CPU_STOP_HALT,           /* cpu_request_stop() was called (e.g. by an MMIO handler) */
};

typedef struct cpu_stop_info {
    enum cpu_stop reason;
    gaddr_t pc;         /* see the per-reason notes above */
    uint32_t insn;      /* the instruction word (SVC, UNDEF, DATA_ABORT) */
    uint32_t svc_imm;   /* the 24-bit SWI immediate */
    gaddr_t fault_addr; /* DATA_ABORT / PREFETCH_ABORT */
    int fault_prot;     /* the access that failed, GMEM_PROT_R/W/X */
} cpu_stop_info_t;

/* The register file. r[13] = sp, r[14] = lr, r[15] = pc (the address of the
 * next instruction to execute, never +8). cpsr holds N Z C V in bits 31..28;
 * the mode bits always read as user mode (0x10). */
typedef struct cpu_regs {
    uint32_t r[16];
    uint32_t cpsr;
} cpu_regs_t;

#define CPSR_N 0x80000000u
#define CPSR_Z 0x40000000u
#define CPSR_C 0x20000000u
#define CPSR_V 0x10000000u
#define CPSR_MODE_USR 0x10u

cpu_t *cpu_create(gmem_t *m);
void cpu_destroy(cpu_t *c);
gmem_t *cpu_gmem(cpu_t *c);
cpu_regs_t *cpu_regs(cpu_t *c); /* live register file; may be read and written between runs */

/* Executes up to max_insns instructions (0 = unlimited) or until an event.
 * Returns the stop reason, which is also in *info (info may be NULL). */
enum cpu_stop cpu_run(cpu_t *c, uint64_t max_insns, cpu_stop_info_t *info);
uint64_t cpu_insn_count(const cpu_t *c); /* instructions retired since creation */

/* Asks the running cpu_run to stop after the current instruction (callable
 * from an MMIO handler or a signal handler). */
void cpu_request_stop(cpu_t *c);

/* Breakpoints (for tests and the debugger). */
int cpu_add_breakpoint(cpu_t *c, gaddr_t pc);
int cpu_remove_breakpoint(cpu_t *c, gaddr_t pc);

/* A memory watch: called after each successful load or store the
 * interpreter makes for this cpu (LDM/STM and SWP included), with the
 * address, the access size in bytes and the value. The JIT's inline accesses
 * do not call it: watch with the JIT off (cpu_set_jit(false)). NULL removes
 * it. Used by the call capture (--capture). */
typedef void (*cpu_watch_fn)(void *ctx, gaddr_t addr, unsigned size, uint32_t value, bool write);
void cpu_set_watch(cpu_t *c, cpu_watch_fn fn, void *ctx);

/* The JIT (docs/JIT.md): on by default where available (x86-64 Linux);
 * cpu_set_jit(false) makes every cpu interpret. Global, not per cpu. */
void cpu_set_jit(bool on);
bool cpu_jit_available(void);

/* Counters for the test modes (spec #23). */
uint64_t cpu_unaligned_count(const cpu_t *c);

#endif /* GPORT2X_CPU_H */
