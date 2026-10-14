# AGENTS.md

Onboarding for coding agents working on TLKRecomp. Humans start with `README.md`.

## What this is

A static recompilation of *The Lion King* (SNES, USA) built on [snesrecomp](https://github.com/RetroPortingToolKit/snesrecomp):
the 65816 program is translated to C, while the PPU/SPC/DMA stay emulated. It is playable start to finish on Linux
and Windows (cross-built). Longer term the C should stay readable and moddable: a planned mod puts Donkey Kong
in Lion King levels, via DKC3Recomp. The port stays in C; any Rust would come later as a mod layer over FFI.

## Read in this order

1. `docs/HOW_IT_WORKS.md`: pipeline, compiled bodies vs interpreter, timing (fibers, pacing).
2. `docs/VERIFYING.md`: the gates. Nothing lands without them.
3. The last few task sections of `docs/devlog.md` (1,900 lines, newest at the bottom). Every fix has its evidence there.
4. `tools/README.md` for tape/BizHawk tooling. Its dated sections are history: "all four tapes desync in level 1"
   was true on 2026-10-12 and has since been fixed. Current state is in `VERIFYING.md` and the devlog.

Lettered task references across the docs ("task FF", "task Z") point to the devlog section with that tag in its
heading.

## Hard rules

- **No game data in git.** No ROMs, extracted assets, generated C (`generated/`) or BizHawk movies. The `.gitignore` covers ROMs, `build*/`, `generated/`. The four `tapes/*.read.tape` files are
  the only input data committed: CC BY 2.0, converted from TASVideos movies (`LICENSE`, README
  "Watching a speedrun"). Any new tape needs the same credit line and license check before it is committed.
- **Never edit the `snesrecomp/` submodule.** Codegen changes go in `patches/snesrecomp/NNNN-*.patch` (applied to a
  copy in `build/snesrecomp-codegen`); runtime changes go in `cmake/runtime-patches/*.hunks` (a missing anchor
  fails the build, on purpose).
- **Fix generally.** A codegen patch, a runtime hunk, or a cfg line that is proven (`scripts/check_exit_mx.py`
  0 WRONG). No per-address special cases in the runner to make a hash match.
- **`--lle` is the oracle.** Compiled mode must give the same frame and WRAM hashes as `--lle` for the same input,
  and both must match BizHawk (bsnes v115) RAM on the reference speedruns.
- snesrecomp is PolyForm Noncommercial. Never sell or package this commercially. Own code is MIT.

## Build

```sh
git submodule update --init          # a fresh clone or worktree has an empty snesrecomp/
./build.sh                           # generates generated/ only if empty, then builds build/tlk_headless, build/tlk_sdl
```

After **any** change to `recomp/*.cfg` or `patches/snesrecomp/*`, regenerate; a plain build does not:

```sh
python3 scripts/generate_snesrecomp.py [--rom PATH] && cmake --build build -j
```

The generator looks for `Lion King, The (USA).sfc` in the repo dir or its parent (SHA-256 `457abe63...0f3712`).
In a worktree under `.claude/worktrees/` neither path has it: pass `--rom`.

Build cost: a regeneration recompiles all 59 generated bank files (`generated/snesrecomp/*.c`) at `-O3`. No
measured timing exists; time one before quoting an ETA, and count it twice if the Windows build is in play.

Windows: MinGW-w64 cross build into `build-win/` (commands in README), run and gate it under Wine
(`scripts/compare_modes.sh -b <wrapper that execs wine build-win/tlk_headless.exe>`). Never tested on real
Windows. macOS: not attempted; needs a Mac or CI runner, and ucontext fibers on arm64 are unproven.

## Hosts

- `build/tlk_headless`: test host. `--rom --frames --lle --input TAPE --input-polled TAPE --ram-log OUT
  --dump-png --dump-every --save-at --load --log --interp-targets --coverage-log`.
- `build/tlk_sdl`: the player. Same emulation path plus `--hash-at`, `--no-pace`, `--no-audio`, `--record`,
  `--input-raw`, `--selftest-remap`, `--list-drivers`, `--wav`.
- Diagnostics are env vars, all in `runner/*.c`: `TLK_HWLOG` (hardware accesses with master clock), `TLK_ROMMAP`
  (coverage map, use with `--lle`), `TLK_TRACE_OPCODES`, `TLK_MX_HIST`, `TLK_CALL_PAIR`, `TLK_RAM_LOG_DEATH`,
  `TLK_WATCH`, `TLK_FIBER_STATS`, ... `grep -n getenv runner/*.c` for the list.

## Tapes: the trap everyone hits

- `*.tape` = one u16 per **frame**. Fed with `--input`.
- `*.read.tape` / `*.polled.tape` = one u16 per **game pad read**. Must be fed with `--input-polled`. Fed with
  `--input` they desync immediately and silently (Simba jumps around, levels never finish). If a tape looks "not in
  sync", check the flag before the tape.
- A `.bk2` cannot be played directly: its rows include BizHawk's lag frames, ours lag on different frames, so
  per-frame replay drifts. Synced tapes come from `tools/bk2_to_tape.py --polled --read-values --lag <RAM log>`,
  where the RAM log is recorded in BizHawk with `tools/bizhawk/ram_log.lua`. A .bk2 player is out of scope; the
  converted tapes are the supported path.
- Synced references: 6987M, 6781M, 4735M (to movie end), All Bugs Final (`scripts/check_allbugs.sh`).
  Not references: 2716M (bsnes v0.87 core), All Bugs WIP (desyncs at poll 3683 on both modes), anything made on
  Snes9x.
- `tlk_sdl` records live play to `last_session.rec` (replay with `--input-raw`). Scripted runs (`--frames`) skip
  recording so they don't overwrite a session someone wants to reproduce.

## Gates

The full list, commands and pass strings are in `docs/VERIFYING.md`. Short form, run after every change:

| Gate | Pass |
|---|---|
| `scripts/compare_modes.sh 600 3000` and `-t build/tapes/6987M.tape 1200 6000` | MATCH, WRAM same |
| `scripts/check_all_tapes.sh -s` | 71 MATCH, 0 MISMATCH |
| `scripts/check_idle_lle.sh` | IDLE MATCH |
| `scripts/check_native_decomp.sh 3000` | IDENTICAL |
| save f3000 / load in new process / f6000 | `946033bd10683657` |
| TAS sync via `tools/ram_sync_compare.py` | 6987M 36,904, 6781M 37,093, 4735M 38,870 |
| `scripts/check_allbugs.sh` (~13 min) | ALLBUGS PASS |
| `scripts/check_exit_mx.py` | 0 WRONG |
| `SDL_VIDEO_DRIVER=dummy build/tlk_sdl --rom R --no-audio --selftest-remap` | PASS (0 failed). Under Xvfb pad events don't arrive and it fails |
| `python3 -m pytest -q tools/tests` | no failures (needs the submodule checked out) |
| run `tlk_sdl` and look at the window | right picture |

`build/tapes/`, `build/playtapes/` and `build-host/out/sync/` are local (gitignored) and regenerated from your own
ROM and movies with `tools/make_play_tapes.py` and `tools/bk2_to_tape.py`.

The last gate is not decoration: one change passed every hash with a black window, because hashes read the pixel
buffer, not what SDL shows.

## When something differs

`scripts/first_divergence.py` (first frame) → `TLK_HWLOG` + `tools/aot_pace/pace_diff.sh` (first hardware access
by master clock) → `scripts/bisect_interp.py` (which compiled body) → fix generally → all gates again. Most past
bugs were codegen (pushed-frame RTS/RTL calls, bank carry on `(dp),Y`) or timing (SPC handshake, `$4212` poll tick),
not cfg typos.

## Working norms

- Commit messages: short title only, no prefix, no body, no AI attribution. Small commits ordered for review.
- Every status claim names its evidence. "All gates pass" means the output was read, not assumed. Mark anything
  else as "headless only" or "not run". Hashes alone are not enough (see the SDL gate above).
- Verify at the surface a user touches: the SDL window, the unpacked release tarball/zip, a real key press.
- Smallest change that solves the problem; propose the bigger one separately with its cost.
- Use a git worktree for parallel work, and record each landed fix with its evidence in `docs/devlog.md`.
- Don't wait on long runs with `while pgrep -f ...` loops: `pgrep -f` matches the loop's own command line and it
  never exits. Don't run a bare `wineserver -k` while other Wine runs are in flight; it kills them.

## Open threads (2026-10-13)

- ~4% of non-idle CPU still interpreted: known routines entered via `$C0B5A9` / `$C0D7F3` at M0X1 into M0X0-only
  bodies; some targets never return (stack reset in the level loader, demo exit). Needs "may not return" tail
  dispatch in codegen. Undecided whether to accept or attempt. Devlog task FF.
- `$7E2006` MVN RAM routine stays interpreted by choice (~1% of run time).
- A level-3 water-death screen scramble was seen once in the SDL build (after task X) and never reproduced. A
  `last_session.rec` of it would help.
- ~52 known routines never run on any tape (`tools/never_ran.py`); needs hand play in late levels, bonus stages,
  ending.
- Windows: audio, gamepad, the ROM file dialog and real hardware untested. Tapes not in the release zip/tarball.
- Remap self-test: five failures seen once under Xvfb; passes with `SDL_VIDEO_DRIVER=dummy`. When the failures
  began was never bisected.
- Auto-selecting `--input-polled` for `*.read.tape` was proposed, not built.
