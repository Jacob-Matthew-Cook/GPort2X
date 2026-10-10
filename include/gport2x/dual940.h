/* The ARM940T second core (src/proc/dual940.c). */
#ifndef GPORT2X_DUAL940_H
#define GPORT2X_DUAL940_H

#include <stdbool.h>
#include <stdint.h>

#include "gport2x/dev.h"

typedef struct dual940 dual940_t;

dual940_t *dual940_create(gpdev_t *d);
void dual940_destroy(dual940_t *c);
/* Follows DUALCTRL940 (starts, restarts or stops the core) and runs up to
 * budget instructions. Returns true while the core runs. */
bool dual940_step(dual940_t *c, uint64_t budget);

#endif
