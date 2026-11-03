@echo off
rem reAL / ALchemy comparison kit, third pass: alprobe.exe, DirectSound and EAX case by case, under ALchemy.
setlocal
cd /d "%~dp0"
if not exist "ALchemy\dsound.dll" (
    echo Copy ALchemy's dsound.dll into the ALchemy folder first, then run this again.
    pause
    exit /b 1
)
if exist results3 rmdir /s /q results3
if exist results3.zip del results3.zip
mkdir results3
set ALPROBE_TIMEOUT=120
copy /y alprobe.exe ALchemy\ >nul
pushd ALchemy
for %%S in (create ctrl lock play notify misc d3 eax eaxval) do (
    echo   %%S ...
    alprobe.exe %%S > "..\results3\%%S.txt" 2>nul
)
popd
echo.
echo Packing results3.zip ...
tar -a -c -f results3.zip results3 >nul 2>nul
if exist results3.zip (
    echo Done. Send results3.zip.
) else (
    echo Done. Zip the results3 folder and send it.
)
pause
exit /b 0
