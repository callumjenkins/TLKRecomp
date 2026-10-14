/* tlk_savestate.c -- savestates for both hosts (task S, docs/devlog.md).
 *
 * File layout (little-endian, this build only: the blob is the runtime's
 * in-process rollback snapshot, which has no cross-build promise):
 *   "TLKSTAT1"  u32 version  u32 host_frame
 *   u32 resume_pc  u8 wai_halted  u8 short_field  u8 late_nmi  u8 pad
 *   u64 blob_len  blob (RtlRollbackSaveToMemory: guest snapshot + timing residue,
 *                       incl. g_cpu, interp bridge state, refresh carry, PPU/DMA residue)
 *
 * Taken between frames only. A fiber suspended mid-routine (task Q) cannot be
 * serialized; on load it is discarded and the driver resumes at the saved PC,
 * which exact pacing (task M) makes indistinguishable from the in-place resume. */
#include "tlk_savestate.h"

#include "common_rtl.h"
#include "cpu_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t tlk_bfd_resume_pc_get(void);
void tlk_bfd_resume_pc_set(uint32_t pc);
int tlk_bfd_wai_halted_get(void);
void tlk_bfd_wai_halted_set(int v);
int tlk_snes_short_field_get(void);
void tlk_snes_short_field_set(int v);
int tlk_snes_late_nmi_get(void);
void tlk_snes_late_nmi_set(int v);
void tlk_fiber_discard(void);
extern uint64_t g_aot_pace_next;

#define TLKS_MAGIC "TLKSTAT1"
#define TLKS_VERSION 1u

typedef struct {
    char magic[8];
    uint32_t version, host_frame;
    uint32_t resume_pc;
    uint8_t wai_halted, short_field, late_nmi, pad;
    uint64_t blob_len;
} TlkStateHeader;

int tlk_state_save(const char *path, uint32_t host_frame) {
    size_t cap = RtlRollbackSnapshotBound();
    uint8_t *blob = (uint8_t *)malloc(cap);
    if (!blob) return 0;
    size_t n = RtlRollbackSaveToMemory(blob, cap);
    if (!n) { free(blob); return 0; }
    TlkStateHeader h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, TLKS_MAGIC, 8);
    h.version = TLKS_VERSION;
    h.host_frame = host_frame;
    h.resume_pc = tlk_bfd_resume_pc_get();
    h.wai_halted = (uint8_t)tlk_bfd_wai_halted_get();
    h.short_field = (uint8_t)tlk_snes_short_field_get();
    h.late_nmi = (uint8_t)tlk_snes_late_nmi_get();
    h.blob_len = n;
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    int ok = f && fwrite(&h, sizeof h, 1, f) == 1 && fwrite(blob, 1, n, f) == n;
    if (f && fclose(f) != 0) ok = 0;
    free(blob);
    if (ok) ok = rename(tmp, path) == 0;
    if (!ok) remove(tmp);
    return ok;
}

int tlk_state_load(const char *path, uint32_t *host_frame) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    TlkStateHeader h;
    int ok = fread(&h, sizeof h, 1, f) == 1 && !memcmp(h.magic, TLKS_MAGIC, 8) &&
             h.version == TLKS_VERSION && h.blob_len > 0 && h.blob_len < (64u << 20);
    uint8_t *blob = ok ? (uint8_t *)malloc((size_t)h.blob_len) : NULL;
    ok = ok && blob && fread(blob, 1, (size_t)h.blob_len, f) == h.blob_len;
    fclose(f);
    if (!ok) { free(blob); return 0; }
    tlk_fiber_discard();
    ok = RtlRollbackLoadFromMemory(blob, (size_t)h.blob_len);
    free(blob);
    if (!ok) return 0;
    tlk_bfd_resume_pc_set(h.resume_pc);
    tlk_bfd_wai_halted_set(h.wai_halted);
    tlk_snes_short_field_set(h.short_field);
    tlk_snes_late_nmi_set(h.late_nmi);
    g_aot_pace_next = 0;
    if (host_frame) *host_frame = h.host_frame;
    return 1;
}
