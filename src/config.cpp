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
}  // namespace

void Config::loadDefaults() {
    *this = Config{};
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
            } else if (key == "output_rate") {
                int parsed = 0;
                if (parseInt(value, parsed) && parsed > 0) {
                    audio.output_rate = parsed;
                }
            } else if (key == "buffer_size") {
                int parsed = 0;
                if (parseInt(value, parsed) && parsed > 0) {
                    audio.buffer_size = parsed;
                }
            }
        } else if (section == "tuner") {
            if (key == "default_freq") {
                int parsed = 0;
                if (parseInt(value, parsed) && parsed > 0) {
                    tuner.default_freq = static_cast<uint32_t>(parsed);
                }
            } else if (key == "default_gain") {
                int parsed = 0;
                if (parseInt(value, parsed)) {
                    tuner.default_gain = parsed;
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
            } else if (key == "guest_mode" || key == "guest") {
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
            } else if (key == "allow_client_gain_override" || key == "allow_client_agc_override") {
                bool parsed = false;
                if (parseBool(value, parsed)) {
                    processing.allow_client_gain_override = parsed;
                }
            } else if (key == "stereo") {
                bool parsed = false;
                if (parseBool(value, parsed)) {
                    processing.stereo = parsed;
                }
            } else if (key == "rds") {
                bool parsed = false;
                if (parseBool(value, parsed)) {
                    processing.rds = parsed;
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
