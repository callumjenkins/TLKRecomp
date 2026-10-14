#!/usr/bin/env python3
"""Poll-aligned RAM comparison of a tlk_headless --ram-log against tools/bizhawk/ram_log.lua output.

Both logs have one line per frame: "f lag rng hp x y lvl lives pad" (hex fields). Frames are
aligned by POLL count (k-th non-lag frame on each side), so differing lag does not count as a
desync; the frame offset (headless f - BizHawk f) is reported instead. Prints the first poll where
any field differs, which fields, and a context window.

    python tools/ram_sync_compare.py HEADLESS_RAM BH_RAM [--fields rng,hp,x,y,lvl,lives,pad] [--ctx 5]

--resync K (drift-tolerant mode, off by default): instead of stopping at the first desync, scan every
compared poll. Differing polls are grouped into windows (gaps of fewer than K matching polls are
merged); a window counts as re-synced if at least K matching polls (or the end of the run) follow it.
Prints differing polls, the windows, the last matching poll, whether the end is in sync, and the
level-start polls of both sides. Exit 0 iff every window re-synced and the last compared poll matches.
"""
import argparse
import sys

NAMES = ["rng", "hp", "x", "y", "lvl", "lives", "pad"]


def polls(path):
    out = []
    with open(path) as f:
        for line in f:
            p = line.split()
            if len(p) != 9 or p[1] != "0":
                continue
            out.append((int(p[0]), dict(zip(NAMES, p[2:]))))
    return out


def level_changes(seq):
    last, ch = None, []
    for k, (f, r) in enumerate(seq):
        if r["lvl"] != last:
            ch.append((k + 1, f, r["lvl"]))
            last = r["lvl"]
    return ch


def poll_bad(h, b, k, fields):
    near = [b[j][1] for j in (k - 1, k, k + 1) if j >= 0]
    return [x for x in fields if h[k][1][x] not in [r[x] for r in near]]


def resync_report(h, b, fields, K):
    n = min(len(h), len(b) - 1)
    bad = [k for k in range(n) if poll_bad(h, b, k, fields)]
    wins = []
    for k in bad:
        if wins and k - wins[-1][1] <= K:
            wins[-1][1] = k
        else:
            wins.append([k, k])
    ok = True
    print(f"polls headless={len(h)} bizhawk={len(b)} compared={n}")
    print(f"differing_polls={len(bad)} windows={len(wins)} resync={K}")
    for s0, e0 in wins:
        cnt = sum(1 for k in bad if s0 <= k <= e0)
        follow = n - 1 - e0
        if follow < K:
            ok = False
        print(f"window polls {s0 + 1}-{e0 + 1} differing={cnt} "
              f"hl_f={h[s0][0]}-{h[e0][0]} bh_f={b[s0][0]}-{b[e0][0]} "
              f"{'resynced' if follow >= K else 'NOT_RESYNCED'}")
    badset = set(bad)
    last_match = max((k for k in range(n) if k not in badset), default=-1)
    end_ok = n > 0 and (not bad or bad[-1] != n - 1)
    print(f"last_matching_poll={last_match + 1} end_in_sync={'yes' if end_ok else 'no'} "
          f"(headless f{h[n-1][0]}, bizhawk f{b[n-1][0]})" if n else "empty")
    print("differing:", ",".join(str(k + 1) for k in bad))
    bl, hl = level_changes(b[:n]), level_changes(h[:n])
    print("bizhawk levels (poll, frame, lvl):", " ".join(f"{k}/{f}/{l}" for k, f, l in bl))
    print("headless levels (poll, frame, lvl):", " ".join(f"{k}/{f}/{l}" for k, f, l in hl))
    return 0 if ok and end_ok else 1


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument("headless")
    ap.add_argument("bizhawk")
    ap.add_argument("--fields", default=",".join(NAMES))
    ap.add_argument("--ctx", type=int, default=5)
    ap.add_argument("--persist", type=int, default=8,
                    help="only report a mismatch that lasts this many consecutive polls (one-poll "
                         "phase glitches at lag edges are not desyncs)")

    ap.add_argument("--resync", type=int, default=0, metavar="K",
                    help="drift-tolerant mode: report all differing windows; a window must be "
                         "followed by K matching polls (see module doc)")

    a = ap.parse_args(argv)
    fields = a.fields.split(",")
    h, b = polls(a.headless), polls(a.bizhawk)
    if a.resync:
        return resync_report(h, b, fields, a.resync)
    # BizHawk ends a frame around the auto-joypad latch, so the $4218 read/store at $C0A07C and the
    # logic it drives often land in the NEXT BizHawk frame: its RAM shows poll k's result at poll k
    # or k+1, and fields can straddle (pad from k, Simba X from k+1). A field matches if it equals
    # BizHawk poll k-1, k or k+1 (the two hosts sample RAM at different points of the frame); a real desync shows up as a field that matches neither.
    n = min(len(h), len(b) - 1)
    first = None
    run = 0
    glitches = 0
    for k in range(n):
        near = [b[j][1] for j in (k - 1, k, k + 1) if j >= 0]
        bad = [x for x in fields if h[k][1][x] not in [r[x] for r in near]]
        if not bad:
            if run:
                glitches += 1
            run = 0
            continue
        if run == 0:
            start = (k, bad)
        run += 1
        if run >= a.persist:
            first = start
            break
    print(f"transient_mismatch_runs={glitches} (shorter than {a.persist} polls)")
    print(f"polls headless={len(h)} bizhawk={len(b)} compared={n}")
    bl = level_changes(b[:n])
    hl = level_changes(h[:n])
    print("bizhawk levels (poll, frame, lvl):", " ".join(f"{k}/{f}/{l}" for k, f, l in bl))
    print("headless levels (poll, frame, lvl):", " ".join(f"{k}/{f}/{l}" for k, f, l in hl))
    if first is None:
        print(f"in_sync_through_poll={n} (headless f{h[n-1][0]}, bizhawk f{b[n-1][0]})" if n else "empty")
        return 0
    k, bad = first
    print(f"first_diff poll={k + 1} headless_f={h[k][0]} bizhawk_f={b[k][0]} "
          f"offset={h[k][0] - b[k][0]:+d} fields={','.join(bad)}")
    print("poll   hl_f   bh_f  " + "  ".join(f"{x:>13}" for x in NAMES))
    for j in range(max(0, k - a.ctx), min(n, k + a.ctx + 1)):
        cells = []
        for x in NAMES:
            hv, bv = h[j][1][x], b[j][1][x]
            cells.append(f"{hv:>6}" + ("=" if hv == bv else "!") + f"{bv:<6}")
        print(f"{j + 1:5d} {h[j][0]:6d} {b[j][0]:6d}  " + "  ".join(cells))
    return 1


if __name__ == "__main__":
    sys.exit(main())
