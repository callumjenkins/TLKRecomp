#!/usr/bin/env python3
"""Find compiled entries that make default mode diverge from --lle.

usage: bisect_interp.py TAPE FRAME LOG.json [--bin build/tlk_headless]
LOG.json is a default-mode --log of the same run (its compiled_entry_pcs are the
candidates). Uses --interp-targets to push subsets back to the interpreter, and
splits until each culprit is a single PC24 (finds all culprits, one at a time).
"""
import argparse, json, os, subprocess, sys, tempfile
ap = argparse.ArgumentParser()
ap.add_argument("tape"); ap.add_argument("frame", type=int); ap.add_argument("log")
ap.add_argument("--bin", default="build/tlk_headless")
ap.add_argument("--rom", default="../Lion King, The (USA).sfc")
a = ap.parse_args()
cands = json.load(open(a.log))["compiled_entry_pcs"]
tmp = tempfile.mkdtemp()

def sha(extra):
    cmd = [a.bin, "--rom", a.rom, "--frames", str(a.frame), "--input", a.tape] + extra
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    return [l for l in out.splitlines() if l.startswith("frame_sha256=")][0]

def ok(excluded):
    p = os.path.join(tmp, "t.txt")
    open(p, "w").write("\n".join("0x" + c for c in excluded) + "\n")
    return sha(["--interp-targets", p]) == want

want = sha(["--lle"])
print("candidates", len(cands), flush=True)
if not ok(cands):
    sys.exit("still mismatches with every compiled entry interpreted")
culprits = []
while not ok(culprits):
    rest = [c for c in cands if c not in culprits]
    # smallest prefix of rest that (with culprits) fixes it, by binary search
    lo, hi = 0, len(rest)
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if ok(culprits + rest[:mid]): hi = mid
        else: lo = mid
    culprits.append(rest[hi - 1]); print("culprit", rest[hi - 1], flush=True)
print("culprits:", " ".join(culprits))
