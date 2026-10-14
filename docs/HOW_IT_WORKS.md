# How TLKRecomp works

## The idea

The Lion King cartridge holds a program for the SNES's 65816 CPU plus data (graphics, levels, music). An emulator
reads that program one instruction at a time and imitates the CPU. A static recompiler instead translates the
program ahead of time into C, which a normal compiler turns into native code for your PC.

Only the CPU program is translated. The picture chip (PPU), the sound chip (SPC700/DSP), RAM and the cartridge data
still come from snesrecomp's emulated hardware, fed from your ROM. So the game looks, sounds and plays like the
cartridge; what changes is that the game logic is ordinary native code that can be read, debugged and modded.

```
your ROM ──> scripts/generate_snesrecomp.py ──> generated/snesrecomp/*.c   (local only, never committed)
                │ reads recomp/*.cfg
                │ uses snesrecomp's code generator + patches/snesrecomp/*.patch
                v
CMake ──> tlk_sdl / tlk_headless = generated C + runner/ + snesrecomp runtime (+ cmake/runtime-patches)
                                         at run time: game data read from the ROM
```

## Pieces

| Piece | Where | Role |
|---|---|---|
| snesrecomp | `snesrecomp/` (submodule, pinned) | code generator, CPU interpreter, emulated PPU/SPC/DMA, desktop host |
| Function lists | `recomp/bank*.cfg` | where each routine starts, its CPU mode (M/X width) on entry and exit, jump tables, hints the analyzer can't work out alone |
| Generator patches | `patches/snesrecomp/*.patch` | fixes and features for the code generator, applied to a copy in `build/snesrecomp-codegen` |
| Runtime patches | `cmake/runtime-patches/*.hunks` | changes to snesrecomp's runtime, applied to build-directory copies; a missing anchor fails the build |
| Runner | `runner/` | the game glue: SDL host with menu, remapping and save states (`sdl_main.c`), test host (`headless_main.c`), fibers, pacing |

The submodule is never edited in place. Every change to snesrecomp lives in a patch file so the pin can be moved and
the patches re-applied.

## Compiled code and the interpreter

Static translation can't see everything up front: the game jumps through pointer tables, pushes return addresses
by hand, and changes the CPU's register widths at run time. So the build carries both:

- **Compiled bodies** for every known routine and CPU mode (1,305 entry points today).
- **snesrecomp's interpreter** as a fallback. When execution reaches code without a compiled body for the current
  mode, the interpreter runs it and hands back to compiled code at the next known entry.

`--lle` turns the compiled bodies off (apart from interrupt entries) and runs everything on the interpreter. That
mode is the reference: the compiled build must produce the same frames and RAM as `--lle` for the same input
(see `VERIFYING.md`).

What still runs on the interpreter is about 4% of non-idle CPU time on the test tapes: known routines entered
through two dispatch sites (`$C0B5A9`, `$C0D7F3`) in a mode the compiler can't yet handle safely, because some
targets never return. Details in `devlog.md`, task FF.

## Timing

SNES games depend on exact timing: when an interrupt lands, how long a frame takes, when the pad is read. Compiled
code runs far faster than the real CPU, so the generated C counts the cycles each instruction would have taken
(`aot_pace`) and syncs the emulated hardware at the same points the interpreter would. Two runtime mechanisms keep
that exact:

- **Fibers** (`runner/tlk_fiber.c`): when compiled code hits the end of a frame or an interrupt, it suspends on its
  own C stack and resumes in place, instead of unwinding and finishing the routine on the interpreter.
- **Pacing** (`runner/aot_pace.c`, patch 0003/0004): compiled code charges the same master-clock cycles, memory
  refresh and DMA stalls as the interpreter.

Timing was also checked against BizHawk (bsnes v115 core): three published speedruns and a 60,648-frame All Bugs run
replay to the end with the same RAM at every controller read.

## Hard parts worth knowing

Most of the work was code-generator bugs found by comparing against `--lle`:

- Pushed-frame calls: the game pushes an address and does `RTS`/`RTL` to jump. These are modelled as calls with a
  continuation (patches 0007, 0008, 0010, 0011).
- Bank carry on indexed indirect addressing (`(dp),Y`, `(sr,S),Y`) crossing a 64 KB bank (patch 0009).
- Routines that never return (level loader resets the stack, demo exit) must not be compiled as calls.
- An interrupt sampled one instruction late inside a nested interpreter frame.

Each is written up with evidence in `devlog.md`.
