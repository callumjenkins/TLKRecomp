# tools/

Host-side Python helpers for the Lion King recomp. Tests: `python -m pytest -q tools/tests` (run from `TLKRecomp/`).

## bk2_to_tape.py: BizHawk movie to input tape

```sh
python tools/bk2_to_tape.py -d build/tapes path/to/*.bk2
```

Output per movie in `build/tapes/` (gitignored via `/build/`):

| File | Content |
| --- | --- |
| `<stem>.tape` | joypad 1, one little-endian u16 per frame |
| `<stem>_p2.tape` | joypad 2, only when the LogKey has a `#P2` group |
| `<stem>.resets.txt` | 0-based frame numbers of Reset/Power events (comment lines start with `#`) |

Word layout is the `$4218/$4219` auto-joypad layout: bit15 B, 14 Y, 13 Select, 12 Start, 11 Up, 10 Down,
9 Left, 8 Right, 7 A, 6 X, 5 L, 4 R, bits 3..0 always 0. The low byte is `$4218`, the high byte is `$4219`.

The column order is read from the `LogKey:` line, not hardcoded. All four movies use the BizHawk order
`Up Down Left Right Select Start Y B X A L R` with a `Reset|Power` group first.

What the movies contain (measured 2026-10-12):

| Movie | Core / emu | Frames (tape) | Expected | P2 | Resets |
| --- | --- | --- | --- | --- | --- |
| 6987M | BSNESv115+ / 2.11 | 39,088 | 39,088 | no | 0 |
| 6781M | BSNESv115+ / 2.11 | 39,270 | 39,270 | no | 0 |
| 4735M | BSNESv115+ / 2.8 | 41,047 | 41,047 | no | 0 |
| 2716M | BSNES (old libsnes core) / 1.7.4 | 46,348 | 46,348 | yes, all zero | 0 |

All four have the same ROM SHA-1 `E5319132495FD34D03F141962D9D6E3D76FE7B02`. The first non-zero input is
at frame 591 in all four.

Format notes:
- 2716M (BizHawk 1.7.4) has no `[Input]` marker and logs a second controller group. The parser keys on
  `|`-prefixed lines, so both formats work. Its P2 column is always empty, so `2716M_p2.tape` is all zeros.
- Lag frames: bk2 has one input line per emulated frame, whether or not the game polled the pad. Lag
  frames are not flagged and not removed. A replaying host must advance one tape word per emulated frame
  (per NMI/VBlank), not per game poll, or the movie desyncs.
- Sync risk: the movies were made on bsnes cores. 2716M also used the old libsnes core (Compatibility
  profile). Timing differences between those cores and the recomp runtime can desync a replay.

## merge_coverage.py: coverage capture into recomp/bank*.cfg

```sh
python tools/merge_coverage.py capture.json capture.jsonl --cfg-dir recomp      # dry run: prints a diff
python tools/merge_coverage.py capture.json capture.jsonl --cfg-dir recomp --apply
```

Inputs:
- Real snesrecomp captures (`SNESRECOMP_TIER2_CAPTURE=1`, see `snesrecomp/docs/COVERAGE_FEEDBACK.md`):
  `.json` checkpoint and `.jsonl` journal. These go through upstream
  `snesrecomp/recompiler/v2/coverage_profile.load_profiles`, so sequence dedup, identity checks and
  counter merging behave exactly as in `tier2_ingest.py` and `v2_emit.py`.
- JSON without a v2 identity (synthetic) falls back to a lenient local parser.
- Text traces in the older Lua tracer format: `FUNC <pc24> <m> <x>` and
  `IND <site_pc24> <target_pc24> ...`.

What it proposes:
- `call_gap` rows become `func FN_BBXXXX XXXX entry_mx:M,X`. An existing func with no width gets
  `entry_mx:` added in place, or a separate `entry_mx_at` line with `--entry-mx-at`.
- `indirect_dispatch` and `indirect_goto` rows become `ptrcall` or `ptrtail` `indirect_dispatch` lines.
  Targets are merged into an existing `targets:` line. Table-form (`idx:X tables:`) lines are left alone.

Rows that are not promoted are counted under "Not promoted". This follows upstream `promotion_reason`:
- bailed rows
- E=1 rows
- missing M/X
- `goto_gap` and `bank_miss` landings (these need a real function boundary)
- WRAM and other non-ROM targets

HiROM mirrors (`$40-$7D`, and `$00-$3F`/`$80-$BF` at `:8000+`) are folded onto `$C0-$FF` to match the
`bankcX.cfg` files.

Upstream alternatives:
- `snesrecomp/tools/tier2_ingest.py` audits a capture.
- `v2_emit.py --profile-manifest` feeds captures straight into analysis without editing cfg.

This tool is for permanently seeding cfg. Every change needs a regen, a build and a replay comparison
(`snesrecomp/tools/coverage_replay.py`).

## Poll-aligned replay and RAM sync against BizHawk

- `tools/bizhawk/ram_log.lua`: whole-movie BizHawk lag log (`LAG_OUT`, `lag_log.lua` format) plus a per-frame RAM
  log (`RAM_OUT`: `f lag rng hp x y lvl lives pad`).
- `tools/bk2_to_tape.py --polled --read-values --lag <RAM log>`: tape of the words the game actually read in
  BizHawk, one per poll. (`--polled --lag <lag log>` gives the movie rows of non-lag frames instead.)
- `tlk_headless --input-polled <tape>` feeds one word per `$C0A07C` pad read. `--ram-log <path>` writes the same
  format as `ram_log.lua`.
- `tools/ram_sync_compare.py HL_RAM BH_RAM`: poll-aligned RAM compare, reports the first persistent desync.
- `tools/bizhawk/wram_dump.lua`: full WRAM dumps at chosen frames.

## Screenshot sync (older, per-frame)

`tools/sync_compare.py` (headless PNGs vs BizHawk shots, best offset per frame), `tools/bizhawk/sync_shots.lua`
(BizHawk screenshots + `$7FFF9E` level log; env `SHOT_DIR`, `SHOT_EVERY`, `SHOT_WIN`, `SHOT_MAX`),
`tools/bizhawk/lag_log.lua` (BizHawk lag frames). Per-frame tapes drift from BizHawk at lag frames; prefer the
poll-aligned path above.

## aot_pace

- `aot_pace/interp_samples.c`: runs `interp816_runOpcode` for every opcode x M x X on random states with a
  logging bus. Build: `gcc -O1 -w -Isnesrecomp/runner/src/snes -Isnesrecomp/runner/src -o /tmp/s
  tools/aot_pace/interp_samples.c snesrecomp/runner/src/snes/interp816.c`, run `/tmp/s 100 > samples.txt`.
- `aot_pace/verify.py samples.txt [--derive OUT.py]`: checks the emitter's pacing model
  (`build/snesrecomp-codegen/recompiler/v2/aot_pace.py`) against those samples (cycles, bus addresses, master
  clocks); `--derive` rebuilds its cycle table.
- `aot_pace/pace_diff.sh TAPE FRAMES TRACE_BIN`: first hardware access, then first opcode, whose master clock
  differs between `--lle` and default mode (`hw_diff.py`, `op_merge.py`). TRACE_BIN is a `-DAOT_PACE_TRACE`
  build.

## Opcode-level timing compare against BizHawk

Logs `PC master_clock` for every opcode between two polls on both sides, then aligns the streams
(`$BIZHAWK` is your BizHawk install, `$CFG` its config, `$R` the ROM):
```sh
TRACE_OUT=$PWD/bh_tr.txt TRACE_FROM=1659 TRACE_TO=1665 LD_LIBRARY_PATH=$BIZHAWK/dll:$BIZHAWK \
  xvfb-run -a mono $BIZHAWK/EmuHawk.exe --config=$CFG --movie=6987M.bk2 \
  --lua=$PWD/tools/bizhawk/trace_opcodes.lua "$R"
TLK_TRACE_OPCODES=1659,1665,$PWD/hl_tr.txt ./build/tlk_headless --rom "$R" --frames 1980 --lle \
  --input-polled 6987M.read.tape
python3 tools/compare_opcode_traces.py hl_tr.txt bh_tr.txt --window 3000
python3 tools/compare_lags.py hl_ram.txt bh_ram.txt
```
- `TRACE_FROM=0` traces from power-on to poll `TRACE_TO`. BizHawk logs the cycle count before each opcode
  runs, as headless does. A DMA's stall shows up on the opcode after `STA $420B` in BizHawk and on the
  STA itself in headless.
- `compare_opcode_traces.py` prints each place the PC streams part (spin loops of different length,
  interrupts landing elsewhere) with the clocks gained or lost, then the per-PC cost differences.
  Only interpreted opcodes are logged: the NMI subroutines that headless runs compiled even under
  `--lle` show up as splits at `$C09E82..$C09E8B`.
- `compare_lags.py` lists the polls where the number of lag frames since the previous poll differs.
- Some BizHawk builds return 0 from `emu.totalexecutedcycles()` for every opcode; check that before trusting
  the BizHawk side.
- `tlk_headless` env `TLK_ARAM_DUMP=<frame>,<path>` writes the 64 KiB SPC RAM after that frame. TLK's sound
  driver main loop is at SPC `$08DF` (timer 0 tick, command check `$192E`), and the streaming upload
  loop is at `$19CF-$19F6`.

## Coverage tapes and ROM accounting

- `make_play_tapes.py OUTDIR --task-t` writes the coverage tapes (`t_*.tape`: options incl. sound test and
  triggers, the three difficulties, pause/unpause, game over -> continue YES / NO, 12000-frame attract, and
  `t_ending.polled.tape` = the 6987M per-poll tape, ending + credits by f49000).
- `*.polled.tape` is fed with `--input-polled` by `scripts/check_all_tapes.sh`, `coverage_runs.sh`
  (`POLLED_FRAMES`, default 49000), `first_divergence.py` and `interp_share.py`.
- `tlk_headless` env `TLK_ROMMAP=<path>` (`runner/tlk_rommap.c`, use with `--lle`) writes one flag byte per ROM
  byte: executed opcode/operand, CPU data read, VRAM / other DMA source, read by the decompressor, read by the
  sound library.
- `rom_accounting.py MAP... [--md out.md]` classes every ROM byte per bank (code executed, audio, level blobs,
  compressed, gfx DMA, other DMA, CPU data, analysed-but-unexecuted code, fill, unknown).
- `function_count.py` and `never_ran.py` read the same maps (`docs/function_count.md`).
