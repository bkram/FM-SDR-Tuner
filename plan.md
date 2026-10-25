# FM-SDR-Tuner Code Review Plan

Last reviewed against current source: 2026-10-25 (on v1.8.0, branch `v1.8`)

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
| P17b | Medium | `src/xdr_server.cpp`, `src/xdr_facade.cpp`, `src/application.cpp` (gain policy) | The IQ FIR L1 normalization landed in v1.6.2 (`processing.iq_fir_l1_normalize`, default off), so post-filter envelope can be hard-bounded. Still open: the gain policy accepts `G01` / `G11` requests that would push the front end into chronic ADC saturation. | Gain policy refuses to enable IF AGC (`G01`/`G11`) when recent `clip` ratio exceeds threshold; log `[GAIN] refusing G01: clip=...` and report the chronic-clip ratio back through the XDR meter rather than silently mis-representing the level. | Gain-policy unit test for the IF-AGC refusal path under a synthetic chronic-clip ratio. |
| P18 | Low | `src/stereo_decoder.cpp` (`m_pilotBand`, `m_delayLine`) | The CPU case is mostly gone: since `a8c777d` the pilot band (245-tap Kaiser prototype) and the RDS band (163 taps) are filtered on the band mixed down to DC at 32 kHz, an eighth of their former cost (see `PERFORMANCE.md`). `research/sdr-j-fm/src/fm/pilot-recover.cpp` removes the band-pass entirely and relies on the PLL loop bandwidth for filtering, which would also remove the FIR group delay from the L-R delay line. | Only worth doing for the simpler architecture. The noise-gated stereo detection, the pilot SNR that drives the blend, and the pilot deviation reading all use the band power this filter measures, so each needs a replacement. | The separation, clean-pilot and dead-channel tests in `test_dsp_chain`; lock acquisition time within 2× of current; a listening test with strong adjacent splatter, where the band-pass earns its keep. |
| P19 | Medium | `src/stereo_decoder.cpp` (L-R recovery block) | Since the pilot filter became analytic, separation is 33 dB flat from 1 to 14 kHz on a clean signal. The remaining limit is that L+R and L-R are aligned only to the nearest whole sample (offsets of -1 and +1 give 19 and 6 dB). `research/sdr-j-fm/src/fm/stereo-separation.cpp` (Thomas Neder, 2023) implements `PerfectStereoSeparation`, a Costas-style adaptive separator that drives the cos/sin cross-coupling on the L-R baseband to zero. | Either a fractional-delay alignment of the L+R path, or the adaptive separator between the pilot PLL and the L-R demix: `sinCosPath = sincos(pilotPhase + accPhaseShift) * mux; LPF(sinCosPath); error = real * imag; accPhaseShift += alpha * error`, bypassed when `m_forceMono` or the blend is low. | The separation test in `test_dsp_chain` with a higher threshold; a synthetic test with a deliberately rotated pilot reference asserting post-convergence crosstalk at most 1/10 of the pre-convergence value. |

## DSP Chain Alignment Review

The FM demod → pilot PLL → stereo → de-emph → resample chain was
cross-checked against the open-source WFM consensus (SDR++, GNU
Radio `gr-analog`, CSDR, redsea, liquid-dsp `freqdem`). The current
implementation matches the consensus in all load-bearing respects:

- Polar discriminator FM demod via liquid `freqdem` (same as GNU
  Radio `quadrature_demod_cf`).
- Coherent 38 kHz generation by squaring the 19 kHz NCO
  (conjugate-multiply-twice on the delayed MPX).
- Matched group-delay compensation on the (L+R) path: the pilot band's
  `(ntaps-1)/2` samples plus the 8-sample interpolation delay of the 32 kHz
  band (`a8c777d`).
- De-emphasis applied exactly once per sample (mono path skipped in
  stereo mode because `processSplit` is called with `monoOut = nullptr`).
- Bilinear pre-warped one-pole de-emphasis, run at 256 kHz before the
  resampler on both the stereo and mono paths, within 0.2 dB of the analogue
  50 and 75 µs curves to 15 kHz.
- Per-channel independent resampling in `AFPostProcessor::process`.
- RDS path mirrors redsea: 171 kHz target, `nco_crcf` PLL at 57 kHz,
  `symsync_crcf` RRC β=0.8, BPSK, 5-offset-word block sync.
- Soft stereo blend driven by pilot coherence / PLL residual / PLL
  frequency error / pilot SNR, with stereo detection gated on the pilot
  SNR so noise is never flagged as stereo. Both SDR++ and GNU Radio omit any soft blend;
  retaining this is a deliberate strength.
- LMS pilot canceller two-tap subtracts the 19 kHz residual from mono
  output; dispersion-form CMA equalizer with leak regularization sits
  between the channel FIR and discriminator.

## Missing Test Coverage

These would catch regressions that current CI can't:

1. ALSA enumeration failure path (`AudioOutput::listAlsaDevices`).
2. IF-AGC refusal path under chronic clip (depends on P17b landing).
3. Scan cancel latency: skipping the fallback pass after a cancel is
   verified on hardware only.
4. `AudioOutput::runWavWriterThread` drain on shutdown; the shutdown-flush
   test covers `WavWriter` only.
5. The high-IQ-rate decimation test uses a clean in-band carrier and cannot
   detect a missing anti-alias filter; it needs an out-of-band interferer.
6. The scan's FFT plan reuse depends on `thread_local IqSpectrum` in
   `scan_engine.cpp`; the test exercises the class, not that line.
7. DSP / feature gaps:
   - Multipath equalizer behaviour on a multi-ray channel that's
     also moving (Doppler-like). Currently only the static
     2-ray test exists in `test_dsp_chain`.
   - HiCut crossfade response audibility test (no current direct test).
   - MPX-audio output reconnect-after-device-disappearance behaviour
     (e.g., USB DAC unplugged mid-stream).

`recvLine()` overflow and auth truncation are **not** a gap: `test_protocol`
exercises oversized XDR lines, oversized FM-DX lines, oversized handshakes and
oversized auth lines end to end. A direct unit test of the file-static helper
would only require exporting it.

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

**Status: v1.7.0 shipped the SDRplay backend but not the capability
interface.** The `SourceKind` switch in `src/tuner_controller.cpp` is still
how the sources are selected, so P6 is still open and `SoapySDR` still
cannot be added cleanly on top of it.

**Stretch candidates that pair naturally with 1.7's architectural work:**

- Drop the pilot bandpass FIR (P18) — no longer a CPU win since the band
  runs at 32 kHz (`a8c777d`); only the architectural simplification remains.
- Adaptive L-R phase alignment (P19) — independent of backend work but
  pairs with 1.7 for the same release-narrative reason.

### Milestone (1.8) — Runtime Structural Cleanup

1. Decompose `Application::run()` (P5) into smaller responsibilities.
2. Pick off the missing-test-coverage items above as the affected
   subsystems are touched.

**Status: in progress on branch `v1.8`.** `Application::run()` is still one
~1050-line function (P5 open), and step 1's premise — "now that the backend
layer is abstracted" — is not true yet: P6 is still open.

## References

Algorithmic ideas in P18 and P19 are drawn from
[`sdr-j-fm`](https://github.com/JvanKatwijk/sdr-j-fm) by Jan van Katwijk
(GPL-3.0), with the adaptive stereo separator (P19) specifically
contributed by Thomas Neder in 2023. A reference clone lives at
`research/sdr-j-fm/` for in-tree comparison; the project is also
acknowledged in `README.md`'s component table.
