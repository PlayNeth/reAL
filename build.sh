#!/bin/sh
# Build reAL's 32-bit dsound.dll.
#   CC=i686-w64-mingw32-gcc ./build.sh   # with a MinGW toolchain
#   ./build.sh                            # default: zig cc (pip install ziglang)
set -e
cd "$(dirname "$0")"
mkdir -p build

if [ -z "$CC" ]; then
    if command -v i686-w64-mingw32-gcc >/dev/null 2>&1; then
        CC=i686-w64-mingw32-gcc
    elif command -v zig >/dev/null 2>&1; then
        CC="zig cc -target x86-windows-gnu"
    else
        CC="python3 -m ziglang cc -target x86-windows-gnu"
    fi
fi

SRCS="src/dllmain.c src/config.c src/log.c src/dsound.c src/buffer.c src/device.c src/mixcore.c src/spatial.c src/reverb.c src/alrv.c src/hrtf.c"
# ported parameter code that reproduces ALchemy's float rounding: no -ffast-math
EXACT_SRCS="src/alrv_params.c src/eax.c src/eax4fx.c src/eax4dsp.c"
CFLAGS_EXACT="-O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -msse2 -mfpmath=sse"
CFLAGS="$CFLAGS_EXACT -ffast-math"

echo "CC = $CC"
# shellcheck disable=SC2086
case "$CC" in
    *zig*) LDFLAGS="" ;;                 # lld honours the .def names as-is
    *)     LDFLAGS="-Wl,--kill-at -static -s" ;;
esac
mkdir -p build/obj
OBJS=""
for f in $EXACT_SRCS; do
    o=build/obj/$(basename "$f" .c).o
    $CC $CFLAGS_EXACT -c -o "$o" "$f"
    OBJS="$OBJS $o"
done
# shellcheck disable=SC2086
$CC $CFLAGS -shared -o build/dsound.dll $SRCS $OBJS src/dsound.def $LDFLAGS -lole32 -luuid -lwinmm
rm -f build/dsound.lib build/dsound.pdb build/dllmain.lib
echo "built build/dsound.dll"

# dstrace: API tracer that sits in front of another dsound.dll (e.g. ALchemy's)
mkdir -p build/tracer
$CC $CFLAGS -shared -o build/tracer/dsound.dll tracer/trace.c tracer/dsound.def $LDFLAGS -lole32 -luuid
rm -f build/tracer/dsound.lib build/tracer/dsound.pdb build/tracer/trace.lib
echo "built build/tracer/dsound.dll"

# dmtrace: DirectMusic API tracer (COM server; dmtrace/gen.py regenerates dmtrace_gen.c from the MinGW headers)
mkdir -p build/dmtrace
$CC -O2 -std=gnu11 -Wall -Wno-unused-parameter -shared -o build/dmtrace/dmtrace.dll dmtrace/dmtrace.c dmtrace/dmtrace_gen.c \
    dmtrace/dmtrace.def $LDFLAGS -lole32 -luuid -ladvapi32 -ldxguid -loleaut32
$CC -O1 -o build/dmtrace/dmtest.exe dmtrace/dmtest.c -lole32 -luuid -ldxguid
cp dmtrace/test.mid build/dmtrace/
rm -f build/dmtrace/*.lib build/dmtrace/*.pdb
echo "built build/dmtrace/dmtrace.dll"

# dmgt: reference renders from Microsoft's DirectMusic (run under Wine, see tools/dmgt/dmgt.c)
mkdir -p build/dmgt
$CC -O1 -o build/dmgt/dmgt.exe tools/dmgt/dmgt.c -lole32 -luuid -ldxguid -ldsound
rm -f build/dmgt/*.lib build/dmgt/*.pdb

# runtime smoke test (run it with: cd build && WINEDLLOVERRIDES=dsound=n wine dstest.exe)
$CC -O1 -o build/dstest.exe tests/dstest.c -lole32 -luuid
rm -f build/dstest.lib build/dstest.pdb
$CC -O1 -o build/hrtfprobe.exe tests/hrtfprobe.c -lole32 -luuid
rm -f build/hrtfprobe.lib build/hrtfprobe.pdb
$CC -O1 -o build/eaxtest.exe tests/eaxtest.c -lole32 -luuid
rm -f build/eaxtest.lib build/eaxtest.pdb

# native unit tests for the platform independent core
if command -v cc >/dev/null 2>&1; then
    cc -O1 -g -std=gnu11 -Wall -o build/test_mixcore tests/test_mixcore.c src/mixcore.c src/spatial.c src/reverb.c src/alrv.c src/alrv_params.c src/eax.c src/eax4fx.c src/hrtf.c -lm
    ./build/test_mixcore
    # comparison with ALchemy's own reverb (ground truth from the emulator, see docs; optional)
    cc -O2 -std=gnu11 -Wall -o build/test_alrv tests/test_alrv.c src/alrv.c src/alrv_params.c -lm
    cc -O2 -std=gnu11 -Wall -o build/test_alrv_params tests/test_alrv_params.c src/alrv_params.c -lm
    cc -O2 -std=gnu11 -Wall -o build/test_eax tests/test_eax.c src/eax.c src/eax4fx.c src/alrv_params.c -lm
    cc -O2 -std=gnu11 -Wall -o build/test_eax4 tests/test_eax4.c src/eax.c src/eax4fx.c src/alrv_params.c -lm
    cc -O2 -std=gnu11 -Wall -o build/test_fx tests/test_fx.c src/eax4dsp.c -lm
    # reAL-Music performance engine against dmime captures (tools/dmgt perf, DMGT_VTIME=1)
    cc -O1 -std=gnu11 -Wall -o build/test_perf tests/test_perf.c src/music/dmperf.c src/music/dmseg.c -lm
    cc -O2 -std=gnu11 -Wall -o build/test_msynth tests/test_msynth.c src/music/msynth.c -lm
    GT=${REAL_GT:-../re/gt}
    if [ -f "$GT/params.bin" ]; then
        ./build/test_alrv_params "$GT/params.bin"
        [ -f "$GT/params_pan.bin" ] && ./build/test_alrv_params "$GT/params_pan.bin"
        [ -f "$GT/eax.bin" ] && ./build/test_eax "$GT/eax.bin"
        [ -f "$GT/eax4.bin" ] && ./build/test_eax4 "$GT/eax4.bin" "$GT/eax4fx.bin"
        for fx in distortion ringmod fshift agc eq autowah echo chorus flanger pshift vmorph; do
            [ -f "$GT/fx_$fx.bin" ] && ./build/test_fx $fx "$GT/fx_$fx.bin"
        done
        ./build/test_alrv -p "$GT" env00 env01 env02 env07 env12 env19 env22 env25 pan_mix pan_mix2 pan_r30 pan_l60 | tail -1
    fi
fi
