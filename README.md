# TLKRecomp

A native PC port of *The Lion King* (SNES, USA, 1994), made by static recompilation with
[snesrecomp](https://github.com/RetroPortingToolKit/snesrecomp). The game's 65816 program is translated to C and
compiled; graphics, sound and timing still run on snesrecomp's emulated PPU/SPC, so it plays exactly like the
cartridge.

**No game data is in this repository.** No ROM, no assets, no generated code. You need your own dump
(`Lion King, The (USA).sfc`, 3 MiB headerless, SHA-256 `457abe63...0f3712`); the build reads it to generate the
C sources locally.

## Status

- Playable start to finish, keyboard and gamepad, save states, remappable controls.
- 1,305 / 1,305 known routines compiled; about 4% of non-idle CPU time on the test tapes still runs in the interpreter (code entered in a CPU
  mode the compiler can't handle yet, see `docs/HOW_IT_WORKS.md`).
- Checked frame by frame against the interpreter on 71 hand-made input tapes, an idle run, save/load, and
  RAM-checked against BizHawk on three published speedruns and a 60,648-frame All Bugs run.

## How to play

Build (Linux: cmake, ninja, gcc/clang, Python 3, SDL3):

    ./build.sh

Windows x86_64, cross-built on Linux with MinGW-w64 (`x86_64-w64-mingw32-gcc`); SDL3 (pinned, fetched) and the MinGW
runtime are linked statically, so the `.exe` files need only system DLLs:

    python3 scripts/generate_snesrecomp.py   # if generated/ is empty
    cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DTLK_STATIC_SDL=ON
    cmake --build build-win --target tlk_headless tlk_sdl

`build-win/tlk_headless.exe` and `tlk_sdl.exe` run under Wine with the same arguments as the Linux builds
(`docs/VERIFYING.md`).

Run:

    build/tlk_sdl --rom "/path/to/Lion King, The (USA).sfc"

Options: `--scale N` (window size, default 3), `--fullscreen`, `--no-audio`, `--config file.ini`.

Default keys:

| SNES | Key |
|---|---|
| D-pad | arrow keys |
| B / A | Z / X |
| Y / X | A / S |
| L / R | Q / W |
| Start / Select | Enter / Right Shift |

F11 or Alt+Enter toggles fullscreen.

| Key | Does |
|---|---|
| F1-F4 | save state to slot 1-4 |
| F5-F8 (or Shift+F1-F4) | load slot 1-4 |
| Tab or Esc | settings menu (game pauses); in the menu Esc quits, Tab resumes |

Save states are `tlk_slot1.state` .. `tlk_slot4.state` in the same folder as the ini. They only load in the build that
wrote them. The settings menu (arrows + Enter, Left/Right change a value) has window scale, fullscreen, vsync, audio
on/off, volume and controls ("Controls...": pick a SNES button, press Enter or pad A, then press the new key *or*
gamepad button; Esc or Guide cancels; "Reset defaults" restores the table above). "Save settings" writes `tlk_sdl.ini`
(it rewrites the file, so comments are dropped). An SDL gamepad works out of the box and can be plugged in while the
game runs (buttons by position: bottom = B, right = A, left = Y, top = X; shoulders and triggers = L/R; left stick or
d-pad moves). Guide, or Back+Start, opens the menu from the pad; in it the d-pad moves, A selects, B goes back, Start
closes.

Without `--rom` (and no `rom =` in the ini), `tlk_sdl` checks the `.sfc`/`.smc` files next to the binary and in the
current directory and uses the first one that is the right ROM. If none is found, it opens a file dialog to pick
the ROM, checks it, and saves the path as `rom =` in `tlk_sdl.ini` so it is not asked again.

Settings go in `tlk_sdl.ini` in the working directory (or the file in `$TLK_SDL_INI`):

    rom = /path/to/Lion King, The (USA).sfc
    scale = 4
    fullscreen = 0
    vsync = 0          ; 0 = exact 60.0988 Hz timer, 1 = follow the monitor
    audio = 1
    audio_rate = 48000
    volume = 100       ; percent
    ; keys use SDL key names
    b = Z
    a = X
    start = Return
    select = Right Shift
    ; gamepad buttons use SDL names (a = bottom, b = right, x = left, y = top)
    pad_b = a
    pad_y = x
    pad_start = start

Packaging: `scripts/package.sh` builds `dist/TLKRecomp-linux-x86_64.tar.gz` (binary + README + license notes, no ROM
and no game data). `scripts/package_win.sh` builds `dist/TLKRecomp-windows-x86_64.zip` the same way from the MinGW
cross build (`build-win/`). `cmake --install build --prefix DIR` installs `bin/tlk_sdl` and the same notes.

## Watching a speedrun

`tapes/` holds four tool-assisted speedruns (TAS) as input tapes, replayed in sync to the end:

    build/tlk_sdl --rom "/path/to/Lion King, The (USA).sfc" --input-polled tapes/allbugsF.read.tape

| Tape | Run |
|---|---|
| `6987M.read.tape` | [TASVideos #6987](https://tasvideos.org/6987M), any% in 10:50.396 by EZGames69, SBDWolf, Akiteru & ShgofcTAS |
| `6781M.read.tape` | [TASVideos #6781](https://tasvideos.org/6781M), any% in 10:53.424 by EZGames69, SBDWolf & Akiteru |
| `4735M.read.tape` | [TASVideos #4735](https://tasvideos.org/4735M), any% in 11:22.99 by EZGames69 & SBDWolf |
| `allbugsF.read.tape` | [Lion King All Bugs Final WIP](https://tasvideos.org/UserFiles/Info/639031773616833383) by ShgofcTAS, whole game with bonus stages |

Use `--input-polled`, not `--input`: these tapes hold one pad value per game pad read, not one per frame, and
played with `--input` they desync at once. `--no-pace` fast-forwards. They were converted from the authors' BizHawk movies
(`tools/bk2_to_tape.py`, `docs/VERIFYING.md`): the button presses, re-timed to the game's pad reads. The movies
are published on TASVideos under [CC BY 2.0](https://creativecommons.org/licenses/by/2.0/), and so are these
tapes; credit the authors above when you reuse them.

## Repository layout

| Path | What |
|---|---|
| `recomp/` | per-bank function lists and hints that drive the recompiler |
| `runner/` | game glue, SDL desktop host (`tlk_sdl`), headless test host (`tlk_headless`) |
| `patches/snesrecomp/` | code generator patches, applied to a copy of the submodule at generate time |
| `cmake/runtime-patches/` | runtime patches, applied the same way |
| `scripts/` | generate, build, package, and the verification gates |
| `tapes/` | speedrun input tapes (see above) |
| `tools/` | tape conversion, BizHawk Lua loggers, coverage and accounting |
| `docs/` | `HOW_IT_WORKS.md`, `VERIFYING.md`, `devlog.md` (engineering diary), function count |

The `snesrecomp` submodule is never edited in place.

## Verifying

`build/tlk_headless` is the windowless test host; `tlk_sdl` runs the same emulation path. With tapes in place
(`tools/make_play_tapes.py`, `tools/bk2_to_tape.py`), `scripts/compare_modes.sh`, `scripts/check_all_tapes.sh` and
`scripts/check_allbugs.sh` compare compiled code against the interpreter (`--lle`). Full list: `docs/VERIFYING.md`.

## Credits

- [snesrecomp](https://github.com/RetroPortingToolKit/snesrecomp) (RetroPortingToolKit); project layout follows
  [DKC3Recomp](https://github.com/elliotttate/DKC3Recomp).
- Reference runs: TASVideos movies #6987 (EZGames69, SBDWolf, Akiteru, ShgofcTAS), #6781 (EZGames69, SBDWolf, Akiteru),
  #4735 (EZGames69, SBDWolf), #2716 (Tompa), and ShgofcTAS's All Bugs run.
- Prototype and unused-content research: The Cutting Room Floor, Hidden Palace.

## Legal

The Lion King is (c) Disney / Virgin Interactive. This project is not affiliated with or endorsed by them.
This repository's own code is MIT (`LICENSE`); the speedrun tapes in `tapes/` are CC BY 2.0 (see above). snesrecomp is under the PolyForm Noncommercial License 1.0.0, so builds of this project may not be sold or used
commercially. Release binaries contain the game's program code translated to native code and do nothing without
your own ROM.
