/* headless_main.c -- The Lion King windowless validation host.
 *
 * usage: tlk_headless --rom <path.sfc> [--frames N] [--dump-png path]
 *                     [--dump-every K] [--input tape] [--log path]
 *                     [--interp-targets file] [--lle]
 *
 * Boots the verified ROM through the recompiled code (interpreter tier as
 * fallback), runs N frames on the beam-aligned driver, writes the last
 * frame as a PNG, and writes a coverage log: interpreted vs. compiled
 * execution, unresolved dispatch abandons, and the tier-2 gap table.
 *
 * --input: raw per-frame little-endian u16 joypad-1 words in $4218 layout
 * (bit 15 = B, Y, Select, Start, Up, Down, Left, Right, A, X, L, bit 4 = R).
 * Frames past the end of the tape get no buttons.
 *
 * --input-polled: same word format, but one word per game POLL instead of per
 * frame (tools/bk2_to_tape.py --polled --lag, i.e. the movie with BizHawk lag
 * frames removed). A poll is one execution of the game's only pad read,
 * `LDA $4218` at $C0A07C (called once per game-loop iteration; lag frames never
 * reach it). The host arms an interpreter pre-opcode hook there that loads the
 * next tape word into the pad state just before the read. Needs the read to run
 * in the interpreter (--lle); polls_seen in the summary shows it fired.
 * --ram-log path: one line per frame "f lag rng hp x y lvl lives pad" (same
 * format as tools/bizhawk/ram_log.lua); lag = no poll during that frame.
 *
 * --interp-targets: SNESRECOMP_LLE_INTERP_TARGET_FILE, a list of PC24s
 * whose compiled bodies are refused (run in the interpreter instead);
 * runner/tlk_interp_targets.txt lists the ones known to diverge.
 * --lle: SNESRECOMP_LLE_BOUNCE=0, never bounce from the interpreter into
 * compiled code (interrupt entries still use compiled bodies).
 *
 * --save-at N path: write a savestate after frame N (runner/tlk_savestate.c).
 * --load path: start from a savestate; the frame count (and tape position of a
 * per-frame tape) continues from the saved frame, --frames is the end frame.
 *
 * --dump-every K: also write <dump-png stem>_NNNNNN.png every K frames.
 *
 * Coverage capture (snesrecomp/docs/COVERAGE_FEEDBACK.md) is enabled when
 * --log is given; the tier-2 manifest goes to <log>.tier2.json. */
#include "tlk_game.h"
#include "verified_rom.h"
#include "tlk_savestate.h"

#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "cpu_state.h"
#include "sha256.h"
#include "snes/interp_bridge.h"
#include "snes/joypad.h"
#include "snes/ppu.h"
#include "snes/snes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tlk_compat.h"

enum { kWidth = 256, kHeight = 224, kBpp = 4 };
static uint8_t s_pixels[kWidth * kHeight * kBpp];

/* ── PNG writer (stored deflate, no dependencies) ───────────────────────── */

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
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static int WriteChunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
    uint8_t hdr[8];
    Be32(hdr, len);
    memcpy(hdr + 4, type, 4);
    uint32_t crc = CrcUpdate(0xffffffffu, hdr + 4, 4);
    crc = CrcUpdate(crc, data, len) ^ 0xffffffffu;
    uint8_t tail[4];
    Be32(tail, crc);
    return fwrite(hdr, 1, 8, f) == 8 &&
           (len == 0 || fwrite(data, 1, len, f) == len) &&
           fwrite(tail, 1, 4, f) == 4;
}

/* px is BGRA (the PPU's native output). */
static int WritePng(const char *path, const uint8_t *px, int w, int h, int pitch) {
    const size_t row = (size_t)w * 3 + 1;
    const size_t raw_len = row * (size_t)h;
    uint8_t *raw = (uint8_t *)malloc(raw_len);
    if (!raw) return 0;
    for (int y = 0; y < h; y++) {
        uint8_t *d = raw + row * (size_t)y;
        const uint8_t *s = px + (size_t)pitch * (size_t)y;
        *d++ = 0; /* filter: none */
        for (int x = 0; x < w; x++) {
            d[0] = s[x * 4 + 2]; d[1] = s[x * 4 + 1]; d[2] = s[x * 4 + 0];
            d += 3;
        }
    }
    const size_t nblocks = (raw_len + 65534) / 65535;
    const size_t z_len = 2 + raw_len + nblocks * 5 + 4;
    uint8_t *z = (uint8_t *)malloc(z_len);
    if (!z) { free(raw); return 0; }
    size_t o = 0;
    z[o++] = 0x78; z[o++] = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_len; i++) { a = (a + raw[i]) % 65521u; b = (b + a) % 65521u; }
    for (size_t off = 0; off < raw_len; off += 65535) {
        size_t n = raw_len - off < 65535 ? raw_len - off : 65535;
        z[o++] = (uint8_t)(off + n == raw_len);
        z[o++] = (uint8_t)n; z[o++] = (uint8_t)(n >> 8);
        z[o++] = (uint8_t)~n; z[o++] = (uint8_t)(~n >> 8);
        memcpy(z + o, raw + off, n);
        o += n;
    }
    Be32(z + o, (b << 16) | a);
    o += 4;
    free(raw);

    FILE *f = fopen(path, "wb");
    if (!f) { free(z); return 0; }
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    uint8_t ihdr[13];
    Be32(ihdr, (uint32_t)w); Be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8; ihdr[9] = 2; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    int ok = fwrite(sig, 1, 8, f) == 8 &&
             WriteChunk(f, "IHDR", ihdr, 13) &&
             WriteChunk(f, "IDAT", z, (uint32_t)o) &&
             WriteChunk(f, "IEND", NULL, 0);
    free(z);
    if (fclose(f) != 0) ok = 0;
    return ok;
}

/* ── Input tape ─────────────────────────────────────────────────────────── */

static uint16_t *s_tape;
static size_t s_tape_frames;

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

/* RtlRunFrame wants the runtime's 12-bit pad word: bit 0 = B .. bit 11 = R,
 * the bit-reverse of the $4218 word. */
static uint32_t InputForFrame(long frame) {
    if (!s_tape || frame < 0 || (size_t)frame >= s_tape_frames) return 0;
    return joypad_auto_read_word(s_tape[frame]) & 0x0fffu;
}

/* Poll-aligned tape (--input-polled). */
static int s_polled;
static size_t s_poll_idx;      /* next unconsumed tape word */
static unsigned long s_polls_frame, s_polls_total;

static uint16_t PolledPadState(void) {
    if (!s_tape || s_poll_idx >= s_tape_frames) return 0;
    /* No up+down / left+right filter here (RtlRunFrame applies one): the TASes press
     * Left+Right together (6987M poll 930 reads $0300), and the hook below writes the pad
     * state after RtlRunFrame's filter, so the game sees the tape word unmodified. */
    return joypad_auto_read_word(s_tape[s_poll_idx]) & 0x0fffu;
}

static void PollHook(CpuState *cpu, uint32_t pc24) {
    (void)cpu; (void)pc24;
    if (getenv("TLK_POLL_TRACE"))
        fprintf(stderr, "poll %lu v=%u h=%u ajt=%u aj=%d idx=%u\n", s_polls_total + 1,
                (unsigned)g_snes->vPos, (unsigned)g_snes->hPos, (unsigned)g_snes->autoJoyTimer,
                (int)g_snes->autoJoyRead, (unsigned)g_snes->joypad1Index);
    if (getenv("TLK_POLL_TRACE")) g_snes->joypad1Index = 0;
    if (s_polled) {
        uint16_t w = PolledPadState();
        g_snes->input1_currentState = w;
        joypad_set_pad(0, w);
        if (s_poll_idx < s_tape_frames) s_poll_idx++;
    }
    s_polls_frame++;
    s_polls_total++;
}
/* Task CC: the same poll when the pad read runs compiled (cpu_state.hunks aot-pad-read-hook). */
extern void (*g_tlk_aot_pad_read_hook)(void);
static void PollHookCompiled(void) { PollHook(&g_cpu, 0xC0A07C); }

/* ── Coverage hooks: interpreted opcodes vs compiled entries ────────────── */

static unsigned long long s_interp_ops, s_compiled_entries;
static uint8_t *s_interp_seen;   /* one byte per 24-bit PC */
static uint8_t *s_compiled_seen;
static unsigned long s_interp_unique, s_compiled_unique;
static unsigned long s_interp_bank_unique[256];

static uint32_t *s_interp_hist;
static uint8_t *s_mx_hist;
void TlkRomMapInit(const uint8_t *rom, size_t size);
void TlkRomMapOpcode(uint32_t pc, int m, int x);
void TlkRomMapWrite(void);
void TlkCallPairInit(const uint8_t *rom, size_t size);
void TlkCallPairOpcode(uint32_t pc, int m, int x);
void TlkCallPairWrite(void);

static void PcHook(uint32_t pc24, int m, int x) {
    pc24 &= 0xffffff;
    s_interp_ops++;
    { /* Diagnostics: TLK_MX_HIST=<path> writes, per interpreted PC, the set of (m,x) widths seen
       * (bit 2m+x). Evidence for exit widths: the widths at a call's return address. */
        static int on = -1;
        if (on < 0) { on = getenv("TLK_MX_HIST") != NULL; if (on) s_mx_hist = (uint8_t *)calloc(1u << 24, 1); }
        if (s_mx_hist) s_mx_hist[pc24] |= (uint8_t)(1u << ((m ? 2 : 0) + (x ? 1 : 0)));
    }
    TlkRomMapOpcode(pc24, m, x); /* TLK_ROMMAP diagnostics (runner/tlk_rommap.c) */
    TlkCallPairOpcode(pc24, m, x); /* TLK_CALL_PAIR diagnostics (runner/tlk_callpair.c) */
    { /* Diagnostics: TLK_INTERP_HIST=<path> writes per-PC interpreted opcode counts at exit. */
        static int on = -1;
        if (on < 0) { on = getenv("TLK_INTERP_HIST") != NULL; if (on) s_interp_hist = (uint32_t *)calloc(1u << 24, 4); }
        if (s_interp_hist) s_interp_hist[pc24]++;
    }
    { /* Diagnostics: TLK_TRAP_PC=<pc24> [TLK_TRAP_FROM=<frame>] dumps the interpreter's recent steps,
       * control transfers and resume points the first time that PC is interpreted. */
        static int trap_init, trap_from; static uint32_t trap_pc; static int trapped;
        if (!trap_init) { const char *e = getenv("TLK_TRAP_PC"); trap_init = 1; trap_pc = e ? (uint32_t)strtoul(e, NULL, 16) : 0xFFFFFFFFu;
                          e = getenv("TLK_TRAP_FROM"); trap_from = e ? atoi(e) : 0; }
        if (pc24 == trap_pc && !trapped && snes_frame_counter >= trap_from) {
            extern void interp_bridge_dump_recent_steps(int, FILE *);
            extern void interp_bridge_dump_recent_edges(int, FILE *);
            extern void interp_bridge_dump_resume_ring(int, FILE *);
            trapped = 1;
            fprintf(stderr, "[trap-pc] %06X frame=%d cpu(A=%04X X=%04X Y=%04X S=%04X D=%04X DB=%02X P=%02X m=%d x=%d master=%llu) stack[S+1..S+4]=%02X %02X %02X %02X\n",
                    pc24, snes_frame_counter, g_cpu.A, g_cpu.X, g_cpu.Y, g_cpu.S, g_cpu.D, g_cpu.DB, g_cpu.P,
                    m, x, (unsigned long long)g_cpu.master_cycles,
                    g_ram[(g_cpu.S + 1) & 0xFFFF], g_ram[(g_cpu.S + 2) & 0xFFFF], g_ram[(g_cpu.S + 3) & 0xFFFF], g_ram[(g_cpu.S + 4) & 0xFFFF]);
            interp_bridge_dump_recent_steps(64, stderr);
            interp_bridge_dump_recent_edges(64, stderr);
            interp_bridge_dump_resume_ring(16, stderr);
        }
    }
    { /* Diagnostics: TLK_WATCH=<wram offset> prints the PC of each opcode that changed it. */
        static int watch = -2; static uint8_t last; static uint32_t prev_pc;
        if (watch == -2) { const char *e = getenv("TLK_WATCH"); watch = e ? (int)strtol(e, NULL, 0) : -1; if (watch >= 0) last = g_ram[watch]; }
        if (watch >= 0) {
            if (g_ram[watch] != last) {
                fprintf(stderr, "watch $%05X %02X->%02X by pc=%06X frame=%d\n", watch, last, g_ram[watch], prev_pc, snes_frame_counter);
                last = g_ram[watch];
            }
            prev_pc = pc24;
        }
    }
    { /* Diagnostics: TLK_TRACE_OPCODES="from,to,path" logs "pc24 master_cycles" for every interpreted
       * opcode from poll <from> up to poll <to> (pairs with tools/bizhawk/trace_opcodes.lua). */
        static int init; static unsigned long from, to; static FILE *tf;
        if (!init) {
            const char *e = getenv("TLK_TRACE_OPCODES"); init = 1;
            if (e) { char path[512] = {0}; if (sscanf(e, "%lu,%lu,%511s", &from, &to, path) == 3) tf = fopen(path, "w"); }
        }
        if (tf && s_polls_total >= from && s_polls_total < to)
            fprintf(tf, "%06X %llu\n", pc24, (unsigned long long)g_cpu.master_cycles);
        if (tf && s_polls_total >= to) { fclose(tf); tf = NULL; }
    }
    if (!s_interp_seen[pc24]) {
        s_interp_seen[pc24] = 1;
        s_interp_unique++;
        s_interp_bank_unique[pc24 >> 16]++;
    }
}

static void BounceHook(uint32_t pc24, int m, int x) {
    (void)m; (void)x;
    pc24 &= 0xffffff;
    s_compiled_entries++;
    if (!s_compiled_seen[pc24]) { s_compiled_seen[pc24] = 1; s_compiled_unique++; }
}

static void PrintHash(FILE *f, const uint8_t h[32]) {
    for (int i = 0; i < 32; i++) fprintf(f, "%02x", h[i]);
}

static void StemPath(char *out, size_t n, const char *png, long frame) {
    const char *dot = strrchr(png, '.');
    int stem = dot ? (int)(dot - png) : (int)strlen(png);
    snprintf(out, n, "%.*s_%06ld.png", stem, png, frame);
}

static void Usage(void) {
    fprintf(stderr,
        "usage: tlk_headless --rom <path.sfc> [--frames N] [--dump-png path]\n"
        "                    [--dump-every K] [--input tape | --input-polled tape]\n"
        "                    [--ram-log path] [--log path]\n"
        "                    [--interp-targets file] [--lle]\n"
        "                    [--save-at N state] [--load state]\n");
}

int main(int argc, char **argv) {
    const char *rom_path = NULL, *dump_png = NULL, *input_path = NULL, *log_path = NULL;
    const char *ram_log_path = NULL;
    long frames = 600, dump_every = 0, save_at = -1;
    const char *save_path = NULL, *load_path = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--rom") && v) { rom_path = v; i++; }
        else if (!strcmp(a, "--frames") && v) { frames = strtol(v, NULL, 10); i++; }
        else if (!strcmp(a, "--dump-png") && v) { dump_png = v; i++; }
        else if (!strcmp(a, "--dump-every") && v) { dump_every = strtol(v, NULL, 10); i++; }
        else if (!strcmp(a, "--input") && v) { input_path = v; i++; }
        else if (!strcmp(a, "--input-polled") && v) { input_path = v; s_polled = 1; i++; }
        else if (!strcmp(a, "--ram-log") && v) { ram_log_path = v; i++; }
        else if (!strcmp(a, "--interp-targets") && v) { setenv("SNESRECOMP_LLE_INTERP_TARGET_FILE", v, 1); i++; }
        else if (!strcmp(a, "--save-at") && v && i + 2 < argc) { save_at = strtol(v, NULL, 10); save_path = argv[i + 2]; i += 2; }
        else if (!strcmp(a, "--load") && v) { load_path = v; i++; }
        else if (!strcmp(a, "--lle")) setenv("SNESRECOMP_LLE_BOUNCE", "0", 1);
        else if ((!strcmp(a, "--log") || !strcmp(a, "--coverage-log")) && v) { log_path = v; i++; }
        else { Usage(); return 2; }
    }
    if (!rom_path) { Usage(); return 2; }
    if (frames < 1 || frames > 10000000) { fprintf(stderr, "bad --frames\n"); return 2; }

    size_t rom_size = 0;
    char err[256];
    uint8_t *rom = TlkReadVerifiedRom(rom_path, &rom_size, err, sizeof err);
    if (!rom) { fprintf(stderr, "%s: %s\n", err, rom_path); return 2; }

    if (input_path) {
        if (!LoadTape(input_path)) { fprintf(stderr, "cannot load tape %s\n", input_path); return 2; }
        fprintf(stderr, "input: %zu %s from %s\n", s_tape_frames, s_polled ? "polls" : "frames", input_path);
    }

    static char manifest_path[1024];
    if (log_path) {
        snprintf(manifest_path, sizeof manifest_path, "%s.tier2.json", log_path);
        setenv("SNESRECOMP_TIER2_CAPTURE", "1", 0);
    }

    CrcInit();
    TlkRomMapInit(rom, rom_size);
    TlkCallPairInit(rom, rom_size);
    s_interp_seen = (uint8_t *)calloc(1u << 24, 1);
    s_compiled_seen = (uint8_t *)calloc(1u << 24, 1);
    if (!s_interp_seen || !s_compiled_seen) { fprintf(stderr, "oom\n"); return 2; }
    g_interp_bridge_pc_hook = PcHook;
    g_interp_bridge_bounce_hook = BounceHook;

    RtlRegisterGame(&kTlkGameInfo);
    if (!SnesInit(rom, (int)rom_size)) {
        fprintf(stderr, "snesrecomp rejected the ROM\n");
        return 4;
    }

    /* Arm after SnesInit: the interp bridge hook table is runtime state. */
    if (s_polled || ram_log_path) {
        interp_bridge_add_pre_opcode_hook(0xC0A07C, PollHook);
        interp_bridge_add_pre_opcode_hook(0x00A07C, PollHook);
        g_tlk_aot_pad_read_hook = PollHookCompiled; /* task CC: compiled $C0A05D / $C0A073 */
    }
    FILE *ram_log = ram_log_path ? fopen(ram_log_path, "w") : NULL;
    if (ram_log_path && !ram_log) { fprintf(stderr, "cannot write %s\n", ram_log_path); return 7; }

    unsigned long active_frames = 0;
    long frame, start_frame = 0;
    if (load_path) {
        uint32_t hf = 0;
        if (!tlk_state_load(load_path, &hf)) { fprintf(stderr, "cannot load state %s\n", load_path); return 8; }
        start_frame = (long)hf;
        printf("loaded_state=%s at_frame=%ld\n", load_path, start_frame);
    }
    for (frame = start_frame; frame < frames; frame++) {
        s_polls_frame = 0;
        if (getenv("TLK_POLL_TRACE")) { fprintf(stderr, "frame %ld start v=%u h=%u aj=%d idx=%u\n", frame + 1, (unsigned)g_snes->vPos, (unsigned)g_snes->hPos, (int)g_snes->autoJoyRead, g_snes->joypad1Index); g_snes->joypad1Index = 0; }
        RtlRunFrame(s_polled ? PolledPadState() : InputForFrame(frame));
        { /* Diagnostics: TLK_ARAM_DUMP="<frame>,<path>" writes the 64 KiB SPC RAM after that frame. */
            static int ainit; static long af = -1; static char ap[512];
            if (!ainit) { const char *e = getenv("TLK_ARAM_DUMP"); ainit = 1; if (e) sscanf(e, "%ld,%511s", &af, ap); }
            if (af == frame + 1) { FILE *f = fopen(ap, "wb"); if (f) { fwrite(g_snes->apu->ram, 1, 0x10000, f); fclose(f); } }
        }
        { /* Diagnostics: TLK_WRAM_DUMP="f1,f2,..." TLK_WRAM_DIR=dir writes hl_wram_<f>.bin. */
            static const char *dl; static int init;
            if (!init) { dl = getenv("TLK_WRAM_DUMP"); init = 1; }
            if (dl) {
                char key[32]; snprintf(key, sizeof key, ",%ld,", frame + 1);
                char buf[4096]; snprintf(buf, sizeof buf, ",%s,", dl);
                if (strstr(buf, key)) {
                    char p[1024];
                    snprintf(p, sizeof p, "%s/hl_wram_%ld.bin", getenv("TLK_WRAM_DIR") ? getenv("TLK_WRAM_DIR") : ".", frame + 1);
                    FILE *wf = fopen(p, "wb");
                    if (wf) { fwrite(g_ram, 1, 0x20000, wf); fclose(wf); }
                }
            }
        }
        { /* Diagnostics: TLK_FRAME_LOG=path writes "frame fnv1a(WRAM) $0A0A fnv1a(VRAM)" per frame. */
            static FILE *fl; static int finit;
            if (!finit) { const char *e = getenv("TLK_FRAME_LOG"); if (e) fl = fopen(e, "w"); finit = 1; }
            if (fl) {
                uint32_t h = 2166136261u;
                for (size_t k = 0; k < 0x20000; k++) h = (h ^ g_ram[k]) * 16777619u;
                uint32_t hv = 2166136261u;
                const uint8_t *vb = (const uint8_t *)g_snes->ppu->vram;
                for (size_t k = 0; k < sizeof g_snes->ppu->vram; k++) hv = (hv ^ vb[k]) * 16777619u;
                fprintf(fl, "%ld %08X %04X %08X\n", frame + 1, (unsigned)h, (unsigned)(g_ram[0xA0A] | g_ram[0xA0B] << 8), (unsigned)hv);
                fflush(fl);
            }
        }
        if (ram_log) {
            const uint8_t *r = g_ram;
#define W(a) (unsigned)(r[(a)] | r[(a) + 1] << 8)
            fprintf(ram_log, "%ld %d %06X %04X %04X %04X %04X %04X %04X\n", frame + 1,
                    s_polls_frame == 0 ? 1 : 0,
                    (unsigned)(r[0x1E00] | r[0x1E01] << 8 | r[0x1E02] << 16),
                    W(0x2004), W(0xB21B), W(0xB21D), W(0x1FF9E), W(0x1FFAA), W(0x5A));
            /* TLK_RAM_LOG_DEATH=1: also "skip_death continues" ($7EA6BA = $FFFF after a fall in water,
             * $7FFFA8), task Z. Off by default so the BizHawk-compatible format stays unchanged. */
            if (getenv("TLK_RAM_LOG_DEATH")) {
                fseek(ram_log, -1, SEEK_CUR);
                fprintf(ram_log, " %04X %04X\n", W(0xA6BA), W(0x1FFA8));
            }
#undef W
        }
        PpuBeginDrawing(g_ppu, s_pixels, kWidth * kBpp, kPpuRenderFlags_NewRenderer);
        kTlkGameInfo.draw_ppu_frame();
        if (g_fail) { fprintf(stderr, "runtime failure (g_fail) at frame %ld\n", frame); break; }
        if (save_path && frame + 1 == save_at) {
            if (!tlk_state_save(save_path, (uint32_t)(frame + 1))) { fprintf(stderr, "cannot save state %s\n", save_path); return 8; }
            printf("saved_state=%s at_frame=%ld\n", save_path, frame + 1);
        }

        int active = 0;
        for (size_t i = 0; i < sizeof s_pixels; i += 4)
            if (s_pixels[i] | s_pixels[i + 1] | s_pixels[i + 2]) { active = 1; break; }
        active_frames += active;

        if (dump_png && dump_every > 0 && (frame + 1) % dump_every == 0) {
            char p[1024];
            StemPath(p, sizeof p, dump_png, frame + 1);
            WritePng(p, s_pixels, kWidth, kHeight, kWidth * kBpp);
        }
    }
    long ran = frame < frames ? frame + 1 : frames;
    TlkRomMapWrite();
    TlkCallPairWrite();

    int t2_sites = 0;
    unsigned long long t2_clean = 0, t2_bail = 0;
    interp_tier2_stats(&t2_sites, &t2_clean, &t2_bail);
    long tier_downs = interp_tier_hit_count();

    uint8_t fh[32], wh[32];
    sha256_compute(s_pixels, sizeof s_pixels, fh);
    sha256_compute(g_ram, 0x20000, wh);

    if (ram_log) fclose(ram_log);
    printf("frames=%ld video_active_frames=%lu\n", ran, active_frames);
    { extern uint64_t tlk_decomp_native_ops(void), tlk_decomp_native_runs(void);
      printf("native_decomp_ops=%llu native_decomp_runs=%llu\n", (unsigned long long)tlk_decomp_native_ops(), (unsigned long long)tlk_decomp_native_runs()); }
    if (s_polled || ram_log_path) printf("polls_seen=%lu tape_words_used=%zu/%zu\n", s_polls_total, s_poll_idx, s_tape_frames);
    printf("video_state inidisp=$%02x bgmode=$%02x main=$%02x sub=$%02x frame_counter=%d\n",
           g_ppu->inidisp, g_ppu->bgmode, g_ppu->screenEnabled[0],
           g_ppu->screenEnabled[1], snes_frame_counter);
    printf("frame_sha256="); PrintHash(stdout, fh);
    printf("\nwram_sha256="); PrintHash(stdout, wh);
    printf("\ninterp_opcodes=%llu interp_unique_pcs=%lu compiled_entries=%llu compiled_unique_entries=%lu\n",
           s_interp_ops, s_interp_unique, s_compiled_entries, s_compiled_unique);
    printf("tier_downs=%ld tier2_sites=%d tier2_clean=%llu tier2_bail=%llu\n",
           tier_downs, t2_sites, t2_clean, t2_bail);
    printf("interp_unique_pcs_by_bank:");
    for (int b = 0; b < 256; b++)
        if (s_interp_bank_unique[b]) printf(" $%02X=%lu", b, s_interp_bank_unique[b]);
    printf("\n");

    int rc = g_fail ? 6 : 0;
    if (dump_png) {
        if (WritePng(dump_png, s_pixels, kWidth, kHeight, kWidth * kBpp))
            printf("dump_png=%s\n", dump_png);
        else { fprintf(stderr, "cannot write %s\n", dump_png); rc = 7; }
    }

    if (log_path) {
        FILE *f = fopen(log_path, "w");
        if (!f) { fprintf(stderr, "cannot write %s\n", log_path); return 7; }
        fprintf(f, "{\n  \"title\": \"tlk\",\n  \"frames\": %ld,\n", ran);
        fprintf(f, "  \"g_fail\": %s,\n", g_fail ? "true" : "false");
        fprintf(f, "  \"interp_opcodes\": %llu,\n  \"interp_unique_pcs\": %lu,\n",
                s_interp_ops, s_interp_unique);
        fprintf(f, "  \"compiled_entries\": %llu,\n  \"compiled_unique_entries\": %lu,\n",
                s_compiled_entries, s_compiled_unique);
        fprintf(f, "  \"tier_downs\": %ld,\n  \"tier2_sites\": %d,\n"
                   "  \"tier2_clean\": %llu,\n  \"tier2_bail\": %llu,\n",
                tier_downs, t2_sites, t2_clean, t2_bail);
        /* Interpreted PCs that were never entered as a compiled body: the
         * worklist for cfg seeding. Listed as contiguous runs. */
        fprintf(f, "  \"interp_pc_runs\": [");
        int first = 1;
        for (uint32_t pc = 0; pc < (1u << 24); pc++) {
            if (!s_interp_seen[pc]) continue;
            uint32_t end = pc;
            while (end + 1 < (1u << 24) && (s_interp_seen[end + 1] ||
                   (end + 4 < (1u << 24) && (s_interp_seen[end + 2] || s_interp_seen[end + 3] || s_interp_seen[end + 4]))))
                end++;
            fprintf(f, "%s\"%06X-%06X\"", first ? "" : ",", pc, end);
            first = 0;
            pc = end;
        }
        fprintf(f, "],\n  \"compiled_entry_pcs\": [");
        first = 1;
        for (uint32_t pc = 0; pc < (1u << 24); pc++)
            if (s_compiled_seen[pc]) { fprintf(f, "%s\"%06X\"", first ? "" : ",", pc); first = 0; }
        fprintf(f, "],\n");
        CpuUnresolvedAbandonDumpJson(f);
        CpuDispatchLogDumpJson(f);
        Tier2CoverageDumpJson(f);
        fprintf(f, "  \"_end\": true\n}\n");
        fclose(f);
        Tier2CoverageWriteManifest(manifest_path, "tlk");
        printf("log=%s tier2_manifest=%s\n", log_path, manifest_path);
    }

    if (s_interp_hist) {
        FILE *hf = fopen(getenv("TLK_INTERP_HIST"), "w");
        if (hf) {
            for (uint32_t pc = 0; pc < (1u << 24); pc++)
                if (s_interp_hist[pc]) fprintf(hf, "%06X %u\n", (unsigned)pc, (unsigned)s_interp_hist[pc]);
            fclose(hf);
        }
    }
    if (s_mx_hist) {
        FILE *hf = fopen(getenv("TLK_MX_HIST"), "w");
        if (hf) {
            for (uint32_t pc = 0; pc < (1u << 24); pc++)
                if (s_mx_hist[pc]) fprintf(hf, "%06X %u\n", (unsigned)pc, (unsigned)s_mx_hist[pc]);
            fclose(hf);
        }
    }
    printf("result=%s frames=%ld\n", rc ? "failed" : "completed", ran);
    free(rom);
    free(s_tape);
    return rc;
}
