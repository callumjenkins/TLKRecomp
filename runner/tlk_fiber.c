/* Compiled-code fibers (task Q, docs/devlog.md).
 *
 * The beam frame driver runs the guest through interp_bridge_run_until_quiescent
 * and returns to the host at the field deadline and at every pending IRQ. When
 * compiled code was running at that moment, the bridge used to unwind every
 * compiled host frame and resume the guest by interpreting from the yield PC,
 * so the rest of each routine on the call chain ran on the interpreter (the
 * SPC upload loop, the main loop, ...).
 *
 * Here the scheduler run itself executes on a separate C stack (the game
 * fiber). A compiled yield point (codegen patch 0005: `tlk_fiber_yield`) and
 * the nested-interpreter hand-offs (runtime hunks) switch back to the driver
 * instead of unwinding, with the same resume PC published. The driver then
 * does exactly what it did before (interrupt, park, end of field) and calls
 * run_until_quiescent(PC) again; if PC, S, PB and the widths are what the
 * fiber stopped at, the fiber continues where it stopped. Otherwise (an
 * interrupt handler rewrote its return frame, a reset) the fiber is told to
 * abort, and its yield point takes the old unwind path.
 *
 * The guest-visible machine state at a switch is the same as at the old
 * unwind (both happen at the same instruction boundary), so only the tier that
 * executes the continuation changes. Interrupt handlers run on the driver's
 * stack, on top of the suspended chain, as on hardware; the bridge's and the
 * call bookkeeping's per-run state is swapped at every switch so they see the
 * same state as before.
 *
 * Off under --lle (SNESRECOMP_LLE_BOUNCE=0) and with TLK_FIBER=0. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>

#include "cpu_state.h"

int tlk_interp_bridge_run_until_quiescent_raw(CpuState *cpu, uint32_t entry_pc24);
size_t tlk_bridge_ctx_size(void);
void tlk_bridge_ctx_save(void *buf);
void tlk_bridge_ctx_restore(const void *buf);
void tlk_bridge_publish_resume(uint32_t pc24, uint16_t sp);
void tlk_bridge_clear_next_unwind_deadline(void);
size_t tlk_infra_ctx_size(void);
void tlk_infra_ctx_save(void *buf);
void tlk_infra_ctx_restore(const void *buf);
int tlk_infra_tailcall_pending(void);
int interp_bridge_in_lle_scheduler(void);
RecompReturn interp_tier_run_call_frame(CpuState *cpu, uint32_t target_pc24,
                                        uint32_t source_pc24, uint8_t frame_size,
                                        uint32_t *return_pc24);
void tlk_bridge_apu_flush(CpuState *cpu);
extern uint64_t g_aot_pace_next;

unsigned long g_tlk_fiber_gen;

#define FIBER_STACK_BYTES (64u << 20)
#define CTX_BYTES 4096

enum { G_IDLE, G_RUNNING, G_SUSPENDED };

static int s_enabled = -1;
static int s_no_aot, s_no_interp, s_no_poll;  /* TLK_FIBER_OFF=aot,interp,poll (diagnostics) */
static int s_state = G_IDLE;
static int s_on_game;
static int s_created;
static ucontext_t s_main_uc, s_game_uc;
static void *s_stack;

static CpuState *s_cpu;
static uint32_t s_run_pc;
static int s_run_result;
static int s_abort;

static uint32_t s_susp_pc;
static uint16_t s_susp_s;
static uint8_t s_susp_pb, s_susp_m, s_susp_x, s_susp_e;

static _Alignas(16) unsigned char s_game_bridge[CTX_BYTES], s_main_bridge[CTX_BYTES];
static _Alignas(16) unsigned char s_game_infra[CTX_BYTES], s_main_infra[CTX_BYTES];

static unsigned long s_stat_switch, s_stat_resume, s_stat_abort;

static int enabled(void) {
    if (s_enabled < 0) {
        const char *f = getenv("TLK_FIBER");
        const char *b = getenv("SNESRECOMP_LLE_BOUNCE");
        s_enabled = !(f && f[0] == '0') && !(b && b[0] == '0');
        const char *off = getenv("TLK_FIBER_OFF");
        if (off) {
            s_no_aot = strstr(off, "aot") != NULL;
            s_no_interp = strstr(off, "interp") != NULL;
            s_no_poll = strstr(off, "poll") != NULL;
        }
        if (tlk_bridge_ctx_size() > CTX_BYTES || tlk_infra_ctx_size() > CTX_BYTES) {
            fprintf(stderr, "[tlk_fiber] context too large, fibers off\n");
            s_enabled = 0;
        }
        if (s_enabled) {
            s_stack = mmap(NULL, FIBER_STACK_BYTES, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
            if (s_stack == MAP_FAILED) { s_stack = NULL; s_enabled = 0; }
        }
        if (getenv("TLK_FIBER_STATS")) {
            extern void tlk_fiber_stats(void);
            atexit(tlk_fiber_stats);
        }
    }
    return s_enabled;
}

void tlk_fiber_stats(void) {
    fprintf(stderr, "[tlk_fiber] switches=%lu resumes=%lu aborts=%lu\n",
            s_stat_switch, s_stat_resume, s_stat_abort);
}

static void game_main(void) {
    for (;;) {
        s_run_result = tlk_interp_bridge_run_until_quiescent_raw(s_cpu, s_run_pc);
        s_state = G_IDLE;
        s_on_game = 0;
        swapcontext(&s_game_uc, &s_main_uc);
    }
}

/* Driver -> game. The bridge/call state of the driver side is saved and the
 * game side's restored (a fresh run starts from the driver's own state). */
static void enter_game(int resume) {
    tlk_bridge_ctx_save(s_main_bridge);
    tlk_infra_ctx_save(s_main_infra);
    if (resume) {
        tlk_bridge_ctx_restore(s_game_bridge);
        tlk_infra_ctx_restore(s_game_infra);
    }
    if (!s_created) {
        getcontext(&s_game_uc);
        s_game_uc.uc_stack.ss_sp = s_stack;
        s_game_uc.uc_stack.ss_size = FIBER_STACK_BYTES;
        s_game_uc.uc_link = NULL;
        makecontext(&s_game_uc, game_main, 0);
        s_created = 1;
    }
    s_on_game = 1;
    swapcontext(&s_main_uc, &s_game_uc);
    /* Back on the driver: the game either finished its run (its state is
     * balanced back to ours) or suspended (switch_to_driver saved it). */
    tlk_bridge_ctx_restore(s_main_bridge);
    tlk_infra_ctx_restore(s_main_infra);
}

int tlk_fiber_run_until_quiescent(CpuState *cpu, uint32_t entry_pc24) {
    if (!enabled() || s_on_game)
        return tlk_interp_bridge_run_until_quiescent_raw(cpu, entry_pc24);
    entry_pc24 &= 0xFFFFFFu;
    if (s_state == G_SUSPENDED) {
        cpu_mirrors_to_p(cpu);
        const int match = entry_pc24 == s_susp_pc && cpu->S == s_susp_s &&
                          cpu->PB == s_susp_pb && (cpu->m_flag & 1) == s_susp_m &&
                          (cpu->x_flag & 1) == s_susp_x && (cpu->emulation & 1) == s_susp_e;
        s_abort = !match;
        if (match) s_stat_resume++; else s_stat_abort++;
        s_state = G_RUNNING;
        enter_game(1);
        if (match) {
            if (s_state == G_SUSPENDED) {
                tlk_bridge_publish_resume(s_susp_pc, s_susp_s);
                return 1;
            }
            return s_run_result;
        }
        /* Aborted: the chain unwound and the run ended; start over at the
         * driver's PC below. */
        if (s_state != G_IDLE) {
            fprintf(stderr, "[tlk_fiber] abort did not end the run\n");
            abort();
        }
    }
    s_cpu = cpu;
    s_run_pc = entry_pc24;
    s_state = G_RUNNING;
    enter_game(0);
    if (s_state == G_SUSPENDED) {
        tlk_bridge_publish_resume(s_susp_pc, s_susp_s);
        return 1;
    }
    return s_run_result;
}

/* Game -> driver at a yield point. Returns 0 when resumed in place, 1 when
 * the caller must take the old unwind path. */
static int s_log = -1;
static int switch_to_driver(CpuState *cpu, uint32_t resume_pc24) {
    if (s_log < 0) s_log = getenv("TLK_FIBER_LOG") ? atoi(getenv("TLK_FIBER_LOG")) : 0x7fffffff;
    {
        extern int snes_frame_counter;
        extern const char *g_last_recomp_func;
        if (snes_frame_counter >= s_log)
            fprintf(stderr, "[fiber] f%d suspend pc=%06X S=%04X fn=%s\n", snes_frame_counter,
                    (unsigned)resume_pc24, (unsigned)cpu->S, g_last_recomp_func ? g_last_recomp_func : "-");
    }
    tlk_bridge_apu_flush(cpu);
    cpu_mirrors_to_p(cpu);
    s_susp_pc = resume_pc24 & 0xFFFFFFu;
    s_susp_s = cpu->S;
    s_susp_pb = cpu->PB;
    s_susp_m = cpu->m_flag & 1;
    s_susp_x = cpu->x_flag & 1;
    s_susp_e = cpu->emulation & 1;
    s_state = G_SUSPENDED;
    s_stat_switch++;
    tlk_bridge_ctx_save(s_game_bridge);
    tlk_infra_ctx_save(s_game_infra);
    s_on_game = 0;
    swapcontext(&s_game_uc, &s_main_uc);
    /* Resumed (enter_game restored our bridge/call state). */
    s_on_game = 1;
    if (s_abort) {
        s_abort = 0;
        return 1;
    }
    g_tlk_fiber_gen++;
    g_aot_pace_next = 0;
    return 0;
}

/* Compiled code: called when a yield is due (deadline or IRQ), before the
 * unwind. */
int tlk_fiber_yield(CpuState *cpu, uint32_t resume_pc24) {
    if (!s_on_game || !s_enabled || s_no_aot) return 1;
    if (switch_to_driver(cpu, resume_pc24)) return 1;
    tlk_bridge_clear_next_unwind_deadline();
    return 0;
}

/* Nested interpreter frame (task H deadline/IRQ hand-off, task M stable-poll
 * hand-off). The opcode at pc24 has not run yet. */
int tlk_fiber_yield_interp(CpuState *cpu, uint32_t pc24) {
    if (!s_on_game || !s_enabled || s_no_interp) return 1;
    return switch_to_driver(cpu, pc24);
}

/* A memory-poll routine's entry (codegen: has_lle_memory_poll). Under the
 * scheduler it must run on the interpreter (stable-poll parking). The old
 * path unwound the whole compiled chain to interpret it in the scheduler
 * frame; on the fiber it runs in a nested interpreter frame on the paired
 * return contract instead, and the chain stays compiled. Returns -1 when not
 * applicable (old path). */
int tlk_fiber_poll_entry(CpuState *cpu, uint32_t entry_pc24) {
    if (!s_on_game || !s_enabled || s_no_poll) return -1;
    const uint8_t hrv = cpu->host_return_valid;
    if ((hrv != 2 && hrv != 3) || tlk_infra_tailcall_pending()) return -1;
    return (int)interp_tier_run_call_frame(cpu, entry_pc24, entry_pc24, hrv, NULL);
}

/* Savestate load (tlk_savestate.c): drop a suspended chain. Its yield point
 * takes the old unwind path against the current (about to be replaced)
 * state; the driver then starts a fresh run at the loaded resume PC, which is
 * what the old non-fiber path always did after a yield. */
void tlk_fiber_discard(void) {
    if (s_on_game || s_state != G_SUSPENDED) return;
    s_abort = 1;
    s_stat_abort++;
    s_state = G_RUNNING;
    enter_game(1);
    if (s_state != G_IDLE) {
        fprintf(stderr, "[tlk_fiber] discard did not end the run\n");
        abort();
    }
}
