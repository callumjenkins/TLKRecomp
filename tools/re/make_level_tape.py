#!/usr/bin/env python3
"""Generate TLKRecomp input tapes (u16 LE per frame, $4218/$4219 layout).

A script is a list of steps; each step is "BUTTONS@frames" (hold) or
"wait N" (no buttons for N frames).  Buttons: B Y Sel Start Up Down Left Right A X L R,
joined with '+'.  Taps are written as "A" (= A@2 followed by a 4-frame release).

usage:
  make_level_tape.py out.tape "wait 700" "Down" "A" ...
  make_level_tape.py --level N out.tape     # preset recipe, see level_select.md
"""
import struct
import sys

BITS = {"B": 15, "Y": 14, "Sel": 13, "Select": 13, "Start": 12, "Up": 11, "Down": 10,
        "Left": 9, "Right": 8, "A": 7, "X": 6, "L": 5, "R": 4}


def word(buttons):
    w = 0
    for b in buttons.split("+"):
        if b:
            w |= 1 << BITS[b]
    return w


def build(steps, tap_hold=2, tap_gap=4):
    out = []
    for s in steps:
        s = s.strip()
        if s.startswith("wait"):
            out += [0] * int(s.split()[1])
        elif "@" in s:
            b, n = s.split("@")
            out += [word(b)] * int(n)
        else:
            out += [word(s)] * tap_hold + [0] * tap_gap
    return out


# Verified recipe (level_select.md): title -> Options -> B,A,R,R,Y opens the debug menu
# (INVUL / LEVEL).  Each A on LEVEL increments $7FFF9E (wraps at $0E).  Start leaves
# the menu, Up+Start on the title begins a game at $7FFF9E.  Presses are spaced
# ~20 frames: the options handler ignores input while its delay timer runs.
BOOT_TO_OPTIONS = ["wait 700", "Down", "wait 10", "Start", "wait 80"]
BARRY = ["B", "wait 20", "A", "wait 20", "R", "wait 20", "R", "wait 20", "Y", "wait 60"]
LEVELS = ["Pride Lands", "Can't Wait to be King", "Elephant Graveyard", "Stampede",
          "Simba's Exile", "Hakuna Matata", "Simba's Destiny", "Be Prepared",
          "Simba's Return", "Pride Rock", "Bug Toss", "Bug Hunt I", "Bug Hunt II", "Bug Hunt III"]


def preset(level, invul=0):
    if not 0 <= level < 14:
        raise SystemExit("level must be 0..13")
    steps = BOOT_TO_OPTIONS + BARRY
    if invul:                                  # INVUL: 0 off, 1 health, 2 lives; the cursor starts on LEVEL
        steps += ["Up", "wait 20"] + ["A", "wait 20"] * invul
    steps += ["Down", "wait 20"] + ["A", "wait 20"] * level
    steps += ["wait 20", "Start", "wait 60", "Up", "wait 20", "Start"]
    return steps


def main(argv):
    if len(argv) >= 3 and argv[0] == "--level":
        steps = preset(int(argv[1], 0))
        path = argv[2]
    elif len(argv) >= 1:
        path, steps = argv[0], argv[1:]
    else:
        print(__doc__)
        return 2
    words = build(steps)
    with open(path, "wb") as f:
        f.write(b"".join(struct.pack("<H", w) for w in words))
    print(f"{path}: {len(words)} frames")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
