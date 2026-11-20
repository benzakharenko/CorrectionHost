# Installs (or reinstalls) the latest Correction Host release for the current user.
# After this, the app keeps itself up to date from GitHub.
#
# Run from PowerShell:
#   irm https://raw.githubusercontent.com/benzakharenko/CorrectionHost/main/install/install-windows.ps1 | iex
#
# To install a specific build instead of the latest (e.g. to roll back), set
# CORRECTIONHOST_TAG first, e.g.  $env:CORRECTIONHOST_TAG = 'build-3'
# (or run install-windows.bat 3). The app then updates from there as usual.

$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'SilentlyContinue'   # makes Invoke-WebRequest much faster

$repo = if ($env:CORRECTIONHOST_REPO) { $env:CORRECTIONHOST_REPO } else { 'benzakharenko/CorrectionHost' }
$dir  = Join-Path $env:LOCALAPPDATA 'Programs\Correction Host'
$exe  = Join-Path $dir 'Correction Host.exe'
$zip  = Join-Path $env:TEMP 'CorrectionHost-windows.zip'

$url = if ($env:CORRECTIONHOST_TAG) { "https://github.com/$repo/releases/download/$($env:CORRECTIONHOST_TAG)/CorrectionHost-windows.zip" }
       else { "https://github.com/$repo/releases/latest/download/CorrectionHost-windows.zip" }
Write-Host "Downloading Correction Host ($(if ($env:CORRECTIONHOST_TAG) { $env:CORRECTIONHOST_TAG } else { 'latest' })) from github.com/$repo ..."
Invoke-WebRequest $url -OutFile $zip -UseBasicParsing

$running = Get-Process -Name 'Correction Host' -ErrorAction SilentlyContinue
if ($running) {
    Write-Host 'Closing the running copy...'
    $running | Stop-Process -Force
    Start-Sleep -Seconds 2
}

New-Item -ItemType Directory -Force -Path $dir | Out-Null
Expand-Archive -Path $zip -DestinationPath $dir -Force
Remove-Item $zip -ErrorAction SilentlyContinue
Unblock-File -Path $exe

# Start menu shortcut (replaces any older one, e.g. from a local build).
$lnk = Join-Path ([Environment]::GetFolderPath('Programs')) 'Correction Host.lnk'
$shell = New-Object -ComObject WScript.Shell
$s = $shell.CreateShortcut($lnk)
$s.TargetPath       = $exe
$s.WorkingDirectory = $dir
$s.IconLocation     = "$exe,0"
$s.Description      = 'Room correction for your second pair of speakers'
$s.Save()

Write-Host "Installed to $dir"
Write-Host 'Starting Correction Host. Your settings and plugin profile are kept.'
Write-Host 'Tip: tick "Start with Windows" in the app so it runs after every restart.'
Start-Process -FilePath $exe
