#!/bin/bash
# Installs (or reinstalls) the latest Correction Host release.
# After this, the app keeps itself up to date from GitHub.
#
# Run in Terminal:
#   curl -fsSL https://raw.githubusercontent.com/benzakharenko/CorrectionHost/main/install/install-mac.sh | bash
set -euo pipefail

REPO="${CORRECTIONHOST_REPO:-benzakharenko/CorrectionHost}"

# /Applications if we can write there (admin users can), otherwise ~/Applications.
DEST="/Applications"
if [ ! -w "$DEST" ]; then
    DEST="$HOME/Applications"
    mkdir -p "$DEST"
fi
APP="$DEST/Correction Host.app"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "Downloading the latest Correction Host from github.com/$REPO ..."
curl -fL --progress-bar -o "$TMP/app.zip" "https://github.com/$REPO/releases/latest/download/CorrectionHost-macos.zip"
ditto -x -k "$TMP/app.zip" "$TMP/unpacked"

if pgrep -x "Correction Host" > /dev/null; then
    echo "Closing the running copy..."
    osascript -e 'quit app "Correction Host"' 2> /dev/null || pkill -x "Correction Host" || true
    sleep 2
fi

rm -rf "$APP"
ditto "$TMP/unpacked/Correction Host.app" "$APP"
xattr -dr com.apple.quarantine "$APP" 2> /dev/null || true

echo "Installed to $APP"
echo "Starting Correction Host. Allow the microphone prompt: macOS treats BlackHole capture as microphone access."
echo "Tip: add it under System Settings > General > Login Items so it starts at login."
open "$APP"
