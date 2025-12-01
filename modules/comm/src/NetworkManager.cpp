#include "comm/NetworkManager.h"

#include <fstream>
#include <string>
#include <algorithm>
#include <cctype>

namespace {

bool read_file_trim(const std::string& path, std::string& out)
{
    std::ifstream in(path);
    if (!in) {
        return false;
    }

    std::string data;
    std::getline(in, data, '\0');

    // Trim whitespace
    auto not_space = [](unsigned char ch) { return !std::isspace(ch); };

    auto begin = std::find_if(data.begin(), data.end(), not_space);
    auto end   = std::find_if(data.rbegin(), data.rend(), not_space).base();

    if (begin >= end) {
        out.clear();
    } else {
        out.assign(begin, end);
    }

    return true;
}

bool file_equals(const std::string& path, const std::string& expected)
{
    std::string val;
    if (!read_file_trim(path, val)) {
        return false;
    }
    return val == expected;
}

bool file_is_one(const std::string& path)
{
    std::string val;
    if (!read_file_trim(path, val)) {
        return false;
    }
    return (val == "1");
}

} // namespace

namespace recum12::comm {

bool NetworkManager::isWifiConnected() const
{
    // Linux'ta tipik WiFi iface: wlan0
    // /sys/class/net/wlan0/operstate == "up" ise "bağlı" kabul ediyoruz.
    return file_equals("/sys/class/net/wlan0/operstate", "up");
}

bool NetworkManager::isEthernetConnected() const
{
    // Önce carrier'a bak (varsa):
    // /sys/class/net/eth0/carrier == "1" → link var
    if (file_is_one("/sys/class/net/eth0/carrier")) {
        return true;
    }

    // Aksi halde operstate fallback
    return file_equals("/sys/class/net/eth0/operstate", "up");
}

bool NetworkManager::isGsmConnected() const
{
    // Şimdilik placeholder: ileride gerçek GSM durumu entegre edilecek.
    return false;
}

bool NetworkManager::isGpsConnected() const
{
    // Şimdilik placeholder: ileride gerçek GPS durumu entegre edilecek.
    return false;
}

NetworkStatus NetworkManager::queryStatus() const
{
    NetworkStatus st;
    st.wifi_connected     = isWifiConnected();
    st.ethernet_connected = isEthernetConnected();
    st.gsm_connected      = isGsmConnected();
    st.gps_connected      = isGpsConnected();
    return st;
}

} // namespace recum12::comm
