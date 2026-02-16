# FM Tuner SDR - Implementation Plan

## Overview
Self-contained FM radio that connects to rtl_tcp server, demodulates WBFM in stereo, and can be controlled via XDR protocol.

## Architecture

```
+---------------------+     +----------------------+     +-------------------+
| rtl_tcp server     |     | FM Tuner SDR        |     | Control Clients   |
| (external RTL-SDR) |---->| (this application)  |<----|                   |
+---------------------+     +----------------------+     +-------------------+
                                   |                             |
                                   ▼                             ▼
                            +-------------+            +--------------------+
                            | RTL-TCP     |            | XDR Server (7373) |
                            | Client      |            +--------------------+
                            +-------------+            +-------+------------+
                                   |                        |            |
                                   ▼                        ▼            ▼
                            +-------------+            XDR-GTK      FM-DX-Tuner
                            | FM Demod   |            (SHA1 auth)   (x protocol)
                            | (SDR++)    |
                            +-------------+
                                   |
                                   ▼
                            +-------------+
                            | Stereo      |
                            | Decoder     |
                            +-------------+
                                   |
                                   ▼
                            +-------------+
                            | Audio       |
                            | Output      |
                            | (PortAudio) |
                            +-------------+
                                   |
                                   ▼
                            +-------------+
                            | WAV File   |
                            +-------------+
```

### Protocol Support

| Client | Protocol | Port | Auth |
|--------|----------|------|------|
| XDR-GTK | xdrd (salt + SHA1) | 7373 | Password |
| FM-DX-Tuner | FM-DX (x command) | 7373 | None |

### Components

| Component | Source | Description |
|-----------|--------|-------------|
| RTL-TCP Client | SDR++ | Connect to rtl_tcp server |
| FM Demod | SDR++ Quadrature | Phase difference demod |
| Stereo Decoder | SDR++ PLL | 19kHz pilot detection |
| Audio Output | PortAudio | Cross-platform audio |
| XDR Server | Original xdrd | Protocol compatibility |
| FM-DX Server | FM-DX-Tuner | Protocol compatibility |

## CODE SOURCE: SDR++ ONLY

All DSP algorithms from `research/SDRPlusPlus/`:

| Component | File | Purpose |
|-----------|------|---------|
| RTL-TCP | `source_modules/rtl_tcp_source/src/rtl_tcp_client.cpp` | Network client |
| Quadrature | `core/src/dsp/demod/quadrature.h` | FM demod |
| BroadcastFM | `core/src/dsp/demod/broadcast_fm.h` | Full WBFM + Stereo |
| Pilot PLL | `core/src/dsp/loop/pll.h` | 19kHz pilot tracking |
| FIR Taps | `core/src/dsp/taps/low_pass.h` | Filter generation |
| Stereo Matrix | `core/src/dsp/convert/l_r_to_stereo.h` | L/R separation |
| RDS | `decoder_modules/radio/src/rds.cpp` | RDS (optional) |

## Command Line Interface

```
./fmtuner-sdr -t <host:port> -f <freq_khz> -w <wav_file>
```

Example:
```bash
# Terminal 1: Start rtl_tcp
rtl_tcp -p 1234 -f 88600000 -g 20 -s 1020000

# Terminal 2: Run FM radio
./fmtuner-sdr -t localhost:1234 -f 88600 -w test.wav
```

## XDR Protocol Commands
- `T<freq>` - Tune to frequency (sends to rtl_tcp)
- `V<vol>` - Volume (0-100)
- `G<gain>` - Tuner gain
- `A<agc>` - AGC mode (0=off, 1=on)
- `S` - Get status

Port: 7373

## SDR++ Implementation Details

### Quadrature FM Demod (core/src/dsp/demod/quadrature.h)

SDR++ uses phase difference demodulation:

```cpp
inline int process(int count, complex_t* in, float* out) {
    for (int i = 0; i < count; i++) {
        float cphase = in[i].phase();  // atan2(im, re)
        out[i] = math::normalizePhase(cphase - phase) * _invDeviation;
        phase = cphase;
    }
    return count;
}
```

Key: `normalizePhase()` wraps phase to [-π, π]

### BroadcastFM Stereo (core/src/dsp/demod/broadcast_fm.h)

Full WBFM with stereo:

```
IQ Input
    │
    ▼
Quadrature Demod (atan2) ──► Mono (L+R)
    │
    ├──► ComplexToReal ──► Pilot Filter (19kHz) ──► Pilot PLL
    │                                                       │
    │                           conjugate ◄────────────────┘
    │                               │
    ▼                               ▼
Delay                          Multiply (L-R × 38kHz) ──► L-R signal
    │                               │
    ▼           ┌───────────────────┘
Add (L+R)      Subtract (L+R)
    │               │
    ▼               ▼
    L               R
    │               │
    └──► FIR 15k ──┘
            │
            ▼
    Stereo Output
```

### Pilot PLL (core/src/dsp/loop/pll.h)

PLL-based pilot detection is more robust than correlation:
- Bandpass filter at 19kHz (18750-19250 Hz)
- PLL tracks pilot frequency
- Handles weak pilots better

## Files to Create/Modify

| File | Source | Purpose |
|------|--------|---------|
| `src/rtl_tcp_client.cpp` | SDR++ adapt | Connect to rtl_tcp |
| `src/fm_demod.cpp` | SDR++ Quadrature | FM demod |
| `src/stereo_decoder.cpp` | SDR++ BroadcastFM | Stereo decode |
| `include/stereo_decoder.h` | | Header |

## Dependencies
- pthread
- CoreAudio (macOS)
- OpenSSL (for XDR auth)
- **No librtlusb needed**

## RTL-TCP Protocol
Commands use network byte order (big-endian):
- `0x01 + freq` - Set frequency (4 bytes BE)
- `0x02 + rate` - Set sample rate
- `0x04 + gain` - Set gain
- `0x08 + 0/1` - Enable/disable AGC

## Test Suite

### Dependencies
```
pytest
numpy          # For spectral/amplitude analysis
```

### Files
- `tests/conftest.py` - Pytest fixtures
- `tests/test_xdr_protocol.py` - XDR protocol tests
- `tests/test_wav_output.py` - WAV validation
- `tests/test_integration.py` - End-to-end tests

### Test Execution
```bash
pip install pytest numpy
pytest tests/
```

## Success Criteria
1. [x] Connects to rtl_tcp server
2. [x] Receives IQ data over TCP
3. [x] FM demod produces valid audio
4. [x] Stereo decoder works (or mono fallback)
5. [x] Audio plays on speakers
6. [x] WAV file is valid (not noise)
7. [x] XDR commands control frequency

## CoreAudio Speaker Output - Bug Fix (FIXED)

### Issue
The `AudioOutput::write()` method only wrote to the WAV file but NOT to the circular buffer used by AudioQueue.

### Root Cause
In `src/audio_output.cpp`, the `write()` method was missing circular buffer writes.

### Fix Applied
Modified `write()` to copy audio samples to the circular buffer when speaker is enabled.
Also changed CIRCULAR_BUFFER_FRAMES from 32768 (power of 2) to 30000 for proper modulo.

### Verification
- [x] Rebuild: `cd build && cmake .. && make`
- [x] Tests pass
- [x] Audio plays on speakers

## XDR Server Parity with Original xdrd

### Authentication Flow
1. Server generates random 16-char salt on connect
2. Server sends salt to client
3. Client sends SHA1(salt + password) as 40-char hash
4. Server returns: `a0` (fail), `a1` (guest), `a2` (authenticated)

### Commands
| Command | Description |
|---------|-------------|
| `P<hash>` | Authenticate |
| `S` | Get status |
| `T<freq>` | Tune frequency (Hz) |
| `V<vol>` | Volume (0-100) |
| `G<gain>` | Gain |
| `A<agc>` | AGC (0=off, 1=on) |

### Implementation
- `src/xdr_server.cpp` - Auth logic with SHA1
- `src/main.cpp` - `-P` password, `-g` guest options

### Verification
- [x] XDR server generates 16-char salt
- [x] SHA1 authentication
- [x] Returns a0/a1/a2
- [x] Commands rejected if not authenticated
- [x] Tests pass

## SDR++ Code Integration (CURRENT)

### Implemented from SDR++:
1. [x] FM Demod - using normalizePhase for proper phase wrapping
2. [x] Stereo Decoder - PLL-based pilot detection
3. [x] Output scaling - proper 16-bit range

### Key SDR++ Files Used:
- `core/src/dsp/demod/quadrature.h` - Phase demod
- `core/src/dsp/loop/pll.h` - Pilot tracking  
- `core/src/dsp/math/normalize_phase.h` - Phase wrapping
- `core/src/dsp/demod/broadcast_fm.h` - Full stereo matrix

### NOT Using:
- rtl_fm code (removed)
- Custom correlation (replaced with PLL)

### Testing
```bash
cd build && cmake .. && make
rtl_tcp -p 1234 -f 88600000 -g 20 -s 1020000 &
./fmtuner-sdr -t localhost:1234 -f 88600 -s -w test.wav
sox test.wav -d
```

### Verification Checklist
- [x] FM demod uses SDR++ normalizePhase
- [x] Stereo uses SDR++ PLL approach
- [x] Audio output scaling correct
- [x] Tests pass
