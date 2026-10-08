/* GPort2X devices: the GP2X physical model and the device nodes (spec 1.5,
 * 3, 4, #9-#16).
 *
 *   - one 32 MB upper-bank object (phys 0x02000000..0x03FFFFFF) that every
 *     /dev/mem window, fb0 and fb1 alias;
 *   - one 64 KB MMSP2 register file (phys 0xC0000000) as latched memory
 *     with the live behaviours: TCOUNT (+0x0A00) counting at 7,372,800 Hz,
 *     GPIO pin levels (+0x1184/+0x1186/+0x1198) from the host pad, active
 *     low, CLKCHGSTREG (+0x0902) bit 0 reading 0, DPC_CNTL (+0x2800) bit 8
 *     reading 0;
 *   - /dev/mem (offset-validated mappings), /dev/fb0 and /dev/fb1
 *     (FBIOGET_FSCREENINFO with the fb physical pages, RW mappings into the
 *     bank), /dev/dsp (OSS: SPEED/SETFMT/STEREO/SETFRAGMENT/GETOSPACE,
 *     8 x 512-byte buffering, writes that return -EAGAIN when full so the
 *     scheduler parks the thread), /dev/mixer (WRITE_PCM as linear gain),
 *     /dev/mmuhack, and the menu-only nodes;
 *   - the flip event: the write of the RGB layer's scanout address (the high
 *     half at +0x2914, after the low half at +0x2912) once the frame clock
 *     (TCOUNT) has been read, which is exactly Video_FlipBuffer's
 *     cacheflush-then-program sequence and excludes init's presentation of
 *     the cleared pages and overlay flushes; frames are dumped as
 *     frame_%05d.ppm with the oracle's numbering and conversion.
 *
 * Clock modes (spec #23, 8.2): REAL follows CLOCK_MONOTONIC; STEP advances
 * TCOUNT by a fixed step per 32-bit read; MAINSTEP counts only the main
 * thread's reads (the oracle runs without the audio thread, so this keeps
 * the main thread's tick sequence identical to the oracle's). Virtual time,
 * which paces the audio device, is derived from TCOUNT in the step modes. */
#ifndef GPORT2X_DEV_H
#define GPORT2X_DEV_H

#include "gport2x/fs.h"
#include "gport2x/gmem.h"

typedef struct gpdev gpdev_t;

#define GP2X_TCOUNT_HZ 7372800u
#define GP2X_FB_PAGE_BYTES 0x25800u /* 320 x 240 x 2 */

enum gpdev_clock_mode {
    GPDEV_CLOCK_REAL = 0,
    GPDEV_CLOCK_STEP,
    GPDEV_CLOCK_MAINSTEP,
};

typedef struct gpdev_config {
    enum gpdev_clock_mode clock_mode;
    uint32_t clock_step;         /* raw counts per counted read; 7680 = 32 game ticks (oracle) */
    gpaddr_t fb0_phys, fb1_phys; /* OPEN-6; defaults 0x03101000 and 0x03381000 */
    const char *dump_dir;        /* frame_%05d.ppm per flip; NULL = no dumps */
    int dump_every;              /* 0 or 1 = every flip */
    const char *wav_path;        /* audio sink; NULL = discard */
    const char *pad_script;      /* "FLIP:KEYS,FLIP:-,..." (names UP DOWN LEFT RIGHT START SELECT L R A B X Y VOLUP VOLDN PUSH) */
    uint32_t snapshot_ms;        /* > 0: every N ms of host time, dump the scanned-out page as snap_%05d.ppm in dump_dir */
    const char *pad_time_script; /* "MS:KEYS,MS:-,...": the same keys keyed to host milliseconds (for the menu, which has no flips) */
} gpdev_config_t;

void gpdev_config_default(gpdev_config_t *cfg);
gpdev_t *gpdev_create(const gpdev_config_t *cfg);
void gpdev_destroy(gpdev_t *d);

gmem_obj_t *gpdev_upper_bank(gpdev_t *d);
gmem_obj_t *gpdev_regs(gpdev_t *d);
/* Registers every GP2X device node (and the devfs aliases) in devfs. */
int gpdev_register_nodes(gpdev_t *d, fs_backend_t *devfs);

/* Clock. The scheduler says whether the running guest thread is the main
 * thread (MAINSTEP). gpdev_tick drains the audio device for the virtual
 * time elapsed; call it at every syscall. */
void gpdev_set_current_thread(gpdev_t *d, bool is_main);
uint32_t gpdev_tcount_peek(const gpdev_t *d);
uint64_t gpdev_virtual_ns(gpdev_t *d);
bool gpdev_sleeps_are_real(const gpdev_t *d);
void gpdev_tick(gpdev_t *d);
/* STEP modes: lets ns of virtual time pass while every guest task waits
 * (TCOUNT advances accordingly). No effect in REAL mode. */
void gpdev_idle_advance_ns(gpdev_t *d, uint64_t ns);

/* Input: the game-layout, active-high mask (bits 0/2/4/6 UP/LEFT/DOWN/RIGHT,
 * 8..15 START SELECT L R A B X Y, 22 VOL-, 23 VOL+, 27 stick push). */
uint32_t gpdev_pad_mask(const gpdev_t *d);
void gpdev_set_pad_mask(gpdev_t *d, uint32_t mask);
int gpdev_pad_bit(const char *name, size_t len); /* -1 if unknown */

/* Flip events. gpdev_cacheflush reports whether a cacheflush covered one
 * display page (for the trace); the flip itself is the scanout write. */
bool gpdev_cacheflush(gpdev_t *d, uint32_t start, uint32_t end);
uint64_t gpdev_flip_count(const gpdev_t *d);
typedef void (*gpdev_flip_fn)(void *ctx, uint64_t flip, const uint16_t *rgb565);
void gpdev_set_flip_hook(gpdev_t *d, gpdev_flip_fn fn, void *ctx);
int gpdev_read_page(gpdev_t *d, gpaddr_t phys, uint16_t *out); /* 320 x 240 RGB565 */
/* The page the display shows: the MLC scanout address, or fb0 before it is programmed. */
gpaddr_t gpdev_scanout_page(const gpdev_t *d);
/* Writes the scanned-out page to path (a PPM). */
int gpdev_snapshot(gpdev_t *d, const char *path);
int gpdev_write_ppm(const uint16_t *rgb565, const char *path);

/* Host front-end hooks. The host hook runs from gpdev_tick (every syscall
 * and scheduler round): a front end pumps its events, updates the pad and
 * presents the scanned-out page there. The audio sink receives the PCM the
 * DAC consumed (after the mixer gain), in the device's current format. */
typedef void (*gpdev_host_fn)(void *ctx);
void gpdev_set_host_hook(gpdev_t *d, gpdev_host_fn fn, void *ctx);
typedef void (*gpdev_audio_fn)(void *ctx, const uint8_t *pcm, size_t n, uint32_t rate, uint32_t channels, uint32_t bits);
void gpdev_set_audio_sink(gpdev_t *d, gpdev_audio_fn fn, void *ctx);

/* Audio diagnostics (spec 8.1: the reopen counter must stay 0). */
uint64_t gpdev_dsp_open_count(const gpdev_t *d);
uint64_t gpdev_dsp_underruns(const gpdev_t *d); /* times the DAC found the ring short */
/* Nanoseconds until the DAC has freed room for the blocked write (or a fragment),
 * or UINT64_MAX if it already fits (or no stream). */
uint64_t gpdev_dsp_next_space_ns(gpdev_t *d);
uint32_t gpdev_mixer_pcm(const gpdev_t *d);
uint64_t gpdev_audio_bytes_out(const gpdev_t *d);

/* Native engine: the register file as shared memory (call before the guest
 * maps it) and the poll that replaces its read/write side effects (TCOUNT,
 * the PLL status bit; with flips, a flip when the scanout address changes). */
int gpdev_native_regs(gpdev_t *d);
void gpdev_native_poll(gpdev_t *d, bool flips);
/* Native: only process pid ticks the device (drains the DAC to the front end, runs its events). */
void gpdev_set_frontend_process(gpdev_t *d, int pid);
/* gpdev_tick without (host = false) or with the host hook; the hook alone.
 * The native engine runs the hook on its own thread, outside its lock,
 * because a display update can block. */
void gpdev_tick_ex(gpdev_t *d, bool host);
void gpdev_run_host(gpdev_t *d);
/* Pull mode (a real-time host): the host's audio callback is the DAC and
 * takes the guest's samples with gpdev_audio_pull, which fills what the
 * guest has not written with silence and returns the bytes it took; the
 * device tick then drains nothing. gpdev_dsp_format says whether /dev/dsp is
 * open and in which format. */
void gpdev_set_audio_pull(gpdev_t *d, bool on);
bool gpdev_dsp_format(const gpdev_t *d, uint32_t *rate, uint32_t *channels, uint32_t *bits);
size_t gpdev_audio_pull(gpdev_t *d, uint8_t *out, size_t n);

#endif /* GPORT2X_DEV_H */
