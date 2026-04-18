# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

FM broadcast tuner and XDR server built around RTL-SDR IQ input. Single executable `fm-sdr-tuner` that owns a DSP pipeline, an optional native audio output, and an XDR-compatible control server on port `7373`. Treat this as experimental software: behavior changes between releases in favor of measurable improvements and end-to-end correctness.

## Common Commands

### Build

Default out-of-tree build lives in `build/`:

```bash
cmake -S . -B build
cmake --build build
```

macOS must select an architecture explicitly (`-DCMAKE_OSX_ARCHITECTURE=arm64` or `x86_64`). AVX2/FMA paths are off by default; enable via `-DFM_TUNER_ENABLE_X86_AVX2=ON` only for known-capable x86 targets.

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
./scripts/test-linux-arm-builds.sh   # ubuntu:24.04, debian:trixie, fedora:40 container smoke tests
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
```

At least one output (`-s` audio, `-w` WAV, or `-i` IQ capture) must be enabled or the app refuses to start. By default the tuner does not auto-start; an XDR client start command activates the pipeline.

## Architecture

### Runtime flow

`main.cpp` parses CLI + INI via `parseAppOptions` and hands the resulting `AppOptions` to `Application::run()`. `Application` is the top-level lifecycle owner: it constructs `AudioOutput`, `WavWriter`, optional IQ capture `FILE*`, `TunerSession`, and `XDRServer`, then drives the IQ read loop, feeding samples into the DSP pipeline and tearing everything down on exit.

Hot path (IQ → audio/WAV):

1. `TunerSession` wraps `TunerController` and talks to either `RtlSdrDevice` (direct USB, default) or `RtlTcpClient` (network).
2. `RuntimeLoop` / `ProcessingRunner` pulls IQ bytes in fixed SDR-block chunks.
3. `DspPipeline` (`src/dsp_pipeline.cpp`) runs complex decimation → `FMDemod` → `StereoDecoder` → `AFPostProcessor`, driven by `liquid-dsp` primitives wrapped in `src/dsp/liquid_primitives.cpp` and the runtime in `src/dsp/runtime.cpp`. The pipeline emits L/R at `48000 Hz` (audio rate is fixed).
4. The MPX tap feeds `RdsWorker`, which runs the `src/redsea_port/` port of redsea on a dedicated thread so RDS decode cannot stall audio.
5. Outputs are demuxed to `AudioOutput` (Core Audio / ALSA / WinMM via `#ifdef`), `WavWriter`, and/or raw IQ capture.

### Control plane

`XDRServer` (port 7373) is the control surface. `XdrFacade` brokers between the server and `TunerController` + `ScanEngine`. `ScanEngine` uses `SignalLevel` (the dBFS / floor / clip estimator — see `[SIG]` log lines) to drive scan decisions. `XDRServer` speaks the XDR-GTK / FM-DX protocol and authenticates with OpenSSL.

### Configuration surface

`fm-sdr-tuner.ini` is the primary control surface (CLI is meant for transient overrides). `Config` (`src/config.cpp`) parses the `[tuner]`, `[audio]`, `[sdr]`, `[processing]`, `[xdr]` sections. Signal-meter calibration keys (`signal_floor_dbfs`, `signal_ceil_dbfs`, `signal_bias_db`) and DSP keys (`w0_bandwidth_hz`, `dsp_agc`, `stereo_blend`, `agc_mode`) all flow through here and down into `DspPipeline` / `SignalLevel`. The README's "Setup And Tuning Guide" is the canonical reference for what each knob does in practice — consult it before changing defaults.

### Platform conditionals

Audio backends are compile-time selected: `FM_TUNER_HAS_COREAUDIO` (Apple), `FM_TUNER_HAS_ALSA` (Linux; unconditionally enabled), `FM_TUNER_HAS_WINMM` (Windows). `FM_TUNER_HAS_RTLSDR` is always defined because librtlsdr is a hard requirement. SIMD paths (SSE/AVX on x86, NEON on ARM) sit behind `#ifdef` in DSP code; AVX2/FMA is opt-in via `FM_TUNER_ENABLE_X86_AVX2`.

### Tests layout

Tests are Catch2-based and compiled as **per-target executables that re-compile the specific source files they exercise** (see `tests/CMakeLists.txt`) — they do not link the main `fm-sdr-tuner` target. When you add or move a source file, update every test target that includes it. `catch_main.cpp` / `catch_compat.h` exist to bridge Catch2 v2 vs v3. Adding a test means adding both `add_executable(test_foo ...)` and `add_test(NAME foo COMMAND test_foo)`.

## Code Conventions

- C++17, no RTTI/exceptions assumptions beyond stdlib defaults.
- Default to writing no comments; the README tuning guide carries the "why" for runtime behavior.
- When modifying DSP or signal-level behavior, treat the captured `mpx_88600_60s.wav` / `stereo_88600_60s.wav` fixtures at the repo root as regression inputs.
- `research/` and `build*/` directories are not committed sources — do not edit as if they were.
