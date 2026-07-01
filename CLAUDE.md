# CLAUDE.md / AGENTS.md

Developer + AI-assistant guide for this repository. `AGENTS.md` is a symlink to this file — they're the same content. Anything you'd want to know to make a change here lives below.

## Project Overview

FM broadcast tuner and XDR server built around RTL-SDR IQ input. Single executable `fm-sdr-tuner` that owns a DSP pipeline, an optional native audio output, and an XDR-compatible control server on port `7373`. Treat this as experimental software: behavior changes between releases in favor of measurable improvements and end-to-end correctness.

## Common Commands

### Build

Default out-of-tree build lives in `build/`:

```bash
cmake -S . -B build
cmake --build build
```

Per-platform notes:

**macOS** — must select an architecture explicitly:

```bash
cmake -S . -B build -DCMAKE_OSX_ARCHITECTURES=arm64    # Apple Silicon
cmake -S . -B build -DCMAKE_OSX_ARCHITECTURES=x86_64   # Intel
cmake --build build -j
```

**Linux** — no architecture flag needed:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

**Windows (MinGW-w64 / MSYS2)** — supported in CI. Install the toolchain and deps, then the same `cmake -S . -B build` invocation works.

If you use vcpkg locally, point CMake to the vcpkg toolchain:

```bash
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
```

AVX2/FMA paths are off by default; enable via `-DFM_TUNER_ENABLE_X86_AVX2=ON` only for known-capable x86 targets. On MSVC this propagates to `/arch:AVX2`; on GCC/Clang to `-mavx2 -mfma`. NEON paths on ARM are auto-enabled.

### Tests

```bash
ctest --test-dir build --output-on-failure --no-tests=error
```

Run one test by name (the `add_test(NAME ...)` entries in `tests/CMakeLists.txt` are the truth — e.g. `signal_level`, `dsp_chain`, `xdr_unit`, `scan_engine`):

```bash
ctest --test-dir build -R '^signal_level$' -V
```

RTL-SDR hardware tests are gated and skipped by default (CTest `SKIP_RETURN_CODE 4`). Enable with `FM_TUNER_RUN_RTL_SDR_LIVE=1` and optionally `FM_TUNER_RUN_RTL_SDR_COMPARE=1`; other `FM_TUNER_RTL_*` env vars override frequencies, sample rate, and gain (see `README.md`).

### Coverage

```bash
cmake -S . -B build-coverage -DCMAKE_BUILD_TYPE=Debug \
      -DFM_TUNER_ENABLE_COVERAGE=ON -DFM_TUNER_ENABLE_X86_AVX2=OFF
cmake --build build-coverage
ctest --test-dir build-coverage --output-on-failure
gcovr --root . build-coverage --txt --txt-summary
```

Coverage requires GCC/Clang (MSVC is a hard error). The CI coverage artifact is `fm-sdr-tuner-coverage-ubuntu-24.04`.

### Packaging

Linux `.deb` / `.rpm`:

```bash
cpack -G DEB --config build/CPackConfig.cmake
./scripts/test-linux-arm-builds.sh   # ubuntu:24.04, debian:trixie, fedora:42, fedora:43 container smoke tests
```

### Static analysis / formatting

```bash
clang-tidy src/*.cpp -- -Iinclude
clang-format -i src/*.cpp include/**/*.h
```

There is no clang-tidy or clang-format enforcement wired into CMake — they are run ad-hoc.

### Running locally

```bash
./build/fm-sdr-tuner -c fm-sdr-tuner.ini           # config-first
./scripts/run_rtl_xdr_test.sh                      # RTL-SDR + XDR on 127.0.0.1:7373, pw test123
./scripts/run_rtl_local_audio.sh                   # --auto-start local speaker listening
./scripts/run_rest_panel.sh                        # browser control panel for the [rest] API (test stand-in for the fm-dx-webserver plugin)
```

At least one output (`-s` audio, `-w` WAV, or `-i` IQ capture) must be enabled or the app refuses to start. By default the tuner does not auto-start; an XDR client start command activates the pipeline.

## Architecture

### Runtime flow

`main.cpp` parses CLI + INI via `parseAppOptions` and hands the resulting `AppOptions` to `Application::run()`. `Application` is the top-level lifecycle owner: it constructs `AudioOutput`, `WavWriter`, optional IQ capture `FILE*`, `TunerSession`, and `XDRServer`, then drives the IQ read loop, feeding samples into the DSP pipeline and tearing everything down on exit.

Hot path (IQ → audio/WAV):

1. `TunerSession` wraps `TunerController` and talks to either `RtlSdrDevice` (direct USB, default) or `RtlTcpClient` (network).
2. `RuntimeLoop` / `ProcessingRunner` pulls IQ bytes in fixed SDR-block chunks.
3. `DspPipeline` (`src/dsp_pipeline.cpp`) runs complex decimation → `FMDemod` → `StereoDecoder` → `AFPostProcessor`, driven by `liquid-dsp` primitives wrapped in `src/dsp/liquid_primitives.cpp` and the runtime in `src/dsp/runtime.cpp`. The pipeline emits L/R at `48000 Hz` (audio rate is fixed).
4. The MPX tap feeds `RdsWorker`, which runs the `src/redsea_port/` port of redsea on a dedicated thread so RDS decode cannot stall audio. The MPX tap is also the source for `--mpx-wav` capture (optionally resampled by `WavWriter`) and `--mpx-audio` live routing to a system audio device (`MpxAudioOutput`, independent of the 48 kHz stereo audio path).
5. Outputs are demuxed to `AudioOutput` (48 kHz stereo audio; Core Audio / ALSA / WinMM via `#ifdef`), `WavWriter` (mono MPX + stereo audio capture), `MpxAudioOutput` (live mono MPX → audio device; Core Audio / ALSA only — WinMM is rejected because its 48 kHz cap aliases the subcarriers), and/or raw IQ capture.

Optional DSP features (all gated by config; default off except `pilot_canceller`):

- **Pilot canceller** (`src/stereo_decoder.cpp`): two-tap LMS subtracting a phase-locked 19 kHz copy from the mono audio path.
- **Adaptive de-emphasis "HiCut"** (`src/af_post_processor.cpp`): per-channel pair of de-emphasis IIRs, crossfaded by stereo quality.
- **Adaptive channel bandwidth** (`src/adaptive_bandwidth.cpp` + `src/runtime_loop.cpp::maybeAdjustAdaptiveBandwidth`): SNR-driven policy with 2 s hysteresis, plumbed through the existing `requestedBandwidthHz` atomic so changes flow through the regular retune path. Waits for `isfinite(signal.snrDb)` before the first decision.
- **Multipath equalizer** (`src/dsp/multipath_eq.cpp`): dispersion-form CMA (Godard 1980) inserted in `FMDemod::demodulate{,Complex}` between the channel FIR and the discriminator. Adaptation gated by `m_stereo.isStereo()` and pulled toward a centered-delta tap via leak regularization to avoid CMA's known phase-ambiguity on FM.

### Control plane

`XDRServer` (port 7373) is the control surface. `XdrFacade` brokers between the server and `TunerController` + `ScanEngine`. `ScanEngine` uses `SignalLevel` (the dBFS / floor / clip estimator — see `[SIG]` log lines) to drive scan decisions. `XDRServer` speaks the XDR-GTK / FM-DX protocol and authenticates with OpenSSL (constant-time hash compare; fails closed if the secure RNG is unavailable).

`RestServer` (`src/rest_server.cpp`, optional `[rest]` port, default off) is a second, **anonymous** control surface — a minimal HTTP/1.1 API for an fm-dx-webserver client plugin. It exists because the XDR protocol has no vocabulary for SDR-specific settings (manual dB gain, SDRplay LNA state, antenna input, bias-tee); the REST API carries those plus the common knobs. It drives the **same** `TunerController` + `XdrCommandState` request path as the XDR server (frequency/bandwidth go through the pending-flag retune path; SDRplay-only knobs call `TunerController` directly and no-op on RTL), so both backends behave identically. No auth by design — bind to localhost/trusted networks. `GET /api/status`, `GET|POST /api/control` (query-string or flat-JSON body). Test it with `scripts/rest_test_panel.py`.

### SDR sources

`TunerController` dispatches over three backends via a `SourceKind` enum: `RtlSdrDevice` (USB), `RtlTcpClient` (network), and `SDRplayDevice` (RSP). RTL sources deliver uint8 IQ (`IqFormat::U8`); SDRplay delivers normalized `complex<float>` (`IqFormat::CF32`) at full 16-bit range. The application read loop branches on `nativeFormat()`: CF32 feeds the demod via `DspPipeline::process(const std::complex<float>*)` while a quantized 8-bit shadow (same full-scale reference) feeds `computeSignalLevel`/scan/calibration unchanged. `SDRplayDevice` `dlopen`s the proprietary `libsdrplay_api` at runtime (built only when `-DFM_TUNER_ENABLE_SDRPLAY=ON` finds the SDK headers; compiles to a no-op stub otherwise), configures 2.048 MHz / 8 = 256 kHz to match the pipeline's `INPUT_RATE` (1:1 decimation).

Two SDRplay-specific scan details: (1) `SDRplayDevice::readIQ` blocks until a full block accumulates (bounded ceiling) rather than returning a tiny partial — each scan retune raises the API `reset` flag and flushes the ring, and a short partial read would starve the scan's FFT. (2) `runtime_loop::handleControlAndScan` drives a **wide-bandwidth scan mode** (`TunerController::setScanWideMode` → `Update_Ctrl_Decimation`): for the duration of a sweep the RSP runs undecimated at 2.048 MHz with a wider IF filter, so each retune covers ~8× more spectrum (full-band sweep ~13 s → <2 s), then 256 kHz is restored when the scan ends. No-op on RTL (whose ring is always full, so its scan is already fast).

### Configuration surface

`fm-sdr-tuner.ini` is the primary control surface (CLI is meant for transient overrides). `Config` (`src/config.cpp`) parses the `[tuner]`, `[audio]`, `[sdr]`, `[sdrplay]`, `[rest]`, `[processing]`, `[xdr]` sections. `[sdrplay]` keys: `lna_state`, `antenna`, `bias_tee`. `[rest]` keys: `enabled`, `port`, `bind_address`. Signal-meter calibration keys (`signal_floor_dbfs`, `signal_ceil_dbfs`, `signal_bias_db`; default window is `-65`/`-5`/`0` = 60 dB range) and DSP keys (`w0_bandwidth_hz`, `dsp_agc`, `stereo_blend`, `agc_mode`, `pilot_canceller`, `hicut`, `adaptive_bandwidth`, `multipath_eq`, `multipath_eq_taps`) all flow through here and down into `DspPipeline` / `SignalLevel`. The README's "Setup And Tuning Guide" is the canonical reference for what each knob does in practice — consult it before changing defaults.

Diagnostic log prefixes (grep-friendly):
- `[SIG]` — per-block signal stats (dbfs, compensated, floor, snr, level, clip).
- `[ST]` — stereo decoder state (pilot magnitude, lock, quality, blend).
- `[METER]` — calibration window at startup and periodic session min/max with floor/ceil suggestions.
- `[BW]` — adaptive bandwidth controller decisions and applied changes.
- `[EQ]` — multipath equalizer's smoothed envelope-error metric (only emitted when the equalizer is enabled and verbose logging is on).

### Platform conditionals

Audio backends are compile-time selected: `FM_TUNER_HAS_COREAUDIO` (Apple), `FM_TUNER_HAS_ALSA` (Linux; unconditionally enabled), `FM_TUNER_HAS_WINMM` (Windows). `FM_TUNER_HAS_RTLSDR` is always defined because librtlsdr is a hard requirement. SIMD paths (SSE/AVX on x86, NEON on ARM) sit behind `#ifdef` in DSP code; AVX2/FMA is opt-in via `FM_TUNER_ENABLE_X86_AVX2`.

### Tests layout

Tests are Catch2-based and compiled as **per-target executables that re-compile the specific source files they exercise** (see `tests/CMakeLists.txt`) — they do not link the main `fm-sdr-tuner` target. When you add or move a source file, update every test target that includes it. `catch_main.cpp` / `catch_compat.h` exist to bridge Catch2 v2 vs v3. Adding a test means adding both `add_executable(test_foo ...)` and `add_test(NAME foo COMMAND test_foo)`.

## Project Structure

```
src/
  main.cpp                 - Entry, hands off to Application::run()
  application.cpp          - Lifecycle owner; wires IQ + DSP + audio + XDR
  app_options.cpp          - CLI argument parser
  config.cpp               - INI parser
  runtime_loop.cpp         - Control + scan dispatch, auto-gain, adaptive BW
  processing_runner.cpp    - Per-block hot path; signal-level + [METER] logging
  rtl_sdr_device.cpp       - Direct USB RTL-SDR source
  rtl_tcp_client.cpp       - rtl_tcp network client
  tuner_session.cpp        - Tuner abstraction over the sources below
  tuner_controller.cpp     - Frequency/gain/AGC state machine; 3-way source dispatch
  sdrplay_device.cpp       - SDRplay RSP source (dlopen'd API; CF32 16-bit IQ)
  rest_server.cpp          - Anonymous HTTP control API (optional [rest] port)
  fm_demod.cpp             - FM discriminator + channel FIR + multipath EQ insertion
  stereo_decoder.cpp       - Gear-shift PLL, pilot canceller, biquad Hi-Blend, L-R
  af_post_processor.cpp    - Resampling, de-emphasis (+ optional HiCut crossfade)
  rds_decoder.cpp          - RDS group decoder (legacy hook)
  rds_worker.cpp           - Background thread feeding the redsea port
  redsea_port/             - Ported RDS decode pipeline
  signal_level.cpp         - dBFS / FFT-based channel + noise estimate
  scan_engine.cpp          - XDR scan execution
  xdr_server.cpp           - XDR protocol server (port 7373)
  xdr_facade.cpp           - Bridge between XDR commands and tuner controller
  adaptive_bandwidth.cpp   - Policy + hysteresis for SNR-driven channel BW
  audio_output.cpp         - Core Audio / ALSA / WinMM 48 kHz stereo speaker output
  mpx_audio_output.cpp     - Core Audio / ALSA live MPX → audio device (mono, ≥192 kHz)
  wav_writer.cpp           - Buffered WAV writer; optional input-side resampler
  cpu_features.cpp         - CPU capability detection (GCC/Clang and MSVC paths)
  dsp/
    liquid_primitives.cpp  - C++ wrappers over liquid-dsp primitives
    multipath_eq.cpp       - CMA equalizer (Godard 1980; dispersion form + leak)
    runtime.cpp            - DSP runtime / reset orchestration

include/                   - Header files including dsp/iq_saturation.h
                             (shared RTL-SDR ADC saturation constants)
tests/                     - Catch2 per-target test executables
                             (test_dsp_chain, test_wav_writer,
                              test_adaptive_bandwidth, etc.)

Not committed in this repository:
research/                  - Reference implementations
  SDRPlusPlus/             - FM demod, stereo, RDS algorithms
  xdr-gtk/                 - XDR protocol client
  FM-DX-Tuner/             - TEF tuner firmware
  xdrd/                    - Original XDR daemon
```

The CLI surface and runtime knobs are documented exhaustively in `README.md`; this file does not duplicate the per-flag table.

## Key Design Decisions

- **SIMD**: DSP hot paths have SSE/AVX (x86) and NEON (ARM) optimized code behind `#ifdef`. AVX2/FMA off by default — opt-in via `FM_TUNER_ENABLE_X86_AVX2`.
- **Audio**: Native backends only — Core Audio (macOS), ALSA (Linux), WinMM (Windows). Plus WAV file output.
- **XDR Protocol**: Compatible with XDR-GTK and FM-DX-Webserver clients on port 7373. Two protocol code paths share `XDRServer` but dispatch via `processCommand` (XDR-GTK) vs `processFmdxCommand` (FM-DX after `x\n` handshake).
- **RDS**: Decoded in a background thread (`RdsWorker`) so real-time audio is never blocked.
- **Live MPX rebroadcast**: `MpxAudioOutput` is a separate sink from `AudioOutput` — same MPX tap source, independent device + rate.

## Code Conventions

- C++17, no RTTI/exceptions assumptions beyond stdlib defaults.
- Default to writing no comments; the README tuning guide carries the "why" for runtime behavior.
- When modifying DSP or signal-level behavior, treat the captured `mpx_88600_60s.wav` / `stereo_88600_60s.wav` fixtures at the repo root as regression inputs.
- `research/` and `build*/` directories are not committed sources — do not edit as if they were.

### Static analysis / formatting tooling

There is no enforcement wired into CMake; the tools below are run ad-hoc.

**clang-tidy** — manually:

```bash
clang-tidy src/*.cpp -- -Iinclude -I/usr/local/include
```

Or via CMake (run from the build directory):

```bash
cmake -S . -B build -DCMAKE_CXX_CLANG_TIDY=clang-tidy
cmake --build build
```

**clang-format** — single file:

```bash
clang-format -i src/main.cpp
```

All source files:

```bash
clang-format -i src/*.cpp include/**/*.h
```

## License

GPLv3 — see `LICENSE`. Derived from components / ideas in SDRPlusPlus, XDR-GTK, FM-DX-Tuner, and xdrd; see `README.md` for the full component table and links.
