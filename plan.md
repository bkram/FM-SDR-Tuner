# FM-SDR-Tuner Code Review Plan

Last reviewed against current source: 2026-04-18

## Scope

This plan tracks verified remaining work in the current tree. It focuses on:

- remaining hot-path latency and allocation behavior
- protocol robustness gaps that still exist
- backend extensibility
- test coverage gaps that could still allow regressions through CI

Closed tickets are not retained; consult `git log` for history.

## Executive Summary

Recently closed (1.5 release work):

- WAV header writes now check every `fseek`/`fwrite`, guard the 4 GiB RIFF limit, and propagate writer-thread fatal errors.
- `XDRServer::start()` now reports `errno` / `WSAGetLastError` on socket/bind/listen failures.
- `FMDemod::setBandwidthHz` / `setDspAgcMode` and the `DspPipeline` wrappers now reset downstream state on runtime reconfig, removing the audible click on XDR bandwidth / AGC changes.
- Output L/R runs through a tanh soft-limiter (0.85 threshold) with a metered `audioClipRatio` in `DspPipeline::Result`, in place of the previous hard clamp.
- Stereo acquire/drop hysteresis is now a time-based EMA (tau 30 ms / 250 ms, thresholds 0.80 / 0.20), independent of block size.
- Out-of-box defaults: `audio.enable_audio = true`, implicit guest-mode when `xdr.password` is empty, `tuner.default_freq = 87500`, `signal_floor_dbfs = -50` / `signal_ceil_dbfs = -18`, `processing.stereo_blend = aggressive`.
- `-b soft|normal|aggressive` CLI flag for one-shot stereo blend override.
- `FMDemod::m_filteredChannelPowerDbfs` clamped to `≤ 0 dBFS` so positive values from IQ-FIR ringing under ADC rail saturation stop misleading the meter and the gain policy.
- Scan engine bin-mapping: `usableHalfSpanHz` is now a true half-span and is bounded by `Fs/2 − halfChannelBW`, so edge channels no longer wrap across Nyquist. Scan output is raw per-channel level (SNR gate removed) to match the XDR-GTK / TEF668x convention.
- Scan `centerStepHz` capped so consecutive captures always overlap by ≥ 25 %, eliminating wide-BW gaps that pushed every in-gap channel into the per-channel fallback path.
- `XDRServer` callback setters consolidated onto one template helper; `recvLine()` truncation handling verified correct; `std::endl` swept to `"\n"` for logging consistency.
- De-emphasis default flipped to 50 µs (EU/ITU); coefficients now use the bilinear-pre-warped form matching GNU Radio `gr-analog fm_emph.py` (≈0.5 dB closer to the analog reference near 10 kHz at 32 kHz output rate).

Remaining problem areas:

1. `Application::run()` still owns too much orchestration (session lifecycle, scan restore, mute windows, reconnect, processing loop) — structural debt that makes backend work harder than necessary.
2. Tuner backend selection is still a binary `rtl_sdr` / `rtl_tcp` switch, not a capability-based abstraction. A `SoapySDR` backend cannot be added cleanly on top of the current controller split.
3. Under ADC-rail saturation the IQ Kaiser FIR rings past unity (L1 norm > 1). The dBFS clamp hides the visual symptom but the filter is not yet L1-renormalized, and the gain policy still accepts `G01`/`G11` requests that would clearly overload.
4. Runtime `stereo_blend` control is not yet wired to any XDR command — the `-b` CLI flag covers startup-time selection but clients cannot change it live.

## Verified Remaining Findings

| ID | Priority | Location | Finding | Recommended action | Validation |
|---|---|---|---|---|---|
| P5 | Medium | `src/application.cpp:146-533` | `Application::run()` still owns too much runtime orchestration: startup, tuner lifecycle, XDR lifecycle, scan restore, mute windows, reconnection flow, and the main processing loop. This makes new backend work and future runtime fixes harder than they need to be. | Split the runtime into smaller responsibilities: session bootstrap, command/control state application, scan coordinator, and main DSP/audio loop. Keep `Application` as the assembly layer. | Refactor with behavior-preserving tests around start/stop, reconnect, scan restore, and retune mute. |
| P6 | Medium | `src/tuner_controller.cpp:3-59`, `src/tuner_session.cpp:19-63` | Tuner backend selection is still a binary `rtl_sdr` vs `rtl_tcp` switch. That is workable today, but it is not a sufficient abstraction for adding `SoapySDR` cleanly. The control surface is still scattered between `TunerController`, `TunerSession`, and gain policy code. | Introduce a backend interface that owns connect/disconnect, tuning, gain/AGC capabilities, sample-rate constraints, and capability queries. Keep policy in one layer and transport/device specifics in another. | Add backend contract tests using a fake backend before adding `SoapySDR`. |
| P15b | Medium | `src/xdr_server.cpp`, `src/xdr_facade.cpp`, `src/application.cpp` | The `-b soft\|normal\|aggressive` CLI flag covers startup-time selection, but `StereoDecoder::setBlendMode` is never reached from an XDR command. Clients cannot A/B blend modes live. | Add an XDR command (candidate prefix `Fb`) that calls `DspPipeline::setBlendMode` via the existing facade callback pattern, and a CLI acknowledgment reply. Reuse the `XDRServer::assignCallback` template for registration. | Extend `test_xdr_unit` with a case that sends the blend command and asserts the target `BlendMode` is set. |
| P17 | Medium | `src/fm_demod.cpp` (IQ filter init, power calc), `src/runtime_loop.cpp:85-125` | Under ADC-rail overload (IF AGC on + strong local, every IQ byte pinned to 0/255) the IQ Kaiser FIR rings with L1 norm > 1, so `\|y\|² > 1`. A dBFS clamp to `≤ 0` was added as a display/UX fix, but the underlying causes (non-L1-bounded FIR under saturation, and gain policy that doesn't reject requests that would clearly overload) remain. | (a) Renormalize the IQ FIR taps so the L1 norm is ≤ 1 (guarantees `\|y\| ≤ max\|x\|`), keeping the DC-passband gain as a documented constant so downstream FM-demod scaling doesn't shift. (b) In the gain-policy code, refuse to enable IF AGC (`G01`/`G11`) when recent `clip` ratio exceeds a threshold (e.g. 0.1) and log a `[GAIN] refusing G01: clip=...` message instead of silently accepting and oscillating. | Extend `test_dsp_chain` with a synthetic 99 % rail-clipped IQ block and assert `out.channelPowerDbfs <= 0` AND that `\|y\|` stays bounded by `max\|x\|` after the L1 normalization lands. Add a gain-policy unit test for the IF-AGC refusal path. |

## DSP Chain Alignment Review

The FM demod → pilot PLL → stereo → de-emph → resample chain was cross-checked against the open-source WFM consensus (SDR++ `core/src/dsp/demod/broadcast_fm.h` and `quadrature.h`; GNU Radio `gr-analog` `wfm_rcv_pll.py` and `fm_emph.py`; CSDR `libcsdr.c`; redsea `src/dsp/subcarrier.cc`; liquid-dsp `freqdem`). The current implementation matches the consensus in all load-bearing respects:

- Polar discriminator FM demod via liquid `freqdem` (same as GNU Radio `quadrature_demod_cf`).
- Coherent 38 kHz generation by squaring the 19 kHz NCO (conjugate-multiply-twice on the delayed MPX, `stereo_decoder.cpp:253-255`).
- Matched group-delay compensation on the (L+R) path (`stereo_decoder.cpp:66`), using `(ntaps-1)/2` samples.
- De-emphasis is applied exactly once per sample: mono path via `FMDemod::downsampleAudio`, stereo path via `AFPostProcessor` (with `FMDemod::processSplit` called with `monoOut = nullptr` so the mono filter is bypassed in stereo mode).
- Bilinear-pre-warped one-pole de-emphasis coefficients matching GNU Radio `fm_emph.py`.
- Per-channel independent resampling in `AFPostProcessor::process` (`af_post_processor.cpp:58-77`).
- RDS path (`redsea_port/`) mirrors redsea: 171 kHz target, `nco_crcf` PLL at 57 kHz, `symsync_crcf` RRC β=0.8, BPSK, 5-offset-word block sync.
- Soft stereo blend driven by pilot coherence / PLL residual / PLL-frequency error (`stereo_decoder.cpp:135-195`). Both SDR++ and GNU Radio omit any soft blend; keeping this is a deliberate strength.

### Claims reviewed and rejected

- **"Double de-emphasis at different rates"** — not true. The stereo path calls `FMDemod::processSplit` with `monoOut = nullptr`, so `downsampleAudio` (and its mono de-emphasis) is skipped. Only `AFPostProcessor` de-emphasis runs in stereo mode.
- **"`ComplexDecimator` uint8→uint8 path has rounding bias"** — real in the code but unreachable. `DspPipeline` uses `ComplexDecimator::executeComplex`; the uint8→uint8 overload has zero callers.

## Missing Test Coverage

1. No ALSA enumeration failure-path test for `AudioOutput::listDevices()` / `listAlsaDevices()`.
2. No dedicated shutdown-flush test for off-thread WAV writing.
3. No decimation-path performance or behavior test for `DspPipeline` staging when `iq_rate` is above `256000`.
4. No scan performance regression test (FFT plan/buffer reuse, fallback retune count bound).
5. No handshake/auth truncation test that exercises `recvLine()` overflow behavior directly.
6. No WAV 4 GiB boundary test covering `WavWriter` / `AudioOutput::writeWAVData` saturation and fatal-error propagation.
7. No response-reference regression test against the analog de-emphasis curve (post P14 bilinear switch).
8. No gain-policy unit test for the IF-AGC refusal path covered by P17(b).

## Cross-Cutting Conclusions

### SoapySDR Readiness

SoapySDR still should not be added on top of the current controller split.

- The current source selection is binary and transport-oriented.
- Capability differences between direct RTL, `rtl_tcp`, and Soapy backends grow quickly.
- The next clean step is a backend capability interface (P6), then a `SoapySDR` implementation behind it.

## Recommended Delivery Order

### Immediate

1. IQ FIR L1 normalization + ADC-rail-aware gain policy (P17).
2. Runtime XDR `stereo_blend` control (P15b).

### Structural Refactor

1. Break `Application::run()` into smaller runtime components (P5).
2. Introduce a tuner backend abstraction in preparation for `SoapySDR` (P6).

## Milestone Mapping

### 1.6 Broad SDR Backend Support

- backend capability interface (P6)
- `SoapySDR` implementation
- backend contract tests
- source/backend selection cleanup in CLI/config/XDR integration

### 1.7 DSP Hardening

- IQ FIR L1 normalization + ADC-rail aware gain policy (P17)
- response-reference regression tests against analog de-emphasis curve

### 1.9 First-Run UX Pass

- runtime `stereo_blend` control via XDR (P15b) — CLI portion already shipped
