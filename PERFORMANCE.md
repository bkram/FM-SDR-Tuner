# CPU Use and Options to Reduce It

Measured on 2026-10-25 on branch `v1.8`, after commit `a8c777d`. This file
records where the DSP chain spends its time, what has been done about it, and
what could still be done, with the expected gain for each option.

## How it was measured

The figures come from offline harnesses that run the project's own sources on
a synthetic stereo FM signal (L and R tones, 9% pilot, light noise) at
256 kHz, in 8192-sample blocks, the same way the application does:

- **Chain:** `DspPipeline::process` plus `computeSignalLevel` plus the RDS
  decoder per block. This is the whole receive path except USB, audio output
  and the network threads.
- **Decoder:** `StereoDecoder::processAudio` on its own.
- **Profile (x86 only):** a 1 ms `SIGPROF` sampler that records the
  instruction pointer and resolves it with `dladdr`. Linux `perf` was not
  available (`perf_event_paranoid` = 4).

CPU is given as a percentage of one core needed to keep up with real time.
The M1 figures are the best of three runs, because its performance and
efficiency cores add noise.

## Where the time goes

| Machine | liquid-dsp build | Chain before `a8c777d` | Chain after | Decoder before | Decoder after |
|---|---|---|---|---|---|
| Intel i7-1355U, Ubuntu | distribution package 1.7.0, scalar | 17.2% | 12.0% | 10.5% | 4.6% |
| Apple M1 Pro (hikari), macOS | Homebrew, NEON | 8.6% | 7.0% | 4.7% | 3.1% |

The x86 profile after `a8c777d`, as a share of the chain's time:

| Share | Where |
|---|---|
| 33% | liquid-dsp dot products, of which: |
| 12% | the audio resampler (`AFPostProcessor`) |
| 10% | the channel filter in the demodulator |
| 9% | the pilot and RDS bands in the stereo decoder (now at 32 kHz) |
| 14% | the stereo decoder's own per-sample code (PLL, noise probes, blend) |
| 4% | `atan2f` in the FM discriminator |
| 6% | FIR window pushes (`firfilt_crcf_push`) |
| 3% | the DC tracker ahead of the channel filter |

The dot products show up as `dotprod_rrrf_run4` and `dotprod_crcf_run4`.
Those are liquid-dsp's portable scalar versions. Ubuntu's `libliquid1` is
built without SIMD so that it runs on any x86 CPU; Debian and Raspberry Pi OS
packages are likely the same, but that has not been checked. Homebrew's
build on macOS uses NEON (`dotprod_crcf_execute_neon4`), which is why the M1
spends a smaller share of its time there.

## Done

### Pilot and RDS bands filtered at 32 kHz (`a8c777d`)

The pilot band-pass (two 245-tap FIRs) and the RDS band-pass (163 taps) ran
on every 256 kHz sample and took 40% of the chain's time on x86. Each band is
now mixed down to DC and filtered with the same Kaiser prototype every eighth
sample. The pilot phasor is rebuilt per sample by interpolation, so the PLL
runs as before.

Separation, blend, PLL error, pilot and RDS deviation and the noise-only
behaviour are unchanged on identical test signals (separation 33.1 dB at
1 kHz on both machines). This cut the decoder by 56% on x86 and 33% on the
M1.

This also removes most of the CPU argument for P18 in `todo.md` (dropping the
pilot band-pass altogether): the filter now costs an eighth of what it did.

## Options

Ordered by expected gain for the risk. None of them is needed for normal use
on either machine; they matter for low-power devices such as a Raspberry Pi.

### 1. Own SIMD dot-product kernel

Write one dot-product kernel and use it in `FIRFilter`, the baseband bands
and the audio resampler instead of liquid's. Use
`__attribute__((target_clones("avx2","default")))` on x86, so a single binary
picks the fast path at run time, and NEON on ARM.

- **Evidence:** a 245-tap real dot product takes 73.6 ns with the packaged
  liquid and 6.8 ns with AVX2+FMA on the i7, 10.9 times faster.
- **Expected gain:** the 33% share falls to about 3–4%, so the x86 chain goes
  from about 12% to about 8% of a core. Likely similar on a Pi with a scalar
  liquid package.
- **On hikari:** small, because Homebrew's liquid already uses NEON.
- **Risk:** low. The filters' output must stay bit-for-bit close; the
  existing DSP tests cover them.
- **Note:** `FM_TUNER_ENABLE_X86_AVX2` exists but only affects the project's
  own code, and it is off by default, so release builds do not use it.

### 2. Two-stage audio resampler

The 256 kHz to 48 kHz resampler became about three times as expensive when the
treble was fixed (half-length 160 at a 16.5 kHz cutoff). A cheap half-band
stage from 256 to 128 kHz ahead of the sharp stage, or a fixed-ratio
polyphase design (256:48 = 16:3), would do the same filtering for less.

- **Expected gain:** roughly half of the resampler's 12%, so about 6% of the
  chain on x86. Helps every platform, including hikari.
- **Risk:** medium. The treble response (within 0.2 dB to 15 kHz) and the
  pilot rejection (82 dB) must be re-measured; the existing de-emphasis test
  checks the response against the analogue curve.
- This replaces the "fixed-rate polyphase 256→48 resampler" idea that
  `todo.md` deferred from v1.6.0.

### 3. Build liquid-dsp from source with SIMD

Fetch and build liquid-dsp in CMake with its SIMD paths enabled, instead of
using the distribution package.

- **Expected gain:** similar to option 1 on x86 Linux and a Pi.
- **Risk:** adds a build dependency and its own platform detection, and every
  packaging target (deb, rpm, Windows, macOS) has to follow. Option 1 gets
  the same result with less to maintain.

### 4. Block-batched filtering

Every filter pushes one sample and executes per sample. Pushing a whole
block and running the filter over it keeps the SIMD kernel busy and removes
most of the 6% spent in window pushes.

- **Expected gain:** a few percent; larger once option 1 makes the dot
  products themselves cheap.
- **Risk:** medium; it touches every filter call site.
- `todo.md` deferred this from v1.6.0 because the M1 noise floor hid the gain.

### 5. Fast `atan2` in the discriminator

The discriminator calls libm's `atan2f` per sample (4%). The polynomial
already used in the stereo decoder (`fast_atan2f`, error about 0.0015 rad)
would remove most of that.

- **Risk:** the error lands directly in the audio. At full deviation it is
  about 0.08% of full scale, so it would need a distortion measurement before
  it could be accepted.

### Not worth it at this load

- **GPU offload:** the whole chain is a few percent of one core; transfer
  overhead would exceed the work.
- **More threads:** RDS already runs on its own thread, and the remaining
  stages depend on each other sample by sample.
