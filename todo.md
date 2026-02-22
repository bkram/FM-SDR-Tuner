# TODO: Migrate Custom DSP to liquid-dsp

## Goal
Replace project-specific DSP implementations with `liquid-dsp` where practical, while preserving current tuner behavior (audio quality, latency, scan responsiveness, and XDR compatibility).

## Scope
- In scope:
  - FM discriminator/demod path (`FMDemod`)
  - MPX filtering and decimation
  - Resampling paths used by audio/RDS
  - Stereo pilot/PLL and stereo separation path
  - RDS subcarrier demod (already partially moved via redsea)
- Out of scope (for now):
  - XDR protocol behavior
  - UI/client command semantics
  - Non-DSP control logic (scan/auth/network)

## Phase 0: Baseline and Instrumentation
- [ ] Freeze current reference behavior on `codex/fix/latency`.
- [ ] Add repeatable test captures (IQ + WAV + known station set).
- [ ] Capture baseline metrics:
  - Audio underruns / choppiness incidents
  - End-to-end latency (retune-to-audio, retune-to-RDS)
  - CPU usage per platform (macOS arm64, Linux x64/arm64, Windows)
  - RDS group rate, PI stability, error nibble distribution
- [ ] Add a runtime DSP backend selector (e.g. `processing.dsp_backend = custom|liquid`) for A/B testing.

## Phase 1: Shared liquid-dsp Infrastructure
- [ ] Create internal wrappers for liquid primitives (AGC, NCO, FIR, resampler, PLL/symsync) similar to `redsea_port/dsp/liquid_wrappers` but app-scoped.
- [ ] Centralize DSP object lifecycle and reset semantics (retune/start/stop/scan).
- [ ] Ensure deterministic block sizes and no unbounded internal queues.
- [ ] Add compile-time guards and clear CMake errors when `liquid-dsp` is unavailable.

## Phase 2: FM Demod Migration (`FMDemod`)
- [ ] Replace custom IQ prefilter + discriminator with liquid equivalents.
- [ ] Preserve existing bandwidth command mapping (`W` behavior) via liquid filter parameterization.
- [ ] Preserve deemphasis behavior and existing output rate contract.
- [ ] Keep custom path available behind backend toggle until parity is proven.
- [ ] Validation criteria:
  - No regression in choppy/fast playback
  - Matching audio level and spectral balance within tolerance
  - No increase in retune artifacts

## Phase 3: Stereo Decoder Migration (`StereoDecoder`)
- [ ] Replace custom pilot PLL with liquid NCO/PLL chain.
- [ ] Move L-R extraction filters to liquid FIR design/execution.
- [ ] Preserve mono-forced mode and blend behavior contracts.
- [ ] Validate stereo lock time and false-lock rate on weak signals.

## Phase 4: AF Post-Processing Migration (`AFPostProcessor`)
- [ ] Replace custom resampling/filter blocks with liquid resampler/FIR.
- [ ] Keep exact output format/rate expected by native audio backends.
- [ ] Validate latency and queue pressure against current defaults.

## Phase 5: RDS Pipeline Consolidation
- [ ] Remove remaining duplicated custom RDS front-end logic not used after redsea integration.
- [ ] Ensure redsea path receives full MPX consistently in all audio/stereo modes.
- [ ] Add targeted RDS stress tests (weak signal, multipath, rapid retune).

## Phase 6: Performance and Stability Hardening
- [ ] Tune chunk sizes and buffering with liquid paths active.
- [ ] Profile hot paths and remove redundant conversions/copies.
- [ ] Verify no new ring overflows/short-read amplification in direct RTL-SDR mode.
- [ ] Confirm clean stop/start without ticking noise when tuner inactive.

## Phase 7: Default Switch and Cleanup
- [ ] Make `liquid` backend default after parity thresholds are met.
- [ ] Remove dead custom DSP code incrementally (demod/stereo/post blocks).
- [ ] Keep rollback option for one release cycle (`custom` backend).
- [ ] Update README and config docs for backend selection/removal timeline.

## Test Matrix (Required Before Default Switch)
- [ ] macOS arm64 (Core Audio, rtl_sdr direct)
- [ ] Linux x64 (ALSA, rtl_sdr direct + rtl_tcp)
- [ ] Linux arm64 (ALSA, rtl_sdr direct)
- [ ] Windows x64 (WinMM, rtl_sdr direct + rtl_tcp)

## Acceptance Gates
- [ ] Audio quality: no recurring choppy/fast playback on known-good stations.
- [ ] Latency: no worse than current baseline by >10% in retune-to-audio.
- [ ] RDS: PI lock stability and group decode rate equal or better than baseline.
- [ ] CPU: within acceptable envelope per platform (documented in benchmark notes).
- [ ] Reliability: 30+ minute soak test without buffer overflow spam or decode collapse.

## Rollback Strategy
- [ ] Keep backend runtime toggle until two stable releases.
- [ ] If regressions appear, switch default back to `custom` and retain liquid path behind flag while fixing.

## Work Order (Recommended)
1. Phase 0 + backend toggle
2. Phase 1 infra
3. Phase 2 demod
4. Phase 3 stereo
5. Phase 4 post-processing
6. Phase 5 RDS cleanup
7. Phase 6 hardening
8. Phase 7 default switch + cleanup
