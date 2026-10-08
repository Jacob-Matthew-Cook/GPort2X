/* dev module tests (spec 3, 4, 8.2): exact values for the physical model,
 * the register behaviours, the device ioctls, the OSS buffering, the flip
 * event and frame dumps, and the pad script. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gport2x/dev.h"
#include "gport2x/log.h"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static fs_t *fs;
static fs_ctx_t ctx;
static gmem_t *m;
#define ARG 0x50000000u /* a guest page for ioctl arguments */

static fs_file_t *openp(const char *path, int flags)
{
    fs_file_t *f = NULL;
    int r = fs_open(fs, &ctx, path, flags, 0, &f);
    if (r < 0)
        printf("open %s: %s\n", path, strerror(-r));
    return f;
}

static uint32_t g32(gaddr_t a) { uint32_t v = 0; gmem_ld32(m, a, &v); return v; }
static uint32_t g16(gaddr_t a) { uint32_t v = 0; gmem_ld16(m, a, &v); return v; }

int main(void)
{
    gp_log_init_from_env();
    char tmp[256];
    snprintf(tmp, sizeof tmp, "/tmp/gport2x-dev-XXXXXX");
    if (!mkdtemp(tmp)) return 2;
    char dump[300], wav[300];
    snprintf(dump, sizeof dump, "%s/frames", tmp);
    mkdir(dump, 0755);
    snprintf(wav, sizeof wav, "%s/out.wav", tmp);

    gpdev_config_t cfg;
    gpdev_config_default(&cfg);
    cfg.clock_mode = GPDEV_CLOCK_MAINSTEP;
    cfg.dump_dir = dump;
    cfg.wav_path = wav;
    cfg.pad_script = "3:DOWN,5:-,7:B+START,9:-";
    gpdev_t *d = gpdev_create(&cfg);
    CHECK(d, "create");
    fs = fs_create();
    fs_backend_t *devfs = fs_backend_devfs();
    CHECK(fs_mount(fs, "/", devfs, "none", "devfs", "rw") == 0, "mount devfs as root for the test");
    CHECK(gpdev_register_nodes(d, devfs) == 0, "register nodes");
    fs_ctx_init(&ctx, 1, "/");
    m = gmem_create();
    gmem_map_anon(m, ARG, GP2X_PAGE_SIZE, GMEM_PROT_RW);

    /* /dev/mem mappings: the offsets the game uses (spec 3.2) */
    fs_file_t *mem = openp("/mem", GUEST_O_RDWR);
    CHECK(mem, "open /dev/mem");
    gmem_obj_t *obj; uint32_t off;
    CHECK(fs_file_mmap(mem, 0x03DA4000, 0x4000, GMEM_PROT_RW, true, &obj, &off) == 0 && obj == gpdev_upper_bank(d) && off == 0x01DA4000, "window 1");
    gmem_map_obj(m, 0x40801000, 0x4000, GMEM_PROT_RW, obj, off); gmem_obj_release(obj);
    for (int k = 0; k < 8; k++) {
        CHECK(fs_file_mmap(mem, 0x03DA8000 + k * 0x4B000u, 0x4B000, GMEM_PROT_RW, true, &obj, &off) == 0 && off == 0x01DA8000 + k * 0x4B000u, "video window %d", k);
        gmem_obj_release(obj);
    }
    CHECK(fs_file_mmap(mem, 0x02000000, 0x02000000, GMEM_PROT_RW, true, &obj, &off) == 0 && off == 0, "whole bank");
    gmem_map_obj(m, 0x41000000, 0x02000000, GMEM_PROT_RW, obj, 0); gmem_obj_release(obj);
    CHECK(fs_file_mmap(mem, 0xC0000000, 0x10000, GMEM_PROT_RW, true, &obj, &off) == 0 && obj == gpdev_regs(d) && off == 0, "registers");
    gmem_map_obj(m, 0x43000000, 0x10000, GMEM_PROT_RW, obj, 0); gmem_obj_release(obj);
    CHECK(fs_file_mmap(mem, 0x01FFF000, 0x2000, GMEM_PROT_RW, true, &obj, &off) == -EINVAL, "below the bank refused");
    CHECK(fs_file_mmap(mem, 0x03FFF000, 0x2000, GMEM_PROT_RW, true, &obj, &off) == -EINVAL, "past the bank refused");
    CHECK(fs_file_mmap(mem, 0xC0000000, 0x20000, GMEM_PROT_RW, true, &obj, &off) == -EINVAL, "over-long register map refused");
    CHECK(fs_file_mmap(mem, 0x00000000, 0x1000, GMEM_PROT_RW, true, &obj, &off) == -EINVAL, "lower bank refused");
    /* aliasing: a write through the whole-bank view is seen through window 1 */
    gmem_st32(m, 0x41000000 + 0x01DA4000 + 0x10, 0xCAFEBABE);
    CHECK(g32(0x40801010) == 0xCAFEBABE, "aliases are coherent");
    /* pread/pwrite of /dev/mem */
    uint8_t b4[4] = { 1, 2, 3, 4 }, r4[4];
    CHECK(fs_file_pwrite(mem, 0x03000000, b4, 4) == 4 && fs_file_pread(mem, 0x03000000, r4, 4) == 4 && !memcmp(b4, r4, 4), "mem pread/pwrite");
    CHECK(fs_file_pread(mem, 0x00001000, r4, 4) == -EFAULT, "mem read outside the model");

    /* TCOUNT: +7680 per 32-bit read by the main thread; other threads peek */
    uint32_t t1 = g32(0x43000A00), t2 = g32(0x43000A00);
    CHECK(t1 == 7680 && t2 == 15360, "TCOUNT step: %u %u", t1, t2);
    gpdev_set_current_thread(d, false);
    uint32_t t3 = g32(0x43000A00);
    gpdev_set_current_thread(d, true);
    uint32_t t4 = g32(0x43000A00);
    CHECK(t3 == 15360 && t4 == 23040, "MAINSTEP: other threads see the value without advancing (%u %u)", t3, t4);
    CHECK(gpdev_tcount_peek(d) == 23040 && gpdev_virtual_ns(d) == 23040ull * 1000000000ull / 7372800ull, "peek and virtual time");
    /* GPIO: released = 1; a mask maps to the three words, active low */
    CHECK(g16(0x43001198) == 0xFFFF && g16(0x43001184) == 0xFFFF && g16(0x43001186) == 0xFFFF, "released pad reads all ones");
    gpdev_set_pad_mask(d, (1u << 4) | (1u << 13) | (1u << 23) | (1u << 27)); /* DOWN, B, VOL+, PUSH */
    CHECK(g16(0x43001198) == 0xFFEF, "GPIOM %04x", g16(0x43001198));
    CHECK(g16(0x43001184) == 0xDFFF, "GPIOC %04x", g16(0x43001184));
    CHECK(g16(0x43001186) == 0xF77F, "GPIOD %04x", g16(0x43001186));
    gpdev_set_pad_mask(d, 0);
    /* CLKCHGSTREG bit 0 reads 0 whatever was written; DPC_CNTL bit 8 is 0 */
    gmem_st16(m, 0x43000902, 0xFFFF);
    CHECK((g16(0x43000902) & 1) == 0 && g16(0x43000902) == 0xFFFE, "CLKCHGSTREG bit 0 = 0 (%04x)", g16(0x43000902));
    CHECK((g16(0x43002800) & 0x100) == 0, "DPC_CNTL bit 8 = 0 (LCD)");
    /* latched memory: MLC scanout pair, 0x0808 32-bit, 0x0910 */
    gmem_st16(m, 0x4300290e, 0x1000); gmem_st16(m, 0x43002910, 0x0310);
    CHECK(g16(0x4300290e) == 0x1000 && g16(0x43002910) == 0x0310, "scanout registers latch");
    gmem_st32(m, 0x43000808, 0xFF8FFFE7);
    CHECK(g32(0x43000808) == 0xFF8FFFE7, "0x0808 latches 32-bit");
    gmem_st16(m, 0x43000910, 0x4A04);
    CHECK(g16(0x43000910) == 0x4A04, "FPLL set register latches");
    gmem_st8(m, 0x43002881, 0x12);
    CHECK(g16(0x43002880) == 0x1200, "byte write into a halfword");

    /* framebuffers */
    fs_file_t *fb0 = openp("/fb0", GUEST_O_RDWR), *fb1 = openp("/fb/1", GUEST_O_RDWR);
    CHECK(fb0 && fb1, "open fbs");
    CHECK(fs_file_ioctl(fb0, 0x4602, m, ARG) == 0 && g32(ARG + 16) == 0x03101000 && g32(ARG + 20) == 0x25800 && g32(ARG + 44) == 640 && g32(ARG + 32) == 2, "fb0 FSCREENINFO smem_start %08x len %x", g32(ARG + 16), g32(ARG + 20));
    CHECK(fs_file_ioctl(fb1, 0x4602, m, ARG) == 0 && g32(ARG + 16) == 0x03381000, "fb1 smem_start %08x", g32(ARG + 16));
    CHECK(fs_file_ioctl(fb0, 0x4600, m, ARG) == 0 && g32(ARG) == 320 && g32(ARG + 4) == 240 && g32(ARG + 24) == 16 && g32(ARG + 32) == 11 && g32(ARG + 36) == 5 && g32(ARG + 44) == 5 && g32(ARG + 48) == 6, "VSCREENINFO 320x240 RGB565");
    CHECK(fs_file_ioctl(fb0, 0x4700, m, ARG) == -ENOTTY, "unknown fb ioctl");
    CHECK(fs_file_mmap(fb0, 0, 0x25800, GMEM_PROT_W, true, &obj, &off) == 0 && obj == gpdev_upper_bank(d) && off == 0x01101000, "fb0 map aliases the bank");
    gmem_map_obj(m, 0x44000000, 0x26000, GMEM_PROT_RW, obj, off); gmem_obj_release(obj);
    CHECK(fs_file_mmap(fb1, 0, 0x25800, GMEM_PROT_W, true, &obj, &off) == 0 && off == 0x01381000, "fb1 map");
    gmem_obj_release(obj);
    CHECK(fs_file_mmap(fb0, 0, 0x27000, GMEM_PROT_W, true, &obj, &off) == -EINVAL, "fb over-length map");
    gmem_st16(m, 0x44000000, 0xF800);
    CHECK(g16(0x41000000 + 0x01101000) == 0xF800, "fb0 pixel visible through the bank window");
    uint8_t px[2];
    CHECK(fs_file_pread(fb0, 0, px, 2) == 2 && px[0] == 0 && px[1] == 0xF8, "fb0 read");

    /* a flip: the scanout address written low then high, after the frame clock started */
    CHECK(!gpdev_cacheflush(d, 0x00008000, 0x00008040), "a code-range cacheflush is not a page flush");
    CHECK(!gpdev_cacheflush(d, 0x03101000, 0x03101000 + 0x25000), "wrong length is not a page flush");
    CHECK(gpdev_cacheflush(d, 0x03101000, 0x03101000 + 0x25800) && gpdev_flip_count(d) == 0, "a page flush alone is not a flip");
    { gpdev_t *d2; gpdev_config_t c2; gpdev_config_default(&c2); c2.clock_mode = GPDEV_CLOCK_STEP; d2 = gpdev_create(&c2);
      gmem_t *m2 = gmem_create(); gmem_map_obj(m2, 0x43000000, 0x10000, GMEM_PROT_RW, gpdev_regs(d2), 0);
      gmem_st16(m2, 0x43002912, 0x1000); gmem_st16(m2, 0x43002914, 0x0310);
      CHECK(gpdev_flip_count(d2) == 0, "scanout writes before the first TCOUNT read are init, not flips");
      uint32_t v; gmem_ld32(m2, 0x43000A00, &v);
      gmem_st16(m2, 0x43002912, 0x1000); gmem_st16(m2, 0x43002914, 0x0310);
      CHECK(gpdev_flip_count(d2) == 1, "scanout write after the clock started is a flip");
      gmem_destroy(m2); gpdev_destroy(d2); }
    #define FLIP(page) do { gmem_st16(m, 0x43002912, (page) & 0xFFFF); gmem_st16(m, 0x43002914, (page) >> 16); } while (0)
    FLIP(0x03101000);
    CHECK(gpdev_flip_count(d) == 1, "flip detected");
    char fpath[400];
    snprintf(fpath, sizeof fpath, "%s/frame_00000.ppm", dump);
    FILE *pf = fopen(fpath, "rb");
    CHECK(pf, "frame dumped");
    if (pf) {
        uint8_t hdr[15], rgb[3];
        CHECK(fread(hdr, 1, 15, pf) == 15 && !memcmp(hdr, "P6\n320 240\n255\n", 15), "ppm header");
        CHECK(fread(rgb, 1, 3, pf) == 3 && rgb[0] == 255 && rgb[1] == 0 && rgb[2] == 0, "first pixel red (%u %u %u)", rgb[0], rgb[1], rgb[2]);
        fseek(pf, 0, SEEK_END);
        CHECK(ftell(pf) == 15 + 320 * 240 * 3, "ppm size");
        fclose(pf);
    }
    /* pad script: entries apply when the flip count passes their frame */
    CHECK(gpdev_pad_mask(d) == 0, "no pad yet");
    FLIP(0x03381000); FLIP(0x03101000);
    CHECK(gpdev_flip_count(d) == 3 && gpdev_pad_mask(d) == (1u << 4), "DOWN at flip 3 (%08x)", gpdev_pad_mask(d));
    FLIP(0x03101000); FLIP(0x03101000);
    CHECK(gpdev_pad_mask(d) == 0, "released at 5");
    FLIP(0x03101000); FLIP(0x03101000);
    CHECK(gpdev_pad_mask(d) == ((1u << 13) | (1u << 8)), "B+START at 7");
    CHECK(gpdev_pad_bit("voldn", 5) == 22 && gpdev_pad_bit("Q", 1) == -1, "pad names");

    /* OSS: the game's init sequence, then buffering */
    fs_file_t *dsp = openp("/dsp", GUEST_O_WRONLY);
    CHECK(dsp && gpdev_dsp_open_count(d) == 1, "open dsp");
    CHECK(openp("/sound/dsp", GUEST_O_WRONLY) == NULL, "second open is EBUSY");
    gmem_st32(m, ARG, 44100); CHECK(fs_file_ioctl(dsp, 0xC0045002, m, ARG) == 0 && g32(ARG) == 44100, "SPEED");
    gmem_st32(m, ARG, 16); CHECK(fs_file_ioctl(dsp, 0xC0045005, m, ARG) == 0 && g32(ARG) == 16, "SETFMT S16");
    gmem_st32(m, ARG, 1); CHECK(fs_file_ioctl(dsp, 0xC0045003, m, ARG) == 0 && g32(ARG) == 1, "STEREO");
    gmem_st32(m, ARG, 0x00080009); CHECK(fs_file_ioctl(dsp, 0xC004500A, m, ARG) == 0, "SETFRAGMENT");
    CHECK(fs_file_ioctl(dsp, 0x8010500C, m, ARG) == 0 && g32(ARG) == 8 && g32(ARG + 4) == 8 && g32(ARG + 8) == 512 && g32(ARG + 12) == 4096, "GETOSPACE %u %u %u %u", g32(ARG), g32(ARG + 4), g32(ARG + 8), g32(ARG + 12));
    static int16_t period[256 * 2];
    for (int i = 0; i < 512; i++) period[i] = (int16_t)(i * 100);
    for (int i = 0; i < 4; i++)
        CHECK(fs_file_write(dsp, period, 1024) == 1024, "write period %d", i);
    CHECK(fs_file_write(dsp, period, 1024) == -EAGAIN, "buffer full: would block");
    CHECK(fs_file_ioctl(dsp, 0x8010500C, m, ARG) == 0 && g32(ARG) == 0 && g32(ARG + 12) == 0, "no space left");
    CHECK(fs_file_poll(dsp, GUEST_POLLOUT) == 0, "poll: not writable");
    /* 2 TCOUNT reads = 2.083 ms of virtual time = 367 bytes drained at 176,400 B/s */
    g32(0x43000A00); g32(0x43000A00);
    gpdev_tick(d);
    CHECK(fs_file_ioctl(dsp, 0x8010500C, m, ARG) == 0 && g32(ARG + 12) == 367, "drained %u bytes", g32(ARG + 12));
    CHECK(fs_file_write(dsp, period, 1024) == -EAGAIN, "still no room for a full write");
    for (int i = 0; i < 30; i++) g32(0x43000A00);
    gpdev_tick(d);
    CHECK(fs_file_write(dsp, period, 1024) == 1024, "room after more time");
    CHECK(gpdev_audio_bytes_out(d) > 0, "audio emitted");
    /* mixer */
    fs_file_t *mix = openp("/mixer", GUEST_O_RDWR);
    gmem_st32(m, ARG, 0); CHECK(mix && fs_file_ioctl(mix, 0xC0044D04, m, ARG) == 0 && gpdev_mixer_pcm(d) == 0, "mute");
    gmem_st32(m, ARG, 50 | 50 << 8); CHECK(fs_file_ioctl(mix, 0xC0044D04, m, ARG) == 0 && gpdev_mixer_pcm(d) == 0x3232, "WRITE_PCM 50/50");
    CHECK(fs_file_ioctl(mix, 0xC0044D07, m, ARG) == -EINVAL, "unsupported mixer channel");
    CHECK(fs_file_ioctl(mix, 0x80044D04, m, ARG) == 0 && g32(ARG) == 0x3232, "READ_PCM");
    /* reopen path: close, open again */
    fs_file_unref(dsp);
    dsp = openp("/dsp", GUEST_O_WRONLY);
    CHECK(dsp && gpdev_dsp_open_count(d) == 2, "reopen counted");
    fs_file_unref(dsp);
    /* mmuhack */
    fs_file_t *mh = openp("/mmuhack", GUEST_O_RDWR);
    CHECK(mh != NULL, "mmuhack opens"); if (mh) fs_file_unref(mh);
    /* GPIO read */
    gpdev_set_pad_mask(d, 0x100);
    fs_file_t *gp = openp("/GPIO", GUEST_O_RDWR | GUEST_O_NONBLOCK);
    uint32_t gv = 0;
    CHECK(gp && fs_file_read(gp, &gv, 4) == 4 && gv == 0x100, "GPIO read");
    fs_file_unref(gp);
    fs_stat_t st;
    CHECK(fs_stat(fs, &ctx, "/mmcsd/disc0/part1", true, &st) == 0 && S_ISBLK(st.mode), "block node present");

    fs_file_unref(fb0); fs_file_unref(fb1); fs_file_unref(mem); fs_file_unref(mix);
    gpdev_destroy(d);
    struct stat ws;
    CHECK(stat(wav, &ws) == 0 && ws.st_size > 44, "wav written (%ld)", ws.st_size ? (long)ws.st_size : 0L);
    gmem_destroy(m);
    fs_destroy(fs);
    char cmd[400]; snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmp); if (system(cmd)) {}
    printf("%s: %d failure(s)\n", __FILE__, failures);
    return failures ? 1 : 0;
}
