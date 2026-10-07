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

SRCS="src/dllmain.c src/config.c src/log.c src/dsound.c src/buffer.c src/device.c src/mixcore.c src/spatial.c src/reverb.c"
CFLAGS="-O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -ffast-math -msse2 -mfpmath=sse"

echo "CC = $CC"
# shellcheck disable=SC2086
case "$CC" in
    *zig*) LDFLAGS="" ;;                 # lld honours the .def names as-is
    *)     LDFLAGS="-Wl,--kill-at -static -s" ;;
esac
$CC $CFLAGS -shared -o build/dsound.dll $SRCS src/dsound.def $LDFLAGS -lole32 -luuid -lwinmm
rm -f build/dsound.lib build/dsound.pdb build/dllmain.lib
echo "built build/dsound.dll"

# dstrace: API tracer that sits in front of another dsound.dll (e.g. ALchemy's)
mkdir -p build/tracer
$CC $CFLAGS -shared -o build/tracer/dsound.dll tracer/trace.c tracer/dsound.def $LDFLAGS -lole32 -luuid
rm -f build/tracer/dsound.lib build/tracer/dsound.pdb build/tracer/trace.lib
echo "built build/tracer/dsound.dll"

# runtime smoke test (run it with: cd build && WINEDLLOVERRIDES=dsound=n wine dstest.exe)
$CC -O1 -o build/dstest.exe tests/dstest.c -lole32 -luuid
rm -f build/dstest.lib build/dstest.pdb

# native unit tests for the platform independent core
if command -v cc >/dev/null 2>&1; then
    cc -O1 -g -std=gnu11 -Wall -o build/test_mixcore tests/test_mixcore.c src/mixcore.c src/spatial.c src/reverb.c -lm
    ./build/test_mixcore
fi
