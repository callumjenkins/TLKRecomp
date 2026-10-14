#!/usr/bin/env python3
"""Interpreted-opcode share for a fixed benchmark, excluding the idle vblank loops.

usage: interp_share.py [--tape T] [--frames N] [--top K] [--bin B]
Runs default mode and --lle with TLK_INTERP_HIST and prints
  share = interp ops outside idle ranges (default) / same (--lle).
Idle ranges: $C21A47-$C21A57 and $C22352-$C22387 (wait-for-vblank spins).
--top lists the hottest interpreted PCs (default mode, idle excluded).
"""
import argparse, os, subprocess, tempfile
IDLE = [(0xC21A47, 0xC21A57), (0xC22352, 0xC22387)]
ap = argparse.ArgumentParser()
ap.add_argument("--tape", default="build/playtapes/l01_right.tape")
ap.add_argument("--frames", type=int, default=3000)
ap.add_argument("--top", type=int, default=0)
ap.add_argument("--bin", default="build/tlk_headless")
ap.add_argument("--rom", default="../Lion King, The (USA).sfc")
a = ap.parse_args()
tmp = tempfile.mkdtemp()

def hist(extra):
    p = os.path.join(tmp, "h.txt")
    env = dict(os.environ, TLK_INTERP_HIST=p)
    subprocess.run([a.bin, "--rom", a.rom, "--frames", str(a.frames), "--input-polled" if a.tape.endswith(".polled.tape") else "--input", a.tape] + extra,
                   env=env, capture_output=True, check=True)
    h = {}
    for l in open(p):
        pc, n = l.split(); h[int(pc, 16)] = int(n)
    return h

def busy(h): return {pc: n for pc, n in h.items() if not any(lo <= pc <= hi for lo, hi in IDLE)}
d, l = hist([]), hist(["--lle"])
bd, bl = busy(d), busy(l)
print(f"default interp total={sum(d.values())} non-idle={sum(bd.values())}")
print(f"lle     interp total={sum(l.values())} non-idle={sum(bl.values())}")
print(f"non-idle interpreted share={100.0 * sum(bd.values()) / sum(bl.values()):.1f}%")
for pc, n in sorted(bd.items(), key=lambda kv: -kv[1])[:a.top]:
    print(f"  {pc:06X} {n}")
