#!/usr/bin/env python3
"""First frame where default mode and --lle differ, per tape.

usage: first_divergence.py [--frames N] [--bin build/tlk_headless] [-j N] TAPE...
Runs both modes with TLK_FRAME_LOG and prints, per tape, the first frame whose
picture differs, the first frame whose $0A0A frame counter differs (a lag-frame
difference) and the first whole-WRAM difference.
"""
import argparse, os, subprocess, tempfile
from concurrent.futures import ThreadPoolExecutor
ap = argparse.ArgumentParser()
ap.add_argument("tapes", nargs="+"); ap.add_argument("--frames", type=int, default=5000)
ap.add_argument("--bin", default="build/tlk_headless"); ap.add_argument("-j", type=int, default=os.cpu_count())
ap.add_argument("--rom", default="../Lion King, The (USA).sfc"); ap.add_argument("--extra", default="")
a = ap.parse_args()
tmp = tempfile.mkdtemp()

def run(job):
    tape, mode = job
    log = os.path.join(tmp, os.path.basename(tape) + "." + mode)
    inp = "--input-polled" if tape.endswith(".polled.tape") else "--input"
    cmd = [a.bin, "--rom", a.rom, "--frames", str(a.frames), inp, tape]
    cmd += ["--lle"] if mode == "lle" else a.extra.split()
    subprocess.run(cmd, env=dict(os.environ, TLK_FRAME_LOG=log), capture_output=True)
    return [l.split() for l in open(log)]

with ThreadPoolExecutor(a.j) as ex:
    res = list(ex.map(run, [(t, m) for t in a.tapes for m in ("lle", "aot")]))
for k, t in enumerate(a.tapes):
    L, A = res[2 * k], res[2 * k + 1]
    first = lambda col: next((x[0] for x, y in zip(L, A) if x[col] != y[col]), "-")
    lag = next((f"{x[0]}({x[2]}/{y[2]})" for x, y in zip(L, A) if x[2] != y[2]), "-")
    print(f"{os.path.basename(t):14} wram {first(1):>6}  A0A {lag:>18}  vram {first(3):>6}")
