/* The scanline overlay: which screen rows of each game line are dark. */
#include "tlk_scanlines.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int s_failed;
#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            s_failed++;                                                       \
            fprintf(stderr, "%s:%d: %s: ", __FILE__, __LINE__, #cond);        \
            fprintf(stderr, __VA_ARGS__);                                     \
            fputc('\n', stderr);                                              \
        }                                                                     \
    } while (0)

enum { kLines = 224 };

int main(void) {
    for (int h = 2 * kLines; h <= 2400; h++) {
        uint32_t *col = malloc((size_t)h * sizeof *col);
        TlkScanlineColumn(col, h, kLines, 60);
        int first_gap = -1, bad = 0;
        for (int i = 0, y = 0; i < kLines && !bad; i++) {
            int clear = 0, gap = 0;
            /* the rows nearest sampling maps to line i */
            for (; y < h && (int)floor((y + 0.5) * kLines / h) == i; y++) {
                if (col[y] == 0) { bad |= gap > 0; clear++; }   /* clear after dark: the gap isn't at the bottom */
                else { bad |= col[y] != (uint32_t)(60 * 255 / 100) << 24; gap++; }
            }
            if (first_gap < 0) first_gap = gap;
            bad |= gap != first_gap || clear == 0 || gap == 0;
            if (bad) CHECK(0, "h=%d line %d: %d dark rows, %d clear, first line had %d dark", h, i, gap, clear, first_gap);
        }
        free(col);
    }
    if (s_failed) fprintf(stderr, "%d failed\n", s_failed);
    else printf("tlk_scanlines: ok\n");
    return s_failed != 0;
}
