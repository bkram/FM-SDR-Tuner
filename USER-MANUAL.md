# FM-SDR-Tuner — User Manual (Startup & Configuration)

This manual focuses on **starting the tuner** and **configuring it**. For the
full feature/algorithm reference and the tuning-quality guide, see `README.md`.

---

## 1. Quick start

With an RTL-SDR plugged in:

```bash
./fm-sdr-tuner
```

What happens on a bare run:

- It **auto-loads `fm-sdr-tuner.ini`** from the current directory if present
  (see §3). The release archive ships a ready-to-use one.
- Audio output is **on by default**.
- The XDR server listens on `127.0.0.1:7373` and **waits for a client**
  (xdr-gtk / FM-DX-Webserver) to send the start command — the tuner does not
  start receiving until then.

To start receiving immediately without a client:

```bash
./fm-sdr-tuner --auto-start
```

> At least one output must be active or the app refuses to start: audio (`-s`,
> the default), WAV (`-w`), MPX WAV (`--mpx-wav`), live MPX (`--mpx-audio`), or
> IQ capture (`-i`).

**Windows:** get the dongle working in SDR# (via Zadig/WinUSB) first; if SDR#
can't tune it, neither can this. Then run `fm-sdr-tuner.exe` from the unzipped
folder.

---

## 2. Startup recipes

```bash
# RTL-SDR, start now, listen on the default speaker
./fm-sdr-tuner --auto-start

# Pick a starting frequency (kHz) and a fixed gain
./fm-sdr-tuner --auto-start -f 98900 -g 20

# SDRplay RSP (macOS/Linux, SDRplay-enabled build only)
./fm-sdr-tuner --source sdrplay --auto-start

# Feed an fm-dx-webserver: XDR password + REST control API on 9090
./fm-sdr-tuner -P "mypassword" --rest-port 9090

# Send audio into a specific output (e.g. a virtual cable) by name or index
./fm-sdr-tuner -d "CABLE Input"      # name substring
./fm-sdr-tuner -d 10                 # device index from `-l`

# List the audio output devices and their indices
./fm-sdr-tuner -l

# One-shot calibration sweep (prints recommended meter floor/ceil), then exits
./fm-sdr-tuner --calibrate
```

---

## 3. The configuration file

`fm-sdr-tuner.ini` is the primary control surface; the CLI is for transient
overrides.

### Loading & precedence

1. **`-c <file>` / `--config <file>`** — load this file explicitly (wins over
   everything below).
2. **Auto-load** — with no `-c`, the binary loads `./fm-sdr-tuner.ini` from the
   working directory if it exists. You'll see
   `[Config] auto-loading fm-sdr-tuner.ini …` at startup.
3. **Built-in defaults** — if no file is found, sensible defaults are used.
4. **CLI flags always override** whatever the config set (handy for A/B tests).

So the simple workflow is: edit `fm-sdr-tuner.ini` next to the executable and
just run it.

### The shipped example configs

The repo and release archives include two documented templates plus a
ready-to-use copy:

| File | Purpose |
|---|---|
| `fm-sdr-tuner.ini` | Ready-to-use config (a copy of the RTL example); auto-loaded on first run. Edit this one. |
| `fm-sdr-tuner.ini.example` | RTL-SDR reference template (sane defaults). |
| `fm-sdr-tuner-sdrplay.ini.example` | SDRplay RSP reference template. |

Your own `fm-sdr-tuner.ini` is git-ignored, so it is never committed. To start
fresh from a template:

```bash
cp fm-sdr-tuner.ini.example fm-sdr-tuner.ini            # RTL-SDR
cp fm-sdr-tuner-sdrplay.ini.example fm-sdr-tuner.ini    # SDRplay
```

---

## 4. Configuration reference

INI format: `[section]` headers, `key = value`, `#`/`;` comments. All keys are
optional; omitted keys take the default shown.

### `[tuner]` — source & RF
| Key | Default | Meaning |
|---|---|---|
| `source` | `rtl_sdr` | `rtl_sdr` (USB), `rtl_tcp` (network), or `sdrplay` (RSP; macOS/Linux, SDRplay build only). |
| `rtl_device` | `0` | RTL-SDR device index when `source=rtl_sdr`. |
| `default_freq` | `87500` | Startup frequency in kHz. |
| `deemphasis` | `0` | `0`=50 µs (EU), `1`=75 µs (US/KR), `2`=off. |

### `[sdrplay]` — RSP front end (used only when `source=sdrplay`)
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
| `sample_rate` | `256000` | IQ sample rate: `256000`, `1024000`, or `2048000` (decimated to 256 kHz). |

### `[sdr]` — gain & signal meter
| Key | Default | Meaning |
|---|---|---|
| `gain_strategy` | `tef` | `tef` (auto clip-protect) or `sdrpp` (manual-like). |
| `rtl_gain_db` | `-1` | `-1` = auto/strategy-managed; `>=0` = fixed manual gain in dB (disables IF AGC). Pin a fixed value to stop auto-gain retuning. |
| `freq_correction_ppm` | `0` | Crystal correction in ppm. |
| `default_custom_gain_flags` | `0` | Packed TEF flags: tens=RF, ones=IF (e.g. `11`). |
| `sdrpp_rtl_agc` / `sdrpp_rtl_agc_gain_db` | `false` / `18` | SDR++-strategy IF AGC + gain. |
| `signal_floor_dbfs` | `-65.0` | Compensated dBFS that maps to meter level 0. |
| `signal_ceil_dbfs` | `-5.0` | Compensated dBFS that maps to meter level 120. |
| `signal_bias_db` | `0.0` | Overall meter offset (align to a reference receiver). |
| `low_latency_iq` | `false` | Drop stale IQ backlog instead of buffering it — set `true` for scanning / low-latency use to avoid retune bursts. |

> The displayed signal level is a **relative**, install-calibrated reading
> (dBFS-referenced, mapped across `floor`/`ceil`), not absolute field strength.
> Run `--calibrate` or watch the `[METER]` log to set the window. See the
> README for details.

### `[audio]` — output
| Key | Default | Meaning |
|---|---|---|
| `enable_audio` | `true` | Enable the 48 kHz speaker output. |
| `device` | (empty) | Output device: empty = system default; a **name substring** (e.g. `CABLE Input`) or a numeric **index** from `-l`. On Windows, a name is more stable than an index. |
| `startup_volume` | `100` | Initial volume, 0–100. |

### `[processing]` — DSP
| Key | Default | Meaning |
|---|---|---|
| `agc_mode` | `2` | TEF AGC profile 0–3 (lower = more sensitivity, higher = more overload protection). |
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
| `squelch_dbfs` | `-120.0` | Absolute channel-power squelch threshold (`-120` ≈ disabled). |

### `[rest]` — anonymous HTTP control API (for an fm-dx-webserver plugin)
| Key | Default | Meaning |
|---|---|---|
| `enabled` | `false` | Turn the REST API on. (Or pass `--rest-port`.) |
| `port` | `0` | Listen port (`0` = disabled). The examples use `9090` to avoid fm-dx-webserver's 8080. |
| `bind_address` | `127.0.0.1` | Bind address. **No authentication** — keep on localhost / trusted LAN. Use `0.0.0.0` only if the webserver is on another machine. |

`GET /api/status` returns live telemetry; `GET|POST /api/control` applies
settings. A browser test panel ships as `rest_test_panel.py`
(`run_rest_panel.sh` on macOS/Linux).

### `[xdr]` — XDR/FM-DX control server
| Key | Default | Meaning |
|---|---|---|
| `port` | `7373` | XDR server port. |
| `password` | (empty) | Client password. Empty + no `-P` ⇒ automatic guest mode (no auth). |
| `guest_mode` | `false` | Force guest mode. |

### `[debug]` / `[reconnection]`
| Key | Default | Meaning |
|---|---|---|
| `log_level` | `1` | `>0` = verbose `[SIG]`/`[ST]`/`[AUDIO]` logging (override per-run with `-v`/`-q`). |
| `auto_reconnect` | `true` | Reconnect automatically if the device drops. |

---

## 5. CLI options (transient overrides)

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
-l, --list-audio        List audio output devices and exit
-w, --wav <file>        Record audio to WAV
    --mpx-wav <file>    Record decoded MPX to WAV (mono)
    --mpx-rate <hz>     MPX sample rate (e.g. 192000)
    --mpx-audio[-device]  Route live MPX to an audio device (macOS/Linux)
    --mpx-gain-db <db>  Gain on MPX WAV/audio outputs
-i, --iq <file>         Capture raw IQ bytes
    --auto-start        Start receiving without waiting for a client
    --low-latency-iq    Drop IQ backlog (lower latency, fewer retune bursts)
-P, --password <pwd>    XDR server password
-G, --guest             Guest mode (no password)
    --rest-port <port>  REST API port (0 disables; enables when non-zero)
    --rest-bind <addr>  REST API bind address
-v, --verbose           Verbose logging      -q, --quiet  Quiet
    --calibrate         Band-sweep calibration, then exit
-h, --help              Show help
```

---

## 6. Startup troubleshooting

- **`WinMM device not found` / no audio (Windows):** select a device with
  `-d "CABLE Input"` or `-d <index>` (see `-l`), or `[audio] device = …`. Empty
  = system default.
- **REST port not open:** the API is **off** unless enabled — pass
  `--rest-port 9090` or set `[rest] enabled = true`. Confirm the
  `[REST] … listening on …` startup line.
- **Audio stutter while tuning/scanning:** each retune produces a backlog
  burst. Set `[sdr] low_latency_iq = true`, pin a fixed `rtl_gain_db`, and make
  sure a virtual-cable output is set to **48000 Hz**.
- **Config seems ignored:** check for the `[Config] auto-loading …` line. If
  it's absent, no `fm-sdr-tuner.ini` was found in the working directory — pass
  `-c <file>` or `cd` into the folder that contains it.
- **`--source sdrplay` does nothing:** SDRplay needs a build with
  `-DFM_TUNER_ENABLE_SDRPLAY=ON` (macOS/Linux only); the prebuilt archives are
  RTL-only. Use `rtl_sdr` otherwise.
