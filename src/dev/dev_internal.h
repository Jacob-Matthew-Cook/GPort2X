/* Shared between dev.c (the model) and devices.c (the device files). */
#ifndef GPORT2X_DEV_INTERNAL_H
#define GPORT2X_DEV_INTERNAL_H

#include <stdio.h>
#include <strings.h>

#include "gport2x/dev.h"

#define DSP_RING_MAX 65536u

typedef struct dsp_state {
    bool open;
    uint32_t rate, channels, fmt; /* fmt: 8 or 16 (AFMT_U8 / AFMT_S16_LE) */
    uint32_t fragsize, nfrags;    /* the buffer is fragsize x nfrags bytes */
    uint8_t ring[DSP_RING_MAX];
    /* single producer (the guest's writes), single consumer (the DAC: the
     * device tick, or the host's audio callback in pull mode), possibly in
     * different threads or processes: monotonically growing positions */
    uint64_t rpos, wpos;
    bool pull; /* the host's audio callback drains the ring (gpdev_audio_pull), not the tick */
    uint64_t last_drain_ns, drain_frac;
    uint64_t opens, underruns;
    size_t want_room; /* bytes a blocked write waits for (0: none) */
    /* a blocking write larger than the whole buffer: OSS queues it piece by
     * piece and returns only when all of it is queued; the bytes of it
     * queued so far (of a write of inflight_n bytes) */
    size_t inflight, inflight_n;
} dsp_state_t;

static inline size_t dsp_fill(const dsp_state_t *s)
{
    return (size_t)(__atomic_load_n(&s->wpos, __ATOMIC_ACQUIRE) - __atomic_load_n(&s->rpos, __ATOMIC_ACQUIRE));
}

struct gpdev {
    gpdev_config_t cfg;
    char dump_dir[FS_PATH_MAX];
    char wav_path[FS_PATH_MAX];
    gmem_obj_t *upper, *regs, *blit;
    uint8_t *regfile;                        /* regfile_store, or the native engine's shared memfd */
    uint8_t regfile_store[GP2X_REGS_SIZE];
    uint8_t *blitregs;                       /* blitregs_store, or shared memory natively */
    uint8_t blitregs_store[GP2X_BLIT_SIZE];
    uint32_t blit_seen[32][4];               /* blit set-ups already logged (dst/src/pattern control, ROP control) */
    unsigned blit_nseen;
    uint64_t blits;
    gpaddr_t native_last_scanout;
    gpaddr_t native_latched_scanout;         /* native, stores trapped: the pair as of its last high-half write */            /* native mode: the scanout pair last seen by gpdev_native_poll */
    bool shared;                             /* native mode: this struct is shared memory, seen by every guest process */
    int fe_pid;                              /* native mode: the process whose tick drains the DAC and runs the front end */
    /* clock */
    uint64_t t0;
    uint64_t reads_counted;
    uint64_t idle_counts;   /* STEP modes: counts added while idle */
    uint64_t idle_frac_ns;
    bool cur_is_main;
    bool tcount_started;    /* the frame clock has been read: flips count from here */
    bool vsync_toggle;
    uint32_t palette[256];  /* the RGB layer's 8 bpp palette, 0x00RRGGBB */
    uint8_t pal_index;
    bool pal_half;          /* the next data write is the R half */      /* stepped clocks: GPIOB's vsync bit, flipped at each read */
    /* pad */
    uint32_t pad;
    struct { int frame; uint32_t mask; } *script;
    size_t script_n, script_cap, script_i;
    struct { int frame; uint32_t mask; } *tscript; /* host-time pad script (ms) */
    size_t tscript_n, tscript_cap, tscript_i;
    uint64_t tscript_t0;
    /* flips */
    uint64_t flips;
    uint64_t snapshots, next_snapshot_ns;
    gpdev_flip_fn flip_fn;
    void *flip_ctx;
    gpdev_host_fn host_fn;
    void *host_ctx;
    gpdev_audio_fn audio_fn;
    void *audio_ctx;
    uint16_t page[320 * 240];
    /* audio */
    dsp_state_t dsp;
    FILE *wav;
    uint32_t wav_bytes, wav_rate;
    uint16_t wav_channels;
    uint32_t mixer_pcm, mixer_volume;
    uint64_t audio_bytes_out;
    /* fb var state (FBIOPUT_VSCREENINFO latched) */
    uint8_t fb_var[2][160];
    bool fb_var_set[2];
};

uint32_t gpdev_reg_read(gpdev_t *d, uint32_t off, unsigned size);
void gpdev_reg_write(gpdev_t *d, uint32_t off, unsigned size, uint32_t v);
void gpdev_audio_emit(gpdev_t *d, const uint8_t *pcm, size_t n);

static inline void dsp_reset(dsp_state_t *s)
{
    s->rate = 8000;
    s->channels = 1;
    s->fmt = 8;
    s->fragsize = 512;
    s->nfrags = 8;
    __atomic_store_n(&s->rpos, __atomic_load_n(&s->wpos, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE); /* empty */
    s->drain_frac = 0;
    s->inflight = s->inflight_n = 0;
}

#endif
