#!/usr/bin/env python3
"""Align a headless opcode trace (TLK_TRACE_OPCODES) with a BizHawk one (bizhawk/trace_opcodes.lua).

Each line is "PC24 master_cycles" logged before the opcode runs. Walks both while the PC streams
agree and charges each opcode the clocks until the next logged opcode. Prints where the streams part,
the accumulated drift (headless - bizhawk clocks) and the per-PC cost differences.
"""
import argparse
from collections import Counter, defaultdict


def load(path):
    out = []
    with open(path) as f:
        for line in f:
            p = line.split()
            if len(p) == 2:
                out.append((int(p[0], 16), int(p[1])))
    return out


def resync(h, i, b, j, window, run=12):
    """Smallest k+l such that h[i+k:] and b[j+l:] agree for `run` PCs."""
    idx = defaultdict(list)
    for l in range(min(window, len(b) - j - run)):
        idx[b[j+l][0]].append(l)
    best = None
    for k in range(min(window, len(h) - i - run)):
        if best is not None and k >= best[0] + best[1]:
            break
        for l in idx.get(h[i+k][0], ()):
            if best is not None and k + l >= best[0] + best[1]:
                break
            if all(h[i+k+t][0] == b[j+l+t][0] for t in range(run)):
                best = (k, l)
                break
    return best if best else (None, None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("headless")
    ap.add_argument("bizhawk")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--window", type=int, default=20000, help="max opcodes to skip per side to rejoin")
    a = ap.parse_args()
    h, b = load(a.headless), load(a.bizhawk)
    # BizHawk logs the window from the opcode after the poll; drop leading headless lines until PCs meet.
    i = 0
    while i < len(h) and h[i][0] != b[0][0]:
        i += 1
    j = 0
    drift = 0
    per_pc = defaultdict(lambda: [0, 0, 0])  # count, hl clocks, bh clocks
    diffs = Counter()
    n = 0
    while i + 1 < len(h) and j + 1 < len(b):
        if h[i][0] != b[j][0]:
            k, l = resync(h, i, b, j, a.window)
            if k is None:
                print(f"streams part for good after {n} opcodes: hl[{i}]={h[i][0]:06X} bh[{j}]={b[j][0]:06X}")
                print("  hl:", " ".join(f"{x[0]:06X}" for x in h[max(0, i-6):i+6]))
                print("  bh:", " ".join(f"{x[0]:06X}" for x in b[max(0, j-6):j+6]))
                break
            th = h[i+k][1] - h[i-1][1]
            tb = b[j+l][1] - b[j-1][1]
            # the opcode before the split already got charged its normal delta; charge the rest
            dprev = (h[i][1] - h[i-1][1]) - (b[j][1] - b[j-1][1])
            gap = (th - tb) - dprev
            print(f"split at pc {h[i-1][0]:06X} (opcode {n}): hl +{k} ops, bh +{l} ops, "
                  f"hl-bh {gap:+d} clocks, rejoin at {h[i+k][0]:06X}; drift before {drift:+d}")
            drift += gap
            i += k; j += l
            continue
        dh = h[i+1][1] - h[i][1]
        db = b[j+1][1] - b[j][1]
        pc = h[i][0]
        e = per_pc[pc]
        e[0] += 1; e[1] += dh; e[2] += db
        if dh != db:
            diffs[(pc, dh, db)] += 1
        drift += dh - db
        n += 1
        i += 1; j += 1
    print(f"compared {n} opcodes, drift (hl - bh) = {drift} master clocks")
    print("hl elapsed", h[i][1] - h[0][1], "bh elapsed", b[j][1] - b[0][1])
    print("top cost differences (pc, hl clocks, bh clocks) x count, total:")
    for (pc, dh, db), c in sorted(diffs.items(), key=lambda kv: -abs((kv[0][1]-kv[0][2])*kv[1]))[:a.top]:
        print(f"  {pc:06X} hl={dh:5d} bh={db:5d} x{c:5d}  total {c*(dh-db):+d}")


if __name__ == "__main__":
    main()
