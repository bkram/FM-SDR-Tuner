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
- [x] Runtime backend plumbing was implemented and used during migration.
- [x] Backend selector removed after migration completion; runtime is now liquid-only.

## Phase 1: Shared liquid-dsp Infrastructure
- [x] Create internal wrappers for liquid primitives (AGC, NCO, FIR, resampler, PLL/symsync) similar to `redsea_port/dsp/liquid_wrappers` but app-scoped.
- [x] Centralize DSP object lifecycle and reset semantics (retune/start/stop/scan).
- [x] Ensure deterministic block sizes and no unbounded internal queues.
- [x] Add compile-time guards and clear CMake errors when `liquid-dsp` is unavailable.

## Phase 2: FM Demod Migration (`FMDemod`)
- [x] Replace custom IQ prefilter + discriminator with liquid equivalents.
- [ ] Preserve existing bandwidth command mapping (`W` behavior) via liquid filter parameterization.
- [x] Preserve deemphasis behavior and existing output rate contract.
- [x] Remove custom path after parity testing; liquid path is authoritative.
- [ ] Validation criteria:
  - No regression in choppy/fast playback
  - Matching audio level and spectral balance within tolerance
  - No increase in retune artifacts

## Phase 3: Stereo Decoder Migration (`StereoDecoder`)
- [x] Replace custom pilot PLL with liquid NCO/PLL chain.
- [x] Move pilot band extraction and L/R audio filters to liquid FIR execution.
- [x] Preserve mono-forced mode and blend behavior contracts.
- [ ] Validate stereo lock time and false-lock rate on weak signals.

## Phase 4: AF Post-Processing Migration (`AFPostProcessor`)
- [x] Replace custom resampling/filter blocks with liquid resampler/FIR.
- [x] Keep exact output format/rate expected by native audio backends.
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
- [x] Make liquid backend default (liquid-only runtime).
- [x] Remove dead custom DSP code for demod/stereo/post blocks.
- [x] Remove rollback option and runtime backend selector.
- [x] Update README and config docs for backend selector removal.

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
- [ ] If regressions appear, reintroduce a temporary compile-time fallback path while fixing.

## Work Order (Recommended)
1. Phase 0 baseline capture
2. Phase 1 infra
3. Phase 2 demod
4. Phase 3 stereo
5. Phase 4 post-processing
6. Phase 5 RDS cleanup
7. Phase 6 hardening
8. Phase 7 cleanup and docs alignment
