/* tlk_scanlines.c -- the scanline overlay's column. */
#include "tlk_scanlines.h"

void TlkScanlineColumn(uint32_t *col, int h, int lines, int pct) {
    const uint32_t gap = (uint32_t)(pct * 255 / 100) << 24;
    /* A third of the shortest line's rows. Lines differ by a row when h isn't a multiple of lines, and the beam takes it. */
    const int rows = (h / lines + 2) / 3;
    for (int i = 0; i < lines; i++) {   /* line i starts at ceil(i * h / lines - 0.5) */
        int y0 = (2 * i * h + lines - 1) / (2 * lines), y1 = (2 * (i + 1) * h + lines - 1) / (2 * lines);
        int dark = y1 - rows;
        for (int y = y0; y < y1; y++) col[y] = y < dark ? 0 : gap;
    }
}
