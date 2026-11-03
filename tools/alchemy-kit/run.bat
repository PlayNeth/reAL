@echo off
rem reAL / ALchemy comparison kit: runs dsfuzz.exe under ALchemy's dsound.dll and under reAL's, writes results.zip.
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
if exist results rmdir /s /q results
if exist results.zip del results.zip
mkdir results
set DSFUZZ_TIMEOUT=300

echo Collecting system information ...
ver > results\system.txt
powershell -NoProfile -Command "Get-CimInstance Win32_SoundDevice | Format-Table -AutoSize Name,Manufacturer; Get-CimInstance Win32_OperatingSystem | Format-List Caption,Version,OSArchitecture" >> results\system.txt 2>nul
powershell -NoProfile -Command "(Get-Item 'ALchemy\dsound.dll').VersionInfo | Format-List FileVersion,ProductName; Get-FileHash 'ALchemy\dsound.dll' -Algorithm SHA1 | Format-List Hash" >> results\system.txt 2>nul

call :side ALchemy
call :side reAL

echo.
echo Packing results.zip ...
tar -a -c -f results.zip results >nul 2>nul
if exist results.zip (
    echo Done. Send results.zip.
) else (
    echo Done. Zip the results folder and send it.
)
pause
exit /b 0

:side
echo.
echo Running against %1 (about 5 minutes; windows may stay blank, that's normal) ...
copy /y dsfuzz.exe "%1\" >nul
pushd "%1"
set DSFUZZ_WHICH=1
dsfuzz.exe formats > "..\results\%1_formats.txt" 2>nul
set DSFUZZ_WHICH=
for /L %%F in (1,25,576) do (
    echo   seeds %%F-...
    dsfuzz.exe %%F 25 200 >> "..\results\%1_ds.txt" 2>nul
    dsfuzz.exe %%F 25 200 eax >> "..\results\%1_eax.txt" 2>nul
)
popd
exit /b 0
