# FM-SDR-Tuner — User Manual

A friendly guide to **starting** and **configuring** the tuner. For the full
feature/algorithm reference and the audio-quality tuning guide, see `README.md`.

## Contents

1. [Quick start](#1-quick-start)
2. [Startup recipes](#2-startup-recipes)
3. [The configuration file](#3-the-configuration-file)
4. [Configuration reference](#4-configuration-reference)
5. [Command-line options](#5-command-line-options)
6. [Compatible clients](#6-compatible-clients)
7. [Standalone control (Python panel)](#7-standalone-control-python-panel)
8. [Troubleshooting startup](#8-troubleshooting-startup)
9. [Glossary](#9-glossary)

---

## 1. Quick start

With an RTL-SDR plugged in:

```bash
./fm-sdr-tuner
```

What a bare run does:

- **Auto-loads `fm-sdr-tuner.ini`** from the current directory if present (see
  §3). The release archive ships a ready-to-use one, so this just works.
- Turns on **audio output** (default).
- Opens the **XDR control server** on `127.0.0.1:7373` and **waits for a
  client** (xdr-gtk / FM-DX-Webserver) to press start — it does not begin
  receiving on its own.

To start receiving immediately, without a client:

```bash
./fm-sdr-tuner --auto-start
```

> **One output is required.** The app needs at least one of: audio (`-s`, the
> default), WAV (`-w`), MPX WAV (`--mpx-wav`), live MPX (`--mpx-audio`), or IQ
> capture (`-i`). With the default config, audio is on, so a bare run is fine.

> **Windows:** first get the dongle working in SDR# (install the WinUSB driver
> with Zadig). If SDR# can't tune it, this won't either. Then run
> `fm-sdr-tuner.exe` from the unzipped folder.

---

## 2. Startup recipes

```bash
# RTL-SDR, start now, play to the default speaker
./fm-sdr-tuner --auto-start

# Start on a frequency (kHz) with a fixed gain
./fm-sdr-tuner --auto-start -f 98900 -g 20

# SDRplay RSP (macOS/Linux, SDRplay-enabled build only)
./fm-sdr-tuner --source sdrplay --auto-start

# For an FM-DX-Webserver: set the XDR password and open the REST API on 9090
./fm-sdr-tuner -P "mypassword" --rest-port 9090

# Send audio to a specific output (e.g. a virtual cable), by name or index
./fm-sdr-tuner -d "CABLE Input"      # name substring (recommended; stable)
./fm-sdr-tuner -d 10                 # device index from `-l`

# List audio output devices and their indices
./fm-sdr-tuner -l

# One-shot calibration sweep (prints recommended meter floor/ceil) and exit
./fm-sdr-tuner --calibrate
```

---

## 3. The configuration file

`fm-sdr-tuner.ini` is the main control surface; the command line is for quick,
temporary overrides.

### How the config is loaded (precedence)

1. **`-c <file>` / `--config <file>`** — load this file (wins over all below).
2. **Auto-load** — with no `-c`, the binary loads `./fm-sdr-tuner.ini` from the
   working directory if it exists. You'll see
   `[Config] auto-loading fm-sdr-tuner.ini …` at startup.
3. **Built-in defaults** — used if no file is found.
4. **Command-line flags** always override whatever the config set.

So the everyday workflow is simply: **edit `fm-sdr-tuner.ini` next to the
program and run it.**

### The files in the package

| File | What it is |
|---|---|
| `fm-sdr-tuner.ini` | Ready-to-use config (a copy of the RTL template). Auto-loaded on first run — **this is the one you edit.** |
| `fm-sdr-tuner.ini.example` | RTL-SDR reference template (sane defaults). |
| `fm-sdr-tuner-sdrplay.ini.example` | SDRplay RSP reference template. |

Your personal `fm-sdr-tuner.ini` is git-ignored and never committed. To reset
or switch hardware, copy a template over it:

```bash
cp fm-sdr-tuner.ini.example fm-sdr-tuner.ini            # RTL-SDR
cp fm-sdr-tuner-sdrplay.ini.example fm-sdr-tuner.ini    # SDRplay
```

> The shipped examples set a couple of values differently from the program's
> built-in defaults on purpose (e.g. `stereo_blend = normal`,
> `startup_volume = 80`, `fade_mute = gentle`, REST enabled on `9090`). The
> reference table below lists the **built-in defaults**; whatever the loaded
> `.ini` sets takes precedence over those.

---

## 4. Configuration reference

INI format: `[section]` headers, `key = value`, `#` or `;` comments. Every key
is optional and falls back to the default shown.

### `[tuner]` — source & RF
| Key | Default | Meaning |
|---|---|---|
| `source` | `rtl_sdr` | `rtl_sdr` (USB), `rtl_tcp` (network), or `sdrplay` (RSP). macOS/Linux prebuilts are SDRplay-capable (install the SDRplay API service); Windows is RTL-only. |
| `rtl_device` | `0` | RTL-SDR device index when `source=rtl_sdr`. |
| `default_freq` | `87500` | Startup frequency in kHz. |
| `deemphasis` | `0` | `0`=50 µs (EU), `1`=75 µs (US/KR), `2`=off. |

### `[sdrplay]` — RSP front end (only when `source=sdrplay`)
| Key | Default | Meaning |
|---|---|---|
| `agc` | `true` | Enable the RSP hardware IF AGC (recommended). |
| `lna_state` | `8` | Front-end LNA gain-reduction step (0 = most gain). |
| `antenna` | `0` | RSPdx/RSPdxR2 input: 0=A, 1=B, 2=C (ignored on single-input models). |
| `bias_tee` | `false` | Enable the RSP bias-tee. |

### `[rtl_tcp]` — network source / sample rate
| Key | Default | Meaning |
|---|---|---|
| `host` | `localhost` | `rtl_tcp` server host (used when `source=rtl_tcp`). |
| `port` | `1234` | `rtl_tcp` server port. |
| `sample_rate` | `256000` | IQ rate: `256000`, `1024000`, or `2048000` (decimated to 256 kHz). |

### `[sdr]` — gain & signal meter
| Key | Default | Meaning |
|---|---|---|
| `gain_strategy` | `tef` | `tef` (auto clip-protect) or `sdrpp` (manual-like). |
| `rtl_gain_db` | `-1` | `-1` = auto/managed; `>=0` = fixed manual gain (dB). **Pin a fixed value to stop auto-gain retuning** (smoother for scanning). |
| `freq_correction_ppm` | `0` | Crystal correction, ppm. |
| `default_custom_gain_flags` | `0` | Packed TEF flags: tens=RF, ones=IF (e.g. `11`). |
| `sdrpp_rtl_agc` / `sdrpp_rtl_agc_gain_db` | `false` / `18` | SDR++-strategy IF AGC + gain. |
| `signal_floor_dbfs` | `-65.0` | Compensated dBFS mapped to meter level **0**. |
| `signal_ceil_dbfs` | `-5.0` | Compensated dBFS mapped to meter level **120**. |
| `signal_bias_db` | `0.0` | Overall meter offset. |
| `low_latency_iq` | `false` | Drop stale IQ backlog rather than buffer it — set `true` for scanning to avoid retune audio bursts. |

> The signal level is a **relative**, install-calibrated reading (not absolute
> field strength). Run `--calibrate` or watch the `[METER]` log line to set the
> `floor`/`ceil` window for your antenna. See the README for the full guide.

### `[audio]` — output
| Key | Default | Meaning |
|---|---|---|
| `enable_audio` | `true` | Enable the 48 kHz speaker output. |
| `device` | (empty) | Empty = system default; a **name substring** (e.g. `CABLE Input`) or a numeric **index** from `-l`. On Windows a name is more stable than an index. |
| `startup_volume` | `100` | Initial volume, 0–100. |

### `[processing]` — DSP
| Key | Default | Meaning |
|---|---|---|
| `agc_mode` | `2` | TEF AGC profile 0–3 (lower = more sensitive, higher = more overload protection). |
| `client_gain_allowed` | `true` | Allow XDR clients to change gain. |
| `dsp_block_samples` | `8192` | IQ samples per processing block. |
| `w0_bandwidth_hz` | `194000` | Channel bandwidth for WFM stereo. |
| `dsp_agc` | `off` | Pre-discriminator IQ AGC: `off`/`fast`/`slow`. |
| `stereo_blend` | `aggressive` | `soft`/`normal`/`aggressive`. |
| `stereo` | `true` | Enable the stereo decoder. |
| `pilot_canceller` | `true` | Subtract residual 19 kHz pilot from mono. |
| `hicut` | `off` | Adaptive de-emphasis: `off`/`gentle`/`strong`. |
| `adaptive_bandwidth` | `off` | SNR-driven channel narrowing: `off`/`conservative`/`aggressive`. |
| `multipath_eq` / `multipath_eq_taps` | `off` / `17` | CMA multipath equalizer + tap count. |
| `fade_mute` | `off` | Soft-mute the demod noise burst on dropouts: `off`/`gentle`/`strong`. |
| `iq_fir_l1_normalize` | `false` | Bound the channel-FIR output envelope (`\|y\| ≤ max\|x\|`). |
| `squelch_dbfs` | `-120.0` | Absolute channel-power squelch threshold (`-120` ≈ off). |

### `[rest]` — anonymous HTTP control API
| Key | Default | Meaning |
|---|---|---|
| `enabled` | `false` | Turn the REST API on (or pass `--rest-port`). |
| `port` | `0` | Listen port (`0` = disabled). The examples use `9090` to avoid FM-DX-Webserver's 8080. |
| `bind_address` | `127.0.0.1` | Bind address. **No authentication** — keep on localhost / a trusted LAN. Use `0.0.0.0` only to reach it from another machine. |

### `[xdr]` — XDR/FM-DX control server
| Key | Default | Meaning |
|---|---|---|
| `port` | `7373` | XDR server port. |
| `password` | (empty) | Client password. Empty + no `-P` ⇒ automatic guest mode. |
| `guest_mode` | `false` | Force guest mode (no password). |

### `[debug]` / `[reconnection]`
| Key | Default | Meaning |
|---|---|---|
| `log_level` | `1` | `>0` = verbose `[SIG]`/`[ST]`/`[AUDIO]` logs (override with `-v`/`-q`). |
| `auto_reconnect` | `true` | Reconnect automatically if the device drops. |

---

## 5. Command-line options

Flags override the config for that run.

```
-c, --config <file>     Load this INI (else ./fm-sdr-tuner.ini is auto-loaded)
    --source <name>     rtl_sdr | rtl_tcp | sdrplay
    --rtl-device <id>   RTL-SDR device index
-t, --tcp <host:port>   rtl_tcp server address
    --iq-rate <rate>    256000 | 1024000 | 2048000
-f, --freq <khz>        Startup frequency in kHz
-g, --gain <db>         Fixed RTL gain in dB (default: auto)
-b, --blend <mode>      soft | normal | aggressive
-s, --audio             Enable audio output
    --no-audio          Disable the 48 kHz audio output
-d, --device <id>       Audio output device (name substring or index)
-l, --list-audio        List audio output devices, then exit
-w, --wav <file>        Record audio to WAV
    --mpx-wav <file>    Record decoded MPX to WAV (mono)
    --mpx-rate <hz>     MPX sample rate (e.g. 192000)
    --mpx-audio         Route live MPX to an audio device (macOS/Linux)
    --mpx-audio-device <id>   Device for live MPX
    --mpx-gain-db <db>  Gain on MPX WAV/audio outputs
-i, --iq <file>         Capture raw IQ bytes
    --auto-start        Start receiving without waiting for a client
    --low-latency-iq    Drop IQ backlog (lower latency, fewer retune bursts)
-P, --password <pwd>    XDR server password
-G, --guest             Guest mode (no password)
    --rest-port <port>  REST API port (0 disables; non-zero enables)
    --rest-bind <addr>  REST API bind address
-v, --verbose           Verbose logging
-q, --quiet             Quiet logging
    --calibrate         Band-sweep calibration, then exit
-h, --help              Show help
```

---

## 6. Compatible clients

The tuner exposes an **XDR/FM-DX control server** on port `7373`, so it works as
a drop-in receiver back-end for the XDR ecosystem:

- **FM-DX-Webserver** — browser-based DX receiver front-end.
  <https://github.com/NoobishSVK/fm-dx-webserver>
- **XDR-GTK** — desktop control client for XDR-protocol tuners.
  <https://github.com/kkonradpl/xdr-gtk>

Point the client at the tuner's host on port `7373`, using the same password
you set with `-P` / `[xdr] password` (or no password in guest mode).

**Audio path:** the tuner plays decoded audio to a local output device; it does
**not** stream audio over the XDR connection. To feed audio into a webserver or
encoder, route the tuner's output through a virtual audio cable (BlackHole on
macOS, `snd-aloop` on Linux, VB-CABLE on Windows) — set `[audio] device` to the
cable and have the consumer record from it. See the README's "Audio Loopback"
section.

The XDR protocol has no vocabulary for SDR-specific settings (manual dB gain,
SDRplay LNA/antenna/bias-tee). Those are exposed through the **REST API**
(§7) — intended for an FM-DX-Webserver plugin, and usable standalone right now
via the bundled control panel.

---

## 7. Standalone control (Python panel)

You don't need a webserver or any plugin to drive the tuner. A small browser
control panel ships in the package (`rest_test_panel.py`) and talks to the REST
API directly — handy for tweaking SDR settings and watching live signal/RDS
stats.

**Step 1 — enable the REST API.** Either set it in the config:

```ini
[rest]
enabled = true
port = 9090
bind_address = 127.0.0.1
```

…or pass it on the command line:

```bash
./fm-sdr-tuner --auto-start --rest-port 9090
```

**Step 2 — launch the panel** (needs Python 3; no extra packages):

```bash
# macOS / Linux — convenience launcher
scripts/run_rest_panel.sh
# defaults: API http://127.0.0.1:9090, panel http://127.0.0.1:9091

# any platform — run the script directly (Windows uses this form)
python3 scripts/rest_test_panel.py --api http://127.0.0.1:9090 --port 9091
```

**Step 3 — open the panel** in a browser: <http://127.0.0.1:9091>

What you can do from the panel:

- Tune frequency and channel bandwidth.
- Gain: toggle AGC or set a manual dB value.
- **SDRplay-only** controls (shown automatically when an RSP is connected): LNA
  state, antenna input, bias-tee.
- **RTL-only** controls: frequency-correction ppm, RTL digital AGC.
- Audio: de-emphasis, stereo-blend, force-mono, volume, start/stop.
- A live **stats** strip at the top: signal level, dBFS, SNR, peak deviation,
  overload, stereo lock, pilot, RDS deviation/BER/group count — with a stats
  reset.

The panel is a pure client: every control sends `GET`/`POST` to
`<api>/api/control`, and it polls `<api>/api/status` once a second. You can also
script the same API yourself:

```bash
curl "http://127.0.0.1:9090/api/control?freq_khz=98900"
curl  http://127.0.0.1:9090/api/status
```

> The REST API is **unauthenticated by design** — keep `bind_address` on
> localhost or a trusted network.

---

## 8. Troubleshooting startup

- **`WinMM device not found` / no audio (Windows):** choose a device with
  `-d "CABLE Input"` or `-d <index>` (`-l` lists them), or `[audio] device = …`.
  Empty = system default.
- **REST port not open:** the API is **off** unless enabled — pass
  `--rest-port 9090` or set `[rest] enabled = true`. Look for the
  `[REST] … listening on …` startup line.
- **Audio stutters while tuning/scanning:** each retune produces a backlog
  burst. Set `[sdr] low_latency_iq = true`, pin a fixed `rtl_gain_db`, and make
  sure any virtual-cable output is set to **48000 Hz**.
- **Config seems ignored:** check for the `[Config] auto-loading …` line at
  startup. If it's missing, no `fm-sdr-tuner.ini` was found in the working
  directory — `cd` into the folder that has it, or pass `-c <file>`.
- **`--source sdrplay` does nothing:** the **macOS and Linux** prebuilt
  binaries are SDRplay-capable, but you must install the **SDRplay API service**
  (from <https://www.sdrplay.com/>) for the binary to talk to an RSP — install
  it and retry. The **Windows** prebuilt is RTL-only (no SDRplay loader).
  Building from source for SDRplay needs `-DFM_TUNER_ENABLE_SDRPLAY=ON`
  (macOS/Linux only).

---

## 9. Glossary

| Term | Meaning |
|---|---|
| **RTL-SDR** | Inexpensive USB software-defined-radio dongle (RTL2832U + tuner, e.g. R820T). The default input. |
| **SDRplay / RSP** | A family of higher-end SDR receivers (RSP1A, RSPdx, …) using SDRplay's API. |
| **rtl_tcp** | A network protocol/server that streams RTL-SDR IQ over TCP; lets the tuner use a remote dongle. |
| **IQ** | The raw complex (in-phase/quadrature) sample stream from the SDR, before demodulation. |
| **MPX (multiplex)** | The full demodulated FM baseband: mono (L+R) audio, 19 kHz pilot, 38 kHz stereo subcarrier (L−R), 57 kHz RDS. |
| **Pilot** | The 19 kHz tone a stereo FM station transmits; its presence/strength drives stereo lock. |
| **Stereo blend** | Gradually collapsing stereo toward mono as reception worsens, to trade stereo image for less noise. |
| **De-emphasis** | The receive-side treble roll-off that undoes the transmitter's pre-emphasis (50 µs in EU, 75 µs in US/KR). |
| **RDS** | Radio Data System — the digital data on the 57 kHz subcarrier (station name, PI, etc.). |
| **BER** | Bit Error Rate — how many RDS data blocks arrive corrupted; lower is better. |
| **SNR** | Signal-to-noise ratio. Here, a demod-domain reception-quality figure (high on clean signals). |
| **dBFS** | Decibels relative to full scale — the receiver's internal power reference (not absolute field strength). |
| **Signal level (0–120)** | The "dBf"-style meter value: a relative reading mapped from dBFS across the `floor`/`ceil` window. |
| **Deviation** | How far the FM carrier swings; ±75 kHz is full modulation. Pilot/RDS/MAX-DEV are reported in kHz. |
| **AGC** | Automatic Gain Control — adjusts gain to keep the signal in range. RF/IF AGC (front-end) here, not audio AGC. |
| **Overload / clipping** | The ADC railing because gain is too high; causes distortion and image/ghost artifacts. Reduce gain. |
| **XDR / FM-DX protocol** | The control protocol (port 7373) spoken by XDR-GTK and FM-DX-Webserver to tune and read the receiver. |
| **REST API** | The tuner's HTTP control surface (`/api/status`, `/api/control`) for SDR-specific settings; unauthenticated. |
| **Virtual audio cable** | A software loopback device (BlackHole / snd-aloop / VB-CABLE) that routes the tuner's audio into another app. |
| **Bias-T** | A DC voltage injected on the antenna coax to power an external LNA (SDRplay/RTL models that support it). |
| **LNA** | Low-Noise Amplifier — the front-end gain stage; on SDRplay, `lna_state` sets its gain-reduction step. |
