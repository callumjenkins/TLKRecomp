/* headless_host.c -- host-contract symbols for the windowless TLK host.
 *
 * The framework leaves these to the frontend (see
 * snesrecomp/runner/src/desktop/host_main.h); tlk_sdl gets them from the
 * framework desktop host, tlk_headless from here. host_report_* are NOT
 * defined here: host_report.c is part of SNESRECOMP_RUNNER_SOURCES. */
#include "types.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

struct SpcPlayer;

bool g_new_ppu = true;
bool g_ws_active = false;
int g_ws_extra = 0;
struct SpcPlayer *g_spc_player = NULL;

void NORETURN Die(const char *error) {
    fprintf(stderr, "fatal: %s\n", error ? error : "unknown error");
    exit(EXIT_FAILURE);
}

void RtlApuLock(void) {}
void RtlApuUnlock(void) {}

/* The headless host reads the PPU field straight from its own buffer. */
void RtlDrawPpuFrame(uint8 *pixel_buffer, size_t pitch, uint32 render_flags) {
    (void)pixel_buffer; (void)pitch; (void)render_flags;
}

void MkDir(const char *s) { mkdir(s, 0755); }
void ChangeWindowScale(int scale_step) { (void)scale_step; }
