/* Diagnostics (task T): TLK_ROMMAP=<path> writes a 3 MiB map, one flag byte per ROM byte, of what the
 * interpreter touched. Run with --lle so (almost) everything is interpreted; compiled interrupt bodies
 * are not seen. Flags (OR-ed; a second run's map can be OR-ed in by tools/rom_accounting.py):
 *   0x01 opcode byte executed        0x02 operand byte of an executed instruction
 *   0x04 data read by a CPU load     0x08 DMA source (VRAM)   0x10 DMA source (other B-bus target)
 *   0x20 data read by the decompressor $C1CCFE-$C1CEFF        0x40 data read by the sound library
 *        ($C21924-$C223FF, SPC uploads)
 * Instruction length uses the m/x flags of that execution. Effective addresses use the CPU registers,
 * synced before each opcode (SNESRECOMP_WLOG_STATE, set by TlkRomMapInit). Indirect pointers are read
 * from WRAM / ROM; only reads that land in ROM are marked. */
#include "cpu_state.h"
#include "snes/dma.h"
#include "snes/snes.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint8_t g_ram[];
extern Snes *g_snes;

static uint8_t *s_map;
static const uint8_t *s_rom;
static size_t s_rom_size;
static const char *s_path;

/* HiROM: $40-$7D and $C0-$FF full banks; $00-$3F/$80-$BF upper half. Returns -1 if not ROM. */
static long RomOff(uint32_t a) {
    uint8_t b = (uint8_t)(a >> 16); uint16_t o = (uint16_t)a;
    if (b == 0x7E || b == 0x7F) return -1;
    if ((b & 0x7F) >= 0x40) return (long)(((uint32_t)(b & 0x3F) << 16) | o) % (long)s_rom_size;
    if (o >= 0x8000) return (long)(((uint32_t)(b & 0x3F) << 16) | o) % (long)s_rom_size;
    return -1;
}

static uint8_t Rd8(uint32_t a) {
    long r = RomOff(a);
    if (r >= 0) return s_rom[r];
    uint8_t b = (uint8_t)(a >> 16); uint16_t o = (uint16_t)a;
    if (b == 0x7E || b == 0x7F) return g_ram[((b & 1) << 16) | o];
    if ((b & 0x7F) < 0x40 && o < 0x2000) return g_ram[o];
    return 0;
}

static void Mark(uint32_t a, int n, uint8_t f) {
    for (int i = 0; i < n; i++) {
        long r = RomOff((a & 0xFF0000) | (uint16_t)(a + i));
        if (r >= 0) s_map[r] |= f;
    }
}

void TlkRomMapVram(uint8_t bank, uint16_t adr, uint16_t vmadd, uint16_t size) {
    (void)vmadd;
    if (!s_map) return;
    uint32_t n = size ? size : 0x10000;
    for (uint32_t i = 0; i < n; i++) { long r = RomOff(((uint32_t)bank << 16) | (uint16_t)(adr + i)); if (r >= 0) s_map[r] |= 0x08; }
}

void TlkRomMapInit(const uint8_t *rom, size_t size) {
    s_path = getenv("TLK_ROMMAP");
    if (!s_path) return;
    s_rom = rom; s_rom_size = size;
    s_map = (uint8_t *)calloc(size, 1);
    setenv("SNESRECOMP_WLOG_STATE", "1", 1);
    dma_set_vram_notify_hook(TlkRomMapVram);
}

void TlkRomMapWrite(void) {
    if (!s_map) return;
    FILE *f = fopen(s_path, "wb");
    if (f) { fwrite(s_map, 1, s_rom_size, f); fclose(f); }
}

/* Addressing modes. */
enum { IMP, IMM_M, IMM_X, IMM8, DP, DPX, DPY, DPI, DPIX, DPIY, DPIL, DPILY, ABS, ABSX, ABSY, LNG, LNGX,
       SR, SRIY, REL, RELL, ABSI, ABSIX, ABSIL, BLK, PEA_, JABS, JLNG };
static const uint8_t kMode[256] = {
/*0x*/ IMM8,DPIX,IMM8,SR,DP,DP,DP,DPIL,IMP,IMM_M,IMP,IMP,ABS,ABS,ABS,LNG,
/*1x*/ REL,DPIY,DPI,SRIY,DP,DPX,DPX,DPILY,IMP,ABSY,IMP,IMP,ABS,ABSX,ABSX,LNGX,
/*2x*/ JABS,DPIX,JLNG,SR,DP,DP,DP,DPIL,IMP,IMM_M,IMP,IMP,ABS,ABS,ABS,LNG,
/*3x*/ REL,DPIY,DPI,SRIY,DPX,DPX,DPX,DPILY,IMP,ABSY,IMP,IMP,ABSX,ABSX,ABSX,LNGX,
/*4x*/ IMP,DPIX,IMM8,SR,BLK,DP,DP,DPIL,IMP,IMM_M,IMP,IMP,JABS,ABS,ABS,LNG,
/*5x*/ REL,DPIY,DPI,SRIY,BLK,DPX,DPX,DPILY,IMP,ABSY,IMP,IMP,JLNG,ABSX,ABSX,LNGX,
/*6x*/ IMP,DPIX,RELL,SR,DP,DP,DP,DPIL,IMP,IMM_M,IMP,IMP,ABSI,ABS,ABS,LNG,
/*7x*/ REL,DPIY,DPI,SRIY,DPX,DPX,DPX,DPILY,IMP,ABSY,IMP,IMP,ABSIX,ABSX,ABSX,LNGX,
/*8x*/ REL,DPIX,RELL,SR,DP,DP,DP,DPIL,IMP,IMM_M,IMP,IMP,ABS,ABS,ABS,LNG,
/*9x*/ REL,DPIY,DPI,SRIY,DPX,DPX,DPY,DPILY,IMP,ABSY,IMP,IMP,ABS,ABSX,ABSX,LNGX,
/*Ax*/ IMM_X,DPIX,IMM_X,SR,DP,DP,DP,DPIL,IMP,IMM_M,IMP,IMP,ABS,ABS,ABS,LNG,
/*Bx*/ REL,DPIY,DPI,SRIY,DPX,DPX,DPY,DPILY,IMP,ABSY,IMP,IMP,ABSX,ABSX,ABSY,LNGX,
/*Cx*/ IMM_X,DPIX,IMM8,SR,DP,DP,DP,DPIL,IMP,IMM_M,IMP,IMP,ABS,ABS,ABS,LNG,
/*Dx*/ REL,DPIY,DPI,SRIY,DP,DPX,DPX,DPILY,IMP,ABSY,IMP,IMP,ABSIL,ABSX,ABSX,LNGX,
/*Ex*/ IMM_X,DPIX,IMM8,SR,DP,DP,DP,DPIL,IMP,IMM_M,IMP,IMP,ABS,ABS,ABS,LNG,
/*Fx*/ REL,DPIY,PEA_,SRIY,PEA_,DPX,DPX,DPILY,IMP,ABSY,IMP,IMP,ABSIX,ABSX,ABSX,LNGX,
};
/* PEA_ is used for $F4 (PEA abs, 3 bytes) and $D4 (PEI dp); $F2 is SBC (dp) -> fix below. */

static int Len(uint8_t op, int mode, int m, int x) {
    switch (mode) {
    case IMP: return 1;
    case IMM_M: return m ? 2 : 3;
    case IMM_X: return x ? 2 : 3;
    case IMM8: case DP: case DPX: case DPY: case DPI: case DPIX: case DPIY: case DPIL: case DPILY:
    case SR: case SRIY: case REL: return 2;
    case LNG: case LNGX: case JLNG: return 4;
    case PEA_: return op == 0xF4 ? 3 : 2;
    default: return 3;
    }
}

/* Opcodes that read memory as data (loads, ALU, compares, BIT, and the RMW ops). */
static int ReadsData(uint8_t op) {
    uint8_t lo = op & 0x1F, hi = op >> 5;
    if ((op & 3) == 1 || (op & 3) == 3) return hi != 4;          /* ORA AND EOR ADC (STA) LDA CMP SBC */
    if ((op & 3) == 2 && (lo == 0x12)) return hi != 4;           /* (dp) forms */
    switch (op) {
    case 0xA2: case 0xA6: case 0xB6: case 0xAE: case 0xBE:       /* LDX */
    case 0xA0: case 0xA4: case 0xB4: case 0xAC: case 0xBC:       /* LDY */
    case 0xE4: case 0xEC: case 0xC4: case 0xCC:                  /* CPX CPY */
    case 0x24: case 0x34: case 0x2C: case 0x3C:                  /* BIT */
        return 1;
    }
    return 0;
}

static int WidthX(uint8_t op) {
    switch (op) { case 0xA6: case 0xB6: case 0xAE: case 0xBE: case 0xA4: case 0xB4: case 0xAC: case 0xBC:
                  case 0xE4: case 0xEC: case 0xC4: case 0xCC: return 1; }
    return 0;
}

static uint32_t Ptr16(uint32_t a) { return Rd8(a) | (uint32_t)Rd8((a & 0xFF0000) | (uint16_t)(a + 1)) << 8; }

void TlkRomMapOpcode(uint32_t pc, int m, int x) {
    if (!s_map) return;
    const CpuState *c = &g_cpu;
    uint8_t op = Rd8(pc);
    int mode = kMode[op];
    if (op == 0xF2) mode = DPI;
    if (op == 0xD4) mode = DP;
    int len = Len(op, mode, m, x);
    Mark(pc, 1, 0x01);
    if (len > 1) Mark((pc & 0xFF0000) | (uint16_t)(pc + 1), len - 1, 0x02);
    uint32_t o1 = Rd8((pc & 0xFF0000) | (uint16_t)(pc + 1));
    uint32_t o2 = Rd8((pc & 0xFF0000) | (uint16_t)(pc + 2));
    uint32_t o3 = Rd8((pc & 0xFF0000) | (uint16_t)(pc + 3));
    uint32_t w16 = o1 | o2 << 8, db = (uint32_t)c->DB << 16;
    uint8_t flag = 0x04;
    uint32_t pc16 = pc & 0xFFFFFF;
    if (pc16 >= 0xC1CCFE && pc16 <= 0xC1CEFF) flag |= 0x20;
    if (pc16 >= 0xC21924 && pc16 <= 0xC223FF) flag |= 0x40;

    /* Jump tables: JMP/JSR (abs,X) read their pointer from the program bank. */
    if (mode == ABSIX) { Mark((pc & 0xFF0000) | (uint16_t)(w16 + c->X), 2, flag); return; }
    /* Block moves: MVN/MVP source bank = second operand byte, source = X, count = A+1. */
    if (mode == BLK) {
        uint32_t n = (uint32_t)c->A + 1, src = o2 << 16;
        for (uint32_t i = 0; i < n; i++) {
            uint16_t xx = op == 0x54 ? (uint16_t)(c->X + i) : (uint16_t)(c->X - i);
            long r = RomOff(src | xx); if (r >= 0) s_map[r] |= flag;
        }
        return;
    }
    /* DMA kick: STA/STX/STY $420B (abs or long). Mark every enabled channel's A-bus source. */
    if ((op == 0x8D || op == 0x8E || op == 0x8C || op == 0x8F) && w16 == 0x420B && g_snes && g_snes->dma) {
        uint8_t v = op == 0x8E ? (uint8_t)c->X : op == 0x8C ? (uint8_t)c->Y : (uint8_t)c->A;
        for (int ch = 0; ch < 8; ch++) if (v & (1 << ch)) {
            const DmaChannel *d = &g_snes->dma->channel[ch];
            if (d->fromB) continue;
            uint32_t n = d->size ? d->size : 0x10000;
            uint8_t f = (d->bAdr == 0x18 || d->bAdr == 0x19) ? 0x08 : 0x10;
            for (uint32_t i = 0; i < n; i++) {
                long r = RomOff(((uint32_t)d->aBank << 16) | (uint16_t)(d->aAdr + (d->fixed ? 0 : i)));
                if (r >= 0) s_map[r] |= f;
            }
        }
        return;
    }
    if (!ReadsData(op)) return;
    int w = WidthX(op) ? (x ? 1 : 2) : (m ? 1 : 2);
    uint32_t ea; uint16_t D = c->D;
    switch (mode) {
    case DP: ea = (uint16_t)(D + o1); break;
    case DPX: ea = (uint16_t)(D + o1 + c->X); break;
    case DPY: ea = (uint16_t)(D + o1 + c->Y); break;
    case DPI: ea = db | Ptr16((uint16_t)(D + o1)); break;
    case DPIX: ea = db | Ptr16((uint16_t)(D + o1 + c->X)); break;
    case DPIY: ea = ((db | Ptr16((uint16_t)(D + o1))) + c->Y) & 0xFFFFFF; break;
    case DPIL: ea = Ptr16((uint16_t)(D + o1)) | (uint32_t)Rd8((uint16_t)(D + o1 + 2)) << 16; break;
    case DPILY: ea = ((Ptr16((uint16_t)(D + o1)) | (uint32_t)Rd8((uint16_t)(D + o1 + 2)) << 16) + c->Y) & 0xFFFFFF; break;
    case ABS: ea = db | w16; break;
    case ABSX: ea = ((db | w16) + c->X) & 0xFFFFFF; break;
    case ABSY: ea = ((db | w16) + c->Y) & 0xFFFFFF; break;
    case LNG: ea = w16 | o3 << 16; break;
    case LNGX: ea = ((w16 | o3 << 16) + c->X) & 0xFFFFFF; break;
    case SRIY: ea = ((db | Ptr16((uint16_t)(c->S + o1))) + c->Y) & 0xFFFFFF; break;
    default: return; /* immediates, stack-relative (RAM) */
    }
    Mark(ea, w, flag);
}
