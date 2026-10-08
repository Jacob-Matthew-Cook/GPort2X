/* The SDL2 front end: a window (or the whole screen) showing the scanned-out
 * page, the keyboard and game controllers as the pad (controllers mapped by
 * button position), the OSS output on the host's audio. Built only with SDL2. */
#ifndef GPORT2X_SDL_HOST_H
#define GPORT2X_SDL_HOST_H

#include "gport2x/dev.h"
#include "gport2x/proc.h"

typedef struct sdl_host sdl_host_t;

/* scale: window size multiplier (0 = 3); fullscreen: the whole display,
 * integer-scaled. Returns NULL without SDL. */
sdl_host_t *sdl_host_create(gpdev_t *dev, gsys_t *sys, int scale, bool audio, bool fullscreen);
void sdl_host_destroy(sdl_host_t *h);

#endif
