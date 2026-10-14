#!/usr/bin/env sh
# Task Y gate: the idle run (no input) must show the same picture in default
# (compiled) mode and under --lle at every K-th frame up to N.
#
# usage: scripts/check_idle_lle.sh [-n frames] [-k every] [-b binary] [-r rom]
#   defaults: -n 12000 -k 5. Prints the first differing frame, or
#   "IDLE MATCH frames=N every=K pictures=P". Exit status 0 on a match.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN="$ROOT/build/tlk_headless"
ROM="$ROOT/../Lion King, The (USA).sfc"
N=12000; K=5
while getopts "n:k:b:r:" opt; do
    case $opt in
        n) N=$OPTARG ;; k) K=$OPTARG ;; b) BIN=$OPTARG ;; r) ROM=$OPTARG ;;
        *) sed -n '2,8p' "$0"; exit 2 ;;
    esac
done
OUT=$(mktemp -d "${TMPDIR:-/tmp}/tlk_idle_lle.XXXXXX")
trap 'rm -rf "$OUT"' EXIT
mkdir -p "$OUT/aot" "$OUT/lle"
: > "$OUT/empty.tape"
"$BIN" --rom "$ROM" --frames "$N" --input "$OUT/empty.tape" \
    --dump-png "$OUT/aot/f.png" --dump-every "$K" >"$OUT/aot.txt" 2>&1 &
"$BIN" --rom "$ROM" --frames "$N" --input "$OUT/empty.tape" --lle \
    --dump-png "$OUT/lle/f.png" --dump-every "$K" >"$OUT/lle.txt" 2>&1
wait
(cd "$OUT/aot" && sha256sum f_*.png) > "$OUT/aot.sum"
(cd "$OUT/lle" && sha256sum f_*.png) > "$OUT/lle.sum"
P=$(wc -l < "$OUT/lle.sum")
[ "$P" -eq $((N / K)) ] || { echo "IDLE FAIL: $P pictures, expected $((N / K))"; exit 1; }
first=$(diff "$OUT/aot.sum" "$OUT/lle.sum" | sed -n 's/^< [0-9a-f]*  f_0*\([0-9]*\)\.png$/\1/p' | head -1)
if [ -n "$first" ]; then
    n=$(diff "$OUT/aot.sum" "$OUT/lle.sum" | grep -c '^<' || true)
    echo "IDLE MISMATCH first_frame=$first differing_pictures=$n of $P"
    exit 1
fi
echo "IDLE MATCH frames=$N every=$K pictures=$P"
