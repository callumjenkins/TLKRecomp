#!/usr/bin/env python3
"""Print every poll where headless and BizHawk disagree on lag frames since the previous poll.

usage: compare_lags.py <headless ram log> <bizhawk ram log>  (tlk_headless --ram-log, bizhawk/ram_log.lua)
"""

def load_log(path):
    frames = []
    with open(path) as f:
        for line in f:
            p = line.split()
            if len(p) >= 2:
                frames.append((int(p[0]), int(p[1])))
    return frames

import sys

if len(sys.argv) != 3:
    sys.exit(__doc__)
hl_path, bh_path = sys.argv[1], sys.argv[2]

hl = load_log(hl_path)
bh = load_log(bh_path)

def poll_and_lag_timeline(frames):
    poll_count = 0
    lags = []
    timeline = []
    for f, is_lag in frames:
        if not is_lag:
            poll_count += 1
            timeline.append((poll_count, f, len(lags)))
        else:
            lags.append(f)
    return timeline, lags

hl_tl, hl_lags = poll_and_lag_timeline(hl)
bh_tl, bh_lags = poll_and_lag_timeline(bh)

print(f"Total polls: HL={len(hl_tl)}, BH={len(bh_tl)}")
min_polls = min(len(hl_tl), len(bh_tl))
for i in range(1, min_polls):
    p_hl, f_hl, lags_hl = hl_tl[i]
    p_bh, f_bh, lags_bh = bh_tl[i]
    delta_hl = f_hl - hl_tl[i-1][1]
    delta_bh = f_bh - bh_tl[i-1][1]
    if delta_hl != delta_bh:
        print(f"Divergence at Poll {p_hl}: HL f={f_hl} (d={delta_hl}, lags={lags_hl}) | BH f={f_bh} (d={delta_bh}, lags={lags_bh}) | diff(lags)={lags_hl - lags_bh}")


