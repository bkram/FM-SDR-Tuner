# Developer Guide

## Building

### macOS

Apple Silicon:

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_OSX_ARCHITECTURE=arm64
cmake --build .
```

Intel:

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

### Windows

MinGW-w64/MSYS2 is supported in CI. Install toolchain and deps, then:

```bash
mkdir -p build && cd build
cmake ..
cmake --build .
```

If you use vcpkg locally, point CMake to the vcpkg toolchain:

```bash
cmake .. -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
```

AVX2/FMA is disabled by default (`FM_TUNER_ENABLE_X86_AVX2=OFF`).
Enable only when targeting known AVX2/FMA-capable CPUs:

```bash
cmake .. -DFM_TUNER_ENABLE_X86_AVX2=ON
```

## Running

### Prerequisites

At least one output must be enabled (`-s`, `-w`, or `-i`).

Default source is direct RTL-SDR (`rtl_sdr`).

If using `rtl_tcp`, start server first:

```bash
rtl_tcp -p 1234 -f 88600000 -g 20 -s 512000
```

### Command Line Options

| Flag | Description | Default |
|------|-------------|---------|
| `-c, --config <file>` | INI config file | - |
| `-t, --tcp <host:port>` | rtl_tcp server | localhost:1234 |
| `--iq-rate <rate>` | IQ sample rate (`256000/1024000/2048000`) | 256000 |
| `--source <rtl_sdr\|rtl_tcp>` | Tuner source | rtl_sdr |
| `--rtl-device <id>` | RTL-SDR device index | 0 |
| `-f, --freq <khz>` | Frequency kHz | 88600 |
| `-g, --gain <db>` | RTL-SDR gain dB | auto |
| `-w, --wav <file>` | Output WAV file | - |
| `-i, --iq <file>` | Capture raw IQ bytes | - |
| `-s, --audio` | Enable audio output | disabled |
| `-d, --device <id|name>` | Audio output device selector | default |
| `--low-latency-iq` | Prefer newest IQ samples under overload | disabled |
| `-P, --password` | XDR server password | - |
| `-G, --guest` | Guest mode (no auth) | disabled |

### Examples

```bash
# Config-first run
./fm-sdr-tuner -c fm-sdr-tuner.ini

# rtl_tcp source + speaker output
./fm-sdr-tuner --source rtl_tcp -t localhost:1234 -f 101100 -s

# Record WAV + IQ
./fm-sdr-tuner -f 101100 -w output.wav -i capture.iq
```

## Project Structure

```
src/
  main.cpp              - Application entry, CLI parsing, main loop
  rtl_sdr_device.cpp    - Direct RTL-SDR source
  rtl_tcp_client.cpp    - RTL-TCP network client
  fm_demod.cpp         - FM quadrature demodulation
  stereo_decoder.cpp    - PLL-based stereo decoder
  af_post_processor.cpp - Audio post-processing, resampling
  rds_decoder.cpp       - RDS group decoding
  xdr_server.cpp        - XDR protocol server (port 7373)
  audio_output.cpp      - Audio output + WAV writer
  cpu_features.cpp     - CPU capability detection
  signal_level.cpp      - RF level estimation/mapping
  dsp/                  - liquid-dsp wrapper primitives/runtime

include/               - Header files

Not committed in this repository
research/              - Reference implementations
  SDRPlusPlus/         - FM demod, stereo, RDS algorithms
  xdr-gtk/             - XDR protocol client
  FM-DX-Tuner/         - TEF tuner firmware
  xdrd/                - Original XDR daemon
```

## Key Design Decisions

- **SIMD**: DSP paths have SSE/AVX (x86) and NEON (ARM) optimized code behind `#ifdef`
- **Audio**: Native backends: Core Audio (macOS), ALSA (Linux), WinMM (Windows), plus WAV output
- **XDR Protocol**: Compatible with XDR-GTK and FM-DX-Webserver clients on port 7373
- **RDS**: Decoded in background thread to avoid real-time audio drops

## Code Style

- C++17 standard
- No comments unless required for understanding
- Use existing libraries/algorithms where practical
- Follow patterns in existing source files

## Code Quality

### Tests

Run full test suite:

```bash
cd build
ctest --output-on-failure
```

Coverage build:

```bash
cmake -S . -B build-coverage -DCMAKE_BUILD_TYPE=Debug -DFM_TUNER_ENABLE_COVERAGE=ON -DFM_TUNER_ENABLE_X86_AVX2=OFF
cmake --build build-coverage
cd build-coverage
ctest --output-on-failure
gcovr --root .. . --txt --txt-summary
```

### clang-tidy (Static Analysis)

Run manually:

```bash
clang-tidy src/*.cpp -- -Iinclude -I/usr/local/include
```

Or integrate with CMake (run from build directory):

```bash
cmake .. -DCMAKE_CXX_CLANG_TIDY=clang-tidy
cmake --build .
```

### clang-format (Code Formatting)

Format a single file:

```bash
clang-format -i src/main.cpp
```

Format all source files:

```bash
clang-format -i src/*.cpp include/**/*.h
```

## License

GPLv3 - see LICENSE file. Derived from SDRPlusPlus, XDR-GTK, FM-DX-Tuner, and xdrd.
