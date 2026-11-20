#!/bin/bash
# Builds Correction Host for macOS as a universal app (Apple silicon + Intel), so it
# runs on any Mac you copy it to, not just the one that compiled it.
#
#   ./build-mac.sh              build
#   ./build-mac.sh --install    build, then copy the app to /Applications
#
# Needs: Xcode command line tools (xcode-select --install), CMake 3.22+, Git.
set -euo pipefail
cd "$(dirname "$0")"

LOG="$PWD/build-mac-log.txt"
BUILD_DIR="build-mac"
APP="$BUILD_DIR/CorrectionHost_artefacts/Release/Correction Host.app"
ZIP="$PWD/Correction Host (macOS).zip"
ARCHS="${ARCHS:-arm64;x86_64}"

# Homebrew and the CMake.app bundle aren't always on PATH in a plain SSH session.
export PATH="/opt/homebrew/bin:/usr/local/bin:/Applications/CMake.app/Contents/bin:$PATH"

fail() { echo "BUILD FAILED: $1" | tee -a "$LOG"; exit 1; }

echo "=== Correction Host macOS build $(date) ===" > "$LOG"

xcode-select -p >/dev/null 2>&1 || fail "Xcode command line tools are missing. Run: xcode-select --install"
command -v cmake >/dev/null 2>&1 || fail "CMake not found. Install it with: brew install cmake  (or from cmake.org)"
command -v git   >/dev/null 2>&1 || fail "Git not found (it comes with the Xcode command line tools)."
{ cmake --version; git --version; xcrun --show-sdk-version; uname -m; } >> "$LOG" 2>&1

JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

echo "[1/3] Configuring (downloads JUCE the first time)..."
cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release "-DCMAKE_OSX_ARCHITECTURES=$ARCHS" >> "$LOG" 2>&1 \
    || fail "configure step - see build-mac-log.txt"

echo "[2/3] Compiling ($ARCHS, $JOBS jobs)..."
cmake --build "$BUILD_DIR" --config Release --target CorrectionHost -j "$JOBS" >> "$LOG" 2>&1 \
    || fail "compile step - see build-mac-log.txt"

[ -d "$APP" ] || fail "the build finished but $APP is missing"

echo "[3/3] Signing and packaging..."
# Ad-hoc signature: required to run on Apple silicon, and keeps the microphone
# permission stable for this copy of the app.
codesign --force --deep --sign - "$APP" >> "$LOG" 2>&1 || fail "codesign"
lipo -info "$APP/Contents/MacOS/Correction Host" | tee -a "$LOG"

rm -f "$ZIP"
ditto -c -k --keepParent "$APP" "$ZIP"

if [ "${1:-}" = "--install" ]; then
    rm -rf "/Applications/Correction Host.app"
    ditto "$APP" "/Applications/Correction Host.app"
    echo "Installed to /Applications/Correction Host.app" | tee -a "$LOG"
fi

echo "BUILD SUCCEEDED" >> "$LOG"
echo
echo "Build succeeded."
echo "  App: $PWD/$APP"
echo "  Zip: $ZIP   (copy this to the Mac you'll use it on)"
echo
echo "The app isn't notarised, so the first time you open a copied app macOS will block it:"
echo "open it once, then go to System Settings > Privacy & Security and click 'Open Anyway'."
