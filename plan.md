# FM-SDR-Tuner Code Review Plan

Last reviewed against current source: 2026-06-17 (post v1.6.2, on v1.6.3)

## Scope

Tracks verified remaining work in the current tree, with rationale and
validation strategy. The shorter `todo.md` is the punch-list view of the
same items. Closed tickets are not retained; consult `git log` and merged
PR descriptions for history.

This plan focuses on:

- protocol robustness gaps that still exist
- backend extensibility (the SoapySDR readiness blocker)
- DSP correctness under ADC-rail saturation
- runtime decomposition for future maintainability
- test coverage gaps that could still allow regressions through CI

## Executive Summary

### Remaining problem areas

1. **Tuner backend selection is still binary** (`rtl_sdr` / `rtl_tcp` /
   `sdrplay`) — dispatched by a `SourceKind` enum, not a capability-based
   abstraction. `SoapySDR` cannot be added cleanly on top of the current
   `TunerController` / `TunerSession` split. This is the headline of
   milestone 1.7.
2. **Application::run() still owns too much orchestration.** Session
   lifecycle, XDR facade wiring, scan restore, mute windows,
   reconnect, processing loop — all in one large function.
   Structural debt for any new runtime feature.
3. **ADC-rail saturation behaviour.** The channel Kaiser FIR rings
   past unit envelope under chronic clipping; the dBFS clamp hides
   the symptom, and the gain policy still accepts requests that would
   overload.

## Verified Remaining Findings

| ID | Priority | Location | Finding | Recommended action | Validation |
|---|---|---|---|---|---|
| P5 | Medium | `src/application.cpp` | `Application::run()` still owns session bootstrap, tuner lifecycle, XDR lifecycle, scan restore, mute windows, reconnection flow, and the main processing loop. Makes backend work and future runtime fixes harder than they need to be. | Split into smaller responsibilities: session bootstrap, command/control state application, scan coordinator, main DSP/audio loop. Keep `Application` as the assembly layer. | Behavior-preserving tests around start/stop, reconnect, scan restore, and retune mute. |
| P6 | High | `src/tuner_controller.cpp`, `src/tuner_session.cpp` | Tuner backend selection is a `SourceKind` switch over `rtl_sdr` / `rtl_tcp` / `sdrplay`. Not a sufficient abstraction for `SoapySDR`. Control surface is scattered between `TunerController`, `TunerSession`, and gain policy. | Backend interface that owns connect/disconnect, tuning, gain/AGC capabilities, sample-rate constraints, and capability queries. Policy in one layer, transport/device specifics in another. | Backend contract tests using a fake backend before adding `SoapySDR`. |
| P17b | Medium | `src/runtime_loop.cpp` (gain policy) | The IQ FIR L1 normalization landed in v1.6.2 (`processing.iq_fir_l1_normalize`, default off), so post-filter envelope can be hard-bounded. Still open: the gain policy accepts `G01` / `G11` requests that would push the front end into chronic ADC saturation. | Gain policy refuses to enable IF AGC (`G01`/`G11`) when recent `clip` ratio exceeds threshold; log `[GAIN] refusing G01: clip=...` and report the chronic-clip ratio back through the XDR meter rather than silently mis-representing the level. | Gain-policy unit test for the IF-AGC refusal path under a synthetic chronic-clip ratio. |
| P18 | High | `src/stereo_decoder.cpp` (`m_liquidPilotBandFilter`, `m_delayLine`) | The 243-tap complex bandpass FIR for pilot recovery dominates the post-v1.6.0 profile (`dotprod_crcf_execute_neon4`, ~525 samples = ~10% of one core). `research/sdr-j-fm/src/fm/pilot-recover.cpp` removes the bandpass entirely and relies on the PLL loop bandwidth itself for filtering: `PhaseError = pilot * sin(oscillatorPhase); oscillatorPhase += PhaseError * gain + omega`. The L-R recovery delay line also shortens because there's no FIR group delay to compensate. | Replace `m_liquidPilotBandFilter.push/execute` with a product-detector PLL on the raw MPX float. Reuse `m_liquidPilotPll` for the NCO. Adjust `m_delaySamples` to whatever residual phase compensation the new path needs (likely zero). Keep the lock-detection (`m_pilotConfidence`) path. | Verify lock acquisition time on a clean strong station is within 2× of current. Verify lock holds on the captured `mpx_88600_60s.wav` fixture. Re-baseline real-RF CPU on the same M1 hardware A/B protocol — expect 5-10 pp savings on one core. Listening test for adjacent-channel splatter scenarios (where the bandpass was earning its keep). |
| P19 | Medium | `src/stereo_decoder.cpp` (L-R recovery block) | Today's L-R recovery `2·Re(MPX·conj(PLL_38k)²)` is sensitive to pilot phase error: a 5° error in the pilot reference rotates the L-R subcarrier by ~10° before demodulation, producing ~3% measurable crosstalk. `research/sdr-j-fm/src/fm/stereo-separation.cpp` (Thomas Neder, 2023) implements `PerfectStereoSeparation`: a Costas-style adaptive separator that drives the cos/sin cross-coupling on the L-R complex baseband to zero. | Add an adaptive phase-shift integrator that sits between the pilot PLL output and the L-R demix multiply: `sinCosPath = sincos(pilotPhase + accPhaseShift) * mux; LPF(sinCosPath); error = real * imag; accPhaseShift += alpha * error`. Lock detector reuses `mean_error < threshold` over a stability window. Bypassed when `m_forceMono` or `m_stereoBlend < threshold`. | Synthetic test feeds MPX with a deliberately-rotated pilot reference and asserts post-convergence crosstalk is ≤ 1/10 of the pre-convergence value. Listening regression against the captured WAV fixture confirms no degradation on clean signals. |

## DSP Chain Alignment Review

The FM demod → pilot PLL → stereo → de-emph → resample chain was
cross-checked against the open-source WFM consensus (SDR++, GNU
Radio `gr-analog`, CSDR, redsea, liquid-dsp `freqdem`). The current
implementation matches the consensus in all load-bearing respects:

- Polar discriminator FM demod via liquid `freqdem` (same as GNU
  Radio `quadrature_demod_cf`).
- Coherent 38 kHz generation by squaring the 19 kHz NCO
  (conjugate-multiply-twice on the delayed MPX).
- Matched group-delay compensation on the (L+R) path using
  `(ntaps-1)/2` samples.
- De-emphasis applied exactly once per sample (mono path skipped in
  stereo mode because `processSplit` is called with `monoOut = nullptr`).
- Bilinear-pre-warped one-pole de-emphasis matching GNU Radio
  `fm_emph.py`.
- Per-channel independent resampling in `AFPostProcessor::process`.
- RDS path mirrors redsea: 171 kHz target, `nco_crcf` PLL at 57 kHz,
  `symsync_crcf` RRC β=0.8, BPSK, 5-offset-word block sync.
- Soft stereo blend driven by pilot coherence / PLL residual / PLL
  frequency error. Both SDR++ and GNU Radio omit any soft blend;
  retaining this is a deliberate strength.
- LMS pilot canceller two-tap subtracts the 19 kHz residual from mono
  output; dispersion-form CMA equalizer with leak regularization sits
  between the channel FIR and discriminator.

## Missing Test Coverage

These would catch regressions that current CI can't:

1. ALSA enumeration failure path (`AudioOutput::listAlsaDevices`).
2. Off-thread WAV writer shutdown-flush.
3. High-IQ-rate decimation-path test (`--iq-rate 1024000` / `2048000`).
4. Scan FFT plan / buffer reuse across retunes (the fallback retune-count
   bound and the post-retune buffer flush are already covered in
   `test_scan_engine`).
5. Handshake/auth truncation directly exercising `recvLine()` overflow.
6. `WavWriter` / `AudioOutput::writeWAVData` 4 GiB boundary +
   fatal-error propagation.
7. Analog-de-emphasis response-reference regression test (post the
   bilinear switch in v1.5.0).
8. IF-AGC refusal path under chronic clip (depends on P17b landing).
9. DSP / feature gaps:
   - Multipath equalizer behaviour on a multi-ray channel that's
     also moving (Doppler-like). Currently only the static
     2-ray test exists in `test_dsp_chain`.
   - HiCut crossfade response audibility test (no current direct test).
   - Adaptive bandwidth state machine across simulated SNR
     transitions over hysteresis windows.
   - MPX-audio output reconnect-after-device-disappearance behaviour
     (e.g., USB DAC unplugged mid-stream).

## Cross-Cutting Conclusions

### SoapySDR Readiness

SoapySDR still should not be added on top of the current controller
split. The next clean step is the backend capability interface (P6),
then a `SoapySDR` implementation behind it. This is the headline
work for the 1.7 milestone.

- Current source selection is a transport-oriented `SourceKind` switch.
- Capability differences between direct RTL, `rtl_tcp`, SDRplay, and
  Soapy backends grow quickly.
- A fake backend in tests would also unblock more comprehensive
  test coverage for retune / reconnect flows.

### Measurement methodology

The v1.6.0 perf pass surfaced a methodology issue worth recording:
M1 P/E core scheduling adds ±5 pp of noise to CPU% measurements on
unchanged code. Going forward, perf work should be A/B'd via
back-to-back same-state runs (the three-runs-at-once protocol used
in the v1.6.0 commits) rather than the open-loop baseline harness
that produced misleading absolute numbers. Better yet, validate on
a more deterministic target (RPi 4 / fixed clock) before celebrating
single-digit-pp gains.

## Recommended Delivery Order

### Patch (1.6.x)

1. ADC-rail-aware gain policy refusal (P17b) — correctness fix, mostly
   invisible until needed.

### Milestone (1.7) — Broad SDR Backend Support

1. Backend capability interface (P6).
2. `SoapySDR` implementation behind that interface.
3. Backend contract tests with a fake backend.
4. CLI / config / XDR integration cleanup for source selection.

**Stretch candidates that pair naturally with 1.7's architectural work:**

- Drop the pilot bandpass FIR (P18) — the biggest remaining CPU win, but
  also an architectural shape change in `StereoDecoder` that benefits
  from being landed in the same major version as the backend refactor
  so users can be told "everything reshuffled at 1.7, here's what
  changed".
- Adaptive L-R phase alignment (P19) — independent of backend work but
  pairs with 1.7 for the same release-narrative reason.

### Milestone (1.8) — Runtime Structural Cleanup

1. Decompose `Application::run()` (P5) into smaller responsibilities
   now that the backend layer is abstracted.
2. Pick off the missing-test-coverage items above as the affected
   subsystems are touched.

## References

Algorithmic ideas in P18 and P19 are drawn from
[`sdr-j-fm`](https://github.com/JvanKatwijk/sdr-j-fm) by Jan van Katwijk
(GPL-3.0), with the adaptive stereo separator (P19) specifically
contributed by Thomas Neder in 2023. A reference clone lives at
`research/sdr-j-fm/` for in-tree comparison; the project is also
acknowledged in `README.md`'s component table.
