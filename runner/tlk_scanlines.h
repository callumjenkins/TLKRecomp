/* tlk_scanlines.h -- the scanline overlay's column. */
#pragma once

#include <stdint.h>

/* Fills col[0..h) for a picture h rows tall showing `lines` of the game's lines: black at pct percent alpha on the
 * dark rows at the bottom of each line, clear elsewhere. A line's rows are the ones nearest sampling gives it. */
void TlkScanlineColumn(uint32_t *col, int h, int lines, int pct);
