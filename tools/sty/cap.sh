#!/bin/sh
# cap.sh <case> [ms]: Microsoft (dmgt perf, DMGT_TIME fixing the rand seeds) vs reAL (test_perf) on <case>.sgt in
# /tmp/real/sty (tools/sty/cases.py writes them).  Env: MINE=1 reuse the reference; TAG output name; FL PlaySegment
# flags; STOP=<ms>; NEXT=<ms>:<case> a second segment; TIME the time() seed; NT notification types.
cd /tmp/real/sty
c=$1; ms=${2:-5000}; t=${TAG:-$c}
W="Z:\\tmp\\real\\sty"
NX=""; TNX=""
if [ -n "$NEXT" ]; then
  case "$NEXT" in
  *.sty:*) NX="${NEXT%%:*}:$W\\${NEXT#*:}"; TNX="${NEXT%%:*}:/tmp/real/sty/${NEXT#*:}" ;;   # <ms>:<style>:<motif>
  *) NX="${NEXT%%:*}:$W\\${NEXT#*:}.sgt"; TNX="${NEXT%%:*}:/tmp/real/sty/${NEXT#*:}.sgt" ;;
  esac
fi
[ -n "$MINE" ] || DMGT_RANDLOG=rref_$t.log DMGT_FLAGS2=$FL2 DMGT_STOP=$STOP DMGT_NEXT="$NX" DMGT_TIME=${TIME:-1234567} DMGT_NOTIFY=nref_$t.log DMGT_NTYPES=${NT:-spm} DMGT_VTIME=1 WINEDLLOVERRIDES="dmusic,dmsynth,dmloader,dmime,dmband,dmstyle,dmcompos,dmscript,dswave=n" timeout 300 wine $HOME/reAL/build/dmgt/dmgt.exe 'Z:\home\user\re\dm' perf "$W\\$c.sgt" ref_$t.log $ms ${FL:-0} >/dev/null 2>&1
seed=$(head -1 ref_$t.log | tr -d "\r" | awk "{print \$3}"); aseed=$(head -1 ref_$t.log | tr -d "\r" | awk "{print \$4}")
TP_RANDLOG=rmine_$t.log TP_FLAGS2=$FL2 TP_STOP=$STOP TP_NEXT="$TNX" TP_NOTIFY=nmine_$t.log TP_NTYPES=${NT:-spm} TP_SEED=$seed TP_APPSEED=$aseed $HOME/reAL/build/test_perf $c.sgt mine_$t.log $ms ${FL:-0}
echo "== $t"; python3 $HOME/reAL/tools/dmgt/cmpevents.py mine_$t.log ref_$t.log | tail -5
[ -n "$MINE" ] || { tr -d "\r" < nref_$t.log > nref2_$t.log && mv nref2_$t.log nref_$t.log; }
diff nmine_$t.log nref_$t.log > /dev/null && echo "notifications match" || { echo "notifications differ:"; diff nmine_$t.log nref_$t.log | head -${ND:-10}; }
