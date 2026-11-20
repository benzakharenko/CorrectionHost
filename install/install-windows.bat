@echo off
rem Double-click to install the latest Correction Host release (it then keeps itself updated).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-windows.ps1"
if errorlevel 1 (echo Install failed. & pause & exit /b 1)
timeout /t 8 >nul
