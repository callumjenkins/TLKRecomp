/* sdl_main.c -- The Lion King playable SDL3 host.
 *
 * usage: tlk_sdl --rom <path.sfc> [options]
 *
 * Runs exactly the emulation path of tlk_headless: same sources, same host
 * contract (headless_host.c), same RtlRunFrame + beam frame driver, same
 * non-extended APU timeline (the framework desktop host turns on
 * RtlEnableExtendedFrameTiming, which bypasses TLK's NTSC SPC ratio; this
 * host never calls it). Everything runs on one thread, so RtlApuLock stays a
 * no-op and the audio device only receives what the main loop queues.
 *
 * Pacing: 60.0988 Hz NTSC (21477272 / 357366) on a wall-clock deadline, or
 * vsync=1 in the ini. Audio: RtlRenderAudio resamples the DSP's 32040 Hz
 * output onto the device rate (with its own ring servo); this host queues
 * rate/60.0988 frames per field, trimmed by up to +/-0.5% to hold the device
 * queue at ~3 fields of latency, so neither side drifts.
 *
 * Options:
 *   --rom path            ROM (required; also rom= in the ini)
 *   --config path         ini file (default tlk_sdl.ini, or $TLK_SDL_INI)
 *   --input tape          per-frame u16 LE $4218 tape (same as tlk_headless)
 *   --input-polled tape   one tape word per game poll (same as tlk_headless)
 *   --input-raw tape      replay a last_session.rec recording (pad word per frame)
 *   --no-record           do not write last_session.rec (live play records by default;
 *                         --frames runs never record unless --record is given)
 *   --record path         record to path instead (also with --input, to test replay)
 *   --lle                 SNESRECOMP_LLE_BOUNCE=0 (same as tlk_headless)
 *   --interp-targets f    SNESRECOMP_LLE_INTERP_TARGET_FILE
 *   --frames N            quit after N frames, print final hashes
 *   --hash-at a,b,...     print frame_sha256 after those frames
 *   --dump-png path       write the last frame as PNG
 *   --no-pace             run as fast as possible (proof runs)
 *   --no-audio            do not open an audio device
 *   --wav path [--wav-seconds S]  record the queued audio (default 10 s)
 *   --scale N  --fullscreen
 *   --save-at N state     write a savestate after frame N (proof runs)
 *   --load state          start from a savestate (frame count continues)
 *   --load-at N state     load a savestate after frame N, in process (proof runs)
 *   --menu-at N           open the settings overlay after frame N (screenshots)
 *   --menu-page controls  with --menu-at: open the controls (remap) page
 *   --selftest-remap      drive the menu/remap/hotplug code with synthetic SDL
 *                         events and a virtual gamepad, print PASS/FAIL, exit
 *   --list-drivers        print the SDL version and the video/audio drivers
 *                         compiled into this binary, exit
 *
 * Without --rom (or rom= in the ini), the .sfc/.smc files next to the binary
 * and in the current directory are verified and the first match is used. If
 * none matches (or the ini's rom is gone), an interactive run asks with the
 * system file dialog and saves the choice as rom= in the ini.
 *
 * Keys (default): arrows, Z=B X=A A=Y S=X Q=L W=R Enter=Start RShift=Select,
 * F11 or Alt+Enter fullscreen. F1-F4 save state slot 1-4, F5-F8 (or
 * Shift+F1-F4) load it; slots are tlk_slotN.state next to the ini. Hold ` (pad:
 * right stick click) for turbo: turbo_speed times faster, 4 by default, sound
 * muted; rebindable on the Controls page. Tab or Esc
 * opens the settings overlay (emulation pauses); Esc in the overlay quits.
 * Gamepads default to by position (south=B, east=A, west=Y, north=X); Guide or
 * Back+Start opens the overlay. The Controls page rebinds a key or a gamepad
 * button per SNES button (whichever is pressed next), with Reset defaults;
 * bindings go to the ini (b = Z ..., pad_b = a ...). Pads are hotplugged.
 *
 * Video goes through snesrecomp's OpenGL 3.3 presenter (desktop/opengl.c):
 * the overlay's Shader row cycles the bundled presets in assets/shaders next
 * to the binary; Aspect, Smooth and Int. scale apply with or without one.
 * Without OpenGL 3.3 (or with opengl = 0) the plain SDL renderer is used and
 * shaders are unavailable. None of this touches s_pixels, which the hashes,
 * --dump-png and the gates read. */
#include "tlk_game.h"
#include "verified_rom.h"
#include "tlk_font8x8.h"
#include "tlk_savestate.h"

#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "cpu_state.h"
#include "sha256.h"
#include "snes/interp_bridge.h"
#include "snes/joypad.h"
#include "snes/ppu.h"
#include "snes/snes.h"

#define SDL_MAIN_HANDLED 1
#include <SDL3/SDL.h>

/* snesrecomp's desktop OpenGL presenter (GLSL shader presets, aspect). */
#include "opengl.h"
#include "util.h"
void OpenGLRenderer_Create(struct RendererFuncs *funcs);
void TlkVideoConfig(const char *shader, int aspect, int smooth);   /* tlk_video.c */

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tlk_compat.h"

enum { kWidth = 256, kHeight = 224, kBpp = 4 };
static uint8_t s_pixels[kWidth * kHeight * kBpp];
/* What is shown: the PPU frame plus the menu/toast overlay. Hashes and PNG
 * dumps read s_pixels, never this. */
static uint32_t s_screen[kWidth * kHeight];

#define NTSC_FPS (21477272.0 / 357366.0) /* 60.0988 */

/* ── PNG writer (stored deflate), same as headless_main.c ───────────────── */
static uint32_t s_crc_table[256];
static void CrcInit(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
        s_crc_table[n] = c;
    }
}
static uint32_t CrcUpdate(uint32_t c, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) c = s_crc_table[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return c;
}
static void Be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static int WriteChunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
    uint8_t hdr[8], tail[4];
    Be32(hdr, len); memcpy(hdr + 4, type, 4);
    uint32_t crc = CrcUpdate(0xffffffffu, hdr + 4, 4);
    crc = CrcUpdate(crc, data, len) ^ 0xffffffffu;
    Be32(tail, crc);
    return fwrite(hdr, 1, 8, f) == 8 && (len == 0 || fwrite(data, 1, len, f) == len) &&
           fwrite(tail, 1, 4, f) == 4;
}
static int WritePng(const char *path, const uint8_t *px, int w, int h, int pitch) {
    const size_t row = (size_t)w * 3 + 1, raw_len = row * (size_t)h;
    uint8_t *raw = (uint8_t *)malloc(raw_len);
    if (!raw) return 0;
    for (int y = 0; y < h; y++) {
        uint8_t *d = raw + row * (size_t)y;
        const uint8_t *s = px + (size_t)pitch * (size_t)y;
        *d++ = 0;
        for (int x = 0; x < w; x++) { d[0] = s[x * 4 + 2]; d[1] = s[x * 4 + 1]; d[2] = s[x * 4 + 0]; d += 3; }
    }
    const size_t nblocks = (raw_len + 65534) / 65535, z_len = 2 + raw_len + nblocks * 5 + 4;
    uint8_t *z = (uint8_t *)malloc(z_len);
    if (!z) { free(raw); return 0; }
    size_t o = 0;
    z[o++] = 0x78; z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_len; i++) { a = (a + raw[i]) % 65521u; b = (b + a) % 65521u; }
    for (size_t off = 0; off < raw_len; off += 65535) {
        size_t n = raw_len - off < 65535 ? raw_len - off : 65535;
        z[o++] = (uint8_t)(off + n == raw_len);
        z[o++] = (uint8_t)n; z[o++] = (uint8_t)(n >> 8); z[o++] = (uint8_t)~n; z[o++] = (uint8_t)(~n >> 8);
        memcpy(z + o, raw + off, n); o += n;
    }
    Be32(z + o, (b << 16) | a); o += 4;
    free(raw);
    FILE *f = fopen(path, "wb");
    if (!f) { free(z); return 0; }
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    uint8_t ihdr[13];
    Be32(ihdr, (uint32_t)w); Be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    int ok = fwrite(sig, 1, 8, f) == 8 && WriteChunk(f, "IHDR", ihdr, 13) &&
             WriteChunk(f, "IDAT", z, (uint32_t)o) && WriteChunk(f, "IEND", NULL, 0);
    free(z);
    if (fclose(f) != 0) ok = 0;
    return ok;
}

/* ── Input tape (same semantics as tlk_headless) ────────────────────────── */
static uint16_t *s_tape;
static size_t s_tape_frames;
static int s_polled;
static int s_raw_tape;  /* --input-raw: words are the host pad value, not $4218 */
static size_t s_poll_idx;

static int LoadTape(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0 || len % 2 != 0) { fclose(f); return 0; }
    s_tape_frames = (size_t)len / 2;
    s_tape = (uint16_t *)calloc(s_tape_frames ? s_tape_frames : 1, 2);
    uint8_t *bytes = (uint8_t *)malloc((size_t)len ? (size_t)len : 1);
    int ok = s_tape && bytes && fread(bytes, 1, (size_t)len, f) == (size_t)len;
    fclose(f);
    if (ok)
        for (size_t i = 0; i < s_tape_frames; i++)
            s_tape[i] = (uint16_t)(bytes[2 * i] | (bytes[2 * i + 1] << 8));
    free(bytes);
    return ok;
}
static uint32_t TapeFrame(long frame) {
    if (!s_tape || frame < 0 || (size_t)frame >= s_tape_frames) return 0;
    return s_raw_tape ? s_tape[frame] : joypad_auto_read_word(s_tape[frame]) & 0x0fffu;
}
/* Live sessions record the pad word fed to each emulated frame (u16 LE, from
 * power-on) so a bug seen while playing can be replayed with --input-raw. */
static FILE *s_rec;
static void RecStop(const char *why) {
    if (!s_rec) return;
    fclose(s_rec); s_rec = NULL;
    fprintf(stderr, "input recording stopped: %s\n", why);
}
static uint16_t TapePolled(void) {
    if (!s_tape || s_poll_idx >= s_tape_frames) return 0;
    return joypad_auto_read_word(s_tape[s_poll_idx]) & 0x0fffu;
}
static void PollHook(CpuState *cpu, uint32_t pc24) {
    (void)cpu; (void)pc24;
    uint16_t w = TapePolled();
    g_snes->input1_currentState = w;
    joypad_set_pad(0, w);
    if (s_poll_idx < s_tape_frames) s_poll_idx++;
}
/* Task CC: the same poll when the pad read runs compiled (cpu_state.hunks aot-pad-read-hook). */
extern void (*g_tlk_aot_pad_read_hook)(void);
static void PollHookCompiled(void) { PollHook(&g_cpu, 0xC0A07C); }

/* ── Config ─────────────────────────────────────────────────────────────── */
/* Runtime pad bits: 0 B, 1 Y, 2 Select, 3 Start, 4 Up, 5 Down, 6 Left,
 * 7 Right, 8 A, 9 X, 10 L, 11 R. Binding 12 is turbo (hold to fast-forward),
 * not a pad bit. */
enum { kSnesButtons = 12, kTurbo = 12, kBindings = 13 };
static const char *const kButtonNames[kBindings] = {
    "b", "y", "select", "start", "up", "down", "left", "right", "a", "x", "l", "r", "turbo" };
static const SDL_Keycode kDefaultKeys[kBindings] = {
    SDLK_Z, SDLK_A, SDLK_RSHIFT, SDLK_RETURN, SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT,
    SDLK_X, SDLK_S, SDLK_Q, SDLK_W, SDLK_GRAVE };
/* Gamepad defaults are by position (south=B, east=A, west=Y, north=X). The
 * left stick and the analog triggers always add d-pad / L / R on top. */
static const SDL_GamepadButton kDefaultPad[kBindings] = {
    SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_BACK,
    SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
    SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_EAST,
    SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
    SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_STICK };
static SDL_Keycode s_keys[kBindings];
static SDL_GamepadButton s_pad_btn[kBindings];
static void ResetBindings(void) {
    memcpy(s_keys, kDefaultKeys, sizeof s_keys);
    memcpy(s_pad_btn, kDefaultPad, sizeof s_pad_btn);
}
static int s_scale = 3, s_fullscreen, s_vsync, s_audio = 1, s_audio_rate = 48000, s_volume = 100;
/* Turbo: while its key or button is held, run s_turbo_speed frames per shown frame. */
static int s_turbo_speed = 4, s_turbo_held;
/* Video: aspect is a SnesDisplayAspect (default 8:7 = square pixels, the
 * picture as it always was here); shader "" = none, else a .glslp/.glsl path.
 * Shaders need OpenGL 3.3 (opengl = 0 forces the plain SDL renderer). */
static int s_aspect = kSnesDisplayAspect_SquarePixels8x7, s_smooth, s_integer, s_opengl = 1;
static char s_shader[1024];
static const char *const kShaders[][2] = {
    { "", "off" },
    { "assets/shaders/crt-soft.glslp", "CRT" },
    { "assets/shaders/lcd-grid.glslp", "LCD grid" },
    { "assets/shaders/sharp.glslp", "Sharpen" },
    { "assets/shaders/warm-composite.glslp", "Composite" } };
enum { kShaderCount = sizeof kShaders / sizeof kShaders[0] };
static char s_rom_cfg[1024];
static const char *s_cfg_path;

static char *Trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

/* Plain "key = value" lines; [sections] and ; or # comments are ignored.
 * Keys: rom, scale, fullscreen, vsync, audio, audio_rate, volume, shader,
 * aspect (4:3 8:7 1:1), smooth, integer_scale, opengl, turbo_speed (2-16),
 * and one per binding
 * (b y select start up down left right a x l r turbo) holding an SDL key name such
 * as "Z", "Return", "Right Shift", "Up". */
static void LoadConfig(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *s = Trim(line);
        if (!*s || *s == ';' || *s == '#' || *s == '[') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = Trim(s), *v = Trim(eq + 1);
        for (char *p = k; *p; p++) *p = (char)tolower((unsigned char)*p);
        if (!strcmp(k, "rom")) snprintf(s_rom_cfg, sizeof s_rom_cfg, "%s", v);
        else if (!strcmp(k, "scale")) s_scale = atoi(v);
        else if (!strcmp(k, "fullscreen")) s_fullscreen = atoi(v);
        else if (!strcmp(k, "vsync")) s_vsync = atoi(v);
        else if (!strcmp(k, "audio")) s_audio = atoi(v);
        else if (!strcmp(k, "audio_rate")) s_audio_rate = atoi(v);
        else if (!strcmp(k, "volume")) s_volume = atoi(v);
        else if (!strcmp(k, "shader")) snprintf(s_shader, sizeof s_shader, "%s", v);
        else if (!strcmp(k, "aspect")) {
            uint8_t a;
            if (SnesDisplayAspect_Parse(v, &a)) s_aspect = a;
            else fprintf(stderr, "config: unknown aspect '%s' (4:3, 8:7 or 1:1)\n", v);
        }
        else if (!strcmp(k, "smooth")) s_smooth = atoi(v);
        else if (!strcmp(k, "integer_scale")) s_integer = atoi(v);
        else if (!strcmp(k, "opengl")) s_opengl = atoi(v);
        else if (!strcmp(k, "turbo_speed")) s_turbo_speed = SDL_clamp(atoi(v), 2, 16);
        else if (!strncmp(k, "pad_", 4)) {
            for (int i = 0; i < kBindings; i++)
                if (!strcmp(k + 4, kButtonNames[i])) {
                    SDL_GamepadButton b = SDL_GetGamepadButtonFromString(v);
                    if (b != SDL_GAMEPAD_BUTTON_INVALID) s_pad_btn[i] = b;
                    else fprintf(stderr, "config: unknown gamepad button '%s' for %s\n", v, k);
                }
        } else {
            for (int i = 0; i < kBindings; i++)
                if (!strcmp(k, kButtonNames[i])) {
                    SDL_Keycode kc = SDL_GetKeyFromName(v);
                    if (kc != SDLK_UNKNOWN) s_keys[i] = kc;
                    else fprintf(stderr, "config: unknown key name '%s' for %s\n", v, k);
                }
        }
    }
    fclose(f);
}

/* Rewrites the ini with the current settings (comments are not kept). */
static int SaveConfig(void) {
    FILE *f = fopen(s_cfg_path, "w");
    if (!f) { fprintf(stderr, "cannot write %s\n", s_cfg_path); return 0; }
    fprintf(f, "; tlk_sdl settings (rewritten by the in-game settings menu)\n");
    if (s_rom_cfg[0]) fprintf(f, "rom = %s\n", s_rom_cfg);
    fprintf(f, "scale = %d\nfullscreen = %d\nvsync = %d\naudio = %d\naudio_rate = %d\nvolume = %d\n",
            s_scale, s_fullscreen, s_vsync, s_audio, s_audio_rate, s_volume);
    fprintf(f, "; shader: empty, a bundled assets/shaders/*.glslp or your own .glslp/.glsl path\n");
    fprintf(f, "shader = %s\naspect = %s\nsmooth = %d\ninteger_scale = %d\nopengl = %d\n",
            s_shader, SnesDisplayAspect_Name(s_aspect), s_smooth, s_integer, s_opengl);
    fprintf(f, "; hold turbo to run turbo_speed times faster\nturbo_speed = %d\n", s_turbo_speed);
    fprintf(f, "; keys use SDL key names\n");
    for (int i = 0; i < kBindings; i++) fprintf(f, "%s = %s\n", kButtonNames[i], SDL_GetKeyName(s_keys[i]));
    fprintf(f, "; gamepad buttons use SDL names (a = south, b = east, x = west, y = north,\n"
               "; back, start, guide, leftshoulder, rightshoulder, dpup, dpdown, ...)\n");
    for (int i = 0; i < kBindings; i++)
        fprintf(f, "pad_%s = %s\n", kButtonNames[i], SDL_GetGamepadStringForButton(s_pad_btn[i]));
    return fclose(f) == 0;
}

/* Last directory separator in a path ('\\' too on Windows), or NULL. */
static const char *LastSep(const char *p) {
    const char *sl = strrchr(p, '/');
#ifdef _WIN32
    const char *bs = strrchr(p, '\\');
    if (bs && (!sl || bs > sl)) sl = bs;
#endif
    return sl;
}

/* Savestate slot path: tlk_slotN.state in the ini's directory. */
static void SlotPath(int slot, char *out, size_t n) {
    const char *sl = LastSep(s_cfg_path);
    if (sl) snprintf(out, n, "%.*s/tlk_slot%d.state", (int)(sl - s_cfg_path), s_cfg_path, slot);
    else snprintf(out, n, "tlk_slot%d.state", slot);
}

/* ── Overlay: 8x8 text in the 256x224 logical space ────────────────────── */
static char s_toast[64];
static uint64_t s_toast_until;

static void Toast(const char *msg) {
    snprintf(s_toast, sizeof s_toast, "%s", msg);
    s_toast_until = SDL_GetTicksNS() + 2000000000ull;
    fprintf(stderr, "%s\n", msg);
}

/* The overlay is drawn in software into s_screen, so a shader treats it like
 * the game picture and both presenters show the same thing. */
static uint32_t s_ink;
static void Ink(int r, int g, int b) { s_ink = (uint32_t)(r << 16 | g << 8 | b); }

static void DrawText(int x, int y, const char *t) {
    for (; *t; t++, x += 8) {
        int c = (unsigned char)*t;
        if (c < 32 || c > 126) c = '?';
        for (int r = 0; r < 8; r++) {
            uint8_t bits = kTlkFont8x8[c - 32][r];
            for (int b = 0; b < 8; b++)
                if ((bits & (1u << b)) && x + b >= 0 && x + b < kWidth && y + r >= 0 && y + r < kHeight)
                    s_screen[(y + r) * kWidth + x + b] = s_ink;
        }
    }
}

/* Black at alpha 230/255 over the picture. */
static void Panel(int x, int y, int w, int h) {
    for (int j = y; j < y + h && j < kHeight; j++)
        for (int i = x; i < x + w && i < kWidth; i++) {
            uint32_t p = s_screen[j * kWidth + i];
            uint32_t r = (p >> 16 & 255) * 25 / 255, g = (p >> 8 & 255) * 25 / 255, b = (p & 255) * 25 / 255;
            s_screen[j * kWidth + i] = r << 16 | g << 8 | b;
        }
}

/* Host state shared by main() and the event handler. */
static SDL_Window *win;
static SDL_Renderer *ren;      /* plain SDL presenter, when OpenGL is off or unavailable */
static SDL_Texture *tex;
static struct RendererFuncs s_gl;
static int s_gl_on;            /* the OpenGL presenter owns the window */
static SDL_AudioStream *astream;
static int pace = 1, running = 1;
static long frame;
static const char *input_path;
static double audio_acc;

enum { M_RESUME, M_SCALE, M_FULLSCREEN, M_VSYNC, M_SHADER, M_ASPECT, M_SMOOTH, M_INTEGER,
       M_AUDIO, M_VOLUME, M_KEYS, M_SAVE, M_QUIT, M_COUNT };
/* Bindings page rows: 12 buttons and turbo, then "Reset defaults", then "Back". */
enum { K_RESET = kBindings, K_BACK = kBindings + 1, K_COUNT = kBindings + 2 };
static int s_menu_open, s_menu_sel, s_menu_keys, s_key_sel, s_key_wait;

/* Short on-screen names by position; the ini keeps SDL's names. */
static const char *PadName(SDL_GamepadButton b) {
    switch (b) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return "Bottom";
    case SDL_GAMEPAD_BUTTON_EAST: return "Right";
    case SDL_GAMEPAD_BUTTON_WEST: return "Left";
    case SDL_GAMEPAD_BUTTON_NORTH: return "Top";
    case SDL_GAMEPAD_BUTTON_BACK: return "Back";
    case SDL_GAMEPAD_BUTTON_GUIDE: return "Guide";
    case SDL_GAMEPAD_BUTTON_START: return "Start";
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "L3";
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "R3";
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "LB";
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "RB";
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return "D-Up";
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "D-Down";
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "D-Left";
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "D-Right";
    default: {
        const char *s = SDL_GetGamepadStringForButton(b);
        return s ? s : "-";
    }
    }
}

/* Index of s_shader in kShaders, or -1 for a custom path from the ini. */
static int ShaderIndex(void) {
    for (int i = 0; i < kShaderCount; i++)
        if (!strcmp(s_shader, kShaders[i][0])) return i;
    return -1;
}

static void DrawMenu(void) {
    char line[64];
    Panel(16, 8, 232, 208);
    Ink(255, 210, 80);
    DrawText(32, 14, s_menu_keys ? "CONTROLS  key / gamepad" : "SETTINGS");
    if (!s_menu_keys) {
        static const char *const kAspectNames[] = { "4:3 TV", "8:7 pixel", "1:1" };
        int sh = ShaderIndex();
        for (int i = 0; i < M_COUNT; i++) {
            switch (i) {
            case M_RESUME: snprintf(line, sizeof line, "Resume"); break;
            case M_SCALE: snprintf(line, sizeof line, "Window     < %dx >", s_scale); break;
            case M_FULLSCREEN: snprintf(line, sizeof line, "Fullscreen < %s >", s_fullscreen ? "on" : "off"); break;
            case M_VSYNC: snprintf(line, sizeof line, "Vsync      < %s >", s_vsync ? "on" : "off"); break;
            case M_SHADER:
                snprintf(line, sizeof line, "Shader     < %s >", !s_gl_on ? "no OpenGL" : sh < 0 ? "custom" : kShaders[sh][1]);
                break;
            case M_ASPECT: snprintf(line, sizeof line, "Aspect     < %s >", kAspectNames[SnesDisplayAspect_Clamp(s_aspect)]); break;
            case M_SMOOTH: snprintf(line, sizeof line, "Smooth     < %s >", s_smooth ? "on" : "off"); break;
            case M_INTEGER: snprintf(line, sizeof line, "Int. scale < %s >", s_integer ? "on" : "off"); break;
            case M_AUDIO: snprintf(line, sizeof line, "Audio      < %s >", s_audio ? "on" : "off"); break;
            case M_VOLUME: snprintf(line, sizeof line, "Volume     < %d%% >", s_volume); break;
            case M_KEYS: snprintf(line, sizeof line, "Controls..."); break;
            case M_SAVE: snprintf(line, sizeof line, "Save settings"); break;
            case M_QUIT: snprintf(line, sizeof line, "Quit"); break;
            }
            if (i == s_menu_sel) Ink(255, 255, 255);
            else Ink(150, 150, 150);
            DrawText(24, 28 + i * 11, i == s_menu_sel ? ">" : " ");
            DrawText(36, 28 + i * 11, line);
        }
        Ink(150, 150, 150);
        DrawText(24, 176, "F1-F4 save, F5-F8 load");
        DrawText(24, 186, "Esc quits  Tab resumes");
        DrawText(24, 196, "Pad: Guide or Back+Start");
    } else {
        for (int i = 0; i < K_COUNT; i++) {
            if (i < kBindings) {
                int w = s_key_wait && i == s_key_sel;
                const char *kn = SDL_GetKeyName(s_keys[i]);
                char kshort[16];
                /* "Right Shift" -> "R Shift" so the name fits the column */
                if (!strncmp(kn, "Right ", 6) || !strncmp(kn, "Left ", 5))
                    snprintf(kshort, sizeof kshort, "%c %s", kn[0], strchr(kn, ' ') + 1);
                else snprintf(kshort, sizeof kshort, "%s", kn);
                snprintf(line, sizeof line, "%-6s %-10.10s %.8s", kButtonNames[i],
                         w ? "press key" : kshort, w ? "/button" : PadName(s_pad_btn[i]));
            } else snprintf(line, sizeof line, i == K_RESET ? "Reset defaults" : "Back");
            if (i == s_key_sel) Ink(255, 255, 255);
            else Ink(150, 150, 150);
            DrawText(24, 28 + i * 11, i == s_key_sel ? ">" : " ");
            DrawText(36, 28 + i * 11, line);
        }
        Ink(150, 150, 150);
        DrawText(24, 196, s_key_wait ? "Esc/Guide cancels" : "Enter/A: bind  Bksp: back");
    }
}

static void DrawToast(void) {
    if (!s_toast[0] || SDL_GetTicksNS() > s_toast_until) return;
    int w = (int)strlen(s_toast) * 8 + 8;
    Panel(4, 4, w, 16);
    Ink(255, 255, 255);
    DrawText(8, 8, s_toast);
}

/* ── Presenting ─────────────────────────────────────────────────────────── */
static void ApplyVideoConfig(void) { TlkVideoConfig(s_shader, s_aspect, s_smooth); }

static void ComputeViewport(int sw, int sh, int dw, int dh, SnesDisplayViewport *vp) {
    SnesDisplayAspect_ComputeViewport(sw, sh, dw, dh, SnesDisplayAspect_Clamp(s_aspect), false,
                                      s_integer != 0, vp);
}

/* Window size for the scale and aspect: height = 224 * scale. */
static int WindowWidth(void) {
    return SnesDisplayAspect_ComputeWindowWidth(kWidth, kHeight, kHeight * s_scale, SnesDisplayAspect_Clamp(s_aspect));
}
static void SizeWindow(void) {
    if (!s_fullscreen) SDL_SetWindowSize(win, WindowWidth(), kHeight * s_scale);
}

static int TurboHeld(void);

/* Shows s_pixels plus the overlay (menu when open, toast, turbo tag). */
static void Present(void) {
    memcpy(s_screen, s_pixels, sizeof s_screen);
    if (s_menu_open) DrawMenu();
    DrawToast();
    if (TurboHeld() && !s_menu_open) {
        char t[8];
        snprintf(t, sizeof t, ">>%dx", s_turbo_speed);
        Panel(kWidth - 48, 4, 44, 16);
        Ink(255, 255, 255);
        DrawText(kWidth - 44, 8, t);
    }
    if (s_gl_on) {
        uint8 *px;
        int pitch;
        s_gl.BeginDraw(kWidth, kHeight, &px, &pitch);
        for (int y = 0; y < kHeight; y++) memcpy(px + (size_t)y * pitch, s_screen + y * kWidth, kWidth * 4);
        s_gl.EndDraw();
        return;
    }
    int dw = 0, dh = 0;
    SDL_GetCurrentRenderOutputSize(ren, &dw, &dh);
    SnesDisplayViewport vp;
    ComputeViewport(kWidth, kHeight, dw, dh, &vp);
    SDL_FRect dst = { (float)vp.x, (float)vp.y, (float)vp.width, (float)vp.height };
    SDL_SetTextureScaleMode(tex, s_smooth ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
    SDL_UpdateTexture(tex, NULL, s_screen, kWidth * kBpp);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_RenderTexture(ren, tex, NULL, &dst);
    SDL_RenderPresent(ren);
}

static void SetVsync(void) {
    int on = (s_vsync && pace) ? 1 : 0;
    if (s_gl_on) {
        snesrecomp_opengl_set_vsync(on);
        if (s_gl.Reconfigure) s_gl.Reconfigure();
    } else SDL_SetRenderVSync(ren, on);
}

/* OpenGL 3.3 presenter when allowed and available, else the SDL renderer
 * (also what the dummy video driver gets, e.g. --selftest-remap). */
static int OpenVideo(void) {
    Uint32 flags = SDL_WINDOW_RESIZABLE | (s_fullscreen ? SDL_WINDOW_FULLSCREEN : 0);
    int w = WindowWidth(), h = kHeight * s_scale;
    ApplyVideoConfig();
    if (s_opengl) {
        OpenGLRenderer_Create(&s_gl);   /* sets the GL 3.3 core attributes */
        win = SDL_CreateWindow("The Lion King", w, h, flags | SDL_WINDOW_OPENGL);
        /* Probe for a 3.3 context first: the presenter exits if it gets none. */
        SDL_GLContext probe = win ? SDL_GL_CreateContext(win) : NULL;
        if (probe) {
            SDL_GL_DestroyContext(probe);
            snesrecomp_opengl_set_vsync((s_vsync && pace) ? 1 : 0);
            snesrecomp_opengl_set_viewport(ComputeViewport);
            s_gl_on = s_gl.Initialize(win);
        }
        if (!s_gl_on) {
            fprintf(stderr, "OpenGL 3.3 unavailable (%s): plain SDL renderer, no shaders\n", SDL_GetError());
            if (win) SDL_DestroyWindow(win);
            win = NULL;
        }
    }
    if (!s_gl_on) {
        SDL_GL_ResetAttributes();
        win = SDL_CreateWindow("The Lion King", w, h, flags);
        ren = win ? SDL_CreateRenderer(win, NULL) : NULL;
        if (!ren) { fprintf(stderr, "SDL window: %s\n", SDL_GetError()); return 0; }
        SDL_SetRenderVSync(ren, (s_vsync && pace) ? 1 : 0);
        /* The PPU leaves the top byte 0: an alpha format would draw every pixel transparent. */
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, kWidth, kHeight);
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_NONE);
    }
    return 1;
}

/* ── Live input ─────────────────────────────────────────────────────────── */
static uint16_t s_kbd_pad;
static SDL_Gamepad *s_pad;
static int s_pad_hold;   /* after the menu closes: ignore the pad until all of it is released */

static void OpenFirstGamepad(void) {
    if (s_pad) return;
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if (ids && n > 0) {
        s_pad = SDL_OpenGamepad(ids[0]);
        if (s_pad) {
            char msg[64];
            snprintf(msg, sizeof msg, "Gamepad: %.50s", SDL_GetGamepadName(s_pad));
            Toast(msg);
        }
    }
    SDL_free(ids);
}

static uint16_t GamepadState(void) {
    if (!s_pad) return 0;
    uint16_t w = 0;
    for (int i = 0; i < kSnesButtons; i++)
        if (SDL_GetGamepadButton(s_pad, s_pad_btn[i])) w |= (uint16_t)(1u << i);
    const int dz = 16000;
    int ax = SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_LEFTX);
    int ay = SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_LEFTY);
    if (ax < -dz) w |= 1u << 6;
    if (ax > dz) w |= 1u << 7;
    if (ay < -dz) w |= 1u << 4;
    if (ay > dz) w |= 1u << 5;
    if (SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > dz) w |= 1u << 10;
    if (SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > dz) w |= 1u << 11;
    int any = w != 0 || SDL_GetGamepadButton(s_pad, SDL_GAMEPAD_BUTTON_GUIDE);
    if (s_pad_hold) {
        if (any) return 0;
        s_pad_hold = 0;
    }
    return w;
}

/* Turbo key or pad button held (the pad waits like GamepadState after the menu). */
static int TurboHeld(void) {
    return s_turbo_held || (s_pad && !s_pad_hold && SDL_GetGamepadButton(s_pad, s_pad_btn[kTurbo]));
}

/* ── Menu actions (keyboard and gamepad share them) ─────────────────────── */
enum { A_UP, A_DOWN, A_LEFT, A_RIGHT, A_OK, A_BACK, A_CLOSE, A_QUIT };

static void OpenMenu(void) {
    s_menu_open = 1; s_menu_keys = 0; s_key_wait = 0; s_kbd_pad = 0; s_turbo_held = 0;
}
static void CloseMenu(void) {
    s_menu_open = 0; s_key_wait = 0; s_pad_hold = 1;
}

static void MenuAction(int act) {
    if (act == A_QUIT) { running = 0; return; }
    if (act == A_CLOSE) { CloseMenu(); return; }
    if (s_menu_keys) {
        if (act == A_UP) s_key_sel = (s_key_sel + K_COUNT - 1) % K_COUNT;
        else if (act == A_DOWN) s_key_sel = (s_key_sel + 1) % K_COUNT;
        else if (act == A_BACK || act == A_LEFT) s_menu_keys = 0;
        else if (act == A_OK) {
            if (s_key_sel == K_BACK) s_menu_keys = 0;
            else if (s_key_sel == K_RESET) { ResetBindings(); Toast("Controls reset (Save to keep)"); }
            else s_key_wait = 1;
        }
        return;
    }
    if (act == A_BACK) { CloseMenu(); return; }
    int dir = act == A_LEFT ? -1 : act == A_RIGHT ? 1 : 0;
    int enter = act == A_OK;
    if (act == A_UP) s_menu_sel = (s_menu_sel + M_COUNT - 1) % M_COUNT;
    else if (act == A_DOWN) s_menu_sel = (s_menu_sel + 1) % M_COUNT;
    else if (dir || enter) {
        switch (s_menu_sel) {
        case M_RESUME: if (enter) CloseMenu(); break;
        case M_SCALE:
            s_scale += dir ? dir : 1;
            if (s_scale < 1) s_scale = 8;
            if (s_scale > 8) s_scale = 1;
            SizeWindow();
            break;
        case M_FULLSCREEN: s_fullscreen = !s_fullscreen; SDL_SetWindowFullscreen(win, s_fullscreen); break;
        case M_VSYNC: s_vsync = !s_vsync; SetVsync(); break;
        case M_SHADER: {
            if (!s_gl_on) { Toast("Shaders need OpenGL 3.3"); break; }
            int sh = ShaderIndex();
            sh = sh < 0 ? 0 : (sh + (dir ? dir : 1) + kShaderCount) % kShaderCount;
            snprintf(s_shader, sizeof s_shader, "%s", kShaders[sh][0]);
            ApplyVideoConfig();
            if (s_gl.Reconfigure) s_gl.Reconfigure();
            break;
        }
        case M_ASPECT:
            s_aspect = (s_aspect + (dir ? dir : 1) + kSnesDisplayAspect_Count) % kSnesDisplayAspect_Count;
            ApplyVideoConfig();
            SizeWindow();
            break;
        case M_SMOOTH: s_smooth = !s_smooth; ApplyVideoConfig(); break;
        case M_INTEGER: s_integer = !s_integer; break;
        case M_AUDIO:
            s_audio = !s_audio;
            if (s_audio && !astream) {
                if (!SDL_WasInit(SDL_INIT_AUDIO)) SDL_InitSubSystem(SDL_INIT_AUDIO);
                SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, s_audio_rate };
                astream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
                if (astream) SDL_ResumeAudioStreamDevice(astream);
                else { fprintf(stderr, "audio: %s\n", SDL_GetError()); s_audio = 0; }
            } else if (!s_audio && astream) { SDL_DestroyAudioStream(astream); astream = NULL; }
            break;
        case M_VOLUME:
            s_volume += (dir ? dir : 1) * 10;
            if (s_volume < 0) s_volume = 0;
            if (s_volume > 100) s_volume = enter ? 0 : 100;
            break;
        case M_KEYS: if (enter) { s_menu_keys = 1; s_key_sel = 0; } break;
        case M_SAVE: if (enter) Toast(SaveConfig() ? "Settings saved" : "Cannot write ini"); break;
        case M_QUIT: if (enter) running = 0; break;
        }
    }
}

static void HandleKey(const SDL_KeyboardEvent *ke) {
    int down = ke->down;
    SDL_Keycode k = ke->key;
    if (s_menu_open) {
        if (!down) return;
        if (s_key_wait) {          /* rebinding: next key wins, Esc cancels */
            if (k != SDLK_ESCAPE) s_keys[s_key_sel] = k;
            s_key_wait = 0;
            return;
        }
        if (k == SDLK_ESCAPE) MenuAction(A_QUIT);
        else if (k == SDLK_TAB) MenuAction(A_CLOSE);
        else if (k == SDLK_UP) MenuAction(A_UP);
        else if (k == SDLK_DOWN) MenuAction(A_DOWN);
        else if (k == SDLK_LEFT) MenuAction(A_LEFT);
        else if (k == SDLK_RIGHT) MenuAction(A_RIGHT);
        else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) MenuAction(A_OK);
        else if (k == SDLK_BACKSPACE && s_menu_keys) MenuAction(A_BACK);
        return;
    }
    if (down && !ke->repeat) {
        if (k == SDLK_ESCAPE || k == SDLK_TAB) { OpenMenu(); return; }
        if (k == SDLK_F11 || (k == SDLK_RETURN && (ke->mod & SDL_KMOD_ALT))) {
            s_fullscreen = !s_fullscreen;
            SDL_SetWindowFullscreen(win, s_fullscreen);
            return;
        }
        int fn = k >= SDLK_F1 && k <= SDLK_F8 ? (int)(k - SDLK_F1) : -1;
        if (fn >= 0) {
            int load = fn >= 4 || (ke->mod & SDL_KMOD_SHIFT);
            int slot = fn % 4 + 1;
            char path[1100], msg[64];
            SlotPath(slot, path, sizeof path);
            if (load) {
                uint32_t hf = 0;
                if (tlk_state_load(path, &hf)) {
                    if (input_path && !s_polled) frame = (long)hf;
                    RecStop("a savestate was loaded (the recording replays only up to here)");
                    audio_acc = 0;
                    if (astream) SDL_ClearAudioStream(astream);
                    snprintf(msg, sizeof msg, "Loaded slot %d", slot);
                } else snprintf(msg, sizeof msg, "Slot %d: no state", slot);
            } else
                snprintf(msg, sizeof msg, tlk_state_save(path, (uint32_t)frame) ? "Saved slot %d" : "Slot %d: save failed", slot);
            Toast(msg);
            return;
        }
    }
    if (k == SDLK_RETURN && (ke->mod & SDL_KMOD_ALT)) return;
    if (k == s_keys[kTurbo]) s_turbo_held = down;
    for (int i = 0; i < kSnesButtons; i++)
        if (s_keys[i] == k) {
            if (down) s_kbd_pad |= (uint16_t)(1u << i);
            else s_kbd_pad &= (uint16_t)~(1u << i);
        }
}

/* Gamepad buttons drive the menu: Guide (or Back+Start) opens it; in it the
 * d-pad moves, South (A) selects, East (B) goes back, Start or Guide closes. */
static void HandlePadButton(const SDL_GamepadButtonEvent *ge) {
    if (!ge->down) return;
    SDL_GamepadButton b = (SDL_GamepadButton)ge->button;
    if (!s_menu_open) {
        int chord = (b == SDL_GAMEPAD_BUTTON_START && s_pad && SDL_GetGamepadButton(s_pad, SDL_GAMEPAD_BUTTON_BACK)) ||
                    (b == SDL_GAMEPAD_BUTTON_BACK && s_pad && SDL_GetGamepadButton(s_pad, SDL_GAMEPAD_BUTTON_START));
        if (b == SDL_GAMEPAD_BUTTON_GUIDE || chord) OpenMenu();
        return;
    }
    if (s_key_wait) {   /* rebinding: next gamepad button wins, Guide cancels */
        if (b != SDL_GAMEPAD_BUTTON_GUIDE) s_pad_btn[s_key_sel] = b;
        s_key_wait = 0;
        return;
    }
    switch (b) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: MenuAction(A_UP); break;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: MenuAction(A_DOWN); break;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: MenuAction(A_LEFT); break;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: MenuAction(A_RIGHT); break;
    case SDL_GAMEPAD_BUTTON_SOUTH: MenuAction(A_OK); break;
    case SDL_GAMEPAD_BUTTON_EAST: MenuAction(A_BACK); break;
    case SDL_GAMEPAD_BUTTON_START:
    case SDL_GAMEPAD_BUTTON_GUIDE: MenuAction(A_CLOSE); break;
    default: break;
    }
}

static void HandleEvent(const SDL_Event *ev) {
    switch (ev->type) {
    case SDL_EVENT_QUIT: running = 0; break;
    case SDL_EVENT_GAMEPAD_ADDED: OpenFirstGamepad(); break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (s_pad && ev->gdevice.which == SDL_GetGamepadID(s_pad)) {
            SDL_CloseGamepad(s_pad); s_pad = NULL;
            Toast("Gamepad disconnected");
            OpenFirstGamepad();
        }
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: HandlePadButton(&ev->gbutton); break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: HandleKey(&ev->key); break;
    case SDL_EVENT_WINDOW_FOCUS_LOST: s_kbd_pad = 0; s_turbo_held = 0; break;
    default: break;
    }
}

/* ── --selftest-remap: drive the menu with synthetic SDL events ─────────── */
static int s_st_fail;
static void StCheck(int ok, const char *what) {
    printf("remap_selftest: %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) s_st_fail++;
}
static void StPump(void) {
    SDL_Event ev;
    SDL_UpdateJoysticks();
    while (SDL_PollEvent(&ev)) HandleEvent(&ev);
}
static void StKey(SDL_Keycode k, int down) {
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    ev.key.key = k;
    ev.key.down = down != 0;
    SDL_PushEvent(&ev);
    StPump();
}
static void StTap(SDL_Keycode k) { StKey(k, 1); StKey(k, 0); }
static void StPadEvent(SDL_GamepadButton b) {   /* button event without a device */
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    ev.gbutton.button = (Uint8)b;
    ev.gbutton.down = true;
    SDL_PushEvent(&ev);
    StPump();
}

static int RemapSelfTest(void) {
    /* Save/reload goes to a scratch ini so the player's settings are never touched. */
    static char st_ini[512];
    const char *tmp = getenv("TMPDIR");
#ifdef _WIN32
    if (!tmp || !*tmp) tmp = getenv("TEMP");   /* no /tmp on Windows */
#endif
    snprintf(st_ini, sizeof st_ini, "%s/tlk_sdl_selftest.ini", tmp && *tmp ? tmp : "/tmp");
    s_cfg_path = st_ini;
    StCheck(!s_menu_open, "menu starts closed");
    StKey(SDLK_Z, 1);
    StCheck(s_kbd_pad == 1u, "default Z presses B");
    StKey(SDLK_Z, 0);
    StTap(SDLK_TAB);
    StCheck(s_menu_open && !s_menu_keys, "Tab opens the settings menu");
    for (int i = 0; i < M_KEYS; i++) StTap(SDLK_DOWN);
    StTap(SDLK_RETURN);
    StCheck(s_menu_keys && s_key_sel == 0, "Controls page opens on B");
    StTap(SDLK_RETURN);
    StCheck(s_key_wait, "Enter waits for a key");
    StTap(SDLK_K);
    StCheck(!s_key_wait && s_keys[0] == SDLK_K, "K is now bound to B");
    StTap(SDLK_RETURN);
    StTap(SDLK_ESCAPE);
    StCheck(!s_key_wait && s_keys[0] == SDLK_K && s_menu_open, "Esc cancels a rebind and stays in the menu");

    /* Gamepad navigation and binding with button events. */
    StPadEvent(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    StCheck(s_key_sel == 1, "pad d-pad down moves to Y");
    StPadEvent(SDL_GAMEPAD_BUTTON_SOUTH);
    StCheck(s_key_wait, "pad South starts a rebind");
    StPadEvent(SDL_GAMEPAD_BUTTON_LEFT_STICK);
    StCheck(!s_key_wait && s_pad_btn[1] == SDL_GAMEPAD_BUTTON_LEFT_STICK, "pad L3 is now bound to Y");

    /* Persist, reset, reload. */
    StCheck(SaveConfig(), "SaveConfig writes the ini");
    for (int i = 1; i < K_RESET; i++) StPadEvent(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    StCheck(s_key_sel == K_RESET, "cursor on Reset defaults");
    StPadEvent(SDL_GAMEPAD_BUTTON_SOUTH);
    StCheck(s_keys[0] == SDLK_Z && s_pad_btn[1] == SDL_GAMEPAD_BUTTON_WEST, "Reset defaults restores Z / West");
    LoadConfig(s_cfg_path);
    StCheck(s_keys[0] == SDLK_K && s_pad_btn[1] == SDL_GAMEPAD_BUTTON_LEFT_STICK, "ini reload restores K / L3");
    StPadEvent(SDL_GAMEPAD_BUTTON_EAST);
    StCheck(s_menu_open && !s_menu_keys, "pad East leaves the Controls page");
    StPadEvent(SDL_GAMEPAD_BUTTON_START);
    StCheck(!s_menu_open, "pad Start closes the menu");
    StKey(SDLK_K, 1);
    StCheck(s_kbd_pad == 1u, "K now presses B in game");
    StKey(SDLK_K, 0);
    StKey(SDLK_Z, 1);
    StCheck(s_kbd_pad == 0, "Z no longer presses B");
    StKey(SDLK_Z, 0);

    /* Turbo: a binding on the Controls page, not a pad bit. */
    StKey(SDLK_GRAVE, 1);
    StCheck(TurboHeld() && s_kbd_pad == 0, "default ` holds turbo and presses no button");
    StKey(SDLK_GRAVE, 0);
    StCheck(!TurboHeld(), "releasing ` ends turbo");
    StTap(SDLK_TAB);
    StCheck(s_menu_open && s_menu_sel == M_KEYS, "menu reopens on Controls...");
    StTap(SDLK_RETURN);
    for (int i = 0; i < 3; i++) StTap(SDLK_UP);
    StCheck(s_menu_keys && s_key_sel == kTurbo, "Up from B wraps to the turbo row");
    StTap(SDLK_RETURN);
    StTap(SDLK_T);
    StCheck(!s_key_wait && s_keys[kTurbo] == SDLK_T, "T is now bound to turbo");
    StTap(SDLK_TAB);
    StKey(SDLK_T, 1);
    StCheck(TurboHeld(), "T holds turbo in game");
    StKey(SDLK_T, 0);
    StCheck(SaveConfig(), "SaveConfig writes the turbo binding");
    ResetBindings();
    StCheck(s_keys[kTurbo] == SDLK_GRAVE && s_pad_btn[kTurbo] == SDL_GAMEPAD_BUTTON_RIGHT_STICK,
            "defaults: turbo ` / R3");
    LoadConfig(s_cfg_path);
    StCheck(s_keys[kTurbo] == SDLK_T && s_keys[0] == SDLK_K, "ini reload restores turbo T");

    /* Hotplug and live state with an SDL virtual gamepad. */
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.name = "tlk virtual pad";
    SDL_JoystickID vid = SDL_AttachVirtualJoystick(&desc);
    StCheck(vid != 0, "virtual gamepad attached");
    StPump();
    StCheck(s_pad != NULL, "hotplug event opened it");
    if (s_pad) {
        SDL_Joystick *js = SDL_GetGamepadJoystick(s_pad);
        s_pad_hold = 0;
        SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_LEFT_STICK, true);
        StPump();
        StCheck(GamepadState() == (1u << 1), "virtual L3 reads as Y");
        SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_LEFT_STICK, false);
        SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_WEST, true);
        StPump();
        StCheck(GamepadState() == 0, "virtual West (old Y) reads nothing");
        SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_WEST, false);
        SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_EAST, true);
        StPump();
        StCheck(GamepadState() == (1u << 8), "virtual East reads as A (default)");
        SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_EAST, false);
        SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_RIGHT_STICK, true);
        StPump();
        StCheck(TurboHeld() && GamepadState() == 0, "virtual R3 holds turbo and presses no button");
        SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_RIGHT_STICK, false);
        StPump();
        StCheck(!TurboHeld(), "releasing R3 ends turbo");
        SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_GUIDE, true);
        StPump();
        StCheck(s_menu_open, "virtual Guide opens the menu");
        SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_GUIDE, false);
        StPump();
        SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_START, true);
        StPump();
        StCheck(!s_menu_open, "virtual Start closes it");
        StCheck(GamepadState() == 0, "Start still held after closing is not sent to the game");
        SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_START, false);
        StPump();
        GamepadState();
        SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_START, true);
        StPump();
        StCheck(GamepadState() == (1u << 3), "after release, Start reaches the game again");
        SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_START, false);
        SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_BACK, true);
        StPump();
        SDL_SetJoystickVirtualButton(js, SDL_GAMEPAD_BUTTON_START, true);
        StPump();
        StCheck(s_menu_open, "virtual Back+Start opens the menu");
        SDL_DetachVirtualJoystick(vid);
        StPump();
        StCheck(s_pad == NULL, "unplug closes it");
    }
    ResetBindings();
    printf("remap_selftest: %s (%d failed)\n", s_st_fail ? "FAIL" : "PASS", s_st_fail);
    return s_st_fail ? 1 : 0;
}

/* ── WAV recorder ───────────────────────────────────────────────────────── */
static FILE *s_wav;
static uint32_t s_wav_frames, s_wav_limit;
static void Le32(FILE *f, uint32_t v) { uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) }; fwrite(b, 1, 4, f); }
static void Le16(FILE *f, uint16_t v) { uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; fwrite(b, 1, 2, f); }
static void WavHeader(FILE *f, int rate, uint32_t frames) {
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); Le32(f, 36 + frames * 4); fwrite("WAVEfmt ", 1, 8, f);
    Le32(f, 16); Le16(f, 1); Le16(f, 2); Le32(f, (uint32_t)rate); Le32(f, (uint32_t)rate * 4);
    Le16(f, 4); Le16(f, 16); fwrite("data", 1, 4, f); Le32(f, frames * 4);
}

static void Usage(void) {
    fprintf(stderr,
        "usage: tlk_sdl --rom <path.sfc> [--config ini] [--scale N] [--fullscreen]\n"
        "               [--input tape | --input-polled tape | --input-raw rec] [--no-record] [--lle] [--interp-targets f]\n"
        "               [--frames N] [--hash-at a,b,..] [--dump-png path] [--no-pace]\n"
        "               [--no-audio] [--wav path [--wav-seconds S]]\n"
        "               [--save-at N state] [--load state] [--load-at N state]\n"
        "               [--menu-at N [--menu-page controls]] [--selftest-remap]\n"
        "               [--list-drivers]\n"
        "Without --rom (or rom= in the ini) the .sfc/.smc files next to the binary and in the\n"
        "current directory are tried. Needs The Lion King (USA), CRC32 C8FBFAA8.\n");
}

/* No --rom: try every .sfc/.smc next to the binary, then in the current
 * directory, and take the first that verifies. Prints what was rejected. */
static uint8_t *FindRom(size_t *size, char *found, size_t found_n) {
    const char *base = SDL_GetBasePath();
    char *cwd = SDL_GetCurrentDirectory();
    const char *dirs[2] = { base ? base : "./", "./" };
    int same = base && cwd && !strcmp(base, cwd);   /* both end in a separator */
    SDL_free(cwd);
    int tried = 0;
    for (int d = 0; d < 2; d++) {
        if (d == 1 && (same || !base)) break;
        for (int pat = 0; pat < 2; pat++) {
            int n = 0;
            char **names = SDL_GlobDirectory(dirs[d], pat ? "*.smc" : "*.sfc", SDL_GLOB_CASEINSENSITIVE, &n);
            for (int i = 0; names && i < n; i++) {
                char path[2048], err[256];
                snprintf(path, sizeof path, "%s%s%s", dirs[d],
                         LastSep(dirs[d]) == dirs[d] + strlen(dirs[d]) - 1 ? "" : "/", names[i]);
                tried++;
                uint8_t *rom = TlkReadVerifiedRom(path, size, err, sizeof err);
                if (rom) {
                    snprintf(found, found_n, "%s", path);
                    SDL_free(names);
                    return rom;
                }
                uint32_t crc = 0;
                FILE *f = fopen(path, "rb");
                if (f) {
                    uint8_t buf[65536];
                    size_t r;
                    crc = 0xffffffffu;
                    while ((r = fread(buf, 1, sizeof buf, f)) > 0) crc = CrcUpdate(crc, buf, r);
                    crc ^= 0xffffffffu;
                    fclose(f);
                }
                fprintf(stderr, "skipping %s: CRC32 %08X, not The Lion King (USA)\n", path, crc);
            }
            SDL_free(names);
        }
    }
    fprintf(stderr,
        "No ROM given and no matching ROM found (%d candidate%s checked) next to the binary\n"
        "(%s) or in the current directory.\n"
        "Run: tlk_sdl --rom \"/path/to/Lion King, The (USA).sfc\"\n"
        "The ROM is not included. It must be The Lion King (USA), CRC32 C8FBFAA8\n"
        "(headered copies are accepted).\n", tried, tried == 1 ? "" : "s", dirs[0]);
    return NULL;
}

/* No ROM found: ask with the system file dialog (portal/zenity on Linux,
 * native on Windows and macOS), verify it, and remember it as rom= in the
 * ini. A wrong file offers another try. Returns NULL on cancel or when no
 * dialog is available (headless, no portal). */
typedef struct { SDL_AtomicInt done; char path[2048]; } PickState;

static void SDLCALL PickDone(void *ud, const char *const *files, int filter) {
    PickState *st = ud;
    (void)filter;
    st->path[0] = 0;
    if (!files) fprintf(stderr, "file dialog: %s\n", SDL_GetError());
    else if (files[0]) snprintf(st->path, sizeof st->path, "%s", files[0]);
    SDL_SetAtomicInt(&st->done, 1);
}

static uint8_t *PickRom(size_t *size, char *found, size_t found_n) {
    static const SDL_DialogFileFilter kFilters[] = {
        { "SNES ROM (*.sfc, *.smc)", "sfc;smc" }, { "All files", "*" } };
    const char *msg =
        "The game data is not included. Choose your own copy of\n"
        "The Lion King (USA) ROM (.sfc or .smc).";
    for (;;) {
        const SDL_MessageBoxButtonData pick[] = {
            { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Choose ROM..." },
            { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Quit" } };
        const SDL_MessageBoxData box = { SDL_MESSAGEBOX_INFORMATION, NULL, "The Lion King",
                                         msg, 2, pick, NULL };
        int button = 1;
        if (SDL_ShowMessageBox(&box, &button) && button != 1) return NULL;
        PickState st;
        SDL_SetAtomicInt(&st.done, 0);
        SDL_ShowOpenFileDialog(PickDone, &st, NULL, kFilters, 2, NULL, false);
        while (!SDL_GetAtomicInt(&st.done)) {
            SDL_PumpEvents();
            SDL_Delay(10);
        }
        if (!st.path[0]) return NULL;
        char err[256];
        uint8_t *rom = TlkReadVerifiedRom(st.path, size, err, sizeof err);
        if (rom) {
            snprintf(found, found_n, "%s", st.path);
            return rom;
        }
        fprintf(stderr, "%s: %s\n", err, st.path);
        msg = "That file is not The Lion King (USA) ROM (CRC32 C8FBFAA8).\n"
              "Other regions, revisions and hacks are not supported.";
    }
}

static void PrintHash(const uint8_t *data, size_t n) {
    uint8_t h[32];
    sha256_compute(data, n, h);
    for (int i = 0; i < 32; i++) printf("%02x", h[i]);
}

int main(int argc, char **argv) {
    const char *rom_path = NULL, *dump_png = NULL, *wav_path = NULL;
    const char *hash_at = NULL;
    const char *cfg_path = getenv("TLK_SDL_INI") ? getenv("TLK_SDL_INI") : "tlk_sdl.ini";
    const char *save_path = NULL, *load_path = NULL, *load_at_path = NULL;
    long frames = 0, save_at = -1, load_at = -1, menu_at = -1;
    int cli_scale = 0, cli_fs = 0, no_audio = 0, no_record = 0;
    const char *record_path = NULL;
    double wav_seconds = 10.0;

    int selftest = 0, menu_page = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--list-drivers")) {
            int v = SDL_GetVersion();
            printf("SDL %d.%d.%d (%s)\nvideo:", SDL_VERSIONNUM_MAJOR(v),
                   SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v), SDL_GetRevision());
            for (int d = 0; d < SDL_GetNumVideoDrivers(); d++) printf(" %s", SDL_GetVideoDriver(d));
            printf("\naudio:");
            for (int d = 0; d < SDL_GetNumAudioDrivers(); d++) printf(" %s", SDL_GetAudioDriver(d));
            printf("\n");
            return 0;
        }
    ResetBindings();
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--config") && i + 1 < argc) cfg_path = argv[i + 1];
    LoadConfig(cfg_path);
    s_cfg_path = cfg_path;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--rom") && v) { rom_path = v; i++; }
        else if (!strcmp(a, "--config") && v) i++;
        else if (!strcmp(a, "--input") && v) { input_path = v; i++; }
        else if (!strcmp(a, "--input-polled") && v) { input_path = v; s_polled = 1; i++; }
        else if (!strcmp(a, "--input-raw") && v) { input_path = v; s_raw_tape = 1; i++; }
        else if (!strcmp(a, "--no-record")) no_record = 1;
        else if (!strcmp(a, "--record") && v) { record_path = v; i++; }
        else if (!strcmp(a, "--lle")) setenv("SNESRECOMP_LLE_BOUNCE", "0", 1);
        else if (!strcmp(a, "--interp-targets") && v) { setenv("SNESRECOMP_LLE_INTERP_TARGET_FILE", v, 1); i++; }
        else if (!strcmp(a, "--frames") && v) { frames = strtol(v, NULL, 10); i++; }
        else if (!strcmp(a, "--hash-at") && v) { hash_at = v; i++; }
        else if (!strcmp(a, "--dump-png") && v) { dump_png = v; i++; }
        else if (!strcmp(a, "--wav") && v) { wav_path = v; i++; }
        else if (!strcmp(a, "--wav-seconds") && v) { wav_seconds = atof(v); i++; }
        else if (!strcmp(a, "--scale") && v) { cli_scale = atoi(v); i++; }
        else if (!strcmp(a, "--fullscreen")) cli_fs = 1;
        else if (!strcmp(a, "--no-pace")) pace = 0;
        else if (!strcmp(a, "--no-audio")) no_audio = 1;
        else if (!strcmp(a, "--save-at") && v && i + 2 < argc) { save_at = atol(v); save_path = argv[i + 2]; i += 2; }
        else if (!strcmp(a, "--load-at") && v && i + 2 < argc) { load_at = atol(v); load_at_path = argv[i + 2]; i += 2; }
        else if (!strcmp(a, "--load") && v) { load_path = v; i++; }
        else if (!strcmp(a, "--menu-at") && v) { menu_at = atol(v); i++; }
        else if (!strcmp(a, "--menu-page") && v) { menu_page = !strcmp(v, "controls") || !strcmp(v, "keys"); i++; }
        else if (!strcmp(a, "--selftest-remap")) selftest = 1;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { Usage(); return 0; }
        else { Usage(); return 2; }
    }
    if (!rom_path && s_rom_cfg[0]) rom_path = s_rom_cfg;
    if (cli_scale > 0) s_scale = cli_scale;
    if (s_scale < 1 || s_scale > 16) s_scale = 3;
    if (cli_fs) s_fullscreen = 1;
    if (no_audio) s_audio = 0;
    if (s_volume < 0) s_volume = 0;
    if (s_volume > 100) s_volume = 100;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | (s_audio ? SDL_INIT_AUDIO : 0))) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 3;
    }

    size_t rom_size = 0;
    char err[256], found[2048];
    CrcInit();
    uint8_t *rom = NULL;
    /* The picker is for players: a scripted run (--frames, a tape, the self
     * test) or an explicit --rom fails with a message instead. */
    int interactive = !frames && !input_path && !selftest;
    if (rom_path) {
        rom = TlkReadVerifiedRom(rom_path, &rom_size, err, sizeof err);
        if (!rom) {
            fprintf(stderr, "%s: %s\nNeeds The Lion King (USA), CRC32 C8FBFAA8.\n", err, rom_path);
            if (rom_path != s_rom_cfg || !interactive) return 2;
        }
    }
    if (!rom) {
        rom = FindRom(&rom_size, found, sizeof found);
        if (!rom && interactive) {
            rom = PickRom(&rom_size, found, sizeof found);
            if (rom) {
                snprintf(s_rom_cfg, sizeof s_rom_cfg, "%s", found);
                if (SaveConfig()) fprintf(stderr, "rom saved to %s\n", s_cfg_path);
            }
        }
        if (!rom) return 2;
        fprintf(stderr, "rom: %s\n", found);
    }
    if (input_path) {
        if (!LoadTape(input_path)) { fprintf(stderr, "cannot load tape %s\n", input_path); return 2; }
        fprintf(stderr, "input: %zu %s from %s\n", s_tape_frames, s_polled ? "polls" : "frames", input_path);
    }
    if (!OpenVideo()) return 3;

    astream = NULL;
    /* The output rate is fixed for the session so turning audio on/off in the
     * menu does not change emulation (RtlRenderAudio runs either way below). */
    RtlSetAudioOutputRate(s_audio_rate);
    if (s_audio) {
        SDL_AudioSpec spec = { SDL_AUDIO_S16, 2, s_audio_rate };
        astream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (!astream) fprintf(stderr, "audio: %s (continuing without sound)\n", SDL_GetError());
        else SDL_ResumeAudioStreamDevice(astream);
    }
    if (wav_path) {
        s_wav = fopen(wav_path, "wb");
        if (!s_wav) { fprintf(stderr, "cannot write %s\n", wav_path); return 7; }
        WavHeader(s_wav, s_audio_rate, 0);
        s_wav_limit = (uint32_t)(wav_seconds * s_audio_rate);
    }
    OpenFirstGamepad();
    if (selftest) {
        int rc = RemapSelfTest();
        SDL_Quit();
        free(rom);
        return rc;
    }

    RtlRegisterGame(&kTlkGameInfo);
    if (!SnesInit(rom, (int)rom_size)) { fprintf(stderr, "snesrecomp rejected the ROM\n"); return 4; }
    if (s_polled) {
        interp_bridge_add_pre_opcode_hook(0xC0A07C, PollHook);
        interp_bridge_add_pre_opcode_hook(0x00A07C, PollHook);
        g_tlk_aot_pad_read_hook = PollHookCompiled; /* task CC: compiled $C0A05D / $C0A073 */
    }

    frame = 0;
    if (load_path) {
        uint32_t hf = 0;
        if (!tlk_state_load(load_path, &hf)) { fprintf(stderr, "cannot load state %s\n", load_path); return 8; }
        frame = (long)hf;
        printf("loaded_state=%s at_frame=%ld\n", load_path, frame);
    }
    /* --frames runs are scripted proof runs: they must not clobber a player's recording. */
    if (record_path || (!input_path && !load_path && !no_record && frames == 0)) {
        char rec[1100];
        const char *sl = LastSep(s_cfg_path);
        if (record_path) snprintf(rec, sizeof rec, "%s", record_path);
        else if (sl) snprintf(rec, sizeof rec, "%.*s/last_session.rec", (int)(sl - s_cfg_path), s_cfg_path);
        else snprintf(rec, sizeof rec, "last_session.rec");
        s_rec = fopen(rec, "wb");
        if (s_rec) fprintf(stderr, "recording input to %s (replay: --input-raw %s)\n", rec, rec);
    }

    const double audio_per_frame = s_audio_rate / NTSC_FPS;
    const int audio_target = (int)(audio_per_frame * 3);  /* ~50 ms device queue */
    audio_acc = 0;
    int16_t abuf[4096 * 2];
    unsigned long underruns = 0, late_resyncs = 0;

    const uint64_t period_ns = (uint64_t)(1e9 / NTSC_FPS + 0.5);
    uint64_t t_start = SDL_GetTicksNS(), deadline = t_start;
    long frame_start = frame;
    running = 1;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) HandleEvent(&ev);
        if (!running) break;

        if (s_menu_open) {   /* paused: keep presenting the last frame plus the overlay */
            Present();
            SDL_Delay(16);
            deadline = SDL_GetTicksNS();
            continue;
        }
        uint32_t pad = input_path ? (s_polled ? TapePolled() : TapeFrame(frame))
                                  : (uint32_t)(s_kbd_pad | GamepadState());
        if (s_rec) {
            uint8_t b[2] = { (uint8_t)pad, (uint8_t)(pad >> 8) };
            fwrite(b, 1, 2, s_rec);
            if (frame % 60 == 0) fflush(s_rec);  /* keep it usable after a crash */
        }
        RtlRunFrame(pad);
        PpuBeginDrawing(g_ppu, s_pixels, kWidth * kBpp, kPpuRenderFlags_NewRenderer);
        kTlkGameInfo.draw_ppu_frame();
        frame++;
        if (g_fail) { fprintf(stderr, "runtime failure (g_fail) at frame %ld\n", frame); break; }
        if (save_path && frame == save_at) {
            if (!tlk_state_save(save_path, (uint32_t)frame)) { fprintf(stderr, "cannot save state %s\n", save_path); return 8; }
            printf("saved_state=%s at_frame=%ld\n", save_path, frame);
        }
        if (load_at_path && frame == load_at) {
            uint32_t hf = 0;
            if (!tlk_state_load(load_at_path, &hf)) { fprintf(stderr, "cannot load state %s\n", load_at_path); return 8; }
            printf("loaded_state=%s at_frame=%ld -> %u\n", load_at_path, frame, hf);
            frame = (long)hf;
            load_at_path = NULL;   /* once */
            Toast("Loaded state");
        }
        if (menu_at >= 0 && frame == menu_at) {
            OpenMenu();
            if (menu_page) { s_menu_sel = M_KEYS; s_menu_keys = 1; s_key_sel = 0; }
            menu_at = -1;
        }

        if (hash_at) {
            /* list built once, any length (a fixed 1024-byte buffer silently dropped frames past ~2260). */
            static char *list;
            if (!list) {
                size_t n = strlen(hash_at) + 3;
                list = malloc(n);
                if (list) snprintf(list, n, ",%s,", hash_at);
            }
            char key[32];
            snprintf(key, sizeof key, ",%ld,", frame);
            if (list && strstr(list, key)) {
                printf("frame=%ld frame_sha256=", frame); PrintHash(s_pixels, sizeof s_pixels);
                printf(" wram_sha256="); PrintHash(g_ram, 0x20000); printf("\n");
                fflush(stdout);
            }
        }

        /* Turbo: only every s_turbo_speed-th frame is shown and paced, and the
         * sound is muted (still rendered, so the resampler keeps up). */
        int turbo = TurboHeld() ? s_turbo_speed : 1;
        int shown = frame % turbo == 0;

        /* Audio: one field's worth, trimmed to hold the device queue. */
        if (astream || s_wav) {
            double want = audio_per_frame;
            if (astream) {
                int queued = SDL_GetAudioStreamQueued(astream) / 4;
                if (queued == 0 && frame > 10 && pace && turbo == 1) underruns++;
                double e = (double)(audio_target - queued) / audio_target;
                if (e > 1) e = 1;
                if (e < -1) e = -1;
                want *= 1.0 + 0.005 * e;
            }
            audio_acc += want;
            int n = (int)audio_acc;
            audio_acc -= n;
            while (n > 0) {
                int c = n > 4096 ? 4096 : n;
                RtlRenderAudio(abuf, c, 2);
                if (s_volume < 100)
                    for (int j = 0; j < c * 2; j++) abuf[j] = (int16_t)(abuf[j] * s_volume / 100);
                if (astream && pace && turbo == 1) SDL_PutAudioStreamData(astream, abuf, c * 4);
                if (s_wav && s_wav_frames < s_wav_limit) {
                    uint32_t w = (uint32_t)c;
                    if (w > s_wav_limit - s_wav_frames) w = s_wav_limit - s_wav_frames;
                    fwrite(abuf, 4, w, s_wav);
                    s_wav_frames += w;
                }
                n -= c;
            }
        }

        if (shown) Present();

        if (frames > 0 && frame >= frames) break;

        if (pace && !s_vsync && shown) {
            deadline += period_ns;
            uint64_t now = SDL_GetTicksNS();
            if (now < deadline) SDL_DelayPrecise(deadline - now);
            else if (now - deadline > 5 * period_ns) { deadline = now; late_resyncs++; }
        }
    }
    RecStop("session ended");
    double wall = (double)(SDL_GetTicksNS() - t_start) / 1e9;

    printf("frames=%ld wall_s=%.3f fps=%.4f audio_underruns=%lu late_resyncs=%lu\n",
           frame, wall, wall > 0 ? (frame - frame_start) / wall : 0, underruns, late_resyncs);
    if (s_polled) printf("tape_words_used=%zu/%zu\n", s_poll_idx, s_tape_frames);
    printf("frame_sha256="); PrintHash(s_pixels, sizeof s_pixels);
    printf("\nwram_sha256="); PrintHash(g_ram, 0x20000); printf("\n");
    int rc = g_fail ? 6 : 0;
    if (dump_png) {
        if (WritePng(dump_png, s_pixels, kWidth, kHeight, kWidth * kBpp)) printf("dump_png=%s\n", dump_png);
        else rc = 7;
    }
    if (s_wav) {
        WavHeader(s_wav, s_audio_rate, s_wav_frames);
        fclose(s_wav);
        printf("wav=%s frames=%u rate=%d\n", wav_path, s_wav_frames, s_audio_rate);
    }
    if (astream) SDL_DestroyAudioStream(astream);
    if (s_pad) SDL_CloseGamepad(s_pad);
    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    free(rom);
    free(s_tape);
    return rc;
}
