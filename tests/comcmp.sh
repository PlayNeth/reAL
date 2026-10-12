#!/bin/sh
# comcmp.sh <exe> [args]: run a COM test under Microsoft's DirectMusic (msprefix) and under reAL, diff the outputs
S=/tmp/real/-home-user/064cfa03-6450-5ca8-80b8-448b8ab58db4/scratchpad/com
mkdir -p $S; cp $HOME/reAL/build/$1 $HOME/reAL/build/dsound.dll $S/
cp $HOME/re/mc2dm/directmusic/pc/bells1.sgt $HOME/re/mc2dm/directmusic/pc/bells.dls $HOME/re/mc2dm/directmusic/pc/fe_hard_04.sgt $HOME/re/mc2dm/directmusic/pc/fe_hard.dls $S/ 2>/dev/null
cd $S; exe=$1; shift
printf '[default]\nlog=1\nmusic=1\n' > reAL.ini
mv dsound.dll real_dsound.dll
WINEDEBUG=-all WINEPREFIX=/tmp/real/sp/msprefix WINEDLLOVERRIDES="dsound=n;dmusic,dmime,dmloader,dmsynth,dmband,dswave,dmstyle,dmcompos,dmscript=n" timeout 120 wine $exe "$@" 2>/dev/null | tr -d '\r' > ms.txt
mv real_dsound.dll dsound.dll
WINEDEBUG=-all WINEDLLOVERRIDES="dsound=n,b" timeout 120 wine $exe "$@" 2>/dev/null | tr -d '\r' > real.txt
if diff ms.txt real.txt > diff.txt; then echo "MATCH ($(wc -l < ms.txt) lines)"; else echo "DIFF"; head -${SHOW:-40} diff.txt; fi
