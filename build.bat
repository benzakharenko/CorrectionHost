@echo off
setlocal EnableExtensions
cd /d "%~dp0"
set "LOG=%~dp0build-log.txt"

echo Building Correction Host. This takes several minutes the first time
echo (it downloads JUCE). Progress is written to build-log.txt.
echo.

echo === Correction Host build %DATE% %TIME% === > "%LOG%"

rem Make sure CMake and Git are reachable even if they're not on PATH.
where cmake >nul 2>nul || set "PATH=%ProgramFiles%\CMake\bin;%PATH%"
where git   >nul 2>nul || set "PATH=%ProgramFiles%\Git\cmd;%PATH%"

echo --- tools --- >> "%LOG%"
cmake --version >> "%LOG%" 2>&1
if errorlevel 1 (
    echo CMake was not found. >> "%LOG%"
    goto failed
)
git --version >> "%LOG%" 2>&1
"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -all -products * -format text -property displayName >> "%LOG%" 2>&1

echo [1/2] Configuring...
echo --- configure --- >> "%LOG%"
cmake -B build -A x64 >> "%LOG%" 2>&1
if errorlevel 1 goto failed

rem A running copy locks its .exe. Windows lets us rename it, so move it aside
rem (the running copy keeps working; restart it to use the new build).
set "OUTDIR=%~dp0build\CorrectionHost_artefacts\Release"
del /q "%OUTDIR%\Correction Host.old-*.exe" >nul 2>nul
if exist "%OUTDIR%\Correction Host.exe" del /q "%OUTDIR%\Correction Host.exe" >nul 2>nul
if exist "%OUTDIR%\Correction Host.exe" (
    ren "%OUTDIR%\Correction Host.exe" "Correction Host.old-%RANDOM%.exe"
    echo Moved the running copy aside - restart Correction Host after the build. >> "%LOG%"
    echo NOTE: Correction Host is running. Restart it after the build to use the new version.
)

echo [2/2] Compiling...
echo --- build --- >> "%LOG%"
cmake --build build --config Release --target CorrectionHost --parallel >> "%LOG%" 2>&1
if errorlevel 1 goto failed

echo BUILD SUCCEEDED >> "%LOG%"

echo.
echo Build succeeded. This is a local test build: it doesn't update itself.
echo Your installed copy (Start menu) keeps updating from GitHub as usual.
echo Opening the folder with "Correction Host.exe"...
start "" explorer "%~dp0build\CorrectionHost_artefacts\Release"
timeout /t 5 >nul
exit /b 0

:failed
echo BUILD FAILED >> "%LOG%"
echo.
echo Build failed - see build-log.txt
timeout /t 15 >nul
exit /b 1
