# FM Tuner SDR

A cross-platform SDR FM tuner that bridges rtl_tcp hardware to the XDR/FM-DX protocol ecosystem. Connect any RTL-SDR (or other rtl_tcp source) and control it remotely using FM-DX-Webserver, XDR-GTK, or similar clients as if it were a native hardware tuner.

## Features

- RTL-TCP Input - Connect to any rtl_tcp server (local or network)
- FM Stereo Demodulation - PLL-based 19kHz pilot detection using SDR++ algorithms
- RDS Decoding - Decodes RDS groups in a background thread
- XDR Protocol Server - Compatible with XDR-GTK and FM-DX-Webserver clients on port 7373
- Audio Output - Audio output via PortAudio or WAV file recording
- SIMD Acceleration - Optimized DSP paths for x86 (SSE/AVX) and ARM (NEON)

## Requirements

- C++17 compiler
- CMake 3.15+
- OpenSSL
- PortAudio

On macOS with Homebrew:

```bash
brew install openssl portaudio
```

On Windows with vcpkg:

```bash
vcpkg install portaudio openssl
```

On Linux (Debian/Ubuntu):

```bash
sudo apt install libssl-dev libssl-dev pkg-config portaudio19-dev
```

On Linux (Fedora):

```bash
sudo dnf install openssl-devel portaudio-devel
```

## Building

### macOS

Apple Silicon (M1/M2/M3):

```bash
mkdir build && cd build
cmake .. -DCMAKE_OSX_ARCHITECTURE=arm64
make
```

Intel:

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

- AVX2/FMA is enabled automatically on x86 builds when supported by the compiler (`FM_TUNER_ENABLE_X86_AVX2=ON` by default).
- To disable this behavior:

```bash
cmake .. -DFM_TUNER_ENABLE_X86_AVX2=OFF
```

## Usage

### Prerequisites

Install rtl-sdr (provides rtl_tcp):

| OS | Install |
|---|---------|
| macOS | `brew install rtl-sdr` |
| Linux | `sudo apt install rtl-sdr` or `sudo dnf install rtl-sdr` |
| Windows | Download [rtl-sdr-blog release](https://github.com/rtlsdrblog/rtl-sdr-blog/releases) (includes rtl_tcp.exe), use [zadig](https://zadig.akeo.ie/) to install USB driver |

### Quick Start

```bash
# Terminal 1: Start rtl_tcp
rtl_tcp -p 1234 -f 88600000 -g 20 -s 1020000

# Terminal 2: Run FM tuner
./fm-tuner-sdr -t localhost:1234 -f 88600 -s
```

### Command Line Options

| Option | Description | Default |
|--------|--------------|---------|
| `-t, --tcp <host:port>` | rtl_tcp server address | localhost:1234 |
| `-f, --freq <khz>` | Frequency in kHz | 88600 |
| `-g, --gain <db>` | RTL-SDR gain in dB | auto |
| `-w, --wav <file>` | Output WAV file | - |
| `-s, --audio` | Enable audio output | disabled |
| `-P, --password <pwd>` | XDR server password | - |
| `-G, --guest` | Enable guest mode (no password) | disabled |
| `-h, --help` | Show help | - |

### Examples

Record to WAV file, for testing:

```bash
./fm-tuner-sdr -t localhost:1234 -f 101100 -w output.wav
```

Play on audio device:

```bash
./fm-tuner-sdr -t localhost:1234 -f 101100 -s
```

With XDR password protection:

```bash
./fm-tuner-sdr -t localhost:1234 -f 101100 -s -P mypassword
```

### INI Configuration

Run with:

```bash
./fm-tuner-sdr -c fm-tuner-sdr.ini -s
```

Key sections:

- `[rtl_tcp]`: RTL-TCP host/port.
- `[sdr]`: SDR hardware-specific settings.
- `[tuner]`: Station/audio settings (frequency, deemphasis).
- `[processing]`: DSP/control behavior.

Important SDR/AGC keys:

```ini
[sdr]
# -1 = auto/AGC profile path, otherwise fixed manual RTL gain in dB
rtl_gain_db = -1
# RF level calibration for 0..90 mapping
signal_floor_dbfs = -55.0
signal_ceil_dbfs = -19.0

[processing]
# 0..3 AGC profile used by this app's RTL mapping
agc_mode = 2

# If false, remote client A/G commands are ignored
client_gain_allowed = false

[audio]
# Underflow fade tail in milliseconds (0..50)
underflow_fade_ms = 8
# Optional short-impulse click limiter in main path (can color transients)
click_suppressor = false

[rds]
# strict|balanced|loose
aggressiveness = loose
# AGC smoothing factors (closer to 1.0 = slower changes)
agc_attack = 0.992
agc_release = 0.9996
# Group debounce thresholds for lock state
lock_acquire_groups = 2
lock_loss_groups = 14
```

`agc_mode` mapping (used when `rtl_gain_db = -1` and IMS/IF auto bit is not set):

- `A0` -> ~44 dB
- `A1` -> ~36 dB
- `A2` -> ~30 dB
- `A3` -> ~24 dB

Notes:

- If `rtl_gain_db >= 0`, fixed manual gain is used and AGC profile selection is bypassed.
- CLI `-g` also forces manual gain and overrides INI gain behavior.

## Audio Loopback

To pipe audio to other applications (like FM-DX-Webserver), use a virtual audio cable:

| OS | Tool |
|---|------|
| Linux | PulseAudio, ALSA loopback, JACK |
| macOS | BlackHole, Soundflower, Loopback (commercial) |
| Windows | VB-Audio Virtual Cable, Voicemeeter |

On macOS with Homebrew:

```bash
brew install blackhole-2ch
```

Then select the loopback device as your audio output in system settings or the target application.

## Architecture

```
+------------------+     +-------------------+     +--------------+
| XDR Client      |---->| FM Tuner SDR     |<----| rtl_tcp     |
| (port 7373)     |     | (this app)       |     | Server      |
+------------------+     +-------------------+     +--------------+
                                 |
         +-----------------------+-----------------------+
         |                       |                       |
         v                       v                       v
   +----------+            +-----------+           +---------+
   | FM Demod |<-----------| RTL-TCP   |           | XDR     |
   | (Phase)  |           | Client    |           | Server  |
   +----------+           +-----------+           +---------+
         |                                               |
         v                                               v
   +----------+            +-----------+           +---------+
   | Stereo   |            | RDS       |           | Audio   |
   | Decoder  |---------->| Decoder   |           | Output  |
   | (PLL)    |           | (Thread)  |           | (WAV/   |
   +----------+            +-----------+           | Speaker)|
         |                                       +---------+
         v
   +----------+
   | AF Post  |
   | Processor|
   +----------+
```

## Testing

```bash
pip install pytest numpy
pytest tests/
```

## IQ Analysis Tool

Capture IQ while running:

```bash
./fm-tuner-sdr -c fm-tuner-sdr.ini -s -i /tmp/capture.iq
```

Analyze capture:

```bash
python3 tools/analyze_iq.py /tmp/capture.iq --sample-rate 256000
```

## Gain Calibration Tool

Calibrate a stable `rtl_gain_db` across multiple FM frequencies:

```bash
python3 tools/calibrate_gain.py \
  --freqs 88600,96800,105900,107200 \
  --gains 0,7,14,20,28,35,42,49 \
  --seconds 8
```

Outputs:

- JSON report with all tested points and scores.
- `recommended_sdr.ini` snippet with recommended `rtl_gain_db` for normal runs.

Full FM-band reference file (87.5-108.0 MHz, 100 kHz steps):

```bash
python3 tools/calibrate_gain.py \
  --full-band \
  --freq-start-khz 87500 \
  --freq-end-khz 108000 \
  --freq-step-khz 100 \
  --gains 0,7,14,20,28,35,42,49 \
  --seconds 6
```

Additional external reference outputs:

- `gain_reference.csv` (`freq_khz,gain_db,source`)
- `gain_reference.ini` (`[gain_reference]` mapping)
- `gain_reference.json` (machine-readable mapping with source)

## Credits

This project is based on or modeled after the following open source projects:

| Project | Description | Link |
|---------|-------------|------|
| SDRPlusPlus | FM demodulation (quadrature), stereo decoder (PLL-based pilot detection), RDS decoding | <https://github.com/AlexandreRouma/SDRPlusPlus> |
| XDR-GTK | XDR protocol client implementation, RDS parser | <https://github.com/kkonradpl/xdr-gtk> |
| librdsparser | RDS parsing library (submodule of XDR-GTK) | <https://github.com/kkonradpl/librdsparser> |
| FM-DX-Tuner | TEF tuner firmware, FM-DX protocol reference | <https://github.com/kkonradpl/FM-DX-Tuner> |
| xdrd | Original XDR daemon protocol implementation | <https://github.com/kkonradpl/xdrd> |

## License

GNU General Public License v3.0 - see [LICENSE](LICENSE) file

## Third-Party Licenses

This software includes the following third-party components:

| Component | License | Notes |
|-----------|---------|-------|
| OpenSSL | Apache 2.0 / SSLeay | See [LICENSE](LICENSE) and [OpenSSL license](https://www.openssl.org/source/license.html) |
| PortAudio | MIT | |
| SDRPlusPlus | GPLv3 | |
| XDR-GTK | GPLv3 | |
| FM-DX-Tuner | GPLv3 | |
| xdrd | GPLv2 | |

For binary distributions, include the LICENSE file and this attribution notice.
