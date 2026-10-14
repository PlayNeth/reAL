#!/bin/sh
# capperf.sh tag segment ms flags stop_ms next_ms segment2: capture dmime (dmgt perf) and reDM (test_perf) event
# streams for a segment, optionally stopped (Stop, StopEx x2 as MC2) and followed by a second segment, and compare them.
# cap2.sh tag seg ms flags stopms nextms seg2
tag=$1; n=$2; ms=$3; fl=$4; st=$5; nx=$6; n2=$7
D=$HOME/re/mc2dm/directmusic/pc
W="Z:$(echo $D | tr / '\\\\')"
DMGT_STOP=$st DMGT_NEXT="$nx:$W\\$n2.sgt" DMGT_VTIME=1 WINEDLLOVERRIDES="dmusic,dmsynth,dmloader,dmime,dmband,dmstyle,dmcompos,dmscript,dswave=n" timeout 900 wine $HOME/reAL/build/dmgt/dmgt.exe 'Z:\home\user\re\dm' perf "$W\\$n.sgt" ref_$tag.log $ms $fl >/dev/null 2>&1
TP_STOP=$st TP_NEXT="$nx:$D/$n2.sgt" $HOME/reAL/build/test_perf $D/$n.sgt mine_$tag.log $ms $fl
echo "== $tag"; python3 "$(dirname "$0")/cmpevents.py" mine_$tag.log ref_$tag.log
