@echo off
rem reAL / ALchemy comparison kit, fourth pass: applyprobe.exe, what each 3D dwApply value does, measured from what
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
echo Leave the volume up and nothing else playing: a tone plays for about a minute, three times.
if exist results4 rmdir /s /q results4
if exist results4.zip del results4.zip
mkdir results4
if not exist Windows mkdir Windows
if exist Windows\dsound.dll del Windows\dsound.dll
copy /y applyprobe.exe ALchemy\ >nul
copy /y applyprobe.exe reAL\ >nul
copy /y applyprobe.exe Windows\ >nul
for %%D in (ALchemy reAL Windows) do (
    echo   %%D ...
    pushd %%D
    applyprobe.exe > "..\results4\%%D.txt" 2>nul
    popd
)
echo.
echo Packing results4.zip ...
tar -a -c -f results4.zip results4 >nul 2>nul
if exist results4.zip (
    echo Done. Send results4.zip.
) else (
    echo Done. Zip the results4 folder and send it.
)
pause
exit /b 0
