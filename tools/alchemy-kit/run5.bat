@echo off
rem reAL / ALchemy comparison kit, fifth pass: levelprobe.exe, how loud a tone comes out on each channel, 2D and 3D, measured from what
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
echo Leave the volume up and nothing else playing: a tone plays for about half a minute, three times.
if exist results5 rmdir /s /q results5
if exist results5.zip del results5.zip
mkdir results5
if not exist Windows mkdir Windows
if exist Windows\dsound.dll del Windows\dsound.dll
copy /y levelprobe.exe ALchemy\ >nul
copy /y levelprobe.exe reAL\ >nul
copy /y levelprobe.exe Windows\ >nul
for %%D in (ALchemy reAL Windows) do (
    echo   %%D ...
    pushd %%D
    levelprobe.exe > "..\results5\%%D.txt" 2>nul
    popd
)
echo.
echo Packing results5.zip ...
tar -a -c -f results5.zip results5 >nul 2>nul
if exist results5.zip (
    echo Done. Send results5.zip.
) else (
    echo Done. Zip the results5 folder and send it.
)
pause
exit /b 0
