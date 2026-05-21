# FM-SDR-Tuner

FM broadcast tuner and XDR server built around RTL-SDR IQ input.

## Project Status / Experiment

This repository is an active experiment in AI-assisted software development.

Goal:
- Validate how a domain expert, aided by AI, can build a real SDR project that
  works in practice and stays manageable over time.

Implications:
- Behavior can change quickly between releases while we iterate.
- We prioritize working end-to-end functionality, measurable regressions
  (tests/coverage), and maintainability.
- Treat this as experimental software, not a finished product.

Current architecture:
- Input: `rtl_sdr` (default) or `rtl_tcp`
- Demod/stereo/audio pipeline in-process
- DSP pipeline uses `liquid-dsp` primitives (FM chain + RDS-related paths)
- XDR-compatible server on port `7373`
- Native audio backends: Core Audio (macOS), ALSA (Linux), WinMM (Windows)

## Highlights

- Direct USB RTL-SDR support (`--source rtl_sdr`) as default mode
- `rtl_tcp` network source support (`--source rtl_tcp`)
- FM stereo demod with gear-shifted PLL on the 19 kHz pilot (wide acquire, narrow hold)
- 2nd-order Butterworth adaptive Hi-Blend on the L−R path — narrows stereo image as reception worsens, with 12 dB/oct rejection of stereo-decoded HF noise instead of 6 dB/oct
- LMS pilot canceller: subtracts a phase-locked 19 kHz copy from the mono path (~20 dB extra suppression beyond the audio LPF; on by default)
- Optional adaptive de-emphasis ("HiCut"): narrows the top end on fringe stations to suppress hiss without affecting strong locals
- Optional adaptive channel bandwidth: narrows the channel FIR when SNR is low, widens when it recovers (hysteresis-gated)
- Optional CMA multipath equalizer (Godard 1980, patent-free): cancels FM ghosting on real multipath, stays transparent on clean signals via dispersion target + leak regularization
- Continuous-quality blend gate (no mono pops on marginal signals)
- Soft-knee audio limiter with metered clip ratio
- RDS decode in dedicated worker thread (redsea-port: 171 kHz, 57 kHz PLL, RRC symbol sync, BPSK, block-sync state machine)
- XDR protocol compatibility for FM-DX clients on port 7373
- Audio output at 48 kHz (native Core Audio / ALSA / WinMM)
- Output to speaker (`-s`), WAV (`-w`), MPX WAV (`--mpx-wav`, with configurable rate via `--mpx-rate` for downstream RDS / spectrum / decoder analysis or feeding an FM exciter that accepts raw MPX), live MPX → audio device (`--mpx-audio` on macOS/Linux, typically into BlackHole / snd-aloop for re-encoding or directly into a TX accepting line-in MPX), and/or raw IQ capture (`-i`)
- Self-documenting signal meter: startup `[METER]` log reports the active calibration window and periodically suggests narrower floor/ceil based on observed session min/max

## Quick Start

With an RTL-SDR connected, the binary works with no CLI flags and no config file — audio is on by default and the XDR server runs in implicit guest mode when no password is set:

```bash
./build/fm-sdr-tuner
```

This opens the XDR server on `127.0.0.1:7373` and waits for a client (xdr-gtk, FM-DX-Webserver, etc.) to send the start command. To skip the wait and hear audio immediately:

```bash
./build/fm-sdr-tuner --auto-start
```

Useful one-shot overrides (all still valid without a config file):

```bash
./build/fm-sdr-tuner -f 101100                              # start at 101.1 MHz
./build/fm-sdr-tuner -b soft                                # soft stereo blend profile
./build/fm-sdr-tuner -P secret                              # require XDR password 'secret'
./build/fm-sdr-tuner -w capture.wav                         # also record audio to WAV
./build/fm-sdr-tuner --mpx-wav mpx.wav --mpx-rate 192000    # capture MPX at 192 kHz for downstream RDS/spectrum analysis or feeding an FM exciter
./build/fm-sdr-tuner --mpx-audio --mpx-audio-device "BlackHole" --mpx-rate 192000   # macOS: live MPX into a virtual loopback at 192 kHz
./build/fm-sdr-tuner -l                                     # list audio output devices
./build/fm-sdr-tuner --calibrate                            # one-shot band sweep — prints stations + recommended signal_floor_dbfs / signal_ceil_dbfs for this location/antenna
```

## Requirements

- C++17 compiler
- CMake `>= 3.15`
- OpenSSL
- `librtlsdr`
- `liquid-dsp`

### macOS

```bash
brew install cmake pkg-config openssl rtl-sdr liquid-dsp
```

### Linux (Debian/Ubuntu)

```bash
sudo apt update
sudo apt install -y cmake libasound2-dev pkg-config libssl-dev librtlsdr-dev libliquid-dev
```

### Linux (Fedora)

```bash
sudo dnf install -y cmake alsa-lib-devel pkgconf-pkg-config openssl-devel rtl-sdr-devel liquid-dsp-devel
```

### Windows

**Before building anything, get the RTL-SDR dongle working on the system first.** Windows does not ship a usable driver for these dongles; the WinUSB driver has to be installed manually via Zadig.

Recommended one-time setup, in order:

1. Plug in the dongle.
2. Download and run [Zadig](https://zadig.akeo.ie/). Under *Options* enable "List All Devices", select the RTL2832U interface (named "Bulk-In, Interface (Interface 0)" or similar), and install the **WinUSB** driver.
3. Download [SDR#](https://airspy.com/download/) (or any other known-working RTL-SDR app such as SDRConsole) and verify the dongle tunes a known local FM station and produces audio. If SDR# does not work, `fm-sdr-tuner` will not work either — fix the driver/Zadig step first.
4. Only then build and run `fm-sdr-tuner`.

Build dependencies: use vcpkg and install at least OpenSSL plus `librtlsdr` and `liquid-dsp` for your triplet.

If `fm-sdr-tuner` reports `[SDR] no rtl_sdr device found at index 0` even after SDR# works, close SDR# first — only one application can hold the dongle at a time.

## Audio Loopback (optional)

The tuner writes PCM to a single OS audio output device (Core Audio / ALSA / WinMM). It does not stream audio over the XDR control connection. If you want another application — FM-DX-Webserver, a streaming encoder, a recorder — to consume the tuner's audio, you need a virtual loopback that exposes the tuner's output as a recordable input on the host. Without a loopback the tuner can only play to a local speaker.

List the devices the tuner can see with `./build/fm-sdr-tuner -l`, then point it at the loopback either per-run with `-d "<name>"` or persistently via `[audio] device = ...` in the INI. The bundled `fm-sdr-tuner.ini` already defaults to `BlackHole 2ch`.

### macOS — BlackHole

BlackHole is distributed as a Homebrew Cask (it installs a system audio driver), so the install flag is `--cask`. A reboot is required to complete the install:

```bash
brew install --cask blackhole-2ch
```

Set the tuner's output to `BlackHole 2ch`, and configure the consumer (FM-DX-Webserver, etc.) to record from `BlackHole 2ch`. To also monitor on real speakers, build a Multi-Output Device in Audio MIDI Setup that contains both BlackHole and the speaker, and select that aggregate as the tuner output.

For routing the raw MPX (not the 48 kHz stereo audio) into BlackHole at 192 kHz — useful for RDS decoders, broadcast analysis software, or feeding a software FM transmitter — see the [`--mpx-audio` section below](#live-mpx--audio-device---mpx-audio).

### Linux — ALSA `snd-aloop`

Load the loopback kernel module (persist with a `/etc/modules-load.d/` entry if you want it across reboots):

```bash
sudo modprobe snd-aloop
```

`snd-aloop` exposes paired PCMs: writes to `hw:Loopback,0,<n>` appear as a capture on `hw:Loopback,1,<n>`. Point the tuner at the playback side via `[audio] device = hw:Loopback,0,0` (or pass `-d hw:Loopback,0,0`) and have the consumer capture from `hw:Loopback,1,0`. Use `aplay -L` / `arecord -L` to confirm the names on your system.

### Windows — VB-CABLE

Install [VB-Audio VB-CABLE](https://vb-audio.com/Cable/). Select `CABLE Input (VB-Audio Virtual Cable)` as the tuner's output device and have the consumer record from `CABLE Output`. VB-CABLE is one-way; for monitoring, use VB-Audio's "VoiceMeeter" or pair VB-CABLE with the Windows "Listen to this device" option on the cable's recording endpoint.

## Build

Use the normal `build/` directory.

### macOS (Apple Silicon)

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_OSX_ARCHITECTURE=arm64
cmake --build .
```

### macOS (Intel)

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_OSX_ARCHITECTURE=x86_64
cmake --build .
```

### Linux

```bash
mkdir -p build && cd build
cmake ..
cmake --build .
```

Build a Debian package on Ubuntu/Debian:

```bash
cd build
cpack -G DEB
```

Install locally:

```bash
sudo apt install ./fm-sdr-tuner_*_*.deb
```

### Windows (vcpkg)

```bash
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build . --config Release
```

Example dependency install (triplet/package names may vary by your vcpkg setup):

```bash
vcpkg install openssl librtlsdr liquid-dsp
```

## Tests

Run the normal test suite:

```bash
ctest --test-dir build --output-on-failure --no-tests=error
```

### RTL-SDR Live Hardware Tests

There is now a gated hardware-in-the-loop test target for a real RTL-SDR dongle.
It is skipped by default and only runs when explicitly enabled.

Smoke test with a connected dongle:

```bash
FM_TUNER_RUN_RTL_SDR_LIVE=1 \
ctest --test-dir build --output-on-failure -R rtl_sdr_live -V
```

Supported environment overrides:

- `FM_TUNER_RTL_DEVICE_INDEX` default `0`
- `FM_TUNER_RTL_FREQ_KHZ` default `101100`
- `FM_TUNER_RTL_SAMPLE_RATE` default `256000`
- `FM_TUNER_RTL_GAIN_TENTHS_DB` optional, if unset the smoke test uses tuner AGC
- `FM_TUNER_RTL_PPM` optional, only applied if explicitly set

The live test binary contains two hardware checks:

1. Smoke test:
   - open device
   - set sample rate and tune
   - read live IQ
   - compute signal level from real samples
   - exercise low-latency read mode
   - retune and verify reads still succeed

2. Strong-vs-weak comparison test:
   - fixed manual tuner gain for both frequencies
   - average several live reads per frequency
   - verify the stronger station reports higher level than the weaker one

Default comparison frequencies:

- strong: `105.9 MHz`
- weak: `88.2 MHz`

The comparison test is intentionally gated separately because it depends on your
actual RF environment and antenna setup.

Override them if your local RF environment is different:

```bash
FM_TUNER_RUN_RTL_SDR_COMPARE=1 \
FM_TUNER_RTL_STRONG_FREQ_KHZ=105900 \
FM_TUNER_RTL_WEAK_FREQ_KHZ=88200 \
FM_TUNER_RTL_COMPARE_GAIN_TENTHS_DB=180 \
ctest --test-dir build --output-on-failure -R rtl_sdr_live -V
```

### Quick XDR Launch (macOS / local testing)

To start the tuner with a connected RTL-SDR and expose the XDR server on
`127.0.0.1:7373` with password `test123`:

```bash
./scripts/run_rtl_xdr_test.sh
```

Defaults used by the script:

- source: `rtl_sdr`
- device index: `0`
- frequency: `101.1 MHz`
- IQ rate: `256000`
- speaker output: enabled, but playback starts when the XDR client sends start
- XDR password: `test123`

Useful overrides:

```bash
RTL_DEVICE_INDEX=0 \
FREQ_KHZ=105900 \
IQ_RATE=256000 \
XDR_PASSWORD=test123 \
./scripts/run_rtl_xdr_test.sh
```

Then connect your XDR client to:

- host: `127.0.0.1`
- port: `7373`
- password: `test123`

If you want immediate local speaker audio without waiting for an XDR client:

```bash
./scripts/run_rtl_local_audio.sh
```

That launcher uses `--auto-start` and is intended for manual local listening,
not for testing XDR client-controlled start/stop behavior.

## Live MPX → audio device (`--mpx-audio`)

Routes the post-discriminator multiplex (MPX) signal — mono baseband containing the 0–15 kHz L+R audio, 19 kHz stereo pilot, 23–53 kHz L−R DSB-SC subcarrier, and 57 kHz RDS subcarrier — to a system audio output device in real time. Independent of the regular 48 kHz stereo audio output, so you can keep listening on speakers while the MPX streams elsewhere.

Two main reasons to use it:
- **Re-encoding / analysis**: route MPX into a virtual loopback (BlackHole on macOS, `snd-aloop` on Linux) so a downstream consumer — RDS decoder, spectrum analyzer, broadcast monitor — picks it up as a regular audio input. No file in the middle.
- **Direct TX line-in**: many FM exciters / software TXs accept raw MPX as line-in audio at 192 kHz. The tuner becomes a clean MPX source for the transmitter.

The MPX bandwidth extends to ~75 kHz (RDS sidebands), so the audio device must be capable of **at least 192 kHz** sample rate to avoid aliasing the 19/38/57 kHz subcarriers into the audible band.

```bash
# macOS: stream MPX to BlackHole at 192 kHz
./build/fm-sdr-tuner -f 88600 --mpx-audio --mpx-audio-device "BlackHole" --mpx-rate 192000 -G

# Linux: stream MPX to snd-aloop at 192 kHz
./build/fm-sdr-tuner -f 88600 --mpx-audio --mpx-audio-device "hw:Loopback,0,0" --mpx-rate 192000 -G

# Default device, default rate (192 kHz):
./build/fm-sdr-tuner -f 88600 --mpx-audio -G
```

### MPX-only output (no 48 kHz audio)

The most common use case for `--mpx-audio` is *clean MPX into a downstream consumer with no other audio on the same machine* — a 192 kHz DAC wired to an FM exciter, an SDR-based software TX, or a broadcast monitor box. In that mode you do not want the regular 48 kHz stereo audio output competing for the audio device or the speakers. Pair `--mpx-audio` with `--no-audio`:

```bash
./build/fm-sdr-tuner \
  --auto-start \
  --source rtl_sdr --rtl-device 0 --iq-rate 256000 \
  -f 88600 \
  --no-audio \
  --mpx-audio --mpx-audio-device "Headphones" \
  --mpx-rate 192000 \
  -G
```

Why this layout makes sense:
- `--no-audio` skips the 48 kHz `AudioOutput` entirely — no FM-decoded stereo plays anywhere. The DSP pipeline still produces stereo internally so RDS and meter still work, the audio is simply not routed to a device.
- `--mpx-audio --mpx-audio-device "Headphones"` opens a single Core Audio (macOS) or ALSA (Linux) stream straight from the MPX tap. `"Headphones"` here is a *device name substring* — match whatever your DAC reports under `./build/fm-sdr-tuner -l`. The name has no relationship to actual headphones; the example device is a 192 kHz DAC that happens to be labelled "Headphones" by Core Audio.
- `--mpx-rate 192000` preserves the 19/38/57 kHz subcarriers (192 kHz Nyquist = 96 kHz, well above the ~75 kHz MPX top end). Lower rates would alias the subcarriers into the audible band and break downstream RDS / stereo decoding.
- `-G` skips XDR authentication so you can run head-less without a client.

If the target device only accepts a 2-channel format, the tuner automatically duplicates the mono MPX to both L and R — no extra flags needed. You'll see `[MPX-AUDIO] device rejected mono format; using stereo (MPX duplicated to L=R)` in the startup log when that fallback engages.

You'll see a startup line like:
```text
[MPX-AUDIO] CoreAudio device: 'BlackHole 2ch' (selector 'BlackHole')
[MPX-AUDIO] streaming mono @ 192000 Hz (resampled from 256000 Hz)
```

If the device cannot negotiate the requested rate (ALSA will fall back to whatever it supports), the tool warns and the MPX gets band-limited accordingly. On macOS Core Audio you may need an aggregate device or BlackHole specifically to hit 192 kHz; built-in speakers usually max out at 96 kHz.

### Windows: not currently supported

The built-in Windows audio backend (WinMM) is the legacy MultiMediaExtensions API and is effectively capped at 48 kHz on common configurations. At that rate the 19/38/57 kHz subcarriers fold into the audible band and the MPX becomes useless for the downstream tools this feature is designed to feed. `--mpx-audio` on Windows therefore refuses to start with a clear error message instead of producing aliased junk.

Workarounds on Windows:
- Use `--mpx-wav --mpx-rate 192000` to capture MPX to a file (works fine on Windows) and feed downstream tools from there.
- Run a Linux VM with USB pass-through to the RTL-SDR if live MPX-to-audio is essential.
- Switch host platform: macOS or Linux supports `--mpx-audio` natively.

A future Windows backend (WASAPI exclusive mode) could lift the 48 kHz limit; not yet implemented.

## Runtime Behavior

- Audio output is enabled by default. CLI flags can add WAV (`-w`), MPX WAV (`--mpx-wav`), and/or raw IQ (`-i`) outputs; use `-s` to re-enable audio when a config has explicitly disabled it.
- Default source is direct RTL-SDR (`rtl_sdr`).
- Default startup frequency is `87500 kHz` (EU bottom of band — unlikely to hit a strong local on first run).
- Audio output sample rate is fixed at `48000 Hz`.
- Device/buffer latency is backend-specific (Core Audio / ALSA / WinMM).
- XDR server listens on port `7373`. If `xdr.password` is empty and no `-P` is passed, the server automatically enters guest mode (no auth).
- The tuner does not auto-start by default; an XDR client start command activates it. Use `--auto-start` to begin playback without a client.
- De-emphasis default is 50 µs (EU/ITU). For US/Korea set `[tuner].deemphasis = 1` in the config (75 µs).
- Default stereo blend profile is `aggressive` (fast mute on marginal pilot); select `normal` or `soft` via `-b` or the config.

## Config-Driven Usage (optional)

Once you know your site profile (gain, meter calibration, bandwidth), move the settings into a config file:

```bash
./build/fm-sdr-tuner -c fm-sdr-tuner.ini
```

The bundled `fm-sdr-tuner.ini` is documented and safe to copy. CLI flags override anything the config sets, which is useful for A/B testing:

```bash
# temporary frequency override on top of a baseline config
./build/fm-sdr-tuner -c fm-sdr-tuner.ini -f 101100

# try a different stereo blend profile without editing the INI
./build/fm-sdr-tuner -c fm-sdr-tuner.ini -b soft
```

## Configuration (`fm-sdr-tuner.ini`)

`fm-sdr-tuner.ini` is the primary control surface.

Important sections:
- `[tuner]`: source, device index, startup frequency, deemphasis
- `[audio]`: enable speaker output and select output device
- `[sdr]`: tuner gain strategy and dBf mapping window
- `[processing]`: DSP block size, W0 bandwidth, stereo blend, optional DSP AGC
- `[xdr]`: server port/password/guest mode

Signal meter related keys (`[sdr]`):
- `signal_floor_dbfs` (default `-65.0`)
- `signal_ceil_dbfs` (default `-5.0`)
- `signal_bias_db` (default `0.0`)

Weak-signal tuning keys (all default to off / static):
- `processing.w0_bandwidth_hz` — static channel bandwidth
- `processing.dsp_agc = off|fast|slow` — I/Q AGC before the discriminator
- `processing.stereo_blend = soft|normal|aggressive` — how aggressively to collapse stereo
- `processing.pilot_canceller = true|false` — 19 kHz pilot residual canceller (default on)
- `processing.hicut = off|gentle|strong` — adaptive de-emphasis HiCut
- `processing.adaptive_bandwidth = off|conservative|aggressive` — SNR-driven channel narrowing
- `processing.multipath_eq = off|light|aggressive` — CMA multipath equalizer
- `processing.multipath_eq_taps` — equalizer tap count (default 17)

## Setup And Tuning Guide

This project behaves best when it is tuned like a real radio front end. Most
bad behavior comes from getting the order wrong.

Correct tuning order:
1. get the hardware stable
2. reduce clipping
3. verify stereo behavior
4. calibrate the meter
5. only then investigate wrong-frequency / ghosting issues

Do not tune by the meter alone. Do not judge stereo while the tuner is still
clipping.

### 1. Start From A Known-Good Baseline

The binary ships with wide, safe defaults (`gain_strategy = tef`, `agc_mode = 2`, `w0_bandwidth_hz = 194000`, `stereo_blend = aggressive`, `signal_floor_dbfs = -65`, `signal_ceil_dbfs = -5`, `signal_bias_db = 0`, deemph 50 µs, `pilot_canceller = true`). The optional adaptive features (`hicut`, `adaptive_bandwidth`, `multipath_eq`) default to off so the audio is identical to a passive demod chain until you opt in.

For a dedicated site profile, put overrides in `fm-sdr-tuner.ini`:

```ini
[sdr]
gain_strategy = tef
rtl_gain_db = -1
default_custom_gain_flags = 1
freq_correction_ppm = 0
# Wide default; watch [METER] log and narrow once you know the local range.
signal_floor_dbfs = -65.0
signal_ceil_dbfs = -5.0
signal_bias_db = 0.0

[processing]
agc_mode = 2
w0_bandwidth_hz = 194000
dsp_agc = off
stereo_blend = aggressive
stereo = true
pilot_canceller = true
# Optional adaptive features — leave off until you have a stable baseline.
hicut = off
adaptive_bandwidth = off
multipath_eq = off
client_gain_allowed = false
```

Why this baseline:
- `tef` is the best default when local RF conditions are not yet known
- `agc_mode = 2` is a stable middle ground
- `w0_bandwidth_hz = 194000` is the correct starting point for normal WFM stereo
- `stereo_blend = aggressive` pairs well with the adaptive Hi-Blend post-filter — the HF of stereo fades cleanly on marginal signals without audible pops
- `client_gain_allowed = false` prevents XDR clients from changing gain while you tune the site profile

### 2. Validate Basic Hardware First

Before tuning quality, make sure the setup is fundamentally correct.

Check:
- the dongle is detected
- you can tune and hear audio
- XDR client control works if you use `xdr-gtk`
- there are no repeated USB errors, disconnects, or device resets

Useful quick paths:

```bash
./scripts/run_rtl_xdr_test.sh
```

or for local audio without a client:

```bash
./scripts/run_rtl_local_audio.sh
```

If the system is not stable here, stop. Fix USB, antenna, audio device, or
driver issues before adjusting gain.

### 3. Pick Three Reference Frequencies

For your location, identify:
- one strong clean local station
- one medium-strength usable station
- one weak station or an empty frequency

You need all three because one station alone cannot tell you whether the system
is overloaded, too insensitive, or simply mis-calibrated.

The best empty frequency is one with no local station and no obvious adjacent
spillover in your area.

### 4. Tune RF Gain Before Anything Else

The primary gain-health signal is:
- `[SIG] clip`

Interpretation:
- `clip = 0.0000` or near zero: healthy
- occasional tiny values on strong stations: usually acceptable
- sustained clipping such as `0.0500`, `0.2000`, or `1.0000`: gain is too high

Examples from real behavior:
- `clip=1.0000`: badly overloaded; all other quality metrics are suspect
- `clip=0.0000` on a strong station: gain is in a usable range

#### How to adjust gain

If using `tef` strategy:
- start with `rtl_gain_db = -1`
- adjust `[processing].agc_mode`

Interpretation:
- lower `agc_mode` = more sensitivity
- higher `agc_mode` = more overload protection

Practical sequence:
- overloaded strong locals: try `agc_mode = 3`
- balanced mixed area: keep `agc_mode = 2`
- weak rural / DX-heavy location: try `agc_mode = 1`, then `0` only if needed

If using fixed manual gain:
- set `rtl_gain_db` to a fixed value
- use `3 dB` to `6 dB` steps

Practical manual range:
- dense urban / strong antenna: `8 .. 16 dB`
- suburban mixed area: `12 .. 22 dB`
- weak rural / DX: `20 .. 36 dB`

Do not chase the highest signal number. The correct gain is the highest gain
that does not produce sustained clipping on strong stations.

### 5. How To Read The Console Output

Watch the console while tuning. The current log fields are meant to tell you
what to fix next.

#### `[SIG]` lines

Current format:

```text
[SIG] dbfs=... compensated=... floor=... snr=... level=... filtered=... clip=...
```

Field meaning:
- `dbfs`: absolute tuned-channel receiver power
  - What to do next: use this to compare empty vs strong channels when calibrating the meter
- `compensated`: gain-adjusted internal power
  - What to do next: use this only as a secondary reference; do not tune gain from this alone
- `floor`: estimated baseline / local floor reference
  - What to do next: compare this with empty frequencies; if empty channels still look too strong, revisit gain first, then meter calibration
- `snr`: currently diagnostic only
  - What to do next: do not make first-pass gain decisions from this field
- `level`: mapped UI / XDR display level
  - What to do next: use it for client display sanity, not as your primary gain control signal
- `filtered`: smoothed display level
  - What to do next: use it only to understand why the client meter moves more slowly than raw `level`
- `clip`: overload indicator
  - What to do next: this is the primary gain-health field; reduce clipping before tuning anything else

#### `[ST]` lines

Current format:

```text
[ST] pilot=... stereo=... quality=... blend=...
```

Field meaning:
- `pilot`: 19 kHz pilot strength estimate
  - What to do next: a strong stereo station should show a clearly stronger pilot than a weak or mono station
- `stereo`: decoder lock state
  - What to do next: `stereo=1` means the decoder is actually locked; `stereo=0` means do not expect stable stereo output
- `quality`: stereo-path confidence
  - What to do next: compare this across stations; low quality on a strong local means recheck gain, bandwidth, and RF cleanliness
- `blend`: actual stereo separation currently applied
  - What to do next: if a strong station has low blend after gain is fixed, adjust stereo blend policy or recheck pilot quality

#### `[DSP]` lines

Examples:

```text
[DSP] reset reason=retune
```

Meaning:
- the DSP pipeline was reset because tuning changed
  - What to do next: nothing; this is expected during retune and is not itself an RF fault

#### `[XDR]` lines

Examples:

```text
[XDR] tuning to 105900 kHz
[XDR] client connected from 127.0.0.1
[XDR] client disconnected from 127.0.0.1
```

Meaning:
- control-plane activity from the XDR client
  - What to do next: use this to confirm the client really sent a tune/start/stop command before blaming DSP behavior

#### What Good Looks Like

Strong clean station:
- `clip` near zero
- strong `pilot`
- `stereo=1`
- `quality` clearly above weak/noisy stations
- `blend` high, usually near full stereo

Weak or noisy station:
- `clip` still near zero
- lower `pilot`
- `stereo` may flicker or drop
- lower `quality`
- lower `blend`, drifting toward mono

Empty or quiet frequency:
- low `dbfs`
- low `level`
- no meaningful stereo lock
- if it still looks strong, gain or meter calibration is wrong

#### What Bad Looks Like

- high `clip`:
  - too much gain; reduce gain first
- high `level` on empty frequencies:
  - likely gain or meter calibration problem; do not blame stereo first
- `stereo=1` but low `blend` on a strong station:
  - gain may still be wrong, or the stereo-quality/blend policy is too conservative
- repeated `[DSP] reset reason=retune` while tuning:
  - expected during retune, not a signal-path fault

#### Example: Reading One Realistic Console Sequence

```text
[SIG] dbfs=-1.79 compensated=-13.79 floor=-4.79 snr=0.00 level=115.0 filtered=115.2 clip=0.0000
[ST] pilot=207 stereo=1 quality=0.788 blend=0.633
[DSP] reset reason=retune
[XDR] tuning to 105900 kHz
```

How to interpret it:
- first `[SIG]` line:
  - `clip=0.0000` means gain is now healthy
  - `level=115.0` means the station is strong
  - action: do not lower gain further just because the station is strong; clipping is already controlled
- `[ST]` line:
  - `pilot=207` and `stereo=1` mean stereo lock is good
  - `quality=0.788` and `blend=0.633` mean stereo is working, but it is still being narrowed
  - action: if this is a clearly strong local station, recheck stereo blend policy after gain is fixed
- `[DSP] reset reason=retune`:
  - expected because tuning changed
  - action: none
- `[XDR] tuning to 105900 kHz`:
  - confirms the client actually requested the new frequency
  - action: none

### 6. Tune Stereo Behavior Only After Gain Is Correct

Do not tune stereo blending while clipping is still high.

Main control:
- `[processing] stereo_blend = soft|normal|aggressive`

Meaning:
- `soft`: keep more stereo, even on weaker signals
- `normal`: balanced default
- `aggressive`: collapse toward mono sooner to suppress stereo noise

Use:
- `soft` if stations are generally clean and you want maximum stereo image
- `normal` for most users
- `aggressive` if weak stereo sounds noisy and you prefer quieter mono-ish output

Important:
- a strong station should not need `soft` just to sound correct
- if a strong local only becomes acceptable in `soft`, recheck gain and pilot quality first

### 7. Set Bandwidth For The Kind Of Listening You Want

Main control:
- `[processing] w0_bandwidth_hz`

Starting point:
- `194000` for normal WFM stereo listening

Use a narrower bandwidth only when needed:
- weak-signal or adjacent-channel trouble: try `168000`, `151000`, `133000`

Tradeoff:
- wider bandwidth = better stereo, pilot, and RDS recovery when clean
- narrower bandwidth = more adjacent-channel rejection, but easier to lose stereo quality

Do not narrow bandwidth first when the real issue is overload.

### 8. Calibrate The Signal Meter For Your Location

These keys are display calibration only:
- `signal_floor_dbfs` (compensated dBFS that maps to meter value 0)
- `signal_ceil_dbfs` (compensated dBFS that maps to meter value 120)
- `signal_bias_db` (overall offset; usually keep at 0)

They do not fix RF overload, wrong tuning, or ghosting.

The default 60 dB window (`-65` floor, `-5` ceil) is intentionally wide so it works on most sites without saturating either end. The `[METER]` log line shipped in every run does the legwork for you:

```text
[METER] mapping window: floor=-65.0 dBFS, ceil=-5.0 dBFS (range 60.0 dB), bias=0.0 dB
[METER] session observed: min=-53.5 dBFS, max=-26.9 dBFS — for full-scale meter consider floor≈-56, ceil≈-24
```

Two procedures, both work end-to-end:

**Procedure A — One-shot band sweep (recommended).** Use the built-in calibration tool. It opens the RTL-SDR, sweeps 87.5–108 MHz, prints all detected stations sorted by quality, and emits copy-paste-ready INI values tuned to your antenna + location:

```bash
./fm-sdr-tuner --calibrate
```

The output includes:
- a per-frequency table (dBFS + meter level at every 100 kHz step)
- a sorted list of detected stations (level120 > 30)
- a recommendation block with `signal_floor_dbfs`, `signal_ceil_dbfs`, and `signal_bias_db` chosen so the 0..120 meter spans the actual signal envelope at your site

Paste the recommendation block into the `[sdr]` section of your INI and restart. Takes ~45 seconds, no XDR client or audio backend needed. Re-run any time the antenna or location changes.

**Procedure B — Live auto-suggester (for tweaking while listening).**
1. Start the tuner with the default wide window.
2. Tune through your representative stations (strong local, mid, fringe, empty) over ~1 minute.
3. Read the `[METER] session observed` line. It tracks the running min/max of compensated dBFS and recommends `floor ≈ min − 3 dB`, `ceil ≈ max + 3 dB` so the meter spans 0..120 across your real signal range.
4. Apply those values to `[sdr]` in your INI.
5. Restart and confirm: strong locals now settle near 100–110 (with room for exceptional signals), and fringe stations register meaningfully around 20–40.

Manual fallback (if you prefer the by-hand approach):
1. Tune an empty or very weak frequency and note typical `dbfs`/`compensated`.
2. Tune a strong clean local station and note typical `compensated`.
3. Set `signal_floor_dbfs` ≈ empty-frequency compensated − 3 dB; `signal_ceil_dbfs` ≈ strong-station compensated + 3 dB.

Symptoms of bad meter calibration:
- empty channels always look too strong: floor is too high, or gain is still wrong
- every strong station saturates at 120: ceil is too low
- everything looks compressed into the middle: floor/ceil window is too wide
- weak stations clamp to 0: floor is too high (raise the window's bottom; the `[METER]` suggester catches this automatically)

### 9. Wrong Frequency / Ghosting Troubleshooting

If stations appear on the wrong frequency, or you hear a strong station turning
up elsewhere, work through the causes in this order.

#### 1. Systematic frequency offset: suspect `freq_correction_ppm`

Symptom:
- all stations are shifted by roughly the same amount

Most relevant key:
- `[sdr] freq_correction_ppm`

What to do:
1. tune one known strong local station
2. set gain so `clip` is near zero
3. check whether the station peak is consistently offset by the same amount on multiple stations
4. if the offset is systematic, adjust `freq_correction_ppm`

Do not try to correct systematic offset with:
- `signal_floor_dbfs`
- `signal_ceil_dbfs`
- `signal_bias_db`

Those only change display calibration.

#### 2. Duplicates of strong stations: suspect overload or image response

Symptom:
- strong stations appear again elsewhere
- this is especially likely if `clip` is nonzero

Most relevant keys:
- `[sdr] rtl_gain_db`
- `[processing] agc_mode`

What to do:
1. lower gain until `clip` is near zero
2. retest the same strong station and the suspected ghost frequency
3. if the duplicate weakens or disappears when gain is reduced, treat it as overload / image behavior

If duplicates remain only at high gain, the fix is RF front-end discipline, not meter calibration.

#### 3. Nearby strong station leaking into adjacent channels: suspect adjacent-channel leakage

Symptom:
- the “ghost” is near a strong station
- the problem improves when bandwidth is narrowed

Most relevant key:
- `[processing] w0_bandwidth_hz`

What to do:
1. keep gain sane first
2. narrow bandwidth from `194000` to `168000`, `151000`, or `133000`
3. retest whether the nearby leakage reduces

If it improves with narrower bandwidth, this is more likely adjacent leakage than frequency math.

#### 4. Big changes with antenna or placement: suspect site/front-end issues

Symptom:
- behavior changes dramatically with antenna swaps, placement changes, or nearby RF devices

What to do:
- treat this as an RF environment problem first
- recheck gain, antenna placement, cabling, and overload conditions

Do not assume the software is tuning wrong if the symptom changes a lot with antenna and site conditions.

#### Quick decision test

- all stations shifted together:
  - suspect `freq_correction_ppm`
- strong stations duplicated elsewhere and clipping is nonzero:
  - suspect overload/image response
- problem sits near a strong adjacent station and improves with narrower bandwidth:
  - suspect adjacent leakage
- behavior changes dramatically with antenna or gain changes:
  - suspect RF front-end/site conditions

### 10. Choose The Right Profile For Your Location

Dense city / rooftop / strong locals:
- prefer overload protection
- start with `tef`, `agc_mode = 3`
- if still overloaded, try fixed `rtl_gain_db = 8 .. 16`
- keep `stereo_blend = normal` or `aggressive`

Suburban / mixed conditions:
- start with `tef`, `agc_mode = 2`
- if weak stations are too soft, try `agc_mode = 1`
- if noise on weak stereo is annoying, use `stereo_blend = aggressive`

Rural / weak-signal / DX (incl. indoor + telescopic antenna):
- start with `tef`, `agc_mode = 1`
- if still too weak, try fixed `rtl_gain_db = 24 .. 36` (R820T tops out near 49.6 dB; 36 dB is usually the sweet spot before adjacent strong locals overload)
- enable `dsp_agc = fast` — normalizes the IF envelope before the discriminator, helping the demodulator stay in its linear range on AM-modulated multipath/fading
- enable `adaptive_bandwidth = aggressive` — the DSP equivalent of TEF's adaptive IF; narrows the channel FIR when SNR drops, widens when it recovers (with hysteresis)
- if a station is right at the threshold of lock, try `w0_bandwidth_hz = 142000` or `95000` manually for that station
- if strong locals occasionally overload at high gain, back off by `3 .. 6 dB`
- keep `stereo_blend = normal` first; use `soft` only if stations are clean enough to justify it; use `aggressive` for cleanest mono-on-weak behavior

#### Reality check: antenna quality dominates

An external roof antenna can have 15–25 dB more gain (plus far less noise) than an indoor telescopic. A station that's "easy" on the external can be "below the noise floor" on the telescopic — that's RF physics, not a software regression. Verify with the same antenna across config changes before blaming DSP.

#### Reality check: R820T vs TEF sensitivity gap

This software runs on an RTL-SDR (R820T tuner). A dedicated TEF6686/TEF6687 hardware tuner has a lower-noise LNA, sharper analog IF filtering, and faster hardware AGC — roughly 10–15 dB more sensitive on fringe FM than R820T at the same antenna. The DSP knobs above close the gap as far as a software demod can; for deep DX an external antenna and ideally a TEF-based receiver remain the gold standard.

### 11. Recommended Validation Pass

After every major tuning change, verify all three:

Strong station:
- no obvious distortion
- `clip` near zero
- stable stereo lock

Medium station:
- still usable
- stereo remains stable if the station is actually strong enough

Weak or empty frequency:
- should not look unrealistically strong
- noise floor should behave consistently

If one setting improves one case while breaking the other two, it is not a good site profile.

### 12. When To Use Fixed Manual Gain

Use fixed `rtl_gain_db` when:
- antenna and site are stable
- you want repeatable comparisons
- you are benchmarking meter or scan behavior

Avoid fixed manual gain when:
- moving between locations
- changing antennas often
- local RF density varies a lot over time

For general listening, strategy-managed gain is usually easier to keep sane.

### 13. Optional Adaptive Features

The pipeline ships four optional features that all default to off (or transparent) and can be turned on per-site once the basic gain/blend baseline is solid. Turning them all on at once is supported and tested; doing it without a stable baseline first will hide whether the improvements are real.

| Key | Default | What it does | When to turn on |
|---|---|---|---|
| `pilot_canceller` | `true` | LMS-tracked 19 kHz tone removal from mono audio | Already on; turn off only to A/B |
| `hicut = gentle\|strong` | `off` | Narrows top-end de-emphasis when signal quality drops | Fringe stations that hiss above 5 kHz |
| `adaptive_bandwidth = conservative\|aggressive` | `off` | Closes channel FIR when SNR drops, reopens when it recovers | Crowded RF environments / adjacent-channel splatter |
| `multipath_eq = light\|aggressive` | `off` | CMA equalizer cancelling I/Q multipath ghosting | Indoor antenna, urban multipath, mobile reception |

Verification logs you should see when these are active:
- `[BW] adaptive: snr=… dB, requesting …` — adaptive bandwidth picking a new target
- `[BW] applied W… (previous W…)` — change committed by the runtime loop
- `[EQ] env_err=…` — multipath equalizer's smoothed envelope error (should be small on clean signals and trend downward on real multipath)

Worth knowing:
- The multipath equalizer adapts only while stereo lock is reported, so it never trains on noise. On a clean signal the dispersion target + leak regularization hold it near a centered-delta (transparent) filter — `env_err` will sit near zero.
- The adaptive bandwidth controller waits for at least one valid SNR measurement before its first policy decision (avoids spurious "snr=0 → narrow" at startup).
- HiCut's two-filter crossfade is smooth (50 ms time constant), so quality changes don't ring or click.

### 14. Fast Troubleshooting

Strong station sounds rough or crunchy:
- gain too high
- reduce gain first; do not touch stereo blend first

Weak stations never hold stereo:
- that may be correct behavior
- if strong stations also fail stereo, recheck gain, bandwidth, and pilot quality

Scan or meter looks wrong:
- fix clipping first
- then recalibrate `signal_floor_dbfs` / `signal_ceil_dbfs`

Stereo image is too narrow on clearly strong locals:
- gain may still be wrong
- if gain is healthy, check `quality`/`blend`, then try `stereo_blend = soft`

Stations appear on the wrong frequency:
- first eliminate clipping
- then determine whether the offset is systematic (`freq_correction_ppm`) or a strong-station duplicate (overload / image response)

Behavior changes when reconnecting XDR client:
- set `client_gain_allowed = false`
- keep all gain policy in `fm-sdr-tuner.ini`

## Running On Low-CPU Devices (Raspberry Pi etc.)

The full DSP pipeline at 256 kHz IQ rate is comfortably real-time on any 64-bit ARM since roughly the Raspberry Pi 4 era. Rough ballparks measured on Apple M1 Pro extrapolated by single-thread benchmark ratios:

| Target | Default config | Headroom |
|---|---|---|
| Apple M1 / M2 / M3 / M4 | ~15-20% of one core | Plenty |
| Modern x86 desktop / laptop | ~10-15% of one core | Plenty |
| Raspberry Pi 5 (Cortex-A76) | ~25-30% of one core | Comfortable |
| Raspberry Pi 4 (Cortex-A72) | ~45-55% of one core | Workable for dedicated-tuner use |
| Raspberry Pi 3 / Zero 2 W | ≥90% of one core | Marginal; expect occasional dropouts |
| Older / 32-bit ARM / Pi 1/2 | won't keep up real-time | Not supported |

If you're targeting a constrained device, these knobs reduce CPU without hurting audio fidelity on normal stations:

```ini
[processing]
# Keep these OFF — they add work on the inner DSP loop
adaptive_bandwidth = off
multipath_eq = off
hicut = off

# Optional: drop the LMS pilot canceller too. Default-on; setting it off
# saves a few cycles per sample at the cost of marginally more 19 kHz
# leakage in mono audio (well below audibility on real broadcasts).
pilot_canceller = false

# Stereo blend "aggressive" mutes stereo on weak signals quickly, avoiding
# work in the L-R recovery loop when there's no real stereo to recover.
stereo_blend = aggressive

[debug]
# Verbose logs aren't free at high block rates.
log_level = 0
```

Build advice:

```bash
# ALWAYS Release build on RPi — Debug is several times slower.
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Runtime advice:

- **Cooling matters.** RPi 4 throttles around 80 °C. A passive heatsink + airflow keeps the demod from stuttering under sustained load.
- **Prefer direct USB RTL-SDR** (`--source rtl_sdr`) over `rtl_tcp` even on the same machine — the TCP roundtrip adds CPU and latency.
- **USB 2.0 root port** is enough for 256 kHz, but avoid sharing the bus with other heavy USB devices.
- **Skip audio output** if you only need the XDR control stream / WAV / MPX. Pass `--no-audio` and feed downstream consumers from `-w` or `--mpx-wav`.
- **Avoid running other heavy workloads** (transcoders, browsers, GUIs) on the same Pi — the FM demod is real-time-sensitive.

If even with all the above the Pi can't keep up:

- Consider running `fm-sdr-tuner` on a more capable host and using `rtl_tcp` to pull IQ from the Pi (move the heavy DSP off the constrained device).
- Or use a TEF-based hardware tuner. A real TEF6686/TEF6687 receiver does the demod in its own DSP silicon and the Pi only handles XDR protocol — basically zero CPU.

## CMake Options

- `FM_TUNER_ENABLE_X86_AVX2=ON|OFF` (default `OFF`)

Notes:
- ALSA is always enabled on Linux builds.
- Audio backends in active use are native only: Core Audio (macOS), ALSA
  (Linux), WinMM (Windows).

## CI Status Notes

Current workflows exist for:
- Linux (`x64`, `arm64`)
- macOS (`macos-latest`)
- Windows (`windows-latest`, MinGW-w64 via MSYS2)

Artifacts currently uploaded by CI:
- Linux coverage job:
  - `fm-sdr-tuner-coverage-ubuntu-24.04` (`coverage.xml`, `coverage.html`)
- Linux package jobs:
  - `fm-sdr-tuner-linux-deb-ubuntu-24.04-x64`
  - `fm-sdr-tuner-linux-deb-ubuntu-24.04-arm64`
  - `fm-sdr-tuner-linux-deb-debian-trixie-x64`
  - `fm-sdr-tuner-linux-deb-debian-trixie-arm64`
  - `fm-sdr-tuner-linux-rpm-fedora-42-x64`
  - `fm-sdr-tuner-linux-rpm-fedora-42-arm64`
  - `fm-sdr-tuner-linux-rpm-fedora-43-x64`
  - `fm-sdr-tuner-linux-rpm-fedora-43-arm64`
- macOS build job:
  - `fm-sdr-tuner-macos` (binary + `README.md` + `fm-sdr-tuner.ini`)
- Windows MinGW job:
  - `fm-sdr-tuner-windows-mingw` (`.exe` + required DLLs + `dependencies.txt`)

Notes:
- Linux smoke-test jobs validate package installability in fresh containers, but
  they do not upload additional artifacts.
- Artifact names and platform matrix are defined in:
  - `.github/workflows/build-linux.yml`
  - `.github/workflows/build-macos.yml`
  - `.github/workflows/build-windows.yml`

Run local Linux package checks with Docker:

```bash
./scripts/test-linux-arm-builds.sh
```

This script runs container builds for `ubuntu:24.04`, `debian:trixie`,
`fedora:42`, and `fedora:43`, then performs package-install smoke tests in
fresh containers.

It validates:
- `.deb` build + install smoke test on Ubuntu and Debian
- `.rpm` build + install smoke test on Fedora
- runtime linkage (`ldd`) and basic CLI startup (`fm-sdr-tuner --help`)

If CI fails on dependencies, align workflow package installs with local requirements listed above.

## Based On / Dependencies

This software is based on or integrates ideas/components from:

- SDRPlusPlus (FM demodulation/stereo/RDS DSP references)
- redsea (RDS decoding pipeline; this project now uses redsea-derived components)
- XDR-GTK and librdsparser (XDR ecosystem compatibility/parsing behavior)
- FM-DX-Tuner and xdrd (protocol and tuner-control ecosystem references)
- sdr-j-fm (algorithmic references for pilot recovery without bandpass FIR, adaptive Costas-style stereo separator, and squelch — see roadmap items P18-P20 in `plan.md`)

Core third-party runtime/build dependencies used by this project include:

- `librtlsdr` (RTL-SDR device and rtl_tcp ecosystem support)
- `liquid-dsp` (required DSP primitive dependency used across FM and RDS paths)
- OpenSSL (authentication/security-related hashing/crypto usage)

### Component Table

| Component | Link | License | Role in this project |
|---|---|---|---|
| SDRPlusPlus | https://github.com/AlexandreRouma/SDRPlusPlus | GPL-3.0 | FM demodulation/stereo/RDS DSP reference base |
| redsea | https://github.com/windytan/redsea | ISC | RDS decoding pipeline (integrated/ported components) |
| liquid-dsp | https://github.com/jgaeddert/liquid-dsp | MIT | DSP primitives used across FM and RDS processing paths |
| XDR-GTK | https://github.com/kkonradpl/xdr-gtk | GPL-3.0 | XDR ecosystem/protocol behavior reference |
| librdsparser | https://github.com/kkonradpl/librdsparser | GPL-3.0 | RDS/XDR parsing reference in ecosystem |
| FM-DX-Tuner | https://github.com/kkonradpl/FM-DX-Tuner | GPL-3.0 | Protocol/tuner-control ecosystem reference |
| xdrd | https://github.com/kkonradpl/xdrd | GPL-2.0 | Original XDR daemon/protocol reference |
| sdr-j-fm | https://github.com/JvanKatwijk/sdr-j-fm | GPL-3.0 | Algorithmic references for pilot recovery without bandpass FIR, adaptive Costas-style stereo separator (PerfectStereoSeparation by Thomas Neder, 2023), and squelch behaviour. Roadmap items P18, P19, P20. |
| OpenSSL | https://www.openssl.org/ | Apache-2.0 (plus OpenSSL terms) | Auth/security-related crypto/hash usage |
| librtlsdr | https://github.com/osmocom/rtl-sdr | GPL-2.0 | RTL-SDR hardware I/O and rtl_tcp ecosystem support |

## License

GPLv3. See `LICENSE`.
