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
        int output_rate = 32000;
        int buffer_size = 1024;
    } audio;

    struct TunerSection {
        uint32_t default_freq = 88600;
        int default_gain = -1;
        int deemphasis = 0;
    } tuner;

    struct XDRSection {
        uint16_t port = 7373;
        std::string password;
        bool guest_mode = false;
    } xdr;

    struct ProcessingSection {
        int agc_mode = 2;
        bool allow_client_gain_override = true;
        bool stereo = true;
        bool rds = true;
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
