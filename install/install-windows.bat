@echo off
rem Double-click to install the latest Correction Host release (it then keeps itself updated).
rem Optional: pass a build number to install that build, e.g. install-windows.bat 3
if not "%~1"=="" set "CORRECTIONHOST_TAG=build-%~1"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-windows.ps1"
if errorlevel 1 (echo Install failed. & pause & exit /b 1)
timeout /t 8 >nul
