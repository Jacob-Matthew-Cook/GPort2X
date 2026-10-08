/* gport2x: the command line.
 *
 *   gport2x [options] [--] PROGRAM [ARGS...]
 *
 * Builds the GP2X namespace (firmware rootfs, card image, tmpfs, /proc,
 * /dev), the devices and the process model, then runs PROGRAM (a guest
 * path) under the interpreter engine. See --help. */
#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gport2x/cpu.h"
#include "gport2x/native.h"
#include "gport2x/card.h"
#include "gport2x/dev.h"
#include "gport2x/fs.h"
#include "gport2x/log.h"
#include "gport2x/proc.h"
#include "sdl_host.h"

static void usage(FILE *out)
{
    fputs("usage: gport2x [options] [--] PROGRAM [ARGS...]\n"
          "  --firmware DIR    the firmware rootfs dump (mounted read-only at / with a scratch upper)\n"
          "  --card IMAGE      the card image (FAT16/32), mounted read-only at /mnt/sd with a saves upper\n"
          "  --card-dir DIR    a card directory instead of an image (non-genuine: statfs is the host's)\n"
          "  --saves DIR       where card writes go (default: a temporary directory, discarded)\n"
          "  --scratch DIR     host directory for scratch state (default: a temporary directory)\n"
          "  --inject HOST:GUEST  copy a host file into the guest namespace before starting (e.g. a game ELF)\n"
          "  --root DIR        a writable host directory as the guest root (tests; instead of --firmware)\n"
          "  --cwd PATH        the initial directory (default /)\n"
          "  --exit-at-menu    a program that returns to the firmware menu (an execve of /usr/gp2x/gp2xmenu)\n"
          "                    exits instead, so the run ends where the console shows its menu (for launchers)\n"
          "  --argv0 NAME      argv[0] for the program (default: the program path)\n"
          "  --env NAME=VALUE  add to the environment (repeatable; default: the rc.sysinit exports)\n"
          "  --clock MODE      real | step | mainstep (default mainstep in test mode, real otherwise)\n"
          "  --step N          raw TCOUNT counts per read in the step modes (default 7680 = 32 game ticks)\n"
          "  --time N          pin time() to N seconds (test mode default 0)\n"
          "  --test            deterministic test mode: --clock mainstep --time 0\n"
          "  --dump DIR        dump every flip as DIR/frame_%05d.ppm\n"
          "  --dump-every N    dump every Nth flip\n"
          "  --snapshot-ms N   also dump the displayed page every N ms of host time (DIR/snap_%05d.ppm)\n"
          "  --wav FILE        write the audio output to a WAV file\n"
          "  --pad SCRIPT      pad script \"FLIP:KEYS,FLIP:-,...\" (UP DOWN LEFT RIGHT START SELECT L R A B X Y VOLUP VOLDN PUSH)\n"
          "  --pad-time SCRIPT pad script keyed to host milliseconds \"MS:KEYS,MS:-,...\" (for the firmware menu)\n"
          "  --state-flip N    dump guest memory [LO,HI) at flip N to --state-dump FILE (lockstep statediff format)\n"
          "  --state-dump FILE  (default state.bin); --state-range LO:HI (default 0x9f1000:0xf23440, .data/.bss)\n"
          "  --flips N         stop after N flips\n"
          "  --insns N         stop after N instructions\n"
          "  --no-jit          interpret every instruction (also GPORT2X_JIT=0)\n"
          "  --capture A[,A..] record calls of the functions at these addresses (JSON lines; JIT off)\n"
          "  --capture-out F   where to write them (default capture.jsonl)\n"
          "  --capture-exe S   only in programs whose path contains S\n"
          "  --capture-max N   calls recorded per address (default 3); --capture-skip N skips the first N\n"
          "  --capture-budget N  instructions before a call is written as incomplete (default 200M)\n"
          "  --fullscreen      the SDL front end on the whole display (implies --sdl)\n"
          "  --engine E        interp (default) or native (armhf build on an AArch64 kernel, docs/NATIVE_ENGINE.md)\n"
          "  --flip-log FILE   per flip: guest instructions since the previous flip and host ns\n"
          "  --quantum N       instructions per scheduling slice\n"
          "  --trace LIST      syscall,mmio,cpu,fs,unaligned,dev (also GPORT2X_TRACE)\n"
          "  --log LEVEL       error|warn|info|debug (also GPORT2X_LOG)\n"
          "  --sdl [SCALE]     open a window (default 3x), keyboard as the pad, audio out; implies --clock real\n"
          "  --no-audio        with --sdl: no host audio\n"
          "  --keep-scratch    do not delete the temporary scratch directory\n",
          out);
}

typedef struct opts {
    const char *capture, *capture_out, *capture_exe;
    unsigned capture_max, capture_skip;
    uint64_t capture_budget;
    const char *firmware, *card, *card_dir, *saves, *scratch, *cwd, *clock, *dump, *wav, *pad, *pad_time, *trace, *log, *root, *argv0, *state_dump, *flip_log;
    int64_t state_flip;
    uint32_t state_lo, state_hi;
    const char *inject[16];
    int ninject;
    const char *env[64];
    int nenv;
    uint32_t step;
    int64_t time_value;
    bool time_set, test, keep_scratch, sdl, no_audio, native, fullscreen, exit_at_menu;
    int sdl_scale;
    int dump_every, snapshot_ms;
    uint64_t flips, insns, quantum;
} opts_t;

/* The native engine's front end, created on its own thread. */
typedef struct native_fe { gpdev_t *dev; gsys_t *sys; int scale; bool audio, fullscreen, sdl; } native_fe_t;
static void native_frontend(void *ctx)
{
    native_fe_t *f = ctx;
    if (f->sdl && !sdl_host_create(f->dev, f->sys, f->scale, f->audio, f->fullscreen))
        gp_error("native: no SDL front end");
}

static int remove_entry(const char *path, const struct stat *sb, int flag, struct FTW *ftw)
{
    (void)sb;
    (void)flag;
    (void)ftw;
    if (remove(path) != 0)
        gp_warn("could not remove %s: %s", path, strerror(errno));
    return 0;
}

/* rm -rf without a shell (natively a host shell would inherit the guest's
 * syscall filter). */
static void remove_tree(const char *dir)
{
    nftw(dir, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
}

/* The native engine's end: the same report as the interpreter's, and the
 * temporary scratch directory removed. */
typedef struct native_end { gpdev_t *dev; char scratch[FS_PATH_MAX]; } native_end_t; /* scratch "" = keep */
static void native_end(void *ctx, const char *why, int status)
{
    native_end_t *e = ctx;
    gp_info("stopped: %s; %llu flips, status %#x", why, (unsigned long long)gpdev_flip_count(e->dev), status);
    gp_info("audio: %llu bytes to the DAC, %llu underruns", (unsigned long long)gpdev_audio_bytes_out(e->dev),
            (unsigned long long)gpdev_dsp_underruns(e->dev));
    if (e->scratch[0])
        remove_tree(e->scratch);
}

static int copy_host_file(const char *host, fs_t *fs, const char *guest)
{
    FILE *in = fopen(host, "rb");
    if (!in) {
        fprintf(stderr, "gport2x: --inject %s: %s\n", host, strerror(errno));
        return -1;
    }
    fs_ctx_t ctx;
    fs_ctx_init(&ctx, 1, "/");
    /* create the parent directories */
    char dir[FS_PATH_MAX];
    snprintf(dir, sizeof dir, "%s", guest);
    for (char *p = dir + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            fs_mkdir(fs, &ctx, dir, 0755);
            *p = '/';
        }
    }
    fs_file_t *f;
    int r = fs_open(fs, &ctx, guest, GUEST_O_WRONLY | GUEST_O_CREAT | GUEST_O_TRUNC, 0755, &f);
    if (r < 0) {
        fprintf(stderr, "gport2x: --inject %s: guest %s: %s\n", host, guest, strerror(-r));
        fclose(in);
        return -1;
    }
    static uint8_t buf[1 << 16];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        int64_t w = fs_file_write(f, buf, n);
        if (w != (int64_t)n) {
            fprintf(stderr, "gport2x: --inject: short write to %s\n", guest);
            fs_file_unref(f);
            fclose(in);
            return -1;
        }
    }
    fs_file_unref(f);
    fclose(in);
    return 0;
}

/* --state-flip: writes guest memory [lo, hi) of the flipping task at the
 * given flip, the lockstep tooling's state.bin (dumped at the flip; the
 * oracle dumps at the flip routine's entry, a few words earlier). */
typedef struct state_dump {
    gsys_t *sys;
    int64_t flip;
    uint32_t lo, hi;
    const char *path;
    bool done;
} state_dump_t;

/* --flip-log FILE: one line per flip, "flip instructions host_ns": the guest
 * instructions retired (all threads) since the previous flip and the host
 * time they took, to find the heaviest frames (spec 1.6, the real-time budget). */
typedef struct flip_hooks {
    gsys_t *sys;
    struct state_dump *sd;
    FILE *log;
    uint64_t last_insns, last_ns;
} flip_hooks_t;

static void state_dump_hook(void *ctx, uint64_t flip, const uint16_t *rgb565);

static uint64_t host_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void flip_hooks(void *ctx, uint64_t flip, const uint16_t *rgb565)
{
    flip_hooks_t *fh = ctx;
    if (fh->log) {
        uint64_t n = gsys_insn_count_now(fh->sys), t = host_ns();
        fprintf(fh->log, "%llu %llu %llu\n", (unsigned long long)flip, (unsigned long long)(n - fh->last_insns),
                (unsigned long long)(fh->last_ns ? t - fh->last_ns : 0));
        fh->last_insns = n;
        fh->last_ns = t;
    }
    if (fh->sd)
        state_dump_hook(fh->sd, flip, rgb565);
}

static void state_dump_hook(void *ctx, uint64_t flip, const uint16_t *rgb565)
{
    (void)rgb565;
    state_dump_t *sd = ctx;
    if (sd->done || (int64_t)flip != sd->flip)
        return;
    sd->done = true;
    gtask_t *t = gsys_current_task(sd->sys);
    uint32_t len = sd->hi - sd->lo;
    uint8_t *buf = malloc(len);
    if (!t || !buf || gtask_read_mem(t, sd->lo, buf, len) < 0) {
        gp_error("state dump at flip %lld: cannot read %08x..%08x", (long long)flip, sd->lo, sd->hi);
        free(buf);
        return;
    }
    FILE *f = fopen(sd->path, "wb");
    if (f) {
        fwrite(buf, 1, len, f);
        fclose(f);
        gp_info("state dump at flip %lld: %u bytes of %08x..%08x -> %s", (long long)flip, len, sd->lo, sd->hi, sd->path);
    } else {
        gp_error("state dump: cannot write %s", sd->path);
    }
    free(buf);
}

static char *make_temp(const char *scratch, const char *name, bool keep)
{
    char *p = malloc(FS_PATH_MAX);
    snprintf(p, FS_PATH_MAX, "%s/%s-XXXXXX", scratch, name);
    if (!mkdtemp(p)) {
        fprintf(stderr, "gport2x: cannot create %s: %s\n", p, strerror(errno));
        exit(2);
    }
    (void)keep;
    return p;
}

int main(int argc, char **argv)
{
    opts_t o;
    memset(&o, 0, sizeof o);
    o.cwd = "/";
    o.state_flip = -1;
    o.state_lo = 0x9f1000;
    o.state_hi = 0xf23440;
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) { i++; break; }
        if (a[0] != '-') break;
#define NEXT() (i + 1 < argc ? argv[++i] : (usage(stderr), exit(2), (char *)0))
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(stdout); return 0; }
        else if (!strcmp(a, "--firmware")) o.firmware = NEXT();
        else if (!strcmp(a, "--card")) o.card = NEXT();
        else if (!strcmp(a, "--card-dir")) o.card_dir = NEXT();
        else if (!strcmp(a, "--saves")) o.saves = NEXT();
        else if (!strcmp(a, "--scratch")) o.scratch = NEXT();
        else if (!strcmp(a, "--inject")) { if (o.ninject < 16) o.inject[o.ninject++] = NEXT(); }
        else if (!strcmp(a, "--cwd")) o.cwd = NEXT();
        else if (!strcmp(a, "--exit-at-menu")) o.exit_at_menu = true;
        else if (!strcmp(a, "--root")) o.root = NEXT();
        else if (!strcmp(a, "--argv0")) o.argv0 = NEXT();
        else if (!strcmp(a, "--env")) { if (o.nenv < 63) o.env[o.nenv++] = NEXT(); }
        else if (!strcmp(a, "--clock")) o.clock = NEXT();
        else if (!strcmp(a, "--step")) o.step = (uint32_t)strtoul(NEXT(), NULL, 0);
        else if (!strcmp(a, "--time")) { o.time_value = strtoll(NEXT(), NULL, 0); o.time_set = true; }
        else if (!strcmp(a, "--test")) o.test = true;
        else if (!strcmp(a, "--dump")) o.dump = NEXT();
        else if (!strcmp(a, "--dump-every")) o.dump_every = atoi(NEXT());
        else if (!strcmp(a, "--snapshot-ms")) o.snapshot_ms = atoi(NEXT());
        else if (!strcmp(a, "--wav")) o.wav = NEXT();
        else if (!strcmp(a, "--pad")) o.pad = NEXT();
        else if (!strcmp(a, "--pad-time")) o.pad_time = NEXT();
        else if (!strcmp(a, "--flips")) o.flips = strtoull(NEXT(), NULL, 0);
        else if (!strcmp(a, "--state-flip")) o.state_flip = strtoll(NEXT(), NULL, 0);
        else if (!strcmp(a, "--state-dump")) o.state_dump = NEXT();
        else if (!strcmp(a, "--state-range")) { const char *v = NEXT(); o.state_lo = (uint32_t)strtoul(v, NULL, 0); const char *c2 = strchr(v, ':'); o.state_hi = c2 ? (uint32_t)strtoul(c2 + 1, NULL, 0) : 0; }
        else if (!strcmp(a, "--insns")) o.insns = strtoull(NEXT(), NULL, 0);
        else if (!strcmp(a, "--no-jit")) cpu_set_jit(false);
        else if (!strcmp(a, "--capture")) { o.capture = NEXT(); cpu_set_jit(false); }
        else if (!strcmp(a, "--capture-out")) o.capture_out = NEXT();
        else if (!strcmp(a, "--capture-exe")) o.capture_exe = NEXT();
        else if (!strcmp(a, "--capture-max")) o.capture_max = (unsigned)strtoul(NEXT(), NULL, 0);
        else if (!strcmp(a, "--capture-skip")) o.capture_skip = (unsigned)strtoul(NEXT(), NULL, 0);
        else if (!strcmp(a, "--capture-budget")) o.capture_budget = strtoull(NEXT(), NULL, 0);
        else if (!strcmp(a, "--fullscreen")) { o.fullscreen = true; o.sdl = true; }
        else if (!strcmp(a, "--engine")) { const char *v = NEXT(); if (!strcmp(v, "native")) o.native = true; else if (strcmp(v, "interp")) { fprintf(stderr, "gport2x: unknown engine %s\n", v); return 2; } }
        else if (!strcmp(a, "--flip-log")) o.flip_log = NEXT();
        else if (!strcmp(a, "--quantum")) o.quantum = strtoull(NEXT(), NULL, 0);
        else if (!strcmp(a, "--trace")) o.trace = NEXT();
        else if (!strcmp(a, "--log")) o.log = NEXT();
        else if (!strcmp(a, "--keep-scratch")) o.keep_scratch = true;
        else if (!strcmp(a, "--sdl")) { o.sdl = true; if (i + 1 < argc && argv[i + 1][0] >= '1' && argv[i + 1][0] <= '9' && !argv[i + 1][1]) o.sdl_scale = atoi(argv[++i]); }
        else if (!strcmp(a, "--no-audio")) o.no_audio = true;
        else { fprintf(stderr, "gport2x: unknown option %s\n", a); usage(stderr); return 2; }
#undef NEXT
    }
    if (i >= argc) {
        usage(stderr);
        return 2;
    }
    const char *program = argv[i];
    char **gargv = argv + i;

    gp_log_init_from_env();
    if (o.log) {
        const char *levels[] = { "error", "warn", "info", "debug" };
        for (int l = 0; l < 4; l++)
            if (!strcmp(o.log, levels[l]))
                gp_log_set_level((enum gp_log_level)l);
    }
    if (o.trace)
        gp_log_enable_trace(gp_log_parse_traces(o.trace));

    /* scratch space */
    char scratch_tmpl[FS_PATH_MAX];
    const char *scratch = o.scratch;
    bool scratch_is_temp = false;
    if (!scratch) {
        const char *base = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
        snprintf(scratch_tmpl, sizeof scratch_tmpl, "%s/gport2x-XXXXXX", base);
        if (!mkdtemp(scratch_tmpl)) {
            perror("gport2x: mkdtemp");
            return 2;
        }
        scratch = scratch_tmpl;
        scratch_is_temp = true;
    } else {
        mkdir(scratch, 0755);
    }

    /* the namespace */
    fs_t *fs = fs_create();
    uint64_t root_dev = 0x1F03;
    fs_backend_t *root;
    if (o.firmware) {
        fs_backend_t *lower = fs_backend_hostdir(o.firmware, true, root_dev);
        fs_statfs_t yaffs = { .type = 0x5941FF53, .bsize = 512, .frsize = 512, .blocks = 61440, .bfree = 20000, .bavail = 20000, .namelen = 255 };
        fs_hostdir_set_statfs(lower, &yaffs);
        fs_backend_t *upper = fs_backend_hostdir(make_temp(scratch, "root", o.keep_scratch), false, root_dev);
        root = fs_backend_overlay(lower, upper, root_dev);
    } else if (o.root) {
        root = fs_backend_hostdir(o.root, false, root_dev);
        fs_hostdir_set_sorted(root, false); /* tests compare listings with the host's own order */
    } else {
        root = fs_backend_hostdir(make_temp(scratch, "root", o.keep_scratch), false, root_dev);
        gp_warn("no --firmware: the guest root is an empty scratch directory");
    }
    fs_mount(fs, "/", root, "/dev/root", "yaffs", "rw,sync,noatime");
    fs_ctx_t rootctx;
    fs_ctx_init(&rootctx, 1, "/");
    const char *dirs[] = { "/proc", "/dev", "/tmp", "/mnt", "/mnt/sd", "/mnt/tmp", "/mnt/nand", "/usr", "/usr/gp2x", "/etc", "/bin", "/lib" };
    for (size_t d = 0; d < sizeof dirs / sizeof *dirs; d++)
        fs_mkdir(fs, &rootctx, dirs[d], 0755);
    fs_backend_t *devfs = fs_backend_devfs();
    fs_mount(fs, "/dev", devfs, "none", "devfs", "rw");
    fs_mount(fs, "/proc", fs_backend_proc(fs, gsys_procinfo_ops(), NULL, "2.4.25|#1 Mon Jan 1 00:00:00 KST 2007"), "none", "proc", "rw");
    {
        fs_backend_t *tmp = fs_backend_hostdir(make_temp(scratch, "tmp", o.keep_scratch), false, 9);
        fs_statfs_t sf = { .type = 0x01021994, .bsize = 4096, .frsize = 4096, .blocks = 1280, .bfree = 1270, .bavail = 1270, .namelen = 255 };
        fs_hostdir_set_statfs(tmp, &sf);
        fs_mount(fs, "/tmp", tmp, "none", "tmpfs", "rw,size=5M");
    }
    card_t *card = NULL;
    if (o.card || o.card_dir) {
        fs_backend_t *lower;
        uint64_t card_dev = 0xF101;
        if (o.card) {
            int r = card_open(o.card, &card);
            if (r < 0) {
                fprintf(stderr, "gport2x: card image %s: %s\n", o.card, strerror(-r));
                return 2;
            }
            lower = fs_backend_card(card);
        } else {
            lower = fs_backend_hostdir(o.card_dir, true, card_dev);
        }
        const char *saves = o.saves ? o.saves : make_temp(scratch, "saves", o.keep_scratch);
        if (o.saves)
            mkdir(o.saves, 0755);
        fs_backend_t *upper = fs_backend_hostdir(saves, false, card_dev);
        fs_mount(fs, "/mnt/sd", fs_backend_overlay(lower, upper, card_dev), "/dev/mmcsd/disc0/part1", "vfat",
                 "rw,sync,noatime,iocharset=utf8");
    }

    /* devices */
    gpdev_config_t dcfg;
    gpdev_config_default(&dcfg);
    if (o.test)
        dcfg.clock_mode = GPDEV_CLOCK_MAINSTEP;
    if (o.sdl && !o.test && !o.clock)
        dcfg.clock_mode = GPDEV_CLOCK_REAL;
    if (o.clock) {
        if (!strcmp(o.clock, "real")) dcfg.clock_mode = GPDEV_CLOCK_REAL;
        else if (!strcmp(o.clock, "step")) dcfg.clock_mode = GPDEV_CLOCK_STEP;
        else if (!strcmp(o.clock, "mainstep")) dcfg.clock_mode = GPDEV_CLOCK_MAINSTEP;
        else { fprintf(stderr, "gport2x: bad --clock %s\n", o.clock); return 2; }
    }
    if (o.step)
        dcfg.clock_step = o.step;
    dcfg.dump_dir = o.dump;
    dcfg.dump_every = o.dump_every;
    dcfg.snapshot_ms = (uint32_t)(o.snapshot_ms > 0 ? o.snapshot_ms : 0);
    dcfg.wav_path = o.wav;
    dcfg.pad_script = o.pad;
    dcfg.pad_time_script = o.pad_time;
    if (o.dump)
        mkdir(o.dump, 0755);
    if (o.native) {
        if (!native_available()) {
            fprintf(stderr, "gport2x: the native engine needs the armhf build (make armhf-native)\n");
            return 2;
        }
        if (native_prepare() < 0)
            return 2;
    }
    gpdev_t *dev = gpdev_create(&dcfg);
    if (dev && o.native && gpdev_native_regs(dev) != 0) {
        fprintf(stderr, "gport2x: cannot share the register file\n");
        return 2;
    }
    gpdev_register_nodes(dev, devfs);

    for (int k = 0; k < o.ninject; k++) {
        char spec[FS_PATH_MAX];
        snprintf(spec, sizeof spec, "%s", o.inject[k]);
        char *colon = strchr(spec, ':');
        if (!colon) {
            fprintf(stderr, "gport2x: --inject wants HOST:GUEST\n");
            return 2;
        }
        *colon = 0;
        if (copy_host_file(spec, fs, colon + 1) < 0)
            return 2;
    }

    /* the system */
    gsys_config_t cfg;
    gsys_config_default(&cfg);
    cfg.fs = fs;
    cfg.dev = dev;
    cfg.scratch_dir = scratch;
    if (o.test || o.time_set) {
        cfg.time_fixed = true;
        cfg.time_value = o.time_set ? o.time_value : 0;
    }
    cfg.max_flips = o.flips;
    if (o.exit_at_menu)
        cfg.exit_at_exec = "/usr/gp2x/gp2xmenu";
    cfg.capture = o.capture;
    cfg.capture_out = o.capture_out;
    cfg.capture_exe = o.capture_exe;
    cfg.capture_max = o.capture_max;
    cfg.capture_skip = o.capture_skip;
    cfg.capture_budget = o.capture_budget;
    cfg.max_insns = o.insns;
    if (o.quantum)
        cfg.quantum = o.quantum;
    gsys_t *sys = gsys_create(&cfg);
    static state_dump_t sd;
    static flip_hooks_t fh;
    fh.sys = sys;
    if (o.state_flip >= 0 && o.state_hi > o.state_lo) {
        sd.sys = sys;
        sd.flip = o.state_flip;
        sd.lo = o.state_lo;
        sd.hi = o.state_hi;
        sd.path = o.state_dump ? o.state_dump : "state.bin";
        fh.sd = &sd;
    }
    if (o.flip_log && !(fh.log = fopen(o.flip_log, "w"))) {
        gp_error("cannot write %s", o.flip_log);
        return 2;
    }
    if (fh.sd || fh.log)
        gpdev_set_flip_hook(dev, flip_hooks, &fh);
    sdl_host_t *sdl = NULL;
    if (o.sdl && !o.native) { /* natively the front-end thread creates it (native_frontend) */
        sdl = sdl_host_create(dev, sys, o.sdl_scale, !o.no_audio, o.fullscreen);
        if (!sdl)
            return 2;
    }

    const char *envp[80];
    int ne = 0;
    if (o.nenv == 0) {
        envp[ne++] = "PATH=/sbin:/usr/sbin:/usr/local/sbin:/bin:/usr/bin:/usr/local/bin";
        envp[ne++] = "LD_LIBRARY_PATH=./:/lib:/usr/local/lib:/usr/lib";
        envp[ne++] = "HOME=/";
        envp[ne++] = "TERM=linux";
        envp[ne++] = "SHELL=/bin/sh";
        envp[ne++] = "PWD=/";
    }
    for (int k = 0; k < o.nenv; k++)
        envp[ne++] = o.env[k];
    envp[ne] = NULL;

    gtask_t *first;
    if (o.argv0)
        gargv[0] = (char *)o.argv0;
    int r = gsys_spawn(sys, program, (const char *const *)gargv, envp, o.cwd, &first);
    if (r < 0) {
        fprintf(stderr, "gport2x: cannot start %s: %s\n", program, strerror(-r));
        return 126;
    }
    int status = -1;
    if (o.native) { /* returns only on a set-up failure; the guest's exit ends the process */
        static native_fe_t nfe;
        nfe = (native_fe_t){ dev, sys, o.sdl_scale, !o.no_audio, o.fullscreen, o.sdl };
        static native_end_t nend;
        nend.dev = dev;
        if (scratch_is_temp && !o.keep_scratch)
            snprintf(nend.scratch, sizeof nend.scratch, "%s", scratch);
        native_set_exit_hook(native_end, &nend);
        return native_run(sys, first, dev, native_frontend, &nfe) < 0 ? 2 : 0;
    }
    enum gsys_stop why = gsys_run(sys, &status);
    static const char *const reasons[] = { "all tasks exited", "instruction limit", "flip limit", "deadlock", "stop requested" };
    gp_info("stopped: %s; %llu instructions, %llu syscalls, %llu flips, status %#x", reasons[why],
            (unsigned long long)gsys_insn_count(sys), (unsigned long long)gsys_syscall_count(sys),
            (unsigned long long)gpdev_flip_count(dev), status);
    gp_info("audio: %llu bytes to the DAC, %llu underruns", (unsigned long long)gpdev_audio_bytes_out(dev),
            (unsigned long long)gpdev_dsp_underruns(dev));
    if (fh.log)
        fclose(fh.log);
    int rc;
    if (why == GSYS_STOP_ALL_EXITED && status >= 0)
        rc = (status & 0x7F) ? 128 + (status & 0x7F) : (status >> 8) & 0xFF;
    else if (why == GSYS_STOP_FLIPS || why == GSYS_STOP_INSNS)
        rc = 0;
    else
        rc = 125;

    sdl_host_destroy(sdl);
    gsys_destroy(sys);
    gpdev_destroy(dev);
    fs_destroy(fs); /* closes the card */
    if (scratch_is_temp && !o.keep_scratch)
        remove_tree(scratch);
    return rc;
}
