@echo off
rem reAL / ALchemy comparison kit, sixth pass: levelprobe.exe noise, how loud pink noise comes out on each channel and band, 2D and 3D at many angles, from what
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
echo Leave the volume up and nothing else playing: noise plays for about forty seconds, three times.
if exist results6 rmdir /s /q results6
if exist results6.zip del results6.zip
mkdir results6
if not exist Windows mkdir Windows
if exist Windows\dsound.dll del Windows\dsound.dll
copy /y levelprobe.exe ALchemy\ >nul
copy /y levelprobe.exe reAL\ >nul
copy /y levelprobe.exe Windows\ >nul
for %%D in (ALchemy reAL Windows) do (
    echo   %%D ...
    pushd %%D
    levelprobe.exe noise > "..\results6\%%D.txt" 2>nul
    popd
)
echo.
echo Packing results6.zip ...
tar -a -c -f results6.zip results6 >nul 2>nul
if exist results6.zip (
    echo Done. Send results6.zip.
) else (
    echo Done. Zip the results6 folder and send it.
)
pause
exit /b 0
