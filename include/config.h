#ifndef CONFIG_H
#define CONFIG_H

#include <string>
#include <cstdint>

struct Config {
    struct RTLTCPSection {
        std::string host = "localhost";
        uint16_t port = 1234;
    } rtl_tcp;

    struct AudioSection {
        std::string device;
        bool enable_audio = false;
    } audio;

    struct SDRSection {
        int rtl_gain_db = -1;
        int default_custom_gain_flags = 0;
        std::string gain_strategy = "tef"; // tef|sdrpp
        bool sdrpp_rtl_agc = false;
        int sdrpp_rtl_agc_gain_db = 18;
        double signal_floor_dbfs = -55.0;
        double signal_ceil_dbfs = -19.0;
    } sdr;

    struct TunerSection {
        std::string source = "rtl_sdr"; // rtl_sdr|rtl_tcp
        uint32_t rtl_device = 0;
        uint32_t default_freq = 88600;
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
        std::string stereo_blend = "normal";
        bool stereo = true;
    } processing;

    struct DebugSection {
        int log_level = 1;
    } debug;

    struct ReconnectionSection {
        bool auto_reconnect = true;
    } reconnection;

    bool loadFromFile(const std::string& filename);
    void loadDefaults();
};

#endif
