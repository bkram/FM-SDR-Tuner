# FM-SDR-Tuner Issues and Improvements

Last verified against current source: 2026-10-22 (on v1.8.0, branch `v1.8`)

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
- **Drop the pilot bandpass FIR (P18)**: the CPU case for this is mostly
  gone. Since `a8c777d` the pilot band (245-tap prototype) and the RDS band
  (163 taps) run on the band mixed down to DC at 32 kHz, an eighth of their
  former cost; see `PERFORMANCE.md`. What remains is the simpler
  architecture: `research/sdr-j-fm/src/fm/pilot-recover.cpp` uses a bare
  product-detector PLL on the raw MPX and relies on the loop bandwidth for
  the band-pass, with no FIR group delay to compensate in the L-R delay line.
  Trade-off: less out-of-band rejection on the PLL input, and the noise-gated
  stereo detection and pilot SNR depend on the band power the filter
  measures, so both would need a replacement.
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

## Future CPU work

See `PERFORMANCE.md` for the measured profile (x86 and M1) and the options,
with the expected gain for each: an own SIMD dot-product kernel, a two-stage
audio resampler, building liquid-dsp with SIMD, block-batched filtering and a
fast discriminator `atan2`. The pilot and RDS bands already run at 32 kHz
(`a8c777d`).

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
- `WavWriter` / `AudioOutput::writeWAVData` 4 GiB boundary saturation
  - fatal-error propagation.
- Analog-de-emphasis response-reference regression.
- IF-AGC refusal path under chronic clip (depends on P17b).

(`recvLine()` overflow and auth truncation are covered end to end in
`test_protocol` — oversized XDR and FM-DX lines, oversized handshakes and
oversized auth lines are all exercised. A direct unit test of the static
helper would need it exported, which is why it is not listed here.)

## Future Milestones

- **1.7** — Broad SDR backend support. Backend capability interface (P6)
  → `SoapySDR` implementation → contract tests → CLI/config/XDR
  integration cleanup. Stretch goals if time allows: the pilot-FIR
  removal (P18) and adaptive L-R alignment (P19), since both are
  architectural-shape changes that pair naturally with the backend
  refactor. **Shipped as v1.7.0 with the SDRplay backend only — the
  capability interface (P6) was not done, so it is still open.**
- **1.8** — Runtime structural cleanup. Decompose `Application::run`
  (P5) into smaller responsibilities. **In progress: the backend layer is
  not abstracted yet (P6 still open), and P5 is still open.**
