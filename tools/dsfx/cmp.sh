#!/bin/sh
# cmp.sh <effect> <rate> <ch> <bits> <chunk> [params...]: run dsdmo.dll's effect (dmgt dmo, Microsoft prefix) and
# reAL's port (build/test_dsfx) on the same signal and compare outputs and printed parameters.
# Inputs come from tools/dsfx/gen.py (in<bits><ch>.raw at the rate) in $FXDIR (default /tmp/real/sp/fx).
e=$1; rate=$2; ch=$3; bits=$4; chunk=$5; shift 5
D=${FXDIR:-/tmp/real/sp/fx}; cd $D || exit 1
in=in_${bits}_${ch}_${rate}.raw
[ -f $in ] || python3 -I $HOME/reAL/tools/dsfx/gen.py $in $rate $ch $bits
tag=$(echo "$e $rate $ch $bits $chunk $*" | tr ' ' '_')
WINEPREFIX=${MSPREFIX:-/tmp/real/sp/msprefix} WINEDEBUG=-all wine $HOME/reAL/build/dmgt/dmgt.exe "${DMDIR:-Z:\home\user\re\dm}" dmo $e $in ref_$tag.raw $rate $ch $bits $chunk "$@" 2>ms_$tag.txt
tr -d '\r' < ms_$tag.txt | grep -v "^cannot load" > ms_$tag.t && mv ms_$tag.t ms_$tag.txt
r=$($HOME/reAL/build/test_dsfx $e $in ref_$tag.raw $rate $ch $bits $chunk "$@" 2>me_$tag.txt)
if cmp -s ms_$tag.txt me_$tag.txt; then p=same; else p="PARAMS DIFFER: $(diff ms_$tag.txt me_$tag.txt | grep '^[<>]' | tr '\n' ' ')"; fi
printf "%-50s %s  %s\n" "$tag" "$r" "$p"
