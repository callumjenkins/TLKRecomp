# Level select from boot (USA 1.0)

Status 2026-10-12: input-only recipe, no RAM pokes, verified in `tlk_headless --lle` for all
14 level indices ($00-$0D). Tapes come from `make_level_tape.py`.

## The debug menu

TCRF: in Options press **B, A, R, R, Y** ("Barry" Green) for the debug menu; in there,
**R, A, A, B** turns on upside-down mode. Where the ROM handles it:

| Address | What |
|---|---|
| `$C46993` | Options task. When its delay timer `($68),$2C` is 0 it calls the checker at `$C469AA` |
| `$C3C1BB` | `Check_Code_BARRY`: compares new presses `$5A` with the table at `$C3C200` (`$8000 $0080 $0010 $0010 $4000 $FFFF`, so B A R R Y). The index is in `$0AD1` and the previous pad in `$0AD5`. Any other non-zero press resets the index. Returns `$A68E=$FFFF` once the whole sequence is in |
| `$C469B6` | on a match: `JSL $C46E59`, the debug menu task |
| `$C46E59` | Debug menu. Draws CPU/PPU1/PPU2 versions, `INVUL`, `LEVEL` and `VERSION: .99`. Up/Down move the cursor (items 0..1). A, B or Y (`$62 & $C080`) calls `$C3BFF9` on the current item. Start (`$62 & $1000`) leaves through `$C465F0` |
| `$C3BFF9` | `$7FFF9C+2*item = (v+1) mod limit`, limits at `$C3C016` = {3, $0E}. Item 0 is `$7FFF9C` (invulnerable: 0/1/2), item 1 is `$7FFF9E` (current level, 0..13) |
| `$C3C20C` | `Check_Code_RAAB`, table at `$C3C251` (`$0010 $0080 $0080 $8000 $FFFF`), index in `$0AD3`. On a match the menu writes `$7FFF90=1` (upside-down) |

A new game started from the title begins at `$7FFF9E`.

## Recipe (all levels)

Frame timings for `tlk_headless --lle` from power-on (one tape word per frame). Every tap is
2 frames held then 4 released. The gaps matter: the options task ignores input while its
timer runs, so presses spaced only 6 frames apart miss (tried).

1. wait 700 (title shows START/OPTIONS at about 600)
2. Down, wait 10, Start, wait 80 (Options screen)
3. B, wait 20, A, wait 20, R, wait 20, R, wait 20, Y, wait 60 (debug menu)
4. optional: A x k on INVUL (k = 1 or 2)
5. Down (cursor to LEVEL), wait 20, then **A x N**, each followed by wait 20 (`$7FFF9E = N`)
6. wait 20, Start (back to title), wait 60, Up, wait 20, Start (game starts at level N)

```sh
python3 -I tools/re/make_level_tape.py --level N out/levelselect/tapes/levelN.tape
build/tlk_headless --rom "$R" --lle --frames 2600 --input out/levelselect/tapes/levelN.tape \
   --dump-png out/levelselect/levelN.png --ram-log out/levelselect/levelN.ram
```

The tape ends about 1,200 + 26*N
frames in; after that the pad is idle.

## Results (frame 2600, `--lle`, no input after the tape)

`$7FFF9E` is the `lvl` column of `--ram-log`. Screenshots: `out/levelselect/levelN.png`, all in
`out/levelselect/montage.png` (row 1 = 0..6, row 2 = 7..13).

| N | Level | `$7FFF9E` at f2600 | Screenshot (described) | Status |
|---|---|---|---|---|
| 0 | Pride Lands | 0 | cub Simba on grass, pink sky | OK |
| 1 | Can't Wait to be King | 1 | "ROAR AT MONKEYS" title card, cub on tree | OK |
| 2 | Elephant Graveyard | 2 | cub by the elephant skull; lives 7 (he died once while idle) | OK |
| 3 | Stampede | 3 | f2000 shows wildebeest running at the camera; by f2600 Simba is dead (HP 0) | OK |
| 4 | Simba's Exile | 4 | cub on a rock, red sunset | OK |
| 5 | Hakuna Matata | 5 | jungle and waterfall, adult Simba | OK |
| 6 | Simba's Destiny | 6 | night jungle, adult Simba | OK |
| 7 | Be Prepared | 7 | red lava cave, adult Simba | OK |
| 8 | Simba's Return | 8 | grey-green rocks, adult Simba | OK |
| 9 | Pride Rock | 9 | purple rocks, red sky, adult Simba | OK |
| 10 | Bug Toss | 0 | Timon intro, "BUG TOSS" round, "00 BUGS CAUGHT", then Pride Lands (`lvl`=0 from f2021): normal bonus end, same in BizHawk (see 2026-10-12 section below) | OK |
| 11 | Bug Hunt I | 11 | Timon in teal jungle, "10 1up", timer 27 | OK |
| 12 | Bug Hunt II | 12 | Timon, timer 28 | OK |
| 13 | Bug Hunt III | 13 | Timon on a branch, timer 28 | OK |

Not verified: Bug Toss started this way might also be broken on real hardware or bsnes (bonus
levels normally follow a regular level and use `$7FFFA0`, which a fresh boot has not set),
or the LLE interpreter might be at fault. Check in BizHawk with the same input.

The `INVUL` option (`$7FFF9C`) is in the tape generator (`preset(level, invul=k)`) but was
not run here.

## RAM-poke alternative (not run, no poke support in tlk_headless)

The practice hack's "Restart Level" does `$7FFFA0 = target; $7EA692 = $FFFF` while in a level.
A negative `$A692` loads from `$7FFFA0` (rammap.txt: "MSB loads from $7FFFA0"). That route
needs a host RAM-write hook. The input tape above needs none.

## Bug Toss (level 10) checked in BizHawk

Verdict: **not broken.** The level-10 tape, fed frame by frame to BizHawk 2.11.1 (Snes9x core, no movie), shows
the same sequence as `tlk_headless --lle` a few frames apart: Timon intro, black, "BUG TOSS" title over the log
arch with Pumbaa, Timon throwing bugs, black, "00 BUGS CAUGHT" (the tape is idle, so nothing is caught), then
Pride Lands. `$7FFF9E` steps 1..A at f999-1233 (menu), stays `000A`, then drops to 0 at f2029 (headless `--lle`:
f2021). The "garbled BG / blank screens" above were the black transition frames between those screens; the
fall-through to level 0 is the normal end of a bonus round (next level comes from `$7FFFA0`, 0 on a fresh boot).
