#include "utils/Settings.h"
#include <fstream>

#include <nlohmann/json.hpp>

namespace recum12::utils {

namespace {

constexpr const char* kDefaultSettingsPath = "configs/default_settings.json";

using Json = nlohmann::json;

} // namespace

Settings::Settings()
{
    // Kod içi varsayılanlar (default_settings.json ile uyumlu)
    remote_.ports.reconnect_ms = 3000;
    remote_.ports.client_port  = 5051;
    remote_.ports.server_host  = "192.168.2.2";
    remote_.ports.server_port  = 5051;

    remote_.prefer_iface = {"eth0", "wlan0", "ppp0"};

    Rs485Config pumpCfg;
    pumpCfg.name      = "pump";
    pumpCfg.port      = "/dev/ttyUSB0";
    pumpCfg.baud      = 9600;
    pumpCfg.data_bits = 8;
    pumpCfg.parity    = 'O';
    pumpCfg.stop_bits = 1;

    rs485_.push_back(pumpCfg);
}

Settings Settings::loadDefault()
{
    return loadFromFile(kDefaultSettingsPath);
}

Settings Settings::loadFromFile(const std::string& path)
{
    Settings settings; // ctor içindeki varsayılanlarla başla

    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        // Dosya yok veya açılamadı → varsayılanlarla devam
        return settings;
    }

    Json root;
    try {
        ifs >> root;
    } catch (...) {
        // Parse hatasında varsayılanları bozma
        return settings;
    }

    try {
        if (root.contains("remote") && root["remote"].is_object()) {
            const auto& jRemote = root["remote"];

            if (jRemote.contains("ports") && jRemote["ports"].is_object()) {
                const auto& jp = jRemote["ports"];
                settings.remote_.ports.reconnect_ms =
                    jp.value("reconnect_ms", settings.remote_.ports.reconnect_ms);
                settings.remote_.ports.client_port =
                    static_cast<std::uint16_t>(jp.value("client", settings.remote_.ports.client_port));
                settings.remote_.ports.server_host =
                    jp.value("server_host", settings.remote_.ports.server_host);
                settings.remote_.ports.server_port =
                    static_cast<std::uint16_t>(jp.value("server_port", settings.remote_.ports.server_port));
            }

            if (jRemote.contains("prefer_iface") && jRemote["prefer_iface"].is_array()) {
                settings.remote_.prefer_iface.clear();
                for (const auto& iface : jRemote["prefer_iface"]) {
                    if (iface.is_string()) {
                        settings.remote_.prefer_iface.push_back(iface.get<std::string>());
                    }
                }
            }
        }

        if (root.contains("rs485") && root["rs485"].is_array()) {
            settings.rs485_.clear();
            for (const auto& item : root["rs485"]) {
                if (!item.is_object()) {
                    continue;
                }
                Rs485Config cfg;
                cfg.baud      = item.value("baud", 9600);
                cfg.data_bits = item.value("data_bits", 8);
                cfg.name      = item.value("name", std::string{});
                cfg.port      = item.value("port", std::string{});
                cfg.stop_bits = item.value("stop_bits", 1);

                cfg.parity = 'N';
                if (item.contains("parity") && item["parity"].is_string()) {
                    const auto p = item["parity"].get<std::string>();
                    if (!p.empty()) {
                        cfg.parity = p.front();
                    }
                }

                settings.rs485_.push_back(std::move(cfg));
            }
        }
    } catch (...) {
        // Herhangi bir beklenmeyen durumda mevcut (kısmen dolu) ayarları koru
    }

    return settings;
}

} // namespace recum12::utils
