/* Diagnostics (task AA): TLK_CALL_PAIR=<path> records, per return site, which (call widths -> return widths)
 * pairs the interpreter saw. Evidence for exit widths that check_exit_mx.py cannot walk (callbacks through
 * function pointers, pushed frames). Calls: JSR abs / JSR (abs,X) / JSL, and the pushed-frame call
 * `PHK / PEA r` (returns to r+1 when S is back to its value before the PHK). A call is paired when an opcode
 * runs at its return PC with S back to the S before the call. Run with --lle. Output lines:
 * `RET24 MASK` with bit (4*cw + rw) set, w = 2m+x. Registers are synced before each opcode
 * (SNESRECOMP_WLOG_STATE, as in tlk_rommap.c). */
#include "cpu_state.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

extern uint8_t g_ram[];

static const uint8_t *s_rom;
static size_t s_rom_size;
static const char *s_path;
static uint16_t *s_mask;   /* per 24-bit return PC */

typedef struct { uint16_t s; uint32_t ret; uint8_t w; } Frame;
static Frame s_stk[1024];
static int s_n;

static long RomOff(uint32_t a) {
    uint8_t b = (uint8_t)(a >> 16); uint16_t o = (uint16_t)a;
    if (b == 0x7E || b == 0x7F) return -1;
    if ((b & 0x7F) >= 0x40 || o >= 0x8000) return (long)(((uint32_t)(b & 0x3F) << 16) | o) % (long)s_rom_size;
    return -1;
}
static int Rd8(uint32_t a) { long r = RomOff(a); return r >= 0 ? s_rom[r] : -1; }

void TlkCallPairInit(const uint8_t *rom, size_t size) {
    s_path = getenv("TLK_CALL_PAIR");
    if (!s_path) return;
    s_rom = rom; s_rom_size = size;
    s_mask = (uint16_t *)calloc(1u << 24, 2);
    setenv("SNESRECOMP_WLOG_STATE", "1", 1);
}

void TlkCallPairOpcode(uint32_t pc, int m, int x) {
    if (!s_mask) return;
    uint16_t s = g_cpu.S;
    uint8_t w = (uint8_t)((m ? 2 : 0) + (x ? 1 : 0));
    /* Drop frames whose S was released without returning to their return PC (stack resets, unwinds). */
    while (s_n && s > s_stk[s_n - 1].s) s_n--;
    if (s_n && s == s_stk[s_n - 1].s && pc == s_stk[s_n - 1].ret) {
        s_mask[pc] |= (uint16_t)(1u << (4 * s_stk[s_n - 1].w + w));
        s_n--;
    }
    int op = Rd8(pc);
    if (op < 0) return;
    uint32_t bank = pc & 0xFF0000, ret = 0;
    if (op == 0x20 || op == 0xFC) ret = bank | (uint16_t)(pc + 3);
    else if (op == 0x22) ret = bank | (uint16_t)(pc + 4);
    else if (op == 0x4B && Rd8(bank | (uint16_t)(pc + 1)) == 0xF4) {
        int lo = Rd8(bank | (uint16_t)(pc + 2)), hi = Rd8(bank | (uint16_t)(pc + 3));
        if (lo >= 0 && hi >= 0) ret = bank | (uint16_t)(((hi << 8) | lo) + 1);
    }
    if (ret && s_n < (int)(sizeof s_stk / sizeof s_stk[0])) {
        /* Same S as an open frame (a frame-reusing call, e.g. a tail jump that calls again): replace it. */
        while (s_n && s_stk[s_n - 1].s == s) s_n--;
        s_stk[s_n].s = s; s_stk[s_n].ret = ret; s_stk[s_n].w = w; s_n++;
    }
}

void TlkCallPairWrite(void) {
    if (!s_mask) return;
    FILE *f = fopen(s_path, "w");
    if (!f) return;
    for (uint32_t pc = 0; pc < (1u << 24); pc++)
        if (s_mask[pc]) fprintf(f, "%06X %u\n", (unsigned)pc, (unsigned)s_mask[pc]);
    fclose(f);
}
