/* SDL2 front end. See sdl_host.h. */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gport2x/log.h"
#include "sdl_host.h"

#ifdef GPORT2X_HAVE_SDL
#include <SDL.h>

struct sdl_host {
    gpdev_t *dev;
    gsys_t *sys;
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex;
    SDL_AudioDeviceID audio;
    uint32_t audio_rate, audio_channels, audio_bits;
    bool pull; /* real time: SDL's audio callback is the DAC (gpdev_audio_pull) */
    uint64_t last_present_ns;
    uint32_t kbd, buttons, axes; /* pad bits from the keyboard, controller buttons and sticks/triggers */
    uint32_t pad;                /* their union, as last given to the device */
    uint64_t quit_combo_ns;      /* when SELECT + START were both first held */
    uint16_t page[320 * 240];
};

/* Game controllers, mapped by POSITION (SDL names the face buttons by
 * position: its A is always the bottom one). The GP2X's face buttons are
 * A left, B right, X bottom, Y top. */
static int button_bit(int b)
{
    switch (b) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return 0;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return 2;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return 4;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return 6;
    case SDL_CONTROLLER_BUTTON_START: return 8;
    case SDL_CONTROLLER_BUTTON_BACK: return 9;           /* SELECT */
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return 10;  /* L */
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return 11; /* R */
    case SDL_CONTROLLER_BUTTON_X: return 12;             /* west -> GP2X A (left) */
    case SDL_CONTROLLER_BUTTON_B: return 13;             /* east -> GP2X B (right) */
    case SDL_CONTROLLER_BUTTON_A: return 14;             /* south -> GP2X X (bottom) */
    case SDL_CONTROLLER_BUTTON_Y: return 15;             /* north -> GP2X Y (top) */
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return 27;     /* stick push */
    default: return -1;
    }
}

#define STICK_DEADZONE 16000
#define TRIGGER_THRESHOLD 16000

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Keyboard -> pad bits (the game's layout, spec 4.3). */
static int key_bit(SDL_Keycode k)
{
    switch (k) {
    case SDLK_UP: return 0;
    case SDLK_LEFT: return 2;
    case SDLK_DOWN: return 4;
    case SDLK_RIGHT: return 6;
    case SDLK_RETURN: return 8;       /* START */
    case SDLK_RSHIFT: case SDLK_SPACE: return 9; /* SELECT */
    case SDLK_q: return 10;           /* L */
    case SDLK_w: return 11;           /* R */
    case SDLK_z: return 12;           /* A */
    case SDLK_x: return 13;           /* B */
    case SDLK_a: return 14;           /* X */
    case SDLK_s: return 15;           /* Y */
    case SDLK_MINUS: case SDLK_KP_MINUS: return 22; /* VOL- */
    case SDLK_EQUALS: case SDLK_KP_PLUS: return 23; /* VOL+ */
    case SDLK_c: return 27;           /* stick push */
    default: return -1;
    }
}

static void present(sdl_host_t *h)
{
    if (gpdev_read_page(h->dev, gpdev_scanout_page(h->dev), h->page) < 0)
        return;
    SDL_UpdateTexture(h->tex, NULL, h->page, 320 * 2);
    SDL_RenderClear(h->ren);
    SDL_RenderCopy(h->ren, h->tex, NULL, NULL);
    SDL_RenderPresent(h->ren);
}

/* SDL's audio thread: the GP2X DAC. Takes the guest's samples as the real
 * audio device consumes them, silence where the guest has not written. */
static void pull_cb(void *ud, Uint8 *stream, int len)
{
    sdl_host_t *h = ud;
    gpdev_audio_pull(h->dev, stream, (size_t)len);
}

/* Pull mode: (re)open the audio device when /dev/dsp's format changes. */
static void pull_audio_follow(sdl_host_t *h)
{
    uint32_t rate, channels, bits;
    if (!gpdev_dsp_format(h->dev, &rate, &channels, &bits) || !rate || !channels)
        return;
    if (h->audio && rate == h->audio_rate && channels == h->audio_channels && bits == h->audio_bits)
        return;
    if (h->audio) {
        SDL_CloseAudioDevice(h->audio);
        h->audio = 0;
    }
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = (int)rate;
    want.format = bits == 16 ? AUDIO_S16LSB : AUDIO_U8;
    want.channels = (Uint8)channels;
    want.samples = 256; /* ~6 ms at 44.1 kHz: well inside the guest's own buffer (8 x 512 bytes) */
    want.callback = pull_cb;
    want.userdata = h;
    h->audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0); /* no format changes allowed: SDL converts */
    h->audio_rate = rate;
    h->audio_channels = channels;
    h->audio_bits = bits;
    if (!h->audio) {
        gp_warn("SDL audio: %s", SDL_GetError());
        return;
    }
    SDL_PauseAudioDevice(h->audio, 0);
    gp_info("SDL audio (callback): %s, %d Hz, %d channels, %d-frame buffer", SDL_GetCurrentAudioDriver(), have.freq,
            have.channels, have.samples);
}

static void update_pad(sdl_host_t *h)
{
    uint32_t pad = h->kbd | h->buttons | h->axes;
    if (pad != h->pad) {
        h->pad = pad;
        gpdev_set_pad_mask(h->dev, pad);
    }
}

/* Left stick -> the 8-way pad; triggers -> VOL- / VOL+ (all open controllers). */
static void read_axes(sdl_host_t *h)
{
    uint32_t a = 0;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        SDL_GameController *gc = SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(i));
        if (!gc)
            continue;
        int x = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
        int y = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
        if (y < -STICK_DEADZONE) a |= 1u << 0;
        if (x < -STICK_DEADZONE) a |= 1u << 2;
        if (y > STICK_DEADZONE) a |= 1u << 4;
        if (x > STICK_DEADZONE) a |= 1u << 6;
        if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > TRIGGER_THRESHOLD) a |= 1u << 22;
        if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > TRIGGER_THRESHOLD) a |= 1u << 23;
    }
    h->axes = a;
}

static void host_hook(void *ctx)
{
    sdl_host_t *h = ctx;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            gsys_request_stop(h->sys);
            break;
        case SDL_CONTROLLERDEVICEADDED:
            if (SDL_GameControllerOpen(e.cdevice.which))
                gp_info("controller: %s", SDL_GameControllerNameForIndex(e.cdevice.which));
            break;
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP: {
            if (e.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE && e.type == SDL_CONTROLLERBUTTONDOWN) {
                gsys_request_stop(h->sys);
                break;
            }
            int bit = button_bit(e.cbutton.button);
            if (e.type == SDL_CONTROLLERBUTTONDOWN) /* in the log: what the pad really sends */
                gp_info("pad: %s down -> %s", SDL_GameControllerGetStringForButton((SDL_GameControllerButton)e.cbutton.button),
                        bit < 0 ? "unused" : "GP2X button");
            if (bit < 0)
                break;
            if (e.type == SDL_CONTROLLERBUTTONDOWN)
                h->buttons |= 1u << bit;
            else
                h->buttons &= ~(1u << bit);
            break;
        }
        case SDL_JOYBUTTONDOWN: /* raw joystick buttons, for pads SDL has no game controller mapping for */
            if (!SDL_IsGameController(e.jbutton.which))
                gp_info("pad: joystick %d button %d down (no controller mapping)", (int)e.jbutton.which, (int)e.jbutton.button);
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            if (e.key.keysym.sym == SDLK_ESCAPE && e.type == SDL_KEYDOWN) {
                gsys_request_stop(h->sys);
                break;
            }
            int bit = key_bit(e.key.keysym.sym);
            if (bit < 0)
                break;
            if (e.type == SDL_KEYDOWN)
                h->kbd |= 1u << bit;
            else
                h->kbd &= ~(1u << bit);
            break;
        }
        default:
            break;
        }
    }
    read_axes(h);
    update_pad(h);
    if (h->pull)
        pull_audio_follow(h);
    uint64_t t = now_ns();
    /* SELECT + START held for a second quits (handhelds without a guide button) */
    if ((h->pad & (1u << 8)) && (h->pad & (1u << 9))) {
        if (!h->quit_combo_ns)
            h->quit_combo_ns = t;
        else if (t - h->quit_combo_ns > 1000000000ull)
            gsys_request_stop(h->sys);
    } else {
        h->quit_combo_ns = 0;
    }
    if (t - h->last_present_ns >= 16000000ull) {
        h->last_present_ns = t;
        present(h);
    }
}

static void audio_sink(void *ctx, const uint8_t *pcm, size_t n, uint32_t rate, uint32_t channels, uint32_t bits)
{
    sdl_host_t *h = ctx;
    if (!h->audio || rate != h->audio_rate || channels != h->audio_channels || bits != h->audio_bits) {
        if (h->audio) {
            SDL_CloseAudioDevice(h->audio);
            h->audio = 0;
        }
        SDL_AudioSpec want, have;
        SDL_zero(want);
        want.freq = (int)rate;
        want.format = bits == 16 ? AUDIO_S16LSB : AUDIO_U8;
        want.channels = (Uint8)channels;
        want.samples = 1024;
        h->audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (!h->audio) {
            gp_warn("SDL audio: %s", SDL_GetError());
            return;
        }
        h->audio_rate = rate;
        h->audio_channels = channels;
        h->audio_bits = bits;
        SDL_PauseAudioDevice(h->audio, 0);
        gp_info("SDL audio: %s, %d Hz, %d channels, format %#x, %d-frame buffer (asked %u Hz, %u channels, %u bits)",
                SDL_GetCurrentAudioDriver(), have.freq, have.channels, have.format, have.samples, rate, channels, bits);
    }
    /* keep at most ~200 ms queued: the guest's OSS buffer already paces it */
    uint32_t frame_bytes = channels * (bits / 8);
    if (SDL_GetQueuedAudioSize(h->audio) > rate * frame_bytes / 5)
        return;
    SDL_QueueAudio(h->audio, pcm, (Uint32)n);
}

sdl_host_t *sdl_host_create(gpdev_t *dev, gsys_t *sys, int scale, bool audio, bool fullscreen)
{
    if (scale <= 0)
        scale = 3;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | (audio ? SDL_INIT_AUDIO : 0)) < 0) {
        gp_error("SDL_Init: %s", SDL_GetError());
        return NULL;
    }
    sdl_host_t *h = calloc(1, sizeof *h);
    if (!h)
        return NULL;
    h->dev = dev;
    h->sys = sys;
    /* controller mappings: SDL_GAMECONTROLLERCONFIG(_FILE) are read by SDL itself;
     * GPORT2X_GAMECONTROLLERDB names a gamecontrollerdb.txt to add */
    const char *db = getenv("GPORT2X_GAMECONTROLLERDB");
    if (db && SDL_GameControllerAddMappingsFromFile(db) < 0)
        gp_warn("controller mappings %s: %s", db, SDL_GetError());
    h->win = SDL_CreateWindow("GPort2X", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 320 * scale, 240 * scale,
                              SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | (fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!h->win) {
        gp_error("SDL window: %s", SDL_GetError());
        free(h);
        return NULL;
    }
    /* no vsync in fullscreen (handhelds): a compositor stops a hidden window's
     * frames, and a present that waits for vsync would then never return */
    h->ren = SDL_CreateRenderer(h->win, -1, SDL_RENDERER_ACCELERATED | (fullscreen ? 0 : SDL_RENDERER_PRESENTVSYNC));
    if (!h->ren)
        h->ren = SDL_CreateRenderer(h->win, -1, 0);
    SDL_RenderSetLogicalSize(h->ren, 320, 240);
    SDL_RenderSetIntegerScale(h->ren, SDL_TRUE); /* crisp pixels: 640x480 screens get exactly 2x */
    if (fullscreen)
        SDL_ShowCursor(SDL_DISABLE);
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i) && SDL_GameControllerOpen(i))
            gp_info("controller: %s", SDL_GameControllerNameForIndex(i));
    h->tex = SDL_CreateTexture(h->ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, 320, 240);
    gpdev_set_host_hook(dev, host_hook, h);
    if (audio && gpdev_sleeps_are_real(dev)) { /* real time: the audio device paces the guest's DAC */
        h->pull = true;
        gpdev_set_audio_pull(dev, true);
    } else if (audio) {
        gpdev_set_audio_sink(dev, audio_sink, h);
    }
    gp_info("SDL front end: %s; keys: arrows, Enter=START, Space=SELECT, Q/W=L/R, Z/X/A/S=A/B/X/Y, -/+=VOL, C=push, Esc=quit",
            fullscreen ? "fullscreen" : "window");
    gp_info("controllers by position: west/east/south/north = GP2X A/B/X/Y, shoulders = L/R, Back = SELECT, "
            "left stick = pad, stick click = push, triggers = VOL-/VOL+, guide or SELECT+START held 1 s = quit");
    return h;
}

void sdl_host_destroy(sdl_host_t *h)
{
    if (!h)
        return;
    gpdev_set_host_hook(h->dev, NULL, NULL);
    gpdev_set_audio_sink(h->dev, NULL, NULL);
    if (h->audio)
        SDL_CloseAudioDevice(h->audio);
    if (h->tex)
        SDL_DestroyTexture(h->tex);
    if (h->ren)
        SDL_DestroyRenderer(h->ren);
    if (h->win)
        SDL_DestroyWindow(h->win);
    SDL_Quit();
    free(h);
}

#else /* no SDL */

sdl_host_t *sdl_host_create(gpdev_t *dev, gsys_t *sys, int scale, bool audio, bool fullscreen)
{
    (void)dev; (void)sys; (void)scale; (void)audio; (void)fullscreen;
    gp_error("this build has no SDL2 front end (build with SDL2 installed)");
    return NULL;
}

void sdl_host_destroy(sdl_host_t *h) { (void)h; }

#endif
