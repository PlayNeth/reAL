#!/bin/sh
# wine.sh <corpus dir> <work dir> <seconds> [play|noplay] [ext]: tests/ldfuzz.c in rounds of 1500 inputs under reAL;
# a round that dies (crash, hang, damaged heap) leaves its input and the end of its output in <work>/found/<n>/.
# BUILD: where dsound.dll and ldfuzz.exe are (default ./build); WINEPREFIX as set.
C=$(cd "$1" && pwd); D=$2; mkdir -p "$D/found"; D=$(cd "$D" && pwd); B=${BUILD:-build}
cp "$B/dsound.dll" "$B/ldfuzz.exe" "$D/" || exit 1
printf '[default]\nlog=0\nmusic=1\n' > "$D/reAL.ini"
cd "$D" || exit 1
end=$(( $(date +%s) + $3 )); n=0; bad=0
while [ "$(date +%s)" -lt "$end" ]; do
    seed=$(( $(date +%s) * 7 + n * 131 + $$ )); n=$((n + 1))
    WINEDEBUG=${WINEDEBUG:--all} WINEDLLOVERRIDES="dsound=n,b" timeout 300 wine ldfuzz.exe "Z:$(echo "$C" | tr / '\\')" 1500 $seed ${4:-noplay} $5 > out.txt 2>&1
    rc=$?
    if ! grep -aq DONE out.txt || grep -aq 'err:heap' out.txt; then
        bad=$((bad + 1)); k=found/$n; mkdir -p $k; cp cur.bin cur.txt $k/ 2>/dev/null
        tr -d '\r' < out.txt | tail -20 > $k/out.txt; echo "rc $rc seed $seed" >> $k/out.txt
    fi
done
echo "rounds $n, $bad died"
