/* tlk_wide.h -- widescreen for The Lion King, drawn by a host compositor.
 *
 * The technique of MegaManXSNESRecomp's adaptive renderer: the PPU stays 256
 * pixels wide and authentic, and a separate compositor draws a wider picture
 * from snapshots of each visible line's PPU state, taken while the field is
 * rasterized. The margins come from data the game already keeps:
 *
 *   BG1  the level map at $7F0000 and the ROM block table. The native BG1
 *        tilemap is a 512x256 ring holding only what the camera streamed in.
 *   BG2  its tilemap is a static 512-pixel loop per level; the margins sample
 *        it with the live per-line scroll, as the PPU would.
 *   OBJ  every piece the game submits ($C0A727 16x16, $C0A7AE 8x8), recorded
 *        with signed coordinates before the game drops the ones off screen.
 *
 * Presentation only: nothing here writes guest state, so frame and WRAM
 * hashes, tapes and save states are the same with it on or off. Screens that
 * are not a scrolling level (title, menus, Stampede, transitions) show the
 * native picture centred. By default enemies still spawn where the game
 * spawns them; TlkWideSetSpawnLead (opt-in, changes the game) widens the
 * game's spawn window and its object draw test by the margin. */
#ifndef TLK_WIDE_H
#define TLK_WIDE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { TLK_WIDE_HEIGHT = 224, TLK_WIDE_MAX_WIDTH = 1024 };

typedef struct TlkWideStats {
    unsigned custom_lines, fallback_lines;
    unsigned pieces, margin_sprite_pixels;
    unsigned native_mismatch;   /* centre pixels that differ from the PPU's (HUD in place, view centred) */
    int column;                 /* the native picture's column: (width - 256) / 2 unless at a level's end */
    bool stage;                 /* BG1 matched the level map this frame */
} TlkWideStats;

void TlkWideSetRom(const uint8_t *rom, size_t size);
/* Arms the hooks (raster observer, piece recorder). Off costs nothing. */
void TlkWideEnable(bool on);
bool TlkWideEnabled(void);
/* After a reset or state load: drop recorded pieces and the last frame. */
void TlkWideReset(void);
/* After draw_ppu_frame: the PPU's own 256x224 XRGB8888 field. Returns
 * whether every visible line was captured. */
bool TlkWideEndFrame(const uint8_t *stock);
/* Draws width x 224 XRGB8888 into out. width is even, 256..TLK_WIDE_MAX_WIDTH;
 * the native picture sits at (width - 256) / 2, or nearer a side where the
 * game's camera stops (stats.column), so the view never shows past a level's
 * end. hud_edges moves the HUD sprites out to the picture's edges; otherwise
 * they stay where a centred native picture has them. */
bool TlkWideDraw(uint32_t *out, int width, bool hud_edges);
TlkWideStats TlkWideGetStats(void);
/* Opt-in, and it changes the game: widen the game's enemy spawn/despawn
 * window by this many pixels on each side (the picture's margin), so enemies
 * are created beyond the wider view as they are beyond the native one. 0 (the
 * default) leaves the game untouched; hashes and tapes only hold at 0. */
void TlkWideSetSpawnLead(int pixels);

#endif
