# FM-SDR-Tuner Issues and Improvements

Last verified against current source: 2026-05-20 (post v1.6.0)

This file is a short-form punch list of open work — see `plan.md` for the
fuller analysis with rationale and validation strategy. Closed tickets are
not retained; consult `git log` and merged PR descriptions for history.

## Verified Open Issues

### High

- **Tuner backend abstraction (P6)**: The `rtl_sdr` vs `rtl_tcp` source switch is
  binary and transport-oriented. Adding `SoapySDR` cleanly requires a backend
  interface that owns connect/tune/gain/capabilities — see `src/tuner_session.cpp`
  and `src/tuner_controller.cpp`. Blocks any future Soapy / HackRF / Airspy
  work.
- **IQ FIR L1 normalization + ADC-rail-aware gain policy (P17)**: under
  saturation the channel Kaiser FIR can ring past unit envelope (L1 > 1). A
  `dBFS ≤ 0` clamp was added as a meter-side band-aid; the underlying FIR
  taps aren't L1-renormalized, and the gain policy still accepts `G01` /
  `G11` requests that would overload. Touch points: `src/fm_demod.cpp` (FIR
  init), `src/runtime_loop.cpp` (gain refusal path).
- **Application::run() decomposition (P5)**: `src/application.cpp` is still
  ~700 lines after the v1.5.1 / v1.6.0 work, owning session bootstrap, XDR
  facade wiring, scan restore, mute windows, reconnect, and the main DSP
  loop. Structural debt that makes backend work and future runtime fixes
  harder than they need to be.

### Medium

- **Runtime `stereo_blend` control via XDR (P15b)**: `-b soft|normal|aggressive`
  covers startup-time selection but no XDR command toggles it at runtime.
  Candidate prefix `Fb`. Wire via `XDRServer::assignCallback` template.
- **`m_demodScratch` growth check on every call** (`src/fm_demod.cpp`):
  reserve-and-reuse strategy can be tightened. Cheap, mechanical, isolated.
- **Mixed newline style** between `std::endl` and `"\n"` across
  `src/audio_output.cpp`, `src/application.cpp`. Logging consistency only.
- **`#if defined(...)` vs `#ifdef` style** in `src/audio_output.cpp`.
  Cosmetic.

### Low / cross-cutting

- **WinMM socket-handle truncation**: `int m_socket` / `int m_serverSocket`
  in `rtl_tcp_client.cpp` and `xdr_server.cpp` hold `SOCKET` (UINT_PTR) on
  Win64. In practice fits in 32 bits and `< 0` check works, but it's a
  long-standing portability wart.
- **`WSAStartup` without `WSACleanup`** in `xdr_server.cpp` and
  `rtl_tcp_client.cpp`. Process exit reclaims; cosmetic.

## Future CPU work (deferred from v1.6.0)

The v1.6.0 perf pass closed out Tier 1 (atan2 polynomial, gated biquad
redesign) and Tier 2A/B (pilot bandpass widening, sincos LUT). The Tier 3
ideas were documented but deferred because the M1 measurement noise
floor exceeded the expected gain:

- **Block-batched FIR convolution**: replace per-sample push+execute with
  64-sample blocks so liquid's NEON kernel stays hot. Estimated ~3-5 pp
  on M1. Significant code change.
- **Custom phase accumulator for the pilot PLL**: skip liquid's NCO step +
  `nco_crcf_constrain` and use a tiny `uint32_t` modulo accumulator.
  Estimated ~1-2 pp.
- **Fixed-rate polyphase 256→48 resampler**: liquid's `resamp_rrrf` has
  per-call ratio handling that a fixed-ratio design can skip. Estimated
  ~1-2 pp.

Worth revisiting only if a real CPU-bound deployment (e.g. Raspberry Pi 4)
needs the headroom — see the README "Running On Low-CPU Devices" section.

## Missing Test Coverage

These would catch regressions that current CI can't:

- ALSA enumeration failure path (`AudioOutput::listAlsaDevices`).
- Off-thread WAV writer shutdown-flush.
- High-IQ-rate (`--iq-rate 1024000` / `2048000`) decimation-path
  behaviour test.
- Scan performance regression (FFT plan/buffer reuse, fallback retune
  count bound).
- `recvLine()` overflow + auth truncation directly.
- `WavWriter` / `AudioOutput::writeWAVData` 4 GiB boundary saturation
  + fatal-error propagation.
- Analog-de-emphasis response-reference regression.
- IF-AGC refusal path under chronic clip (depends on P17).

## Future Milestones

- **1.6.x** patch releases for the small remaining `stereo_blend` /
  IQ-L1 fixes if they come up before the SoapySDR work.
- **1.7** — Broad SDR backend support. Backend capability interface (P6)
  → `SoapySDR` implementation → contract tests → CLI/config/XDR
  integration cleanup. This is the headline of 1.7.
- **1.8** — Runtime structural cleanup. Decompose `Application::run`
  (P5) into smaller responsibilities now that the backend layer is
  abstracted.
