#!/bin/sh
# Install reAL into a Wine prefix.
#
#   ./install.sh                       global: every app in the prefix uses reAL
#   ./install.sh --app game.exe ...    only these executables use reAL (others keep Wine's dsound)
#   ./install.sh --uninstall
#
# Uses $WINEPREFIX (default ~/.wine). Note: `wineboot -u` (runs after Wine upgrades) restores
# Wine's own dsound.dll file; just run this script again afterwards.
set -e
cd "$(dirname "$0")"
PREFIX="${WINEPREFIX:-$HOME/.wine}"
DLL="build/dsound.dll"
export WINEPREFIX="$PREFIX"
export WINEDEBUG=-all

[ -d "$PREFIX/drive_c/windows" ] || { echo "no Wine prefix at $PREFIX"; exit 1; }

# 32-bit system dir: syswow64 in a 64-bit prefix, system32 in a 32-bit one
if [ -d "$PREFIX/drive_c/windows/syswow64" ]; then SYS="$PREFIX/drive_c/windows/syswow64"
else SYS="$PREFIX/drive_c/windows/system32"; fi

if [ "$1" = "--uninstall" ]; then
    [ -f "$SYS/dsound.dll.wine" ] && mv -f "$SYS/dsound.dll.wine" "$SYS/dsound.dll"
    wine reg delete 'HKCU\Software\Wine\DllOverrides' /v dsound /f >/dev/null 2>&1 || true
    echo "reAL removed from $SYS (per-app overrides under AppDefaults are left; they now fall back to builtin)"
    exit 0
fi

[ -f "$DLL" ] || ./build.sh
[ -f "$SYS/dsound.dll.wine" ] || cp -f "$SYS/dsound.dll" "$SYS/dsound.dll.wine"
cp -f "$DLL" "$SYS/dsound.dll"
echo "installed $DLL -> $SYS/dsound.dll"

if [ ! -f "$PREFIX/drive_c/windows/reAL.ini" ]; then
    cp reAL.ini.example "$PREFIX/drive_c/windows/reAL.ini"
    echo "config: $PREFIX/drive_c/windows/reAL.ini"
fi

if [ "$1" = "--app" ]; then
    shift
    for exe in "$@"; do
        wine reg add "HKCU\\Software\\Wine\\AppDefaults\\$exe\\DllOverrides" /v dsound /d native,builtin /f >/dev/null
        echo "override: $exe -> dsound=native,builtin"
    done
else
    wine reg add 'HKCU\Software\Wine\DllOverrides' /v dsound /d native,builtin /f >/dev/null
    echo "override: global dsound=native,builtin"
fi
