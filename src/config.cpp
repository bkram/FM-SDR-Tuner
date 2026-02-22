#include "config.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <string>

namespace {
std::string trim(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start])) != 0) {
        start++;
    }

    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        end--;
    }

    return value.substr(start, end - start);
}

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool parseBool(const std::string& raw, bool& out) {
    const std::string value = toLower(trim(raw));
    if (value == "1" || value == "true" || value == "yes" || value == "on") {
        out = true;
        return true;
    }
    if (value == "0" || value == "false" || value == "no" || value == "off") {
        out = false;
        return true;
    }
    return false;
}

bool parseInt(const std::string& raw, int& out) {
    try {
        size_t idx = 0;
        const int value = std::stoi(trim(raw), &idx);
        if (idx != trim(raw).size()) {
            return false;
        }
        out = value;
        return true;
    } catch (...) {
        return false;
    }
}

bool parseFloat(const std::string& raw, float& out) {
    try {
        const std::string t = trim(raw);
        size_t idx = 0;
        const float value = std::stof(t, &idx);
        if (idx != t.size()) {
            return false;
        }
        out = value;
        return true;
    } catch (...) {
        return false;
    }
}

bool parseDouble(const std::string& raw, double& out) {
    try {
        const std::string t = trim(raw);
        size_t idx = 0;
        const double value = std::stod(t, &idx);
        if (idx != t.size()) {
            return false;
        }
        out = value;
        return true;
    } catch (...) {
        return false;
    }
}
}  // namespace

void Config::loadDefaults() {
    rtl_tcp = RTLTCPSection{};
    audio = AudioSection{};
    sdr = SDRSection{};
    tuner = TunerSection{};
    xdr = XDRSection{};
    processing = ProcessingSection{};
    debug = DebugSection{};
    reconnection = ReconnectionSection{};
}

bool Config::loadFromFile(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Failed to open config file: " << filename << "\n";
        return false;
    }

    std::string section;
    std::string line;
    int lineNo = 0;

    while (std::getline(file, line)) {
        lineNo++;

        const size_t commentPos = line.find_first_of("#;");
        if (commentPos != std::string::npos) {
            line = line.substr(0, commentPos);
        }

        line = trim(line);
        if (line.empty()) {
            continue;
        }

        if (line.front() == '[' && line.back() == ']') {
            section = toLower(trim(line.substr(1, line.size() - 2)));
            continue;
        }

        const size_t equalsPos = line.find('=');
        if (equalsPos == std::string::npos) {
            std::cerr << "Config parse warning (" << filename << ":" << lineNo
                      << "): expected key=value\n";
            continue;
        }

        const std::string key = toLower(trim(line.substr(0, equalsPos)));
        const std::string value = trim(line.substr(equalsPos + 1));

        if (section == "rtl_tcp") {
            if (key == "host") {
                rtl_tcp.host = value;
            } else if (key == "port") {
                int parsed = 0;
                if (parseInt(value, parsed) && parsed >= 1 && parsed <= 65535) {
                    rtl_tcp.port = static_cast<uint16_t>(parsed);
                }
            }
        } else if (section == "audio") {
            if (key == "device") {
                audio.device = value;
            } else if (key == "enable_audio") {
                bool parsed = false;
                if (parseBool(value, parsed)) {
                    audio.enable_audio = parsed;
                }
            }
        } else if (section == "sdr") {
            if (key == "rtl_gain_db") {
                int parsed = 0;
                if (parseInt(value, parsed)) {
                    sdr.rtl_gain_db = parsed;
                }
            } else if (key == "default_custom_gain_flags") {
                int parsed = 0;
                if (parseInt(value, parsed)) {
                    const int rf = ((parsed / 10) % 10) ? 1 : 0;
                    const int ifv = (parsed % 10) ? 1 : 0;
                    sdr.default_custom_gain_flags = rf * 10 + ifv;
                }
            } else if (key == "gain_strategy") {
                const std::string parsed = toLower(trim(value));
                if (parsed == "tef" || parsed == "sdrpp") {
                    sdr.gain_strategy = parsed;
                }
            } else if (key == "sdrpp_rtl_agc") {
                bool parsed = false;
                if (parseBool(value, parsed)) {
                    sdr.sdrpp_rtl_agc = parsed;
                }
            } else if (key == "sdrpp_rtl_agc_gain_db") {
                int parsed = 0;
                if (parseInt(value, parsed)) {
                    sdr.sdrpp_rtl_agc_gain_db = std::clamp(parsed, 0, 28);
                }
            } else if (key == "signal_floor_dbfs") {
                double parsed = 0.0;
                if (parseDouble(value, parsed)) {
                    sdr.signal_floor_dbfs = parsed;
                }
            } else if (key == "signal_ceil_dbfs") {
                double parsed = 0.0;
                if (parseDouble(value, parsed)) {
                    sdr.signal_ceil_dbfs = parsed;
                }
            }
        } else if (section == "tuner") {
            if (key == "source") {
                const std::string parsed = toLower(trim(value));
                if (parsed == "rtl_sdr" || parsed == "rtl_tcp") {
                    tuner.source = parsed;
                }
            } else if (key == "rtl_device") {
                int parsed = 0;
                if (parseInt(value, parsed) && parsed >= 0) {
                    tuner.rtl_device = static_cast<uint32_t>(parsed);
                }
            } else if (key == "default_freq") {
                int parsed = 0;
                if (parseInt(value, parsed) && parsed > 0) {
                    tuner.default_freq = static_cast<uint32_t>(parsed);
                }
            } else if (key == "deemphasis") {
                int parsed = 0;
                if (parseInt(value, parsed)) {
                    tuner.deemphasis = parsed;
                }
            }
        } else if (section == "xdr") {
            if (key == "port") {
                int parsed = 0;
                if (parseInt(value, parsed) && parsed >= 1 && parsed <= 65535) {
                    xdr.port = static_cast<uint16_t>(parsed);
                }
            } else if (key == "password") {
                xdr.password = value;
            } else if (key == "guest_mode") {
                bool parsed = false;
                if (parseBool(value, parsed)) {
                    xdr.guest_mode = parsed;
                }
            }
        } else if (section == "processing") {
            if (key == "agc_mode") {
                int parsed = 0;
                if (parseInt(value, parsed)) {
                    processing.agc_mode = parsed;
                }
            } else if (key == "client_gain_allowed") {
                bool parsed = false;
                if (parseBool(value, parsed)) {
                    processing.client_gain_allowed = parsed;
                }
            } else if (key == "dsp_block_samples") {
                int parsed = 0;
                if (parseInt(value, parsed)) {
                    processing.dsp_block_samples = std::clamp(parsed, 1024, 32768);
                }
            } else if (key == "w0_bandwidth_hz") {
                int parsed = 0;
                if (parseInt(value, parsed)) {
                    processing.w0_bandwidth_hz = std::clamp(parsed, 0, 400000);
                }
            } else if (key == "stereo_blend") {
                const std::string parsed = toLower(trim(value));
                if (parsed == "soft" || parsed == "normal" || parsed == "aggressive") {
                    processing.stereo_blend = parsed;
                }
            } else if (key == "stereo") {
                bool parsed = false;
                if (parseBool(value, parsed)) {
                    processing.stereo = parsed;
                }
            }
        } else if (section == "debug") {
            if (key == "log_level") {
                int parsed = 0;
                if (parseInt(value, parsed)) {
                    debug.log_level = parsed;
                }
            }
        } else if (section == "reconnection") {
            if (key == "auto_reconnect") {
                bool parsed = false;
                if (parseBool(value, parsed)) {
                    reconnection.auto_reconnect = parsed;
                }
            }
        }
    }

    return true;
}
