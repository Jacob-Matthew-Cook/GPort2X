/* Device files: /dev/mem, /dev/fb0|fb1, /dev/dsp, /dev/mixer, /dev/mmuhack,
 * /dev/GPIO, /dev/batt, /dev/cx25874 and the block-device placeholders. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "gport2x/log.h"
#include "dev_internal.h"

#define MKDEV(ma, mi) (((uint64_t)(ma) << 8) | (mi))

static int chr_fstat_common(fs_file_t *f, fs_stat_t *st, uint64_t rdev)
{
    memset(st, 0, sizeof *st);
    st->dev = f->backend ? f->backend->dev : 0;
    st->mode = S_IFCHR | 0600;
    st->nlink = 1;
    st->blksize = 4096;
    st->rdev = rdev;
    return 0;
}

/* ---- /dev/mem ---------------------------------------------------------- */

static bool in_bank(uint64_t off, uint64_t len)
{
    return off >= GP2X_UPPER_BANK_PHYS && off + len <= (uint64_t)GP2X_UPPER_BANK_PHYS + GP2X_UPPER_BANK_SIZE && off + len >= off;
}

static bool in_regs(uint64_t off, uint64_t len)
{
    return off >= GP2X_REGS_PHYS && off + len <= (uint64_t)GP2X_REGS_PHYS + GP2X_REGS_SIZE && off + len >= off;
}

static int64_t mem_read(fs_file_t *f, int64_t off, void *buf, size_t n)
{
    gpdev_t *d = f->priv;
    if (in_bank((uint64_t)off, n)) {
        memcpy(buf, gmem_obj_host(d->upper) + (off - GP2X_UPPER_BANK_PHYS), n);
        return (int64_t)n;
    }
    if (in_regs((uint64_t)off, n)) {
        uint8_t *dst = buf;
        for (size_t i = 0; i < n; i++)
            dst[i] = (uint8_t)gpdev_reg_read(d, (uint32_t)(off - GP2X_REGS_PHYS + (int64_t)i), 1);
        return (int64_t)n;
    }
    return -EFAULT;
}

static int64_t mem_write(fs_file_t *f, int64_t off, const void *buf, size_t n)
{
    gpdev_t *d = f->priv;
    if (in_bank((uint64_t)off, n)) {
        memcpy(gmem_obj_host(d->upper) + (off - GP2X_UPPER_BANK_PHYS), buf, n);
        return (int64_t)n;
    }
    if (in_regs((uint64_t)off, n)) {
        const uint8_t *src = buf;
        for (size_t i = 0; i < n; i++)
            gpdev_reg_write(d, (uint32_t)(off - GP2X_REGS_PHYS + (int64_t)i), 1, src[i]);
        return (int64_t)n;
    }
    return -EFAULT;
}

static int mem_fstat(fs_file_t *f, fs_stat_t *st) { return chr_fstat_common(f, st, MKDEV(1, 1)); }

static int mem_mmap(fs_file_t *f, uint64_t off, uint32_t len, int prot, bool shared, gmem_obj_t **obj, uint32_t *obj_off)
{
    (void)prot; (void)shared;
    gpdev_t *d = f->priv;
    if (in_bank(off, len)) {
        *obj = gmem_obj_ref(d->upper);
        *obj_off = (uint32_t)(off - GP2X_UPPER_BANK_PHYS);
        gp_trace(GP_TRACE_DEV, "/dev/mem map phys %08llx len %x (upper bank)", (unsigned long long)off, len);
        return 0;
    }
    if (in_regs(off, len)) {
        *obj = gmem_obj_ref(d->regs);
        *obj_off = (uint32_t)(off - GP2X_REGS_PHYS);
        gp_trace(GP_TRACE_DEV, "/dev/mem map phys %08llx len %x (registers)", (unsigned long long)off, len);
        return 0;
    }
    if (off >= GP2X_BLIT_PHYS && off + len <= (uint64_t)GP2X_BLIT_PHYS + GP2X_BLIT_SIZE) {
        *obj = gmem_obj_ref(gpdev_blitter(d));
        *obj_off = (uint32_t)(off - GP2X_BLIT_PHYS);
        gp_trace(GP_TRACE_DEV, "/dev/mem map phys %08llx len %x (blitter)", (unsigned long long)off, len);
        return 0;
    }
    gp_warn("/dev/mem: mapping of phys %08llx len %x refused (outside the GP2X model)", (unsigned long long)off, len);
    return -EINVAL;
}

static const fs_file_ops_t mem_ops = { .read = mem_read, .write = mem_write, .fstat = mem_fstat, .mmap = mem_mmap };

static int mem_open(void *ctx, int gflags, fs_file_t **out)
{
    fs_file_t *f = fs_file_new(&mem_ops, gflags);
    if (!f)
        return -ENOMEM;
    f->priv = ctx;
    *out = f;
    return 0;
}

/* ---- /dev/fb0, /dev/fb1 ------------------------------------------------ */

typedef struct fb_ctx {
    gpdev_t *d;
    int index;
} fb_ctx_t;

static gpaddr_t fb_phys(const fb_ctx_t *c) { return c->index ? c->d->cfg.fb1_phys : c->d->cfg.fb0_phys; }

static int fb_fstat(fs_file_t *f, fs_stat_t *st)
{
    fb_ctx_t *c = f->priv;
    return chr_fstat_common(f, st, MKDEV(29, c->index));
}

static int64_t fb_read(fs_file_t *f, int64_t off, void *buf, size_t n)
{
    fb_ctx_t *c = f->priv;
    if (off < 0 || (uint64_t)off >= GP2X_FB_PAGE_BYTES)
        return 0;
    if (n > GP2X_FB_PAGE_BYTES - (uint64_t)off)
        n = (size_t)(GP2X_FB_PAGE_BYTES - (uint64_t)off);
    memcpy(buf, gmem_obj_host(c->d->upper) + (fb_phys(c) - GP2X_UPPER_BANK_PHYS) + off, n);
    return (int64_t)n;
}

static int64_t fb_write(fs_file_t *f, int64_t off, const void *buf, size_t n)
{
    fb_ctx_t *c = f->priv;
    if (off < 0 || (uint64_t)off >= GP2X_FB_PAGE_BYTES)
        return -ENOSPC;
    if (n > GP2X_FB_PAGE_BYTES - (uint64_t)off)
        n = (size_t)(GP2X_FB_PAGE_BYTES - (uint64_t)off);
    memcpy(gmem_obj_host(c->d->upper) + (fb_phys(c) - GP2X_UPPER_BANK_PHYS) + off, buf, n);
    return (int64_t)n;
}

static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void put16(uint8_t *p, uint16_t v) { memcpy(p, &v, 2); }

static int fb_ioctl(fs_file_t *f, uint32_t req, gmem_t *m, gaddr_t arg)
{
    fb_ctx_t *c = f->priv;
    switch (req) {
    case 0x4602: { /* FBIOGET_FSCREENINFO */
        uint8_t fix[68];
        memset(fix, 0, sizeof fix);
        memcpy(fix, c->index ? "mmsp2 fb1" : "mmsp2 fb0", 10);
        put32(fix + 16, fb_phys(c));          /* smem_start */
        put32(fix + 20, GP2X_FB_PAGE_BYTES);  /* smem_len */
        put32(fix + 24, 0);                   /* FB_TYPE_PACKED_PIXELS */
        put32(fix + 32, 2);                   /* FB_VISUAL_TRUECOLOR */
        put16(fix + 36, 0);
        put16(fix + 38, 0);
        put16(fix + 40, 0);
        put32(fix + 44, 640);                 /* line_length */
        put32(fix + 48, GP2X_REGS_PHYS);      /* mmio_start */
        put32(fix + 52, GP2X_REGS_SIZE);
        return gmem_write(m, arg, fix, sizeof fix) == GP_OK ? 0 : -EFAULT;
    }
    case 0x4600: { /* FBIOGET_VSCREENINFO */
        uint8_t var[160];
        if (c->d->fb_var_set[c->index]) {
            memcpy(var, c->d->fb_var[c->index], sizeof var);
        } else {
            memset(var, 0, sizeof var);
            put32(var + 0, 320); put32(var + 4, 240); put32(var + 8, 320); put32(var + 12, 240);
            put32(var + 24, 16);                          /* bits_per_pixel */
            put32(var + 32, 11); put32(var + 36, 5);      /* red */
            put32(var + 44, 5); put32(var + 48, 6);       /* green */
            put32(var + 56, 0); put32(var + 60, 5);       /* blue */
            put32(var + 88, 0xFFFFFFFFu); put32(var + 92, 0xFFFFFFFFu); /* height, width unknown */
        }
        return gmem_write(m, arg, var, sizeof var) == GP_OK ? 0 : -EFAULT;
    }
    case 0x4601: { /* FBIOPUT_VSCREENINFO: latch what the menu asks for (OPEN-20) */
        if (gmem_read(m, arg, c->d->fb_var[c->index], 160) != GP_OK)
            return -EFAULT;
        c->d->fb_var_set[c->index] = true;
        return 0;
    }
    case 0x4606: /* FBIOPAN_DISPLAY */
    case 0x4611: /* FBIOBLANK */
    case 0x4604: /* FBIOGETCMAP */
    case 0x4605: /* FBIOPUTCMAP */
        return 0;
    default:
        return -ENOTTY;
    }
}

static int fb_mmap(fs_file_t *f, uint64_t off, uint32_t len, int prot, bool shared, gmem_obj_t **obj, uint32_t *obj_off)
{
    (void)prot; (void)shared;
    fb_ctx_t *c = f->priv;
    /* As fb_mmap does: the mapping starts at the page holding smem_start and
     * may cover PAGE_ALIGN(smem_start & ~PAGE_MASK) + smem_len bytes. */
    gpaddr_t start = fb_phys(c) & GP2X_PAGE_MASK;
    uint32_t limit = GP2X_PAGE_ALIGN_UP((fb_phys(c) & (GP2X_PAGE_SIZE - 1)) + GP2X_FB_PAGE_BYTES);
    if (off >= limit || len > limit - off)
        return -EINVAL;
    *obj = gmem_obj_ref(c->d->upper);
    *obj_off = (uint32_t)(start - GP2X_UPPER_BANK_PHYS + off);
    gp_trace(GP_TRACE_DEV, "/dev/fb%d map off %llx len %x -> phys %08x", c->index, (unsigned long long)off, len,
             (uint32_t)(fb_phys(c) + off));
    return 0;
}

static void fb_release(fs_file_t *f) { free(f->priv); }

static const fs_file_ops_t fb_ops = { .read = fb_read, .write = fb_write, .fstat = fb_fstat, .ioctl = fb_ioctl, .mmap = fb_mmap, .release = fb_release };

static int fb_open_n(gpdev_t *d, int index, int gflags, fs_file_t **out)
{
    fs_file_t *f = fs_file_new(&fb_ops, gflags);
    fb_ctx_t *c = calloc(1, sizeof *c);
    if (!f || !c) {
        free(f);
        free(c);
        return -ENOMEM;
    }
    c->d = d;
    c->index = index;
    f->priv = c;
    *out = f;
    return 0;
}

static int fb0_open(void *ctx, int gflags, fs_file_t **out) { return fb_open_n(ctx, 0, gflags, out); }
static int fb1_open(void *ctx, int gflags, fs_file_t **out) { return fb_open_n(ctx, 1, gflags, out); }

/* ---- /dev/dsp (OSS) ---------------------------------------------------- */

static int rd_int(gmem_t *m, gaddr_t arg, int32_t *v)
{
    uint32_t u;
    if (gmem_ld32(m, arg, &u) != GP_OK)
        return -EFAULT;
    *v = (int32_t)u;
    return 0;
}

static int wr_int(gmem_t *m, gaddr_t arg, int32_t v)
{
    return gmem_st32(m, arg, (uint32_t)v) == GP_OK ? 0 : -EFAULT;
}

static size_t dsp_capacity(const dsp_state_t *s)
{
    size_t cap = (size_t)s->fragsize * s->nfrags;
    return cap > DSP_RING_MAX ? DSP_RING_MAX : cap;
}

static void dsp_queue(dsp_state_t *s, const void *buf, size_t n);

static int64_t dsp_write(fs_file_t *f, int64_t off, const void *buf, size_t n)
{
    (void)off;
    gpdev_t *d = f->priv;
    dsp_state_t *s = &d->dsp;
    gpdev_tick(d);
    size_t cap = dsp_capacity(s);
    size_t room = cap - dsp_fill(s);
    if (n <= cap) {
        if (room < n) {
            s->want_room = n; /* OSS blocks until the whole write fits; the scheduler wakes it then */
            return -EAGAIN;
        }
    } else {
        /* larger than the buffer: queue what fits and block for the rest,
         * then return the whole count (each retry repeats the same call) */
        size_t done = s->inflight_n == n ? s->inflight : 0;
        size_t m = n - done < room ? n - done : room;
        dsp_queue(s, (const uint8_t *)buf + done, m);
        done += m;
        if (done < n) {
            s->inflight = done;
            s->inflight_n = n;
            size_t want = s->fragsize ? s->fragsize : 1;
            s->want_room = n - done < want ? n - done : want;
            return -EAGAIN;
        }
        s->inflight = s->inflight_n = 0;
        s->want_room = 0;
        return (int64_t)n;
    }
    s->want_room = 0;
    dsp_queue(s, buf, n);
    return (int64_t)n;
}

static void dsp_queue(dsp_state_t *s, const void *buf, size_t n)
{
    const uint8_t *src = buf;
    size_t tail = (size_t)(s->wpos % DSP_RING_MAX);
    size_t first = DSP_RING_MAX - tail;
    if (first > n)
        first = n;
    memcpy(s->ring + tail, src, first);
    if (n > first)
        memcpy(s->ring, src + first, n - first);
    __atomic_store_n(&s->wpos, s->wpos + n, __ATOMIC_RELEASE);
}

static int64_t dsp_read(fs_file_t *f, int64_t off, void *buf, size_t n)
{
    (void)f; (void)off;
    memset(buf, 0, n); /* no capture: silence */
    return (int64_t)n;
}

static int dsp_fstat(fs_file_t *f, fs_stat_t *st) { return chr_fstat_common(f, st, MKDEV(14, 3)); }

static int dsp_ioctl(fs_file_t *f, uint32_t req, gmem_t *m, gaddr_t arg)
{
    gpdev_t *d = f->priv;
    dsp_state_t *s = &d->dsp;
    int32_t v;
    int r;
    switch (req) {
    case 0x5000: /* SNDCTL_DSP_RESET */
        __atomic_store_n(&s->rpos, __atomic_load_n(&s->wpos, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
        return 0;
    case 0x5001: /* SNDCTL_DSP_SYNC */
    case 0x5008: /* SNDCTL_DSP_POST */
        return 0;
    case 0xC0045002: /* SNDCTL_DSP_SPEED */
        if ((r = rd_int(m, arg, &v)) < 0) return r;
        if (v > 0) s->rate = (uint32_t)v;
        gp_trace(GP_TRACE_DEV, "dsp: speed %u", s->rate);
        return wr_int(m, arg, (int32_t)s->rate);
    case 0xC0045003: /* SNDCTL_DSP_STEREO */
        if ((r = rd_int(m, arg, &v)) < 0) return r;
        s->channels = v ? 2 : 1;
        return wr_int(m, arg, v ? 1 : 0);
    case 0xC0045006: /* SNDCTL_DSP_CHANNELS */
        if ((r = rd_int(m, arg, &v)) < 0) return r;
        if (v == 1 || v == 2) s->channels = (uint32_t)v;
        return wr_int(m, arg, (int32_t)s->channels);
    case 0xC0045004: /* SNDCTL_DSP_GETBLKSIZE */
        return wr_int(m, arg, (int32_t)s->fragsize);
    case 0xC0045005: /* SNDCTL_DSP_SETFMT */
        if ((r = rd_int(m, arg, &v)) < 0) return r;
        if (v == 0x10) s->fmt = 16;          /* AFMT_S16_LE */
        else if (v == 0x8) s->fmt = 8;       /* AFMT_U8 */
        else if (v == 0) { /* query */ }
        else s->fmt = 16;
        return wr_int(m, arg, s->fmt == 16 ? 0x10 : 0x8);
    case 0x8004500B: /* SNDCTL_DSP_GETFMTS */
        return wr_int(m, arg, 0x10 | 0x8);
    case 0xC004500A: { /* SNDCTL_DSP_SETFRAGMENT */
        if ((r = rd_int(m, arg, &v)) < 0) return r;
        uint32_t count = ((uint32_t)v >> 16) & 0xFFFF, shift = (uint32_t)v & 0xFFFF;
        if (shift >= 4 && shift <= 16)
            s->fragsize = 1u << shift;
        if (count >= 2 && count <= 0x7FFF)
            s->nfrags = count;
        while ((size_t)s->fragsize * s->nfrags > DSP_RING_MAX)
            s->nfrags /= 2;
        gp_trace(GP_TRACE_DEV, "dsp: fragments %u x %u", s->nfrags, s->fragsize);
        return 0;
    }
    case 0x8010500C: { /* SNDCTL_DSP_GETOSPACE */
        gpdev_tick(d);
        size_t cap = dsp_capacity(s);
        uint8_t info[16];
        put32(info + 0, (uint32_t)((cap - dsp_fill(s)) / s->fragsize)); /* fragments */
        put32(info + 4, s->nfrags);                                /* fragstotal */
        put32(info + 8, s->fragsize);                              /* fragsize */
        put32(info + 12, (uint32_t)(cap - dsp_fill(s)));           /* bytes */
        return gmem_write(m, arg, info, sizeof info) == GP_OK ? 0 : -EFAULT;
    }
    case 0x8010500D: { /* SNDCTL_DSP_GETISPACE */
        uint8_t info[16];
        put32(info + 0, 0); put32(info + 4, s->nfrags); put32(info + 8, s->fragsize); put32(info + 12, 0);
        return gmem_write(m, arg, info, sizeof info) == GP_OK ? 0 : -EFAULT;
    }
    case 0x8004500F: /* SNDCTL_DSP_GETCAPS */
        return wr_int(m, arg, 0x0400 /* DSP_CAP_TRIGGER */ | 0x0100 /* DSP_CAP_REALTIME */);
    case 0x80045010: /* SNDCTL_DSP_GETTRIGGER */
        return wr_int(m, arg, 2 /* PCM_ENABLE_OUTPUT */);
    case 0x40045010: /* SNDCTL_DSP_SETTRIGGER */
        return 0;
    case 0x80045017: /* SNDCTL_DSP_GETODELAY */
        return wr_int(m, arg, (int32_t)dsp_fill(s));
    case 0x500E: /* SNDCTL_DSP_NONBLOCK */
        f->gflags |= GUEST_O_NONBLOCK;
        return 0;
    default:
        gp_trace(GP_TRACE_DEV, "dsp: unsupported ioctl %08x", req);
        return -EINVAL;
    }
}

static int dsp_poll(fs_file_t *f, int events)
{
    gpdev_t *d = f->priv;
    gpdev_tick(d);
    int rev = GUEST_POLLIN;
    if (dsp_capacity(&d->dsp) - dsp_fill(&d->dsp) >= d->dsp.fragsize)
        rev |= GUEST_POLLOUT;
    return rev & events;
}

static void dsp_release(fs_file_t *f)
{
    gpdev_t *d = f->priv;
    gpdev_tick(d);
    d->dsp.open = false;
    gp_trace(GP_TRACE_DEV, "dsp: closed (%zu bytes discarded, %llu underruns)", dsp_fill(&d->dsp),
             (unsigned long long)d->dsp.underruns);
    __atomic_store_n(&d->dsp.rpos, __atomic_load_n(&d->dsp.wpos, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
}

static const fs_file_ops_t dsp_ops = { .read = dsp_read, .write = dsp_write, .fstat = dsp_fstat, .ioctl = dsp_ioctl, .poll = dsp_poll, .release = dsp_release };

static int dsp_open(void *ctx, int gflags, fs_file_t **out)
{
    gpdev_t *d = ctx;
    if (d->dsp.open)
        return -EBUSY;
    fs_file_t *f = fs_file_new(&dsp_ops, gflags);
    if (!f)
        return -ENOMEM;
    f->priv = d;
    f->seekable = false;
    dsp_reset(&d->dsp);
    d->dsp.open = true;
    d->dsp.opens++;
    d->dsp.last_drain_ns = gpdev_virtual_ns(d);
    gp_trace(GP_TRACE_DEV, "dsp: open #%llu", (unsigned long long)d->dsp.opens);
    *out = f;
    return 0;
}

/* ---- /dev/mixer -------------------------------------------------------- */

static int mixer_fstat(fs_file_t *f, fs_stat_t *st) { return chr_fstat_common(f, st, MKDEV(14, 0)); }

static int mixer_ioctl(fs_file_t *f, uint32_t req, gmem_t *m, gaddr_t arg)
{
    gpdev_t *d = f->priv;
    int32_t v;
    int r;
    switch (req) {
    case 0xC0044D04: /* SOUND_MIXER_WRITE_PCM */
        if ((r = rd_int(m, arg, &v)) < 0) return r;
        d->mixer_pcm = (uint32_t)v & 0xFFFF;
        gp_trace(GP_TRACE_DEV, "mixer: pcm %u/%u", d->mixer_pcm & 0xFF, d->mixer_pcm >> 8);
        return wr_int(m, arg, (int32_t)d->mixer_pcm);
    case 0x80044D04: /* SOUND_MIXER_READ_PCM */
        return wr_int(m, arg, (int32_t)d->mixer_pcm);
    case 0xC0044D00: /* SOUND_MIXER_WRITE_VOLUME */
        if ((r = rd_int(m, arg, &v)) < 0) return r;
        d->mixer_volume = (uint32_t)v & 0xFFFF;
        return wr_int(m, arg, (int32_t)d->mixer_volume);
    case 0x80044D00: /* SOUND_MIXER_READ_VOLUME */
        return wr_int(m, arg, (int32_t)d->mixer_volume);
    case 0x80044DFE: /* SOUND_MIXER_READ_DEVMASK */
    case 0x80044DFD: /* SOUND_MIXER_READ_STEREODEVS */
        return wr_int(m, arg, (1 << 0) | (1 << 4)); /* VOLUME, PCM */
    case 0x80044DFC: /* SOUND_MIXER_READ_RECMASK */
    case 0x80044DFF: /* SOUND_MIXER_READ_RECSRC */
        return wr_int(m, arg, 0);
    case 0x80044DFB: /* SOUND_MIXER_READ_CAPS */
        return wr_int(m, arg, 0);
    default:
        return -EINVAL;
    }
}

static const fs_file_ops_t mixer_ops = { .fstat = mixer_fstat, .ioctl = mixer_ioctl };

static int mixer_open(void *ctx, int gflags, fs_file_t **out)
{
    fs_file_t *f = fs_file_new(&mixer_ops, gflags);
    if (!f)
        return -ENOMEM;
    f->priv = ctx;
    f->seekable = false;
    *out = f;
    return 0;
}

/* ---- /dev/mmuhack, /dev/GPIO, /dev/batt, /dev/cx25874 ------------------ */

static int mmuhack_fstat(fs_file_t *f, fs_stat_t *st) { return chr_fstat_common(f, st, MKDEV(250, 0)); }
static const fs_file_ops_t mmuhack_ops = { .fstat = mmuhack_fstat };

static int mmuhack_open(void *ctx, int gflags, fs_file_t **out)
{
    (void)ctx;
    fs_file_t *f = fs_file_new(&mmuhack_ops, gflags);
    if (!f)
        return -ENOMEM;
    f->seekable = false;
    gp_trace(GP_TRACE_DEV, "mmuhack: opened");
    *out = f;
    return 0;
}

/* /dev/GPIO: a 4-byte read returns the button state in the layout the
 * firmware's SDL joystick driver decodes (bit i = button i of its GP2X
 * enumeration: 0 UP, 1 UPLEFT, 2 LEFT, 3 DOWNLEFT, 4 DOWN, 5 DOWNRIGHT,
 * 6 RIGHT, 7 UPRIGHT, 8 START, 9 SELECT, 10 L, 11 R, 12 A, 13 B, 14 X,
 * 15 Y, 16 VOL+, 17 VOL-, 18 stick push), active high (OPEN-7). */
static int64_t gpio_read(fs_file_t *f, int64_t off, void *buf, size_t n)
{
    (void)off;
    gpdev_t *d = f->priv;
    uint32_t v = (d->pad & 0xFFFF) | ((d->pad >> 23) & 1) << 16 | ((d->pad >> 22) & 1) << 17 | ((d->pad >> 27) & 1) << 18;
    if (n > 4)
        n = 4;
    memcpy(buf, &v, n);
    return (int64_t)n;
}
static int gpio_fstat(fs_file_t *f, fs_stat_t *st) { return chr_fstat_common(f, st, MKDEV(253, 0)); }
static const fs_file_ops_t gpio_ops = { .read = gpio_read, .fstat = gpio_fstat };

static int gpio_open(void *ctx, int gflags, fs_file_t **out)
{
    fs_file_t *f = fs_file_new(&gpio_ops, gflags);
    if (!f)
        return -ENOMEM;
    f->priv = ctx;
    f->seekable = false;
    *out = f;
    return 0;
}

/* /dev/batt: ioctl 0x80047600 (arg 1) then reads; reports a full battery (OPEN-8). */
static int64_t batt_read(fs_file_t *f, int64_t off, void *buf, size_t n)
{
    (void)f; (void)off;
    uint32_t v = 0xFF; /* full */
    if (n > 4) n = 4;
    memcpy(buf, &v, n);
    return (int64_t)n;
}
static int batt_ioctl(fs_file_t *f, uint32_t req, gmem_t *m, gaddr_t arg)
{
    (void)f; (void)m; (void)arg;
    return req == 0x80047600 ? 0 : -EINVAL;
}
static int batt_fstat(fs_file_t *f, fs_stat_t *st) { return chr_fstat_common(f, st, MKDEV(10, 240)); }
static const fs_file_ops_t batt_ops = { .read = batt_read, .ioctl = batt_ioctl, .fstat = batt_fstat };

static int batt_open(void *ctx, int gflags, fs_file_t **out)
{
    (void)ctx;
    fs_file_t *f = fs_file_new(&batt_ops, gflags);
    if (!f)
        return -ENOMEM;
    f->seekable = false;
    *out = f;
    return 0;
}

/* /dev/cx25874: TV-out encoder. Reports no TV. */
static int cx_ioctl(fs_file_t *f, uint32_t req, gmem_t *m, gaddr_t arg) { (void)f; (void)req; (void)m; (void)arg; return 0; }
static int cx_fstat(fs_file_t *f, fs_stat_t *st) { return chr_fstat_common(f, st, MKDEV(10, 241)); }
static const fs_file_ops_t cx_ops = { .ioctl = cx_ioctl, .fstat = cx_fstat };

static int cx_open(void *ctx, int gflags, fs_file_t **out)
{
    (void)ctx;
    fs_file_t *f = fs_file_new(&cx_ops, gflags);
    if (!f)
        return -ENOMEM;
    f->seekable = false;
    *out = f;
    return 0;
}

/* Block-device placeholders for the menu's mount/umount commands. */
static int blk_open(void *ctx, int gflags, fs_file_t **out) { (void)ctx; (void)gflags; (void)out; return -EACCES; }

int gpdev_register_nodes(gpdev_t *d, fs_backend_t *devfs)
{
    int r = 0;
    r |= fs_devfs_register(devfs, "mem", S_IFCHR | 0640, MKDEV(1, 1), mem_open, d);
    r |= fs_devfs_register(devfs, "fb0", S_IFCHR | 0660, MKDEV(29, 0), fb0_open, d);
    r |= fs_devfs_register(devfs, "fb1", S_IFCHR | 0660, MKDEV(29, 1), fb1_open, d);
    r |= fs_devfs_register(devfs, "fb/0", S_IFCHR | 0660, MKDEV(29, 0), fb0_open, d);
    r |= fs_devfs_register(devfs, "fb/1", S_IFCHR | 0660, MKDEV(29, 1), fb1_open, d);
    r |= fs_devfs_register(devfs, "dsp", S_IFCHR | 0660, MKDEV(14, 3), dsp_open, d);
    r |= fs_devfs_register(devfs, "sound/dsp", S_IFCHR | 0660, MKDEV(14, 3), dsp_open, d);
    r |= fs_devfs_register(devfs, "mixer", S_IFCHR | 0660, MKDEV(14, 0), mixer_open, d);
    r |= fs_devfs_register(devfs, "sound/mixer", S_IFCHR | 0660, MKDEV(14, 0), mixer_open, d);
    r |= fs_devfs_register(devfs, "mmuhack", S_IFCHR | 0660, MKDEV(250, 0), mmuhack_open, d);
    r |= fs_devfs_register(devfs, "GPIO", S_IFCHR | 0660, MKDEV(253, 0), gpio_open, d);
    r |= fs_devfs_register(devfs, "batt", S_IFCHR | 0660, MKDEV(10, 240), batt_open, d);
    r |= fs_devfs_register(devfs, "misc/batt", S_IFCHR | 0660, MKDEV(10, 240), batt_open, d);
    r |= fs_devfs_register(devfs, "cx25874", S_IFCHR | 0660, MKDEV(10, 241), cx_open, d);
    r |= fs_devfs_register(devfs, "misc/cx25874", S_IFCHR | 0660, MKDEV(10, 241), cx_open, d);
    r |= fs_devfs_register(devfs, "loop/7", S_IFBLK | 0660, MKDEV(7, 7), blk_open, d);
    r |= fs_devfs_register(devfs, "mmcsd/disc0/part1", S_IFBLK | 0660, MKDEV(241, 1), blk_open, d);
    r |= fs_devfs_register(devfs, "mmcsd/disc0/disc", S_IFBLK | 0660, MKDEV(241, 0), blk_open, d);
    r |= fs_devfs_register(devfs, "mtdblock/3", S_IFBLK | 0660, MKDEV(31, 3), blk_open, d);
    r |= fs_devfs_register(devfs, "mtdblock/4", S_IFBLK | 0660, MKDEV(31, 4), blk_open, d);
    return r ? -ENOMEM : 0;
}
