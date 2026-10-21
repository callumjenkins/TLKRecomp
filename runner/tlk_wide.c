/* tlk_wide.c -- widescreen compositor for The Lion King (see tlk_wide.h).
 *
 * Mode 1 decode and composition follow snesrecomp's PPU (ppu.c): the same
 * priorities, windows, colour math, brightness and sprite range/time limits,
 * so the centre 256 columns of a level frame equal the PPU's own picture
 * (headless --wide reports any difference as native_mismatch). Only where
 * the inputs come from differs, and only in the margins. */
#include "tlk_wide.h"

#include "common_rtl.h"
#include "cpu_state.h"
#include "snes/interp_bridge.h"
#include "snes/ppu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* aot_pace.h: generated code, after the watched instructions. */
void (*g_tlk_wide_pc_hook)(CpuState *cpu, uint32_t pc24);
/* tlk_game.c: passed to the beam driver's per-line raster observer. */
extern void (*g_tlk_raster_observer)(const Ppu *ppu, unsigned line, void *context);

enum {
    kPcPiece16 = 0x00A727,   /* one 16x16 piece; drops x outside [-15,255], y outside [-15,238] */
    kPcPiece8 = 0x00A7AE,    /* one 8x8 piece; drops x outside [-7,255], y outside [-7,223] */
    kPcOamDma = 0x00A033,    /* NMI's OAM upload, made only for a frame the game finished */
    kPcSpawnWindow = 0x00B915, /* FN_C0B8EA: its left/right tile columns $46/$14 are set, not yet used */
    kPcCullRight = 0x00A5C2, /* FN_C0A5BA: right limit camera+256 in $14, about to be compared */
    kPcCullLeft = 0x00A5CF,  /* FN_C0A5BA: LDY between LDA $9C (box left) and ADC width; compare with camera */
    kPcRideScript = 0x00B618, /* $C0B5FD: LDA $0A56 before the obstacle script reads distance $0A40 */
    kPcRideScriptEnd = 0x00B69F, /* its common exit (PLA) */
    kPcRideSpawnX = 0x00BB98,  /* $C0BB80: LDY after STA $88, the new obstacle's x */
    kMaxPieces = 512,
    kVramCopies = 4,
    kHudSlotsMax = 16,
};

typedef struct Piece {
    int16_t x, y;            /* signed screen position, OAM convention (row = line - 1 - y) */
    uint16_t attr;           /* OAM attribute word: tile, name select, palette, priority, flips */
    uint16_t obj;            /* the object being drawn ($7C) */
    uint8_t size, slot;      /* 16 or 8; the OAM slot it gets if the game keeps it */
    bool kept;               /* the game wrote it to OAM (inside its screen test) */
    bool fixed;              /* a screen-fixed object (HUD): obj+9 bit 6, drawn with no camera */
    int8_t side;             /* HUD at the edges: -64 (left edge) .. 64 (right edge), 0 stays (set at latch) */
} Piece;

/* One visible line as the PPU drew it. */
typedef struct Line {
    uint8_t inidisp, obsel, bgmode, mosaic, setini, oamaddl, oamaddh;
    uint8_t bgsc[4];
    uint16_t bgchr;
    uint16_t hofs[4], vofs[4];
    uint16_t fixed;
    uint32_t windowsel;
    uint8_t w1l, w1r, w2l, w2r;
    uint16_t wlog;
    uint8_t tm, ts, tmw, tsw, cgadsub, cgwsel;
    uint8_t vram;            /* which s_vram copy */
    uint16_t cgram[256];
    uint16_t oam[256];
    uint8_t hoam[32];
} Line;

/* The level's BG1 source: map at $7F0000 (or a copy of it), row size and
 * height from the level record, block table in ROM. */
typedef struct Level {
    unsigned rowb, rows;
    uint32_t blk;
    const uint8_t *map;
} Level;

static const uint8_t *s_rom;
static size_t s_rom_size;
static bool s_enabled;
static int s_spawn_lead;   /* pixels added to each side of the game's spawn window; 0 = the game's own */
static int s_ride_lead;    /* pixels the ride script runs ahead this call (kPcRideScript..End), 0 outside */
static int s_draw_extra;   /* the last drawn view's (width - 256) / 2, for ObjLog */

static Piece s_build[kMaxPieces], s_latched[kMaxPieces];
static unsigned s_build_count, s_latched_count;

static uint16_t s_vram[kVramCopies][0x8000];
static uint32_t s_vram_count[kVramCopies];
static bool s_vram_have;
static int s_vram_used, s_vram_cur;

static struct {
    Line lines[TLK_WIDE_HEIGHT];
    int captured;
    uint8_t ram[0x20000];
    Piece pieces[kMaxPieces];
    unsigned piece_count;
    int hud_end;
    uint32_t stock[256 * TLK_WIDE_HEIGHT];
    bool valid, stage;
    Level level;
    uint8_t stage_sc;
    uint16_t stage_chr;
} s_frame;

static TlkWideStats s_stats;

static unsigned rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t rd24(const uint8_t *p) { return p[0] | p[1] << 8 | (uint32_t)p[2] << 16; }

/* Where the wide view sits. The game clamps its camera x between $A6EC and
 * $D8 ($C0B273..$C0B284; in every level 0 and the map width - 256), and the
 * wide view stops where the camera stops: its left edge is the camera minus
 * the margin, kept inside [$A6EC, $D8 + 256]. A level narrower than the view
 * is centred. Returns the native picture's column in a view width wide;
 * *lo/*hi get the view columns that lie inside the camera's range. */
static int ViewColumn(const uint8_t *ram, int camera, int width, int *lo, int *hi) {
    int extra = (width - 256) / 2, min = (int)rd16(ram + 0xA6EC), max = (int)rd16(ram + 0xD8);
    *lo = 0;
    *hi = width;
    if (min > max) return extra;
    int left = camera - extra, last = max + 256 - width;
    if (last < min) left = (min + last) / 2;
    else left = left < min ? min : left > last ? last : left;
    int column = camera - left;
    column = column < 0 ? 0 : column > width - 256 ? width - 256 : column;
    left = camera - column;
    if (min - left > 0) *lo = min - left;
    if (max + 256 - left < width) *hi = max + 256 - left;
    return column;
}

/* How far towards an edge a HUD sprite centred at native column c goes, in
 * 64ths: all the way within 64 pixels of the native edge (health, roar,
 * lives), not at all within 16 of the middle (titles, centred text), and in
 * between in proportion, so one that flies across the screen (a bug on its
 * way to the health box) moves smoothly instead of jumping by the margin. */
static int8_t HudSide(int c) {
    int d = c - 128, a = d < 0 ? -d : d;
    int w = a <= 16 ? 0 : a >= 64 ? 64 : (a - 16) * 64 / 48;
    return (int8_t)(d < 0 ? -w : w);
}

/* The view's margins left and right of the native picture, from the live
 * camera ($B29B), for the spawn and draw tests. */
static void LiveMargins(int lead, int *left, int *right) {
    int lo, hi, width = 256 + 2 * lead;
    *left = ViewColumn(g_ram, (int)rd16(g_ram + 0xB29B), width, &lo, &hi);
    *right = width - 256 - *left;
}

/* ── Piece recorder ────────────────────────────────────────────────────── */

/* A guest read for the recorder: WRAM or HiROM, nothing with side effects. */
static int Bus8(uint32_t a) {
    unsigned bank = (a >> 16) & 0xFF, off = a & 0xFFFF;
    if (bank == 0x7E) return g_ram[off];
    if (bank == 0x7F) return g_ram[0x10000 + off];
    if ((bank & 0x7F) < 0x40 && off < 0x2000) return g_ram[off];
    if (bank >= 0xC0 || ((bank & 0x7F) >= 0x40 && (bank & 0x7F) < 0x7E) ||
        ((bank & 0x7F) < 0x40 && off >= 0x8000)) {
        uint32_t r = ((uint32_t)(bank & 0x3F) << 16) | off;
        return s_rom && r < s_rom_size ? s_rom[r] : -1;
    }
    return -1;
}
static int Bus16(uint32_t a) {
    int lo = Bus8(a), hi = Bus8((a & 0xFF0000) | ((a + 1) & 0xFFFF));
    return lo < 0 || hi < 0 ? -1 : lo | hi << 8;
}

/* $C0A727 / $C0A7AE at entry (M=0 X=1, D=0 in every recorded call): the
 * piece's offset is at [$99], the object's screen position at $B4/$B6, its
 * flips at $AF/$B2, tile index at $93, attribute bits at $B1. This repeats
 * the routine's arithmetic without its screen test. */
static void RecordPiece(const CpuState *cpu, bool big) {
    if (cpu->m_flag || !cpu->x_flag || s_build_count >= kMaxPieces) return;
    const uint32_t d = cpu->D;
#define DP8(a) Bus8((d + (a)) & 0xFFFF)
#define DP16(a) Bus16((d + (a)) & 0xFFFF)
    int lo = DP16(0x99), bank = DP8(0x9B);
    if (lo < 0 || bank < 0) return;
    uint32_t ptr = (uint32_t)bank << 16 | (uint32_t)lo;
    int dx = Bus16(ptr), dy = Bus16((ptr & 0xFF0000) | ((ptr + 2) & 0xFFFF));
    int vflip = DP8(0xB2), hflip = DP8(0xAF), sx = DP16(0xB4), sy = DP16(0xB6);
    int index = DP16(0x93), bits = DP16(0xB1), slot = DP8(0xA8), yoff = DP8(0x8C), obj = DP16(0x7C);
    if (dx < 0 || dy < 0 || vflip < 0 || hflip < 0 || sx < 0 || sy < 0 || index < 0 || bits < 0 ||
        slot < 0 || yoff < 0 || obj < 0)
        return;
    /* $C0A58E: an object with obj+9 bit 6 is drawn with the camera ($52/$54) at zero. */
    int flags = Bus8((uint32_t)cpu->DB << 16 | ((obj + 9) & 0xFFFF));
    if (vflip & 0x80) dy = ((dy ^ 0xFFFF) - (big ? 0x10 : 0x08)) & 0xFFFF;
    if (hflip) dx = ((dx ^ 0xFFFF) - (big ? 0x0F : 0x07)) & 0xFFFF;
    int16_t y = (int16_t)(dy + sy), x = (int16_t)(dx + sx);
    /* 16x16: TAY with 8-bit index; 8x8: REP #$10 first. */
    unsigned table = big ? 0xACFD + ((index << 1) & 0xFF) : 0xACFD + 0x100 + ((index << 1) & 0xFFFF);
    int attr = Bus16((uint32_t)cpu->DB << 16 | (table & 0xFFFF));
    if (attr < 0) return;
#undef DP8
#undef DP16
    int lo_lim = big ? -15 : -7, y_lim = big ? 0xEF : 0xE0;
    bool kept = x >= lo_lim && x < 256 && y >= lo_lim && y < y_lim;
    s_build[s_build_count++] = (Piece){ x, (int16_t)(y - yoff), (uint16_t)(attr | bits), (uint16_t)obj,
                                        (uint8_t)(big ? 16 : 8), (uint8_t)slot, kept,
                                        flags >= 0 && (flags & 0x40), 0 };
}

/* The game's spawn window ($C0B8EA, run by the camera object): tile columns
 * from 12 (96 px) left of the camera to 42 (336 px, 80 past the native right
 * edge) right of it, 16-pixel aligned, clamped to the map ($D0 = its width in
 * tiles). Enemies inside it are created; the despawn bounds $B203/$B205 come
 * from the same columns. Widening both by the picture's margin keeps the
 * game's own lead beyond the wider view. This changes the game. */
static void WidenSpawn(CpuState *cpu) {
    const uint32_t d = cpu->D;
    if (s_spawn_lead <= 0 || ((d + 0xD1) & 0xFFFF) >= 0x2000) return;
    int ml, mr;
    LiveMargins(s_spawn_lead, &ml, &mr);
    unsigned kl = (unsigned)(ml + 15) / 16 * 2, kr = (unsigned)(mr + 15) / 16 * 2;
    uint8_t *left = &g_ram[(d + 0x46) & 0xFFFF], *right = &g_ram[(d + 0x14) & 0xFFFF];
    unsigned l = left[0] | left[1] << 8, r = right[0] | right[1] << 8;
    unsigned cap = (unsigned)(g_ram[(d + 0xD0) & 0xFFFF] | g_ram[(d + 0xD1) & 0xFFFF] << 8) - 2;
    l = l > kl ? l - kl : 0;
    r = r + kr < cap ? r + kr : (cap > r ? cap : r);
    left[0] = (uint8_t)l; left[1] = (uint8_t)(l >> 8);
    right[0] = (uint8_t)r; right[1] = (uint8_t)(r >> 8);
}

/* Diagnostics: TLK_WIDE_OBJLOG=<path> writes, at each latch, one line per
 * drawn object: frame, object, x0 x1 y0 y1 of its pieces, fixed, obj+4, and
 * the view's left and right margins (from the camera, as drawn). Pop-in
 * shows as an object whose first box is already inside the view
 * (scripts/wide_popin.py). */
static void ObjLog(void) {
    static FILE *f;
    static int init;
    if (!init) { const char *e = getenv("TLK_WIDE_OBJLOG"); if (e) f = fopen(e, "w"); init = 1; }
    extern int snes_frame_counter;
    for (unsigned i = 0, j; f && i < s_latched_count; i = j) {
        int x0 = 1 << 15, x1 = -(1 << 15), y0 = 1 << 15, y1 = -(1 << 15);
        for (j = i; j < s_latched_count && s_latched[j].obj == s_latched[i].obj; j++) {
            const Piece *q = &s_latched[j];
            if (q->x < x0) x0 = q->x;
            if (q->x + q->size > x1) x1 = q->x + q->size;
            if (q->y < y0) y0 = q->y;
            if (q->y + q->size > y1) y1 = q->y + q->size;
        }
        unsigned o = s_latched[i].obj;
        int ml = s_draw_extra, mr = s_draw_extra;
        if (s_draw_extra) LiveMargins(s_draw_extra, &ml, &mr);
        fprintf(f, "%d %04x %d %d %d %d %d %u %d %d\n", snes_frame_counter, o, x0, x1, y0, y1, s_latched[i].fixed,
                (unsigned)(g_ram[(o + 4) & 0xFFFF] | g_ram[(o + 5) & 0xFFFF] << 8), ml, mr);
    }
}

/* FN_C0A5BA draws an object only if its box overlaps camera..camera+256
 * horizontally; otherwise no piece is even submitted, so an object alive in a
 * margin is never seen. Widen that test by the margin: the right limit in $14,
 * and the box's left edge in A for the left test (the hook point is an LDY, so
 * A holds the same value before and after it, compiled or interpreted). Every
 * piece of such an object still fails the game's own OAM test, so OAM is the
 * same; what changes is the drawing work itself (tile uploads, CPU time). */
static void WidenCull(CpuState *cpu, bool right) {
    if (s_spawn_lead <= 0 || cpu->m_flag) return;
    int ml, mr;
    LiveMargins(s_spawn_lead, &ml, &mr);
    if (right) {
        uint8_t *lim = &g_ram[(cpu->D + 0x14) & 0x1FFF];
        unsigned v = (unsigned)(lim[0] | lim[1] << 8) + (unsigned)mr;
        lim[0] = (uint8_t)v; lim[1] = (uint8_t)(v >> 8);
    } else {
        cpu->A = (uint16_t)(cpu->A + ml);
    }
}

/* The ostrich ride's obstacles come from a script, not the spawn window:
 * while $0A56 is set, $C0B5FD compares the distance ridden ($0A40, the
 * camera's x travel) with the next entry of a table at [$BA] and, once
 * reached, $C0BB80 creates the obstacles at x + $140 of the object at $AA,
 * some 30-90 pixels past the native right edge; the draw test then shows them
 * only on reaching it. The obstacles stand still in the world, so running the script ahead by
 * the view's right margin and placing each one that much further right
 * leaves every obstacle where and when the game puts it, but created beyond
 * the wider view. All three hook points leave alone what they change ($0A40,
 * $88): an LDA of $0A56, a PLA, an LDY. This changes the game. */
static void RideScript(CpuState *cpu, int stage) {
    uint8_t *dist = &g_ram[0x0A40];
    if (stage == 0) {
        if (s_spawn_lead <= 0 || s_ride_lead || cpu->DB != 0x7E || !(g_ram[0x0A56] | g_ram[0x0A57])) return;
        int ml, mr;
        LiveMargins(s_spawn_lead, &ml, &mr);
        s_ride_lead = (mr + 15) & ~15;   /* the script places on 16-pixel steps */
    } else if (stage == 2) {
        if (s_ride_lead) {
            unsigned x = (unsigned)(g_ram[(cpu->D + 0x88) & 0xFFFF] | g_ram[(cpu->D + 0x89) & 0xFFFF] << 8) + (unsigned)s_ride_lead;
            g_ram[(cpu->D + 0x88) & 0xFFFF] = (uint8_t)x;
            g_ram[(cpu->D + 0x89) & 0xFFFF] = (uint8_t)(x >> 8);
        }
        return;
    }
    if (!s_ride_lead) return;
    unsigned v = (unsigned)(dist[0] | dist[1] << 8) + (unsigned)(stage == 0 ? s_ride_lead : -s_ride_lead);
    dist[0] = (uint8_t)v;
    dist[1] = (uint8_t)(v >> 8);
    if (stage == 1) s_ride_lead = 0;
}

static void Observe(CpuState *cpu, uint32_t pc24) {
    switch (pc24 & 0x3FFFFF) {
    case kPcSpawnWindow: WidenSpawn(cpu); return;
    case kPcCullRight: WidenCull(cpu, true); return;
    case kPcCullLeft: WidenCull(cpu, false); return;
    case kPcRideScript: RideScript(cpu, 0); return;
    case kPcRideScriptEnd: RideScript(cpu, 1); return;
    case kPcRideSpawnX: RideScript(cpu, 2); return;
    }
    if (!s_enabled) return;
    switch (pc24 & 0x3FFFFF) {
    case kPcPiece16: RecordPiece(cpu, true); break;
    case kPcPiece8: RecordPiece(cpu, false); break;
    case kPcOamDma:
        memcpy(s_latched, s_build, s_build_count * sizeof *s_build);
        s_latched_count = s_build_count;
        s_build_count = 0;
        ObjLog();
        /* A screen-fixed object goes towards the edge it sits nearer, by
         * HudSide of its centre: the HUD boxes all the way, a title or text
         * across the middle not at all. */
        for (unsigned i = 0, j; i < s_latched_count; i = j) {
            int lo = 1 << 15, hi = -(1 << 15);
            for (j = i; j < s_latched_count && s_latched[j].obj == s_latched[i].obj &&
                        s_latched[j].fixed == s_latched[i].fixed; j++)
                if (s_latched[j].kept) {
                    if (s_latched[j].x < lo) lo = s_latched[j].x;
                    if (s_latched[j].x + s_latched[j].size > hi) hi = s_latched[j].x + s_latched[j].size;
                }
            int8_t side = !s_latched[i].fixed || lo > hi ? 0 : HudSide((lo + hi) / 2);
            for (unsigned k = i; k < j; k++) s_latched[k].side = side;
        }
        break;
    }
}

static void ArmInterpreterHooks(void) {
    static const uint32_t pcs[] = { 0xC0A727, 0x00A727, 0xC0A7AE, 0x00A7AE, 0xC0A033, 0x00A033,
                                    0xC0B915, 0x00B915, 0xC0A5C2, 0x00A5C2, 0xC0A5CF, 0x00A5CF,
                                    0xC0B618, 0x00B618, 0xC0B69F, 0x00B69F, 0xC0BB98, 0x00BB98 };
    for (size_t i = 0; i < sizeof pcs / sizeof pcs[0]; i++) interp_bridge_add_pre_opcode_hook(pcs[i], Observe);
}

/* ── Line capture ──────────────────────────────────────────────────────── */

static void CaptureLine(const Ppu *p, unsigned line, void *context) {
    (void)context;
    if (line < 1 || line > TLK_WIDE_HEIGHT) return;
    if (line == 1) {
        s_frame.captured = 0;
        s_frame.valid = false;
        memcpy(s_frame.ram, g_ram, sizeof s_frame.ram);
        memcpy(s_frame.pieces, s_latched, s_latched_count * sizeof *s_latched);
        s_frame.piece_count = s_latched_count;
        s_frame.hud_end = s_latched_count && s_latched[0].slot <= kHudSlotsMax ? s_latched[0].slot : 0;
        /* VRAM changes rarely: keep the first copy while its write count holds. */
        s_vram_used = 1;
        s_vram_cur = 0;
        if (!s_vram_have || s_vram_count[0] != p->vramWriteCount) {
            memcpy(s_vram[0], p->vram, sizeof s_vram[0]);
            s_vram_count[0] = p->vramWriteCount;
            s_vram_have = true;
        }
    } else if (s_vram_count[s_vram_cur] != p->vramWriteCount) {
        if (s_vram_used < kVramCopies) s_vram_cur = s_vram_used++;
        memcpy(s_vram[s_vram_cur], p->vram, sizeof s_vram[0]);
        s_vram_count[s_vram_cur] = p->vramWriteCount;
        if (s_vram_cur == 0) s_vram_have = false;
    }
    if ((int)line != s_frame.captured + 1) return;
    Line *l = &s_frame.lines[line - 1];
    l->inidisp = p->inidisp; l->obsel = p->obsel; l->bgmode = p->bgmode; l->mosaic = p->mosaic;
    l->setini = p->setini; l->oamaddl = p->oamaddl; l->oamaddh = p->oamaddh;
    memcpy(l->bgsc, p->bgXsc, 4);
    l->bgchr = p->bgTileAdr;
    memcpy(l->hofs, p->hScroll, sizeof l->hofs);
    memcpy(l->vofs, p->vScroll, sizeof l->vofs);
    l->fixed = p->fixedColor;
    l->windowsel = p->windowsel;
    l->w1l = p->window1left; l->w1r = p->window1right; l->w2l = p->window2left; l->w2r = p->window2right;
    l->wlog = p->wbgobjlog;
    l->tm = p->screenEnabled[0]; l->ts = p->screenEnabled[1];
    l->tmw = p->screenWindowed[0]; l->tsw = p->screenWindowed[1];
    l->cgadsub = p->cgadsub; l->cgwsel = p->cgwsel;
    l->vram = (uint8_t)s_vram_cur;
    memcpy(l->cgram, p->cgram, sizeof l->cgram);
    memcpy(l->oam, p->oam, sizeof l->oam);
    memcpy(l->hoam, p->highOam, sizeof l->hoam);
    s_frame.captured = (int)line;
}

/* ── Level map ─────────────────────────────────────────────────────────── */

static bool LevelOf(const uint8_t *ram, Level *lv) {
    unsigned n = rd16(ram + 0x1FF9E);   /* $7FFF9E current level */
    if (!s_rom || n >= 16 || 0x357 + n * 3 + 3 > s_rom_size) return false;
    uint32_t rec = rd24(s_rom + 0x357 + n * 3) & 0x3FFFFF;
    if (rec + 0x32 > s_rom_size) return false;
    lv->rowb = rd16(s_rom + rec + 0x12);
    lv->rows = rd16(s_rom + rec + 0x14);
    lv->blk = rd24(s_rom + rec + 0x0F) & 0x3FFFFF;
    lv->map = ram + 0x10000;
    return lv->rowb && !(lv->rowb & 1) && lv->rows && lv->rowb * lv->rows <= 0x10000 && lv->blk < s_rom_size;
}

static int Reflect(int v, int n) {
    if (v < 0) v = -v - 1;
    if (v >= n) v = 2 * n - 1 - v;
    return v < 0 ? 0 : v >= n ? n - 1 : v;
}

/* The BG1 tilemap word at world pixel (*wx, *wy). Past the level's edges the
 * map is reflected, and *wx/*wy become the reflected position. */
static bool MapTile(const Level *lv, int *wx, int *wy, uint16_t *word) {
    *wx = Reflect(*wx, (int)lv->rowb * 8);
    *wy = Reflect(*wy, (int)lv->rows * 16);
    unsigned cell = rd16(lv->map + (*wy >> 4) * lv->rowb + (*wx >> 4) * 2) & 0xFFF8;
    uint32_t a = lv->blk + cell + (((*wx >> 3) & 1) | ((*wy >> 3) & 1) << 1) * 2;
    if (a + 2 > s_rom_size) return false;
    *word = (uint16_t)rd16(s_rom + a);
    return true;
}

/* World position nearest the camera with this scroll phase. */
static int Expand(int camera, unsigned scroll, int period) {
    int d = (int)((scroll - (unsigned)camera) & (unsigned)(period - 1));
    if (d >= period / 2) d -= period;
    return camera + d;
}

static uint16_t VramTilemap(const uint16_t *vram, unsigned sc, unsigned px, unsigned py, unsigned size) {
    unsigned tx = px / size, ty = py / size;
    unsigned a = (sc & 0xFC) << 8;
    a += (tx & 31) + (ty & 31) * 32;
    if ((sc & 1) && (tx & 32)) a += 0x400;
    if ((sc & 2) && (ty & 32)) a += (sc & 1) ? 0x800 : 0x400;
    return vram[a & 0x7FFF];
}

/* BG1's world origin on a line: world x of column 0, world y of the line. */
static void Bg1Origin(const Line *l, int y0, int *wx, int *wy) {
    *wx = Expand(rd16(s_frame.ram + 0xB29B), l->hofs[0] & 0x1FF, 512);
    *wy = Expand((int)rd16(s_frame.ram + 0xB29D) + y0 + 1, (l->vofs[0] + (unsigned)y0 + 1) & 0xFF, 256);
}

static bool LineHasStage(const Line *l) {
    return (l->bgmode & 7) == 1 && !(l->bgmode & 0x10) && !(l->mosaic & 0xF0 && l->mosaic & 7) &&
           !(l->setini & 0x0B) && l->bgsc[0] == s_frame.stage_sc && l->bgchr == s_frame.stage_chr;
}

/* How many sampled cells of the native BG1 tilemap agree with s_frame.level's
 * map (*same of *total). */
static void MapScore(unsigned *same_out, unsigned *total_out) {
    unsigned total = 0, same = 0;
    *same_out = *total_out = 0;
    for (int y0 = 8; y0 < TLK_WIDE_HEIGHT; y0 += 32) {
        const Line *l = &s_frame.lines[y0];
        if (!LineHasStage(l) || (l->inidisp & 0x80)) continue;
        int ox, oy;
        Bg1Origin(l, y0, &ox, &oy);
        for (int x = 16; x < 248; x += 32) {
            int wx = ox + x, wy = oy;
            uint16_t word;
            if (!MapTile(&s_frame.level, &wx, &wy, &word)) return;
            if (wx != ox + x || wy != oy) continue;   /* only inside the level */
            total++;
            same += VramTilemap(s_vram[l->vram], l->bgsc[0], (unsigned)(l->hofs[0] + x) & 1023,
                                (unsigned)(l->vofs[0] + y0 + 1) & 1023, 8) == word;
        }
    }
    *same_out = same;
    *total_out = total;
}

/* A level frame when most of it agrees. Not all of it: after the ostrich
 * ride's warp some cells differ for a while (the margins then follow
 * SeamAgrees); a title or menu agrees nowhere. */
static bool Agrees(unsigned same, unsigned total) { return total >= 20 && same * 2 >= total; }

static Level s_last_level;   /* the last level whose map agreed everywhere, map copied to s_last_map */
static uint8_t s_last_map[0x10000];
static uint8_t s_last_limits[4];   /* its camera limits, $A6EC and $D8 */
static bool s_have_last;

/* A level frame when a map agrees. The map in RAM can stop describing the
 * screen while the level is still shown: a level's closing scene overwrites
 * it (levels 4 and 6), and a Bug Hunt changes the level number about 14
 * frames before its fade (and zeroes the camera's right limit 2 frames
 * before). Then a copy of the last map that agreed everywhere is tried, and
 * the one that agrees more is used, with the camera limits it had. */
static bool StageMap(void) {
    /* Line 112, or the first line shown when the game has turned the screen
     * off below the top (a Bug Hunt's last frame: dark from line 30). */
    const Line *mid = &s_frame.lines[112];
    for (int y = 0; y < TLK_WIDE_HEIGHT && ((mid->inidisp & 0x80) || !(mid->inidisp & 15)); y++) mid = &s_frame.lines[y];
    if ((mid->bgmode & 7) != 1 || (mid->bgmode & 0x10) || !(mid->tm & 1)) return false;
    s_frame.stage_sc = mid->bgsc[0];
    s_frame.stage_chr = mid->bgchr;
    unsigned same = 0, total = 0, kept_same = 0, kept_total = 0;
    Level live;
    bool have_live = LevelOf(s_frame.ram, &live);
    if (have_live) {
        s_frame.level = live;
        MapScore(&same, &total);
        if (total >= 20 && same == total) {
            s_last_level = live;
            memcpy(s_last_map, live.map, live.rowb * live.rows);
            s_last_level.map = s_last_map;
            memcpy(s_last_limits, s_frame.ram + 0xA6EC, 2);
            memcpy(s_last_limits + 2, s_frame.ram + 0xD8, 2);
            return s_have_last = true;
        }
    }
    if (s_have_last) {
        s_frame.level = s_last_level;
        MapScore(&kept_same, &kept_total);
        /* Inside the run a few lines that all agree will do (that last frame
         * leaves one sampled line, 8 cells). */
        bool kept_ok = Agrees(kept_same, kept_total) || (kept_total >= 8 && kept_same == kept_total);
        if (kept_ok && (!have_live || !Agrees(same, total) || kept_same * total > same * kept_total)) {
            memcpy(s_frame.ram + 0xA6EC, s_last_limits, 2);
            memcpy(s_frame.ram + 0xD8, s_last_limits + 2, 2);
            return true;
        }
    }
    s_frame.level = live;
    return have_live && Agrees(same, total);
}

/* The copy only bridges an unbroken run of level frames: after a frame that
 * is not one (a fade to black, a title) it could match a mostly blank
 * tilemap, as the title screen's after the attract demo. */
static bool DetectStage(void) {
    if (StageMap()) return true;
    s_have_last = false;
    return false;
}

/* ── Compositor ────────────────────────────────────────────────────────── */

static unsigned TilePixel(const uint16_t *vram, unsigned address, unsigned x, unsigned y, unsigned bpp) {
    unsigned a = (address + y) & 0x7FFF, shift = 7 - x;
    unsigned bits = vram[a] >> shift;
    unsigned pixel = (bits & 1) | ((bits >> 7) & 2);
    if (bpp == 4) {
        bits = vram[(a + 8) & 0x7FFF] >> shift;
        pixel |= ((bits & 1) << 2) | ((bits >> 5) & 8);
    }
    return pixel;
}

/* z<<12 | layer<<8 | CGRAM index, 0 for transparent. map: BG1 at x comes
 * from the level map (a margin column on a side where map and VRAM agree). */
static uint16_t BgPixel(const Line *l, const uint16_t *vram, unsigned layer, int x, int y0, bool map,
                        int ox, int oy) {
    static const uint8_t low[3] = { 8, 7, 1 }, high[3] = { 12, 11, 3 };
    unsigned bpp = layer == 2 ? 2 : 4, size = (l->bgmode >> (4 + layer)) & 1 ? 16 : 8;
    uint16_t tile;
    int px, py;
    if (layer == 0 && map) {
        px = ox + x;
        py = oy;
        if (!MapTile(&s_frame.level, &px, &py, &tile)) return 0;
    } else {
        if (layer == 2 && (x < 0 || x >= 256)) return 0;   /* BG3 carries text, not scenery */
        px = (int)(((unsigned)x + l->hofs[layer]) & 1023);
        py = (int)(((unsigned)y0 + 1 + l->vofs[layer]) & 1023);
        tile = VramTilemap(vram, l->bgsc[layer], (unsigned)px, (unsigned)py, size);
    }
    unsigned cx = (unsigned)px & (size - 1), cy = (unsigned)py & (size - 1);
    if (tile & 0x4000) cx = size - 1 - cx;
    if (tile & 0x8000) cy = size - 1 - cy;
    unsigned number = ((tile & 1023) + cx / 8 + cy / 8 * 16) & 1023;
    unsigned base = ((l->bgchr >> (layer * 4)) & 15) << 12;
    unsigned pixel = TilePixel(vram, (base + number * bpp * 4) & 0x7FFF, cx & 7, cy & 7, bpp);
    if (!pixel) return 0;
    unsigned index = (((tile >> 10) & 7) << bpp) | pixel;
    unsigned z = tile & 0x2000 ? (layer == 2 && (l->bgmode & 8) ? 15 : high[layer]) : low[layer];
    return (uint16_t)(z << 12 | layer << 8 | index);
}

static const uint8_t kSpriteSizes[8][2] = {
    { 8, 16 }, { 8, 32 }, { 8, 64 }, { 16, 32 }, { 16, 64 }, { 32, 64 }, { 16, 32 }, { 16, 32 } };

/* One row of a sprite into obj (indexed by screen x + column), screen
 * columns [lo, hi) only. Later calls overwrite, so callers go from the lowest
 * priority sprite to the highest. Returns pixels drawn. */
static unsigned SpriteRow(uint16_t *obj, int width, int column, const Line *l, const uint16_t *vram, int x,
                          int row, int size, uint16_t attr, int lo, int hi) {
    if (attr & 0x8000) row = size - 1 - row;
    unsigned base = (l->obsel & 7u) << 13;
    if (attr & 0x100) base += ((l->obsel & 0x18u) + 8) << 9;
    unsigned z = ((((attr >> 12) & 3u) * 4 + 2) << 12) | ((attr & 0x800) ? 4u : 6u) << 8 |
                 (0x80 + 16 * ((attr >> 9) & 7u));
    unsigned drawn = 0;
    for (int c = 0; c < size; c++) {
        int sx = x + c, dest = sx + column;
        if (sx < lo || sx >= hi || dest < 0 || dest >= width) continue;
        int uc = (attr & 0x4000) ? size - 1 - c : c;
        unsigned t = ((((attr & 0xFFu) >> 4) + ((unsigned)row >> 3)) << 4) | (((attr & 0xFu) + ((unsigned)uc >> 3)) & 0xF);
        unsigned pixel = TilePixel(vram, base + t * 16, (unsigned)uc & 7, (unsigned)row & 7, 4);
        if (pixel) { obj[dest] = (uint16_t)(z | pixel); drawn++; }
    }
    return drawn;
}

static int OamX(const Line *l, unsigned index) {
    int x = (l->oam[index] & 0xFF) | ((l->hoam[index >> 3] >> (index & 7)) & 1) << 8;
    return x >= 256 ? x - 512 : x;
}
static int OamSize(const Line *l, unsigned index) {
    return kSpriteSizes[l->obsel >> 5][(l->hoam[index >> 3] >> ((index & 7) + 1)) & 1];
}

/* The PPU's sprites for one line (ppu_evaluateSprites: 32 in range, 34 slivers
 * fetched backwards), inside the native columns. Slots marked in skip (HUD
 * moved to the edges) are counted but left for the caller to place. */
static void NativeSprites(uint16_t *obj, int width, int column, const Line *l, const uint16_t *vram, int y0,
                          const bool *skip) {
    uint8_t found[32];
    int count = 0, tiles = 0;
    uint8_t index = (l->oamaddh & 0x80) ? (l->oamaddl & 0xFE) : 0;
    for (int i = 0; i < 128; i++, index += 2) {
        uint8_t row = (uint8_t)(y0 - (l->oam[index] >> 8));
        int size = OamSize(l, index);
        if (row >= size || OamX(l, index) + size <= 0) continue;
        if (count == 32) break;
        found[count++] = index;
    }
    for (int i = count; i > 0; i--) {
        unsigned idx = found[i - 1];
        int size = OamSize(l, idx), x = OamX(l, idx);
        int row = (uint8_t)(y0 - (l->oam[idx] >> 8));
        uint16_t attr = l->oam[idx + 1];
        for (int col = 0; col < size; col += 8) {
            if (col + x <= -8 || col + x >= 256) continue;
            if (++tiles > 34) break;
            if (skip[idx >> 1]) continue;
            int lo = col + x < 0 ? 0 : col + x, hi = col + x + 8 > 256 ? 256 : col + x + 8;
            /* One sliver: draw the whole sprite row clipped to it. */
            SpriteRow(obj, width, column, l, vram, x, row, size, attr, lo, hi);
        }
        if (tiles > 34) break;
    }
}

static bool Window(const Line *l, unsigned layer, int x, int column, int width) {
    unsigned f = (l->windowsel >> (layer * 4)) & 15;
    int w1l = l->w1l, w1r = l->w1r, w2l = l->w2l, w2r = l->w2r;
    /* A hardware edge pinned to 0/255 means the screen edge (ppu.c). */
    if (w1l == 0) w1l = -column;
    if (w1r == 255) w1r = width - 1 - column;
    if (w2l == 0) w2l = -column;
    if (w2r == 255) w2r = width - 1 - column;
    bool a = (f & 2) && w1l <= w1r && x >= w1l && x <= w1r;
    bool b = (f & 8) && w2l <= w2r && x >= w2l && x <= w2r;
    if ((f & 3) == 3) a = !a;
    if ((f & 12) == 12) b = !b;
    if ((f & 10) != 10) return a || b;
    switch ((l->wlog >> (layer * 2)) & 3) {
    case 0: return a || b;
    case 1: return a && b;
    case 2: return a != b;
    default: return a == b;
    }
}

/* Whether the level map and the native tilemap agree at native column x of
 * this line. After a warp (the ostrich ride jumps back 432 pixels) the map
 * behind the camera no longer shows what is on screen. */
static bool SeamAgrees(const Line *l, const uint16_t *vram, int y0, int ox, int oy, int x) {
    int wx = ox + x, wy = oy;
    uint16_t word;
    if (!MapTile(&s_frame.level, &wx, &wy, &word)) return false;
    return word == VramTilemap(vram, l->bgsc[0], ((unsigned)x + l->hofs[0]) & 1023,
                               ((unsigned)y0 + 1 + l->vofs[0]) & 1023, 8);
}

static bool s_hud_skip[128];   /* OAM slots the HUD pass places itself */
static int8_t s_hud_side[128];  /* where it places them, as Piece.side (64ths of the way to an edge) */
static bool s_hud_edges;
static bool s_map_left, s_map_right;   /* margins from the level map this frame, per side */
static int s_vis_lo, s_vis_hi;         /* view columns inside the camera's range */

/* How far the HUD pass moves a HUD sprite, in screen pixels: from where it
 * sits in a centred native picture, side 64ths of the margin towards an edge. */
static int HudShift(int side, int width, int column) {
    int extra = (width - 256) / 2;
    return extra + extra * side / 64 - column;
}

static void ComposeLine(uint32_t *out, int width, int column, int y0) {
    const Line *l = &s_frame.lines[y0];
    const uint16_t *vram = s_vram[l->vram];
    if (l->inidisp & 0x80) { memset(out, 0, (size_t)width * 4); return; }

    uint16_t obj[TLK_WIDE_MAX_WIDTH];
    memset(obj, 0, (size_t)width * sizeof obj[0]);
    if (l->tm & 0x10 || l->ts & 0x10) {
        NativeSprites(obj, width, column, l, vram, y0, s_hud_skip);
        /* Margins: every piece the game submitted, last first (OAM order).
         * Screen-fixed pieces belong to the native picture only. */
        for (int i = (int)s_frame.piece_count - 1; i >= 0; i--) {
            const Piece *pc = &s_frame.pieces[i];
            int row = y0 - pc->y;
            if (pc->fixed || row < 0 || row >= pc->size) continue;
            s_stats.margin_sprite_pixels +=
                SpriteRow(obj, width, column, l, vram, pc->x, row, pc->size, pc->attr, -column, 0) +
                SpriteRow(obj, width, column, l, vram, pc->x, row, pc->size, pc->attr, 256, width - column);
        }
        /* The HUD: screen-fixed objects, then the slots reserved before the
         * object pass (lives), placed by the view, not by the camera. */
        for (int i = (int)s_frame.piece_count - 1; i >= 0; i--) {
            const Piece *pc = &s_frame.pieces[i];
            int row = y0 - pc->y;
            if (!pc->fixed || !pc->kept || !s_hud_skip[pc->slot & 127] || row < 0 || row >= pc->size) continue;
            SpriteRow(obj, width, column, l, vram, pc->x + HudShift(s_hud_side[pc->slot & 127], width, column), row,
                      pc->size, pc->attr, -column, width - column);
        }
        for (int s = s_frame.hud_end - 1; s >= 0; s--) {
            unsigned idx = (unsigned)s * 2;
            int size = OamSize(l, idx), x = OamX(l, idx);
            int row = (uint8_t)(y0 - (l->oam[idx] >> 8));
            if (!s_hud_skip[s] || row >= size) continue;
            int side = s_hud_edges ? HudSide(x + size / 2) : 0;
            SpriteRow(obj, width, column, l, vram, x + HudShift(side, width, column), row, size, l->oam[idx + 1],
                      -column, width - column);
        }
    }

    uint8_t mult[63], half[64];
    unsigned bright = l->inidisp & 15;
    for (int i = 0; i < 32; i++) mult[i] = (uint8_t)(((i << 3) | (i >> 2)) * bright / 15);
    for (int i = 32; i < 63; i++) mult[i] = mult[31];
    for (int i = 0; i < 64; i++) half[i] = mult[i >> 1];
    const unsigned clip_mode = l->cgwsel >> 6, prevent = (l->cgwsel >> 4) & 3;
    const bool add_sub = (l->cgwsel & 2) != 0, subtract = (l->cgadsub & 0x80) != 0;
    const bool halve = (l->cgadsub & 0x40) != 0;
    const bool need_sub = add_sub && (l->cgadsub & 0x3F) && prevent != 3 && l->ts;
    int ox = 0, oy = 0;
    Bg1Origin(l, y0, &ox, &oy);
    const bool map_left = s_map_left, map_right = s_map_right;
    /* A layer that fills the native line but is nearly empty in the margins
     * (the sunset's sky: BG2 static, the other half of its 512-wide tilemap
     * unused but for a few stray tiles) shows the native columns mirrored
     * there instead. */
    bool mirror[2] = { false, false };
    for (unsigned layer = 0; layer < 2; layer++) {
        if (!((l->tm | (need_sub ? l->ts : 0)) & (1u << layer))) continue;
        unsigned native = 0, margin = 0, samples = 0;
        for (int x = 0; x < 256; x += 4) native += BgPixel(l, vram, layer, x, y0, false, ox, oy) != 0;
        const int sides[2][2] = { { -column, 0 }, { 256, width - column } };
        for (int side = 0; side < 2; side++) {
            if (layer == 0 && (side ? map_right : map_left)) continue;   /* drawn from the level map */
            for (int x = sides[side][0]; x < sides[side][1]; x += 2, samples++)
                margin += BgPixel(l, vram, layer, x, y0, false, ox, oy) != 0;
        }
        mirror[layer] = samples && margin * 10 <= samples && native * 10 >= 64 * 9;
    }

    for (int i = 0; i < width; i++) {
        int x = i - column;
        uint16_t main = 0x0500, sub = 0x0500;   /* backdrop: layer 5, colour 0 */
        /* Past where the camera stops: nothing of the level, black. */
        bool outside = i < s_vis_lo || i >= s_vis_hi;
        if (outside && !obj[i]) { out[i] = 0; continue; }
        for (unsigned layer = 0; layer < 3 && !outside; layer++) {
            unsigned bit = 1u << layer;
            if (!((l->tm | (need_sub ? l->ts : 0)) & bit)) continue;
            bool in_margin = x < 0 || x >= 256;
            uint16_t px = layer < 2 && mirror[layer] && in_margin
                              ? BgPixel(l, vram, layer, Reflect(x, 256), y0, false, ox, oy)
                              : BgPixel(l, vram, layer, x, y0, x < 0 ? map_left : x >= 256 && map_right, ox, oy);
            if (!px) continue;
            if ((l->tm & bit) && (px >> 12) > (main >> 12) && !((l->tmw & bit) && Window(l, layer, x, column, width)))
                main = px;
            if (need_sub && (l->ts & bit) && (px >> 12) > (sub >> 12) &&
                !((l->tsw & bit) && Window(l, layer, x, column, width)))
                sub = px;
        }
        uint16_t o = obj[i];
        if (o) {
            if ((l->tm & 0x10) && (o >> 12) > (main >> 12) && !((l->tmw & 0x10) && Window(l, 4, x, column, width)))
                main = o;
            if (need_sub && (l->ts & 0x10) && (o >> 12) > (sub >> 12) &&
                !((l->tsw & 0x10) && Window(l, 4, x, column, width)))
                sub = o;
        }
        bool inside = (clip_mode == 1 || clip_mode == 2 || prevent == 1 || prevent == 2) && Window(l, 5, x, column, width);
        bool keep = clip_mode == 0 || (clip_mode == 1 && inside) || (clip_mode == 2 && !inside);
        bool math_ok = prevent == 0 || (prevent == 1 && inside) || (prevent == 2 && !inside);
        unsigned mask = keep ? 31 : 0, c = l->cgram[main & 255], layer = (main >> 8) & 15;
        unsigned r = c & mask, g = (c >> 5) & mask, b = (c >> 10) & mask;
        const uint8_t *map = mult;
        if (math_ok && (l->cgadsub & 0x3F & (1u << layer))) {
            unsigned c2;
            if (add_sub && (sub & 0xFF)) { c2 = l->cgram[sub & 255]; if (halve) map = half; }
            else if (add_sub) c2 = l->fixed;
            else { c2 = l->fixed; if (halve) map = half; }
            unsigned r2 = c2 & 31, g2 = (c2 >> 5) & 31, b2 = (c2 >> 10) & 31;
            if (subtract) {
                r = r >= r2 ? r - r2 : 0; g = g >= g2 ? g - g2 : 0; b = b >= b2 ? b - b2 : 0;
            } else {
                r += r2; g += g2; b += b2;
            }
        }
        out[i] = (uint32_t)map[r] << 16 | (uint32_t)map[g] << 8 | map[b];
    }
}

/* ── API ───────────────────────────────────────────────────────────────── */

void TlkWideSetRom(const uint8_t *rom, size_t size) { s_rom = rom; s_rom_size = size; }

void TlkWideEnable(bool on) {
    s_enabled = on;
    if (on) ArmInterpreterHooks();
    g_tlk_wide_pc_hook = on || s_spawn_lead ? Observe : NULL;
    g_tlk_raster_observer = on ? CaptureLine : NULL;
    if (!on) TlkWideReset();
}

bool TlkWideEnabled(void) { return s_enabled; }

void TlkWideSetSpawnLead(int pixels) {
    s_spawn_lead = pixels > 0 ? pixels : 0;
    if (s_spawn_lead) {
        ArmInterpreterHooks();
        g_tlk_wide_pc_hook = Observe;
    }
}

void TlkWideReset(void) {
    s_build_count = s_latched_count = 0;
    s_frame.valid = false;
    s_frame.captured = 0;
    s_vram_have = false;
    s_have_last = false;
}

bool TlkWideEndFrame(const uint8_t *stock) {
    if (!s_enabled) return false;
    ArmInterpreterHooks();   /* idempotent; the bridge's table is runtime state */
    if (s_frame.captured != TLK_WIDE_HEIGHT || !stock) return s_frame.valid = false;
    memcpy(s_frame.stock, stock, sizeof s_frame.stock);
    s_frame.stage = DetectStage();
    return s_frame.valid = true;
}

bool TlkWideDraw(uint32_t *out, int width, bool hud_edges) {
    if (!out || !s_frame.valid || width < 256 || width > TLK_WIDE_MAX_WIDTH || (width & 1)) return false;
    int extra = (width - 256) / 2;
    s_draw_extra = extra;
    memset(&s_stats, 0, sizeof s_stats);
    s_stats.stage = s_frame.stage;
    s_stats.pieces = s_frame.piece_count;
    /* A side takes its margin from the level map only if the map matches the
     * native picture at that edge on every line; else from the PPU's own
     * tilemap ring, as the hardware would show it. */
    s_map_left = s_map_right = s_frame.stage;
    for (int y = 0; y < TLK_WIDE_HEIGHT && (s_map_left || s_map_right); y++) {
        const Line *l = &s_frame.lines[y];
        if (!LineHasStage(l) || (l->inidisp & 0x80) || !(l->tm & 1)) continue;
        const uint16_t *vram = s_vram[l->vram];
        int ox, oy;
        Bg1Origin(l, y, &ox, &oy);
        s_map_left = s_map_left && SeamAgrees(l, vram, y, ox, oy, 0) && SeamAgrees(l, vram, y, ox, oy, 8);
        s_map_right = s_map_right && SeamAgrees(l, vram, y, ox, oy, 247) && SeamAgrees(l, vram, y, ox, oy, 255);
    }
    /* The view stops where the camera stops: at a level's ends the native
     * picture moves off centre instead of the margin showing past the end. */
    int column = extra;
    s_vis_lo = 0;
    s_vis_hi = width;
    for (int y = 0; y < TLK_WIDE_HEIGHT && s_frame.stage; y++) {
        const Line *l = &s_frame.lines[y];
        if (!LineHasStage(l) || (l->inidisp & 0x80)) continue;
        int ox, oy;
        Bg1Origin(l, y, &ox, &oy);
        column = ViewColumn(s_frame.ram, ox, width, &s_vis_lo, &s_vis_hi);
        break;
    }
    s_stats.column = column;
    /* HUD sprites the HUD pass places: at the edges, or where they sit in a
     * centred picture when the native one is off centre. */
    memset(s_hud_skip, 0, sizeof s_hud_skip);
    memset(s_hud_side, 0, sizeof s_hud_side);
    s_hud_edges = hud_edges;   /* the reserved slots' side comes from each sprite's x, per line */
    for (int i = 0; i < s_frame.hud_end; i++) s_hud_skip[i] = hud_edges || column != extra;
    for (unsigned i = 0; i < s_frame.piece_count; i++) {
        const Piece *pc = &s_frame.pieces[i];
        int side = hud_edges ? pc->side : 0;
        if (pc->fixed && pc->kept && HudShift(side, width, column)) {
            s_hud_skip[pc->slot & 127] = true;
            s_hud_side[pc->slot & 127] = (int8_t)side;
        }
    }
    for (int y = 0; y < TLK_WIDE_HEIGHT; y++) {
        uint32_t *row = out + (size_t)y * width;
        const uint32_t *stock = s_frame.stock + y * 256;
        if (!s_frame.stage || !LineHasStage(&s_frame.lines[y])) {
            memset(row, 0, (size_t)width * 4);
            memcpy(row + column, stock, 256 * 4);
            s_stats.fallback_lines++;
            continue;
        }
        ComposeLine(row, width, column, y);
        s_stats.custom_lines++;
        /* The oracle: with the HUD where the game put it, the native columns
         * are the PPU's picture. */
        if (!hud_edges && column == extra)
            for (int x = 0; x < 256; x++) s_stats.native_mismatch += (row[column + x] & 0xFFFFFF) != (stock[x] & 0xFFFFFF);
    }
    return true;
}

TlkWideStats TlkWideGetStats(void) { return s_stats; }
