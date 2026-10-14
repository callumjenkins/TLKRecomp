#!/bin/sh
# usage: tools/aot_pace/pace_diff.sh TAPE FRAMES TRACE_BIN
# First hardware-register access whose master clock (or APU-port SPC clock)
# differs between --lle and default mode (TLK_HWLOG), then the first opcode
# whose start clock differs around it (SNESRECOMP_CYC_WATCH + AOT trace).
# TRACE_BIN: a tlk_headless built with -DAOT_PACE_TRACE.
S=${TMPDIR:-/tmp}/aot_pace_diff; mkdir -p $S; H=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
T=$1; N=$2; B=${3:?trace build binary (-DAOT_PACE_TRACE)}; R="$H/../../../Lion King, The (USA).sfc"
cd "$H/../.."
TLK_HWLOG=$S/hw_l.txt $B --rom "$R" --frames $N --input $T --lle >/dev/null 2>&1 &
TLK_HWLOG=$S/hw_a.txt $B --rom "$R" --frames $N --input $T >/dev/null 2>&1
wait
grep -v " aot " $S/hw_l.txt > $S/hw_l2.txt; grep -v " aot " $S/hw_a.txt > $S/hw_a2.txt
python3 $H/hw_diff.py $S/hw_l2.txt $S/hw_a2.txt > $S/hwdiff.out
head -3 $S/hwdiff.out
W=$(grep WINDOW $S/hwdiff.out)
[ -z "$W" ] && exit 0
set -- $W; F0=$2; FR=$3; LO=$4; HI=$5
echo "frame $FR window $LO..$HI"
for m in l a; do x=; [ $m = l ] && x=--lle
  SNESRECOMP_CYC_WATCH=000000-FFFFFF TLK_HWLOG=$S/tr_$m.txt TLK_HWLOG_FROM=$F0 $B --rom "$R" --frames $((FR+1)) --input $T $x 2>&1 | awk -v a=$F0 -v b=$FR '/^\[cyc\] f=/{split($2,q,"="); if (q[2]+0>=a && q[2]+0<=b) print}' > $S/cyc_$m.txt &
done; wait
python3 $H/op_merge.py $S/cyc_l.txt $S/tr_l.txt $S/cyc_a.txt $S/tr_a.txt $LO $((HI+2000)) > $S/merged.txt
n=$(grep -n "<<<" $S/merged.txt | head -1 | cut -d: -f1); [ -n "$n" ] && sed -n "$((n>6?n-6:1)),$((n+4))p" $S/merged.txt
