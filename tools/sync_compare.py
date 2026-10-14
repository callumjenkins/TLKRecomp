#!/usr/bin/env python3
"""Compare tlk_headless --dump-every PNGs with BizHawk screenshots (tools/bizhawk/sync_shots.lua).

For each headless frame N it finds the BizHawk shot in N-win..N+win with the lowest mean absolute
pixel difference and prints the best offset and its score. A score near 0 means same picture.

    python tools/sync_compare.py HEADLESS_DIR STEM BH_DIR [--win 4] [--thresh 2.0]
"""
import argparse
import re
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageStat


def mad(a, b):
    d = ImageChops.difference(a, b)
    return sum(ImageStat.Stat(d).mean) / 3.0


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("headless_dir")
    ap.add_argument("stem")
    ap.add_argument("bh_dir")
    ap.add_argument("--win", type=int, default=4)
    ap.add_argument("--thresh", type=float, default=2.0)
    a = ap.parse_args(argv)
    hd, bd = Path(a.headless_dir), Path(a.bh_dir)
    rx = re.compile(re.escape(a.stem) + r"_(\d{6})\.png$")
    frames = sorted(int(m.group(1)) for p in hd.iterdir() if (m := rx.match(p.name)))
    first_bad = None
    for n in frames:
        h = Image.open(hd / f"{a.stem}_{n:06d}.png").convert("RGB")
        best = None
        for off in range(-a.win, a.win + 1):
            p = bd / f"bh_{n + off:06d}.png"
            if p.exists():
                s = mad(h, Image.open(p).convert("RGB"))
                if best is None or s < best[1]:
                    best = (off, s)
        if best is None:
            continue
        ok = best[1] <= a.thresh
        if not ok and first_bad is None:
            first_bad = n
        print(f"{n:6d} off={best[0]:+d} mad={best[1]:7.3f} {'ok' if ok else 'DIFF'}")
    print(f"first_diff={first_bad}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
