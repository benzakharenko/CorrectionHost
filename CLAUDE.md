# Correction Host: notes for Claude Code

A small cross-platform tray app (JUCE 9, C++17, CMake) that takes a copy of the
computer's audio, runs it through a room-correction plugin (IK Multimedia ARC X, VST3
or AU), and plays the result on a *second* pair of speakers. The main monitors keep
getting the untouched signal.

Personal machine and studio details live in `CLAUDE.local.md` (git-ignored, not in
the public repo). Read it if it exists.

## Ground rules

- **The repo is public.** Never commit personal paths, machine names, Tailscale
  addresses, usernames, credentials or signing keys. Put those in `CLAUDE.local.md`.
- **Every push to `main` ships to every machine.** The release workflow builds Windows
  and macOS, publishes `build-N`, and installed copies update themselves when the audio
  has been quiet for 2 minutes. Before pushing app code:
  1. build locally,
  2. run the tests,
  3. launch the app and check the status box.
  Use a branch for anything experimental. Markdown-only changes don't trigger a release.
- Keep the main-monitor path untouched. On Windows the design depends on WASAPI
  **loopback**: nothing is inserted between apps and the main outputs.

## Build

- **Windows:** `build.bat`, or:
  ```
  cmake -B build -A x64
  cmake --build build --config Release --target CorrectionHost
  ```
  Output: `build\CorrectionHost_artefacts\Release\Correction Host.exe`. A running exe is
  locked; `build.bat` renames it aside first. Log: `build-log.txt`.
- **macOS:** `./build-mac.sh [--install]` builds a universal app (arm64 + x86_64) into
  `build-mac/` and writes `build-mac-log.txt`.
- **Linux:** used only for tests and UI checks under Xvfb. MinGW is not supported by
  JUCE, so Windows code can only be compiled on Windows (or in CI).
- JUCE 9.0.6 is fetched by CMake (FetchContent). `-DCORRECTIONHOST_JUCE_DIR=<path>`
  uses a local checkout instead.
- Local builds have build number 0 and **never self-update**. Only CI builds get
  `CORRECTIONHOST_BUILD_NUMBER` / `CORRECTIONHOST_UPDATE_REPO`.

## Tests

`-DCORRECTIONHOST_BUILD_TESTS=ON` builds these headless programs:

- `DriftFifoTest`: clock drift, jitter, dropouts and THD+N of the resampling FIFO.
- `EngineTest <path to TestGain.vst3>`: hosts a real VST3 through the real audio
  callback with a fake device. Build `TestGainPlugin_VST3` first.
- `LoopbackGlueTest`: non-Windows only; runs the Windows loopback adapter against a
  stub.
- `UpdaterTest`: release-JSON parsing, build numbers, waiting for the previous process.
- `IconRenderTest <dir>`: writes the tray logo PNGs for checking by eye.

## Architecture

| File | Role |
|---|---|
| `Source/DriftFifo.h` | SPSC FIFO between two clock domains. Windowed-sinc resampler (gain compensated) steered by a critically damped PI loop on a timestamp-based fill estimate. Seqlock stats; stall and resync detection. |
| `Source/CaptureSource.h` | Interface for "where the audio comes from". |
| `Source/WasapiLoopback.*` | Raw WASAPI loopback (plain C++, no JUCE), MMCSS "Pro Audio", event-free 1 ms polling. |
| `Source/LoopbackCaptureSource.*` | JUCE adapter for loopback (Windows default capture mode). |
| `Source/JuceDeviceCaptureSource.*` | Input-device capture. On macOS this is BlackHole; on Windows it's VB-Cable and the optional shared-mode interface input. |
| `Source/HostEngine.*` | Opens the output device **directly** (`createDevice` + `open` at the device's *current* rate, no AudioDeviceManager), hosts the plugin, handles wet/dry bypass crossfade, mixes the optional interface input, runs a 1 s watchdog with auto restart, and tracks `getSecondsSinceAudible()`. |
| `Source/MainComponent.*` | UI and settings (PropertiesFile, XML). Start with Windows uses the HKCU Run key. |
| `Source/Main.cpp` | App, window (minimise/close to tray), tray icon (grey when bypassed), single instance, updater wiring. |
| `Source/Updater.*` | Self-update from GitHub releases. Details below. |
| `Source/AppIcon.h` | Logo drawn in code for the tray at the exact DPI size; template version for the macOS menu bar. |
| `Resources/` | App icon PNGs (`icon_windows_1024`, `icon_mac_1024` with Big Sur padding, `icon_small_256`) and their SVG sources. |
| `cmake/PatchJuceAsio.cmake` | Patches JUCE's ASIO reset handler to reopen at the *driver's* current rate. Without it, JUCE forces the interface back to its old rate (it was forcing 44.1 kHz). Idempotent; applied at configure time. |

### Things that were learned the hard way

- **Sample rate:** never let JUCE choose it. The output device opens at
  `getCurrentSampleRate()`, so the app follows the interface (48 kHz in practice).
  AudioDeviceManager's saved and default setups forced 44.1 kHz.
- **ASIO:** this only works alongside a DAW if the driver is multi-client. When ASIO is
  the output, the interface input comes *directly* in the same callback (same clock,
  no FIFO), which is how ADAT inputs are reached when the driver doesn't expose them to
  Windows.
- **Feedback guard:** refuse a capture source that is the same endpoint as the
  output.
- **JUCE 9 API changes:**
  - `createEditorAndMakeActive()` (not `createEditorIfNeeded`)
  - `exactlyEqual` for float comparison
  - `addDefaultFormatsToManager`
  - tests that pump the message loop need `JUCE_MODAL_LOOPS_PERMITTED=1`
- **Tray:**
  - Windows 11 hides new tray icons in the overflow until the user enables them.
  - Minimising is caught with `minimisationStateChanged`. The native title bar
    doesn't call `minimiseButtonPressed`.

## Release and update flow

`.github/workflows/release.yml`, on push to `main` (ignores `**.md`):

1. **windows-latest:** builds with `-DCORRECTIONHOST_BUILD_NUMBER=${{ github.run_number }}`
   and `-DCORRECTIONHOST_UPDATE_REPO=${{ github.repository }}`, then zips the exe into
   `CorrectionHost-windows.zip`. The C++ runtime is linked statically, so no
   redistributable is needed.
2. **macos-latest:** builds universal and signs with the "Correction Host Signing"
   identity from secrets (`MACOS_SIGNING_P12`, `MACOS_SIGNING_PASSWORD`), or ad hoc if
   they're absent. It zips the app with `ditto` into `CorrectionHost-macos.zip`. See
   `packaging/README.md` for why a stable identity matters (the microphone permission
   survives updates).
3. **ubuntu:** creates release `build-N` with both zips and marks it latest.

The app (`Updater`) checks `releases/latest` 45 s after launch and every 6 h. It also
checks from the tray's "Check for updates", and when the app is opened again while
running if the last check was over 10 min ago:

1. Downloads the platform zip and verifies GitHub's SHA-256 asset digest.
2. Unpacks it (`ZipFile` on Windows, `ditto` on macOS).
3. Once the output has been silent for 120 s, swaps the app in place:
   - **Windows:** renames the running exe to `Correction Host.old-N.exe`, then copies
     the new one in.
   - **macOS:** moves the bundle to `Correction Host (previous version).app`, then moves
     the new one in.
4. Relaunches with `--updated-from=<pid>` (plus `--minimised` if the window was hidden)
   and quits. The new process waits for the old pid to exit inside
   `moreThanOneInstanceAllowed()`, so it inherits the single-instance lock and the audio
   devices.
5. Leftovers are deleted at the next startup.

Every step is logged to `%APPDATA%\CorrectionHost\updater.log` (Windows) or
`~/Library/Logs/CorrectionHost/updater.log` (macOS), including HTTP status codes.
Check there first when a machine doesn't update.

Installers (first install only): `install/install-windows.ps1` (or the `.bat`
wrapper) installs to `%LOCALAPPDATA%\Programs\Correction Host` and adds a Start menu
shortcut. `install/install-mac.sh` installs to `/Applications` (or `~/Applications`).
Both take `CORRECTIONHOST_TAG=build-N` to install a specific build, which is how you
roll back (`install-windows.bat N`). The app then carries on updating to newer builds,
so to stay rolled back, revert on `main` instead.
Settings live in the per-user app-data folder and are shared by all copies on a
machine.
