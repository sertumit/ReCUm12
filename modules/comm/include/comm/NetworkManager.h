#pragma once

#include <string>

namespace recum12::comm {

struct NetworkStatus {
    bool wifi_connected{false};
    bool ethernet_connected{false};
    bool gsm_connected{false}; // şimdilik placeholder
    bool gps_connected{false}; // şimdilik placeholder
};

class NetworkManager {
public:
    NetworkManager() = default;

    /// Mevcut durumu tek seferde döner.
    NetworkStatus queryStatus() const;

    bool isWifiConnected() const;
    bool isEthernetConnected() const;
    bool isGsmConnected() const; // şu an sabit false
    bool isGpsConnected() const; // şu an sabit false
};

} // namespace recum12::comm
