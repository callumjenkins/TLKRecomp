#!/usr/bin/env python3
"""Count objects that appear or vanish inside the widescreen margins.

usage: wide_popin.py OBJLOG EXTRA

OBJLOG comes from TLK_WIDE_OBJLOG=<path> tlk_headless --wide W ...; EXTRA is the
margin, (W - 256) / 2. An object whose first (or last) box is inside the view
without touching its outer edge popped in (or out) where the player can see it:
"margin" when the box is in the margins only, "edge" when it also covers native
columns (an enemy appearing at the old screen edge), "native" when it sits fully
inside the native 256 (the game's own business: projectiles, effects).
"""
import sys
from collections import Counter, defaultdict


def main():
    path, extra = sys.argv[1], int(sys.argv[2])
    frames = defaultdict(dict)
    for line in open(path):
        f, obj, x0, x1, y0, y1, fixed, handler = line.split()
        if fixed == "1":
            continue
        frames[int(f)][obj] = (int(x0), int(x1), int(y0), int(y1), handler)

    def where(box):
        x0, x1, y0, y1, _ = box
        if y1 <= 0 or y0 >= 224:
            return "off"
        if x1 <= -extra or x0 >= 256 + extra:
            return "off"
        if x0 <= -extra or x1 >= 256 + extra:
            return "outer"   # crossing the view's edge: entering or leaving normally
        if x0 >= 0 and x1 <= 256:
            return "native"
        if x1 > 0 and x0 < 256:
            return "edge"
        return "margin"

    counts, seen = Counter(), []
    order = sorted(frames)
    for a, b in zip(order, order[1:]):
        if b - a > 3:   # a load or a pause: not a pop
            continue
        for obj, box in frames[b].items():
            old = frames[a].get(obj)
            if not old:
                counts["appear_" + where(box)] += 1
                if where(box) in ("margin", "edge"):
                    seen.append((b, "in", obj, box[:4]))
        for obj, box in frames[a].items():
            new = frames[b].get(obj)
            if not new:
                counts["vanish_" + where(box)] += 1
                if where(box) in ("margin", "edge"):
                    seen.append((a, "out", obj, box[:4]))
    print(" ".join(f"{k}={counts[k]}" for k in sorted(counts)))
    for s in seen[:20]:
        print(*s)


if __name__ == "__main__":
    main()
