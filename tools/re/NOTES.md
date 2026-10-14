# The Lion King (SNES, USA): level data map

ROM: `Lion King, The (USA).sfc`, 3 MiB, headerless, HiROM ($FFD5 = $31),
SHA-256 `457abe634e0a8be03e29513a3dca8f3e9d0ddc6bf97d8931f2316094260f3712`.
HiROM banks $C0-$FF map linearly: PC offset = address & $3FFFFF.

Starting labels/RAM map: `../SNES_TLK_PracticeHack_U/resources/{LK_labels.asm,rammap.txt}`.

## Level header table

`$C00357`: 16 x 24-bit pointers, indexed by level number (`$7FFF9E` current,
`$7FFFA0` next). Each points to a `$32`-byte record in bank $C0.

| Level | Record | Name |
|---|---|---|
| $00 | $C00387 | Pride Lands |
| $01 | $C003B9 | Can't Wait to Be King |
| $02 | $C003EB | Elephant Graveyard |
| $03 | $C0041D | The Stampede (special engine) |
| $04 | $C0044F | Simba's Exile |
| $05 | $C00481 | Hakuna Matata |
| $06 | $C004B3 | Simba's Destiny |
| $07 | $C004E5 | Be Prepared |
| $08 | $C00517 | Simba's Return |
| $09 | $C00549 | Pride Rock |
| $0A-$0D | $C005DD.. | bonus games? (share gfx and routine) |
| $0E | $C0057B | special, no gfx (cutscene?) |
| $0F | $C0070E | copy of $04 with a different checkpoint |

Level names for $00-$09 follow the game order and the hijack comments in
`src/timer.asm`. They are consistent with `next_level`, but not otherwise verified.

## Record layout ($32 bytes)

Status: **confirmed** = checked against bytes or another source, **likely** = pattern
across all 16 records, **?** = guess.

| Off | Size | Field | Status | Evidence |
|---|---|---|---|---|
| +00 | 3 | block collision | confirmed (code) | decompressed to `$7E4434`; 4 bytes per block, indexed by `(cell & $FFF8) >> 1`; first word + sub-position indexes +03. Read by collision probe `$C079EE` |
| +03 | 3 | collision height tables | confirmed (code) | 16 bytes per shape (`00..`, `01..`, `02..` ramps); value `& $1F` -> `$A68E`. Shared $EA2001 by all platform levels |
| +06 | 3 | **BG tiles** | confirmed (render) | 4bpp, tile 0 = first tile |
| +09 | 3 | **level map** | confirmed (render) | one **16-bit word per 16x16 cell**, row-major; size == +12 x +14 for 13/14 levels. Copied to `$7F0000` at load. Low 3 bits always 0 in Pride Lands |
| +0C | 3 | palette | confirmed | data is BGR555 (`0000 3F7F 1EFB ...`); `CGRAM_Transfer_DF` takes a long addr at level load |
| +0F | 3 | **block table** | confirmed (render) | `cell & $FFF8` = byte offset to 4 SNES tilemap words (TL, TR, BL, BR): tile `& $3FF`, palette bits 10-12, priority 13, H/V flip 14/15 |
| +12 | 2 | map row size (bytes) | confirmed | = cells x 2; camera max X = this x 8 - 256 |
| +14 | 2 | map height (cells) | confirmed | 16 px each |
| +16 | 2 | checkpoint_x | confirmed | `rammap.txt` / `LK_labels.asm` |
| +18 | 2 | checkpoint_y | confirmed | same |
| +1A | 3 | gfx_e | compressed, graphics | loader `$C082D6` decompresses to `$7E2434` and DMAs it to VRAM in `$500` chunks; Stampede ($EBEE95) has its own path |
| +1D | 1 | flags? | ? | $30, $22, $10, $40, $00 ... |
| +1E | 4 | ? | ? | zero except $05 and $09 (look like $C0 pointers) |
| +22 | 4 | ? | ? | increasing word + small byte |
| +26 | 3 | level_routine | confirmed code | $C32960 = `PHP; REP #$30; PHD; LDA $A68C`; platform levels share $C329xx, Stampede $C45CD7, bonus $C5765F |
| +29 | 1 | music? | likely | loader passes it to `JSL $C22ACA` and `JSL $C22B36` (audio bank $C2) |
| +2B | 3 | ptr_list_entry | likely | levels point at successive 3-byte entries of a pointer list at $C587C4 (into bank $C0); 0 in some levels |
| +2E | 2 | next_level | likely | 1,2,3,F,5,6,7,8,9,0; bonus = $FFFF; $0F -> $04 |
| +30 | 2 | ? | ? | |

## Code

| Address | What | Status |
|---|---|---|
| $C08275 | level load: `level*3` -> record pointer in `$DF` (long) | confirmed |
| $C070F6 | reads +0F, +03, +12, +14 into direct page | confirmed |
| $C07189 | clears RAM, decompresses +00 to `$7E4434` | confirmed |
| $C1CCFE | **decompressor** (labelled `Write_MVN_Code` by the practice hack). In: src long `$0D`, dst `$0A`/`$0C`. Writes through `$2180` DMA, back-refs through an `MVN` built at `$7E2006` | confirmed; reimplemented in `decompress.py`, all 54 level blobs decode cleanly |

## Compression format

See the docstring in `decompress.py`. Literals `$81-$BF`, end `$80`, short
back-ref `$00-$7F` (12-bit distance), long back-ref `$C0-$FD`/`$FF` (absolute
output offset), fill `$FE`.

## Open questions / next steps

1. ~~Map cell -> tiles~~ done: `render_level.py` renders the foreground of every
   platform level (`out/level_*.png`). Verified visually on $00, $01, $05.
2. Background/sky layer (BG2) is not drawn yet; where does it come from?
3. Bonus level $0A has 32 extra map bytes.
4. Object/enemy placement: probably reached from `level_routine` or `+22`.
5. Stampede ($03) uses a different engine; skip it for the mashup.

## Recomp feasibility test (2026-10-11)

`snesrecomp build` (analyzer from the DKC3Recomp submodule, native Rust backend) on the
USA ROM: **9 roots -> 24 functions**, then stops. Output in `../TLKRecomp-test/`.

Blockers on the first pass:
- Boot: call to `$C22C4E` (audio bank), exit M/X width not provable -> rest of boot truncated.
- NMI: calls to `$C0A0F0` and `$C09EC6`, same problem.
- IRQ: `JMP ($0A00)` through the RAM IRQ pointer (`rammap.txt`).
- `func` lines added to `config-seeded/bankc0..c5.cfg` did not become analysis roots;
  the native analyzer still reported 9 roots. Cause not found yet.

Scale: 361 long-call (JSL) targets called 2+ times, almost all in banks $C0-$C6
(C0:64 C1:83 C2:31 C3:62 C4:39 C5:51 C6:26). Plus local JSR functions. Rough guess
1,500-3,000 functions; DKC3Recomp's config lists 4,718 funcs and 353 jump tables,
generated from a full disassembly that TLK does not have.

## Public symbols (2026-10-12)

`symbols/ram.csv` (88 rows) and `symbols/rom.csv` (53 rows) come from `symbols/build_symbols.py`
(rerun it to regenerate). Sources:
- `ph_rammap` / `ph_labels`: SNES_TLK_PracticeHack_U `resources/rammap.txt` and `LK_labels.asm`. Only the vanilla labels; the hack's own RAM/SRAM is skipped.
- `lion_lua`: `ext/lionking_lua/lion.lua` (AkiteruSDA, cloned). Gives the SNES object linked list `$76/$78/$7A`, Simba subpixel/speed `$B248-$B251`, camera `$B29B/$B29D`, RNG bytes `$1E00-02`, RNG roll `$C0CBBA`. Object fields at `obj+$08` X, `+$30` handler pointer (`$DC17` = geyser inactive, `$D8CC` = active), `+$46` countdown, `+$4C`.
- `tlk_map`: ours (debug-menu code, see `level_select.md`).
- **Data Crystal RAM map: not fetched.** Cloudflare returns 403 to curl and WebFetch, and archive.org has no snapshot. Its range ($7E0056-$7FFFB4, about 61 entries) matches `rammap.txt` exactly, so it is probably the same data.

Conflicts: only `$7E1E00`, which `rammap.txt` lists as a 3-byte seed and lion.lua as byte A. Both are right.
The practice hack also notes "first byte of RNG returned" for `$A68E`, but disassembly shows
`$A68E` is a general return-value slot (code checkers return `$FFFF` there).

RAM spot-check, verified with `--ram-log` in `tlk_headless --lle` (`out/levelselect/ram_6987M.txt`
= 6987M tape for 8,000 frames, plus the level tapes):
- `$7FFF9E` current level: `$0E` on title/boot, `0` at f585 when the game starts. In the debug menu each A press goes 1→2→3 (f999/1025/1051), and the game then starts in that level.
- `$7FFFAA` lives: 3 → 2 (f587) → 8 at f625 (easy difficulty sets it). Level-2 run: 8 → 7 after an idle death.
- `$7E2004` health: 4 → 3 → 1 → 0 across the 6987M run.
- `$7EB21B/$7EB21D` Simba X/Y: change every frame in levels (for example `02CE,04C4` at f1200). Both stay 0 in the Stampede (different engine).
- `$7E1E00` RNG: changes as play goes on (`A35757` → `8C5E40` → `60F4A8` ...).
- `$7E005A` filtered pad: reads `$0080` on the A-press frames in the debug menu.

## TCRF (fetched through archive.org: tcrf.net is behind Cloudflare too)

- Debug menu: in Options press B A R R Y. It shows CPU/PPU1/PPU2 versions, INVUL, LEVEL and VERSION .99. R A A B inside it turns on upside-down mode. ROM side is in `level_select.md`.
- Prototypes (Proto:The_Lion_King_(SNES)), four known builds:
  - **March 22nd, 1994**: very early alpha. Pencil-test sprites, L+R+Start+Select level selector, starts on an early Stampede. TCRF says nothing about where or how it was dumped.
  - **Early Prototype**: dumped by "Z" in 2015. Documented on Hidden Palace, with a download (`Lion King (Early Prototype).zip`, 2 MB). The SMS/Game Gear versions are based on it. Levels can't be completed. Level select is Left+Select.
  - **Version 0.21**: on Hidden Palace. Has unused Simba sprites and a bonus level found in no other build (Bug Hunt I-IV).
  - **Version 0.48**: on Hidden Palace. Close to final, though some levels have no exit. Neither BARRY nor Left+Select works in it.
  - No ROMs downloaded.

## Graphics: backgrounds and sprites (2026-10-12/13)

The sprite/background experiment scripts are not in this repo (one-off, tied to local savestates); the formats below are what they used.

- **Savestate layout** (`TLKRecomp/runner/tlk_savestate.c`): find the `"PPU0"` tag; then 0x40 bytes of PPU registers, CGRAM, OAM, high OAM, VRAM (`st.py`).
- **Gameplay BG setup** (levels 00-09): BG mode 1. BG1 = foreground, tilemap at VRAM word 0x4000, 64x32. BG2 = background, tilemap 0x4800, 64x32, 4bpp, char base 0. BG3 = HUD. Full maps with backgrounds: `bg.py` + `comp.py` (foreground decoded from ROM as in `render_level.py` with colour 0 transparent, BG2 band tiled behind; sky above the band is a filled colour, not from the game).
- Stampede has no tile map (pseudo-3D). Bonus stages: Bug Toss is one screen, Bug Hunts scroll vertically. Level tapes l10-l13 / d_10-d_13 show the bonus stage only ~f1500-2100 before dropping to Pride Lands.
- **Sprite frame records** (gameplay, `fmtb.py`; parsed set saved as `fsB.pkl`): each record follows an `AA AA` separator: count byte, type byte (low nibble = number of 16x16 pieces), bounding box, tile count x16, hitbox values, 3-byte tile-data address, then one (dx,dy) per piece (16x16 first, then 8x8), up to the next `AA AA`. Tile data is a two-row strip of 8x8 tiles: each 16x16 piece takes two columns; 8x8 pieces fill the following columns in groups of four (TL,TR,BL,BR). 1,551 records parsed, ~1,020 point into ROM; others point to WRAM ($7E/$7F: graphics unpacked at runtime, e.g. Timon, Pumbaa, wildebeest).
- Cub Simba also has an older-style record set (24 frames at file 0x11C20). His tiles are stored raw: one pose = 768 bytes = six 16x16 pieces in OAM slot order, e.g. file 0x1AB84B ($DA:B84B); poses back to back ~0x1AA04B-0x1AE84B. Palette 0 raw at 0x7B93 ($C0:7B93).
- Record/tile/palette offsets for cub/adult Simba, gorilla, hyena, ostrich, vulture, Scar, bug: see the table in the blog's "Straight from the ROM" captions (`../blog/`), palettes found by matching snapshot colours (not traced). Rhino, bats, a white spotted cat also present (unidentified cat).
- Rafiki frames: records 0-13, tiles at 0x18C119 (palette not found yet; title-screen palette renders him wrong).
