#!/usr/bin/env sh
# Run every tape in a dir in default mode with --log, in parallel.
# usage: scripts/coverage_runs.sh TAPEDIR OUTDIR [jobs]
# Frame count per run = tape length + 300; *.polled.tape: --input-polled, $POLLED_FRAMES (49000). Prints one summary line per tape.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BIN="${BIN:-$ROOT/build/tlk_headless}"
ROM="${ROM:-$ROOT/../Lion King, The (USA).sfc}"
TD=$1; OD=$2; J=${3:-10}
mkdir -p "$OD"
export BIN ROM OD
ls "$TD"/*.tape | xargs -P "$J" -I{} sh -c '
  t={}; s=$(basename "$t" .tape); n=$(( $(stat -c %s "$t") / 2 + 300 )); inp=--input
  case $t in *.polled.tape) inp=--input-polled; n=${POLLED_FRAMES:-49000} ;; esac
  "$BIN" --rom "$ROM" --frames $n $inp "$t" --log "$OD/$s.json" --dump-png "$OD/$s.png" >"$OD/$s.txt" 2>&1 || true
  printf "%-14s %s\n" "$s" "$(grep -E "^result=" "$OD/$s.txt" | tr "\n" " ")"'
