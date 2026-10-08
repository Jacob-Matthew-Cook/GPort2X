/* The GP2X physical model: upper bank, register file, clock, pad, flips,
 * audio ring and sink. Device file operations are in devices.c. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__)
#include <sys/mman.h>
#endif
#include <unistd.h>
#include <time.h>

#include "gport2x/dev.h"
#include "gport2x/log.h"
#include "dev_internal.h"

#define REG_CLKCHGSTREG 0x0902
#define REG_TCOUNT 0x0A00
#define REG_GPIOC 0x1184
#define REG_GPIOD 0x1186
#define REG_GPIOM 0x1198
#define REG_DPC_CNTL 0x2800
#define REG_MLC_STL_EADRL 0x2912
#define REG_MLC_STL_EADRH 0x2914

void gpdev_config_default(gpdev_config_t *cfg)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->clock_mode = GPDEV_CLOCK_REAL;
    cfg->clock_step = 32 * 240;
    /* OPEN-6: fb0 at 0x03101000 is agreed; fb1 is the MMSP2 fb driver's second RGB
     * region, 0x03381000 (page-aligned, below the texture cache and the windows). */
    cfg->fb0_phys = 0x03101000;
    cfg->fb1_phys = 0x03381000;
}

/* ---- clock ------------------------------------------------------------- */

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t gpdev_virtual_ns(gpdev_t *d)
{
    if (d->cfg.clock_mode == GPDEV_CLOCK_REAL) {
        if (!d->t0)
            d->t0 = monotonic_ns();
        return monotonic_ns() - d->t0;
    }
    /* counted reads x step counts (plus idle time), at 7,372,800 counts per second */
    uint64_t counts = d->reads_counted * (uint64_t)d->cfg.clock_step + d->idle_counts;
    return counts * 1000000000ull / GP2X_TCOUNT_HZ;
}

void gpdev_idle_advance_ns(gpdev_t *d, uint64_t ns)
{
    if (d->cfg.clock_mode == GPDEV_CLOCK_REAL)
        return;
    uint64_t total = ns + d->idle_frac_ns;
    uint64_t counts = total * GP2X_TCOUNT_HZ / 1000000000ull;
    d->idle_frac_ns = total - counts * 1000000000ull / GP2X_TCOUNT_HZ;
    d->idle_counts += counts;
}

uint32_t gpdev_tcount_peek(const gpdev_t *d)
{
    if (d->cfg.clock_mode == GPDEV_CLOCK_REAL) {
        uint64_t ns = d->t0 ? monotonic_ns() - d->t0 : 0;
        return (uint32_t)(ns * GP2X_TCOUNT_HZ / 1000000000ull);
    }
    return (uint32_t)(d->reads_counted * d->cfg.clock_step + d->idle_counts);
}

static uint32_t tcount_read(gpdev_t *d)
{
    d->tcount_started = true;
    switch (d->cfg.clock_mode) {
    case GPDEV_CLOCK_REAL:
        if (!d->t0)
            d->t0 = monotonic_ns();
        return gpdev_tcount_peek(d);
    case GPDEV_CLOCK_MAINSTEP:
        if (!d->cur_is_main)
            return gpdev_tcount_peek(d);
        /* fall through */
    case GPDEV_CLOCK_STEP:
    default:
        d->reads_counted++;
        return gpdev_tcount_peek(d);
    }
}

void gpdev_set_current_thread(gpdev_t *d, bool is_main) { d->cur_is_main = is_main; }
bool gpdev_sleeps_are_real(const gpdev_t *d) { return d->cfg.clock_mode == GPDEV_CLOCK_REAL; }

/* ---- register file ----------------------------------------------------- */

static uint32_t regs_read(void *ctx, uint32_t off, unsigned size)
{
    gpdev_t *d = ctx;
    uint32_t v;
    if (off == REG_TCOUNT && size == 4) {
        v = tcount_read(d);
        memcpy(d->regfile + off, &v, 4); /* little-endian host assumed, as the whole harness does */
        gp_trace(GP_TRACE_MMIO, "regs rd32 %04x -> %08x (TCOUNT)", off, v);
        return v;
    }
    if (off + size > GP2X_REGS_SIZE)
        return 0;
    v = 0;
    for (unsigned i = 0; i < size; i++)
        v |= (uint32_t)d->regfile[off + i] << (8 * i);
    if (off <= REG_CLKCHGSTREG && REG_CLKCHGSTREG < off + size)
        v &= ~(1u << (8 * (REG_CLKCHGSTREG - off))); /* PLL change complete */
    gp_trace(GP_TRACE_MMIO, "regs rd%u %04x -> %0*x", size * 8, off, (int)size * 2, v);
    return v;
}

static void flip_event(gpdev_t *d, gpaddr_t page);

static void regs_write(void *ctx, uint32_t off, unsigned size, uint32_t value)
{
    gpdev_t *d = ctx;
    if (off + size > GP2X_REGS_SIZE)
        return;
    gp_trace(GP_TRACE_MMIO, "regs wr%u %04x <- %0*x", size * 8, off, (int)size * 2, value);
    for (unsigned i = 0; i < size; i++)
        d->regfile[off + i] = (uint8_t)(value >> (8 * i));
    /* The flip event: the RGB layer's even-field address pair is written low
     * half (0x2912) then high half (0x2914); the write of the high half makes
     * the MLC show the new page at the next vsync (spec 4.4). Writes before
     * the frame clock has started are init's presentation of the cleared
     * pages, which the oracle does not count as flips. */
    if (off <= REG_MLC_STL_EADRH && REG_MLC_STL_EADRH < off + size && d->tcount_started) {
        uint16_t lo, hi;
        memcpy(&lo, d->regfile + REG_MLC_STL_EADRL, 2);
        memcpy(&hi, d->regfile + REG_MLC_STL_EADRH, 2);
        flip_event(d, ((gpaddr_t)hi << 16) | lo);
    }
}

static const gmem_mmio_ops_t regs_ops = { regs_read, regs_write };

uint32_t gpdev_reg_read(gpdev_t *d, uint32_t off, unsigned size) { return regs_read(d, off, size); }
void gpdev_reg_write(gpdev_t *d, uint32_t off, unsigned size, uint32_t v) { regs_write(d, off, size, v); }

/* ---- pad --------------------------------------------------------------- */

static const struct { const char *name; int bit; } pad_names[] = {
    { "UP", 0 }, { "LEFT", 2 }, { "DOWN", 4 }, { "RIGHT", 6 },
    { "START", 8 }, { "SELECT", 9 }, { "L", 10 }, { "R", 11 },
    { "A", 12 }, { "B", 13 }, { "X", 14 }, { "Y", 15 },
    { "VOLDN", 22 }, { "VOLUP", 23 }, { "PUSH", 27 },
};

int gpdev_pad_bit(const char *name, size_t len)
{
    for (size_t i = 0; i < sizeof pad_names / sizeof *pad_names; i++)
        if (strlen(pad_names[i].name) == len && !strncasecmp(pad_names[i].name, name, len))
            return pad_names[i].bit;
    return -1;
}

uint32_t gpdev_pad_mask(const gpdev_t *d) { return d->pad; }

void gpdev_set_pad_mask(gpdev_t *d, uint32_t mask)
{
    d->pad = mask;
    uint32_t raw = ~mask; /* active low; every other pin reads released (1) */
    uint16_t m = (uint16_t)(0xFF00 | (raw & 0xFF));
    uint16_t c = (uint16_t)(0x00FF | (raw & 0xFF00));
    uint16_t dd = (uint16_t)((raw >> 16) & 0xFFFF);
    memcpy(d->regfile + REG_GPIOM, &m, 2);
    memcpy(d->regfile + REG_GPIOC, &c, 2);
    memcpy(d->regfile + REG_GPIOD, &dd, 2);
    gp_trace(GP_TRACE_DEV, "pad %08x -> GPIOM %04x GPIOC %04x GPIOD %04x", mask, m, c, dd);
}

typedef struct { int frame; uint32_t mask; } pad_entry_t;

static void pad_script_parse_into(const char *s, pad_entry_t **arr, size_t *n, size_t *cap)
{
    while (*s && *n < 4096) {
        int frame = atoi(s);
        const char *colon = strchr(s, ':');
        const char *end = strchr(s, ',');
        if (!end)
            end = s + strlen(s);
        uint32_t mask = 0;
        if (colon && colon < end) {
            const char *p = colon + 1;
            while (p < end) {
                const char *q = p;
                while (q < end && *q != '+')
                    q++;
                if (q > p && *p != '-') {
                    int bit = gpdev_pad_bit(p, (size_t)(q - p));
                    if (bit >= 0)
                        mask |= 1u << bit;
                    else
                        gp_warn("pad script: unknown key %.*s", (int)(q - p), p);
                }
                p = q + 1;
            }
        }
        if (*n == *cap) {
            *cap = *cap ? *cap * 2 : 64;
            *arr = realloc(*arr, *cap * sizeof **arr);
        }
        (*arr)[*n].frame = frame;
        (*arr)[*n].mask = mask;
        (*n)++;
        s = *end == ',' ? end + 1 : end;
    }
}

static void pad_script_parse(gpdev_t *d, const char *s)
{
    pad_script_parse_into(s, (pad_entry_t **)&d->script, &d->script_n, &d->script_cap);
    gp_info("pad script: %zu entries", d->script_n);
}

/* Host-time pad script: applied from gpdev_tick. */
static void pad_time_tick(gpdev_t *d)
{
    if (d->tscript_i >= d->tscript_n)
        return;
    if (!d->tscript_t0)
        d->tscript_t0 = monotonic_ns();
    uint64_t ms = (monotonic_ns() - d->tscript_t0) / 1000000ull;
    while (d->tscript_i < d->tscript_n && (uint64_t)d->tscript[d->tscript_i].frame <= ms) {
        gpdev_set_pad_mask(d, d->tscript[d->tscript_i].mask);
        gp_info("pad: %llu ms -> %08x", (unsigned long long)ms, d->pad);
        d->tscript_i++;
    }
}

static void pad_script_tick(gpdev_t *d, uint64_t frame)
{
    while (d->script_i < d->script_n && (uint64_t)d->script[d->script_i].frame <= frame) {
        gpdev_set_pad_mask(d, d->script[d->script_i].mask);
        gp_info("pad: frame %llu -> %08x", (unsigned long long)frame, d->pad);
        d->script_i++;
    }
}

/* ---- flips and frame dumps --------------------------------------------- */

int gpdev_read_page(gpdev_t *d, gpaddr_t phys, uint16_t *out)
{
    if (phys < GP2X_UPPER_BANK_PHYS || phys + GP2X_FB_PAGE_BYTES > GP2X_UPPER_BANK_PHYS + GP2X_UPPER_BANK_SIZE)
        return -EINVAL;
    memcpy(out, gmem_obj_host(d->upper) + (phys - GP2X_UPPER_BANK_PHYS), GP2X_FB_PAGE_BYTES);
    return 0;
}

int gpdev_write_ppm(const uint16_t *rgb565, const char *path)
{
    FILE *o = fopen(path, "wb");
    if (!o)
        return -errno;
    fprintf(o, "P6\n320 240\n255\n");
    static uint8_t buf[320 * 240 * 3];
    for (int i = 0; i < 320 * 240; i++) {
        unsigned p = rgb565[i];
        buf[i * 3 + 0] = (uint8_t)(((p >> 11) & 0x1F) * 255 / 31);
        buf[i * 3 + 1] = (uint8_t)(((p >> 5) & 0x3F) * 255 / 63);
        buf[i * 3 + 2] = (uint8_t)((p & 0x1F) * 255 / 31);
    }
    fwrite(buf, 1, sizeof buf, o);
    fclose(o);
    return 0;
}

static void flip_event(gpdev_t *d, gpaddr_t page)
{
    uint64_t frame = d->flips;
    if (gpdev_read_page(d, page, d->page) < 0) {
        gp_warn("flip %llu: scanout address %08x is outside the upper bank", (unsigned long long)frame, page);
        return;
    }
    if (d->dump_dir[0] && (d->cfg.dump_every < 2 || frame % (uint64_t)d->cfg.dump_every == 0)) {
        char path[FS_PATH_MAX + 32];
        snprintf(path, sizeof path, "%s/frame_%05llu.ppm", d->dump_dir, (unsigned long long)frame);
        int r = gpdev_write_ppm(d->page, path);
        if (r < 0)
            gp_warn("frame dump %s: %s", path, strerror(-r));
    }
    if (d->flip_fn)
        d->flip_fn(d->flip_ctx, frame, d->page);
    gp_trace(GP_TRACE_DEV, "flip %llu: page %08x", (unsigned long long)frame, page);
    d->flips++;
    pad_script_tick(d, d->flips);
}

/* A cacheflush of exactly one page inside the upper bank is the game's
 * presentation flush (Video_FlipBuffer, and init's page clears); it is
 * reported for the trace, but the flip itself is the scanout write. */
bool gpdev_cacheflush(gpdev_t *d, uint32_t start, uint32_t end)
{
    (void)d;
    return start >= GP2X_UPPER_BANK_PHYS && start < GP2X_UPPER_BANK_PHYS + GP2X_UPPER_BANK_SIZE &&
           end - start == GP2X_FB_PAGE_BYTES;
}

uint64_t gpdev_flip_count(const gpdev_t *d) { return d->flips; }

gpaddr_t gpdev_scanout_page(const gpdev_t *d)
{
    uint16_t lo, hi;
    memcpy(&lo, d->regfile + REG_MLC_STL_EADRL, 2);
    memcpy(&hi, d->regfile + REG_MLC_STL_EADRH, 2);
    gpaddr_t page = ((gpaddr_t)hi << 16) | lo;
    if (page < GP2X_UPPER_BANK_PHYS || page + GP2X_FB_PAGE_BYTES > GP2X_UPPER_BANK_PHYS + GP2X_UPPER_BANK_SIZE)
        page = d->cfg.fb0_phys;
    return page;
}

int gpdev_snapshot(gpdev_t *d, const char *path)
{
    int r = gpdev_read_page(d, gpdev_scanout_page(d), d->page);
    if (r < 0)
        return r;
    return gpdev_write_ppm(d->page, path);
}

static void maybe_snapshot(gpdev_t *d)
{
    if (!d->cfg.snapshot_ms || !d->dump_dir[0])
        return;
    uint64_t now = monotonic_ns();
    if (now < d->next_snapshot_ns)
        return;
    d->next_snapshot_ns = now + (uint64_t)d->cfg.snapshot_ms * 1000000ull;
    char path[FS_PATH_MAX + 32];
    snprintf(path, sizeof path, "%s/snap_%05llu.ppm", d->dump_dir, (unsigned long long)d->snapshots++);
    gpdev_snapshot(d, path);
}

void gpdev_set_flip_hook(gpdev_t *d, gpdev_flip_fn fn, void *ctx)
{
    d->flip_fn = fn;
    d->flip_ctx = ctx;
}

void gpdev_set_host_hook(gpdev_t *d, gpdev_host_fn fn, void *ctx)
{
    d->host_fn = fn;
    d->host_ctx = ctx;
}

void gpdev_set_audio_sink(gpdev_t *d, gpdev_audio_fn fn, void *ctx)
{
    d->audio_fn = fn;
    d->audio_ctx = ctx;
}

/* ---- audio sink and drain ---------------------------------------------- */

static void wav_header(FILE *f, uint32_t data_bytes, uint32_t rate, uint16_t channels)
{
    uint8_t h[44];
    uint32_t byte_rate = rate * channels * 2;
    memcpy(h, "RIFF", 4);
    uint32_t riff = 36 + data_bytes;
    memcpy(h + 4, &riff, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    uint32_t fmtlen = 16;
    memcpy(h + 16, &fmtlen, 4);
    uint16_t pcm = 1, bits = 16, align = (uint16_t)(channels * 2);
    memcpy(h + 20, &pcm, 2);
    memcpy(h + 22, &channels, 2);
    memcpy(h + 24, &rate, 4);
    memcpy(h + 28, &byte_rate, 4);
    memcpy(h + 32, &align, 2);
    memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &data_bytes, 4);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
}

static void wav_sink_write(gpdev_t *d, const uint8_t *pcm, size_t n);

/* The mixer PCM level as linear gain (OSS 0..100 per channel), in place. */
static void apply_mixer(const gpdev_t *d, uint8_t *buf, size_t n)
{
    uint32_t left = d->mixer_pcm & 0xFF, right = (d->mixer_pcm >> 8) & 0xFF;
    if (left > 100) left = 100;
    if (right > 100) right = 100;
    if (d->dsp.fmt != 16 || d->dsp.channels != 2 || (left == 100 && right == 100))
        return;
    for (size_t i = 0; i + 3 < n; i += 4) {
        int16_t l, r;
        memcpy(&l, buf + i, 2);
        memcpy(&r, buf + i + 2, 2);
        l = (int16_t)((int)l * (int)left / 100);
        r = (int16_t)((int)r * (int)right / 100);
        memcpy(buf + i, &l, 2);
        memcpy(buf + i + 2, &r, 2);
    }
}

void gpdev_audio_emit(gpdev_t *d, const uint8_t *pcm, size_t n)
{
    d->audio_bytes_out += n;
    static uint8_t tmp[65536];
    while (n) {
        size_t chunk = n < sizeof tmp ? n : sizeof tmp;
        memcpy(tmp, pcm, chunk);
        apply_mixer(d, tmp, chunk);
        if (d->audio_fn)
            d->audio_fn(d->audio_ctx, tmp, chunk, d->dsp.rate, d->dsp.channels, d->dsp.fmt);
        wav_sink_write(d, tmp, chunk);
        pcm += chunk;
        n -= chunk;
    }
}

static void wav_sink_write(gpdev_t *d, const uint8_t *pcm, size_t n)
{
    if (!d->wav) {
        if (!d->wav_path[0])
            return;
        d->wav = fopen(d->wav_path, "wb");
        if (!d->wav) {
            gp_warn("audio sink %s: %s", d->wav_path, strerror(errno));
            d->wav_path[0] = 0;
            return;
        }
        d->wav_rate = d->dsp.rate;
        d->wav_channels = (uint16_t)d->dsp.channels;
        wav_header(d->wav, 0, d->wav_rate, d->wav_channels);
    }
    fwrite(pcm, 1, n, d->wav);
    d->wav_bytes += (uint32_t)n;
}

/* Removes from the ring what the DAC consumed since the last call. */
void gpdev_tick(gpdev_t *d) { gpdev_tick_ex(d, true); }

void gpdev_run_host(gpdev_t *d)
{
    if (d->host_fn)
        d->host_fn(d->host_ctx);
}

void gpdev_tick_ex(gpdev_t *d, bool host)
{
    if (d->fe_pid && getpid() != d->fe_pid)
        return; /* native: another guest process; the front end's process drains the DAC it shares */
    maybe_snapshot(d);
    pad_time_tick(d);
    if (host && d->host_fn)
        d->host_fn(d->host_ctx);
    uint64_t now = gpdev_virtual_ns(d);
    dsp_state_t *s = &d->dsp;
    if (!s->open || s->pull) { /* in pull mode the host's audio callback is the DAC */
        s->last_drain_ns = now;
        return;
    }
    uint64_t elapsed = now - s->last_drain_ns;
    s->last_drain_ns = now;
    uint32_t frame_bytes = s->channels * (s->fmt == 16 ? 2 : 1);
    uint64_t want = (elapsed * s->rate * frame_bytes + s->drain_frac) / 1000000000ull;
    s->drain_frac = (elapsed * s->rate * frame_bytes + s->drain_frac) % 1000000000ull;
    if (want == 0)
        return;
    size_t fill = dsp_fill(s);
    if (want > fill) {
        if (fill) {
            s->underruns++;
            gp_trace(GP_TRACE_DEV, "dsp: underrun at %llu ns: %llu bytes due over %llu ns, %zu in the ring",
                     (unsigned long long)now, (unsigned long long)want, (unsigned long long)elapsed, fill);
        }
        want = fill;
    }
    while (want) {
        size_t head = (size_t)(s->rpos % DSP_RING_MAX);
        size_t chunk = DSP_RING_MAX - head;
        if (chunk > want)
            chunk = (size_t)want;
        gpdev_audio_emit(d, s->ring + head, chunk);
        __atomic_store_n(&s->rpos, s->rpos + chunk, __ATOMIC_RELEASE);
        want -= chunk;
    }
}

void gpdev_set_audio_pull(gpdev_t *d, bool on) { d->dsp.pull = on; }

bool gpdev_dsp_format(const gpdev_t *d, uint32_t *rate, uint32_t *channels, uint32_t *bits)
{
    *rate = d->dsp.rate;
    *channels = d->dsp.channels;
    *bits = d->dsp.fmt;
    return d->dsp.open;
}

size_t gpdev_audio_pull(gpdev_t *d, uint8_t *out, size_t n)
{
    dsp_state_t *s = &d->dsp;
    size_t frame = (size_t)s->channels * (s->fmt == 16 ? 2u : 1u);
    size_t have = s->open ? dsp_fill(s) : 0;
    size_t take = have < n ? have : n;
    if (frame)
        take -= take % frame;
    for (size_t done = 0; done < take;) {
        size_t head = (size_t)((s->rpos + done) % DSP_RING_MAX);
        size_t chunk = DSP_RING_MAX - head;
        if (chunk > take - done)
            chunk = take - done;
        memcpy(out + done, s->ring + head, chunk);
        done += chunk;
    }
    __atomic_store_n(&s->rpos, s->rpos + take, __ATOMIC_RELEASE);
    apply_mixer(d, out, take);
    memset(out + take, s->fmt == 16 ? 0 : 0x80, n - take); /* silence for what the guest has not written */
    wav_sink_write(d, out, n); /* --wav: exactly what the host plays */
    if (take < n && s->open && have)
        s->underruns++;
    d->audio_bytes_out += take;
    return take;
}

uint64_t gpdev_dsp_open_count(const gpdev_t *d) { return d->dsp.opens; }
uint64_t gpdev_dsp_underruns(const gpdev_t *d) { return d->dsp.underruns; }

uint64_t gpdev_dsp_next_space_ns(gpdev_t *d)
{
    const dsp_state_t *s = &d->dsp;
    if (!s->open || !s->rate || !s->fragsize)
        return UINT64_MAX;
    size_t cap = (size_t)s->fragsize * s->nfrags;
    size_t want = s->want_room ? s->want_room : s->fragsize;
    if (want > cap)
        want = cap;
    size_t fill = dsp_fill(s);
    if (fill + want <= cap)
        return UINT64_MAX; /* the waiting write (or a fragment) already fits */
    if (s->pull)
        return 1000000; /* the host's callback drains it: look again in a millisecond */
    uint64_t need = fill + want - cap;
    uint64_t bps = (uint64_t)s->rate * s->channels * (s->fmt == 16 ? 2u : 1u);
    return (need * 1000000000ull + bps - 1) / bps;
}
uint32_t gpdev_mixer_pcm(const gpdev_t *d) { return d->mixer_pcm; }
uint64_t gpdev_audio_bytes_out(const gpdev_t *d) { return d->audio_bytes_out; }

/* ---- lifecycle --------------------------------------------------------- */

gpdev_t *gpdev_create(const gpdev_config_t *cfg)
{
    gpdev_t *d;
    bool shared = false;
#if defined(__linux__)
    if (gmem_native_mode()) { /* the native engine: guest processes are host processes sharing one device */
        void *p = mmap(NULL, sizeof *d, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        d = p == MAP_FAILED ? NULL : p;
        shared = true;
    } else
#endif
        d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->shared = shared;
    d->cfg = *cfg;
    if (!d->cfg.clock_step)
        d->cfg.clock_step = 32 * 240;
    if (cfg->dump_dir)
        snprintf(d->dump_dir, sizeof d->dump_dir, "%s", cfg->dump_dir);
    if (cfg->wav_path)
        snprintf(d->wav_path, sizeof d->wav_path, "%s", cfg->wav_path);
    d->cfg.dump_dir = d->dump_dir;
    d->cfg.wav_path = d->wav_path;
    d->cfg.pad_script = NULL;
    d->upper = gmem_obj_ram(GP2X_UPPER_BANK_SIZE);
    d->regfile = d->regfile_store;
    d->regs = gmem_obj_mmio(GP2X_REGS_SIZE, &regs_ops, d);
    if (!d->upper || !d->regs) {
        gpdev_destroy(d);
        return NULL;
    }
    d->cur_is_main = true;
    gpdev_set_pad_mask(d, 0);
    d->mixer_pcm = 0;
    dsp_reset(&d->dsp);
    if (cfg->pad_script && *cfg->pad_script)
        pad_script_parse(d, cfg->pad_script);
    if (cfg->pad_time_script && *cfg->pad_time_script) {
        pad_script_parse_into(cfg->pad_time_script, (pad_entry_t **)&d->tscript, &d->tscript_n, &d->tscript_cap);
        gp_info("pad time script: %zu entries", d->tscript_n);
    }
    d->cfg.pad_time_script = NULL;
    gp_info("devices: fb0 %08x fb1 %08x, clock %s (step %u)", d->cfg.fb0_phys, d->cfg.fb1_phys,
            d->cfg.clock_mode == GPDEV_CLOCK_REAL ? "real" : d->cfg.clock_mode == GPDEV_CLOCK_STEP ? "step" : "mainstep",
            d->cfg.clock_step);
    return d;
}

void gpdev_destroy(gpdev_t *d)
{
    if (!d)
        return;
    if (d->wav) {
        wav_header(d->wav, d->wav_bytes, d->wav_rate, d->wav_channels);
        fclose(d->wav);
    }
    if (d->upper)
        gmem_obj_release(d->upper);
    if (d->regs)
        gmem_obj_release(d->regs);
    free(d->script);
    free(d->tscript);
#if defined(__linux__)
    if (d->shared) {
        munmap(d, sizeof *d);
        return;
    }
#endif
    free(d);
}

void gpdev_set_frontend_process(gpdev_t *d, int pid) { d->fe_pid = pid; }

gmem_obj_t *gpdev_upper_bank(gpdev_t *d) { return d->upper; }
gmem_obj_t *gpdev_regs(gpdev_t *d) { return d->regs; }

/* Native engine (docs/NATIVE_ENGINE.md, spec 4.1): the guest reads and writes
 * the register file as plain shared memory, so the device's read and write
 * side effects become a poll: gpdev_native_poll stores TCOUNT, keeps the
 * PLL-change status bit clear and reports a flip when the scanout address
 * pair changes. */
int gpdev_native_regs(gpdev_t *d)
{
    gmem_obj_t *o = gmem_obj_ram(GP2X_REGS_SIZE);
    if (!o)
        return -1;
    memcpy(gmem_obj_host(o), d->regfile, GP2X_REGS_SIZE);
    gmem_obj_release(d->regs);
    d->regs = o;
    d->regfile = gmem_obj_host(o);
    return 0;
}

void gpdev_native_poll(gpdev_t *d, bool flips)
{
    uint32_t v = tcount_read(d);
    __atomic_store_n((uint32_t *)(void *)(d->regfile + REG_TCOUNT), v, __ATOMIC_RELAXED);
    if (d->regfile[REG_CLKCHGSTREG] & 1u)
        d->regfile[REG_CLKCHGSTREG] &= (uint8_t)~1u; /* PLL change complete */
    if (!flips)
        return;
    uint16_t lo, hi;
    memcpy(&lo, d->regfile + REG_MLC_STL_EADRL, 2);
    memcpy(&hi, d->regfile + REG_MLC_STL_EADRH, 2);
    gpaddr_t page = ((gpaddr_t)hi << 16) | lo;
    if (page != d->native_last_scanout) {
        d->native_last_scanout = page;
        if (page)
            flip_event(d, page);
    }
}
