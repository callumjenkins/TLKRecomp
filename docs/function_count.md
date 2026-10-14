# How many functions does Lion King have?

Short answer: **about 1,390 functions are known to exist; the real total is probably 1,450-1,550, with a hard
ceiling near 1,900. The recomp's manifest holds 1,305 entry points (1,380 entry x M/X variants), all
with a compiled (AOT) body.** So roughly 85-90% of the game's functions are already in the manifest, and all of
those are compiled.

## Method

`tools/function_count.py [--tables] build/rommap/*.map` (ROM bytes are read from the local ROM, nothing is written).

1. **Compiled.** `generated/snesrecomp/program_manifest.json`: every node is an entry pc with an M/X width.
   Disposition `aot_eligible` = compiled body, `lle_only` = known but run in the interpreter.
2. **Static walk.** Recursive descent over the ROM from every known root: all manifest nodes, every `func`
   and `indirect_dispatch` target in `recomp/*.cfg`, and the reset/NMI/IRQ vectors. Branches, `JMP`/`BRL`/`JML`
   and the fall-through after calls are followed; `REP`/`SEP` update M/X. A `JSR`/`JSL` target is a function.
   A path stops at `RTS`/`RTL`/`RTI`, a RAM-pointer jump, or `BRK`/`COP`/`STP`/`WDM` (a width guess gone wrong or
   data; 42 such stops). `--tables` also reads same-bank word tables behind `JMP (abs,X)`/`JSR (abs,X)`
   (entries taken while they point back into $C0-$C6 and decode to a sane first opcode; 51 targets).
3. **Execution.** 74 `TLK_ROMMAP` maps from `tlk_headless --lle` (all 71 play tapes in `build/playtapes`,
   including 17 death tapes, plus the 2716M/4735M/6781M per-poll TAS tapes; 6987M is `t_ending`).
   Executed opcode bytes that the walk never reached are grouped into clusters (a gap over 16 bytes starts a
   new one). Those are routines entered only through RAM pointers or data tables (object handlers, script VM
   ops, `JMP ($00A4)` targets that are not constant).
4. **What is left.** Bytes in the code banks $C0-$C6 that no walk, no execution, no data read/DMA touched,
   and that are not $00/$FF, divided by the median walked function size, give the ceiling if all of it were code.

## Numbers

| | count |
|---|---|
| Manifest nodes (entry x M/X) | **1,380**, all `aot_eligible` |
| Manifest entry pcs | **1,305**, all with an AOT body |
| Static walk with tables, entry pcs | **1,318** (13 not in the manifest) |
| Walked entries executed on some tape | 1,230 (88 never ran) |
| Executed code outside the walk | 3,811 opcode starts in **72 clusters** -> ~72 pointer-only functions |
| **Known total** | **~1,390** (1,318 + ~72) |
| Untouched non-fill bytes in $C0-$C6 | 39,992; median walked function ~77 bytes -> **at most ~520 more** |
| Ceiling | ~1,910 |

By bank (walked entry pcs / of which in the manifest): $C0 382/379, $C1 187/187, $C2 344/334, $C3 112/112,
$C4 123/123, $C5 124/124, $C6 46/46.

ROM accounting with the same 74 maps (`tools/rom_accounting.py`): 179,821 code bytes executed, 32,552 analysed
but never executed; 84.7% of known code ran at least once.

## Why the best guess is 1,450-1,550, not 1,900

- The untouched 39,992 bytes are mostly data that no tape reads (text, unused tables, sound data reached by
  the SPC not the CPU, data for content the tapes do not reach). ROM accounting shows the same banks hold
  tables between functions. Counting all of it as code is the ceiling, not an estimate.
- The ~72 pointer-only clusters are a rough count: one cluster can hold two adjacent small handlers, and one
  function with a long data block inside can split in two. +-30%.
- The walk cannot see code that is only reached through pointers and never executed by any tape (object
  types that never spawn on our routes, unused debug code). That is what lies between the known 1,390 and
  the ceiling; with the executed share of known code at 85%, ~100-150 more is plausible.

## Limits

- Interrupt bodies that run compiled even in `--lle` (NMI `$C09EC6`, `$C0A0F0`) do not mark the rommap, but
  they are manifest roots, so the walk covers them.
- `PLP`/`RTI` keep the current widths and calls are assumed to return with the caller's widths; a few paths
  decode with a wrong width (the 42 bad stops). The manifest's proven exit widths are better; this tool
  only counts entries.
- Manifest nodes include some mid-routine entry points the analyzer split out (shared tails, resume points),
  so "functions" here means distinct entry points, the unit the recompiler compiles.
