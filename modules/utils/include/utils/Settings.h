#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace recum12::utils {

struct RemotePortsConfig {
    int              reconnect_ms{3000};
    std::uint16_t    client_port{5051};
    std::string      server_host{"192.168.2.2"};
    std::uint16_t    server_port{5051};
};

struct RemoteConfig {
    RemotePortsConfig         ports{};
    std::vector<std::string>  prefer_iface{}; // Örn: {"eth0","wlan0","ppp0"}
};

struct Rs485Config {
    std::string  name{"pump"};
    std::string  port{"/dev/ttyUSB0"};
    int          baud{9600};
    int          data_bits{8};
    char         parity{'O'};   // 'O', 'E', 'N' vb.
    int          stop_bits{1};
};

class Settings {
public:
    /// Varsayılan değerleri (kod içi defaults) yükler.
    Settings();

    /// Belirtilen JSON dosyasından ayarları okuyup döner.
    /// Okuma/parse hatası olursa kod içi varsayılanlarla devam eder.
    static Settings loadFromFile(const std::string& path);

    /// `configs/default_settings.json` dosyasını kullanarak ayar yükler.
    static Settings loadDefault();

    const RemoteConfig& remote() const noexcept { return remote_; }
    const std::vector<Rs485Config>& rs485() const noexcept { return rs485_; }

private:
    RemoteConfig              remote_{};
    std::vector<Rs485Config>  rs485_{};
};

} // namespace recum12::utils
