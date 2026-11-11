@echo off
rem reAL / ALchemy comparison kit, seventh pass: levelprobe.exe near, how loud pink noise comes out from a 3D buffer at the listener's own position, inside the minimum distance and from a stereo 3D buffer (then applyprobe.exe cross: what else applies a deferred 3D change), from what
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
echo Leave the volume up and nothing else playing: noise plays for about forty seconds, then a tone for about thirty, three times.
if exist results7 rmdir /s /q results7
if exist results7.zip del results7.zip
mkdir results7
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
    levelprobe.exe near > "..\results7\%%D.txt" 2>nul
    applyprobe.exe cross > "..\results7\%%D-cross.txt" 2>nul
    popd
)
echo.
echo Packing results7.zip ...
tar -a -c -f results7.zip results7 >nul 2>nul
if exist results7.zip (
    echo Done. Send results7.zip.
) else (
    echo Done. Zip the results7 folder and send it.
)
pause
exit /b 0
