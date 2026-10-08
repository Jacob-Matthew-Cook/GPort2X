/* The native engine (docs/NATIVE_ENGINE.md): guest code runs on the host CPU
 * in an armhf build of GPort2X on an AArch64 kernel with 32-bit compat
 * support. Available only where native_available() says so. */
#ifndef GPORT2X_NATIVE_H
#define GPORT2X_NATIVE_H

#include <stdbool.h>

#include "gport2x/dev.h"
#include "gport2x/proc.h"

/* True on a 32-bit ARM Linux build. */
bool native_available(void);

/* Call before anything maps guest memory: reserves the guest's address range
 * [0x8000, 0xC0000000) and switches gmem to native mode. Returns 0 or a
 * negative errno with a message logged. */
int native_prepare(void);

/* Called once on the front-end thread before it starts polling (the SDL
 * front end is created there, so its window and events live on one thread). */
typedef void (*native_frontend_fn)(void *ctx);

/* Called once in GPort2X's own process as it ends: why is "all tasks
 * exited" (status = the first guest process's wait status), "flip limit" or
 * "stop requested" (status -1). Every other guest process is gone by then.
 * It runs with guest syscalls held off and must not call into gsys. */
typedef void (*native_exit_fn)(void *ctx, const char *why, int status);
void native_set_exit_hook(native_exit_fn fn, void *ctx);

/* Runs the spawned task t natively; the process ends when the guest does
 * (or the front end requests a stop). Returns only on a set-up failure.
 * The register file must already be shared memory (gpdev_native_regs). */
int native_run(gsys_t *s, gtask_t *t, gpdev_t *dev, native_frontend_fn fe, void *ctx);

#endif /* GPORT2X_NATIVE_H */
