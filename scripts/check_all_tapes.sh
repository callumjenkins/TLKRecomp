#!/usr/bin/env sh
# Default (recompiled) mode vs --lle on every play tape at one frame, plus the
# compare_modes.sh standard set. Prints a summary table; exit 0 iff all MATCH.
#
# usage: scripts/check_all_tapes.sh [-f FRAME] [-j JOBS] [-d TAPEDIR] [-s] [-b bin]
#   -f frame for the play tapes (default 5000)
#   -j parallel jobs (default nproc)
#   *.polled.tape files are fed with --input-polled (one word per pad read)
#   -s skip the standard set (no input 600/3000, 6987M 1200/6000)
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

if [ "${1:-}" = --one ]; then # internal: --one TAPE MODE (env BIN ROM FRAME OUT)
    t=$2; m=$3; n=$(basename "$t" .tape)
    if [ "$m" = lle ]; then x=--lle; else x=; fi
    inp=--input; case $t in *.polled.tape) inp=--input-polled ;; esac  # one word per pad poll
    "$BIN" --rom "$ROM" --frames "$FRAME" $inp "$t" $x >"$OUT/$n.$m.txt" 2>&1 || true
    awk -F'[= ]' '/^frames=/{r=$2} /^frame_sha256=/{f=substr($2,1,16)}
        END{printf "%s %s\n", (r?r:"?"), (f?f:"none")}' "$OUT/$n.$m.txt" >"$OUT/$n.$m"
    exit 0
fi

BIN="$ROOT/build/tlk_headless"
ROM="$ROOT/../Lion King, The (USA).sfc"
FRAME=5000; JOBS=$(nproc); DIR="$ROOT/build/playtapes"; STD=1
while getopts "f:j:d:sb:" opt; do
    case $opt in
        f) FRAME=$OPTARG ;; j) JOBS=$OPTARG ;; d) DIR=$OPTARG ;; s) STD=0 ;; b) BIN=$OPTARG ;;
        *) sed -n '2,8p' "$0"; exit 2 ;;
    esac
done
OUT=$(mktemp -d "${TMPDIR:-/tmp}/tlk_check_all.XXXXXX")
export BIN ROM FRAME OUT

for t in "$DIR"/*.tape; do for m in lle aot; do printf '%s\n%s\n' "$t" "$m"; done; done |
    xargs -d '\n' -P "$JOBS" -n 2 "$0" --one

ok=0; bad=0; fail=0
printf '%-14s %-8s %-18s %-18s %s\n' tape result lle_frame aot_frame 'ran(lle/aot)'
for t in "$DIR"/*.tape; do
    n=$(basename "$t" .tape)
    set -- $(cat "$OUT/$n.lle") $(cat "$OUT/$n.aot")
    res=MATCH; { [ "$2" = "$4" ] && [ "$1" = "$3" ] && [ "$2" != none ]; } || res=MISMATCH
    if [ $res = MATCH ]; then ok=$((ok+1)); else bad=$((bad+1)); fail=1; fi
    printf '%-14s %-8s %-18s %-18s %s/%s\n' "$n" $res "$2" "$4" "$1" "$3"
done
echo "play tapes @f$FRAME: $ok MATCH, $bad MISMATCH"
if [ $STD = 1 ]; then
    echo "standard set:"
    "$ROOT/scripts/compare_modes.sh" -b "$BIN" -o "$OUT/std" 600 3000 || fail=1
    "$ROOT/scripts/compare_modes.sh" -b "$BIN" -o "$OUT/std" -t "$ROOT/build/tapes/6987M.tape" 1200 6000 || fail=1
fi
if [ $fail = 0 ]; then echo "ALL MATCH"; else echo "SOME MISMATCH"; fi
rm -rf "$OUT"
exit $fail
