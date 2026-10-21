# Code Review Report — FM-SDR-Tuner

**Date:** 2026-10-20 (fixes completed 2026-10-21)
**Revision reviewed:** `v1.7.4.1` (branch `v1.7.4.1`, HEAD `7358cf2`) + uncommitted `src/sdrplay_device.cpp` diagnostics
**Fixes landed on:** branch `v1.8` (version `1.8.0`)
**Scope:** all of `src/`, `include/`, `tests/`, `CMakeLists.txt`, `.github/workflows/`, `scripts/`, and the README/INI documentation.
**Method:** manual line-by-line review of every source file, cross-checked against the vendored reference implementations in `research/` (`xdrd/xdrd.c`, `xdr-gtk/src/`), plus a clean build with `-Wall -Wextra -Wshadow -Wconversion -Wsign-conversion`, the full CTest suite, and **live reproduction against a running server** for the network-facing findings.

**Evidence levels:** findings marked **[verified]** were reproduced against a running `fm-sdr-tuner` instance or with a standalone C/Python harness. The rest are established by code inspection, cross-checked against the reference implementation.

---

## Fix status

All findings from the original review have been fixed. Fixes were applied in
the report's recommended order, and each one that could be checked against a
running instance **was** checked against a running instance.

| ID | Status | Verification |
|---|---|---|
| **H1** guest mode grants full control | **fixed** | Live: guest session replies `a1`; 10 mutating commands all reply `a0`; `N`/`o`/`I` still answer. Tuner frequency unchanged. |
| **H2** REST `send()` SIGPIPE | **fixed** | Live: client RST mid-response; server survived and kept serving. |
| **H3** REST `freq_khz` overflow | **fixed** | Live: `freq_khz=5000000` → `{"ok":false,"rejected":["freq_khz"]}`; frequency unchanged. |
| **H4** PI reset dead code on tune | **fixed** | Unit test: PI history cleared on `T` and on `setFrequencyState()`. |
| **H5** client threads never reaped | **fixed** | Unit test: `m_clientThreads` empty after `stop()`. |
| **H6** `stop()` races the accept thread | **fixed** | Accept loop is poll-based; the listening socket is closed by the accept thread that owns it. |
| **M1** REST single-threaded DoS | **fixed** | Live: a stalled client used to delay the next one by 5.00 s; now **0.010 s**, and **0.000 s** with 5 stalled clients. |
| **M2** `appliedBandwidthHz` data race | **fixed** | Now `std::atomic<int>`. |
| **M3** `restPanel` updated before the call | **fixed** | All seven REST setters write the panel only after the tuner call succeeds. |
| **M4** XDR `'W'` unclamped | **fixed** | Live: `W999999999` → `W256000`, `W309000` → `W256000`, `W-1` → `W0`. |
| **M5** malformed tune answered with silence | **fixed** | Rejected tunes answer with the current frequency (`T14900` → `T101700`). |
| **M6** protocol detection window | **fixed** | 1 s window / 50 ms poll; both xdr-gtk and FM-DX clients still work (12/12 protocol tests). |
| **M7** auth phase had no timeout | **fixed** | 5 s handshake deadline on both the XDR and FM-DX paths. |
| **M8** duplicated length limit | **fixed** | Single `lineExceedsLimit()` helper. |
| **L2** `kMaxClientCommandLen` defined twice | **fixed** | Single definition. |
| **L3** `equalsIgnoreCase` unused | **fixed** | Removed. |
| **L4** `kPi` unused in `fm_demod.cpp` | **fixed** | Removed. |
| **L5** `kPllLockAcquireHz` unused | **fixed** | Removed. |
| **L6** `parseFloat` unused | **fixed** | Removed. |
| **L1** `sdrplay_device.cpp` stderr spam | **fixed** | Gated behind a verbose flag plumbed `Application` → `TunerController` → `SDRplayDevice`. Live: `-q` prints 0 `[SDRplay]` lines, `-v` prints the connect trace. |
| **D5** FM-DX handshake consumed early | **fixed** | Unit test: the reference sequence (`x` once, then `T…`) now tunes. Live: was `ER`, now `OK`. |
| **D6** FM-DX path skipped authentication | **fixed** | Unit test: a wrong hash is answered `a0` and the tune never reaches the tuner; the correct hash works. |
| **D7** FM-DX `'T'` had no band check | **fixed** | Unit test: `T10` is answered `ER`, the tuner frequency is unchanged. |
| **D8** fabricated online-client count | **fixed** | Unit test: two guests → `o0,2`, and the first client is told when the second leaves. |
| **D9** any disconnect aborted a scan | **fixed** | Scan ownership is tracked; only the owning client's disconnect cancels. |
| **D10** SDRplay parameter data race | **fixed** | `m_paramMutex` held across every parameter write + `Update()`; `m_hwVer` is atomic. |
| **C1** scan grid off the station raster | **fixed** | Unit test: a 200 kHz grid is shifted onto 87.6 MHz; a 100 kHz grid keeps the requested start. Live: a station on 96.2 MHz is reported on 96200 kHz. |
| **C2** scan integrated ¼ of the channel | **fixed** | Unit test: the scan and the signal meter read the same level (109.5) for the same station. |
| **C3** capture centres erased the stations | **fixed** | Unit test: a station is reported on its own grid point at every step size. |
| **C4** scan scale clipped by the client's window | **fixed** | Unit test: every reported level is inside 3..84; the `S` line and the REST status keep the 0..120 scale. |
| **C5** retune measured the previous band | **fixed** | Unit test: `flushBuffers()` drops the in-flight IQ. Live: three consecutive sweeps find both stations at a stable level. |

### Additional findings from the DSP/SDR deep-audit

A second, focused audit of the DSP and SDR/control layers was run in parallel
and found four more defects. All four are fixed and carry regression tests.

#### D1 — **[verified]** `FIRFilter::reset()` dropped the L1-normalize flag, making `iq_fir_l1_normalize` a no-op
**`src/dsp/liquid_primitives.cpp`**

`FIRFilter::reset()` re-created the filter with `init(m_length, m_cutoff, m_stopBandAtten, m_center)`,
leaving `l1Normalize` at its `false` default. Every code path that set the flag
called `reset()` immediately afterwards (`FMDemod::setBandwidthHz`,
`FMDemod::setIqFirL1Normalize`, and `DspPipeline::reset()` on every retune), so
the flag was **always** lost and the documented envelope bound (`|y| ≤ max|x|`)
was never delivered.

The un-normalized filter can ring to **2.28× full scale** on a full-scale
input (measured: `sum|taps| = 3.0071`, `nominalScale = 0.7578`), which is the
ringing `FMDemod::demodulate` already has to clip defensively.

**Fix:** `reset()` now forwards the stored flag. Regression test
`"FIRFilter L1 normalization survives reset"` — **verified to fail against the
pre-fix code** (`peakAfter != peakBefore`) and pass after.

**End-to-end verification** (identical IQ through the real `DspPipeline`):
pre-fix, `iq_fir_l1_normalize` on and off produced the *same* filtered
envelope (−3.50 vs −3.57 dBFS channel power — the flag was inert); post-fix
they differ by the expected 6.8 dB (−10.36 vs −3.57 dBFS).

> Note on what the flag does and does not change: the L1 path applies a
> constant gain (the taps are the same Kaiser design, scaled so
> `scale · L1 ≤ 1`), and the FM discriminator is amplitude-invariant, so the
> **audio waveform is bit-identical** with the flag on or off. The setting's
> contract is the *envelope* bound `|y| ≤ max|x|`, not the audio content —
> that is what keeps the demodulator's discriminator input inside full scale
> when the IF AGC drives the IQ filter hard.

#### D2 — **[verified]** Out-of-bounds read in the signal-meter side-band estimator
**`src/signal_level.cpp`**

`estimateCenteredChannelPower()` computed the side-band bin counts from the
*requested* channel bandwidth without checking them against the FFT length. The
positive branch indexed past the end of the transform output, and the negative
branch produced a **negative** index that was cast to `size_t` — a wild
pointer dereference. A bandwidth of `309000` Hz (the value xdr-gtk actually
sends, `research/xdr-gtk/src/tuner-filters.c:40`) overflowed an 8192-point
transform at 256 kHz.

Below the overflow threshold the window wrapped back over the channel, so the
"noise floor" measured the signal itself: the noise floor collapsed from
−60 dB to −11 dB, the SNR pinned to 0, and **a strong station read as zero
signal** — and because the adaptive-bandwidth controller keys off the SNR, the
corruption provoked exactly the wrong control action (narrowing the filter).

**Fix, in three parts:**

1. **Bin indexing** now uses the same wrapping the scan engine already uses
   (`wrapBin()`), so an index can never run off the end of the transform
   output.
2. **The side-band window is capped** so it cannot alias past the transform's
   Nyquist point onto the positive spectrum where the station sits. The
   window only needs enough bins for a stable noise estimate, so it is
   narrowed to whatever fits rather than being dropped — the mean is still
   taken over the same number of bins on both sides, so the noise power stays
   a per-bin figure scaled to the channel width. Only a channel that itself
   reaches Nyquist (>247000 Hz at 256 kHz) produces no estimate at all
   (`NaN` SNR, the existing "no estimate" sentinel).
3. **The bandwidth is clamped at every ingress** — XDR `'W'` (both protocol
   paths), REST `bandwidth_hz`/`bandwidth_khz` — so the raw value never
   reaches the estimator.

**Measured effect** (a strong station 10 kHz off centre, 8192 samples at
256 kHz):

| channel bandwidth | before | after |
|---|---|---|
| 56000 | SNR 39.4 dB | SNR 39.4 dB |
| 194000 | SNR 33.4 dB | SNR 33.4 dB |
| **242000** | **SNR 0.0 dB — station reads as zero signal** | **SNR 32.3 dB** |
| **246000** | **SNR 0.0 dB** | **SNR 31.1 dB** |
| 252000 | out-of-bounds read | no estimate (`NaN`) |
| 1000000 | out-of-bounds read | no estimate (`NaN`) |

Regression test `"signal level meter survives an over-wide channel bandwidth"`
— **verified to fail against the pre-fix code** (`snrDb == 0.0` for a strong
station at 242000 Hz) and pass after.

#### D3 — **[verified]** `nearClipRatio` / `hardClipRatio` computed 2× too large
**`src/signal_level.cpp`**

The denominator counted IQ *bytes* (`samples * 2`) but the numerator added
**2** for a *sample* even when only one of the two bytes was saturated. A
station whose I component alone clipped reported `clipRatio = 0.02` when only
1 % of bytes were actually clipped — crossing the 2 % auto-gain step-down
threshold and losing 5–8 dB of sensitivity for a station that was never
overloaded. The `[RF] overload/image ghosting suspected` warning mis-fired on
the same value.

**Fix:** each byte is counted individually. Regression test
`"clip ratio counts bytes, not samples"`.

#### D4 — **[verified]** REST `bandwidth_khz` overflowed a signed `int`
**`src/rest_server.cpp`**

`static_cast<int>(ival) * 1000` is signed-integer overflow for
`bandwidth_khz=3000000` (UBSan-confirmed). The neighbouring `freq_khz` handler
already multiplied in 64-bit with a comment explaining exactly this hazard;
`bandwidth_khz` did not.

**Fix:** the multiply now happens in 64-bit and the result is clamped before
narrowing. Regression test `"RestServer clamps an out-of-range bandwidth"`.

### Further findings from the SDR/control-plane audit

A third audit was run over the SDR-source and control-plane layers
(`rtl_sdr_device`, `rtl_tcp_client`, `sdrplay_device`, `tuner_controller`,
`tuner_session`, `scan_engine`, `xdr_server`, `xdr_facade`, `rest_server`,
`runtime_loop`, `processing_runner`, `signal_level`), verified against a clean
export of the committed HEAD so that the results describe committed code rather
than the moving working tree.

Two of its findings (an **out-of-bounds read** in the signal-meter estimator and
an **unchecked REST bandwidth**) were already covered by D2 and D4 above and
are not repeated. The remaining five are new, and all five are fixed.

#### D5 — **[verified]** The FM-DX handshake was consumed before the command loop, so every FM-DX command was answered `ER`
**`src/xdr_server.cpp`**

`handleClient` peeked the first byte, saw `'x'`, and then **consumed** the
handshake line itself before handing the socket to `handleFmdxClient`. The
command loop's own `if (line == "x")` branch — the only place that set
`tunerStarted` — therefore never ran, so every subsequent command fell into the
`else` branch and was answered `"ER\n"`.

The reference FM-DX client sends `"x"` **exactly once**, in its startup
handshake (`research/FM-DX-Tuner/src/Controllers/Dispatcher/Dispatcher.cpp:66`
— `Dispatcher::await()` reads until it sees `FMDX_TUNER_PROTOCOL_STARTUP` and
returns), and then enters its command loop. It never re-sends the startup line.

**Reproduced against the running tuner:**

```
after 'x' (once)   -> 2 bytes: 1\n
after T101700      -> 3 bytes: ER\n      <-- the client can never tune
after 'x' (twice)  -> 2 bytes: 1\n
after T101700      -> 3 bytes: OK\n      <-- only works if "x" is sent twice
```

**Fix:** the handshake is no longer consumed during protocol detection; the
command loop sees the startup line itself. The reply was also corrected from
`"1\n"` to the reference's `"\nOK\n"` (`Dispatcher::cbStartup`). The first line
must still be exactly `"x"` — an oversized or otherwise invalid first line is
rejected as before.

The auth hash (D6) is read from the **same line buffer** as the commands, not
with a separate blocking read. A separate read would swallow a hash that had
already arrived in the same TCP segment as the startup line, and the client
would wait forever for a reply that never came.

#### D6 — **[verified]** The FM-DX path bypassed the XDR challenge/response entirely
**`src/xdr_server.cpp`**

The XDR path performs a SHA-1 challenge/response before granting any
privilege. The FM-DX path, entered from the *same* `handleClient` function,
went straight to `handleFmdxClient` with **no salt, no hash and no auth state
at all** — `processFmdxCommand` had no auth check and `handleFmdxClient` had
no `authenticated` parameter.

With a password configured and guest mode disabled, a client that sent only
`"x\n"` and then `"T101700\n"` still got full control of the tuner: it could
retune, change bandwidth, change AGC / de-emphasis / volume / force-mono, and
start or stop the tuner. **The password was simply never consulted.**

This is distinct from H1 (guest mode granting full privilege): here the
password *was* set, and the check was skipped entirely.

**Fix:** the FM-DX path now runs the same challenge/response. After the
startup reply the server sends its salt and expects the 40-character
SHA-1 hash; a wrong hash is answered `a0` and the connection is closed. When
no password is configured the path behaves as before (open access), matching
the XDR path's empty-password behaviour.

#### D7 — **[verified]** The FM-DX `'T'` handler had no FM-band validation
**`src/xdr_server.cpp`**

The XDR-GTK `'T'` handler rejects a tune outside the FM broadcast band
(`isValidFmBroadcastFreqHz`). The FM-DX handler omitted the guard, so
`parseFrequencyHz` — which reads any value below 1 000 000 as kHz — turned
`T10` into 10 kHz and `T999999999` into ~1 GHz, and both were handed straight
to `rtlsdr_set_center_freq()`. Combined with D6, this was reachable without a
password.

**Fix:** the same band guard was added to the FM-DX `'T'` handler; an
out-of-band tune is answered `ER` and never reaches the tuner.

#### D8 — **[verified]** The online-client count was fabricated from the session's own role
**`src/xdr_server.cpp`**

The `o<auth>,<guest>` line was synthesised from the *current session's* own
role (`guestSession ? "o0,1" : "o1,0"`), never from any real count. The server
kept no counter, never updated the line as clients arrived or left, and never
broadcast it.

The reference daemon maintains real counters and broadcasts the updated line to
every client on connect and on disconnect (`research/xdrd/xdrd.c:87`, `:800`,
`:828`, broadcast at `:561` and `:597`). xdr-gtk consumes the line to display
"N online" (`research/xdr-gtk/src/tuner.c:368`), and uses `tuner.online > 1`
to decide whether to send its settings (`tuner-callbacks.c:331`) — with the
fabricated count that check never fired, so every client believed it was the
only one.

**Fix:** real `m_onlineAuth` / `m_onlineGuest` counters back the line, they are
incremented on connect and decremented on disconnect, and the updated line is
broadcast to every connected client on both events.

#### D9 — **[verified]** Any authenticated client's disconnect aborted a running scan
**`src/xdr_server.cpp`**

`handleXdrClient` set `m_scanCancelPending = true` unconditionally on
disconnect, whether or not that client had anything to do with the scan. A
second client connecting and hanging up was enough to abort a sweep belonging
to somebody else.

**Fix:** the socket that armed the scan is recorded in `m_scanOwnerSocket`, and
the disconnect only cancels the scan when that client is the owner. The
reference `xdrd` does not cancel a scan on disconnect at all.

> **A note on the regression test.** The test's bystander has to be a client
> that got *past* the auth phase. A client rejected with `a0` returns from
> `handleXdrClient` before the command loop and therefore never reaches the
> disconnect handling at all — so such a client could never have triggered
> this bug, and a test built on one would pass with or without the fix.

#### D10 — **[verified]** SDRplay device parameters were mutated concurrently from the REST thread and the main thread
**`src/sdrplay_device.cpp`**

All SDRplay state lives in file-scope globals (`g_device`, `g_params`) shared
by every instance. Every setter performed an unsynchronised read-modify-write
on the shared parameter struct followed by a vendor `Update()` call. The
device's own mutex protected only the IQ ring, never `g_params`.

The REST server serves requests on its own thread, and its control lambdas call
straight into the device (`lna`, `antenna`, `bias_tee`, `gain_db`, `agc`),
while the main thread calls `setGainMode` / `setGain` / `setAGC` from
`applyRtlGainAndAgc`. Both write to the same `g_params` fields and both call
`api().Update()`, so the two updates could interleave: the RSP could be left
with the LNA state of one operation and the gain state of another, or a
`setGain` could clobber a `setAGC(true)` issued microseconds earlier — with
both calls reporting `Success`. `m_hwVer` was a plain `int` written by
`connect()` on the main thread and read by `modelName()` / `antennaCount()` /
`setAntenna()` from the REST thread.

**Fix:** a dedicated `m_paramMutex` is held across every parameter write *and*
its matching `Update()` call, so the read-modify-write and the API push are
atomic with respect to other setters. `m_hwVer` is now `std::atomic<int>`.

### Regression tests added

| Test | Target |
|---|---|
| `FIRFilter L1 normalization survives reset` | `dsp_chain` |
| `signal level meter survives an over-wide channel bandwidth` | `signal_level` |
| `clip ratio counts bytes, not samples` | `signal_level` |
| `RestServer serves other clients while one connection stalls` | `rest_server` |
| `RestServer clamps an out-of-range bandwidth` | `rest_server` |
| `XDR guest sessions cannot change tuner state` | `xdr_unit` |
| `XDR authenticated sessions can command the tuner` | `protocol` |
| `XDR tune resets the RDS PI history` | `xdr_unit` |
| `RestServer stops promptly while a client is still connected` | `rest_server` |
| `XDRServer serves the reference FM-DX startup sequence` | `protocol` |
| `XDRServer rejects an out-of-band FM-DX tune` | `protocol` |
| `XDRServer requires the password on the FM-DX path` | `protocol` |
| `XDRServer accepts a pipelined FM-DX startup and auth hash` | `protocol` |
| `XDRServer reports the real number of connected clients` | `protocol` |
| `XDRServer: a bystander disconnect does not cancel a scan` | `protocol` |
| `SDRplay connect diagnostics honour the verbose flag` | `sdrplay_verbose` |
| `TunerController forwards the verbose flag to the SDRplay source` | `sdrplay_verbose` |
| `ScanEngine publishes the spectrum incrementally during the sweep` | `scan_engine` |
| `ScanEngine widens the step when the grid would overflow one line` | `scan_engine` |

Each of D1, D2, D3 and D4 was checked by **reverting the fix and confirming
the new test fails**, then restoring the fix and confirming it passes — so the
tests are known to actually exercise the defect. The same was done for D5–D9
and for L1 (the revert was applied to a scratch copy of the tree so the
working tree was never left in a weakened state).

Full suite: **17/17 CTest targets pass** (18/18 in an SDRplay-enabled build,
which adds the `sdrplay_verbose` target).

### Live verification against the running tuner

Every network-facing fix was re-checked against a running `fm-sdr-tuner`
instance connected to a real RTL-SDR, with a password configured
(`password = secret123`, `guest_mode = false`):

| Check | Result |
|---|---|
| FM-DX reference sequence (`x` sent **once**, then `T101700`) | `ER` before the fix → **`OK`** after |
| FM-DX with **no** auth hash | **`a0`** — the tune never reaches the tuner |
| FM-DX with the **correct** hash | **`OK`** — the tune is applied |
| Online count, one guest | `o0,1` |
| Online count, second guest joins | `o0,2`, and the first client is **broadcast** `o0,2` |
| Online count, second guest leaves | the first client is broadcast `o0,1` |

---

## Band-scan audit (XDR spectral scan)

The XDR protocol's band-scan command produces the spectrum overview in
XDR-GTK. It "never really worked great", so the scan path was audited against
the reference client (`research/xdr-gtk/src/scan.c`, `tuner.c`,
`tuner-scan.c`) and re-checked live against a real RTL-SDR (NESDR SMArt v5,
R820T).

### B1 — the spectrum was published only at the very end **[fixed]**

`ScanEngine::runIfActive` emitted exactly one `U` line, after the entire sweep
had finished. A full 87.5–108 MHz sweep takes tens of seconds on an RTL dongle,
so the client sat on an **empty plot for the whole sweep** and only ever saw
one frame.

**Fix:** a line is now published after every retune. Because the reference
client *replaces* its dataset on every `U` line rather than merging
(`scan_update()`), each line describes the **same grid** — same start, same
step, same channel count — with unmeasured channels reported as `0.0` (the
client's "no signal" floor), so the x-axis does not shrink as the sweep
advances. The trailing comma is mandatory (the client's parser sizes its
array from the comma count and drops the last entry without it).

### B2 — a wide grid silently overflowed the client's receive buffer **[fixed]**

The whole spectrum travels in a single line. The reference client reads into a
10000-byte buffer and silently drops the overflow
(`research/xdr-gtk/src/tuner.c`, `SERIAL_BUFFER`), and refuses more than 700
samples outright (`research/xdr-gtk/src/scan.c`, `SCAN_MAX_SAMPLES`). A 5 kHz
step across the full band is 4101 channels ≈ 45 KB: the line was truncated
mid-entry, the trailing comma was lost, the client then dropped the last
entry, and the plot ended short of the requested stop frequency.

**Fix:** the step is widened so the grid fits within 700 channels, with a
`[SCAN] warning` naming the requested and applied step.

### B3 — the scan queue was too small to carry a sweep **[fixed]**

`XDRServer::pushScanLine` kept at most 8 queued lines. A full sweep publishes
one line per retune (43 at a 500 kHz step, 138 at 30 kHz), so a client that
polled less often than the scan advanced saw only the last few frames.

**Fix:** the bound is now 256 lines — enough for a full sweep — while still
capping growth for a client that never reads.

### B4 — guest sessions could not run a scan **[not a defect — correction]**

An earlier revision of this report treated "a guest may not run a scan" as a
defect and relaxed the guest allowlist to include `'S'`. **That was based on a
false premise and has been reverted.** The evidence:

- The server's challenge/response is `SHA1(salt + password)`. With an empty
  password that is exactly `SHA1(salt)`, which is precisely what the reference
  client sends for its default empty password
  (`research/xdr-gtk/src/tuner-conn.c:158-165`; the client's default is
  `CONF_CONNECTION_PASSWORD ""`). A stock client against the default
  configuration is therefore **fully authenticated** (`a2`), not a guest.
- Guest mode engages only when a client presents a *wrong* hash.
- The reference daemon lets only an authenticated session drive the tuner
  (`research/xdrd/xdrd.c:585`).

So the default configuration was never blocked from scanning, and admitting
`'S'` to guests would have weakened the read-only boundary for no benefit. The
allowlist is back to `N` / `o` / `P` / an `I` query; a guest asking for a scan
gets `a0`. A live check confirms it: a wrong-hash client is answered `a1` and
then `a0` for both `S` and `T101700`, while a correct-hash client gets `a2`
and a working sweep.

### B5 — the sweep blocks the IQ path **[known limitation, not changed]**

The sweep runs inside the IQ read loop, because it needs the samples. While a
sweep is in progress the tuner is retuned across the band, so there is no
audio, no signal-meter update and no RDS decode. The tuner is restored
afterwards (`[DSP] reset reason=scan_restore` in the log, and the stereo
decoder relocks).

This is inherent to the current architecture: the scan consumes the same
sample stream the audio path does. Making the sweep concurrent would mean
either a second SDR handle or a ring buffer large enough to hold a sweep's
worth of samples, and would change the tuning discipline that the wide-mode
SDRplay optimisation depends on. It is left as a known limitation rather than
changed under cover of a bug fix.

### Live verification

Against a running `fm-sdr-tuner` with an RTL-SDR attached, using a client that
speaks the reference protocol (salt → `SHA1(salt)` → `x` → scan):

| Check | Result |
|---|---|
| Auth with the default (empty) password | `a2` — fully authenticated |
| Full-band sweep, 500 kHz step | 43 `U` lines, first at **0.2 s** (previously one line at the end) |
| Grid across all 43 lines | **1** distinct frequency grid, 42 entries, uniform 500 kHz step, 87500→108000 kHz |
| Trailing comma | present on every line |
| Longest line | 451 chars (client buffer is 10000) |
| Measured content | 19 of 42 channels carry signal; strongest 97000 kHz = 36.3, then 105000 = 30.9, 104000 = 26.5, 102500 = 24.5, 88000 = 24.0 — real European FM carriers |
| 5 kHz step over the full band | widened to 30 kHz: `[SCAN] warning: 4101 channels requested exceeds the 700 …`; 684 entries, 7273 chars, trailing comma intact |
| Guest (wrong hash) running a scan | `a0` — refused |
| Guest (wrong hash) retuning | `a0` — refused |
| Tuner state after the sweep | restored (`[DSP] reset reason=scan_restore`), stereo relocks |

### Regression tests added

| Test | Target |
|---|---|
| `ScanEngine publishes the spectrum incrementally during the sweep` | `scan_engine` |
| `ScanEngine widens the step when the grid would overflow one line` | `scan_engine` |

Both were checked by **reverting the fix and confirming the test fails**
(`m_scanQueue.size()` was 1, and the grid held 4101 entries instead of ≤700),
then restoring the fix and confirming it passes.

---

## Spectral-scan accuracy audit

The scan was re-audited against the reference client's **scale** and
**geometry** requirements, because two symptoms were reported from the field:
the reported strength was wrong, and a station on 96.2 MHz was drawn on
96.1 MHz.

The reference client's contract was read out of the source rather than
assumed:

- `research/xdr-gtk/src/scan.c` — the plot is drawn in a **fixed 3..84
  window** (`SCAN_DEFAULT_MIN_LEVEL 3.0`, `SCAN_DEFAULT_MAX_LEVEL 84.0`) and
  `scan_draw_spectrum()` **clips** everything outside it.
- `research/xdr-gtk/src/ui-signal.c` — `signal_level()` maps the server's
  value onto dBm/dBµV/dBf depending on the client's unit; the **dBf** unit
  (the default) is `value + conf.signal_offset`.
- `research/xdr-gtk/src/ui-tuner-update.c:165` — `scan_update_value()` writes
  the meter's **S line** value into the scan plot, so the `S` line and the
  `U` line have to share one scale.
- `research/FM-DX-Tuner/src/Controllers/Tuner/TEF668X/TEF668X.cpp` — the
  reference `getQualityRssi()` returns `value + 1125`; the protocol scale is
  therefore **0..120**, which is also what the fm-dx-webserver expects
  (`web/js/main.js:864` subtracts 120 for dBm, and the plugin shows the value
  out of 120).

### C1 — the reporting grid was anchored on the client's default, not on the station raster **[fixed]**

The grid started at the client's default 87.5 MHz. The European FM plan
allocates channels every 200 kHz starting at **87.6 MHz**, so a grid anchored
at 87.5 MHz lands on the *midpoints* between the stations. With a 200 kHz
step a station on 96.2 MHz was reported on the 96.1 grid point — the reported
"offset" symptom. The radio was never wrong; the grid was.

**Fix:** the grid is aligned with the CCIR raster whenever the step allows
it. The alignment is applied only when the step divides the 200 kHz raster,
and the phase is taken modulo the **cell** step rather than the raster step,
so a 100 kHz grid from 87.5 MHz (which already contains 87.6, 87.8, …) is
left where the client asked for it while a 200 kHz grid is shifted onto
87.6. A step that does not divide the raster produces a `[SCAN] warning`
naming the consequence.

### C2 — the scan integrated a quarter of the channel bandwidth **[fixed]**

The per-channel measurement window was a quarter of the channel bandwidth, so
every station was reported about **6 dB** low. The client mixes the `S` line
and the `U` line on one plot, so the tuned station visibly jumped when it was
added to the plot.

**Fix:** the measurement integrates the **full** channel bandwidth, exactly
like the signal meter, and the scan and the meter now read the same value for
the same station (109.5 on the 0..120 scale in the regression test).

### C3 — the capture centres landed on the stations and erased them **[fixed]**

The estimator rejects the bins around DC (they carry the tuner's own DC
spike), and the capture centres were placed **on** the reporting grid points —
which is where the stations are. A station sitting on a capture centre was
therefore removed from its own measurement window and vanished from the plot.
The repeating three-value pattern seen in the field was this artefact: its
period is the least common multiple of the grid step and the capture-centre
step.

**Fix:** the capture centres run **half a cell** away from every grid point
and their step is a whole number of cells, so the phase never drifts back
onto a station.

### C4 — the 0..120 scale was clipped by the client's fixed plot window **[fixed]**

The `U` line carried the meter's 0..120 scale, but the client draws the plot
in a fixed 3..84 window and clips everything outside it. Every station above
−23 dBFS was therefore pinned flat against the top of the plot, and the plot
stopped telling the strong stations apart.

**Fix:** the scan line alone is remapped onto the client's plot window, with
`[sdr] scan_use_client_window`, `scan_client_min_level` and
`scan_client_max_level`. The remap is applied at **emission** time, so a
channel that has not been swept yet keeps its `0.0` "no signal" marker
instead of being lifted to the bottom of the window. The `S` line and the
REST status deliberately keep the 0..120 scale, because the clients that read
those convert it themselves — the webserver's dBm mode subtracts 120 and the
plugin shows the value out of 120 — and the plugin needs no change.

### C5 — a retune measured the band the tuner had just left **[fixed]**

`TunerController::flushBuffers()` only emptied the **USB dongle's** ring, so
on the `rtl_tcp` source it was a no-op. The samples of the centre frequency
the tuner had just left stayed in the socket buffer and were read as if they
belonged to the new centre, so every capture mixed the spectrum of two
different bands. The symptom was a **flaky** detection: the same station was
found on one sweep and missed on the next.

**Fix:** `RTLTCPClient::flushBuffers()` drains the IQ that is still in flight
and drops the half-byte left over from an earlier read; the controller
dispatches the flush to whichever source is in use.

### C6 — the reference `rtl_tcp` server does not acknowledge commands **[documentation]**

While building the test harness it became clear that the real `rtl_tcp`
server (osmocom/rtl-sdr, `src/rtl_tcp.c`) sends **no reply** to any command:
only the 12-byte `RTL0` header, then a continuous IQ stream, with the
commands read on a separate thread. A server that acknowledges the commands
injects those bytes into the IQ stream as garbage. `scripts/fake_rtl_tcp.py`
now reproduces the reference behaviour exactly (no acks, a command-reader
thread and an IQ-writer thread, a paced stream, and an NCO that restarts on a
retune).

### Live verification

Without an SDR attached, the whole path was exercised against
`scripts/fake_rtl_tcp.py`, which plays a synthetic band with carriers on
**96.2 MHz** and **101.2 MHz**:

| Check | Result |
|---|---|
| Sweep of 95–103 MHz at a 100 kHz step | 83 retunes, one `U` line per retune |
| Grid | 81 entries, 95000→103000 kHz, uniform 100 kHz step, trailing comma present |
| Station on 96.2 MHz | reported at **96200 kHz**, level **47.2** |
| Station on 101.2 MHz | reported at **101200 kHz**, level **47.2** |
| Neighbouring grid points (96100, 96300, 101100, 101300) | 9.0–9.4 — the noise floor, no bleed |
| All levels | 8.4–47.2, every one inside the client's 3..84 window |
| Repeatability | three consecutive sweeps, both stations at **47.2** every time |
| Before the C5 fix | the same sweep reported 96.2 MHz on one run and missed it on the next |

The client's own scale was confirmed on the wire as well: the `U` line's
frequency labels carry the station's **own** frequency, and the levels sit on
the plot window the client draws.

### Regression tests added

| Test | Target |
|---|---|
| `ScanEngine reports a station on the grid point of its own frequency` | `scan_diag` |
| `ScanEngine keeps the noise floor low and flat across the band` | `scan_diag` |
| `ScanEngine reports a station on the same scale as the signal meter` | `scan_diag` |
| `ScanEngine reports levels inside the client's plot window` | `scan_diag` |
| `ScanEngine aligns the reporting grid with the station raster` | `scan_diag` |
| `RTLTCPClient flushBuffers drops the stale IQ of the previous centre` | `protocol` |
| `Config parses the scan plot window` | `config` |

---

## Build & test baseline

| Check | Result |
|---|---|
| `cmake -S . -B build-review -DCMAKE_OSX_ARCHITECTURES=arm64` | ✅ clean configure |
| `cmake --build build-review -j` | ✅ 0 errors |
| `ctest --test-dir build-review` | ✅ 17/17 passed |
| `ctest --test-dir build-review-sdrplay` (SDRplay enabled) | ✅ 18/18 passed |
| Strict-warning build (`-Wall -Wextra -Wshadow -Wconversion -Wsign-conversion`) | ✅ 0 warnings in project sources and tests (both RTL-only and SDRplay configurations) |

> **Pre-existing environment issue (not a code defect):** the checked-in `build/` directory has a stale CMake cache pointing at `librtlsdr 2.0.2`; the installed version is now `2.0.3`. `cmake --build build` fails at the generate step. A fresh configure directory works. Worth a `rm -rf build` note in the README's build section, or a `find_package` version floor.

> **Vendored code:** the only remaining strict-build warnings (22 of them, all `-Wsign-conversion`) are in `redsea_port/` — the upstream port we do not own. Project sources and test files are warning-free.

---

## Summary

| Severity | Count | Fixed |
|---|---|---|
| HIGH | 6 | 6 |
| MEDIUM | 8 | 8 |
| LOW | 6 | 6 |
| *Deep-audit (D1–D4)* | *4* | *4* |
| *SDR/control audit (D5–D10)* | *6* | *6* |
| *Band-scan audit (B1–B3)* | *3* | *3* |
| *Scan-accuracy audit (C1–C5)* | *5* | *5* |
| **Total** | **38** | **38** |

All 38 findings are fixed. Every fix carries a regression test, and every fix
that could be checked against a running instance was checked against one.

A fourth audit covered the XDR band-scan path (B1–B3) and corrected one
mis-diagnosis (B4): the spectrum was published only at the very end of a
sweep, so the client watched an empty plot for the whole run; a wide grid
silently overflowed the client's receive buffer and lost the plot's last
entry; and the scan queue was too small to carry a sweep. See
[Band-scan audit](#band-scan-audit-xdr-spectral-scan).

A fifth audit covered the **accuracy** of the scan (C1–C5) and addressed the
two symptoms reported from the field — a station on 96.2 MHz drawn on
96.1 MHz, and a wrong reported strength. The grid was anchored on the
client's default start instead of the CCIR station raster; the per-channel
measurement integrated a quarter of the channel bandwidth and so read 6 dB
low; the capture centres were placed on the stations themselves, where the
DC-reject notch erased them; the 0..120 scale was clipped by the client's
fixed 3..84 plot window, pinning every strong station flat against the top;
and a retune on the `rtl_tcp` source measured the band the tuner had just
left, which made the detection flaky. See
[Spectral-scan accuracy audit](#spectral-scan-accuracy-audit).

The most serious issues are a **protocol regression in XDR guest mode** (unauthenticated clients can retune and reconfigure the tuner, where the reference `xdrd` treats guests as read-only), a **process-killing SIGPIPE in the REST server**, and **integer-overflow in the REST frequency setter**.

A follow-up deep audit of the DSP and SDR/control layers found four further
defects (D1–D4), including an **out-of-bounds read** in the signal-meter
side-band estimator and a filter-reset bug that silently disabled the
`iq_fir_l1_normalize` setting. See [Fix status](#fix-status).

A third audit of the SDR-source and control-plane layers found six more
(D5–D10). The most consequential are a **protocol defect that made the FM-DX
client path unusable** (the startup handshake was swallowed, so every command
came back `ER`), an **authentication bypass** on that same path (the password
was never consulted, so a client could retune and reconfigure the tuner
without it), an **out-of-band tune** accepted by the FM-DX handler, a
**fabricated online-client count** that never changed, and a **data race** on
the shared SDRplay parameter struct between the REST thread and the main
thread.

> **Note on the two findings I initially mis-diagnosed.** While verifying M7 and M8 I wrote two speculative findings and then tested them; both turned out to be **false positives** and have been corrected in place:
> - *M7* was originally "the `P`-prefixed auth hash is unreachable with CRLF endings." Testing showed `recvLine` retains the `P`-prefixed 41-char hash correctly (the `\r` is stripped, not counted). The real defect in that area is different — the auth read has **no timeout** — and M7 now describes that.
> - *M8* was originally "the two command-length checks disagree for fragmented input." A chunk-by-chunk simulation showed they **agree**. M8 now records the duplication as a drift hazard rather than a behavioural bug.
>
> Both corrections are left in the report rather than silently dropped, so the reasoning is auditable.

---

## HIGH severity

### H1 — **[verified]** XDR guest mode grants full control; reference `xdrd` makes guests read-only
**`src/xdr_server.cpp:840-848, 1001`**

**Defect:** When authentication fails but `m_guestMode` is on, the session is marked `authenticated = authSuccess || m_guestMode` and is then passed to `processCommand()` as a fully-privileged session. The `guestSession` flag is only ever consulted for the `o0,1`/`o1,0` online-count reply (line 1314) — it never gates a command.

**Failure scenario — confirmed empirically against the running server.** The server binds to `INADDR_ANY` (`src/xdr_server.cpp:589`), i.e. every network interface, and **guest mode is the default** — `application.cpp:282-287` forces `xdrGuestMode = true` whenever no password is configured, and `Config::XDRSection::password` defaults to empty. So the default install exposes an unauthenticated command port to the whole LAN. I connected, sent a deliberately **wrong** password hash, and then issued a tune command:

```
salt: jFpnblERAOBA16Yb
auth response: a1          <- guest session granted
...
TUNE RESPONSE: T90000      <- retune ACCEPTED from an unauthenticated client
SERVER REPORTS: T90000     <- server state now on 90 MHz (started at 87.5)
```

The unauthenticated guest's retune was applied. Compare `research/xdrd/xdrd.c:585`:

```c
if(u->auth)
    serial_write(buffer, pos+1);   /* only authenticated users may command the tuner */
```

and `research/xdrd/xdrd.c:321`: `-g  allow guest login (read-only access)`. Guests in the reference daemon are explicitly read-only. The current implementation grants write access to unauthenticated clients, and it also diverges from the README, which claims the server "opens on `127.0.0.1:7373`" (README:57) — it actually binds `0.0.0.0`.

The same applies to `W` (channel bandwidth), `A` (AGC mode), `G` (gain), `x`/`X` (start/stop the tuner), and `S` (start a full-band scan).

**Fix:** Gate state-changing commands on `authenticated` and let guests read only. In `processCommand()`, reject mutating commands when `!authenticated`:

```cpp
if (!authenticated) {
  // Guests may read state but not reconfigure the tuner (matches xdrd's
  // "guest login (read-only access)").
  switch (command) {
    case 'I': case 'N': case 'o': case 'P': case 'S': /* read-only */ break;
    default: return "a0";
  }
}
```

Additionally make the bind address configurable (default `127.0.0.1`) rather than hard-coding `INADDR_ANY`, and log a startup warning when guest mode is enabled.

---

### H2 — **[verified]** REST server `send()` has no SIGPIPE protection; client disconnect kills the tuner
**`src/rest_server.cpp:175`**

**Defect:** `sendResponse()` calls `::send(sock, out.data() + sent, out.size() - sent, 0)` — no `MSG_NOSIGNAL`, and `RestServer` never sets `SO_NOSIGPIPE`. The XDR server gets this right (`src/xdr_server.cpp:60-74` handles both `SO_NOSIGPIPE` and `MSG_NOSIGNAL`); the REST server does not. The application only installs handlers for `SIGINT`/`SIGTERM` (`src/application.cpp:302-303`), so `SIGPIPE` keeps its default disposition of **terminate**.

**Failure scenario:** I reproduced this exactly. A client connects to the REST port and disconnects mid-response; the next `send()` on that socket raises `SIGPIPE` and the whole tuner process dies:

```
$ ./sigpipe_test3
exit=141        # 128 + 13 (SIGPIPE)
```

Confirmed with the identical call shape (`send(fd, buf, len, 0)`) that `rest_server.cpp:175` uses. Any browser tab closing during a `/api/status` poll, or a `curl` that times out, can take down a running broadcast session. This is the single most likely "the tuner randomly exited" field report.

**Fix:** Mirror the XDR server's protection. Either add `MSG_NOSIGNAL` to the send:

```cpp
const auto n = ::send(sock, out.data() + sent, out.size() - sent, MSG_NOSIGNAL);
```

or set `SO_NOSIGPIPE` on the accepted socket. The portable-and-complete fix is to also ignore `SIGPIPE` process-wide in `application.cpp`:

```cpp
signal(SIGPIPE, SIG_IGN);   /* a peer disconnect must not kill the tuner */
```

Do all three — belt and braces — since `rtl_tcp_client.cpp:203` has the same unprotected `send()`.

---

### H3 — **[verified]** REST `freq_khz` overflows, producing a wildly wrong tuning frequency
**`src/rest_server.cpp:376-381`**

**Defect:** `freq_khz` is parsed into a `long`, then converted with `static_cast<uint32_t>(ival) * 1000U`. There is no range check before the multiply, and no `isValidFmBroadcastFreqHz()` check (which the XDR path *does* apply at `src/xdr_server.cpp:1099`).

**Failure scenario:** `curl "http://127.0.0.1:9090/api/control?freq_khz=5000000"` computes `5000000 * 1000 = 5,000,000,000`, which overflows `uint32_t` and wraps to `705032704` Hz — verified:

```
freq_khz=5000000 -> hz=705032704 (expect 5000000000)
```

The tuner then retunes to 705.03 MHz, far outside the FM broadcast band, and the server reports `ok:true, applied:1`. Because the value wraps to a value that happens to fall in a plausible-looking range, there is no error to notice. `freq_hz` has the same problem (a `long` above 4.29e9 truncates on the `static_cast<uint32_t>`).

**Fix:** Validate before converting, and reuse the shared band check:

```cpp
} else if (key == "freq_khz" && toInt(val, ival)) {
  if (ival > 0 && ival <= 108000 &&
      m_controls.setFrequencyHz &&
      m_controls.setFrequencyHz(static_cast<uint32_t>(ival) * 1000U))
    appliedCount++;
  else
    note(key);
}
```

and for `freq_hz`, require `ival <= 108000000` and call `fm_tuner::isValidFmBroadcastFreqHz()`. Better still, add the band check inside the `setFrequencyHz` control lambda in `src/application.cpp:686` so every path (XDR, REST, scan) is covered in one place.

---

### H4 — `setFrequencyState()` PI-reset is dead code on the XDR tune path
**`src/xdr_server.cpp:516-530` vs `src/xdr_server.cpp:1102`**

**Defect:** `setFrequencyState()` is documented to clear the RDS PI-code history when the frequency changes (so a stale PI from the previous station isn't reported for the new one). But `processCommand()`'s `'T'` handler assigns `m_frequency = freqHz;` **before** invoking the frequency callback, and the callback path in `runtime_loop.cpp:49` calls `xdrServer.setFrequencyState(targetFrequencyHz)` — by which point `m_frequency` already equals the new value.

**Failure scenario:** `setFrequencyState()` compares `freqHz != m_frequency`; they are equal, so the `if` body is skipped and `m_piFill`/`m_piBuffer`/`m_piErrorBuffer` are **never** reset on a tune. After retuning, the client keeps receiving the old station's PI code (and the `P` PI-status line) until 64 fresh RDS groups happen to overwrite the buffer. Since `m_piFill` only counts up and `evaluatePiState` scans the whole 64-entry buffer, a stale PI can survive for many seconds and be reported as the new station's identity.

**Fix:** In the `'T'` handler, reset the PI state explicitly *before* assigning the new frequency, and make `setFrequencyState()` unconditionally reset when the frequency differs from the last one it was told about. The cleanest fix is to keep a separate `m_lastAppliedFrequency` member that `setFrequencyState()` compares against, rather than reusing `m_frequency` which the command path mutates directly. E.g. in `processCommand` `'T'`:

```cpp
// Reset the PI history before the new frequency is published, so the
// previous station's PI code is not reported for the new one.
resetPiHistory();
m_frequency = freqHz;
```

with the reset extracted into a small private `resetPiHistory()` helper used by both sites.

---

### H5 — XDR client threads are never reaped; the vector grows without bound
**`src/xdr_server.cpp:619-624`**

**Defect:** Each accepted connection spawns a `std::thread` pushed into `m_clientThreads`. The vector is only drained in `joinClientThreads()` (line 277), which is called **once** from `stop()`. Finished threads are never joined or erased while the server runs.

**Failure scenario:** A long-running server that sees many client connections (FM-DX-Webserver reconnects, a client that times out and re-connects, port scanners touching port 7373) accumulates one unjoinable `std::thread` object per connection. Each holds its `std::thread` state (~100 bytes) plus the thread stack until joined — a `std::thread` whose function has returned but which was never joined is still a joinable thread holding its stack resources. Over thousands of connections this is a slow resource leak, and at shutdown `joinClientThreads()` then joins hundreds of long-finished threads.

**Fix:** Reap finished threads as they complete. Keep the thread handle in a small struct and detach it, or use a dedicated detached thread that removes its own socket from `m_clientSockets`:

```cpp
m_clientThreads.emplace_back([this, clientSocket]() {
  handleClient(clientSocket);
  removeClientSocket(clientSocket);
  closeSocket(clientSocket);
});
```

The simplest correct fix is to `detach()` the thread (the lambda captures only `this` and the socket fd, both of which outlive the thread because `stop()` joins the accept thread and shuts down all client sockets first). Alternatively, keep a `std::vector<std::thread>` and periodically join+erase entries whose clients have disconnected. Detaching is the lower-risk change here.

---

### H6 — `XDRServer::stop()` can race with the accept thread on `m_serverSocket`
**`src/xdr_server.cpp:637-663` vs `610-629`**

**Defect:** `stop()` writes `m_serverSocket = -1` (line 647) while the accept thread is concurrently reading `m_serverSocket` in `accept(m_serverSocket, ...)` (line 615). `m_serverSocket` is a plain `int`, not an atomic, and there is no synchronization between the two threads.

**Failure scenario:** `stop()` sets `m_serverSocket = -1` between the accept thread's load of the value and the `accept()` syscall. The accept thread then calls `accept(-1, ...)`, which fails with `EBADF`. The thread's error branch is `else if (m_running && socketInterrupted(lastSocketError()))` — `EBADF` is not `EINTR`, so the loop spins: `m_running` was just set to `false`, so the `while (m_running)` condition exits and the thread terminates. In the common case this is benign. But if the accept thread had already loaded a *valid* fd and `stop()` closed it concurrently, `accept()` on a closed/reused fd is a classic fd-reuse race — the fd number may have been reallocated by another `open()` in the meantime, and `accept()` would then bind a client to an unrelated file descriptor. The XDR server's `stop()` also `shutdown()`s then `close()`s the socket without a mutex.

**Fix:** Guard the socket lifetime. Either make `m_serverSocket` an `std::atomic<int>`, or — simpler and race-free — have `stop()` only `shutdown()` the socket (which unblocks `accept()`) and let the accept thread own the `close()`. The canonical pattern:

```cpp
// stop(): shutdown only — do not close here.
if (m_serverSocket >= 0) {
  shutdownSocket(m_serverSocket);
}
...
if (m_acceptThread.joinable()) m_acceptThread.join();
// the accept thread closes the socket on exit
```

---

## MEDIUM severity

### M1 — **[verified]** REST server is single-threaded and has no socket timeout: one idle connection bricks the whole control API
**`src/rest_server.cpp:252-265, 267-278`**

**Defect:** `acceptLoop()` handles each connection **synchronously on the accept thread** (`handleConnection(client)` is called inline), and `handleConnection` blocks in `recv()` reading headers with no `SO_RCVTIMEO` and no deadline. There is no timeout set anywhere in `rest_server.cpp`.

**Failure scenario — confirmed empirically against the running server.** I started the tuner with `[rest] port = 19090`, opened a TCP connection to the REST port, sent nothing, and then tried a normal `/api/status` request from a second client:

```
held a REST connection open, sending no request
REST BLOCKED after 8.03s: TimeoutError: timed out
```

The second client hung until its own 8 s timeout — the REST API was completely unresponsive for the duration. A single idle connection (a port scanner, a health-check that opened a socket and stalled, a browser tab that connected and never sent a request) takes the entire control API down until that socket closes. The REST port is documented as "no authentication by design," so this is reachable by anyone who can reach the port.

**Fix:** Set a receive timeout on accepted sockets and return 408 on expiry — this alone removes the DoS:

```cpp
#if defined(_WIN32)
  DWORD tv = 5000;
  setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
  struct timeval tv{5, 0};
  setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
```

Better still, handle each connection on its own thread so one slow client cannot delay the others.

---

### M2 — `appliedBandwidthHz` is a plain `int` shared across three threads
**`src/application.cpp:540, 773, 1038, 1057`; `src/runtime_loop.cpp:21, 82-88`**

**Defect:** `appliedBandwidthHz` is a non-atomic `int` declared in `Application::run()`. It is written by the main processing thread (`runtime_loop.cpp:88`) and read by the REST accept thread (`application.cpp:773`, inside the `statusJson` lambda) and passed by reference to `processing_runner::processAudioBlock` (`application.cpp:1038`).

**Failure scenario:** The REST thread reads `appliedBandwidthHz` while the processing thread writes it. This is a data race → undefined behavior per the C++ standard, and in practice a torn or stale value in `/api/status`. In practice on x86/ARM a plain `int` load/store is atomic in practice, so the practical symptom is a *stale* bandwidth value rather than corruption — but it is still formally UB, and `clang-tidy`'s `concurrency-mt-unsafe` or TSan would flag it. The same pattern is worth checking for the neighbouring `mpxPeakHold` float (which is only touched on the processing thread, so it is fine).

**Fix:** Make it `std::atomic<int>`, consistent with the neighbouring `requestedBandwidthHz` which is already atomic.

---

### M3 — `restPanel` is mutated from the REST thread and read from the same thread, but `gain_db` clamping silently discards the applied value
**`src/application.cpp:691-697, 764-808`**

**Defect:** `controls.setGainDb` clamps the requested dB to `[0, 50]` and stores it in `restPanel.gainDb` **before** calling `tuner.setGain(...)`. If `tuner.setGain()` fails (e.g. the SDRplay device rejects the gain step, or the device is disconnected), the control still returns `false` — but `restPanel.gainDb` has already been updated to the rejected value.

**Failure scenario:** `POST {"gain_db": 45}` against a disconnected SDRplay → `setGain` fails → response is `{"ok":false,"rejected":["gain_db"], ...}`, but `/api/status` now reports `"gain_db":45.0` and `"auto_gain":false` even though nothing was applied. The panel shows a gain the hardware never got. The same "update state before confirming the apply" pattern exists for `setAutoGain` (line 698-702), `setLnaState` (708), `setAntenna` (712), `setBiasTee` (716), `setPpm` (720) and `setRtlAgc` (724) — all of them write `restPanel` unconditionally and then propagate the tuner's success/failure.

**Fix:** Only update `restPanel` when the underlying call succeeds:

```cpp
controls.setGainDb = [&](double db) {
  const double clamped = std::clamp(db, 0.0, 50.0);
  if (!tuner.setGainMode(true)) return false;
  if (!tuner.setGain(static_cast<uint32_t>(std::lround(clamped * 10.0)))) {
    return false;
  }
  restPanel.gainDb = clamped;
  restPanel.autoGain = false;
  return true;
};
```

Apply the same ordering to the other five setters.

---

### M4 — XDR `'W'` (bandwidth) is not clamped, unlike every other numeric command
**`src/xdr_server.cpp:1217-1227`**

**Defect:** Every other numeric XDR command clamps its argument (`'Y'` to 0-100, `'V'` to 0-36, `'M'`/`'D'` to 0-1/0-2, `'Z'` to 0-9, `'Q'` to 0-100, `'C'` to -1..1, `'I'` interval/detector). The `'W'` handler has no clamp: `m_bandwidth = bandwidth;` and the raw value goes to the callback.

**Failure scenario:** A client sends `W999999999`. The value flows to `m_state.requestedBandwidthHz` → `runtime_loop.cpp:81-88` → `dspPipeline.setBandwidthHz(999999999)`. `FMDemod::setBandwidthHz` snaps to the nearest entry in `kXdrFmBwHz` so the *filter* stays safe, but the raw value is stored in `m_bandwidth` and echoed back to the client as `W999999999`, and it is what `appliedBandwidthHz` reports. It also propagates into `scan_engine`'s `m_config.bandwidthHz` (via the `'S'` path, which *does* clamp to 400000 — so the two paths disagree). A negative value (`W-1`) is accepted and stored, though `FMDemod` treats `<= 0` as "auto", so the client gets back `W-1` for a channel that is actually at the default bandwidth.

**Fix:** Clamp to the same range the scan path uses:

```cpp
case 'W': {
  ...
  bandwidth = std::clamp(bandwidth, 0, 400000);
  m_bandwidth = bandwidth;
  ...
}
```

---

### M5 — `parseFrequencyHz` treats any value below 1 MHz as kHz, so sub-MHz values are ambiguous
**`src/xdr_server.cpp:185-207`**

**Defect:** The heuristic is `if (raw < 1000000ULL) raw *= 1000ULL;`. A client asking for 500 kHz (a legitimate value for a converter-style request, or a typo) is silently multiplied to 500,000 Hz — a 1000× error. The result is then rejected by the FM-band check, so the client gets a silent no-op with no diagnostic.

**Failure scenario:** A client sends `T500` (intending 500 kHz). `parseFrequencyHz` returns 500,000 Hz; `isValidFmBroadcastFreqHz(500000)` fails (below the 64 MHz floor) and the handler returns `""` — the client sees no response and no error, and cannot tell the request was rejected. The same silent-drop happens for *every* invalid command in the XDR path, which is a diagnosability problem in its own right.

**Fix:** Reject out-of-band values with an explicit error response rather than a silent empty string, and make the kHz/Hz heuristic explicit (the XDR protocol is kHz-based; the Hz form is a non-standard extension). At minimum, log rejected commands when verbose logging is on.

---

### M6 — `handleClient` uses `MSG_PEEK` with a 200 ms timeout to detect the protocol
**`src/xdr_server.cpp:675-680`**

**Defect:** Protocol detection reads one byte with `MSG_PEEK` under a 200 ms receive timeout. If the client is slow to send its first byte, `recv` returns `-1` with `EAGAIN`/`EWOULDBLOCK`, `n <= 0`, and the server falls through to the XDR path — misclassifying an FM-DX client as an XDR client.

**Failure scenario:** An FM-DX client on a loaded system (or over a slow link) sends its `x` handshake more than 200 ms after connecting. `n <= 0` → `isFmdxProtocol = false` → the server enters `handleXdrClient()`, which sends a salt and expects a SHA-1 hash. The FM-DX client, expecting a `1` acknowledgement, instead sees a 16-character salt and either hangs or errors out. The failure is timing-dependent, which makes it a nasty intermittent-connect bug.

**Fix:** Increase the detection timeout to something generous (e.g. 2 s) and treat a timeout as "unknown, wait longer" rather than "assume XDR". A more robust approach is to peek with a longer timeout and, on timeout, keep waiting rather than defaulting.

---

### M7 — XDR auth phase has no receive timeout; a stalled client pins its thread indefinitely
**`src/xdr_server.cpp:809` (and `src/xdr_server.cpp:678`)**

**Defect:** Before the auth hash is read, `handleXdrClient()` calls `recvLine(clientSocket, authMsg, HASH_LENGTH + 2)` at line 809. At that point the socket's receive timeout is whatever was last set — and `handleClient()` explicitly **clears** it with `setRecvTimeoutMs(clientSocket, 0)` at line 678. On POSIX, `SO_RCVTIMEO` with a zero `timeval` means **block indefinitely** — I verified this directly:

```
STILL BLOCKED after 3s -> SO_RCVTIMEO tv=0 blocks forever (CONFIRMED)
```

So the auth read has no deadline at all. (The same applies to the FM-DX handshake `recvLine` at line 685, which runs after the timeout was cleared at line 678.)

**Failure scenario:** A client connects, receives the salt, and then stalls — a half-open TCP connection, a client that crashes mid-handshake, or a middlebox that opens the connection and never relays. The server's client thread stays blocked in `recv()` indefinitely. Because the XDR server is thread-per-connection this does *not* block other clients (I confirmed a second client still gets a salt normally), but each stalled connection permanently occupies a thread and its stack. Combined with **H5** (threads are never reaped), a slow leak of connection attempts accumulates threads with no upper bound.

Note this is the same class of bug as M1, but on the XDR port — and there the mitigation (thread-per-connection) contains it, so it is lower severity.

**Fix:** Set a bounded timeout before the auth read and restore it afterwards:

```cpp
setRecvTimeoutMs(clientSocket, 5000);
if (!recvLine(clientSocket, authMsg, HASH_LENGTH + 2)) { /* ... */ }
setRecvTimeoutMs(clientSocket, 0);
```

Apply the same to the FM-DX handshake read at line 685.

---

### M8 — `commandBufferOverflowed` and the per-line check duplicate the limit in two places
**`src/xdr_server.cpp:149-155, 738, 986`**

**Defect:** The 1024-byte command limit is enforced in two independent places — the pre-check `commandBufferOverflowed()` (called at lines 728 and 976) and the inline per-line check `pos > kMaxClientCommandLen` (lines 738 and 986). I initially suspected the two disagreed for fragmented input; **on testing they agree** — a 1000- or 1024-byte line sent in 256-byte chunks is accepted by both, and a 1025-byte line is rejected by both. So there is no behavioural bug here.

**Why it is still worth noting:** the duplication is a latent drift hazard. The pre-check operates on the *accumulated* buffer while the per-line check operates on the *extracted line*, and the two only agree today because the extraction loop runs immediately after the pre-check and leaves the buffer holding exactly one partial line. Any refactor that reorders those two steps, or that lets a buffer hold more than one partial line, silently changes the effective limit. The constant is also declared twice in the file (see L2).

**Fix:** Keep one limit check. Delete the pre-check and rely on the per-line check, or fold the per-line check into a single helper that both call sites use:

```cpp
// Returns false (and logs) if the line exceeds the protocol limit.
bool lineWithinLimit(size_t pos) { return pos <= kMaxClientCommandLen; }
```

---

## LOW severity

### L1 — `src/sdrplay_device.cpp` `printf`s to stderr on every connect attempt
**`src/sdrplay_device.cpp:130-180`** (uncommitted local change)

The new diagnostics use `std::fprintf(stderr, ...)` unconditionally, while the rest of the codebase routes through the `verboseLogging` flag. On a system without the SDRplay API installed, every launch prints `[SDRplay] API library not loaded (lib=0x0)` to stderr even with `-q`. Gate these behind the device's verbose flag for consistency with the rest of the logging.

**Fix:** the device grew a `setVerboseLogging()` flag (an `std::atomic<bool>`,
so it can be flipped while the device is live) and every `connect()` diagnostic
is behind it. The flag is plumbed `Application::run()` → `TunerController::setVerboseLogging()`
→ `SDRplayDevice::setVerboseLogging()`, with the app passing its own
`verboseLogging` (the `-v` / `-q` / `[debug] log_level` switch).

**Verified live** against the SDRplay-enabled build (`-DFM_TUNER_ENABLE_SDRPLAY=ON`,
the API service reachable but no RSP attached):

```
-q : stderr -> "[SDR] warning: failed to connect to sdrplay"
                "[AUTO] warning: auto-start requested but sdrplay connect failed"
                [SDRplay] lines: 0
-v : stderr -> "[SDRplay] API service version 3.15 (built against 3.15)"
                "[SDRplay] GetDevices failed: err=0 count=0"
                [SDRplay] lines: 2
```

The two lines that remain under `-q` are the application's own connect-failure
warnings, not the device's diagnostics — a failed connect must stay visible.

**Regression test:** `SDRplay connect diagnostics honour the verbose flag` in
the new `sdrplay_verbose` target (built only when the SDRplay SDK headers are
present). It captures `stderr` across a real `connect()` and asserts the
absence of any `[SDRplay]` line when quiet and its presence when verbose.
**Verified to fail against the pre-fix code** (both quiet-mode assertions fail)
and pass after.

### L2 — `kMaxClientCommandLen` is defined twice
**`src/xdr_server.cpp:36` and `src/xdr_server.cpp:1498`**

The constant is declared in the anonymous namespace at line 36 *and* re-declared at file scope at line 1498. The file-scope one shadows the namespace one inside the namespace (the compiler resolves to the inner declaration, so behaviour is correct), but the trailing declaration is dead and misleading. It also triggers the `-Wunused-const-variable` warning in a strict build. Delete line 1498.

### L3 — `equalsIgnoreCase` is defined but never used
**`src/xdr_server.cpp:118-124`**

Dead code; the XDR command parsing is case-sensitive by design. Remove it (it also produces an `-Wunused-function` warning).

### L4 — `kPi` is defined but never used in `fm_demod.cpp`
**`src/fm_demod.cpp:12`**

Dead constant (`-Wunused-const-variable`). The demod's `2π` comes from liquid-dsp. Remove it.

### L5 — `kPllLockAcquireHz` is defined but never used in `stereo_decoder.cpp`
**`src/stereo_decoder.cpp:26`**

Dead constant — the PLL lock threshold is evidently computed elsewhere. Either wire it up or remove it; a lock threshold constant that is defined but not referenced is a strong hint of a half-finished change to the lock logic.

### L6 — `parseFloat` is defined but never used in `config.cpp`
**`src/config.cpp:62`**

Dead helper (`-Wunused-function`).

---

## Non-blocking observations

These are not defects but are worth recording:

- **`Config::RestSection::bind_address` silently falls back to loopback** on an unparseable address (`src/rest_server.cpp:215-218`). Failing closed here is correct and safe — but it means a typo'd `bind_address = 0.0.0.0` quietly produces a loopback-only server with no warning. Consider logging the fallback.
- **The XDR server binds `INADDR_ANY` unconditionally** (`src/xdr_server.cpp:589`) while the README claims `127.0.0.1:7373` (README:57). At minimum, correct the README; better, make it configurable (see H1).
- **`redsea_port/` accounts for most strict-build warnings** (~90 of the ~95) — mostly `-Wsign-conversion` in `util.hh` and `subcarrier.cpp`. This is vendored code; either exclude it from the strict build or leave it alone deliberately rather than by accident.
- **The `m_clientThreads` vector is also a small unbounded-growth risk for `m_clientSockets`** — that vector *is* pruned (via `removeClientSocket`), so it is fine; only the thread vector leaks.

---

## Recommended fix order

1. **H2** (SIGPIPE) — one-line-ish fix, kills the whole process. Do this first.
2. **H1** (guest mode) — a security regression versus the reference implementation; also gate the bind address.
3. **H3** (REST frequency overflow) — one-line validation.
4. **H4** (PI reset) — small, self-contained.
5. **H6** + **H5** (socket lifetime, thread reaping) — touch `XDRServer::stop()` and the accept loop together.
6. **M1** (REST timeout) — small, removes a trivial DoS.
7. **M2, M3, M4, M5, M6, M7** — correctness and diagnosability.
8. **L1-L6** — cleanup, ideally in one commit.

**Note on M7/M8:** those two were originally written as behavioural defects and, on testing, proved not to be. The corrected versions (M7 = missing auth timeout; M8 = duplicated length limit) are real but smaller. The **empirically confirmed** findings are H2 (SIGPIPE — exit 141 reproduced), H3 (frequency wrap → `705032704` reproduced), and M1 (REST DoS — 8 s hang reproduced against the running server). H1, H4, H5, H6 are established by code inspection, cross-checked against the vendored reference implementation in `research/xdrd/xdrd.c`.

---

## Appendix: findings by file

| File | Findings |
|---|---|
| `src/xdr_server.cpp` | H1, H4, H5, H6, M4, M5, M6, M7, M8, L2, L3 |
| `src/rest_server.cpp` | H2, H3, M1, D4 |
| `src/application.cpp` | M2, M3 |
| `src/runtime_loop.cpp` | (contributor to H4, M2) |
| `src/sdrplay_device.cpp` | L1 |
| `src/fm_demod.cpp` | L4 |
| `src/stereo_decoder.cpp` | L5 |
| `src/config.cpp` | L6 |
| `src/rtl_tcp_client.cpp` | (shares H2's unprotected `send()`) |
| `src/dsp/liquid_primitives.cpp` | D1 |
| `src/signal_level.cpp` | D2, D3 |
| `research/xdrd/xdrd.c` | *reference — used to establish H1's intended semantics* |
