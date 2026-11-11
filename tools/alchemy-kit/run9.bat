@echo off
rem reAL / ALchemy comparison kit, ninth pass: levelprobe.exe clicks (the response to one sample from each direction, written to a file) and the noise again after asking DirectSound for headphones, from what
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
echo Leave the volume up and nothing else playing: clicks and noise play for about two minutes, three times.
if exist results9 rmdir /s /q results9
if exist results9.zip del results9.zip
mkdir results9
if not exist Windows mkdir Windows
if exist Windows\dsound.dll del Windows\dsound.dll
copy /y levelprobe.exe ALchemy\ >nul
copy /y levelprobe.exe reAL\ >nul
copy /y levelprobe.exe Windows\ >nul
for %%D in (ALchemy reAL Windows) do (
    echo   %%D ...
    pushd %%D
    levelprobe.exe clicks "..\results9\%%D.f32" > "..\results9\%%D-clicks.txt" 2>nul
    levelprobe.exe noise headphones > "..\results9\%%D-headphones.txt" 2>nul
    popd
)
echo.
echo Packing results9.zip ...
tar -a -c -f results9.zip results9 >nul 2>nul
if exist results9.zip (
    echo Done. Send results9.zip.
) else (
    echo Done. Zip the results9 folder and send it.
)
pause
exit /b 0
