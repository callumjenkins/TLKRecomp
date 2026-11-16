/* The game fiber's switches (tlk_fiber.c), with the bridge it drives replaced by a scripted run. */
#include "cpu_state.h"

#include <stdio.h>
#include <string.h>

int tlk_fiber_run_until_quiescent(CpuState *cpu, uint32_t entry_pc24);
int tlk_fiber_yield(CpuState *cpu, uint32_t resume_pc24);
void tlk_fiber_discard(void);

static int s_failed;
#define CHECK(cond, what)                                                     \
    do {                                                                      \
        if (!(cond)) {                                                        \
            s_failed++;                                                       \
            fprintf(stderr, "%s:%d: %s: %s\n", __FILE__, __LINE__, #cond, what); \
        }                                                                     \
    } while (0)

/* The scripted run: yields `s_yields` times at 0x8000 + its count, then returns 42. A yield that
 * comes back as 1 (aborted) ends the run with -1. */
static int s_yields, s_runs, s_yield_results[8], s_yield_n;
static uint32_t s_entries[8], s_published;

int tlk_interp_bridge_run_until_quiescent_raw(CpuState *cpu, uint32_t entry_pc24) {
    s_entries[s_runs++ & 7] = entry_pc24;
    for (int i = 0; i < s_yields; i++) {
        int r = tlk_fiber_yield(cpu, 0x8000u + (uint32_t)i);
        s_yield_results[s_yield_n++ & 7] = r;
        if (r) return -1;
    }
    return 42;
}

void tlk_bridge_publish_resume(uint32_t pc24, uint16_t sp) { (void)sp; s_published = pc24; }
size_t tlk_bridge_ctx_size(void) { return 16; }
void tlk_bridge_ctx_save(void *buf) { (void)buf; }
void tlk_bridge_ctx_restore(const void *buf) { (void)buf; }
void tlk_bridge_clear_next_unwind_deadline(void) {}
size_t tlk_infra_ctx_size(void) { return 16; }
void tlk_infra_ctx_save(void *buf) { (void)buf; }
void tlk_infra_ctx_restore(const void *buf) { (void)buf; }
int tlk_infra_tailcall_pending(void) { return 0; }
int interp_bridge_in_lle_scheduler(void) { return 0; }
void tlk_bridge_apu_flush(CpuState *cpu) { (void)cpu; }
RecompReturn interp_tier_run_call_frame(CpuState *cpu, uint32 target_pc24, uint32 source_pc24, uint8 frame_size,
                                        uint32 *return_pc24) {
    (void)cpu; (void)target_pc24; (void)source_pc24; (void)frame_size; (void)return_pc24;
    return (RecompReturn)0;
}
uint64_t g_aot_pace_next;
int snes_frame_counter;
const char *g_last_recomp_func;

static CpuState s_cpu;

static void Reset(int yields) {
    s_yields = yields;
    s_runs = s_yield_n = 0;
    s_published = 0;
}

static void fiber_run_with_no_yield_returns_its_result(void) {
    Reset(0);
    CHECK(tlk_fiber_run_until_quiescent(&s_cpu, 0x1234) == 42, "the run's result");
    CHECK(s_runs == 1 && s_entries[0] == 0x1234, "one run at the entry");
}

static void fiber_yield_switches_and_resume_continues_in_place(void) {
    Reset(2);
    CHECK(tlk_fiber_run_until_quiescent(&s_cpu, 0x1000) == 1, "the first yield hands back 1");
    CHECK(s_published == 0x8000, "and publishes its resume PC");
    CHECK(tlk_fiber_run_until_quiescent(&s_cpu, 0x8000) == 1, "resumed at that PC, it runs to the second yield");
    CHECK(s_published == 0x8001, "which publishes its own PC");
    CHECK(tlk_fiber_run_until_quiescent(&s_cpu, 0x8001) == 42, "resumed again, it finishes");
    CHECK(s_runs == 1, "all in one run, never restarted");
    CHECK(s_yield_n == 2 && s_yield_results[0] == 0 && s_yield_results[1] == 0, "each yield came back resumed");
}

static void fiber_resume_at_another_pc_aborts_and_starts_over(void) {
    Reset(1);
    CHECK(tlk_fiber_run_until_quiescent(&s_cpu, 0x1000) == 1, "suspended");
    s_yields = 0;
    CHECK(tlk_fiber_run_until_quiescent(&s_cpu, 0x2000) == 42, "a different PC runs fresh to the end");
    CHECK(s_yield_n == 1 && s_yield_results[0] == 1, "the suspended yield came back aborted");
    CHECK(s_runs == 2 && s_entries[1] == 0x2000, "a second run started at the new PC");
}

static void fiber_discard_ends_a_suspended_run(void) {
    Reset(1);
    CHECK(tlk_fiber_run_until_quiescent(&s_cpu, 0x1000) == 1, "suspended");
    tlk_fiber_discard();
    CHECK(s_yield_n == 1 && s_yield_results[0] == 1, "the discarded yield came back aborted");
    s_yields = 0;
    CHECK(tlk_fiber_run_until_quiescent(&s_cpu, 0x8000) == 42, "the next call starts a new run, even at the old PC");
    CHECK(s_runs == 2 && s_entries[1] == 0x8000, "at the PC it was given");
    tlk_fiber_discard();
    CHECK(s_runs == 2, "discarding with nothing suspended does nothing");
}

int main(void) {
    fiber_run_with_no_yield_returns_its_result();
    fiber_yield_switches_and_resume_continues_in_place();
    fiber_resume_at_another_pc_aborts_and_starts_over();
    fiber_discard_ends_a_suspended_run();
    printf("%s (%d failed)\n", s_failed ? "FAIL" : "PASS", s_failed);
    return s_failed != 0;
}
