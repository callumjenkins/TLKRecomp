/* Trace hooks the generated code calls; empty outside a trace build. */
#include <stdint.h>

#if !defined(SNESRECOMP_TRACE) || !SNESRECOMP_TRACE
void debug_on_block_enter(uint32_t pc, uint32_t a, uint32_t x, uint32_t y)
{ (void)pc; (void)a; (void)x; (void)y; }
void debug_on_wram_write_byte(uint32_t addr, uint8_t o, uint8_t n)
{ (void)addr; (void)o; (void)n; }
void debug_on_wram_write_word(uint32_t addr, uint16_t o, uint16_t n)
{ (void)addr; (void)o; (void)n; }
#endif
