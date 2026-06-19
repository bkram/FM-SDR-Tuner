# FM-SDR-Tuner Issues and Improvements

Last verified against current source: 2026-06-17 (post v1.6.2, on v1.6.3)

This file is a short-form punch list of open work — see `plan.md` for the
fuller analysis with rationale and validation strategy. Closed tickets are
not retained; consult `git log` and merged PR descriptions for history.

## Verified Open Issues

### High

- **Tuner backend abstraction (P6)**: source selection is a `SourceKind`
  switch (`rtl_sdr` / `rtl_tcp` / `sdrplay`), transport-oriented rather than
  capability-based. Adding `SoapySDR` cleanly requires a backend interface
  that owns connect/tune/gain/capabilities — see `src/tuner_session.cpp`
  and `src/tuner_controller.cpp`. Blocks any future Soapy / HackRF / Airspy
  work.
- **Drop the pilot bandpass FIR (P18)**: today `StereoDecoder` runs a 243-tap
  complex bandpass on every IQ sample at 256 kHz — the biggest remaining
  hotspot in the post-v1.6.0 profile (`dotprod_crcf_execute_neon4`). Reference
  implementation in `research/sdr-j-fm/src/fm/pilot-recover.cpp` uses a bare
  product-detector PLL on the raw MPX float and relies on the loop bandwidth
  itself to provide the bandpass behavior. Estimated **5-10 pp of one core
  saved** plus a simpler architecture (no FIR group delay to compensate in
  the L-R recovery delay line). Trade-off: less out-of-band rejection on the
  PLL input, must validate on stations with strong adjacent splatter.
- **ADC-rail-aware gain policy refusal (P17b)**: the L1 normalization on the
  IQ FIR is wired (`processing.iq_fir_l1_normalize`, default off — see v1.6.2)
  so post-filter envelope can be hard-bounded. Still open: the gain policy
  in `src/runtime_loop.cpp` accepts `G01` / `G11` requests that would push
  the front end into chronic ADC saturation. Add a refusal path that
  reports the chronic-clip ratio back through the XDR meter rather than
  silently mis-representing the level.
- **Application::run() decomposition (P5)**: `src/application.cpp` still
  owns session bootstrap, XDR facade wiring, scan restore, mute windows,
  reconnect, and the main DSP loop in one large function. Structural debt
  that makes backend work and future runtime fixes harder than they need to
  be.

### Medium

- **Adaptive L-R phase alignment (P19)**: replace the current
  `2·Re(MPX·conj(PLL)²)` L-R recovery (sensitive to pilot phase error) with
  a Costas-style adaptive separator that drives the cos/sin cross-coupling
  to zero. Reference: `PerfectStereoSeparation` in
  `research/sdr-j-fm/src/fm/stereo-separation.cpp` by Thomas Neder. Improves
  measured stereo separation on stations with marginal pilot quality
  (where our current chain has 5-10° pilot phase error → ~3% crosstalk).
  Self-contained ~150 lines; uses the existing pilot PLL as the phase
  reference and adds a small integrator + LPF + error-witness path.
- **`m_demodScratch` growth check on every call** (`src/fm_demod.cpp`):
  reserve-and-reuse strategy can be tightened. Cheap, mechanical, isolated.
- **Mixed newline style** between `std::endl` and `"\n"` across
  `src/audio_output.cpp`, `src/application.cpp`. Logging consistency only.
- **`#if defined(...)` vs `#ifdef` style** in `src/audio_output.cpp`.
  Cosmetic.

### Low / cross-cutting

- **Subsystem-grouped include layout**: `research/sdr-j-fm` organizes
  `includes/fm/`, `includes/rds/`, `includes/various/` by subsystem. We
  do this partially in `include/dsp/` but most headers are flat. As the
  codebase grows this matters; defer until P5 refactor naturally surfaces
  the grouping.
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
- Scan FFT plan / buffer reuse across retunes (the fallback retune-count
  bound and the post-retune buffer flush are already covered in
  `test_scan_engine`).
- `recvLine()` overflow + auth truncation directly.
- `WavWriter` / `AudioOutput::writeWAVData` 4 GiB boundary saturation
  + fatal-error propagation.
- Analog-de-emphasis response-reference regression.
- IF-AGC refusal path under chronic clip (depends on P17b).

## Future Milestones

- **1.6.x** further patch releases for the remaining gain-policy refusal
  work (P17b) if it comes up before the SoapySDR work.
- **1.7** — Broad SDR backend support. Backend capability interface (P6)
  → `SoapySDR` implementation → contract tests → CLI/config/XDR
  integration cleanup. Stretch goals if time allows: the pilot-FIR
  removal (P18) and adaptive L-R alignment (P19), since both are
  architectural-shape changes that pair naturally with the backend
  refactor.
- **1.8** — Runtime structural cleanup. Decompose `Application::run`
  (P5) into smaller responsibilities now that the backend layer is
  abstracted. Pick up the mixed-newline / preprocessor-style sweep
  alongside the affected subsystems.
