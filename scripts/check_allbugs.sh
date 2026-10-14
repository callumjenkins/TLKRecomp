#!/usr/bin/env sh
# All Bugs Final TAS gate (tas/extra/allbugs_final.bk2, per-poll tape allbugsF.read.tape).
# Runs tlk_headless default and --lle over the whole movie, asserts the two RAM logs are
# byte-identical, then compares against BizHawk with ram_sync_compare.py --resync and asserts:
#   - in sync at the last compared poll (re-synced to the end),
#   - every differing poll inside the known drift window (polls 3500-3800, plus 4234-4235),
#   - at most 150 differing polls,
#   - every level start on the same poll as BizHawk (outside the known window).
#
# usage: scripts/check_allbugs.sh [-b bin] [-o outdir]
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN="$ROOT/build/tlk_headless"
ROM="$ROOT/../Lion King, The (USA).sfc"
S="$ROOT/build-host/out/sync/ram"
OUT=
while getopts "b:o:" opt; do
    case $opt in b) BIN=$OPTARG ;; o) OUT=$OPTARG ;; *) sed -n '2,10p' "$0"; exit 2 ;; esac
done
KEEP=1
if [ -z "$OUT" ]; then OUT=$(mktemp -d "${TMPDIR:-/tmp}/tlk_allbugs.XXXXXX"); KEEP=0; fi
mkdir -p "$OUT"
FRAMES=$(wc -l <"$S/bh_ram_allbugsF.txt")
TAPE="$S/tapes/allbugsF.read.tape"

"$BIN" --rom "$ROM" --frames "$FRAMES" --input-polled "$TAPE" --ram-log "$OUT/aot.txt" >"$OUT/aot.log" 2>&1 &
p1=$!
"$BIN" --rom "$ROM" --frames "$FRAMES" --input-polled "$TAPE" --lle --ram-log "$OUT/lle.txt" >"$OUT/lle.log" 2>&1 &
p2=$!
wait $p1 || true
wait $p2 || true

fail=0
if cmp -s "$OUT/aot.txt" "$OUT/lle.txt"; then
    echo "default == --lle: IDENTICAL ($(wc -l <"$OUT/aot.txt") frames)"
else
    echo "default == --lle: DIFFER"; cmp "$OUT/aot.txt" "$OUT/lle.txt" || true; fail=1
fi

python3 "$ROOT/tools/ram_sync_compare.py" "$OUT/aot.txt" "$S/bh_ram_allbugsF.txt" --resync 50 \
    >"$OUT/sync.txt" || true
grep -v '^differing:' "$OUT/sync.txt"

python3 - "$OUT/sync.txt" <<'EOF' || fail=1
import sys
lines = open(sys.argv[1]).read().splitlines()
kv = lambda pre: next(l for l in lines if l.startswith(pre))
allowed = lambda p: 3500 <= p <= 3800 or p in (4234, 4235)
diff = [int(x) for x in kv("differing:").split(":", 1)[1].split(",") if x.strip()]
end = "end_in_sync=yes" in kv("last_matching_poll=")
outside = [p for p in diff if not allowed(p)]
lv = lambda pre: [tuple(t.split("/")) for t in kv(pre).split(":", 1)[1].split()]
bl, hl = lv("bizhawk levels"), lv("headless levels")
lvl_bad = [(x, y) for x, y in zip(bl, hl)
           if (x[0], x[2]) != (y[0], y[2]) and not (allowed(int(x[0])) or allowed(int(y[0])))]
if len(bl) != len(hl):
    lvl_bad.append(("count", len(bl), len(hl)))
ok = end and len(diff) <= 150 and not outside and not lvl_bad
print(f"differing={len(diff)} (<=150) outside_window={len(outside)} {outside[:10]} "
      f"end_in_sync={'yes' if end else 'no'} level_starts={len(bl)} level_mismatch={len(lvl_bad)} {lvl_bad[:5]}")
sys.exit(0 if ok else 1)
EOF

[ $KEEP = 1 ] || rm -rf "$OUT"
if [ $fail = 0 ]; then echo "ALLBUGS PASS"; else echo "ALLBUGS FAIL"; fi
exit $fail
