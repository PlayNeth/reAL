#!/bin/sh
# run.sh <dls> <seed> [secs] [dens]: GT render + msynth compare
D=${DLSDIR:-$HOME/re/mc2dm/directmusic/pc}
python3 -I $HOME/reAL/tools/dmgt/genscript.py $D/$1 $2 ${3:-6} ${4:-1} > s_$2.txt
WINEDLLOVERRIDES="dmusic,dmsynth,dmloader,dmime,dmband,dmstyle,dmcompos,dmscript,dswave=n" DMGT_DLDUMP=dl_$2.bin DMGT_RATE=${RATE:-44100} DMGT_VOICES=${VOICES:-64} timeout 300 wine $HOME/reAL/build/dmgt/dmgt.exe 'Z:\home\user\re\dm' synth "$(winepath -w $D/$1 2>/dev/null)" ref_$2.wav s_$2.txt >/dev/null 2>&1
printf "%-22s seed %-4s " $1 $2; MS_VOICES=${VOICES:-64} $HOME/reAL/build/test_msynth dl_$2.bin s_$2.txt ref_$2.wav mine_$2.wav
