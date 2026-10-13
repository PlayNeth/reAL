#!/bin/sh
# cfgcmp.sh [config...]: tests/cfgtest.c's configurations one at a time (Microsoft's sink does not always recover
# from a failed one) under Microsoft's DirectMusic and reAL-Music, through comcmp.sh with the probe DMO's logs.
# The port and sink stages are left out: reAL-Music has no port objects.
cd "$(dirname "$0")/.." || exit 1
D=${COMCMP_DIR:-/tmp/real/sp/cfg}
set -- ${*:-std std2 mono3d envrev custom plain custom3d mixin order nofeature mixplay envplay errors}
for c in "$@"; do
    COMCMP_DIR=$D FXPROBE_LOG=fxlog.txt FXPROBE_DATA=fxdata.raw sh tests/comcmp.sh cfgtest.exe "$c" >/dev/null
    grep -v '^  \(sink\|port\): ' $D/ms.txt > $D/ms.f; grep -v '^  \(sink\|port\): ' $D/real.txt > $D/real.f
    if diff $D/ms.f $D/real.f > $D/diff.txt; then echo "$c: MATCH ($(wc -l < $D/ms.f) lines)"
    else echo "$c: DIFF"; head -${SHOW:-20} $D/diff.txt; fi
done
