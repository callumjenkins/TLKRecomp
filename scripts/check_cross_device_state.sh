#!/usr/bin/env bash
# A state saved on this PC loads on the phone and plays the same, and the reverse. Each side plays TAPE to frame
# SAVE and saves; the other loads that state with its sidecar and runs on with no input to END. Both must reach the
# hashes of the same load here, which differ from a fresh run's, so a load that was ignored fails. Needs the APK from
# the same commit installed, its ROM in place, and the phone on adb.
#
# usage: scripts/check_cross_device_state.sh [-b tlk_sdl] [-r ROM] [-t TAPE] [SAVE END]    (default 3000 6000)
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "$0")/.." && pwd)
bin="$ROOT/build/tlk_sdl" rom="" tape="$ROOT/build/playtapes/l05_random.tape"
while getopts b:r:t: o; do
    case $o in b) bin=$OPTARG ;; r) rom=$OPTARG ;; t) tape=$OPTARG ;; *) exit 2 ;; esac
done
shift $((OPTIND - 1))
save=${1:-3000} end=${2:-6000}
for r in "$rom" "$ROOT/Lion King, The (USA).sfc" "$ROOT/../Lion King, The (USA).sfc"; do
    [ -n "$r" ] && [ -f "$r" ] && { rom=$r; break; }
done
[ -f "$rom" ] || { echo "no ROM: pass -r" >&2; exit 2; }
[ -f "$tape" ] || { echo "no tape: pass -t (a per-frame .tape)" >&2; exit 2; }
adb=${ADB:-adb}
pkg=uk.callum.tlkrecomp
files=/data/data/$pkg/files

work=$(mktemp -d)
trap 'rm -rf "$work"; "$adb" shell run-as $pkg rm -f files/xd_pc.state files/xd_pc.png files/xd_pc.json \
    files/xd_an.state files/xd_an.png files/xd_an.json files/xd_out.txt files/xd.tape >/dev/null 2>&1 || true' EXIT
run() { SDL_VIDEO_DRIVER=dummy "$bin" --rom "$rom" --config "$work/t.ini" --no-audio --no-pace --no-record "$@" 2>/dev/null; }
hashes() { grep -E '^(frame|wram)_sha256=' | tr '\n' ' '; }
phone() {   # tlk_sdl options, comma-separated; prints the run's stdout
    "$adb" shell am force-stop $pkg
    "$adb" shell am start -W -n $pkg/.GameActivity --esa args "--no-audio,--no-pace,--no-record,$1" --es stdout xd_out.txt >/dev/null
    for _ in $(seq 240); do
        sleep 0.5
        "$adb" shell pidof $pkg:game >/dev/null || break
    done
    "$adb" shell run-as $pkg cat files/xd_out.txt
}
push() { for p in state png json; do "$adb" shell run-as $pkg sh -c "'cat > files/$2.$p'" < "$1.$p"; done; }
pull() { for p in state png json; do "$adb" shell run-as $pkg cat "files/$1.$p" > "$2.$p"; done; }

pc_id=$("$bin" --build-id | cut -d' ' -f1)
run --input "$tape" --frames "$save" --save-at "$save" "$work/pc.state" --device pc >/dev/null
want=$(run --load "$work/pc.state" --frames "$end" --hash-at "$end" | hashes)
fresh=$(run --frames "$end" --hash-at "$end" | hashes)
[ -n "$want" ] || { echo "no hashes from $bin" >&2; exit 1; }
[ "$want" != "$fresh" ] || { echo "the tape changed nothing by frame $save, so a load can't be told from none" >&2; exit 1; }
fail=0

push "$work/pc" xd_pc
got=$(phone "--load,$files/xd_pc.state,--frames,$end,--hash-at,$end" | hashes)
[ "$got" = "$want" ] && echo "MATCH    PC state on the phone" || { echo "MISMATCH PC state on the phone: '$got'"; fail=1; }

"$adb" shell run-as $pkg sh -c "'cat > files/xd.tape'" < "$tape"
phone "--input,$files/xd.tape,--frames,$save,--save-at,$save,$files/xd_an.state,--device,phone" >/dev/null
pull xd_an "$work/an"
an_id=$(sed -n 's/.*"build": "\([^"]*\)".*/\1/p' "$work/an.json")
[ "$an_id" = "$pc_id" ] && echo "MATCH    both builds are $pc_id" || { echo "MISMATCH build ids: PC $pc_id, phone $an_id"; fail=1; }
got=$(run --load "$work/an.state" --frames "$end" --hash-at "$end" | hashes)
[ "$got" = "$want" ] && echo "MATCH    phone state on the PC" || { echo "MISMATCH phone state on the PC: '$got'"; fail=1; }

[ $fail = 0 ] && echo "CROSS-DEVICE MATCH" || { echo "CROSS-DEVICE MISMATCH"; exit 1; }
