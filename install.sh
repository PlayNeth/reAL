#!/bin/sh
# Install reAL into a Wine prefix.
#
#   ./install.sh                       global: every app in the prefix uses reAL
#   ./install.sh --app game.exe ...    only these executables use reAL (others keep Wine's dsound)
#   ./install.sh --winmm /path/to/game.exe ...
#                                      also reAL's winmm.dll next to these games (their waveOut sound through reAL;
#                                      for games that never load DirectSound, it is what loads reAL)
#   ./install.sh --winmm               reAL's winmm.dll for every app in the prefix: it takes the place of Wine's in
#                                      the system directory, with Wine's kept beside it as winmm.dll.wine
#   ./install.sh --uninstall
#
# Uses $WINEPREFIX (default ~/.wine) and the wine in PATH, which should be the Wine the prefix is used with (any
# wine command brings a prefix to its own version).  After a Wine upgrade run this script again: reAL's DLLs stay
# in place, but the copies of Wine's own kept beside them (dsound.dll.wine, winmm.dll.wine) are the old Wine's
# until it has.
set -e
cd "$(dirname "$0")"
PREFIX="${WINEPREFIX:-$HOME/.wine}"
DLL="build/dsound.dll"
if [ -f dsound.dll ]; then DLL="dsound.dll"; fi     # (a release package: the DLL next to this script)
export WINEPREFIX="$PREFIX"
export WINEDEBUG=-all

[ -d "$PREFIX/drive_c/windows" ] || { echo "no Wine prefix at $PREFIX"; exit 1; }

# 32-bit system dir: syswow64 in a 64-bit prefix, system32 in a 32-bit one
if [ -d "$PREFIX/drive_c/windows/syswow64" ]; then SYS="$PREFIX/drive_c/windows/syswow64"
else SYS="$PREFIX/drive_c/windows/system32"; fi

# Wine's own DLL is kept beside reAL's as <name>.wine (for winmm.dll, reAL's loads it from there).  It has to be
# this Wine's: a Wine upgrade leaves reAL's DLLs in place, and the copies kept would be the Wine before's.  So with
# reAL's in place already, it is removed first and Wine asked to put its own back (wineboot -u does that for a DLL
# that is missing); only if it does not is the copy kept before used again.  A copy kept that is a link (a prefix
# whose system DLLs are links into the Wine it uses) is this Wine's as it is, and goes back as the link it was.
is_real() {     # reAL's dsound.dll (any version names reAL.ini) or winmm.dll
    [ -f "$1" ] && grep -q -e 'reAL\.ini' -e reAL_winmm "$1"
}
there() { [ -e "$1" ] || [ -L "$1" ]; }
wine_back() {   # names: Wine's own DLLs in place again
    again=0
    for n in "$@"; do
        if is_real "$SYS/$n" || { [ -L "$SYS/$n" ] && [ ! -e "$SYS/$n" ]; }; then rm -f "$SYS/$n"; fi
        if [ -L "$SYS/$n.wine" ] && [ ! -e "$SYS/$n.wine" ]; then rm -f "$SYS/$n.wine"; fi   # (to a Wine gone)
        if ! there "$SYS/$n"; then
            if [ -L "$SYS/$n.wine" ]; then mv -f "$SYS/$n.wine" "$SYS/$n"; else again=1; fi
        fi
    done
    if [ $again = 1 ]; then
        wine wineboot -u >/dev/null 2>&1 || true
        wineserver -w 2>/dev/null || true
    fi
    for n in "$@"; do
        # (a copy kept that is reAL's: an older reAL's dsound.dll, which the script of 1.8.1 did not know for one)
        if there "$SYS/$n" || is_real "$SYS/$n.wine"; then rm -f "$SYS/$n.wine"
        elif there "$SYS/$n.wine"; then mv -f "$SYS/$n.wine" "$SYS/$n"; fi
    done
}
# reAL's DLL in the system directory, Wine's moved aside (moved, and the place written anew: reAL must not be
# written through a link; and written beside it first, so that the place is never empty for longer than a rename)
put() {         # reAL's file, name
    cp -f "$1" "$SYS/$2.reAL"
    if there "$SYS/$2"; then mv -f "$SYS/$2" "$SYS/$2.wine"; fi
    mv -f "$SYS/$2.reAL" "$SYS/$2"
}

command -v wine >/dev/null 2>&1 || { echo "no wine in PATH"; exit 1; }

# (reAL's winmm.dll for the whole prefix, from an earlier --winmm: looked after by every run from then on.  Wine's
# kept beside it says so too, should reAL's have gone missing)
WINMM_SYS=0
NO_WINMM=0
if is_real "$SYS/winmm.dll" || there "$SYS/winmm.dll.wine"; then WINMM_SYS=1; fi

if [ "$1" = "--uninstall" ]; then
    wine_back dsound.dll winmm.dll
    wine reg delete 'HKCU\Software\Wine\DllOverrides' /v dsound /f >/dev/null 2>&1 || true
    wine reg delete 'HKCU\Software\Wine\DllOverrides' /v winmm /f >/dev/null 2>&1 || true
    echo "reAL removed from $SYS (per-app overrides under AppDefaults are left; they now fall back to builtin)"
    exit 0
fi

WINMM="build/winmm.dll"
if [ -f winmm.dll ]; then WINMM="winmm.dll"; fi

[ -f "$DLL" ] || ./build.sh
if [ "$1" = "--winmm" ] && [ $# = 1 ]; then WINMM_SYS=1; fi
if [ $WINMM_SYS = 1 ]; then
    [ -f "$WINMM" ] || { echo "no $WINMM"; exit 1; }
    wine_back dsound.dll winmm.dll
    # (reAL's winmm.dll needs Wine's real one beside it: an old Wine keeps only a placeholder there)
    # (reAL's dsound.dll is installed all the same)
    if ! grep -q "Wine builtin DLL" "$SYS/winmm.dll" 2>/dev/null; then
        echo "this Wine keeps no winmm.dll of its own in $SYS (only a placeholder, or none): reAL's is not put"
        echo "there.  Use --winmm /path/to/game.exe, which puts it next to the game"
        WINMM_SYS=0; NO_WINMM=1
    fi
else
    wine_back dsound.dll
fi
put "$DLL" dsound.dll
echo "installed $DLL -> $SYS/dsound.dll"
if [ $WINMM_SYS = 1 ]; then
    put "$WINMM" winmm.dll
    echo "installed $WINMM -> $SYS/winmm.dll (Wine's: winmm.dll.wine)"
fi

if [ ! -f "$PREFIX/drive_c/windows/reAL.ini" ]; then
    cp reAL.ini.example "$PREFIX/drive_c/windows/reAL.ini"
    echo "config: $PREFIX/drive_c/windows/reAL.ini"
fi

if [ "$1" = "--winmm" ] && [ $# = 1 ]; then
    if [ $NO_WINMM = 1 ]; then      # (dsound.dll alone, as without --winmm)
        wine reg add 'HKCU\Software\Wine\DllOverrides' /v dsound /d native,builtin /f >/dev/null
        echo "override: global dsound=native,builtin"
        exit 1
    fi
    for dll in winmm dsound; do
        wine reg add 'HKCU\Software\Wine\DllOverrides' /v $dll /d native,builtin /f >/dev/null
    done
    echo "override: global winmm, dsound=native,builtin"
    exit 0
fi

if [ "$1" = "--winmm" ]; then
    [ -f "$WINMM" ] || { echo "no $WINMM"; exit 1; }
    shift
    for exe in "$@"; do
        [ -f "$exe" ] || { echo "no $exe"; exit 1; }
        cp -f "$WINMM" "$(dirname "$exe")/winmm.dll"
        name=$(basename "$exe")
        for dll in winmm dsound; do
            wine reg add "HKCU\\Software\\Wine\\AppDefaults\\$name\\DllOverrides" /v $dll /d native,builtin /f >/dev/null
        done
        echo "installed winmm.dll next to $exe; override: $name -> winmm, dsound=native,builtin"
    done
    exit 0
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
