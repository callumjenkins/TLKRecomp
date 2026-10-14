#!/usr/bin/env sh
# Compare default (recompiled) mode against the --lle interpreter oracle.
#
# usage: scripts/compare_modes.sh [-t tape] [-r rom] [-b binary] [-o outdir] FRAME...
#   e.g. scripts/compare_modes.sh 600 3000
#        scripts/compare_modes.sh -t build/tapes/6987M.tape 1200 3000 6000
# Extra arguments for the default-mode run only: AOT_ARGS="--interp-targets f".
# Prints one line per frame: MATCH or MISMATCH, with frame/WRAM hashes and the
# number of frames each mode actually ran. Exit status: 0 if every frame matches.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN="$ROOT/build/tlk_headless"
ROM="$ROOT/../Lion King, The (USA).sfc"
TAPE=""
OUT="${TMPDIR:-/tmp}/tlk_compare_modes"
while getopts "t:r:b:o:" opt; do
    case $opt in
        t) TAPE=$OPTARG ;;
        r) ROM=$OPTARG ;;
        b) BIN=$OPTARG ;;
        o) OUT=$OPTARG ;;
        *) sed -n '2,9p' "$0"; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[ $# -gt 0 ] || { sed -n '2,9p' "$0"; exit 2; }
mkdir -p "$OUT"

run() { # mode frame -> "ran frame_sha wram_sha"
    mode=$1; n=$2
    set -- --rom "$ROM" --frames "$n" --dump-png "$OUT/${mode}_$n.png"
    [ -n "$TAPE" ] && set -- "$@" --input "$TAPE"
    if [ "$mode" = lle ]; then set -- "$@" --lle; else set -- "$@" ${AOT_ARGS:-}; fi
    "$BIN" "$@" >"$OUT/${mode}_$n.txt" 2>&1 || true
    awk -F'[= ]' '/^frames=/{r=$2} /^frame_sha256=/{f=$2} /^wram_sha256=/{w=$2}
        END{printf "%s %s %s", (r?r:"?"), (f?substr(f,1,16):"none"), (w?substr(w,1,16):"none")}' \
        "$OUT/${mode}_$n.txt"
}

fail=0
printf '%-6s %-8s %-18s %-18s %-11s %s\n' frame result lle_frame aot_frame wram 'ran(lle/aot)'
for n in "$@"; do
    set -- $(run lle "$n"); lr=$1 lf=$2 lw=$3
    set -- $(run aot "$n"); ar=$1 af=$2 aw=$3
    res=MATCH; [ "$lf" = "$af" ] && [ "$lr" = "$ar" ] || { res=MISMATCH; fail=1; }
    wm=same; [ "$lw" = "$aw" ] || wm=differs
    printf '%-6s %-8s %-18s %-18s %-11s %s/%s\n' "$n" "$res" "$lf" "$af" "$wm" "$lr" "$ar"
done
exit $fail
