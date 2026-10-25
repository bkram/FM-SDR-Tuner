# FM-SDR-Tuner

FM broadcast tuner and XDR server built around RTL-SDR IQ input.

> New here? **[USER-MANUAL.md](USER-MANUAL.md)** is a focused guide to startup
> and configuration. This README is the full feature/algorithm reference.

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
- Input: `rtl_sdr` (default), `rtl_tcp`, or `sdrplay` (RSP series)
- Demod/stereo/audio pipeline in-process
- DSP pipeline uses `liquid-dsp` primitives (FM chain + RDS-related paths)
- XDR-compatible server on port `7373`
- Native audio backends: Core Audio (macOS), ALSA (Linux), WinMM (Windows)

## Highlights

- Direct USB RTL-SDR support (`--source rtl_sdr`) as default mode
- `rtl_tcp` network source support (`--source rtl_tcp`)
- SDRplay RSP support (`--source sdrplay`): 16-bit IQ fed at full dynamic range into the demod; the SDRplay API is `dlopen`'d at runtime (built with `-DFM_TUNER_ENABLE_SDRPLAY=ON`, no link/bundle), so the binary still runs and falls back to RTL where the RSP library is absent
- Anonymous REST control API on a dedicated port (`[rest]`): carries the SDR-specific settings XDR has no vocabulary for (manual dB gain, SDRplay LNA state, antenna input, bias-tee) plus the common ones (frequency, bandwidth, AGC, de-emphasis, force-mono, volume, start/stop); works for both RTL and SDRplay backends. Intended for an fm-dx-webserver client plugin
- FM stereo demod with gear-shifted PLL on the 19 kHz pilot (wide acquire, narrow hold)
- 2nd-order Butterworth adaptive Hi-Blend on the L−R path — narrows stereo image as reception worsens, with 12 dB/oct rejection of stereo-decoded HF noise instead of 6 dB/oct
- LMS pilot canceller: subtracts a phase-locked 19 kHz copy from the mono path (~20 dB extra suppression beyond the audio LPF; on by default)
- Optional adaptive de-emphasis ("HiCut"): narrows the top end on fringe stations to suppress hiss without affecting strong locals
- Optional adaptive channel bandwidth: narrows the channel FIR when SNR is low, widens when it recovers (hysteresis-gated)
- Optional CMA multipath equalizer (Godard 1980, patent-free): cancels FM ghosting on real multipath, stays transparent on clean signals via dispersion target + leak regularization
- Continuous-quality blend gate (no mono pops on marginal signals)
- Soft-knee audio limiter with metered clip ratio
- RDS decode in dedicated worker thread (redsea-port: 171 kHz, 57 kHz PLL, RRC symbol sync, BPSK, block-sync state machine)
- XDR protocol compatibility for FM-DX clients on port 7373. Two wire protocols are served on the same port and told apart by the client's first byte: the XDR-GTK protocol (server sends a 16-char salt, client answers with the SHA-1 hash) and the FM-DX-Tuner protocol (client sends `x`, server answers `\nOK\n`). Both are subject to the configured password.
- Audio output at 48 kHz (native Core Audio / ALSA / WinMM)
- Output to speaker (`-s`), WAV (`-w`), MPX WAV (`--mpx-wav`, with configurable rate via `--mpx-rate` for downstream RDS / spectrum / decoder analysis or feeding an FM exciter that accepts raw MPX), live MPX → audio device (`--mpx-audio` on macOS/Linux, typically into BlackHole / snd-aloop for re-encoding or directly into a TX accepting line-in MPX), and/or raw IQ capture (`-i`)
- Self-documenting signal meter: startup `[METER]` log reports the active calibration window and periodically suggests narrower floor/ceil based on observed session min/max

## Quick Start

With an RTL-SDR connected, the binary works with no CLI flags and no config file — audio is on by default and the XDR server runs in implicit guest mode when no password is set:

```bash
./build/fm-sdr-tuner
```

This opens the XDR server on port 7373 on all network interfaces (set `[xdr] password` if the machine is reachable from other hosts) and waits for a client (xdr-gtk, FM-DX-Webserver, etc.) to send the start command. To skip the wait and hear audio immediately:

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

List the devices the tuner can see with `./build/fm-sdr-tuner -l`, then point it at the loopback either per-run with `-d "<name>"` or persistently via `[audio] device = ...` in the INI (e.g. `device = BlackHole 2ch`). The shipped example configs leave `device` empty (system default).

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

## SDRplay (RSP) support

Prebuilt release binaries are RTL-only. CI does not build SDRplay support
because the vendor SDK headers are no longer reliably downloadable in an
unattended build. SDRplay remains available for local macOS and Linux builds
when the SDK is already installed; Windows SDRplay remains unsupported.

### Building with SDRplay support

1. **Install the SDRplay API** from <https://www.sdrplay.com/api/>. This
   provides the SDK headers (`sdrplay_api.h`) the build needs and the runtime
   service (`sdrplay_apiService`) + shared library (`libsdrplay_api`) the binary
   `dlopen`s at run time. The tuner never links the library — only the headers
   are needed at compile time.
2. **Configure with the flag.** SDRplay code only compiles when
   `-DFM_TUNER_ENABLE_SDRPLAY=ON` is set *and* CMake can find `sdrplay_api.h`:

   ```bash
   cmake -S . -B build -DFM_TUNER_ENABLE_SDRPLAY=ON   # + your platform arch flag
   cmake --build build -j
   ```

   CMake searches `/usr/local/include` and `/Library/SDRplayAPI/*/include` for
   the header. If the SDK is installed elsewhere, point it there explicitly:

   ```bash
   cmake -S . -B build -DFM_TUNER_ENABLE_SDRPLAY=ON \
         -DSDRPLAY_INCLUDE_DIR=/path/to/sdrplay/include
   ```

   Watch the configure output: `SDRplay support enabled (headers: ...)` means it
   worked. If you instead see `building SDRplay stub (source unavailable at
   runtime)`, the header wasn't found — the build still succeeds but
   `--source sdrplay` will be a no-op until you point CMake at the SDK and
   rebuild.

Then run it (the SDRplay API service must be installed and running so
`libsdrplay_api` and `sdrplay_apiService` are present):

```bash
./build/fm-sdr-tuner --source sdrplay --auto-start -s
```

The RSP front end is configured to deliver 2.048 MHz / 8 = 256 kHz IQ, matching
the tuner's internal input rate (1:1 decimation). The 16-bit samples feed the
demod at full dynamic range; an 8-bit shadow drives the signal meter / scan
engine so the existing calibration windows stay valid. SDRplay-specific knobs
live in an `[sdrplay]` INI section:

```ini
[tuner]
source = sdrplay

[sdrplay]
lna_state = 4      ; front-end LNA gain-reduction step (0 = most gain)
antenna   = 0      ; RSPdx/RSPdxR2 antenna input: 0=A, 1=B, 2=C (ignored on other models)
bias_tee  = false
```

If no RSP (or no SDRplay API service) is present, `--source sdrplay` reports the
failure; use `--source rtl_sdr` to fall back to an RTL dongle.

**Spectral scan:** an SDRplay full-band sweep is dominated by per-retune LO
settling, so the tuner automatically switches the RSP to a wide-bandwidth scan
mode for the duration of a scan — it drops the hardware decimation (256 kHz →
2.048 MHz) and widens the IF filter, so each retune covers ~8× more spectrum
(~17 retunes instead of ~140 for the full band). A full 87.5–108 MHz sweep
drops from ~13 s to under 2 s, then the 256 kHz audio rate is restored
automatically. This is transparent (no config) and a no-op on RTL.

## XDR Spectral Scan (band overview)

The XDR protocol carries a band-scan command that the XDR-GTK client turns
into its spectrum plot. The tuner sweeps the requested range, measures each
channel, and reports the result as `U<freq_kHz>=<level>,…` lines.

```text
Sa87500     sweep starts at 87.5 MHz
Sb108000    sweep stops at 108.0 MHz
Sc500       channel step in kHz (minimum 5)
Sw56000     measurement bandwidth in Hz
S           start the sweep
Sm          start the sweep in continuous mode
```

The sweep runs on the IQ path, so it owns the tuner while it lasts: audio is
interrupted and the signal meter freezes until the sweep ends. The tuner is
restored to the frequency and bandwidth it had before, and the DSP chain is
reset (`[DSP] reset reason=scan_restore`).

**The plot fills in progressively.** A line is published after every retune, so
the client shows the spectrum growing instead of an empty graph for the whole
sweep. Each line describes the *same* grid — the same start frequency, the same
step, the same channel count — because the reference client replaces its
dataset on every line rather than merging. Channels not yet measured are
reported as `0.0` (the client's "no signal" floor), so the x-axis does not
shrink as the sweep advances. The trailing comma is mandatory: the client's
parser sizes its array from the comma count and drops the last entry without
it.

**The plot is a spectrum, not a list of stations.** Every grid point reports
the level of the band at that frequency, measured with the same estimator as the
signal meter over a 30 kHz window. The meter integrates the whole channel, so
the two read the same number for an unmodulated carrier and the meter reads a
modulated station higher than the plot. A signal that is not a station stays a
bump in the noise floor, the way a spectrum display shows it, instead of
turning into a station of its own. That is the difference between this plot
and a sparse "stations only" scan, where a single noise maximum is drawn like
a station on a frequency nobody broadcasts on — the "ghost stations" a
spectrum display does not have. A grid point that holds a station reports the
station's peak instead, so the station is drawn exactly on its own grid point
and shows its real strength.

**The step is widened when the grid would not fit.** The whole spectrum travels
in a single line, and the reference client reads into a 10000-byte buffer and
silently drops whatever exceeds it — a truncated line loses its trailing comma
and the client then discards the last entry, ending the plot short of the
requested stop frequency. The client also refuses more than 700 samples
outright. A request that would need more channels than that (a 5 kHz step
across the full band is 4101) is therefore widened automatically to fit, with
a note in the log:

```text
[SCAN] warning: 4101 channels requested exceeds the 700 the protocol carries
in one line; widening the step from 5 to 30 kHz
```

**Authorization.** The scan retunes the radio, so it is a tuner command, and a
guest session (a client that failed the challenge/response) may not run one —
it gets `a0`, the protocol's "not authorized" reply. This costs the default
configuration nothing: with no password the server accepts `SHA1(salt)`, which
is exactly what the reference client sends for an empty password, so such a
client is fully authenticated rather than a guest.

**Stations are reported on their own frequency.** The European FM plan
allocates channels every 200 kHz starting at 87.6 MHz, but the client's
default sweep starts at 87.5 MHz. A grid anchored there lands on the
*midpoints* between the stations, so a station on 96.2 MHz would be reported
on the 96.1 grid point and clicking the plot would tune to the wrong
frequency. The grid is therefore aligned with the station raster whenever the
requested step divides it — a 100 kHz step from 87.5 MHz already contains
87.6, 87.8, …, so the requested start is kept as-is, while a 200 kHz step
cannot hold every station and the grid is shifted onto the raster. A step that
does not divide the 200 kHz raster is reported in the log:

```text
[SCAN] warning: a 300 kHz step does not divide the 200 kHz CCIR channel
raster, so some stations are reported at the nearest grid point instead of
their own frequency
```

The station's own frequency is measured, not assumed: a station that sits
between two grid points is reported on the one it belongs to. A station
measured on 96.188 MHz — 12 kHz off the 96.2 grid point — is reported on
96.2.

**Levels are on the client's plot scale.** The reference client draws the scan
in a fixed 3..84 window and clips everything outside it, so the scan line is
remapped onto that window — on the meter's raw 0..120 scale every station
above −23 dBFS would be pinned flat against the top of the plot and the plot
would stop telling strong stations apart. The "S" line and the REST status
keep the 0..120 scale, because the clients that read them do the conversion
themselves (the webserver's dBm mode subtracts 120, the plugin shows the value
out of 120) and the client writes the meter's value straight into the scan
plot, so the two scales have to stay comparable. The window is configurable:

```ini
[sdr]
scan_use_client_window = true   # false keeps the raw 0..120 scale
scan_client_min_level = 3.0
scan_client_max_level = 84.0
```

A channel that has not been swept yet keeps its `0.0` "no signal" marker
rather than being lifted to the bottom of the window.

## REST control API (fm-dx-webserver plugin)

An optional anonymous HTTP API exposes the SDR settings on a dedicated port,
including the SDR-specific ones the XDR protocol cannot carry. It drives the
**same** `TunerController` and request-state as the XDR path, so it works
identically for the RTL-SDR and SDRplay backends (SDRplay-only knobs no-op on
RTL). There is **no authentication** by design — bind it to localhost or a
trusted network only.

```ini
[rest]
enabled = true
port = 9090
bind_address = 127.0.0.1
```

The port and bind address can also be overridden on the command line (handy for
transient runs): `--rest-port <port>` (0 disables the API; any non-zero port
enables it, overriding `[rest] enabled`/`port`) and `--rest-bind <addr>`.

```bash
./build/fm-sdr-tuner -c fm-sdr-tuner.ini -s --rest-port 9090 --rest-bind 0.0.0.0
```

Endpoints:

- `GET /api/status` → JSON of the current source/model/frequency/bandwidth/gain/etc.,
  plus live telemetry:
  - `signal` (0–120 meter level), `dbfs` (tuned-channel power), `clip` (0–1 IQ
    clip ratio), `overload` (bool — front-end/ADC overloading; reduce gain or
    LNA when true), `ghost_risk` (bool — the front end is clipping *while the
    tuned station is strong*, so its intermodulation products are heard as a
    duplicate of a station; lower the gain).
  - `stereo` (bool, decoder locked), `stereo_quality` (0–1), `pilot_khz` (19 kHz
    pilot deviation), `blend` (applied stereo blend).
  - `snr` (dB) — demod-domain SNR from the noise-triangle band above the
    multiplex. Unlike `signal` (a relative, install-calibrated meter — see
    [Getting reasonable signal-strength readings](#getting-reasonable-signal-strength-readings)),
    this is a self-referencing reception-quality figure: high and steady on a
    clean signal, dropping on a fade or interference. Always finite.
  - `rds_dev_khz` (57 kHz RDS subcarrier deviation), `rds_ber` (block error
    rate), `rds_groups` (groups decoded this session).
  - `mpx` (relative composite magnitude) and `mpx_peak_khz` (MAX DEV — decaying
    peak composite deviation).

  All numeric telemetry is rounded to one decimal.
- `GET  /api/control?key=value&...` or `POST /api/control` (JSON or form body) →
  applies settings and returns `{"ok":..,"applied":N,"rejected":[..],"status":{..}}`.

Recognised keys: `freq_hz` / `freq_khz`, `gain_db`, `agc` (bool), `bandwidth_hz`
/ `bandwidth_khz`, `lna`, `antenna`, `bias_tee` (bool), `ppm`, `rtl_agc` (bool),
`deemphasis` (0=50µs,1=75µs,2=off), `force_mono` (bool), `volume` (0–100),
`action` (`start`/`stop`).

The gain keys drive the **tuner's own gain state**, not the hardware
registers: `gain_db` pins a manual gain, `agc` hands the gain back to the
tuner's auto-gain ladder, and `rtl_agc` switches the front end's own IF AGC.
A gain set here therefore survives a retune, a reconnect and the periodic
gain re-assert — and the software ladder never fights the hardware AGC over
the same register. The gain keys are refused when `client_gain_allowed` is
false in the profile, or when the gain was pinned on the command line with
`-g`.

Examples:

```bash
curl "http://127.0.0.1:9090/api/control?freq_khz=94900&bandwidth_khz=200"
curl -X POST -H 'Content-Type: application/json' \
     -d '{"gain_db":28,"lna":2,"antenna":1}' http://127.0.0.1:9090/api/control
curl http://127.0.0.1:9090/api/status
```

For interactive testing there is a tiny static control panel served by Python:

```bash
scripts/run_rest_panel.sh                                  # defaults: API :9090, panel :9091
# or directly / with overrides:
python3 scripts/rest_test_panel.py --api http://127.0.0.1:9090 --port 9091
API_URL=http://127.0.0.1:9090 PANEL_PORT=9091 scripts/run_rest_panel.sh
# then open http://127.0.0.1:9091
```

## Tests

Run the normal test suite:

```bash
ctest --test-dir build --output-on-failure --no-tests=error
```

### Testing Without An SDR (fake rtl_tcp server)

`scripts/fake_rtl_tcp.py` is a fake `rtl_tcp` server that plays a synthetic
FM band, so the whole path — the XDR handshake, the scan engine, the wire
format — can be exercised without a dongle. It reproduces the reference
server's behaviour exactly: the 12-byte `RTL0` greeting, **no** per-command
acknowledgements, the commands read on a separate thread from the IQ stream,
a stream paced at the sample rate, and an NCO that restarts on a retune.

```bash
# A synthetic band with carriers on 96.2 and 101.2 MHz
python3 -I scripts/fake_rtl_tcp.py --port 12345 --stations 96200000,101200000

# Point the tuner at it
./build/fm-sdr-tuner --source rtl_tcp -t 127.0.0.1:12345 --iq-rate 256000 \
                     --auto-start -w /tmp/wire.wav --no-audio -P test123
```

A carrier is only present in a read when it falls inside the captured band,
exactly like a real front end, so the tuner has to sweep to find it — the
scan is genuinely exercised rather than handed the answer.

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
- Device/buffer latency is backend-specific (Core Audio / ALSA / WinMM). The
  macOS CoreAudio path keeps a 200 ms playback reserve to absorb short SDR
  control/USB stalls; after an underrun it rebuilds that reserve before
  resuming. Windows WinMM is event-driven (`CALLBACK_EVENT`, ~170 ms of device
  buffering). If you ever see `[AUDIO] CoreAudio underrun` or `[AUDIO] WinMM
  output can't keep up with 48 kHz`, the selected device/driver could not
  sustain the stream.
- Diagnostic logging (`[SIG]`, `[ST]`, `[AUDIO]`, …) follows `[debug] log_level`
  in the INI (default on). Override per-run with `-v`/`--verbose` or
  `-q`/`--quiet`. The SDRplay source's own connect trace (`[SDRplay] …`) follows
  the same switch — it is silent under `-q` unless the connect actually fails.
- XDR server listens on port `7373`. If `xdr.password` is empty and no `-P` is passed, the server automatically enters guest mode (no auth). The password guards **both** wire protocols on that port: a guest may read the tuner state but not change it.
- The `o<auth>,<guest>` line reports how many clients are actually connected, and is re-sent to every client whenever one joins or leaves.
- The tuner does not auto-start by default; an XDR client start command activates it. Use `--auto-start` to begin playback without a client.
- A tune request for the frequency that is **already** tuned does not retune:
  the tuner only re-asserts the gain/AGC registers. A retune flushes the audio
  queue, resets the DSP and mutes the output while the demodulator settles, and
  the listener hears that as the programme restarting. Clients re-send the
  current frequency routinely (xdr-gtk on every click of the tuned entry, the
  REST panel on every poll that carries a frequency), so this is the common
  case.
- A control client that disconnects does **not** stop a tuner it did not start.
  On the FM-DX path the tuner is stopped when the client that started it goes
  away; a client that attaches to an already-running tuner (`--auto-start`, or
  a tuner another client is using) no longer cuts the programme when it
  disconnects. Without this, a reconnecting control client restarted the
  tuner — and the listener heard the station start over.
- The first ~250 ms of IQ after a **connect** is dropped before it reaches the
  DSP. An RTL-SDR delivers a loud, narrowband settling burst at the LO centre
  in that window (measured: 12 dB above the settled level, clipping ~8% of
  the samples). It is not programme: left in, it reached the audio as a burst
  of noise, polluted the signal statistics and the meter calibration, and
  triggered the front-end overload warnings on a perfectly healthy front end.
  The raw IQ capture (`-i`) still contains it, because the capture is the
  tuner's raw output.
- Engaging the front end's **own AGC** costs a short silence (~600 ms), for
  the same reason. The dongle's analog auto-gain overshoots when it is
  switched on: on an R820T the samples right after the switch saturate the
  converter for 100–600 ms (measured here: 55–90% of the samples at the rail)
  before the AGC settles ~7 dB below the ceiling. Left in, that burst reached
  the listener as noise, and it tripped the overload/duplicate warning for a
  front end that was not overloaded — the exact false "duplicate station"
  report the plugin shows. The settling burst is dropped instead, and the
  audio fades back in over the gap. A re-assert of an already-running AGC
  (the periodic keep-alive) leaves the AGC alone instead of restarting its
  attack, so it does not cost a silence every few minutes.
- The gain is the **tuner's** state, not the hardware's. A client that sets
  the gain — the fm-dx-webserver plugin over REST, an XDR client over the XDR
  protocol — sets it in the tuner, so it survives a retune, a reconnect and
  the periodic gain re-assert, and the software auto-gain ladder never fights
  the front end's own AGC over the same register. A `-g` on the command line
  pins the gain: no client may change it after that. The gain in the profile
  is only the starting point, unless the profile sets
  `client_gain_allowed = false`.
- De-emphasis default is 50 µs (EU/ITU). For US/Korea set `[tuner].deemphasis = 1` in the config (75 µs).
- Default stereo blend profile is `normal`: it holds the stereo image steady on a station whose quality estimate dips. Measured here on two back-to-back captures of the same station, `aggressive` left the image 8 dB weaker and swinging over 22 dB, while `normal` held it steady. Select `soft` or `aggressive` via `-b` or the config.

## Config-Driven Usage (optional)

**Auto-load:** when you run the binary **without** `-c`/`--config`, it looks for
`fm-sdr-tuner.ini` in the current working directory and loads it automatically
(you'll see `[Config] auto-loading fm-sdr-tuner.ini …` at startup). So you can
just edit the config next to the executable and run it — no `-c` needed. The
release artifacts ship a ready-to-use `fm-sdr-tuner.ini` (a copy of the RTL
example) for exactly this, so it works on first run:

```bash
./build/fm-sdr-tuner            # auto-loads ./fm-sdr-tuner.ini if present
```

An explicit `-c <file>` always wins over the auto-loaded file, and if no
`fm-sdr-tuner.ini` is present the binary just uses its built-in defaults.

The repo and release artifacts also ship two documented example configs — copy
the one for your hardware over `fm-sdr-tuner.ini` and tune it for your site
(your personal `fm-sdr-tuner.ini` is git-ignored, so it is never committed):

```bash
cp fm-sdr-tuner.ini.example fm-sdr-tuner.ini          # RTL-SDR
# or, for an SDRplay RSP (macOS/Linux, SDRplay build only):
cp fm-sdr-tuner-sdrplay.ini.example fm-sdr-tuner.ini

./build/fm-sdr-tuner            # or: -c fm-sdr-tuner.ini to be explicit
```

The RTL example is the profile that was measured on this site (an R820T with
the indoor antenna): a fixed 10 dB gain, the meter window that goes with it
(`-33 / -10`, shifted from the `-43 / -19` measured with the earlier 30 kHz
meter), and the REST control API enabled on `127.0.0.1:9090` — the
address the fm-dx-webserver plugin talks to by default. The gain, the meter
window and the antenna in it are the ones that were verified on real
hardware; re-derive them with `--calibrate` after a change of antenna or
cable.

Both examples enable the REST control API (`[rest] enabled = true`, port 9090)
and the bundled test panel (`scripts/rest_test_panel.py`, shipped in the
artifact). CLI flags override anything the config sets, which is useful for A/B
testing:

```bash
# temporary frequency override on top of a baseline config
./build/fm-sdr-tuner -c fm-sdr-tuner.ini -f 101100

# try a different stereo blend profile without editing the INI
./build/fm-sdr-tuner -c fm-sdr-tuner.ini -b soft
```

## Configuration (`fm-sdr-tuner.ini`)

`fm-sdr-tuner.ini` is the primary control surface. It is **git-ignored** — start
from a shipped example (`fm-sdr-tuner.ini.example` for RTL-SDR,
`fm-sdr-tuner-sdrplay.ini.example` for SDRplay) and copy it to
`fm-sdr-tuner.ini`.

Important sections:
- `[tuner]`: source, device index, startup frequency, deemphasis
- `[audio]`: enable speaker output and select output device
- `[sdr]`: tuner gain strategy and dBf mapping window
- `[processing]`: DSP block size, W0 bandwidth, stereo blend, optional DSP AGC
- `[xdr]`: server port/password/guest mode

Signal meter related keys (`[sdr]`):
- `signal_floor_dbfs` (default `-45.0`)
- `signal_ceil_dbfs` (default `-1.0`)
- `signal_bias_db` (default `0.0`)

### Getting reasonable signal-strength readings

The displayed signal level (the `0..120` value XDR/FM-DX clients show as dBf) is
a **linear mapping of the tuned-channel power** (compensated dBFS) onto a
`0..120` scale:

- `signal_floor_dbfs` is the power that reads as **0** (your noise floor / empty
  channel).
- `signal_ceil_dbfs` is the power that reads as **120** (your strongest local).
- `signal_bias_db` shifts the whole scale up/down to line up with a reference
  receiver, if you have one (otherwise leave it at `0`).

The measured power is the **band power of the whole 200 kHz channel** around
the tuned carrier. An FM signal has a constant envelope, so the reading does not
move with the programme (a 30 kHz window read the same carrier 8 dB lower at
full modulation than in silence). The spectral scan uses the same estimator
over 30 kHz, so the two agree for an unmodulated carrier and the meter reads a
modulated station higher than the plot. Half of the applied gain is taken out
of the reading, so a station keeps its level when the gain strategy changes.

The shipped default (`-45` / `-1`) puts the noise floor of an empty band at 0
and a strong local station near 115. To make the reading track your own site:

1. **Fix RF gain first.** A meter calibrated over a clipping or starved front
   end is meaningless. Get `[SIG] clip` near zero on strong locals (see
   [Tune RF Gain](#4-tune-rf-gain-before-anything-else)) before touching these
   keys.
2. **Let the tuner measure your site.** Either run the one-shot sweep
   `./fm-sdr-tuner --calibrate` (prints copy-paste `signal_floor_dbfs` /
   `signal_ceil_dbfs` / `signal_bias_db` for your antenna), or just watch the
   `[METER] session observed` log line, which continuously suggests a floor/ceil
   from the running min/max it sees.
3. **Apply the suggested window** to `[sdr]` and restart. Strong locals should
   then settle near the top and an empty frequency at zero.

The full step-by-step procedure, the `[METER]` output format, and the symptoms
of a mis-set window are in
[Calibrate The Signal Meter For Your Location](#8-calibrate-the-signal-meter-for-your-location).

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

> **Short version:** the antenna dominates everything (15–25 dB — more than
> every DSP knob combined). Get the RF path right first; no amount of software
> recovers a signal that never reaches the dongle.
>
> For a condensed, ordered version of this guide — which factor matters most,
> which gain profile to start from, and what each adaptive feature is actually
> for — see **[RECEPTION-STRATEGIES.md](RECEPTION-STRATEGIES.md)**.

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

The shipped RTL profile (`fm-sdr-tuner.ini.example`) is the one that was measured on real hardware at this site: a fixed 10 dB gain with the most headroom of the settings tried, the meter window that goes with it (`-33 / -10`, shifted from the `-43 / -19` measured with the earlier 30 kHz meter), and the REST control API on `127.0.0.1:9090` so the fm-dx-webserver plugin can drive the tuner. Copy it over `fm-sdr-tuner.ini` and start from there. The optional adaptive features (`hicut`, `adaptive_bandwidth`, `multipath_eq`) are off so the audio is identical to a passive demod chain until you opt in.

For a dedicated site profile, put overrides in `fm-sdr-tuner.ini`:

```ini
[sdr]
gain_strategy = tef
rtl_gain_db = 10
default_custom_gain_flags = 0
freq_correction_ppm = 0
# The window measured with the 10 dB gain above: the empty band reads 0 and
# the local station reads 91-100 of 120.
signal_floor_dbfs = -33.0
signal_ceil_dbfs = -10.0
signal_bias_db = 0.0

[processing]
agc_mode = 2
w0_bandwidth_hz = 194000
dsp_agc = off
stereo_blend = normal
stereo = true
pilot_canceller = true
# Optional adaptive features — leave off until you have a stable baseline.
hicut = off
adaptive_bandwidth = off
multipath_eq = off
# false freezes the gain (and with it the meter calibration) against a client
# that would move it; true lets the plugin's gain control work.
client_gain_allowed = false
```

Why this baseline:
- `rtl_gain_db = 10` is the measured setting for this site: 9.7 dB between the
  station's peaks and the converter's ceiling, no pumping, and the meter off
  the top of its scale
- `default_custom_gain_flags = 0` leaves the dongle's own AGC off, so the
  software auto-gain ladder stays in charge and its clip-protect can act. The
  IF flag (= 1) turns that hardware AGC on, and a hardware AGC sets the level
  by itself, so the ladder has to stand down
- `agc_mode = 2` is a stable middle ground for the ladder
- `w0_bandwidth_hz = 194000` is the correct starting point for normal WFM stereo
- `stereo_blend = normal` holds the stereo image steady on a station whose quality estimate dips — measured here on two back-to-back captures of the same station, `aggressive` left the image 8 dB weaker and swinging over 22 dB, while `normal` held it steady. Use `aggressive` only on a station whose stereo is noisy rather than clean
- `client_gain_allowed = false` prevents clients from changing gain while you tune the site profile — set it to `true` when the plugin's gain control should work

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

The clip column is the one to watch on FM specifically: the modulation peaks
of an FM station sit a few dB above its average, so a gain that looks
comfortable on the level readout can still clip the ADC on the peaks. If
`clip` is not `0.0000` on your strongest station, lower the gain — and if it
*is* `0.0000` but the peaks sit within a couple of dB of the ceiling, lower
it anyway: that is the margin that decides whether a stronger neighbour turns
into a duplicate of the station (see "Duplicates of strong stations" for the
measured numbers).

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
- `floor`: FFT-path noise-floor estimate — prints `n/a` in the normal live demod
  path (only the `computeSignalLevel` / scan / `--calibrate` path fills it in)
  - What to do next: don't rely on this while listening; read it from `--calibrate` instead
- `snr`: FFT channel SNR — also prints `n/a` in the live demod path, and is
  **distinct** from the demod-domain SNR (the REST `snr` field) that actually
  caps the displayed meter
  - What to do next: for the SNR that governs the meter, read REST `/api/status` `snr`, not this
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
  - A lock on a signal *below* the noise floor is meaningless: the PLL locks onto noise and reports `stereo=1` with a quality of 0 and a blend of 0. Read `quality` and `blend`, not `stereo` on its own
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
- `dbfs` may still be high under auto-gain (the front end amplifies noise), but
  `level` reads ~0 when the empty band is below `signal_floor_dbfs`
- no meaningful stereo lock
- if `level` itself still looks strong on a truly empty channel, the floor is
  too low for your site — raise `signal_floor_dbfs` (the `[METER]` suggester
  does this for you)

#### What Bad Looks Like

- high `clip`:
  - too much gain; reduce gain first
- high `level` on empty frequencies:
  - the floor is below your site's noise floor; raise `signal_floor_dbfs`
    (the `[METER]` suggester does this for you)
- `stereo=1` but low `blend` on a strong station:
  - gain may still be wrong, or the stereo-quality/blend policy is too conservative
- repeated `[DSP] reset reason=retune` while tuning:
  - expected during retune, not a signal-path fault

#### Example: Reading One Realistic Console Sequence

```text
[SIG] dbfs=-11.70 compensated=-21.70 floor=n/a snr=n/a level=89.9 filtered=89.5 clip=0.0000
[ST] pilot=207 stereo=1 quality=0.788 blend=0.633
[DSP] reset reason=retune
[XDR] tuning to 105900 kHz
```

(`floor=n/a snr=n/a` is normal in the live demod path — those two fields are only
filled in on the FFT / `computeSignalLevel` path used by scan and `--calibrate`.)

How to interpret it:
- first `[SIG]` line:
  - `clip=0.0000` means gain is now healthy
  - `level=89.9` means a strong, clean station (high demod SNR clears the cap and
    the floor/ceil window positions it near the top)
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

The default 45 dB window (`-55` floor, `-10` ceil) puts the noise floor of an empty band at 0 and a strong local station near 115. The `[METER]` log line shipped in every run does the legwork for you (it tracks the running min/max of your site and suggests the window that spans it):

```text
[METER] mapping window: floor=-55.0 dBFS, ceil=-10.0 dBFS (range 45.0 dB), bias=0.0 dB
[METER] session observed: min=-53.5 dBFS, max=-26.9 dBFS — for full-scale meter consider floor≈-57, ceil≈-24
```

Two procedures, both work end-to-end:

**Procedure A — One-shot band sweep (recommended).** Use the built-in calibration tool. It opens the RTL-SDR, sweeps 87.5–108 MHz, prints all detected stations sorted by quality, and emits copy-paste-ready INI values tuned to your antenna + location:

```bash
./fm-sdr-tuner --calibrate
```

The output includes:
- a per-frequency table (dBFS + meter level at every 100 kHz step). The level
  column is the same absolute mapping the live meter uses, so the two agree
  for the same signal
- a sorted list of detected stations (level120 > 30)
- a recommendation block with `signal_floor_dbfs`, `signal_ceil_dbfs`, and `signal_bias_db` chosen so the window spans the signal envelope at your site

Paste the recommendation block into the `[sdr]` section of your INI and
restart. Calibration temporarily captures at least 1.024 MHz so each FFT covers
several 100 kHz grid points; a full sweep normally takes about 5–10 seconds,
with no XDR client or audio backend needed. Re-run any time the antenna,
location, or gain changes.

**Procedure B — Live auto-suggester (for tweaking while listening).**
1. Start the tuner with the default wide window.
2. Tune through your representative stations (strong local, mid, fringe, empty) over ~1 minute.
3. Read the `[METER] session observed` line. It tracks the running min/max of
   clean compensated dBFS, excluding blocks whose IQ clips, and recommends
   `floor ≈ min − 3 dB`, `ceil ≈ max + 3 dB` so the window spans the usable
   signal range.
4. Apply those values to `[sdr]` in your INI.
5. Restart and confirm: strong locals now settle near the top and an empty frequency at zero.

Manual fallback (if you prefer the by-hand approach):
1. Tune an empty or very weak frequency and note typical `dbfs`/`compensated`.
2. Tune a strong clean local station and note typical `compensated`.
3. Set `signal_floor_dbfs` ≈ empty-frequency compensated − 3 dB; `signal_ceil_dbfs` ≈ strong-station compensated + 3 dB.

Symptoms of bad meter calibration:
- empty channels always look too strong: the floor is below your site's noise floor, so raise `signal_floor_dbfs` (the `[METER]` suggester catches this automatically)
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

The tuner reports this condition itself, so you do not have to hunt for it:

- the console prints `[RF] overload/image ghosting suspected: clip=… level=… dbfs=…`
  as soon as the front end is clipping while the tuned station is strong. The
  hint is not verbose-only, because it is the one diagnostic that explains a
  duplicate *while you listen*; it is rate-limited so a persistent condition
  does not flood the log.
- `GET /api/status` carries `ghost_risk` (bool) next to `clip` and `overload`.
- the fm-dx-webserver control plugin shows it as the **Duplicate** cell
  (`RISK` when set), with the clip ratio next to the **Overload** cell.

On the paths where the gain is pinned (the sdrpp gain strategy, a fixed
`-g`/`rtl_gain_db`, or the tuner's own AGC) the auto-gain ladder cannot pull
it down, so the tuner prints `[GAIN] clip-protect inactive (…): …` naming the
knob to turn instead.

Note that a duplicate of a station is *not* the same thing as the station
repeating itself: a retune to the frequency that is already tuned no longer
restarts the programme (the tuner skips the redundant retune and only
re-asserts the gain/AGC registers), so what you hear is a genuine second
copy, not the DSP restarting.

**How much headroom a gain setting leaves.** An FM station's modulation peaks
sit a few dB above its average level, and a saturated front end mixes the
strong station with itself — the intermodulation product is heard as a second
copy of that station. What decides whether that happens is how much room the
peaks have below the converter's ceiling, not the average level. Measured on
an R820T with the local station on 96.2 MHz (peak = the highest sample in
the capture, ceiling = full scale):

| Gain setting | Station (channel) | Peak | Headroom | Clip ratio |
|---|---|---|---|---|
| manual 10 dB | −19 dBFS | −9.7 dBFS | 9.7 dB | 0.0000 |
| manual 12 dB | −16 dBFS | −7.3 dBFS | 7.3 dB | 0.0000 |
| manual 14 dB | −15 dBFS | −5.4 dBFS | 5.4 dB | 0.0000 |
| dongle AGC (`rtl_gain_db = -1`) | −14 dBFS | −6.2 dBFS | 6.2 dB | 0.0000 |
| auto ladder (climbs to 18 dB) | −13 dBFS | −3.4 dBFS | 3.4 dB | 0.0000 |

So on this site nothing clips in steady state, and the AGC is not the villain
it looks like in the numbers. But the margin is what matters: 3.4 dB of
headroom is one strong neighbour away from the clipper, while 9.7 dB is not.
That is why the shipped profile uses a fixed 10 dB: the most headroom, no
pumping, and the meter off the top of its scale.

One thing *does* clip with the dongle's AGC: the front end's own auto-gain
overshoots when it is engaged. On an R820T the samples right after the switch
saturate the converter for 100–600 ms (measured here: 55–90% of the samples
at the rail) before the AGC settles ~7 dB below the ceiling. The tuner drops
that burst before the DSP sees it (see "Runtime Behavior"), so the switch
costs a short silence instead of a burst of noise and a false "duplicate
station" warning. The same window covers the settling burst after a connect
or a reconnect.


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
- a strong local station makes the dongle's own AGC clip the ADC (see
  "Duplicates of strong stations" above: the AGC targets the average level,
  the FM peaks clip)

Avoid fixed manual gain when:
- moving between locations
- changing antennas often
- local RF density varies a lot over time

For general listening, strategy-managed gain is usually easier to keep sane —
but on a site with a strong local station, a fixed gain that leaves headroom
for the modulation peaks is the safer choice. The `[SIG] clip=` column tells
you when the front end is overdriven: lower `rtl_gain_db` until it reads
`0.0000` while the station still sits clearly above the noise floor.

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

Spectrum plot is empty or ends short of the requested frequency:
- an empty plot during a sweep is normal — the plot fills in as the sweep advances
- a plot that *ends short* means the line was longer than the client's receive buffer; the tuner now widens the step automatically and logs a `[SCAN] warning`
- a plot that stays empty after the sweep ended means the client was refused the scan (`a0`); a guest session may not run one — configure a password, or use an authenticated client

A station is drawn on the wrong frequency:
- the reporting grid is aligned with the CCIR raster (87.6 MHz + n × 0.2 MHz) whenever the step divides it, so a station is drawn on its own frequency
- a step that does not divide the 200 kHz raster cannot hold every station exactly; the tuner logs a `[SCAN] warning` naming the consequence
- if *every* station sits slightly off, that is the dongle's frequency error — set `freq_correction_ppm` from a station whose exact frequency you know
- if one strong station has a weaker copy next to it, that is an overload image — reduce the gain first

The scan levels and the signal meter disagree:
- the meter integrates the whole channel and the scan 30 kHz per grid point, so a modulated station reads higher on the meter than on the plot; an unmodulated carrier reads the same on both
- if the scan is uniformly much lower, check that the channel bandwidth matches what the meter is using (`Sw` / `w0_bandwidth_hz`)
- the scan line is remapped onto the client's fixed 3..84 plot window; the `S` line and the REST status keep the 0..120 scale, so the two are not directly comparable as numbers

A phantom station appears in the plot:
- the plot is a spectrum: a signal that is not a station is a bump in the noise floor, not a spike
- a spike that is much higher than the floor is a real signal — check the gain, an overloaded front end answers with an image next to the station
- a bump that only shows up on one sweep is a noise maximum; a second sweep tells the two apart

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
# This is a CPU trade, not a quality trade: on a strong local station the
# stereo image then collapses and comes back (measured here: 8 dB weaker and
# swinging over 22 dB, with 6 % of the seconds fully muted). Keep the default
# "normal" unless the CPU is the problem.
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
- macOS (`macos-latest`, manual dispatch only)
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
- Manually dispatched macOS build job:
  - `fm-sdr-tuner-macos` (RTL-only binary + `README.md` + `USER-MANUAL.md` + ready-to-use `fm-sdr-tuner.ini` + `fm-sdr-tuner.ini.example` + `rest_test_panel.py` + `run_rest_panel.sh`)
- Windows MinGW job:
  - `fm-sdr-tuner-windows-mingw` (`.exe` + required DLLs + `dependencies.txt` + `README.md` + `USER-MANUAL.md` + ready-to-use `fm-sdr-tuner.ini` + `fm-sdr-tuner.ini.example` + `rest_test_panel.py`)

All CI release artifacts are RTL-only. SDRplay builds require a local macOS or
Linux SDK installation and `-DFM_TUNER_ENABLE_SDRPLAY=ON`.

Notes:
- macOS CI does not run automatically for pushes or pull requests. Dispatch
  `.github/workflows/build-macos.yml` manually when a macOS build or release
  artifact is needed.
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
- sdr-j-fm (algorithmic references for pilot recovery without a bandpass FIR,
  adaptive Costas-style stereo separation, and squelch — see P18 and P19 in
  `todo.md`)

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
