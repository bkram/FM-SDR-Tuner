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

## Important Runtime Behavior

- At least one output must be selected: `-s` and/or `-w <file>` and/or `-i <file>`.
- The tuner does **not** auto-start by default. It starts when an XDR client sends a start command.
- Default source is direct RTL-SDR (`--source rtl_sdr`).
- You can fully select source/device via config (`[tuner] source`, `[tuner] rtl_device`) and run with only `-c`.
- Audio output format is fixed in current architecture:
  - sample rate: `32000`
  - frames per buffer: `4096`

## Usage

```bash
./fm-sdr-tuner [options]
```

### CLI Options

| Option | Description | Default |
|---|---|---|
| `-c, --config <file>` | INI config file | none |
| `-t, --tcp <host:port>` | rtl_tcp server address | `localhost:1234` |
| `--source <rtl_tcp\|rtl_sdr>` | tuner source | `rtl_sdr` |
| `--rtl-device <id>` | RTL-SDR device index for direct mode | `0` |
| `-f, --freq <khz>` | frequency in kHz | `88600` |
| `-g, --gain <db>` | RTL gain dB (`-1` = auto strategy) | from config |
| `-w, --wav <file>` | write WAV output | off |
| `-i, --iq <file>` | write raw IQ bytes | off |
| `-s, --audio` | enable speaker output | from config (`[audio].enable_audio`) |
| `-l, --list-audio` | list available audio devices | off |
| `-d, --device <id/name>` | audio output device selector | system default |
| `-P, --password <pwd>` | XDR server password | from config |
| `-G, --guest` | allow guest mode | off |
| `-h, --help` | show help | off |

### Examples

List audio devices:

```bash
./build/fm-sdr-tuner -l
```

Direct RTL-SDR, WAV capture:

```bash
./build/fm-sdr-tuner --source rtl_sdr --rtl-device 0 -f 88600 -w test.wav
```

rtl_tcp source + speaker:

```bash
rtl_tcp -p 1234 -f 88600000 -g 20 -s 512000
./build/fm-sdr-tuner --source rtl_tcp -t localhost:1234 -f 88600 -s
```

Direct RTL-SDR + speaker + IQ capture:

```bash
./build/fm-sdr-tuner --source rtl_sdr -f 101100 -s -i capture.iq
```

## Configuration (`fm-sdr-tuner.ini`)

Supported sections/keys:

- `[rtl_tcp]`
  - `host`, `port`
- `[audio]`
  - `enable_audio` (start with speaker output enabled without `-s`)
  - `device`
- `[sdr]`
  - `rtl_gain_db`
  - `default_custom_gain_flags`
  - `gain_strategy` (`tef` or `sdrpp`)
  - `sdrpp_rtl_agc`
  - `sdrpp_rtl_agc_gain_db`
  - `signal_floor_dbfs`
  - `signal_ceil_dbfs`
- `[tuner]`
  - `source` (`rtl_sdr` or `rtl_tcp`)
  - `rtl_device` (RTL-SDR index for direct mode)
  - `default_freq`
  - `deemphasis` (`0=50us`, `1=75us`, `2=off`)
- `[xdr]`
  - `port`, `password`, `guest_mode`
- `[processing]`
  - `agc_mode`
  - `client_gain_allowed`
  - `dsp_block_samples` (fixed DSP block size, clamped to `1024..32768`)
  - `w0_bandwidth_hz` (fallback bandwidth for `W0` in Hz; set `0` for widest)
  - `stereo_blend` (`soft`, `normal`, `aggressive`)
  - `stereo`
- `[debug]`
  - `log_level`
- `[reconnection]`
  - `auto_reconnect`

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
