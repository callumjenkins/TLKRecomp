/* Exact AOT pacing runtime. See aot_pace.h. */
#include "aot_pace.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "common_cpu_infra.h"
#include "snes/cart.h"
#include "snes/snes.h"
#include "snes/apu.h"

extern Snes *g_snes;

/* cmake/runtime-patches (exports only). */
uint64_t tlk_interp_bridge_master_deadline(void);
int tlk_interp_bridge_aot_guards(void);
void tlk_interp_bridge_mark_deadline_unwind(void);
uint64_t tlk_refresh_next_master(void);
void tlk_bridge_apu_flush(CpuState *cpu);
uint64_t tlk_bridge_apu_flush_thresh(void);

uint64_t g_aot_pace_next;
uint64_t g_aot_apu_thresh = 1024;
uint32_t g_aot_bus_mc;

/* Diagnostics: TLK_HWLOG=<path> [TLK_HWLOG_FROM=<frame>] logs every hardware
 * register touch as "frame master addr" (interpreted and compiled alike), so
 * a --lle run and a default run can be diffed for the first access whose
 * master clock differs. Installed by aot_pace_diag_init(). */
void (*g_aot_pace_hw_log)(uint16_t addr);
/* Task CC: called before a compiled read of $4218 (hosts arm it for --input-polled / --ram-log). */
void (*g_tlk_aot_pad_read_hook)(void);
static FILE *s_hwlog;
static int s_hwlog_from;
static void hw_log(uint16_t addr) {
    extern int snes_frame_counter;
    if (snes_frame_counter < s_hwlog_from) return;
    extern uint32_t g_interp816_cur_pc;
    extern const char *g_last_recomp_func;
    fprintf(s_hwlog, "%d %llu %04X ipc=%06X fn=%s apu=%llu/%llu/%llu\n", snes_frame_counter,
            (unsigned long long)g_cpu.master_cycles, addr,
            (unsigned)g_interp816_cur_pc,
            g_last_recomp_func ? g_last_recomp_func : "-",
            g_snes && g_snes->apu ? (unsigned long long)g_snes->apu->portClock : 0ull,
            g_snes && g_snes->apu ? (unsigned long long)g_snes->apu->portGuestAnchor : 0ull,
            g_snes && g_snes->apu ? (unsigned long long)g_snes->apu->portTargetAnchor : 0ull);
}
#ifdef AOT_PACE_TRACE
/* Build with -DAOT_PACE_TRACE: TLK_HWLOG also gets one line per compiled
 * instruction ("frame pc cyc mc"), comparable with SNESRECOMP_CYC_WATCH. */
void aot_pace_trace(uint32_t pc24, uint32_t cyc, uint32_t mc) {
    extern int snes_frame_counter;
    if (s_hwlog && snes_frame_counter >= s_hwlog_from)
        fprintf(s_hwlog, "%d %llu aot pc=$%06X cyc=%u mc=%u S=%04X\n", snes_frame_counter,
                (unsigned long long)g_cpu.master_cycles, (unsigned)pc24, cyc, mc, (unsigned)g_cpu.S);
}
#endif
void (*g_aot_pace_flush_log)(uint64_t pending);
void (*g_aot_pace_resume_log)(int kind, uint32_t pc24, int site);
static void resume_log(int kind, uint32_t pc24, int site) {
    extern int snes_frame_counter;
    if (snes_frame_counter < s_hwlog_from) return;
    fprintf(s_hwlog, "%d %llu %s pc=$%06X site=%d\n", snes_frame_counter,
            (unsigned long long)g_cpu.master_cycles, kind ? "unwind" : "resume",
            (unsigned)pc24, site);
}
static void flush_log(uint64_t pending) {
    extern int snes_frame_counter;
    if (snes_frame_counter < s_hwlog_from) return;
    fprintf(s_hwlog, "%d %llu flush pending=%llu apu=%llu\n", snes_frame_counter,
            (unsigned long long)g_cpu.master_cycles, (unsigned long long)pending,
            g_snes && g_snes->apu ? (unsigned long long)g_snes->apu->portClock : 0ull);
}
void aot_pace_diag_frame(void) {
    if (!s_hwlog) return;
    extern int snes_frame_counter;
    if (snes_frame_counter < s_hwlog_from) return;
    fprintf(s_hwlog, "%d %llu frame apu=%llu beam=%llu v=%u h=%u\n", snes_frame_counter,
            (unsigned long long)g_cpu.master_cycles,
            g_snes && g_snes->apu ? (unsigned long long)g_snes->apu->portClock : 0ull,
            (unsigned long long)(g_snes ? g_snes->beamMasterLast : 0),
            g_snes ? (unsigned)g_snes->vPos : 0, g_snes ? (unsigned)g_snes->hPos : 0);
}
void aot_pace_diag_init(void) {
    g_aot_apu_thresh = tlk_bridge_apu_flush_thresh();
    const char *p = getenv("TLK_HWLOG");
    if (!p || !*p) return;
    s_hwlog = fopen(p, "wb");
    if (!s_hwlog) return;
    const char *f = getenv("TLK_HWLOG_FROM");
    s_hwlog_from = f ? atoi(f) : 0;
    g_aot_pace_hw_log = hw_log;
    g_aot_pace_resume_log = resume_log;
    if (getenv("TLK_HWLOG_FLUSH")) g_aot_pace_flush_log = flush_log;
}

uint8_t aot_peek_slow(CpuState *cpu, uint32_t adr24) {
    uint8_t bank = (uint8_t)(adr24 >> 16);
    uint16_t a = (uint16_t)adr24;
    int system = bank < 0x40 || (bank >= 0x80 && bank < 0xc0);
    if (system && a >= 0x2000 && a < 0x8000) return 0; /* I/O: cost only needs a guess */
    uint8_t ob = cpu->open_bus;
    uint8_t v = cpu_read8(cpu, bank, a);
    cpu->open_bus = ob;
    return v;
}

static uint64_t min64(uint64_t a, uint64_t b) { return a < b ? a : b; }

static int pace_tail(CpuState *cpu);

int aot_pace_event(CpuState *cpu) {
    const int due = pace_tail(cpu);
    if (due) tlk_interp_bridge_mark_deadline_unwind();
    return due;
}

/* Task Q: a nested interpreter frame entered from compiled code right after
 * an instruction charged without its boundary (an indirect JMP/JML dispatch
 * into a target that has no compiled variant) first runs that boundary's
 * work (DRAM refresh, beam, SPC), as the interpreter would have after the
 * jump. Deadline / IRQ are left to the interpreter's own checks. */
void tlk_aot_pace_settle(CpuState *cpu) {
    if (cpu->master_cycles >= g_aot_pace_next || s_apu_pending_master >= g_aot_apu_thresh)
        (void)pace_tail(cpu);
}

static int pace_tail(CpuState *cpu) {
    /* The step loop's post-opcode tail (interp_bridge.c), in its order. */
    snes_refresh_charge();
    cpu->coprocessor_master_cycles = cpu->master_cycles;
    if (g_snes) snes_sync_master_clock(g_snes, cpu->master_cycles);
    if (g_snes && g_snes->cart)
        cart_sync_coprocessors(g_snes->cart, cpu->master_cycles);

    if (s_apu_pending_master >= g_aot_apu_thresh) tlk_bridge_apu_flush(cpu);

    const int guards = tlk_interp_bridge_aot_guards();
    const uint64_t deadline = tlk_interp_bridge_master_deadline();
    uint64_t next = UINT64_MAX;
    uint64_t r = tlk_refresh_next_master();
    if (r) next = min64(next, r);
    if (guards && deadline) next = min64(next, deadline);
    if (g_snes) {
        if (g_snes->inIrq) {
            next = 0; /* pending: sample at every boundary until taken */
        } else {
            /* The walk latches when it moves past the comparator position
             * (snes.c snes_advance_beam), so a beam parked exactly ON the
             * target latches on the next clock; snes_master_clocks_until_irq
             * reports the next match instead (a line / field later). */
            const uint32_t th = g_snes->hIrqEnabled ? (uint32_t)g_snes->hTimer * 4u : 0u;
            if ((g_snes->hIrqEnabled || g_snes->vIrqEnabled) && th < 1364u &&
                g_snes->hPos == th &&
                (!g_snes->vIrqEnabled || g_snes->vPos == g_snes->vTimer)) {
                next = min64(next, g_snes->beamMasterLast + 1);
            } else {
                uint32_t u = snes_master_clocks_until_irq(g_snes);
                if (u) next = min64(next, g_snes->beamMasterLast + u);
            }
        }
    }
    g_aot_pace_next = next;
#ifdef AOT_PACE_TRACE
    {
        extern int snes_frame_counter;
        if (s_hwlog && snes_frame_counter >= s_hwlog_from)
            fprintf(s_hwlog, "%d %llu event next=%llu irq=%d I=%d P=%02X guards=%d dl=%llu beam=%llu h=%u v=%u ht=%u vt=%u en=%d%d\n",
                    snes_frame_counter, (unsigned long long)cpu->master_cycles,
                    (unsigned long long)next, g_snes ? g_snes->inIrq : -1,
                    cpu->_flag_I, cpu->P, guards, (unsigned long long)deadline,
                    (unsigned long long)g_snes->beamMasterLast, g_snes->hPos, g_snes->vPos,
                    g_snes->hTimer, g_snes->vTimer, g_snes->hIrqEnabled, g_snes->vIrqEnabled);
    }
#endif

    if (!guards || !deadline) return 0;
    return cpu->master_cycles >= deadline ||
           (!cpu->_flag_I && g_snes && g_snes->inIrq);
}
