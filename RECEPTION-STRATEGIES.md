# Reception Strategies — FM-SDR-Tuner

A consolidated guide to getting the best reception out of an RTL-SDR / SDRplay
based FM tuner, ordered by how much each factor actually matters. Every
recommendation below is checked against the code, and the knob it refers to
exists in the shipped configuration.

The short version:

> **The antenna dominates everything.** A good external antenna is worth
> 15–25 dB — more than every DSP knob in this project combined. Get the RF
> path right first; no amount of software recovers a signal that never
> reaches the dongle.

---

## The order that matters

Fixing these in the wrong order is the most common cause of "it sounds bad and
I don't know why". Work down the list:

| # | Step | Why it comes here |
|---|---|---|
| 1 | Antenna and RF path | 15–25 dB. Nothing else comes close. |
| 2 | Gain, and *not* clipping | Clipping is unrecoverable; every later step is tuning a distorted signal. |
| 3 | Channel bandwidth | Sets how much adjacent-channel splatter gets in. |
| 4 | Stereo / blend behaviour | Depends on 2 and 3 being right. |
| 5 | Meter calibration | Makes 2 and 3 measurable. |
| 6 | Adaptive DSP features | Only once the baseline is stable. |

Do not tune by the meter alone. Do not judge stereo while the tuner is still
clipping.

---

## 1. Antenna and RF path — the dominant factor

| Situation | What to do |
|---|---|
| Indoor, portable | Telescopic whip, fully extended, **away from** USB cables and the PC chassis. A USB cable draped next to the antenna is a common cause of noise and overload. |
| Indoor, fixed | A small FM-band dipole (~75 cm) or a proper indoor FM antenna beats a telescopic by 6–10 dB. |
| Outdoor | A roof-mounted FM dipole or a dedicated FM-band antenna. Expect 15–25 dB over an indoor telescopic, **plus** much less noise (the indoor case picks up the whole house's electronics). |
| Co-channel rejection | An antenna that rejects the ~10 strong locals is worth more than any DSP: the locals' splatter is what destroys fringe reception. |

Verify by **changing only the antenna** and re-checking the meter on the same
frequency. If the meter moves by several dB, the DSP is not the limiting
factor.

### The R820T sensitivity gap

This project runs on an RTL-SDR's R820T tuner. A dedicated TEF6686/TEF6687
hardware tuner has a lower-noise LNA, sharper analogue IF filtering and faster
hardware AGC — roughly **10–15 dB more sensitive on fringe FM** than an R820T
at the same antenna. The DSP options below close that gap as far as a software
demodulator can, but for deep DX an external antenna — and ideally a
TEF-based receiver — remains the gold standard.

---

## 2. Gain: the auto ladder and when to take manual control

The tuner drives a four-rung gain ladder (A0 = most gain, A3 = least).

| Mode | TEF ladder (dB) | Direct-RTL ladder (dB) |
|---|---|---|
| A0 | 44 | 18 |
| A1 | 36 | 12 |
| A2 | 30 | 7 |
| A3 | 24 | 0 |

The **TEF** ladder is the default (`gain_strategy = tef`) and is the right
choice for an indoor telescopic, because it gives the auto-gain loop much
more headroom to climb on weak signals.

The auto-gain servo has a deliberate 15 dB dead-band: it steps **down** when
the clip ratio exceeds 0.02 **or** the level rises above −5 dBFS, and steps
**up** when the clip ratio is below 0.0005 **and** the level is below
−20 dBFS. That dead-band is wider than a gain step, so the servo settles
instead of pumping.

### Which profile to start from

| Location | Start with | Then |
|---|---|---|
| Dense city / rooftop / strong locals | `tef`, `agc_mode = 3` | If still overloaded, fixed `rtl_gain_db = 8 .. 16` |
| Suburban / mixed | `tef`, `agc_mode = 2` | `agc_mode = 1` if weak stations are too soft |
| Rural / weak-signal / DX | `tef`, `agc_mode = 1` | Fixed `rtl_gain_db = 24 .. 36` if still too weak; `dsp_agc = fast`; `adaptive_bandwidth = aggressive` |

The R820T's total gain tops out near 49.6 dB, so **36 dB is usually the
sweet spot** for DX before adjacent strong locals start to overload. If a
strong local overloads at 36 dB, back off by 3–6 dB.

### When to switch to fixed manual gain

Use fixed `rtl_gain_db` when the site and antenna are stable and you want
repeatable comparisons (benchmarking the meter, or comparing profiles).
Avoid it when moving between locations, swapping antennas, or when local RF
density changes over time — the ladder is there to handle exactly that.

### Overload has a specific sound and a specific log

Overload shows up as **AGC pumping**, blend instability, momentary pilot-quality
drops, and — on the plot — a **weaker copy of a strong station next to it**
(an image response). If a strong local has a ghost beside it on the scan,
reduce the gain first. The `[SIG]` line's `clip` field is the direct indicator;
it should be near zero for a healthy signal.

---

## 3. Channel bandwidth

The channel FIR before the discriminator sets how much adjacent-channel
splatter reaches the demodulator.

| Value | Use for |
|---|---|
| `194000` (default) | Normal WFM stereo. Start here. |
| `168000` / `151000` / `133000` | Slightly less splatter, still full stereo. |
| `95000` / `56000` | Marginal stations, crowded sites, adjacent-channel trouble. |
| `36000` | Mono fringe reception. |

Narrowing the bandwidth raises the SNR of a weak station (less noise in) at
the cost of distortion on a strong one (its own modulation sidebands are
cut). **It is a trade, not an improvement** — which is why the static
default should be 194 kHz and the adaptive controller should decide.

### Adaptive bandwidth (optional)

`adaptive_bandwidth` narrows the FIR when the SNR falls and widens it when it
recovers, with a 2 s hysteresis so it does not oscillate. The policy:

| Mode | SNR → bandwidth |
|---|---|
| `conservative` | >20 dB → 194 kHz, >10 dB → 142 kHz, else 95 kHz |
| `aggressive` | >25 dB → 200 kHz, >15 dB → 130 kHz, >8 dB → 95 kHz, >3 dB → 56 kHz, else 36 kHz |

The controller waits for a valid SNR measurement before its **first**
decision, so it does not narrow on a bogus `snr=0` at startup. Turn it on for
crowded RF environments; leave it off for a quiet site where the static
setting is already right.

---

## 4. Stereo and blend

`stereo_blend` controls how aggressively the decoder trades stereo separation
for noise when the signal is marginal.

| Setting | Behaviour |
|---|---|
| `soft` | Maximum separation. Use only on genuinely strong, clean signals. |
| `normal` | Balanced. |
| `aggressive` | Cleanest mono on weak signals; the HF of a fading stereo signal collapses smoothly instead of hissing. Pairs well with HiCut. |

The `pilot_canceller` (on by default) is a two-tap LMS that subtracts a
phase-locked 19 kHz copy of the pilot tone from the mono path, giving roughly
20 dB of extra suppression beyond the audio low-pass. It is a pure
improvement — turn it off only to A/B it.

**A station at the threshold of lock losing stereo is correct behaviour**, not
a bug. If strong stations also lose stereo, the problem is gain, bandwidth or
pilot quality, not the blend.

---

## 5. De-emphasis, HiCut, and the noise floor

De-emphasis (`[tuner].deemphasis`) is a **transmitter-side** property, not a
receiver preference: use `0` (50 µs) for Europe and most of the world, `1`
(75 µs) for North America and Japan, `2` to disable. Setting the wrong one
audibly dulls the top end.

**HiCut** (`hicut = off | gentle | strong`) is the receiver-side response: it
crossfades between two de-emphasis IIRs, narrowing the top end when the
decoder reports low quality, to suppress hiss on fringe stations without
touching clean ones. `gentle` doubles the configured τ, `strong` multiplies it
by five. The crossfade has a 50 ms time constant, so quality changes do not
ring or click.

Use HiCut when a fringe station hisses above 5 kHz. Leave it off when the
hiss is on a *strong* station — that is overload, and HiCut will only hide it.

### Keeping the noise floor out of the way

Two knobs act on the audio output rather than on the RF path:

| Key | What it does |
|---|---|
| `fade_mute = off \| gentle \| strong` | Smoothly mutes the demod noise burst during a real RF fade. It tracks the station's *own* level — and FM is constant-envelope, so a channel-power drop is a genuine fade, never quiet programme audio — so it will not silence a steady weak station. |
| `squelch_dbfs` | Channel-power squelch on the 48 kHz output, in compensated dBFS, with 3 dB of hysteresis. Useful for scan/DX and unattended operation, where silence on empty channels beats hiss. −120 (the default) disables it. |

Both are output-side. They do not improve the SNR — they stop the noise from
reaching the speakers.

---

## 6. Multipath

Indoor and mobile reception suffer from **ghosting**: a second, delayed copy
of the signal, which in FM causes the classic "fluttering" and a dull,
phase-cancelled sound.

`multipath_eq` inserts a CMA (Godard 1980) complex equalizer in the I/Q path
before the discriminator, in dispersion form with leak regularization toward a
centered-delta tap, so it stays near-transparent on a clean signal instead of
solving for the wrong filter. It only adapts **while stereo lock is
reported**, so it never trains on noise.

| Setting | Use for |
|---|---|
| `light` | Safest — small step size, strong leak. |
| `aggressive` | Real multipath, urban, mobile. |

Watch `[EQ] env_err=` in the log: it should sit near zero on a clean signal
and trend downward when the equalizer is actually cancelling a ghost.

---

## 7. Metrics worth trusting

| Metric | Where | Reading it |
|---|---|---|
| `clip` | `[SIG]` line | Anything above ~0.02 means overload. Fix the gain first. |
| `snr` | `[SIG]` line | The demodulator's SNR — the best single indicator of a usable signal. |
| `pilot` / `quality` | `[ST]` line | Pilot magnitude and decoder quality. `quality` near 0 with `stereo=1` means the lock is marginal. |
| `floor` / `ceil` | `[METER]` line | The calibration window in force. |
| `env_err` | `[EQ]` line | Multipath equalizer's envelope error. |

The `[METER]` session line reports the observed dBFS range and suggests
`floor`/`ceil` values that would use the scale fully. That is the fastest way
to calibrate a site.

---

## 8. Order-of-operations checklist

1. **Fix the hardware.** Stable USB, no disconnects, dongle detected, audio
   works. If this is not true, stop — everything else is noise.
2. **Fix clipping.** Tune a strong local, get `clip` near zero. Do not touch
   the blend before this.
3. **Verify stereo** on that strong local — it should lock cleanly.
4. **Set the bandwidth** to 194 kHz unless you have a specific reason.
5. **Calibrate the meter** (`--calibrate`, or copy the `[METER]` suggestion).
6. **Now** enable adaptive features, one at a time, and re-verify a strong, a
   medium and an empty frequency after each. A change that improves one case
   while breaking the other two is not a good profile.

---

## 9. Site profile

`fm-sdr-tuner.ini.site.example` is a ready-made profile for a European FM
band with an indoor telescopic: TEF gain, 194 kHz bandwidth, aggressive blend,
pilot canceller on, the adaptive features off, and the scan on the client's
plot scale. Copy it to `fm-sdr-tuner.ini` and adjust the gain and the meter
window for your own antenna.

---

## 10. What this software cannot fix

- **Sensitivity.** A software demodulator cannot recover a signal below the
  dongle's own noise floor. That is the antenna and the tuner, not the DSP.
- **Overload that happened in the analogue front end.** Clipping before the
  ADC is not invertible.
- **Co-channel rejection.** Two stations on the same frequency are one
  signal; only antenna selectivity separates them.
- **Distance.** Physics.

The honest summary: **this software can make a marginal signal clean and
comfortable, and it can stop a strong signal from being ruined. It cannot
make an absent signal appear.**
