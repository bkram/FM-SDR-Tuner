# Developer Guide

## Building

### macOS

Apple Silicon:

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_OSX_ARCHITECTURE=arm64
make
```

Intel:

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_OSX_ARCHITECTURE=x86_64
make
```

### Linux

```bash
mkdir -p build && cd build
cmake ..
make
```

### Windows

Requires vcpkg:

```bash
vcpkg install portaudio openssl
mkdir -p build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
make
```

Manual PortAudio/OpenSSL paths (if auto-detection fails):

```bash
cmake .. -DPORTAUDIO_INCLUDE_DIR=C:/path/to/portaudio/include -DPORTAUDIO_LIBRARY=C:/path/to/portaudio/lib/portaudio.lib
```

AVX2/FMA is disabled by default for portable x86 binaries. Enable if target CPU has AVX2:

```bash
cmake .. -DFM_TUNER_ENABLE_X86_AVX2=ON
```

## Running

### Prerequisites

Start an rtl_tcp server first:

```bash
rtl_tcp -p 1234 -f 88600000 -g 20 -s 512000
```

### Command Line Options

| Flag | Description | Default |
|------|-------------|---------|
| `-t, --tcp <host:port>` | rtl_tcp server | localhost:1234 |
| `-f, --freq <khz>` | Frequency kHz | 88600 |
| `-g, --gain <db>` | RTL-SDR gain dB | auto |
| `-w, --wav <file>` | Output WAV file | - |
| `-s, --audio` | Enable audio output | disabled |
| `-P, --password` | XDR server password | - |
| `-G, --guest` | Guest mode (no auth) | disabled |

### Examples

```bash
# Play audio
./fm-sdr-tuner -t localhost:1234 -f 101100 -s

# Record to WAV
./fm-sdr-tuner -t localhost:1234 -f 101100 -w output.wav
```

## Project Structure

```
src/
  main.cpp              - Application entry, CLI parsing, main loop
  rtl_tcp_client.cpp    - RTL-TCP network client
  fm_demod.cpp         - FM quadrature demodulation
  stereo_decoder.cpp    - PLL-based stereo decoder
  af_post_processor.cpp - Audio post-processing, resampling
  rds_decoder.cpp       - RDS group decoding
  xdr_server.cpp        - XDR protocol server (port 7373)
  audio_output.cpp     - PortAudio/WAV output
  cpu_features.cpp     - CPU capability detection

include/               - Header files

Not commited in this repository
research/              - Reference implementations
  SDRPlusPlus/         - FM demod, stereo, RDS algorithms
  xdr-gtk/             - XDR protocol client
  FM-DX-Tuner/         - TEF tuner firmware
  xdrd/                - Original XDR daemon
```

## Key Design Decisions

- **SIMD**: DSP paths have SSE/AVX (x86) and NEON (ARM) optimized code behind `#ifdef`
- **Audio**: PortAudio for cross-platform audio output; WAV for recording
- **XDR Protocol**: Compatible with XDR-GTK and FM-DX-Webserver clients on port 7373
- **RDS**: Decoded in background thread to avoid real-time audio drops

## Code Style

- C++17 standard
- No comments unless required for understanding
- Use existing libraries (SDR++ algorithms, not custom implementations)
- Follow patterns in existing source files

## License

GPLv3 - see LICENSE file. Derived from SDRPlusPlus, XDR-GTK, FM-DX-Tuner, and xdrd.
