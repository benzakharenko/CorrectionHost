# Correction Host

A small tray app that sends a **room-corrected copy** of your computer's audio to a
second pair of speakers, while your main monitors keep playing the original,
untouched.

```
Windows:
  apps ──► Windows mixer ──► interface outs 1-2 ──► main monitors   (unchanged)
                     │
                     └─ WASAPI loopback copy ──► Correction Host ─┬─► ARC X ──► interface outs 3-4 ──► D3Vs
  interface input (optional, e.g. ADAT 9/10) ──────────────────────┘

macOS:
  apps ──► Multi-Output Device ─┬─► interface outs 1-2 ──► main monitors   (unchanged)
                                └─► BlackHole ──► Correction Host ──► ARC X ──► interface outs 3-4 ──► second pair
```

It hosts any VST3 (and AU on macOS) effect. It's built for IK Multimedia's ARC X,
but anything works.

What it handles for you:

- **Clock drift.** The captured stream and the output run on separate clocks. A
  drift-tracking resampler keeps latency constant without clicks. It uses
  windowed-sinc resampling, measured at −108 dB THD+N at 1 kHz and −96 dB at 15 kHz
  in simulation, which is below the 16-bit noise floor.
- **Devices coming and going.** A watchdog restarts audio automatically after
  unplugging, sleep and wake, or Windows changing a device's format.
- **Click-free bypass and plugin swaps,** using a 20 ms crossfade.
- **Remembers everything,** including the plugin's state (your ARC X profile).
  State is saved every 15 s, when you close the plugin window, and on quit.
- **Feedback guard.** It refuses to output to the same device and channels it's
  capturing.
- **Optional interface input.** It can mix in an input pair from your interface
  (e.g. ADAT 9/10) as well, with its own drift tracking and a level control. The
  input can be switched on, off or changed while audio keeps running, and if it
  disappears, it retries without interrupting the main path.

- **Keeps itself up to date.** Every change pushed to `main` is built for Windows
  and macOS automatically, and installed copies update themselves. They wait until
  nothing has played for 2 minutes, so an update never interrupts listening.

---

## 0. Install (no build tools needed)

**Windows:** in PowerShell:

```powershell
irm https://raw.githubusercontent.com/benzakharenko/CorrectionHost/main/install/install-windows.ps1 | iex
```

Or download the repo and double-click `install\install-windows.bat`. It installs to
`%LOCALAPPDATA%\Programs\Correction Host` and adds a Start menu shortcut.

**macOS:** in Terminal:

```bash
curl -fsSL https://raw.githubusercontent.com/benzakharenko/CorrectionHost/main/install/install-mac.sh | bash
```

This installs to `/Applications`.

**Updates:**
- The installed app checks GitHub 45 seconds after starting and every 6 hours after
  that. You can also check from the tray menu with **Check for updates**.
- It downloads a new version in the background and checks its checksum.
- It installs once the output has been silent for 2 minutes, then restarts with the
  same settings and plugin.
- The **Version** line in the status box shows what it's doing.

Copies you build yourself (below) never update themselves.

---

## 1. Build it

You need **CMake 3.22+** and **Git**, plus:

- **Windows:** Visual Studio 2022 (Community is fine) with the *Desktop
  development with C++* workload.
- **macOS:** Xcode (or the Command Line Tools).

**Easiest:** double-click `build.bat` on Windows, or run `./build-mac.sh` on a Mac.
`build.bat` also adds a *Correction Host* shortcut to the Start menu. `build-mac.sh`
makes a universal app (Apple silicon and Intel), signs it ad hoc, and zips it as
`Correction Host (macOS).zip` so you can copy it to another Mac. Add `--install` to
copy it into `/Applications` as well.

By hand, from a terminal in this folder:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target CorrectionHost
```

The first configure downloads JUCE 9.0.6, which takes a minute. The app ends up in:

- Windows: `build\CorrectionHost_artefacts\Release\Correction Host.exe`
- macOS: `build/CorrectionHost_artefacts/Release/Correction Host.app`

> **macOS architecture:** a plugin only loads into a host of the same
> architecture. `build-mac.sh` builds a universal app, which runs natively on
> either kind of Mac and loads Universal plugins such as current ARC X. For an
> Intel-only plugin on Apple silicon, run the app under Rosetta (*Get Info → Open
> using Rosetta*), or build with `ARCHS=x86_64 ./build-mac.sh`.

---

## 2. Windows setup

1. **Set both of your interface's playback devices to 48 kHz.** Go to *Sound settings → More
   sound settings → Playback*, then *Properties → Advanced* for each. Choose
   "24 bit, 48000 Hz" and untick *Enable audio enhancements* if you see it.
2. **Leave Windows' default output on your main monitors.** The loopback copy
   doesn't change what they play.
3. **Run Correction Host** and set:
   - **Capture:** `Loopback (copy of a playback device)`
   - **From:** the device your main monitors play from (it defaults to the Windows
     default), **Channels:** `1-2`
   - **Driver:** `Windows Audio` (WASAPI shared mode, so your DAW can keep using
     ASIO at the same time)
   - **To / Channels**, depending on how your interface's driver shows its outputs:
     - *One multichannel device* (e.g. "Speakers (Focusrite USB Audio)" with
       4+ channels): pick it and **Channels `3-4`**.
     - *A separate device per output pair* (e.g. "Speakers 3-4 (…)"): pick that
       device and **Channels `1-2`**.

     If you pick channels the device doesn't have, the status box tells you which
     case you're in.
4. **Plugin:** choose ARC X. Its window opens; load the profile you measured for
   the D3Vs.
5. Optional, **interface input:** tick *Also mix in an input from your interface*,
   pick the channel pair, and set its **Level**. Double-click the slider to reset it
   to 0 dB.

### Using ASIO (recommended if your driver allows several apps at once)

Many interfaces (the SSL 18 included) only show some channels to Windows. Their
ADAT channels, for example, are available through ASIO only. With **Driver: ASIO**:

- **Channel names:** the output and input channel menus list every channel by the
  driver's names (e.g. "9-10   ADAT 1 / ADAT 2").
- **Interface input:** it comes straight from the same ASIO device, so it shares
  the output's clock. There's no drift correction and the lowest possible latency.
- **Windows audio:** still arrives through loopback as before.
- **Sample rate:** Correction Host never changes the device's sample rate. It runs
  at whatever your DAW and driver are set to.

This only works alongside your DAW if the interface's ASIO driver is **multi-client**.
To check, play audio from two ASIO apps at once (e.g. REAPER and Ableton). If your
driver is single-client, use *Windows Audio* instead.
6. Optional: tick **Start with Windows**. It launches minimised to the tray.

Closing or minimising the window keeps it running in the tray. Each has its own
tickbox at the bottom of the window. Left-click the tray icon to show the window;
right-click for **Bypass** and **Quit**. The icon turns grey while correction is
bypassed.

**Windows 11 hides new tray icons in the ^ overflow.** To keep it visible, go to
*Settings → Personalization → Taskbar → Other system tray icons* and turn on
**Correction Host**. You can also drag it out of the overflow onto the taskbar.

### Things to know on Windows

- **Your DAW's audio is not captured.** A DAW using ASIO talks to the interface
  directly and never passes through the Windows mixer that loopback copies. That's
  the point of the design: the DAW path stays untouched. To hear a DAW session on the
  D3Vs, route it to outs 3-4 in the DAW, with ARC X on that output. A monitoring-only
  slot (REAPER's Monitoring FX, Cubase's Control Room) is best, so it never ends up
  in a bounce.
- **"Waiting for audio (nothing is playing)" is normal.** Windows stops delivering
  loopback data when nothing is playing to that device. Playback resumes
  automatically.
- **The D3Vs play slightly behind the main monitors.** The status box shows the
  exact figure, typically 30–50 ms plus any plugin latency. That doesn't matter if
  you listen to one pair at a time.
- **Exclusive mode:** if another app grabs the D3V output in WASAPI exclusive mode,
  Correction Host can't play until that app lets go. It retries automatically.
- **The interface input is read in WASAPI shared mode,** so your DAW can keep
  using the same inputs over ASIO. Don't pick an input that already carries the
  D3V outputs (for example, an interface loopback/mixer channel fed from outs 3-4),
  or you'll create a feedback loop.
- **Windows privacy settings:** reading an interface input counts as microphone
  access. If the input shows an error, check *Settings → Privacy & security →
  Microphone → Let desktop apps access your microphone*.

---

## 3. macOS setup

1. **Install BlackHole 2ch** (free, from Existential Audio).
2. **Create a Multi-Output Device** in *Audio MIDI Setup → + → Create Multi-Output
   Device*. Tick your interface and *BlackHole 2ch*, set your **interface as the
   primary (clock) device**, and tick *Drift Correction* for BlackHole. Set the
   interface and BlackHole to 48 kHz.
3. **Use the Multi-Output Device as the system output** (*System Settings → Sound →
   Output*). Your main monitors play directly from the interface, untouched.
4. **Run Correction Host:**
   - **Capture:** `Input device (BlackHole)`, **From:** `BlackHole 2ch`,
     **Channels:** `1-2`
   - **Driver:** `CoreAudio`, **To:** your interface, **Channels:** `3-4`
   - **Plugin:** ARC X (AU or VST3), then load your profile.
5. Allow the **microphone permission** prompt. macOS treats capturing from
   BlackHole as microphone access.
6. **The first launch:** the app isn't notarised. On the Mac that built it, it opens
   normally. On a Mac you copied it to, open it once, then go to *System Settings →
   Privacy & Security* and click **Open Anyway**. For launch at login, add it under
   *System Settings → General → Login Items*.

The logo sits in the menu bar (it follows light and dark mode). Click it for
**Show window**, **Bypass correction** and **Quit**.

Volume keys don't work with a Multi-Output Device. Set levels with your
interface's or speakers' controls.

---

## 4. Measuring with ARC X

Make the D3V profile the way IK describes for ARC X:

- Play the test signals through **interface outs 3-4**.
- Put the mic where you sit at the **secondary setup**.

Then load that profile in ARC X inside Correction Host. If ARC X's measurement has
to run inside a plugin host with a mic input, do the measurement in your DAW
(routed to outs 3-4), save the profile, and load it here.

Keep **Correction Host's ARC X bypassed or the app quit while measuring**, so
nothing else is playing into the D3Vs.

---

## 5. Troubleshooting

| Status says | What to do |
|---|---|
| `…would feed back` | The output device and channels are the same as the ones being captured. Pick the D3V outputs. |
| `…has 2 output channels, so channels 3-4 aren't available` | Your driver shows each output pair as its own device. Pick that device and channels 1-2. |
| `Not running – retrying` | A device is missing or busy. It reconnects by itself once the device is back. |
| Underruns or rebuffers keep climbing | Raise **Safety buffer** to 10 or 20 ms. |
| ARC X isn't in the plugin list | It must be installed as VST3 (Windows: `C:\Program Files\Common Files\VST3`). Click **Rescan**. |
| ARC X shows a licence or activation screen | Activate it as you would in a DAW. Correction Host is just a host. |

Settings are stored in `%APPDATA%\CorrectionHost` on Windows and
`~/Library/Application Support/CorrectionHost` on macOS.

---

## 6. What has been tested

All of this was tested on Linux:

- **`DriftFifoTest`:** simulates two independent device clocks for 300 s per case.
  Cases cover ±150 ppm drift, mismatched block sizes, a 44.1→48 kHz rate change,
  late and bunched packets with up to 12 ms scheduling jitter, and the source
  going silent for 2 s. It checks for drift lock within 3 ppm, zero underruns after
  settling, clean recovery, and THD+N.
- **`LoopbackGlueTest`:** runs the Windows loopback adapter in real time against
  a fake WASAPI backend. It checks the device list, errors, and channel-pair routing.
- **`EngineTest`:** hosts a real VST3 through the engine's actual audio callback
  with a fake output device. It checks processing, bypass crossfades, state save and
  restore, unloading, device restart, mixing in the interface input (level,
  summing, turning it off), and a bad plugin path.
- **The app** builds and runs (UI checked under a virtual display).
- **`UpdaterTest`:** release-JSON parsing (including drafts, missing assets, non-HTTPS
  links and checksums), build numbers, and waiting for the previous version to exit.
- **`IconRenderTest`:** renders the tray logo at 16–64 px in colour, bypassed and
  menu-bar template styles, for checking by eye.
- **The WASAPI loopback code** cross-compiles and links for Windows (MinGW, no
  warnings).

**Not yet tested:** a real Windows or Mac machine with real audio devices, and ARC
X itself. The first run on your machine is the real test, and the status box is
there to show what's happening.

To build and run the tests (Linux/macOS):

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCORRECTIONHOST_BUILD_TESTS=ON
cmake --build build --target DriftFifoTest LoopbackGlueTest EngineTest TestGainPlugin_VST3 UpdaterTest
./build/DriftFifoTest_artefacts/Release/DriftFifoTest
./build/LoopbackGlueTest_artefacts/Release/LoopbackGlueTest
./build/UpdaterTest_artefacts/Release/UpdaterTest
./build/EngineTest_artefacts/Release/EngineTest "$PWD/build/TestGainPlugin_artefacts/Release/VST3/TestGain.vst3"
```

## 7. Code map

| File | What it does |
|---|---|
| `Source/DriftFifo.h` | Clock-drift-compensating FIFO and resampler |
| `Source/WasapiLoopback.*` | Raw WASAPI loopback capture (Windows) |
| `Source/LoopbackCaptureSource.*` | Connects loopback capture to the app |
| `Source/JuceDeviceCaptureSource.*` | Input-device capture (BlackHole, VB-Cable) |
| `Source/HostEngine.*` | Output device, plugin hosting, bypass, watchdog |
| `Source/MainComponent.*`, `Source/Main.cpp` | UI, tray icon, settings, start with Windows |
| `Source/Updater.*` | Self-update from GitHub releases |
| `.github/workflows/release.yml` | Builds Windows + macOS and publishes a release on every push to `main` |
| `install/` | One-line installers |
| `Source/AppIcon.h`, `Resources/` | The logo: drawn in code for the tray, PNG/SVG artwork for the app icon |
| `Tests/` | The test programs above |

JUCE is used under its AGPLv3 / commercial licence terms. That's fine for personal
use; read JUCE's licence before distributing builds.
