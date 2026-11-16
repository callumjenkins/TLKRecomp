#!/usr/bin/env bash
# Saves a state at each of several frames of a per-frame tape, resumes each in a new process to the end
# frame, and compares the final frame and WRAM with one uninterrupted run. Then the same for the autosave:
# a run that autosaves every 10 s of play, resumed from its last autosave. Last, the load guard.
# A FRAME written as FRAME=LEVEL,LIVES also checks what the sidecar says, such as 4000=Hakuna,6.
# END must be in play, where quitting autosaves.
#
# usage: scripts/check_state_resume.sh [-b tlk_sdl] [-r ROM] TAPE END FRAME[=LEVEL,LIVES]...
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "$0")/.." && pwd)
bin="$ROOT/build/tlk_sdl" rom=""
while getopts b:r: o; do
    case $o in b) bin=$OPTARG ;; r) rom=$OPTARG ;; *) exit 2 ;; esac
done
shift $((OPTIND - 1))
[ $# -ge 3 ] || { echo "usage: $0 [-b tlk_sdl] [-r ROM] TAPE END FRAME[=LEVEL,LIVES]..." >&2; exit 2; }
tape=$1 end=$2
shift 2
for r in "$rom" "$ROOT/Lion King, The (USA).sfc" "$ROOT/../Lion King, The (USA).sfc"; do
    [ -n "$r" ] && [ -f "$r" ] && { rom=$r; break; }
done
[ -f "$rom" ] || { echo "no ROM: pass -r" >&2; exit 2; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
run() { "$bin" --rom "$rom" --config "$work/t.ini" --no-audio --no-pace --input "$tape" "$@" 2>/dev/null; }
hashes() { grep -E '^(frame|wram)_sha256=' | tr '\n' ' '; }

want=$(run --frames "$end" | hashes)
fail=0
check() {   # label, state
    local got
    got=$(run --load "$2" --frames "$end" | hashes)
    if [ "$got" = "$want" ]; then echo "MATCH    $1"; else echo "MISMATCH $1"; fail=1; fi
}
first=${1%%=*}
for arg in "$@"; do
    f=${arg%%=*}
    run --frames "$f" --save-at "$f" "$work/s$f.state" --device gate-host >/dev/null
    # The json is written last, so it can vouch for the other two.
    order=$(stat -c '%.9Y %n' "$work/s$f".{state,png,json} | sort -s -n | sed 's/.*\.//' | tr '\n' ' ')
    [ "$order" = "state png json " ] || { echo "MISMATCH sidecar order at $f: $order"; fail=1; }
    said="$(sed -n 's/.*"level_name": "\([^"]*\)".*/\1/p' "$work/s$f.json"),$(sed -n 's/.*"lives": \([0-9]*\).*/\1/p' "$work/s$f.json")"
    sha=$(sed -n 's/.*"state_sha256": "\([0-9a-f]*\)".*/\1/p' "$work/s$f.json")
    [ "$sha" = "$(sha256sum < "$work/s$f.state" | cut -d' ' -f1)" ] || { echo "MISMATCH sidecar sha at $f: '$sha'"; fail=1; }
    grep -q '"device": "gate-host"' "$work/s$f.json" || { echo "MISMATCH sidecar at $f has no device"; fail=1; }
    [ "$arg" = "$f" ] || [ "${arg#*=}" = "$said" ] || { echo "MISMATCH sidecar at $f says $said, want ${arg#*=}"; fail=1; }
    check "saved at $f ($said)" "$work/s$f.state"
done

printf 'autosave_seconds = 10\n' > "$work/t.ini"
autos=$(run --frames "$end" --autosave "$work/auto.state" | sed -n 's/^autosave=.* at_frame=//p')
[ "$(tail -n1 <<<"$autos")" = "$end" ] && echo "MATCH    autosave on quitting" || { echo "MISMATCH no autosave on quitting at $end"; fail=1; }
last=$(tail -n2 <<<"$autos" | head -n1)
rm -f "$work/auto.state" "$work/auto.json"
# Stop at that autosave, so the file holds it rather than the one taken on leaving.
run --frames "$last" --autosave "$work/auto.state" >/dev/null
check "autosave at $last" "$work/auto.state"
# The title and the attract demo that follows it are not places to resume.
n=$("$bin" --rom "$rom" --config "$work/t.ini" --no-audio --no-pace --frames 6000 --autosave "$work/idle.state" 2>/dev/null | grep -c '^autosave=' || true)
[ "$n" = 0 ] && echo "MATCH    no autosave on the title or the attract demo" || { echo "MISMATCH $n autosaves with no input"; fail=1; }

# The load guard: a sidecar naming another build refuses the state; with no sidecar, as an earlier build
# leaves it, the state loads as before.
cp "$work/s$first.state" "$work/other.state"
sed 's/"build": "[^"]*"/"build": "another"/' "$work/s$first.json" > "$work/other.json"
if run --load "$work/other.state" --frames "$end" >/dev/null; then echo "MISMATCH another build's state loaded"; fail=1
else echo "MATCH    another build's state refused"; fi
if run --frames "$end" --load-at "$first" "$work/other.state" >/dev/null; then echo "MISMATCH --load-at loaded another build's state"; fail=1
else echo "MATCH    --load-at refused another build's state"; fi
rm "$work/other.json"
check "no sidecar loads" "$work/other.state"

[ $fail = 0 ] && echo "RESUME MATCH" || { echo "RESUME MISMATCH"; exit 1; }
