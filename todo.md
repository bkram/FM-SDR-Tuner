# FM-SDR-Tuner Issues and Improvements

Last verified against current source: 2026-03-01

## Verified Open Issues

### High
- **src/rtl_tcp_client.cpp:137-145**: Initial rtl_tcp header is read but not validated (magic/version/tuner fields are not checked).
- **src/audio_output.cpp:1052-1059**: `AudioOutput::write()` allocates `scaledLeft/scaledRight` on each call (avoidable per-block heap churn on the audio path).
- **src/audio_output.cpp:1084-1086**: WAV writing is performed inline in `AudioOutput::write()` (file I/O in the hot path).
- **src/audio_output.cpp:1088-1179**: Speaker path should use a bounded lock-efficient ring buffer (explicit overflow policy) instead of grow-and-trim vectors.
- **src/rtl_sdr_device.cpp:218-237**: `readIQ()` can block up to ~35 ms while waiting for buffered data.
- **src/xdr_server.cpp:1221-1278**: Callback setter boilerplate is repetitive and easy to desynchronize over time.

### Medium
- **src/application.cpp:1-532**: Runtime orchestration is still concentrated in a single large compilation unit.
- **src/xdr_server.cpp:828-844**: Scan queue is copied each loop iteration before send (extra churn during active scan streaming).
- **src/scan_engine.cpp:165-197**: FFT input/output buffers and FFT plan are recreated inside scan loops; preallocate/reuse to reduce scan CPU overhead.
- **src/fm_demod.cpp:229-273**: `m_demodScratch` growth checks run on each call; reserve and reuse strategy can be tightened for stable high-rate processing.

### Low
- **src/audio_output.cpp:757,910** and **src/application.cpp:72,84**: Mixed newline style (`std::endl` and `"\n"`) remains inconsistent.
- **src/audio_output.cpp:8+**: Extensive `#if defined(...)` style remains inconsistent with preferred `#ifdef` style.

---

## Priority Recommendations

1. **High**: Harden rtl_tcp handshake validation (reject malformed headers early).
2. **High**: Reduce audio-path latency/churn (avoid per-block allocations and move WAV I/O off hot path).
3. **High**: Abstract tuner/audio backends for cleaner multi-backend support.
4. **Medium**: Rework audio output buffering to a bounded ring buffer model.
5. **Medium**: Optimize scan DSP path by reusing FFT buffers/plans and minimizing per-iteration allocations.
6. **Low**: Apply style consistency pass (naming, preprocessor style, newline style).

---

## Future Milestones

1.5 **High**: Audio latency fixes and improvements.
1.6 **High**: Add SoapySDR support for broad SDR backend compatibility.
1.7 **Medium**: DSP optimization pass (scan FFT reuse, demod scratch reuse, hot-path allocation audit).
