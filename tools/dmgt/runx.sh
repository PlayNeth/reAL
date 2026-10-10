#!/bin/sh
# runx.sh <dls> <seed> [secs] [dens]: DirectX 8 (multi-bus) GT + compare. env BUSES, ASSIGN, VOICES, RATE
D=${DLSDIR:-$HOME/re/mc2dm/directmusic/pc}
python3 -I $HOME/reAL/tools/dmgt/genscript.py $D/$1 $2 ${3:-6} ${4:-1} > sx_$2.txt
DMGT_ASSIGN="$ASSIGN" DMGT_BUSES="${BUSES:-0,1,64,65}" DMGT_VOICES=${VOICES:-64} DMGT_RATE=${RATE:-44100} WINEDLLOVERRIDES="dmusic,dmsynth,dmloader,dmime,dmband,dmstyle,dmcompos,dmscript,dswave=n" DMGT_DLDUMP=dlx_$2.bin timeout 300 wine $HOME/reAL/build/dmgt/dmgt.exe 'Z:\home\user\re\dm' synth "$(winepath -w $D/$1 2>/dev/null)" refx_$2.wav sx_$2.txt >/dev/null 2>&1
[ -z "$ASSIGN" ] && unset ASSIGN
printf "%-18s seed %-4s %-28s " $1 $2 "${BUSES:-0,1,64,65}"
MS_ASSIGN="$ASSIGN" MS_BUSES="${BUSES:-0,1,64,65}" MS_VOICES=${VOICES:-64} $HOME/reAL/build/test_msynth dlx_$2.bin sx_$2.txt refx_$2.wav minex_$2.wav | awk '{print $5, "differ, first", $11, "max", $14}'
