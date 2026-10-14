#!/usr/bin/env python3
"""Build coverage play tapes (u16 LE per frame, $4218/$4219 layout) for tlk_headless.

Per level N (0..13): debug-menu level-select prefix (tools/re/make_level_tape.py preset),
150 idle frames for the level to load, then PLAY frames of one of:
  right   hold Right, jump (B) every 40 frames, roar (Y) every 97, slash (A) every 151
  random  seeded random pad (weighted to Right), held for 4..30 frames per pick
  idle    no input: Simba dies repeatedly -> death, game over, continue screens
Plus three menu tapes:
  attract  6000 idle frames (title, attract demo loop)
  options  title -> Options, cursor wander and toggles, back out, start a game
  gameover level 0 idle for 9000 frames (all lives lost, continue / game over)

usage: make_play_tapes.py OUTDIR [--play 5000] [--seed 1] [--levels 0-13]
"""
import argparse
import random
import struct
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "re"))
import make_level_tape as mlt  # noqa: E402

W = mlt.word
LOAD_WAIT = 150


def play_right(n):
    out = []
    for f in range(n):
        b = ["Right"]
        if f % 40 < 8:
            b.append("B")
        if f % 97 < 3:
            b.append("Y")
        if f % 151 < 2:
            b.append("A")
        out.append(W("+".join(b)))
    return out


def play_random(n, seed):
    rng = random.Random(seed)
    names = ["B", "Y", "A", "X", "L", "R", "Up", "Down", "Left", "Right"]
    out = []
    while len(out) < n:
        b = set()
        if rng.random() < 0.6:
            b.add("Right")
        elif rng.random() < 0.3:
            b.add("Left")
        for nm in names:
            if rng.random() < 0.12:
                b.add(nm)
        if "Left" in b and "Right" in b:
            b.discard("Left")
        if "Up" in b and "Down" in b:
            b.discard("Down")
        if rng.random() < 0.01:
            b.add("Start")  # occasional pause/unpause
        out += [W("+".join(sorted(b)))] * rng.randint(4, 30)
    return out[:n]


def level_prefix(level):
    return mlt.build(mlt.preset(level)) + [0] * LOAD_WAIT


def options_tape():
    steps = mlt.BOOT_TO_OPTIONS + []
    for b in ["Down", "Down", "Right", "Left", "Down", "Right", "Right", "Up", "A", "B",
              "Down", "Left", "Up", "Up", "Right", "Down", "Down", "Down", "Right", "Left"]:
        steps += [b, "wait 25"]
    steps += ["Start", "wait 100", "Up", "wait 20", "Start", "wait 300"]
    return mlt.build(steps)


# ---- task T: content the speedruns skip -------------------------------------------------
# Options screen (seen): A cycles the item under the cursor (Left/Right do nothing), Up/Down move
# it; items DIFFICULTY, MUSIC, SOUND FX, SOUND TEST, TRIGGERS. A on SOUND TEST opens the track
# list (Down scrolls, A plays), Start leaves it. Presses closer than ~25 frames are dropped.
OPT = mlt.BOOT_TO_OPTIONS
GAP = "wait 30"


def t_options_all():
    s = list(OPT)
    s += ["A", GAP] * 3                       # difficulty: all three values, back to NORMAL
    s += ["Down", GAP] + ["A", GAP] * 3       # music: mono / off / stereo
    s += ["Down", GAP] + ["A", GAP] * 2       # sound fx: off / on
    s += ["Down", GAP, "A", "wait 60"]        # sound test
    for _ in range(14):
        s += ["A", "wait 90", "Down", GAP]   # play every track in the list
    s += ["Start", "wait 150"] + ["Down", GAP] * 4  # back to options (cursor on DIFFICULTY), to TRIGGERS
    s += ["A", GAP] * 6                       # all six button layouts
    s += ["Start", "wait 60", "Up", "wait 20", "Start", "wait 600"]
    return mlt.build(s)


def t_difficulty(k, play=3000):
    """k A presses on DIFFICULTY (0 normal, 1 difficult, 2 easy), then a normal new game."""
    s = list(OPT) + ["A", GAP] * k + ["Start", "wait 60", "Up", "wait 20", "Start"]
    return mlt.build(s) + [0] * 400 + play_right(play)


def t_pause(play=4000):
    out = level_prefix(0)
    body = play_right(play)
    for f in range(200, play - 200, 400):     # pause, 120 frames paused (+ a held press), unpause
        body[f:f + 2] = [W("Start")] * 2
        body[f + 2:f + 60] = [0] * 58
        body[f + 60:f + 64] = [W("Right")] * 4  # input while paused
        body[f + 120:f + 122] = [W("Start")] * 2
    return out + body


def t_gameover(choice):
    """Stampede idle until the last life is gone (continue screen at ~f13700), then YES or NO."""
    out = list(mlt.build(mlt.preset(3))) + [0] * LOAD_WAIT
    out += [0] * (14000 - len(out))
    if choice == "yes":
        out += mlt.build(["A", "wait 600"]) + play_right(2500)
    else:
        out += mlt.build(["Right", GAP, "A", "wait 3000"])
    return out


def task_t(out):
    write(out / "t_options_all.tape", t_options_all())
    for k, n in enumerate(["normal", "difficult", "easy"]):
        write(out / f"t_diff_{n}.tape", t_difficulty(k))
    write(out / "t_pause.tape", t_pause())
    write(out / "t_continue_yes.tape", t_gameover("yes"))
    write(out / "t_continue_no.tape", t_gameover("no"))
    write(out / "t_attract.tape", [0] * 12000)
    # Ending + credits: the 6987M TAS as a per-poll tape (bk2_to_tape.py --polled --read-values). A per-frame
    # tape cannot carry it: --input drops Left+Right, which the TAS presses (716 polls). Run 49000 frames.
    src = HERE.parent / "build-host/out/sync/ram/tapes/6987M.read.tape"
    if src.exists():
        write(out / "t_ending.polled.tape", list(struct.unpack(f"<{src.stat().st_size // 2}H", src.read_bytes())))


# ---- task Z: deaths and respawns in every level -----------------------------------------
# Each d_LL_* tape starts level LL from the debug menu, then plays a pattern picked (by --ram-log, lives column)
# because it loses lives in that level. Bug levels 10-13 have no lives: the bonus ends, the game drops to
# Pride Lands (level 0) and Simba dies there. Deaths per tape are listed in docs/devlog.md (task Z).
#   idle   stand still: enemies / timed hazards kill Simba
#   run    hold Right, short hop every 25 frames
#   jump   hold Right, B held 30 of every 60 frames (long jumps into pits / water)
#   rN     play_random(seed N), weighted to Right;  lN  the same mirrored (weighted to Left)
def play_jump(n):
    return [W("Right+B") if f % 60 < 30 else W("Right") for f in range(n)]


def play_run(n):
    return [W("Right+B") if f % 25 < 3 else W("Right") for f in range(n)]


def mirror(words):
    r, l = W("Right"), W("Left")
    return [(w & ~(r | l)) | (l if w & r else 0) | (r if w & l else 0) for w in words]


Z_TAPES = [  # (level, kind, arg, play frames)
    (0, "r", 7007, 6000), (1, "jump", 0, 6000), (1, "run", 0, 6000), (2, "idle", 0, 6000),
    (2, "r", 7106, 6000), (3, "idle", 0, 6000), (3, "jump", 0, 6000), (4, "jump", 0, 6000),
    (5, "r", 7252, 6000), (6, "l", 9301, 8000), (7, "jump", 0, 6000), (8, "r", 7424, 8000),
    (9, "l", 9454, 8000), (10, "r", 7504, 6000), (11, "r", 7556, 6000), (12, "r", 7607, 6000),
    (13, "r", 7658, 6000),
]


def task_z(out, levels):
    for lv, kind, arg, n in Z_TAPES:
        if lv not in levels:
            continue
        body = {"idle": lambda: [0] * n, "run": lambda: play_run(n), "jump": lambda: play_jump(n),
                "r": lambda: play_random(n, arg), "l": lambda: mirror(play_random(n, arg))}[kind]()
        name = kind + (str(arg) if arg else "")
        write(out / f"d_{lv:02d}_{name}.tape", level_prefix(lv) + body)


def write(path, words):
    path.write_bytes(b"".join(struct.pack("<H", w) for w in words))
    print(f"{path}: {len(words)} frames")


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("outdir")
    ap.add_argument("--play", type=int, default=5000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--levels", default="0-13")
    ap.add_argument("--task-t", action="store_true", help="only the task T tapes")
    ap.add_argument("--task-z", action="store_true", help="only the task Z death/respawn tapes")
    a = ap.parse_args(argv)
    out = Path(a.outdir)
    out.mkdir(parents=True, exist_ok=True)
    if a.task_t:
        out.mkdir(parents=True, exist_ok=True)
        task_t(out)
        return 0
    lo, _, hi = a.levels.partition("-")
    if a.task_z:
        task_z(out, range(int(lo), int(hi or lo) + 1))
        return 0
    for lv in range(int(lo), int(hi or lo) + 1):
        pre = level_prefix(lv)
        write(out / f"l{lv:02d}_right.tape", pre + play_right(a.play))
        write(out / f"l{lv:02d}_random.tape", pre + play_random(a.play, a.seed * 100 + lv))
        write(out / f"l{lv:02d}_idle.tape", pre + [0] * a.play)
    write(out / "attract.tape", [0] * 6000)
    write(out / "options.tape", options_tape())
    write(out / "gameover.tape", level_prefix(0) + [0] * 9000)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
