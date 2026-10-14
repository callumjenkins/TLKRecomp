#!/usr/bin/env sh
# Equivalence check for runner/tlk_decomp.c (native $C1CCFE loop, task K).
# Runs the --lle interpreter oracle twice, with the native decompressor loop
# forced off and on, and compares the per-frame WRAM hash log (TLK_FRAME_LOG)
# and the final frame hash. Any difference means the native loop is not
# bit-identical to interpreting it.
#
# usage: scripts/check_native_decomp.sh [-t tape] FRAMES
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN="$ROOT/build/tlk_headless"
ROM="$ROOT/../Lion King, The (USA).sfc"
TAPE=""
while getopts "t:" opt; do
    case $opt in t) TAPE=$OPTARG ;; *) sed -n '2,9p' "$0"; exit 2 ;; esac
done
shift $((OPTIND - 1))
[ $# -eq 1 ] || { sed -n '2,9p' "$0"; exit 2; }
N=$1
OUT=$(mktemp -d)
for on in 0 1; do
    set -- --rom "$ROM" --frames "$N" --lle
    [ -n "$TAPE" ] && set -- "$@" --input "$TAPE"
    TLK_NATIVE_DECOMP=$on TLK_FRAME_LOG="$OUT/f$on.txt" "$BIN" "$@" >"$OUT/o$on.txt" 2>&1 || true
done
ops=$(sed -n 's/^native_decomp_ops=\([0-9]*\).*/\1/p' "$OUT/o1.txt")
h0=$(sed -n 's/^frame_sha256=\(.\{16\}\).*/\1/p' "$OUT/o0.txt")
h1=$(sed -n 's/^frame_sha256=\(.\{16\}\).*/\1/p' "$OUT/o1.txt")
if cmp -s "$OUT/f0.txt" "$OUT/f1.txt" && [ "$h0" = "$h1" ]; then res=IDENTICAL; rc=0; else res=DIFFERENT; rc=1; fi
d=$(diff "$OUT/f0.txt" "$OUT/f1.txt" | grep -c '^<' || true)
printf '%s frames=%s tape=%s native_ops=%s frame=%s/%s wram_frames_differing=%s\n' \
    "$res" "$N" "${TAPE:-none}" "${ops:-?}" "$h0" "$h1" "$d"
rm -rf "$OUT"
exit $rc
