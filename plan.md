# FM-SDR-Tuner Code Review Plan

Last reviewed against current source: 2026-05-20 (post v1.6.0)

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

### Recently closed

**v1.6.0 (CPU pass, perf-focused):**
- Polynomial `fast_atan2f` for the pilot PLL phase detector, max error
  ~0.0015 rad — replaces libm atan2f in the inner loop.
- Gated Hi-Blend biquad redesign (every 64 samples instead of per-sample)
  — eliminates `tanf` from the hot path.
- Pilot bandpass FIR transition widened 3 kHz → 4 kHz, tap count drops
  from ~325 to ~243 at 256 kHz (~25% fewer complex MACs/sec through
  liquid's NEON dotprod).
- 1024-entry sin/cos LUT for the two per-sample sincos pairs. Wash
  against Apple's `__sincosf_stret` on M1; insurance for glibc / MSVC
  builds where libm is less optimised, and a stepping stone for SIMD
  batching.

**v1.5.1 (DSP overhaul, feature work):**
- CMA multipath equalizer with dispersion target + leak regularization
  toward centered delta (`src/dsp/multipath_eq.cpp`).
- Adaptive de-emphasis HiCut crossfade.
- SNR-driven adaptive channel bandwidth with hysteresis.
- LMS 19 kHz pilot canceller (default on).
- 2nd-order Butterworth Hi-Blend biquad, gear-shifted pilot PLL.
- Live MPX → audio device path (`src/mpx_audio_output.cpp`) — Core
  Audio + ALSA, WinMM refused due to 48 kHz cap.
- WAV writer learned input-side resampling; new `--mpx-rate` flag.
- Meter calibration window widened to -65/-5 dBFS; `[METER]` log
  with observed-min/max suggestions; `signal_level.snrDb` returns
  NaN when no channel-FFT estimate available.
- `forceMono` now suppresses the stereo indicator broadcast for
  consistency with the audio actually being mono.
- Unified RTL-SDR clip threshold via `include/dsp/iq_saturation.h`.
- XDR FM-DX protocol gained `B` / `W` / `D` handlers (force-mono,
  bandwidth, de-emphasis) — were silently erroring for clients
  using the FM-DX protocol.
- WinMM `waveOutPrepareHeader` return now checked; invalid
  `WHDR_DONE` preset dropped.
- MSVC: native `__cpuid` / `_xgetbv` CPU feature detection; AVX2
  path enabled when `/arch:AVX2` is set.
- Mono / MPX live: same-device collision guard; mono-then-stereo
  Core Audio fallback (MPX duplicated to L=R) for strict DACs.

**v1.5.0 (audio + scan hardening — for context):**
- WAV header writes now check every `fseek`/`fwrite`, guard the 4 GiB
  RIFF limit, propagate writer-thread fatal errors.
- `XDRServer::start()` reports `errno` / `WSAGetLastError` on failures.
- `FMDemod::setBandwidthHz` / `setDspAgcMode` reset downstream state on
  reconfig (no audible click on XDR retune).
- Output L/R through a tanh soft-limiter with metered `audioClipRatio`.
- Stereo acquire/drop hysteresis is now a time-based EMA, block-size
  independent.
- Default `audio.enable_audio = true`, implicit guest-mode when no
  password set, `tuner.default_freq = 87500`, blend = aggressive.
- `-b soft|normal|aggressive` CLI flag.
- `FMDemod::m_filteredChannelPowerDbfs` clamped ≤ 0 dBFS (display-side
  band-aid for ADC-rail ringing).
- Scan engine `usableHalfSpanHz` is a true half-span; consecutive
  captures overlap by ≥ 25%; scan output is raw per-channel level.
- `XDRServer` callback setters consolidated onto one template helper.
- De-emphasis default is 50 µs; bilinear-pre-warped coefficients
  matching GNU Radio `fm_emph.py`.

### Remaining problem areas

1. **Tuner backend selection is still binary** (`rtl_sdr` / `rtl_tcp`)
   — not a capability-based abstraction. `SoapySDR` cannot be added
   cleanly on top of the current `TunerController` / `TunerSession`
   split. This is the headline of milestone 1.7.
2. **Application::run() still owns too much orchestration.** Session
   lifecycle, XDR facade wiring, scan restore, mute windows,
   reconnect, processing loop — all in one ~700-line function.
   Structural debt for any new runtime feature.
3. **ADC-rail saturation behaviour.** The channel Kaiser FIR rings
   past unit envelope under chronic clipping; the dBFS clamp hides
   the symptom but the FIR isn't L1-renormalized and the gain
   policy still accepts requests that would overload.
4. **Runtime `stereo_blend` control is not wired to XDR** — `-b`
   covers startup, but clients cannot A/B blend modes live.

## Verified Remaining Findings

| ID | Priority | Location | Finding | Recommended action | Validation |
|---|---|---|---|---|---|
| P5 | Medium | `src/application.cpp` (~700 LoC) | `Application::run()` still owns session bootstrap, tuner lifecycle, XDR lifecycle, scan restore, mute windows, reconnection flow, and the main processing loop. Makes backend work and future runtime fixes harder than they need to be. | Split into smaller responsibilities: session bootstrap, command/control state application, scan coordinator, main DSP/audio loop. Keep `Application` as the assembly layer. | Behavior-preserving tests around start/stop, reconnect, scan restore, and retune mute. |
| P6 | High | `src/tuner_controller.cpp`, `src/tuner_session.cpp` | Tuner backend selection is binary `rtl_sdr` / `rtl_tcp`. Not a sufficient abstraction for `SoapySDR`. Control surface is scattered between `TunerController`, `TunerSession`, and gain policy. | Backend interface that owns connect/disconnect, tuning, gain/AGC capabilities, sample-rate constraints, and capability queries. Policy in one layer, transport/device specifics in another. | Backend contract tests using a fake backend before adding `SoapySDR`. |
| P15b | Medium | `src/xdr_server.cpp`, `src/xdr_facade.cpp`, `src/application.cpp` | `-b soft\|normal\|aggressive` covers startup-time selection, but `StereoDecoder::setBlendMode` is never reached from an XDR command. Clients cannot A/B blend modes live. | Add an XDR command (candidate prefix `Fb`) that calls `DspPipeline::setBlendMode` via the existing facade callback pattern. Reuse the `XDRServer::assignCallback` template. | Extend `test_xdr_unit` with a case that sends the blend command and asserts the target `BlendMode` is set. |
| P17 | Medium | `src/fm_demod.cpp` (IQ filter init, power calc), `src/runtime_loop.cpp` | Under ADC-rail overload the IQ Kaiser FIR rings with L1 norm > 1, so `\|y\|² > 1`. The `≤ 0 dBFS` display clamp hides the symptom; underlying causes (non-L1-bounded FIR, gain policy not refusing overload-causing requests) remain. | (a) Renormalize IQ FIR taps so L1 norm ≤ 1 (guarantees `\|y\| ≤ max\|x\|`), keep the DC-passband gain as a documented constant. (b) Gain policy refuses to enable IF AGC (`G01`/`G11`) when recent `clip` ratio exceeds threshold; log `[GAIN] refusing G01: clip=...`. | `test_dsp_chain` synthetic 99% rail-clipped IQ block asserts `out.channelPowerDbfs <= 0` AND `\|y\|` stays bounded by `max\|x\|` after L1 normalization. Gain-policy unit test for the IF-AGC refusal path. |

## DSP Chain Alignment Review

(Carried forward from v1.5.0 plan — all checks still valid as of
v1.6.0. The CMA multipath equalizer and pilot canceller added in
v1.5.1 don't change the load-bearing parts of the FM → pilot → stereo
chain.)

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
- **New in v1.5.1**: LMS pilot canceller two-tap subtracts the 19 kHz
  residual from mono output; dispersion-form CMA equalizer with leak
  regularization sits between the channel FIR and discriminator.

## Missing Test Coverage

These would catch regressions that current CI can't:

1. ALSA enumeration failure path (`AudioOutput::listAlsaDevices`).
2. Off-thread WAV writer shutdown-flush.
3. High-IQ-rate decimation-path test (`--iq-rate 1024000` / `2048000`).
4. Scan performance regression (FFT plan / buffer reuse, fallback
   retune count bound).
5. Handshake/auth truncation directly exercising `recvLine()` overflow.
6. `WavWriter` / `AudioOutput::writeWAVData` 4 GiB boundary +
   fatal-error propagation.
7. Analog-de-emphasis response-reference regression test (post the
   bilinear switch in v1.5.0).
8. IF-AGC refusal path under chronic clip (depends on P17 landing).
9. **New gaps from v1.5.1 / v1.6.0:**
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

- Current source selection is binary and transport-oriented.
- Capability differences between direct RTL, `rtl_tcp`, and Soapy
  backends grow quickly.
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

1. Runtime XDR `stereo_blend` control (P15b) — small, isolated,
   high user value, completes the half-shipped CLI flag.
2. IQ FIR L1 normalization + ADC-rail-aware gain policy (P17) —
   correctness fix, mostly invisible until needed.

### Milestone (1.7) — Broad SDR Backend Support

1. Backend capability interface (P6).
2. `SoapySDR` implementation behind that interface.
3. Backend contract tests with a fake backend.
4. CLI / config / XDR integration cleanup for source selection.

### Milestone (1.8) — Runtime Structural Cleanup

1. Decompose `Application::run()` (P5) into smaller responsibilities
   now that the backend layer is abstracted.
2. Pick off the missing-test-coverage items above as the affected
   subsystems are touched.
