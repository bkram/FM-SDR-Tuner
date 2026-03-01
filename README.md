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

## Gain Setup Guide (Per Location)

Most "bad reception" reports in FM SDR setups come from gain mismatch:
- too much gain in strong-signal areas -> clipping, distortion, unstable scan
- too little gain in weak-signal areas -> noisy audio, missing stereo/RDS

Use this section to set gain for your specific RF environment.

### 1. Understand The Gain Controls

Main keys in `[sdr]` and `[processing]`:
- `gain_strategy = tef|sdrpp`
- `rtl_gain_db = -1` (strategy-managed) or fixed manual dB value
- `default_custom_gain_flags` (TEF strategy only)
- `sdrpp_rtl_agc`, `sdrpp_rtl_agc_gain_db` (SDR++ strategy only)
- `agc_mode` in `[processing]` (TEF strategy only, 0..3)

Practical meaning:
- `tef`:
  - best default for mixed conditions
  - supports runtime AGC profile (`agc_mode`) and clip-protect behavior
- `sdrpp`:
  - predictable "set and hold" style
  - easier when you want fixed behavior similar to SDR++ workflows
- `rtl_gain_db`:
  - `-1` = let selected strategy manage gain
  - `>=0` = force fixed tuner gain (overrides strategy behavior)

### 2. Pick A Baseline Profile

Start with one of these, then adjust from there.

Baseline A (recommended for most users):

```ini
[sdr]
gain_strategy = tef
rtl_gain_db = -1
default_custom_gain_flags = 1

[processing]
agc_mode = 2
```

Baseline B (manual-like behavior):

```ini
[sdr]
gain_strategy = sdrpp
rtl_gain_db = -1
sdrpp_rtl_agc = false
sdrpp_rtl_agc_gain_db = 18
```

### 3. Choose By RF Density (Your Location Type)

Use the profile that matches your area, then fine-tune.

Dense urban / many strong locals / rooftop antenna:
- Goal: prevent front-end overload and clipping
- Use:
  - `gain_strategy = tef`
  - `rtl_gain_db = -1`
  - `agc_mode = 3` (or `2` if too insensitive)
  - keep `default_custom_gain_flags = 1`
- If still overloaded:
  - try fixed `rtl_gain_db = 8` to `16`
  - lower antenna gain or add attenuation externally

Suburban / mixed conditions (strong + medium stations):
- Goal: stable all-round decode
- Use:
  - `gain_strategy = tef`
  - `rtl_gain_db = -1`
  - `agc_mode = 2` (default)
- If weak stations are too noisy:
  - try `agc_mode = 1`
- If strong stations distort:
  - move back to `agc_mode = 3` or set fixed `rtl_gain_db` around `12..20`

Rural / weak-signal DX area:
- Goal: maximize sensitivity without constant clipping
- Use:
  - `gain_strategy = tef`
  - `rtl_gain_db = -1` first
  - `agc_mode = 1` (or `0` for very weak environments)
- If still too weak:
  - try fixed `rtl_gain_db = 24` to `36`
- If clipping appears on occasional strong locals:
  - reduce fixed gain by 3..6 dB

### 4. Validate With A 3-Station Check

After each gain change, test:
- one very strong local station
- one medium station
- one weak/distant station

What to listen for:
- strong station should be clean (no gritty distortion, pumping, crackle)
- medium station should keep stable stereo if available
- weak station should improve SNR without making strong station unusable

If available in your client view, also watch clipping/level behavior:
- frequent high clipping indicators = too much gain
- constantly very low levels and noisy demod = too little gain

### 5. Calibrate Signal Meter For Your Site

These do not change RF performance directly, but they make meter/scan values
meaningful for your installation:
- `signal_floor_dbfs`
- `signal_ceil_dbfs`
- `signal_bias_db`

Suggested process:
1. Tune a no-signal frequency and note baseline.
2. Tune a strong clean local station and note top-end.
3. Set:
   - `signal_floor_dbfs` near typical no-signal noise floor
   - `signal_ceil_dbfs` near strong-local level
   - `signal_bias_db` to align displayed values with your expected dBf-like range

Starting point that works for many setups:

```ini
[sdr]
signal_floor_dbfs = -55.0
signal_ceil_dbfs = -12.0
signal_bias_db = -6.0
```

If your scan looks mostly empty except spikes:
- first fix gain/overload as above
- then widen meter window slightly (for example lower `signal_floor_dbfs`)

### 6. When To Use Fixed `rtl_gain_db`

Use fixed manual gain when:
- your antenna/system is stable and you want repeatable results
- you are doing comparisons/logging and need reproducible levels

Avoid fixed manual gain when:
- traveling between locations
- switching antennas frequently
- local RF conditions vary widely over time

### 7. Client Control Safety

If users control gain from XDR clients and this causes confusion:
- set `client_gain_allowed = false` in `[processing]`
- keep gain policy only in `fm-sdr-tuner.ini`

This prevents accidental client-side `A`/`G` changes from overriding your tuned
site profile.

### 8. Fast Troubleshooting

Audio sounds distorted on strong stations:
- lower gain (`agc_mode` higher number in TEF, or lower fixed `rtl_gain_db`)

Weak stations vanish/no stereo anywhere:
- increase gain (`agc_mode` lower number in TEF, or higher fixed `rtl_gain_db`)

Behavior changes wildly when reconnecting client:
- disable client gain control: `client_gain_allowed = false`

Scan graph looks unrealistic:
- verify gain first
- then recalibrate `signal_floor_dbfs` / `signal_ceil_dbfs` / `signal_bias_db`

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
