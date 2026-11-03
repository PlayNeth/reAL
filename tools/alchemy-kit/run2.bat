@echo off
rem reAL / ALchemy comparison kit, second pass: ALchemy only, one seed per run (a crash loses one seed).
setlocal
cd /d "%~dp0"
if not exist "ALchemy\dsound.dll" (
    echo Copy ALchemy's dsound.dll into the ALchemy folder first, then run this again.
    pause
    exit /b 1
)
if exist results2 rmdir /s /q results2
if exist results2.zip del results2.zip
mkdir results2
set DSFUZZ_TIMEOUT=60
copy /y dsfuzz.exe ALchemy\ >nul
pushd ALchemy
echo Running against ALchemy, one seed at a time (an hour or more: each seed starts its own process; seeds that hang wait 60 seconds) ...
for /L %%S in (1,1,600) do (
    if %%S==1 echo   seed 1 of 600 ...
    if %%S==100 echo   seed 100 of 600 ...
    if %%S==200 echo   seed 200 of 600 ...
    if %%S==300 echo   seed 300 of 600 ...
    if %%S==400 echo   seed 400 of 600 ...
    if %%S==500 echo   seed 500 of 600 ...
    dsfuzz.exe %%S 1 200 >> "..\results2\ALchemy_ds.txt" 2>nul
    dsfuzz.exe %%S 1 200 eax >> "..\results2\ALchemy_eax.txt" 2>nul
)
popd
echo.
echo Packing results2.zip ...
tar -a -c -f results2.zip results2 >nul 2>nul
if exist results2.zip (
    echo Done. Send results2.zip.
) else (
    echo Done. Zip the results2 folder and send it.
)
pause
exit /b 0
