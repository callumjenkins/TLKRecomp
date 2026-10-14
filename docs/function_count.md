# How many functions does Lion King have? (task Z, 2026-10-12)

Short answer: **about 1,350 functions are known to exist; the real total is probably 1,400-1,500, with a hard
ceiling near 1,900. The recomp's manifest holds 1,305 entry points (1,380 entry x M/X variants, task FF), all 1,305
with a compiled (AOT) body (task EE; task CC 1,260 / 1,260; task AA 1,259 / 1,255; was 1,244 / 1,085).** So roughly 85-90% of the game's functions are already
in the manifest, and about 90% are compiled.

## Method

`tools/function_count.py [--tables] build/rommap/*.map` (ROM bytes are read from the local ROM, nothing is written).

1. **Compiled.** `generated/snesrecomp/program_manifest.json`: every node is an entry pc with an M/X width.
   Disposition `aot_eligible` = compiled body, `lle_only` = known but run in the interpreter.
2. **Static walk.** Recursive descent over the ROM from every known root: all manifest nodes, every `func`
   and `indirect_dispatch` target in `recomp/*.cfg`, and the reset/NMI/IRQ vectors. Branches, `JMP`/`BRL`/`JML`
   and the fall-through after calls are followed; `REP`/`SEP` update M/X. A `JSR`/`JSL` target is a function.
   A path stops at `RTS`/`RTL`/`RTI`, a RAM-pointer jump, or `BRK`/`COP`/`STP`/`WDM` (a width guess gone wrong or
   data; 40 such stops). `--tables` also reads same-bank word tables behind `JMP (abs,X)`/`JSR (abs,X)`
   (entries taken while they point back into $C0-$C6 and decode to a sane first opcode; 51 targets).
3. **Execution.** 74 `TLK_ROMMAP` maps from `tlk_headless --lle` (all 71 play tapes in `build/playtapes`,
   including the 17 task Z death tapes, plus the 2716M/4735M/6781M per-poll TAS tapes; 6987M is `t_ending`).
   Executed opcode bytes that the walk never reached are grouped into clusters (a gap over 16 bytes starts a
   new one). Those are routines entered only through RAM pointers or data tables (object handlers, script VM
   ops, `JMP ($00A4)` targets that are not constant).
4. **What is left.** Bytes in the code banks $C0-$C6 that no walk, no execution, no data read/DMA touched,
   and that are not $00/$FF, divided by the median walked function size, give the ceiling if all of it were code.

## Numbers

| | count |
|---|---|
| Manifest nodes (entry x M/X) | **1,380** all `aot_eligible` (task FF; task EE 1,378; task AA 1,330: 1,326 / 4 `lle_only`; task Z: 1,315 / 1,154 / 161) |
| Manifest entry pcs | **1,305** (all with an AOT body, task EE; task AA 1,259 / 1,255; task Z: 1,244 / 1,085) |
| Static walk, entry pcs (no tables / with tables) | 1,250 / **1,259** (15 not in the manifest: 6 without tables, in $C0/$C1/$C6; 9 more table targets, all in $C2) |
| Walked entries executed on some tape | 1,192 (67 never ran) |
| Executed code outside the walk | 6,136 opcode starts in **~95 clusters** -> ~95 pointer-only functions |
| **Known total** | **~1,350** (1,259 + ~95; task EE moved 45 of the pointer-only ones into the manifest) |
| Untouched non-fill bytes in $C0-$C6 | 41,771; median walked function 73 bytes -> **at most ~570 more** |
| Ceiling | ~1,920 |

By bank (walked entry pcs / of which in the manifest): $C0 373/369, $C1 176/175, $C2 336/327, $C3 111/111,
$C4 115/115, $C5 113/113, $C6 35/34.

ROM accounting with the same 74 maps (`docs/rom_accounting.md`, regenerated): 179,821 code bytes executed
(was 172,836 before the task Z tapes and the extra TAS maps), 30,108 analysed but never executed; 85.7% of
known code ran at least once.

## Why the best guess is 1,400-1,500, not 1,900

- The untouched 41,771 bytes are mostly data that no tape reads (text, unused tables, sound data reached by
  the SPC not the CPU, data for content the tapes do not reach). Task T found the same banks hold tables
  between functions. Counting all of it as code is the ceiling, not an estimate.
- The ~95 pointer-only clusters are a rough count: one cluster can hold two adjacent small handlers, and one
  function with a long data block inside can split in two. +-30%.
- The walk cannot see code that is only reached through pointers and never executed by any tape (object
  types that never spawn on our routes, unused debug code). That is what lies between the known 1,350 and
  the ceiling; with the executed share of known code at 86%, ~100-150 more is plausible.

## Limits

- Interrupt bodies that run compiled even in `--lle` (NMI `$C09EC6`, `$C0A0F0`) do not mark the rommap, but
  they are manifest roots, so the walk covers them.
- `PLP`/`RTI` keep the current widths and calls are assumed to return with the caller's widths; a few paths
  decode with a wrong width (the 40 bad stops). The manifest's proven exit widths are better; this tool
  only counts entries.
- Manifest nodes include some mid-routine entry points the analyzer split out (shared tails, resume points),
  so "functions" here means distinct entry points, the unit the recompiler compiles.
