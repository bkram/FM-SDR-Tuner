# FM-SDR-Tuner

A cross-platform SDR FM tuner that bridges rtl_tcp hardware to the XDR/FM-DX protocol ecosystem. Control any RTL-SDR (or other rtl_tcp source) remotely using FM-DX-Webserver, XDR-GTK, or similar clients as if it were a native hardware tuner.

> **Note**: While this works great, the quality will not match a hardware tuner like a TEF.

## Features

- RTL-TCP Input - Connect to any rtl_tcp server (local or network)
- FM Stereo Demodulation - PLL-based 19kHz pilot detection using SDR++ algorithms
- RDS Decoding - Decodes RDS groups in a background thread
- XDR Protocol Server - Compatible with XDR-GTK and FM-DX-Webserver clients on port 7373
- Audio Output - Audio output via ALSA (Linux) or PortAudio (macOS), or WAV file recording
- SIMD Acceleration - Optimized DSP paths for x86 (SSE/AVX) and ARM (NEON)

## Requirements

- C++17 compiler
- CMake 3.15+
- OpenSSL

### macOS

```bash
brew install openssl portaudio
```

### Linux (Debian/Ubuntu)

```bash
sudo apt install libssl-dev pkg-config libasound2-dev
```

### Linux (Fedora)

```bash
sudo dnf install openssl-devel alsa-lib-devel
```

## Building

### macOS

Apple Silicon build:

```bash
mkdir build && cd build
cmake .. -DCMAKE_OSX_ARCHITECTURE=arm64
make
```

Intel (x86_64) build:

```bash
mkdir build && cd build
cmake .. -DCMAKE_OSX_ARCHITECTURE=x86_64
make
```

### Linux

```bash
mkdir build && cd build
cmake ..
make
```

### Windows

With vcpkg:

```bash
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
make
```

Or with manual PortAudio:

```bash
cmake .. -DPORTAUDIO_INCLUDE_DIR=C:/path/to/portaudio/include -DPORTAUDIO_LIBRARY=C:/path/to/portaudio/lib/portaudio.lib
```

### AVX2/FMA Notes

- AVX2/FMA is disabled by default for portable x86 binaries.
- Enable if target CPU has AVX2:

```bash
cmake .. -DFM_TUNER_ENABLE_X86_AVX2=ON
```

## Usage

### Prerequisites

Install rtl-sdr (provides rtl_tcp):

| OS      | Install Command |
|---------|----------------|
| macOS   | `brew install rtl-sdr` |
| Linux   | `sudo apt install rtl-sdr` or `sudo dnf install rtl-sdr` |
| Windows | Download [rtl-sdr-blog release](https://github.com/rtlsdrblog/rtl-sdr-blog/releases) (includes rtl_tcp.exe), use [zadig](https://zadig.akeo.ie/) to install USB driver |

### Quick Start

```bash
# Terminal 1: Start rtl_tcp
rtl_tcp -p 1234 -f 88600000 -g 20 -s 512000

# Terminal 2: Run FM tuner
./fm-sdr-tuner -t localhost:1234 -f 88600 -s
```

### Command Line Options

| Option                      | Description                  | Default        |
|-----------------------------|-------------------------------|---------------|
| `-t, --tcp <host:port>`    | rtl_tcp server address       | localhost:1234 |
| `-f, --freq <khz>`         | Frequency in kHz             | 88600         |
| `-g, --gain <db>`          | RTL-SDR gain in dB           | auto          |
| `-w, --wav <file>`         | Output WAV file              | -             |
| `-i, --iq <file>`          | Capture raw IQ bytes to file | -             |
| `-s, --audio`              | Enable audio output          | disabled      |
| `-l, --list-audio`        | List available audio devices | -             |
| `-d, --device <id>`       | Audio output device          | default       |
| `-P, --password <pwd>`    | XDR server password          | -             |
| `-G, --guest`             | Enable guest mode            | disabled      |
| `-h, --help`              | Show help                    | -             |

### Examples

Record to WAV file, for testing:

```bash
./fm-sdr-tuner -t localhost:1234 -f 101100 -w output.wav
```

Play on audio device:

```bash
./fm-sdr-tuner -t localhost:1234 -f 101100 -s
```

With XDR password protection:

```bash
./fm-sdr-tuner -t localhost:1234 -f 101100 -s -P mypassword
```

### INI Configuration

Run with:

```bash
./fm-sdr-tuner -c fm-sdr-tuner.ini -s
```

Key sections:

- `[rtl_tcp]`: RTL-TCP host/port.
- `[sdr]`: SDR hardware-specific settings.
- `[tuner]`: Station/audio settings (frequency, deemphasis).
- `[processing]`: DSP/control behavior.

## Gain Strategies

Two gain strategies are supported:

### SDR++ Style (RTL AGC)

```ini
[sdr]
gain_strategy = sdrpp
sdrpp_rtl_agc = true
sdrpp_rtl_agc_gain_db = 18
```

Uses RTL hardware AGC with fixed IF gain (0-28 dB). Run with `-g -1` to enable.

### TEF Style (Manual IF Gain)

```ini
[sdr]
gain_strategy = tef
```

Uses manual IF gain with TEF-like A profiles:

| A-Mode | Gain (dB) |
|--------|------------|
| A0     | 44         |
| A1     | 36         |
| A2     | 30 (default) |
| A3     | 24         |

Run with `-g -1` for auto (A2=30dB), or `-g 36` for specific gain.

## Audio Loopback

An audio loopback (virtual audio cable) creates a virtual audio device that
captures output from one application and makes it available as input to
another. This lets you pipe FM-SDR-Tuner audio to other apps like
FM-DX-Webserver.

| OS      | Tool                        |
|---------|-----------------------------|
| Linux   | ALSA loopback kernel module |
| macOS   | BlackHole                  |
| Windows | VB-Audio Virtual Cable     |

To use: install the virtual audio device for your OS (Linux: `sudo modprobe snd-aloop`,
macOS: `brew install blackhole-2ch`, Windows: VB-Audio Virtual Cable), then set the
audio device in your config file:

```ini
[audio]
device = your_loopback_device
```

Finally, configure your target application to use the loopback device as input.

## Architecture

```text
rtl_tcp Server ----> RTL-TCP Client ----> FM Demod ----> Stereo Decoder
                              |                    |
                              |                    v
                              |               RDS Decoder
                              |                    |
                              v                    v
                        XDR Server <---- AF Post Processor ----> Audio Output
```

Components:

- **RTL-TCP Client**: Connects to rtl_tcp server, receives IQ samples
- **FM Demod**: Quadrature demodulation (atan2 phase detector)
- **Stereo Decoder**: PLL-based 19kHz pilot detection, stereo/mono blend
- **RDS Decoder**: Background thread decoding RDS groups
- **AF Post Processor**: Deemphasis, resampling, volume
- **Audio Output**: ALSA/PortAudio speaker or WAV file recording
- **XDR Server**: Handles remote client connections on port 7373

## Testing

```bash
pip install pytest numpy
pytest tests/
```

## Credits

This project is based on or modeled after the following open source projects:

| Project      | Description                                                                              | Link                                           |
|--------------|------------------------------------------------------------------------------------------|------------------------------------------------|
| SDRPlusPlus  | FM demodulation (quadrature), stereo decoder (PLL-based pilot detection), RDS decoding   | https://github.com/AlexandreRouma/SDRPlusPlus |
| XDR-GTK      | XDR protocol client implementation, RDS parser                                         | https://github.com/kkonradpl/xdr-gtk          |
| librdsparser | RDS parsing library (submodule of XDR-GTK)                                              | https://github.com/kkonradpl/librdsparser      |
| FM-DX-Tuner  | TEF tuner firmware, FM-DX protocol reference                                              | https://github.com/kkonradpl/FM-DX-Tuner       |
| xdrd         | Original XDR daemon protocol implementation                                             | https://github.com/kkonradpl/xdrd             |

## License

GNU General Public License v3.0 - see [LICENSE](LICENSE) file

## Third-Party Licenses

This software includes the following third-party components:

| Component    | License          | Notes                                                                                     |
|--------------|------------------|-------------------------------------------------------------------------------------------|
| OpenSSL      | Apache 2.0 / SSLeay | See [LICENSE](LICENSE) and [OpenSSL license](https://www.openssl.org/source/license.html) |
| PortAudio    | MIT              |                                                                                           |
| SDRPlusPlus  | GPLv3            |                                                                                           |
| XDR-GTK      | GPLv3            |                                                                                           |
| FM-DX-Tuner  | GPLv3            |                                                                                           |
| xdrd         | GPLv2            |                                                                                           |

For binary distributions, include the LICENSE file and this attribution notice.
