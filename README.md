# FM-SDR-Tuner

FM broadcast tuner and XDR server built around RTL-SDR IQ input.

Current architecture:
- Input: `rtl_sdr` (default) or `rtl_tcp`
- Demod/stereo/audio pipeline in-process
- RDS decode path based on redsea components (`liquid-dsp` required)
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
- Audio output is fixed at `32000 Hz`, buffer size `4096` frames.

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
- `dbf_compensation_factor`

Weak-signal tuning keys:
- `processing.w0_bandwidth_hz`
- `processing.dsp_agc = off|fast|slow`
- `processing.stereo_blend = soft|normal|aggressive`

## CMake Options

- `FM_TUNER_ENABLE_X86_AVX2=ON|OFF` (default `ON`)
- `FM_TUNER_ENABLE_PORTAUDIO=ON|OFF`

Notes:
- ALSA is always enabled on Linux builds.
- On macOS and Windows, native audio backends are forced and PortAudio is disabled in CMake (the `FM_TUNER_ENABLE_PORTAUDIO` toggle is ignored there).

## CI Status Notes

Current workflows exist for:
- Linux (`x64`, `arm64`)
- macOS
- Windows

Linux CI publishes real `.deb` artifacts for Ubuntu x64 and arm64 runners.

Run local arm64 Linux CI-equivalent builds with Docker:

```bash
./scripts/test-linux-arm-builds.sh
```

This script validates both `debian:trixie` and `fedora:40` arm64 container builds.

If CI fails on dependencies, align workflow package installs with local requirements listed above.

## Based On / Dependencies

This software is based on or integrates ideas/components from:

- SDRPlusPlus (FM demodulation/stereo/RDS DSP references)
- redsea (RDS decoding pipeline; this project now uses redsea-derived components)
- XDR-GTK and librdsparser (XDR ecosystem compatibility/parsing behavior)
- FM-DX-Tuner and xdrd (protocol and tuner-control ecosystem references)

Core third-party runtime/build dependencies used by this project include:

- `librtlsdr` (RTL-SDR device and rtl_tcp ecosystem support)
- `liquid-dsp` (required for current redsea-based RDS decode path)
- OpenSSL (authentication/security-related hashing/crypto usage)

### Component Table

| Component | Link | License | Role in this project |
|---|---|---|---|
| SDRPlusPlus | https://github.com/AlexandreRouma/SDRPlusPlus | GPL-3.0 | FM demodulation/stereo/RDS DSP reference base |
| redsea | https://github.com/windytan/redsea | ISC | RDS decoding pipeline (integrated/ported components) |
| liquid-dsp | https://github.com/jgaeddert/liquid-dsp | MIT | DSP primitives used by redsea-based RDS path |
| XDR-GTK | https://github.com/kkonradpl/xdr-gtk | GPL-3.0 | XDR ecosystem/protocol behavior reference |
| librdsparser | https://github.com/kkonradpl/librdsparser | GPL-3.0 | RDS/XDR parsing reference in ecosystem |
| FM-DX-Tuner | https://github.com/kkonradpl/FM-DX-Tuner | GPL-3.0 | Protocol/tuner-control ecosystem reference |
| xdrd | https://github.com/kkonradpl/xdrd | GPL-2.0 | Original XDR daemon/protocol reference |
| OpenSSL | https://www.openssl.org/ | Apache-2.0 (plus OpenSSL terms) | Auth/security-related crypto/hash usage |
| librtlsdr | https://github.com/osmocom/rtl-sdr | GPL-2.0 | RTL-SDR hardware I/O and rtl_tcp ecosystem support |

## License

GPLv3. See `LICENSE`.
