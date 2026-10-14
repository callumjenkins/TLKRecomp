/* tlk_decomp.c -- native execution of the $C1CCFE decompressor loop (and
 * other force_lle loading loops) at interpreter timing.
 *
 * $C1CCFE (custom LZ/RLE, see tools/re/decompress.py) is the hottest
 * interpreted code during level loads. It cannot be compiled as is:
 *   - its long-copy command calls a 4-byte routine it writes into WRAM on
 *     every call ($C1CD27-$C1CD3D: 54 bb bb 6B = MVN bb,bb / RTL at $7E2006);
 *   - more importantly, compiled blocks charge cycles as one lump per block at
 *     code-region speed, while the interpreter charges every bus transfer at
 *     its own region speed plus 6 master clocks per internal cycle. The
 *     decompressor runs for many frames, so the compiled body finishes a load
 *     about one frame earlier than --lle and the title screen animation
 *     drifts (task K, devlog.md).
 *
 * So $C1CCFE stays force_lle and this file accelerates its main loop from a
 * pre-opcode hook at $C1CD75 (the loop head every command returns to). The
 * hook executes the routine's own ROM bytes (and the MVN/RTL it wrote at
 * $7E2006) with the interpreter's exact semantics and cycle model, and stops
 * at the first instruction boundary where the interpreter step loop could
 * stop (master deadline reached, or IRQ asserted with I clear), or when the
 * PC leaves the loop. It then redirects the interpreter to that PC, so every
 * yield happens at the same instruction and master clock as in --lle.
 *
 * Bus accesses and the per-opcode tail (cycle charge, DRAM refresh, beam
 * sync, APU pending/flush, progress epochs) go through the bridge's own code,
 * exported by a runtime hunk, so the result is bit-identical to interpreting.
 *
 * Regions (kRegions): the decompressor command loop $C1CD43-$C1CE7E plus the
 * MVN/RTL at $7E2006-$7E2009 (prologue and the $C1CD83 epilogue stay
 * interpreted), and the same treatment for the loading loops of $C07189 and
 * $C084A0, which are force_lle for the same cycle-model reason. Any opcode not
 * handled in Step() stops the run, so an unexpected path falls back to the
 * interpreter instead of running wrong.
 *
 * Enabled in recompiled mode; off under --lle (SNESRECOMP_LLE_BOUNCE=0) so the
 * interpreter oracle is untouched. TLK_NATIVE_DECOMP=0/1 forces it, e.g. to
 * prove equivalence by running --lle with it on (scripts/check_native_decomp.sh).
 */
#include "tlk_game.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cpu_state.h"
#include "common_cpu_infra.h"
#include "snes/snes.h"
#include "snes/cart.h"
#include "snes/interp_bridge.h"

/* Runtime hunk exports (cmake/runtime-patches/interp_bridge.hunks, task K). */
uint64_t tlk_interp_bridge_master_deadline(void);
uint8_t tlk_bridge_read8(CpuState *cpu, uint32_t adr);
void tlk_bridge_write8(CpuState *cpu, uint32_t adr, uint8_t val);
uint16_t tlk_bridge_read16(CpuState *cpu, uint32_t adrl, uint32_t adrh);
void tlk_bridge_write16(CpuState *cpu, uint32_t adrl, uint32_t adrh, uint16_t val, int reversed);
int tlk_bridge_hook_auto_quiescent(void);
uint64_t tlk_bridge_dynamic_epoch(void);
void tlk_bridge_op_begin(CpuState *cpu);
void tlk_bridge_op_end(CpuState *cpu, int cyc, uint64_t *progress_write, uint64_t *progress_dynamic);
extern uint64_t g_interp_bridge_write_epoch;

/* Same as interp816.c cyclesPerOpcode. */
static const uint8_t kCycles[256] = {
  7, 6, 7, 4, 5, 3, 5, 6, 3, 2, 2, 4, 6, 4, 6, 5,
  2, 5, 5, 7, 5, 4, 6, 6, 2, 4, 2, 2, 6, 4, 7, 5,
  6, 6, 8, 4, 3, 3, 5, 6, 4, 2, 2, 5, 4, 4, 6, 5,
  2, 5, 5, 7, 4, 4, 6, 6, 2, 4, 2, 2, 4, 4, 7, 5,
  6, 6, 2, 4, 7, 3, 5, 6, 3, 2, 2, 3, 3, 4, 6, 5,
  2, 5, 5, 7, 7, 4, 6, 6, 2, 4, 3, 2, 4, 4, 7, 5,
  6, 6, 6, 4, 3, 3, 5, 6, 4, 2, 2, 6, 5, 4, 6, 5,
  2, 5, 5, 7, 4, 4, 6, 6, 2, 4, 4, 2, 6, 4, 7, 5,
  3, 6, 4, 4, 3, 3, 3, 6, 2, 2, 2, 3, 4, 4, 4, 5,
  2, 6, 5, 7, 4, 4, 4, 6, 2, 5, 2, 2, 4, 5, 5, 5,
  2, 6, 2, 4, 3, 3, 3, 6, 2, 2, 2, 4, 4, 4, 4, 5,
  2, 5, 5, 7, 4, 4, 4, 6, 2, 4, 2, 2, 4, 4, 4, 5,
  2, 6, 3, 4, 3, 3, 5, 6, 2, 2, 2, 3, 4, 4, 6, 5,
  2, 5, 5, 7, 6, 4, 6, 6, 2, 4, 3, 3, 6, 4, 7, 5,
  2, 6, 3, 4, 3, 3, 5, 6, 2, 2, 2, 3, 4, 4, 6, 5,
  2, 5, 5, 7, 5, 4, 6, 6, 2, 4, 4, 2, 8, 4, 7, 5
};

/* Interpreter-shaped register file (interp816.h field meanings). */
typedef struct {
    CpuState *cpu;
    uint16_t a, x, y, sp, dp, pc;
    uint8_t db, k;
    uint8_t c, z, v, n, i, d, mf, xf;
    int cyc;              /* cyclesUsed of the current opcode */
} M;

static uint64_t s_native_ops;      /* opcodes run here (diagnostic) */
static uint64_t s_native_runs;

uint64_t tlk_decomp_native_ops(void) { return s_native_ops; }
uint64_t tlk_decomp_native_runs(void) { return s_native_runs; }

/* Bus: the interpreter bridge's own callbacks (runtime hunk, task K), so
 * timing, epochs and APU-port handling are those of an interpreted opcode. */
static inline uint8_t Rd(M *m, uint32_t adr) { return tlk_bridge_read8(m->cpu, adr & 0xFFFFFF); }
static inline void Wr(M *m, uint32_t adr, uint8_t v) { tlk_bridge_write8(m->cpu, adr & 0xFFFFFF, v); }
static inline uint16_t RdW(M *m, uint32_t lo, uint32_t hi) { return tlk_bridge_read16(m->cpu, lo, hi); }
static inline void WrW(M *m, uint32_t lo, uint32_t hi, uint16_t v) { tlk_bridge_write16(m->cpu, lo, hi, v, 0); }
static inline uint8_t Op8(M *m) { return Rd(m, (uint32_t)m->k << 16 | m->pc++); }
static inline uint16_t Op16(M *m) { uint8_t l = Op8(m); return (uint16_t)(l | Op8(m) << 8); }
static inline void ZN(M *m, uint16_t v, int byte) {
    if (byte) { m->z = (v & 0xFF) == 0; m->n = (v & 0x80) != 0; }
    else      { m->z = v == 0;          m->n = (v & 0x8000) != 0; }
}
static inline void Push8(M *m, uint8_t v) { Wr(m, m->sp, v); m->sp--; }
static inline uint8_t Pull8(M *m) { m->sp++; return Rd(m, m->sp); }
static inline uint8_t Flags(const M *m) {
    return (uint8_t)(m->n << 7 | m->v << 6 | m->mf << 5 | m->xf << 4 |
                     m->d << 3 | m->i << 2 | m->z << 1 | m->c);
}
static inline void SetFlags(M *m, uint8_t p) {
    m->n = (p >> 7) & 1; m->v = (p >> 6) & 1; m->mf = (p >> 5) & 1; m->xf = (p >> 4) & 1;
    m->d = (p >> 3) & 1; m->i = (p >> 2) & 1; m->z = (p >> 1) & 1; m->c = p & 1;
    if (m->xf) { m->x &= 0xFF; m->y &= 0xFF; }
}
/* Operand address helpers (interp816 adr* functions). */
static inline uint32_t AdrDp(M *m) {
    uint8_t o = Op8(m);
    if (m->dp & 0xFF) m->cyc++;
    return (uint16_t)(m->dp + o);
}
static inline uint32_t AdrImm(M *m, int xflag) {
    uint32_t lo = (uint32_t)m->k << 16 | m->pc++;
    if (!((xflag && m->xf) || (!xflag && m->mf))) m->pc++;
    return lo;
}
/* Load a value of the current A width (or index width) from lo/lo+1. */
static inline uint16_t LoadM(M *m, uint32_t lo, uint32_t hi) {
    if (m->mf) return Rd(m, lo);
    m->cyc++;
    return RdW(m, lo, hi);
}
static inline uint16_t LoadX(M *m, uint32_t lo, uint32_t hi) {
    if (m->xf) return Rd(m, lo);
    m->cyc++;
    return RdW(m, lo, hi);
}
static inline void Adc(M *m, uint16_t v) {
    if (m->mf) {
        int r = (m->a & 0xFF) + (v & 0xFF) + m->c;
        m->v = (m->a & 0x80) == (v & 0x80) && (v & 0x80) != (r & 0x80);
        m->c = r > 0xFF;
        m->a = (uint16_t)((m->a & 0xFF00) | (r & 0xFF));
    } else {
        int r = m->a + v + m->c;
        m->v = (m->a & 0x8000) == (v & 0x8000) && (v & 0x8000) != (r & 0x8000);
        m->c = r > 0xFFFF;
        m->a = (uint16_t)r;
    }
    ZN(m, m->a, m->mf);
}

/* Run one opcode at m->pc. Returns 0 if the opcode is not handled here (the
 * state is untouched: nothing has been fetched yet). */
static int Step(M *m) {
    const uint8_t op = cpu_read8(m->cpu, m->k, m->pc);  /* peek; refetched below */
    uint32_t lo, hi;
    switch (op) {
    case 0x10: case 0x18: case 0x22: case 0x29: case 0x38: case 0x3A: case 0x48:
    case 0x49: case 0x4C: case 0x54: case 0x5A: case 0x65: case 0x68: case 0x69:
    case 0x6B: case 0x80: case 0x84: case 0x85: case 0x86: case 0x8A: case 0x8F:
    case 0x90: case 0x98: case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xA8:
    case 0xA9: case 0xAA: case 0xB0: case 0xB9: case 0xBF: case 0xC2: case 0xC8:
    case 0xC9: case 0xCA: case 0xD0: case 0xE0: case 0xE2: case 0xE6: case 0xE8:
    case 0xE9: case 0xEB: case 0xF0:
    case 0x0A: case 0x1A: case 0x4A: case 0x8D: case 0x99: case 0x9D: case 0x9E:
    case 0x9F: case 0xA0: case 0xA2: case 0xB7: case 0xBB: case 0xC0: case 0xC6:
        break;
    default:
        return 0;
    }
    tlk_bridge_op_begin(m->cpu);
    Op8(m);                       /* the opcode fetch */
    m->cyc = kCycles[op];
    switch (op) {
    case 0x10: { uint8_t o = Op8(m); if (!m->n) { m->cyc++; m->pc += (int8_t)o; } break; }
    case 0x90: { uint8_t o = Op8(m); if (!m->c) { m->cyc++; m->pc += (int8_t)o; } break; }
    case 0xB0: { uint8_t o = Op8(m); if (m->c) { m->cyc++; m->pc += (int8_t)o; } break; }
    case 0xD0: { uint8_t o = Op8(m); if (!m->z) { m->cyc++; m->pc += (int8_t)o; } break; }
    case 0xF0: { uint8_t o = Op8(m); if (m->z) { m->cyc++; m->pc += (int8_t)o; } break; }
    case 0x80: { uint8_t o = Op8(m); m->pc += (int8_t)o; break; }
    case 0x4C: m->pc = Op16(m); break;
    case 0x18: m->c = 0; break;
    case 0x38: m->c = 1; break;
    case 0xC2: SetFlags(m, Flags(m) & ~Op8(m)); break;
    case 0xE2: SetFlags(m, Flags(m) | Op8(m)); break;
    case 0x22: {  /* JSL long */
        uint16_t t = Op16(m);
        uint8_t nk = Op8(m);
        Push8(m, m->k);
        uint16_t ret = (uint16_t)(m->pc - 1);
        Push8(m, (uint8_t)(ret >> 8));
        Push8(m, (uint8_t)ret);
        m->pc = t; m->k = nk;
        break;
    }
    case 0x6B: {  /* RTL */
        uint8_t l = Pull8(m);
        uint16_t w = (uint16_t)(l | Pull8(m) << 8);
        m->pc = (uint16_t)(w + 1);
        m->k = Pull8(m);
        break;
    }
    case 0x54: {  /* MVN dest,src: one byte per execution */
        uint8_t dest = Op8(m), src = Op8(m);
        m->db = dest;
        uint8_t v = Rd(m, (uint32_t)src << 16 | m->x);
        Wr(m, (uint32_t)dest << 16 | m->y, v);
        m->a--; m->x++; m->y++;
        if (m->a != 0xFFFF) m->pc -= 3;
        if (m->xf) { m->x &= 0xFF; m->y &= 0xFF; }
        break;
    }
    case 0x48:  /* PHA */
        if (m->mf) Push8(m, (uint8_t)m->a);
        else { m->cyc++; Push8(m, (uint8_t)(m->a >> 8)); Push8(m, (uint8_t)m->a); }
        break;
    case 0x5A:  /* PHY */
        if (m->xf) Push8(m, (uint8_t)m->y);
        else { m->cyc++; Push8(m, (uint8_t)(m->y >> 8)); Push8(m, (uint8_t)m->y); }
        break;
    case 0x68:  /* PLA */
        if (m->mf) m->a = (uint16_t)((m->a & 0xFF00) | Pull8(m));
        else { m->cyc++; uint8_t l = Pull8(m); m->a = (uint16_t)(l | Pull8(m) << 8); }
        ZN(m, m->a, m->mf);
        break;
    case 0x3A:  /* DEC A */
        if (m->mf) m->a = (uint16_t)((m->a & 0xFF00) | ((m->a - 1) & 0xFF));
        else m->a--;
        ZN(m, m->a, m->mf);
        break;
    case 0x0A:  /* ASL A */
        if (m->mf) { m->c = (m->a >> 7) & 1; m->a = (uint16_t)((m->a & 0xFF00) | ((m->a << 1) & 0xFF)); }
        else { m->c = (m->a >> 15) & 1; m->a = (uint16_t)(m->a << 1); }
        ZN(m, m->a, m->mf);
        break;
    case 0x4A:  /* LSR A */
        m->c = m->a & 1;
        if (m->mf) m->a = (uint16_t)((m->a & 0xFF00) | ((m->a >> 1) & 0x7F));
        else m->a >>= 1;
        ZN(m, m->a, m->mf);
        break;
    case 0x1A:  /* INC A */
        if (m->mf) m->a = (uint16_t)((m->a & 0xFF00) | ((m->a + 1) & 0xFF));
        else m->a++;
        ZN(m, m->a, m->mf);
        break;
    case 0xBB: m->x = m->xf ? (m->y & 0xFF) : m->y; ZN(m, m->x, m->xf); break;
    case 0xA0: case 0xA2: case 0xC0: {  /* LDY/LDX/CPY imm(x) */
        lo = AdrImm(m, 1);
        hi = ((uint32_t)m->k << 16) | (uint16_t)(lo + 1);
        uint16_t v = LoadX(m, lo, hi);
        if (op == 0xA0) { m->y = v; ZN(m, m->y, m->xf); }
        else if (op == 0xA2) { m->x = v; ZN(m, m->x, m->xf); }
        else {
            int r;
            if (m->xf) { r = (m->y & 0xFF) + (v ^ 0xFF) + 1; m->c = r > 0xFF; }
            else       { r = m->y + (v ^ 0xFFFF) + 1;       m->c = r > 0xFFFF; }
            ZN(m, (uint16_t)r, m->xf);
        }
        break;
    }
    case 0x8D: case 0x99: case 0x9D: case 0x9E: {  /* STA abs / abs,Y / abs,X, STZ abs,X */
        uint16_t a16 = Op16(m);
        uint16_t ix = op == 0x8D ? 0 : op == 0x99 ? m->y : m->x;
        lo = (((uint32_t)m->db << 16) + a16 + ix) & 0xFFFFFF;
        hi = (((uint32_t)m->db << 16) + a16 + ix + 1) & 0xFFFFFF;
        uint16_t v = op == 0x9E ? 0 : m->a;
        if (m->mf) Wr(m, lo, (uint8_t)v); else { m->cyc++; WrW(m, lo, hi, v); }
        break;
    }
    case 0x9F: {  /* STA long,X */
        uint16_t a16 = Op16(m);
        uint32_t b = (uint32_t)Op8(m) << 16 | a16;
        lo = (b + m->x) & 0xFFFFFF; hi = (b + m->x + 1) & 0xFFFFFF;
        if (m->mf) Wr(m, lo, (uint8_t)m->a); else { m->cyc++; WrW(m, lo, hi, m->a); }
        break;
    }
    case 0xB7: {  /* LDA [dp],Y */
        uint8_t o = Op8(m);
        if (m->dp & 0xFF) m->cyc++;
        uint32_t p = RdW(m, (uint16_t)(m->dp + o), (uint16_t)(m->dp + o + 1));
        p |= (uint32_t)Rd(m, (uint16_t)(m->dp + o + 2)) << 16;
        lo = (p + m->y) & 0xFFFFFF; hi = (p + m->y + 1) & 0xFFFFFF;
        uint16_t v = LoadM(m, lo, hi);
        m->a = m->mf ? (uint16_t)((m->a & 0xFF00) | v) : v; ZN(m, m->a, m->mf);
        break;
    }
    case 0xC6: {  /* DEC dp (RMW, reversed word write-back) */
        lo = AdrDp(m); hi = (uint16_t)(lo + 1);
        int r;
        if (m->mf) { r = Rd(m, lo) - 1; Wr(m, lo, (uint8_t)r); }
        else {
            m->cyc += 2;
            r = RdW(m, lo, hi) - 1;
            tlk_bridge_write16(m->cpu, lo, hi, (uint16_t)r, 1);
        }
        ZN(m, (uint16_t)r, m->mf);
        break;
    }
    case 0x8A: m->a = m->mf ? (uint16_t)((m->a & 0xFF00) | (m->x & 0xFF)) : m->x; ZN(m, m->a, m->mf); break;
    case 0x98: m->a = m->mf ? (uint16_t)((m->a & 0xFF00) | (m->y & 0xFF)) : m->y; ZN(m, m->a, m->mf); break;
    case 0xA8: m->y = m->xf ? (m->a & 0xFF) : m->a; ZN(m, m->y, m->xf); break;
    case 0xAA: m->x = m->xf ? (m->a & 0xFF) : m->a; ZN(m, m->x, m->xf); break;
    case 0xC8: m->y = m->xf ? ((m->y + 1) & 0xFF) : (uint16_t)(m->y + 1); ZN(m, m->y, m->xf); break;
    case 0xE8: m->x = m->xf ? ((m->x + 1) & 0xFF) : (uint16_t)(m->x + 1); ZN(m, m->x, m->xf); break;
    case 0xCA: m->x = m->xf ? ((m->x - 1) & 0xFF) : (uint16_t)(m->x - 1); ZN(m, m->x, m->xf); break;
    case 0xEB: {  /* XBA */
        uint8_t l = (uint8_t)m->a, h = (uint8_t)(m->a >> 8);
        m->a = (uint16_t)(l << 8 | h);
        ZN(m, h, 1);
        break;
    }
    case 0x29: case 0x49: case 0x69: case 0xA9: case 0xC9: case 0xE9: {
        lo = AdrImm(m, 0);
        hi = ((uint32_t)m->k << 16) | (uint16_t)(lo + 1);
        uint16_t v = LoadM(m, lo, hi);
        switch (op) {
        case 0x29: m->a = m->mf ? (uint16_t)((m->a & 0xFF00) | (m->a & v & 0xFF)) : (m->a & v); ZN(m, m->a, m->mf); break;
        case 0x49: m->a = m->mf ? (uint16_t)((m->a & 0xFF00) | ((m->a ^ v) & 0xFF)) : (m->a ^ v); ZN(m, m->a, m->mf); break;
        case 0x69: Adc(m, v); break;
        case 0xE9: Adc(m, (uint16_t)(m->mf ? (v ^ 0xFF) : (v ^ 0xFFFF))); break;
        case 0xA9: m->a = m->mf ? (uint16_t)((m->a & 0xFF00) | v) : v; ZN(m, m->a, m->mf); break;
        case 0xC9: {
            int r;
            if (m->mf) { r = (m->a & 0xFF) + (v ^ 0xFF) + 1; m->c = r > 0xFF; }
            else       { r = m->a + (v ^ 0xFFFF) + 1;       m->c = r > 0xFFFF; }
            ZN(m, (uint16_t)r, m->mf);
            break;
        }
        }
        break;
    }
    case 0xE0: {  /* CPX imm */
        lo = AdrImm(m, 1);
        hi = ((uint32_t)m->k << 16) | (uint16_t)(lo + 1);
        uint16_t v = LoadX(m, lo, hi);
        int r;
        if (m->xf) { r = (m->x & 0xFF) + (v ^ 0xFF) + 1; m->c = r > 0xFF; }
        else       { r = m->x + (v ^ 0xFFFF) + 1;       m->c = r > 0xFFFF; }
        ZN(m, (uint16_t)r, m->xf);
        break;
    }
    case 0x65: { lo = AdrDp(m); hi = (uint16_t)(lo + 1); Adc(m, LoadM(m, lo, hi)); break; }
    case 0xA5: { lo = AdrDp(m); hi = (uint16_t)(lo + 1); uint16_t v = LoadM(m, lo, hi);
                 m->a = m->mf ? (uint16_t)((m->a & 0xFF00) | v) : v; ZN(m, m->a, m->mf); break; }
    case 0xA6: { lo = AdrDp(m); hi = (uint16_t)(lo + 1); m->x = LoadX(m, lo, hi); ZN(m, m->x, m->xf); break; }
    case 0xA4: { lo = AdrDp(m); hi = (uint16_t)(lo + 1); m->y = LoadX(m, lo, hi); ZN(m, m->y, m->xf); break; }
    case 0x85: { lo = AdrDp(m); hi = (uint16_t)(lo + 1);
                 if (m->mf) Wr(m, lo, (uint8_t)m->a); else { m->cyc++; WrW(m, lo, hi, m->a); } break; }
    case 0x86: { lo = AdrDp(m); hi = (uint16_t)(lo + 1);
                 if (m->xf) Wr(m, lo, (uint8_t)m->x); else { m->cyc++; WrW(m, lo, hi, m->x); } break; }
    case 0x84: { lo = AdrDp(m); hi = (uint16_t)(lo + 1);
                 if (m->xf) Wr(m, lo, (uint8_t)m->y); else { m->cyc++; WrW(m, lo, hi, m->y); } break; }
    case 0xE6: {  /* INC dp (RMW, reversed word write-back) */
        lo = AdrDp(m); hi = (uint16_t)(lo + 1);
        int r;
        if (m->mf) { r = Rd(m, lo) + 1; Wr(m, lo, (uint8_t)r); }
        else {
            m->cyc += 2;
            r = RdW(m, lo, hi) + 1;
            tlk_bridge_write16(m->cpu, lo, hi, (uint16_t)r, 1);
        }
        ZN(m, (uint16_t)r, m->mf);
        break;
    }
    case 0x8F: {  /* STA long */
        uint16_t a16 = Op16(m);
        lo = (uint32_t)Op8(m) << 16 | a16;
        hi = (lo + 1) & 0xFFFFFF;
        if (m->mf) Wr(m, lo, (uint8_t)m->a); else { m->cyc++; WrW(m, lo, hi, m->a); }
        break;
    }
    case 0xA7: {  /* LDA [dp] */
        uint8_t o = Op8(m);
        if (m->dp & 0xFF) m->cyc++;
        uint32_t p = RdW(m, (uint16_t)(m->dp + o), (uint16_t)(m->dp + o + 1));
        p |= (uint32_t)Rd(m, (uint16_t)(m->dp + o + 2)) << 16;
        lo = p; hi = (p + 1) & 0xFFFFFF;
        uint16_t v = LoadM(m, lo, hi);
        m->a = m->mf ? (uint16_t)((m->a & 0xFF00) | v) : v; ZN(m, m->a, m->mf);
        break;
    }
    case 0xBF: {  /* LDA long,X */
        uint16_t a16 = Op16(m);
        uint32_t b = (uint32_t)Op8(m) << 16 | a16;
        lo = (b + m->x) & 0xFFFFFF; hi = (b + m->x + 1) & 0xFFFFFF;
        uint16_t v = LoadM(m, lo, hi);
        m->a = m->mf ? (uint16_t)((m->a & 0xFF00) | v) : v; ZN(m, m->a, m->mf);
        break;
    }
    case 0xB9: {  /* LDA abs,Y */
        uint16_t a16 = Op16(m);
        int crossed = (a16 >> 8) != (((uint32_t)a16 + m->y) >> 8);
        if (!m->xf || crossed) m->cyc++;
        lo = (((uint32_t)m->db << 16) + a16 + m->y) & 0xFFFFFF;
        hi = (((uint32_t)m->db << 16) + a16 + m->y + 1) & 0xFFFFFF;
        uint16_t v = LoadM(m, lo, hi);
        m->a = m->mf ? (uint16_t)((m->a & 0xFF00) | v) : v; ZN(m, m->a, m->mf);
        break;
    }
    }
    return 1;
}

/* Straight-line loop code run natively. A run stops when the PC leaves the
 * region it started in (or enters a listed exclusion), at an unhandled
 * opcode, or where the interpreter step loop could yield. */
typedef struct { uint32_t lo, hi; } Range;
typedef struct { const char *name; uint32_t hooks[4]; Range in[2]; uint32_t stop; } Region;
static const Region kRegions[] = {
    /* $C1CCFE decompressor: command loop (+ the MVN/RTL it writes at $7E2006);
     * $C1CD83 PLB/PLP/RTL epilogue stays interpreted. (Task V tried a second
     * hook at $7E2006: share 7.8% -> 6.7% on l01_right, but the native run is
     * slower than the interpreter there, 4.97 -> 5.25 s per 5000 frames.) */
    { "C1CCFE", { 0xC1CD75 }, { { 0xC1CD43, 0xC1CE7E }, { 0x7E2006, 0x7E2009 } }, 0xC1CD83 },
    /* $C07189 level-load init: WRAM clears ($C07199-$C071D8) ... */
    { "C07189a", { 0xC07199, 0xC071C0 }, { { 0xC07199, 0xC071D8 } }, 0 },
    /* ... and the lookup-table builders ($C07209-$C072AA, before JSL $C1C597). */
    { "C07189b", { 0xC07209 }, { { 0xC07209, 0xC072AA } }, 0 },
    /* $C084A0 32x32-word copy [$0D],Y -> $7F0000,X (RTS at $C084C3 interpreted). */
    { "C084A0", { 0xC084A0 }, { { 0xC084A0, 0xC084C2 } }, 0 },
};
enum { kRegionCount = sizeof kRegions / sizeof kRegions[0] };

static inline int InRegion(const Region *r, const M *m) {
    uint32_t pc = (uint32_t)m->k << 16 | m->pc;
    if (pc == r->stop) return 0;
    for (int i = 0; i < 2; i++)
        if (r->in[i].hi && pc >= r->in[i].lo && pc <= r->in[i].hi) return 1;
    return 0;
}

static int s_ram_ok;  /* $7E2006 holds 54 xx xx 6B (rechecked per run) */

static void NativeHook(CpuState *cpu, uint32_t pc24) {
    const Region *rg = NULL;
    for (int i = 0; i < kRegionCount && !rg; i++)
        for (int h = 0; h < 4; h++)
            if (kRegions[i].hooks[h] && (kRegions[i].hooks[h] & 0x7FFFFF) == (pc24 & 0x7FFFFF)) { rg = &kRegions[i]; break; }
    if (!rg || cpu->emulation) return;
    const uint8_t *r = cpu->ram;
    s_ram_ok = r[0x2006] == 0x54 && r[0x2009] == 0x6B;
    M m;
    memset(&m, 0, sizeof m);
    m.cpu = cpu;
    m.a = cpu->A; m.x = cpu->X; m.y = cpu->Y; m.sp = cpu->S; m.dp = cpu->D;
    m.db = cpu->DB; m.k = cpu->PB; m.pc = (uint16_t)pc24;
    m.c = cpu->_flag_C & 1; m.z = cpu->_flag_Z & 1; m.v = cpu->_flag_V & 1;
    m.n = cpu->_flag_N & 1; m.i = cpu->_flag_I & 1; m.d = cpu->_flag_D & 1;
    m.mf = cpu->m_flag & 1; m.xf = cpu->x_flag & 1;
    if (m.d) return;  /* decimal mode never occurs here; keep ADC simple */
    const uint64_t deadline = tlk_interp_bridge_master_deadline();
    uint64_t pw = g_interp_bridge_write_epoch, pd = tlk_bridge_dynamic_epoch();
    unsigned long n = 0;
    for (;;) {
        if (!InRegion(rg, &m)) break;
        if (m.k == 0x7E && !s_ram_ok) break;
        if (n) {  /* the bridge already sampled these for the first opcode */
            if (deadline && cpu->master_cycles >= deadline) break;
            if (!m.i && g_snes && g_snes->inIrq) break;
        }
        if (!Step(&m)) break;
        tlk_bridge_op_end(cpu, m.cyc, &pw, &pd);
        n++;
    }
    if (!n) return;
    s_native_ops += n;
    s_native_runs++;
    cpu->A = m.a; cpu->X = m.x; cpu->Y = m.y; cpu->S = m.sp; cpu->D = m.dp;
    cpu->DB = m.db; cpu->PB = m.k;
    cpu->_flag_C = m.c; cpu->_flag_Z = m.z; cpu->_flag_V = m.v;
    cpu->_flag_N = m.n; cpu->_flag_I = m.i; cpu->_flag_D = m.d;
    cpu->m_flag = m.mf; cpu->x_flag = m.xf;
    cpu_mirrors_to_p(cpu);
    interp_bridge_pre_opcode_redirect((uint32_t)m.k << 16 | m.pc);
}

void TlkDecompInstall(void) {
    const char *e = getenv("TLK_NATIVE_DECOMP");
    int on;
    if (e && *e) on = *e != '0';
    else {
        const char *b = getenv("SNESRECOMP_LLE_BOUNCE");
        on = !(b && b[0] == '0');
    }
    if (!on) return;
    for (int i = 0; i < kRegionCount; i++)
        for (int h = 0; h < 4; h++)
            if (kRegions[i].hooks[h]) interp_bridge_add_pre_opcode_hook(kRegions[i].hooks[h], NativeHook);
}
