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
    // Old defaults (-50/-18) covered only 32 dB and saturated at both ends on
    // a typical mixed band (strong locals → 120, fringe → 0). The wider 60 dB
    // window below leaves headroom: strong locals settle near 90-100, fringe
    // stations register meaningfully around 20-40. Override per site in the
    // INI; the active values are logged at startup as [METER].
    double signal_floor_dbfs = -65.0;
    double signal_ceil_dbfs = -5.0;
    double signal_bias_db = 0.0;
    bool low_latency_iq = false;
  } sdr;

  struct TunerSection {
    std::string source = "rtl_sdr"; // rtl_sdr|rtl_tcp
    uint32_t rtl_device = 0;
    uint32_t default_freq = 87500;
    int deemphasis = 0;
  } tuner;

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
    std::string stereo_blend = "aggressive";
    bool stereo = true;
    bool pilot_canceller = true;
    std::string hicut = "off"; // off|gentle|strong
    std::string adaptive_bandwidth = "off"; // off|conservative|aggressive
    std::string multipath_eq = "off"; // off|light|aggressive
    int multipath_eq_taps = 17;
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
