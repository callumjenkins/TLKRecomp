# Verifying the recompiled build

The rule: for the same input, the compiled build must produce the same frames and RAM as snesrecomp's interpreter
(`--lle`), and both must match a reference emulator on real runs. Every change to `recomp/*.cfg`, a patch or the
runner is checked with the gates below before it lands.

All commands run from `TLKRecomp/` after `./build.sh`. Everything they read or write (tapes, logs, reference
captures) lives under `build*/`, which is not committed: you regenerate it from your own ROM and movie files.

After changing a cfg or a patch, regenerate first; a plain build does not:

```sh
python3 scripts/generate_snesrecomp.py && cmake --build build -j
```

## Input tapes

A tape is one little-endian u16 per frame in the `$4218/$4219` joypad layout (bit 15 B ... bit 4 R).
A `*.polled.tape` / `*.read.tape` has one word per game pad read instead (`--input-polled`), which keeps a replay
aligned when the two sides spend a different number of frames loading.

| Set | Made with | What it covers |
|---|---|---|
| 71 play tapes (`build/playtapes/`) | `tools/make_play_tapes.py build/playtapes` (+ `--task-t`, `--task-z`) | every level 0-13 run right / random / idle, menus, options, the three difficulties, pause, continue yes/no, game over, attract demo, 17 death-and-respawn tapes |
| Speedruns | `tools/bk2_to_tape.py` on TASVideos movies #6987, #6781, #4735 | whole game, glitch routes |
| All Bugs | same, on ShgofcTAS's All Bugs run | whole game incl. bonus stages, 60,648 frames |

The level tapes start from the debug-menu level select (`tools/re/level_select.md`).

## Gates

| Gate | Command | Pass |
|---|---|---|
| Standard set | `scripts/compare_modes.sh 600 3000` and `-t build/tapes/6987M.tape 1200 6000` | `MATCH` on every frame hash, WRAM `same` |
| All play tapes | `scripts/check_all_tapes.sh -s` | `71 MATCH, 0 MISMATCH` at frame 5000 |
| Deaths | `check_all_tapes.sh` on the death tapes, every 10th frame | all match |
| Idle | `scripts/check_idle_lle.sh` | `IDLE MATCH frames=12000 every=5` |
| Native decompressor | `scripts/check_native_decomp.sh 3000` | `IDENTICAL` |
| Save / load | `tlk_headless --frames 3000 --save-at 3000 s.state`, then `--load s.state --frames 6000` in a new process | same hash as an uninterrupted run |
| Unit tests | `(cd build && ctest --output-on-failure)` | `100% tests passed` |
| Save / resume | `scripts/check_state_resume.sh build/playtapes/l05_random.tape 9000 600=Title,2 1450=Hakuna,8 4000=Hakuna,6`: saves, resumes in new processes, the autosave and the build-id guard | `RESUME MATCH` |
| Android unit tests | `scripts/build_android.sh` runs them before it packages | `BUILD SUCCESSFUL` |
| Cross-device states | `scripts/check_cross_device_state.sh` with the same commit's APK on the phone: a PC state played on the phone and a phone state played here | `CROSS-DEVICE MATCH` |
| Save sync | `python3 -m pytest -q sync/tests`: the server, the stores, the desktop client, and the merge cases the Kotlin tests run too | no failures |
| Speedruns vs BizHawk | `tools/ram_sync_compare.py <headless ram log> <BizHawk ram log>` | in sync to the last poll (6987M 36,904, 6781M 37,093, 4735M 38,870) |
| All Bugs | `scripts/check_allbugs.sh` | `ALLBUGS PASS` |
| Remap / menu | `SDL_VIDEO_DRIVER=dummy build/tlk_sdl --rom <rom> --no-audio --selftest-remap` (under Xvfb the virtual pad's events don't arrive) | `PASS (0 failed)` |
| Exit widths | `scripts/check_exit_mx.py` | 0 `WRONG` |
| Windows build | `build-win/tlk_headless.exe` (README, MinGW cross build) under Wine, same arguments as `build/tlk_headless`: standard set default and `--lle`, play tapes, save/load, All Bugs `--ram-log` | same frame/WRAM hashes as Linux; RAM log byte-identical (`cmp`) |
| Window | run `tlk_sdl` and look | the right picture (one change once passed every hash with a black window) |

Expected hashes on the current tree: frame 600 `57ccb8d5b24918fd`, 3000 `89a48d6c4f12621b`, 6987M 1200
`dd861e164c295e15`, 6000 `946033bd10683657`.

## Reference captures from BizHawk

`tools/bizhawk/ram_log.lua` logs, per frame, lag flag, RNG, health, position, level, lives and the pad value the
game read, on BizHawk 2.11 with the BSNESv115+ core. `tools/bk2_to_tape.py --polled --read-values --lag <RAM log>`
turns a movie plus that log into a per-poll tape. `tlk_headless --input-polled <tape> --ram-log <out>` writes the
same format, and `ram_sync_compare.py` reports the first poll where they part (`--resync K` tolerates a short drift
window). The exact BizHawk command lines are in `tools/README.md`.

Movies made on Snes9x or old bsnes cores do not sync with either side and are not used as references.

## When something differs

1. Find the first differing frame (`scripts/first_divergence.py`).
2. Find the first differing hardware access, stamped with the master clock (`TLK_HWLOG`,
   `tools/aot_pace/pace_diff.sh`).
3. If a change introduced it, bisect against the tree before the change (`scripts/bisect_interp.py` narrows which
   compiled body is wrong).
4. Fix it generally (code generator patch, runtime patch, or a proven cfg line), never as a special case, and run
   the gates again.

## Coverage

`TLK_ROMMAP=<file> tlk_headless --lle ...` marks every ROM byte executed. `tools/rom_accounting.py`,
`tools/function_count.py` and `tools/never_ran.py` read those maps: share of the ROM that ran, how many routines
exist, and which known routines never ran on any tape (and which tapes reach their callers).
