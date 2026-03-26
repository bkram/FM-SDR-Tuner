# FM-SDR-Tuner Code Review Plan

Last reviewed against current source: 2026-03-26

## Scope

This plan tracks verified remaining work in the current tree.
It is not a copy of `todo.md`, and it intentionally excludes items that are already fixed.

It focuses on:

- remaining hot-path latency and allocation behavior
- protocol robustness gaps that still exist
- backend extensibility
- test coverage gaps that could still allow regressions through CI

## Executive Summary

The current tree has already closed several earlier high-risk issues:

- `XDRServer` now owns and joins client threads safely
- `RTLTCPClient` validates the rtl_tcp handshake header
- ALSA enumeration now propagates failures correctly
- audio output now uses bounded speaker/WAV ring buffers, off-thread WAV writing, and preallocated scaling scratch buffers
- the live signal meter now rejects DC contamination, reuses FFT state, and has regression coverage for center-spur and multi-rate cases
- live and scan tuning now respect FM broadcast band limits
- retune failure is no longer ignored by the runtime loop
- oversized unterminated XDR/FMDX command lines are now bounded and disconnected safely

The biggest remaining problems are now concentrated in four areas:

1. Scan performance still does avoidable allocation and tuner churn.
2. `DspPipeline` still uses `resize()`/`memmove()` staging for decimated IQ.
3. `Application::run()` still owns too much orchestration.
4. Backend abstraction is still too thin for the next major step (`SoapySDR`).

## Verified Remaining Findings

| ID | Priority | Location | Finding | Recommended action | Validation |
|---|---|---|---|---|---|
| P1 | Medium | `src/scan_engine.cpp:176-208` | Scan FFT input/output buffers and the FFT plan are still rebuilt inside the scan loop. This repeats for every tuned center and every average pass. It is avoidable CPU and allocator churn in an already expensive path. | Promote FFT plan and work buffers into reusable scan state sized for the current `nfft`. Recreate only when `nfft` changes. | Add a scan micro-benchmark and verify identical `U...` protocol output before/after the refactor. |
| P2 | Medium | `src/scan_engine.cpp:251-281` | Uncovered-channel fallback still retunes the tuner once per missing channel and computes signal level separately. On wide scans or awkward sample-rate/step combinations, this can dominate scan time and generate avoidable tuner churn. | Improve channel coverage math first, then reduce fallback scope. If fallback remains necessary, batch adjacent uncovered channels where possible or mark them explicitly so the cost is bounded and observable. | Add scan tests for wide ranges and unusual step/sample-rate combinations, plus timing comparison before/after optimization. |
| P3 | Medium | `src/dsp_pipeline.cpp:94-116` | Decimation staging still uses `m_iqStaging.resize()`, `memcpy()`, and `memmove()` every block when `m_iqDecimation > 1`. That introduces steady-state copy overhead directly in the DSP path. | Replace staging vector shuffling with a small ring/FIFO abstraction or phase-aware incremental decimator input handling. | Add a decimation-focused benchmark for `1024000` and `2048000` IQ rates. |
| P4 | Medium | `src/xdr_server.cpp:852-874` | Scan queue entries are still copied into a temporary vector on every poll loop before sending. The queue is small, but the pattern is still unnecessary churn in a tight network loop and scales poorly if scan traffic grows. | Keep the queue bounded, but change the send path to walk entries without rebuilding a temporary string vector each iteration. A sequence-indexed ring or snapshot cursor would simplify this. | Extend protocol tests to verify ordering and delivery while changing the internal queue representation. |
| P5 | Medium | `src/application.cpp:146-533` | `Application::run()` still owns too much runtime orchestration: startup, tuner lifecycle, XDR lifecycle, scan restore, mute windows, reconnection flow, and the main processing loop. This makes new backend work and future runtime fixes harder than they need to be. | Split the runtime into smaller responsibilities: session bootstrap, command/control state application, scan coordinator, and main DSP/audio loop. Keep `Application` as the assembly layer. | Refactor with behavior-preserving tests around start/stop, reconnect, scan restore, and retune mute. |
| P6 | Medium | `src/tuner_controller.cpp:3-59`, `src/tuner_session.cpp:19-63` | Tuner backend selection is still a binary `rtl_sdr` vs `rtl_tcp` switch. That is workable today, but it is not a sufficient abstraction for adding `SoapySDR` cleanly. The control surface is still scattered between `TunerController`, `TunerSession`, and gain policy code. | Introduce a backend interface that owns connect/disconnect, tuning, gain/AGC capabilities, sample-rate constraints, and capability queries. Keep policy in one layer and transport/device specifics in another. | Add backend contract tests using a fake backend before adding `SoapySDR`. |
| P7 | Medium | `src/xdr_server.cpp:1234-1311` | Callback setter boilerplate is still repetitive and easy to drift as the callback surface expands. This is low runtime risk but recurring maintenance drag while protocol support evolves. | Consolidate callback registration into a small callback bundle or helper so new callbacks are added in one place with consistent locking behavior. | No special runtime test needed beyond existing XDR command tests, but the refactor should be covered by protocol regression tests. |
| P8 | Low | `src/xdr_server.cpp:106-124` | `recvLine()` still returns `true` when `maxLen` is reached without seeing a newline. The command-loop DoS is fixed separately by bounded per-client command buffers, but the helper itself still accepts truncated input as success. | Return failure on overflow/truncation, or explicitly mark truncated lines invalid. Log malformed input in verbose mode. | Add tests for oversized handshake and auth lines specifically through `recvLine()`. |
| P9 | Low | `src/application.cpp:72`, `src/application.cpp:84`, `src/audio_output.cpp:757`, `src/audio_output.cpp:910` | Logging style remains inconsistent (`std::endl` mixed with `"\n"`), and preprocessor style in `audio_output.cpp` is still mixed. This does not change behavior, but it keeps future code review noisier than necessary. | Apply a small consistency pass after functional changes settle. | Style-only verification. |

## Missing Test Coverage

The current test suite is materially stronger than before, but the following gaps still stand out:

1. No ALSA enumeration failure-path test for `AudioOutput::listDevices()` / `listAlsaDevices()`.
2. No dedicated shutdown-flush test for off-thread WAV writing.
3. No decimation-path performance or behavior test for `DspPipeline` staging when `iq_rate` is above `256000`.
4. No scan performance regression test to catch reintroduced per-iteration FFT allocations or excessive fallback retunes.
5. No handshake/auth truncation test that exercises `recvLine()` overflow behavior directly.

## Cross-Cutting Conclusions

### Ring Buffer

The ring-buffer work that was previously called out is already done for the audio path.
The remaining buffer-related work is in DSP staging, not speaker/WAV delivery.

### Liquid-DSP Migration Status

Most of the meaningful DSP migration is already done.
The best remaining work is optimization rather than broad replacement:

- scan FFT reuse
- decimation staging cleanup
- hot-path allocation audits where they still exist

### SoapySDR Readiness

`SoapySDR` should still not be added directly on top of the current controller split.

- The current source selection model is still binary and transport-oriented.
- Capability differences between direct RTL, `rtl_tcp`, and Soapy backends will grow quickly.
- The next clean step is a backend capability interface, then a `SoapySDR` implementation behind it.

## Recommended Delivery Order

### Immediate

1. Reuse scan FFT buffers/plans.
2. Reduce or restructure uncovered-channel fallback retunes.
3. Remove decimation `memmove()` churn from `DspPipeline`.

### Structural Refactor

1. Break up `Application::run()` into smaller runtime components.
2. Introduce a real tuner backend abstraction in preparation for `SoapySDR`.
3. Consolidate XDR callback registration.
4. Tighten `recvLine()` truncation handling.

## Milestone Mapping

### 1.6 Broad SDR Backend Support

- backend capability interface
- `SoapySDR` implementation
- backend contract tests
- source/backend selection cleanup in CLI/config/XDR integration

### 1.7 DSP Optimization Pass

- scan FFT plan/buffer reuse
- scan fallback optimization
- decimation staging optimization
- targeted micro-benchmarks for scan and high-rate IQ modes
- remaining hot-path allocation audit across DSP and protocol send paths
