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
- FM stereo demod with runtime bandwidth/deemphasis control
- RDS decode in dedicated worker thread
- XDR protocol compatibility for FM-DX clients
- Output to speaker (`-s`), WAV (`-w`), and/or IQ capture (`-i`)

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

Use vcpkg and install at least OpenSSL plus `librtlsdr` and `liquid-dsp` for your triplet.

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

## Runtime Behavior

- At least one output must be enabled (`audio`, `wav`, or `iq`).
- The tuner does not auto-start by default; an XDR client start command activates it.
- Default source is direct RTL-SDR (`rtl_sdr`).
- Audio output sample rate is fixed at `32000 Hz`.
- Device/buffer latency is backend-specific (Core Audio / ALSA / WinMM).

## Config-First Usage

Primary workflow is config-driven:

```bash
./build/fm-sdr-tuner -c fm-sdr-tuner.ini
```

Recommended:
- Put all normal runtime settings in `fm-sdr-tuner.ini`.
- Use CLI only for temporary overrides during testing.

Useful override examples:

```bash
# temporary frequency override
./build/fm-sdr-tuner -c fm-sdr-tuner.ini -f 101100

# list audio devices once, then keep device in config
./build/fm-sdr-tuner -l
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
- `signal_floor_dbfs`
- `signal_ceil_dbfs`
- `signal_bias_db`

Weak-signal tuning keys:
- `processing.w0_bandwidth_hz`
- `processing.dsp_agc = off|fast|slow`
- `processing.stereo_blend = soft|normal|aggressive`

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

Recommended baseline for most users:

```ini
[sdr]
gain_strategy = tef
rtl_gain_db = -1
default_custom_gain_flags = 1
freq_correction_ppm = 0
signal_floor_dbfs = -55.0
signal_ceil_dbfs = -12.0
signal_bias_db = -6.0

[processing]
agc_mode = 2
w0_bandwidth_hz = 194000
dsp_agc = off
stereo_blend = normal
stereo = true
client_gain_allowed = false
```

Why this baseline:
- `tef` is the best default when local RF conditions are not yet known
- `agc_mode = 2` is a stable middle ground
- `w0_bandwidth_hz = 194000` is the correct starting point for normal WFM stereo
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
- `signal_floor_dbfs`
- `signal_ceil_dbfs`
- `signal_bias_db`

They do not fix RF overload, wrong tuning, or ghosting.

Procedure:
1. Tune an empty or very weak frequency and note typical `dbfs`
2. Tune a strong clean local station and note typical `dbfs`
3. Set:
   - `signal_floor_dbfs` near the empty-frequency value
   - `signal_ceil_dbfs` near the strong-station value
   - `signal_bias_db` only if you want the displayed scale nudged up or down

Example:
- empty frequency around `-52 dBFS`
- strong local around `-14 dBFS`

Starting point:

```ini
[sdr]
signal_floor_dbfs = -52.0
signal_ceil_dbfs = -14.0
signal_bias_db = -6.0
```

Symptoms of bad meter calibration:
- empty channels always look too strong: floor is too high, or gain is still wrong
- every strong station saturates near the top: ceil is too low
- everything looks compressed into the middle: floor/ceil window is too wide

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

Rural / weak-signal / DX:
- start with `tef`, `agc_mode = 1`
- if still too weak, try fixed `rtl_gain_db = 24 .. 36`
- if strong locals occasionally overload, back off by `3 .. 6 dB`
- keep `stereo_blend = normal` first; use `soft` only if stations are clean enough to justify it

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

### 13. Fast Troubleshooting

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
  - `fm-sdr-tuner-linux-rpm-fedora-40-x64`
  - `fm-sdr-tuner-linux-rpm-fedora-40-arm64`
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

This script runs container builds for `ubuntu:24.04`, `debian:trixie`, and
`fedora:40`, then performs package-install smoke tests in fresh containers.

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
| OpenSSL | https://www.openssl.org/ | Apache-2.0 (plus OpenSSL terms) | Auth/security-related crypto/hash usage |
| librtlsdr | https://github.com/osmocom/rtl-sdr | GPL-2.0 | RTL-SDR hardware I/O and rtl_tcp ecosystem support |

## License

GPLv3. See `LICENSE`.
