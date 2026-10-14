/* Exact interpreter-equivalent pacing for generated (AOT) code.
 *
 * The LLE step loop charges each opcode as sum(region speed of every bus
 * transfer) + 6 per remaining internal cycle, then (per opcode) charges DRAM
 * refresh, walks the beam (which latches H/V IRQs), and samples the master
 * deadline and pending IRQs before the next opcode. Generated code used to
 * charge `block cycles x code-region speed` as one lump at block entry and
 * check the deadline only per block, so long compiled loops finished at a
 * different master clock than --lle and lag frames moved (docs/devlog.md,
 * task M).
 *
 * With patches/snesrecomp/0003-aot-exact-pacing.patch the emitter charges
 * every instruction after its effects with the cost the interpreter would
 * charge (recompiler/v2/aot_pace.py, verified against interp816.c), and calls
 * aot_pace_point() at each instruction boundary. The slow path runs only when
 * master_cycles reaches g_aot_pace_next = min(next refresh line, armed
 * deadline, next H/V IRQ latch) or the interpreter's pending APU master
 * clocks reach their flush threshold; it does exactly what the step loop does
 * after an opcode (refresh, beam, SPC catch-up) and reports whether the
 * interpreter would yield before the next one (deadline reached, or IRQ
 * asserted with I clear).
 *
 * g_aot_pace_next is cleared (forcing the slow path) whenever anything other
 * than generated code advances the clock or changes the inputs: every
 * interpreted opcode, every armed deadline change, every hardware register
 * access (cmake/runtime-patches). */
#ifndef TLK_AOT_PACE_H
#define TLK_AOT_PACE_H

#include <stdint.h>
#include "cpu_state.h"

extern uint64_t g_aot_pace_next;
extern uint64_t s_apu_pending_master;   /* interp_bridge.c (runtime patch) */
extern uint64_t g_aot_apu_thresh;
/* Bus clocks of the current compiled instruction up to its data access
 * (codegen patch 0004); times an APU-port access like the interpreter. */
extern uint32_t g_aot_bus_mc;
int aot_pace_event(CpuState *cpu);
/* runner/tlk_fiber.c (task Q): suspend the compiled chain at a due yield
 * (0 = resumed in place, 1 = take the unwind), and run a memory-poll routine
 * in a nested interpreter frame (-1 = not on the fiber). */
int tlk_fiber_yield(CpuState *cpu, uint32_t resume_pc24);
int tlk_fiber_poll_entry(CpuState *cpu, uint32_t entry_pc24);
uint8_t aot_peek_slow(CpuState *cpu, uint32_t adr24);

/* cpu_region_speed (cpu_state.c), inline for generated code. */
static inline uint32_t aot_spd(uint32_t adr) {
    uint8_t bank = (uint8_t)(adr >> 16);
    uint16_t a = (uint16_t)adr;
    if (bank >= 0x40 && bank <= 0x7f) return 8;
    if (bank >= 0xc0) return g_memsel ? 6 : 8;
    if (a >= 0x8000) {
        if (bank <= 0x3f) return 8;
        return g_memsel ? 6 : 8;
    }
    if (a < 0x2000) return 8;
    if (a < 0x4000) return 6;
    if (a < 0x4200) return 12;
    if (a < 0x6000) return 6;
    return 8;
}

/* Side-effect-free byte at a 24-bit address (pointer bytes for the cost of
 * an indirect access; read before the instruction runs). */
static inline uint8_t aot_peek(CpuState *cpu, uint32_t adr24) {
    uint8_t bank = (uint8_t)(adr24 >> 16);
    uint16_t a = (uint16_t)adr24;
    if (a < 0x2000 && (bank < 0x40 || (bank >= 0x80 && bank < 0xc0)))
        return cpu->ram[a];
    return aot_peek_slow(cpu, adr24 & 0xFFFFFFu);
}
#define AOT_PEEK0(a) aot_peek(cpu, (uint32_t)((a) & 0xFFFF))
#define AOT_XCROSS(b, i) ((((uint32_t)(b) >> 8) != (((uint32_t)(b) + (uint32_t)(i)) >> 8)) ? 1u : 0u)
#define AOT_DPL ((cpu->D & 0xFF) ? 1u : 0u)

/* Charge one executed instruction; nonzero when the interpreter would stop
 * before the next instruction (deadline / IRQ). */
#ifdef AOT_PACE_TRACE
void aot_pace_trace(uint32_t pc24, uint32_t cyc, uint32_t mc);
#endif
static inline int aot_pace_point(CpuState *cpu, uint32_t cyc, uint32_t mc,
                                 uint32_t pc24) {
#ifdef AOT_PACE_TRACE
    aot_pace_trace(pc24, cyc, mc);
#else
    (void)pc24;
#endif
    cpu->cycles += cyc;
    cpu->master_cycles += mc;
    s_apu_pending_master += mc;
    return (cpu->master_cycles >= g_aot_pace_next ||
            s_apu_pending_master >= g_aot_apu_thresh) ? aot_pace_event(cpu) : 0;
}

/* Block / function entry: the same boundary test without a charge. */
static inline int aot_pace_block(CpuState *cpu) {
    return (cpu->master_cycles >= g_aot_pace_next ||
            s_apu_pending_master >= g_aot_apu_thresh) ? aot_pace_event(cpu) : 0;
}

/* Charge without the boundary event (the next block / function entry runs
 * it), so it can be undone exactly when the interpreter re-executes the
 * instruction (unresolved indirect jump). */
static inline void aot_pace_charge(CpuState *cpu, uint32_t cyc, uint32_t mc) {
    cpu->cycles += cyc;
    cpu->master_cycles += mc;
    s_apu_pending_master += mc;
}
static inline void aot_pace_refund(CpuState *cpu, uint32_t cyc, uint32_t mc) {
    cpu->cycles -= cyc;
    cpu->master_cycles -= mc;
    s_apu_pending_master -= mc;
}

#ifdef AOT_PACE_GENERATED
/* Generated blocks test this at entry; route it through the paced check. */
#define interp_bridge_lle_master_deadline_reached(c) aot_pace_block(c)
#endif

#endif
