#ifndef CONFIG_H
#define CONFIG_H

#include <cstdint>
#include <string>

struct Config {
  struct RTLTCPSection {
    std::string host = "localhost";
    uint16_t port = 1234;
    uint32_t sample_rate = 256000;
  } rtl_tcp;

  struct AudioSection {
    std::string device;
    bool enable_audio = true;
    int startup_volume = 100;
  } audio;

  struct SDRSection {
    int rtl_gain_db = -1;
    int freq_correction_ppm = 0;
    int default_custom_gain_flags = 0;
    std::string gain_strategy = "tef"; // tef|sdrpp
    bool sdrpp_rtl_agc = false;
    int sdrpp_rtl_agc_gain_db = 18;
    // Meter calibration window: maps (compensated dBFS) → 0..120 linearly.
    // The measured power is the band power of a 30 kHz window around the
    // tuned carrier (the same window the scan engine integrates), so the
    // values are the power of that band: an empty band reads about -55 dBFS
    // and a strong local station about -12 dBFS at 18 dB of gain. The
    // default window below puts the noise floor at 0 and a strong local
    // station near 115. Override per site in the INI; the active values are
    // logged at startup as [METER] and the observed range is reported as
    // [METER] session observed.
    double signal_floor_dbfs = -55.0;
    double signal_ceil_dbfs = -10.0;
    double signal_bias_db = 0.0;
    // The spectral-scan plot window of the reference client. xdr-gtk draws
    // the scan in a FIXED 3..84 window (SCAN_DEFAULT_MIN_LEVEL/MAX_LEVEL)
    // and clips everything outside it, so a scale that runs to 120 pins every
    // strong station flat against the top of the plot. When enabled, the
    // scan line is remapped from the meter's 0..120 onto this window; the
    // "S" line and the REST status keep the 0..120 scale, because the
    // clients that read those convert it themselves (the webserver's dBm
    // mode subtracts 120, the plugin shows it out of 120).
    //
    // Disabled by default: the scan then reports the very same 0..120 number
    // the meter shows for the same station, so the two can be read against
    // each other. A client that draws the scan in a fixed window should
    // switch to its relative scale.
    bool scan_use_client_window = false;
    double scan_client_min_level = 3.0;
    double scan_client_max_level = 84.0;
    bool low_latency_iq = false;
  } sdr;

  struct TunerSection {
    std::string source = "rtl_sdr"; // rtl_sdr|rtl_tcp|sdrplay
    uint32_t rtl_device = 0;
    uint32_t default_freq = 87500;
    int deemphasis = 0;
  } tuner;

  struct SDRplaySection {
    // Enable the RSP hardware IF AGC (recommended; trims IF gain to avoid
    // overload). When true it overrides the [sdr] manual-gain/TEF path for the
    // SDRplay source. LNA state below still sets the front-end gain reduction.
    bool agc = true;
    // Front-end LNA gain-reduction step (0 = most gain). 8 keeps strong
    // broadcast FM out of front-end overload while AGC trims the IF gain.
    int lna_state = 8;
    // Antenna input index (RSPdx/RSPdxR2: 0=A,1=B,2=C; other models: ignored).
    int antenna = 0;
    bool bias_tee = false;
  } sdrplay;

  struct RestSection {
    // Anonymous HTTP control API (for an fm-dx-webserver plugin). Disabled
    // unless a non-zero port is set. No authentication by design — bind to
    // localhost or a trusted network only.
    bool enabled = false;
    uint16_t port = 0;
    std::string bind_address = "127.0.0.1";
  } rest;

  struct XDRSection {
    uint16_t port = 7373;
    std::string password;
    bool guest_mode = false;
  } xdr;

  struct ProcessingSection {
    int agc_mode = 2;
    bool client_gain_allowed = true;
    int dsp_block_samples = 8192;
    int w0_bandwidth_hz = 194000;
    std::string dsp_agc = "off"; // off|fast|slow
    // soft|normal|aggressive. "normal" holds the stereo image steady on a
    // station whose quality estimate dips: measured here on two back-to-back
    // captures of the same station, "aggressive" left the image 8 dB weaker
    // and swinging over 22 dB, while "normal" held it steady.
    std::string stereo_blend = "normal";
    bool stereo = true;
    bool pilot_canceller = true;
    std::string hicut = "off"; // off|gentle|strong
    std::string adaptive_bandwidth = "off"; // off|conservative|aggressive
    std::string multipath_eq = "off"; // off|light|aggressive
    int multipath_eq_taps = 17;
    // Adaptive fade-mute: smoothly mutes the demod noise burst when the RF
    // signal drops out (deep fade / dropout) and fades back in on recovery.
    // off (default) | gentle (only deep fades) | strong (mutes more readily).
    std::string fade_mute = "off";
    // When true, the IQ channel FIR is L1-normalized at design time so that
    // |y[n]| ≤ max|x[n]| for every output sample. Defaults to false to
    // preserve existing meter calibration; enable when ADC-rail clipping is
    // observed and downstream consumers rely on |IQ| ≤ 1.
    bool iq_fir_l1_normalize = false;
    // Squelch threshold on the post-DSP channel power (compensated dBFS).
    // -120 = disabled (default). Useful for scan / unattended workflows
    // where silence on empty channels is preferable to hiss. Hysteresis is
    // a fixed 3 dB above the open threshold.
    double squelch_dbfs = -120.0;
  } processing;

  struct DebugSection {
    int log_level = 1;
  } debug;

  struct ReconnectionSection {
    bool auto_reconnect = true;
  } reconnection;

  bool loadFromFile(const std::string &filename);
  void loadDefaults();
};

#endif
