/* One-instruction emulator for the native engine (docs/NATIVE_ENGINE.md,
 * spec 5.2, 5.3, 5.6).
 *
 * On an ARMv8 core in AArch32 state some ARMv4 instructions cannot run:
 * SWP/SWPB and the firmware's FPA (CP1/CP2) instructions raise SIGILL, and
 * LDM/STM with an unaligned base raise SIGBUS where the ARM920T ignored the
 * low address bits. The native engine's signal handler passes the guest
 * registers here; aemu_step executes exactly that one instruction with the
 * interpreter's semantics (the ones every gate checks) and returns the
 * registers advanced past it.
 *
 * Memory is the caller's: page_host() returns the host address of a guest
 * page if it may be accessed with the given protection, or NULL. In the
 * native engine guest addresses are host addresses, so it returns the page
 * itself after checking the engine's mapping table; the tests use a buffer.
 * Pages are mapped into a private gmem only while one instruction runs.
 *
 * SWP and SWPB are done as a real atomic exchange on the host word or byte
 * (LDREX/STREX on an armhf host), because guest threads are host threads
 * there and the 8 game sites are LinuxThreads spinlocks (spec 5.2).
 *
 * The FPA register file lives in the aemu_t, so the caller keeps one aemu_t
 * per guest thread. Not async-signal-unsafe beyond malloc on the first use
 * of a page table level; the engine creates its aemu_t (and warms it) before
 * installing the handlers. */
#ifndef GPORT2X_AEMU_H
#define GPORT2X_AEMU_H

#include "gport2x/types.h"

typedef struct aemu aemu_t;
typedef uint8_t *(*aemu_page_fn)(void *ctx, gaddr_t page, int prot);

enum aemu_result {
    AEMU_OK = 0,      /* executed (or skipped by its condition); regs advanced */
    AEMU_SEGV,        /* the access faulted: deliver SIGSEGV at *fault */
    AEMU_UNDEF,       /* not an instruction the emulator handles: deliver SIGILL */
};

aemu_t *aemu_create(aemu_page_fn page_host, void *ctx);
void aemu_destroy(aemu_t *a);

/* r[15] is the address of the instruction; on AEMU_OK it is the next one.
 * cpsr carries NZCV in bits 31..28. *fault receives the faulting address. */
enum aemu_result aemu_step(aemu_t *a, uint32_t r[16], uint32_t *cpsr, gaddr_t *fault);

#endif /* GPORT2X_AEMU_H */
