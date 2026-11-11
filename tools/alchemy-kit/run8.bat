@echo off
rem reAL / ALchemy comparison kit, eighth pass: levelprobe.exe noise (each band's level at every angle), applyprobe.exe cross (which calls apply a deferred 3D change), and the noise again after asking DirectSound for headphones, from what
rem the speakers get, under ALchemy, under reAL and under Windows' own DirectSound.
setlocal
cd /d "%~dp0"
if not exist "ALchemy\dsound.dll" (
    echo Copy ALchemy's dsound.dll into the ALchemy folder first, then run this again.
    pause
    exit /b 1
)
if not exist "reAL\dsound.dll" (
    echo reAL\dsound.dll is missing: unpack the whole kit again.
    pause
    exit /b 1
)
echo Leave the volume up and nothing else playing: noise and a tone play for about two and a half minutes, three times.
if exist results8 rmdir /s /q results8
if exist results8.zip del results8.zip
mkdir results8
if not exist Windows mkdir Windows
if exist Windows\dsound.dll del Windows\dsound.dll
copy /y levelprobe.exe ALchemy\ >nul
copy /y levelprobe.exe reAL\ >nul
copy /y levelprobe.exe Windows\ >nul
copy /y applyprobe.exe ALchemy\ >nul
copy /y applyprobe.exe reAL\ >nul
copy /y applyprobe.exe Windows\ >nul
for %%D in (ALchemy reAL Windows) do (
    echo   %%D ...
    pushd %%D
    levelprobe.exe noise > "..\results8\%%D.txt" 2>nul
    applyprobe.exe cross > "..\results8\%%D-cross.txt" 2>nul
    levelprobe.exe noise headphones > "..\results8\%%D-headphones.txt" 2>nul
    popd
)
echo.
echo Packing results8.zip ...
tar -a -c -f results8.zip results8 >nul 2>nul
if exist results8.zip (
    echo Done. Send results8.zip.
) else (
    echo Done. Zip the results8 folder and send it.
)
pause
exit /b 0
