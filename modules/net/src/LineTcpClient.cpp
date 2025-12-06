#include "net/LineTcpClient.h"
#include "net/CommandDispatcher.h"
#include <nlohmann/json.hpp>

#include <vector>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string>
#include <arpa/inet.h>

using nlohmann::json;

#ifndef PROJECT_ROOT
#define PROJECT_ROOT "."
#endif

namespace Net {

// trim helper
static inline std::string trim_copy(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string{};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// ---- settings yol & okuma yardımcıları (PROJECT_ROOT öncelikli) ----
static inline std::string settings_path() {
    std::vector<std::filesystem::path> cands = {
        std::filesystem::path(PROJECT_ROOT) / "configs" / "default_settings.json",
        std::filesystem::current_path()     / "configs" / "default_settings.json"
    };
    for (const auto& c : cands) {
        std::error_code ec;
        if (std::filesystem::exists(c, ec)) return c.string();
    }
    return (std::filesystem::path(PROJECT_ROOT) / "configs" / "default_settings.json").string();
}

static inline nlohmann::json load_settings_safe() {
    nlohmann::json j = nlohmann::json::object();
    try {
        std::ifstream in(settings_path());
        if (in) in >> j;
    } catch(...) {}
    return j;
}

// --- ctor / dtor ---

LineTcpClient::LineTcpClient(const std::string& host, int port)
    : host_(host)
    , port_(port)
{
    // JSON yok/okunamazsa güvenli öntanımlar
    if (host_.empty())      host_ = "127.0.0.1";
    if (port_ <= 0)         port_ = 5050; // command server default port
    if (reconnect_ms_ <= 0) reconnect_ms_ = 3000;
}

LineTcpClient::~LineTcpClient()
{
    stop();
}

// dispatcher bağlama: eventSink → upstream send
void LineTcpClient::setDispatcher(std::shared_ptr<CommandDispatcher> d)
{
    dispatcher_ = std::move(d);
    if (dispatcher_) {
        dispatcher_->setEventSink([this](const std::string& jline) {
            this->sendLine(jline);
        });
    }
}

// --- soket yardımcıları ---

int LineTcpClient::openSocket()
{
    // runtime ayarlar (remote.server_host / remote.server_port / remote.ports.client / remote.reconnect_ms)
    int local_client_port = -1; // isteğe bağlı: çıkış portunu sabitlemek için
    try {
        nlohmann::json cfg = load_settings_safe();
        if (cfg.contains("remote") && cfg["remote"].is_object()) {
            const auto& r = cfg["remote"];

            // reconnect_ms
            if (r.contains("reconnect_ms") && r["reconnect_ms"].is_number_integer()) {
                reconnect_ms_ = r["reconnect_ms"].get<int>();
            }

            // Uzak command sunucusu: server_host + server_port
            if (r.contains("server_host") && r["server_host"].is_string()) {
                host_ = r["server_host"].get<std::string>();
            }

            if (r.contains("server_port") && r["server_port"].is_number_integer()) {
                port_ = r["server_port"].get<int>();
            }

            // İsteğe bağlı client (yerel) portu: remote.ports.client
            if (r.contains("ports") && r["ports"].is_object()) {
                const auto& p = r["ports"];
                if (p.contains("client") && p["client"].is_number_integer()) {
                    local_client_port = p["client"].get<int>();
                }
            }
        }
    } catch (...) {
        // defaults already set
    }

    if (host_.empty()) host_ = "127.0.0.1";
    if (port_ <= 0)    port_ = 5050;

    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        return -1;
    }

    // İstenen local client port'u varsa, connect öncesi bind et
    if (local_client_port > 0) {
        int opt = 1;
        ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in local {};
        local.sin_family      = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port        = htons(static_cast<uint16_t>(local_client_port));

        if (::bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0) {
            ::close(s);
            return -1;
        }
    }
    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(port_));
    if (::inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0) {
        ::close(s);
        return -1;
    }

    // non-blocking connect + basit bekleme
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(s);
        return -1;
    }

    // recv timeout: 1 sn
    timeval tv {};
    tv.tv_sec  = 1;
    tv.tv_usec = 0;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    return s;
}

void LineTcpClient::closeSocket()
{
    if (sock_ >= 0) {
        ::close(sock_);
        sock_ = -1;
    }
}

bool LineTcpClient::sendLine(const std::string& s)
{
    if (sock_ < 0) return false;
    std::string line = s;
    line.push_back('\n');

    const char* buf = line.data();
    size_t left = line.size();
    while (left > 0) {
        ssize_t n = ::send(sock_, buf, left, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        buf  += n;
        left -= static_cast<size_t>(n);
    }
    return true;
}

// --- thread / loop ---

void LineTcpClient::start()
{
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return; // zaten çalışıyor
    }
    thread_ = std::thread([this]() { this->loop(); });
}

void LineTcpClient::stop()
{
    bool expected = true;
    if (!running_.compare_exchange_strong(expected, false)) {
        return; // zaten durdu
    }
    closeSocket();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void LineTcpClient::loop()
{
    std::string buf;
    buf.reserve(4096);
    char tmp[1024];

    int last_seq = 0; // ileride sequence guard için kullanılabilir

    while (running_) {
        if (sock_ < 0) {
            sock_ = openSocket();
            if (sock_ < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(reconnect_ms_));
                continue;
            }
            last_seq = 0; // (re)connect
        }

        ssize_t n = ::recv(sock_, tmp, sizeof(tmp), 0);
        if (n == 0) {
            // Karşı taraf kapattı → yeniden bağlan
            closeSocket();
            std::this_thread::sleep_for(std::chrono::milliseconds(reconnect_ms_));
            continue;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            } else {
                closeSocket();
                std::this_thread::sleep_for(std::chrono::milliseconds(reconnect_ms_));
                continue;
            }
        }

        buf.append(tmp, tmp + n);

        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);

            std::string raw = trim_copy(line);
            if (raw.empty()) continue;
            if (raw.size() == 1 && (raw[0] == '\'' || raw[0] == '"')) continue;

            // --- V2 envelope detection & handling (client) ---
            try {
                json v2 = json::parse(raw, nullptr, false);
                if (v2.is_object()) {
                    std::string t = v2.value("type", "");
                    if (t == "response" || t == "event") {
                        // peer responses & events are ignored (şimdilik)
                        continue;
                    }
                    if (t == "request") {
                        std::string out;
                        if (dispatcher_) {
                            dispatcher_->dispatch(raw, out, guiCb_, host_, port_);
                        } else {
                            json er = {
                                {"type","response"},
                                {"action","unknown"},
                                {"status","error"},
                                {"error", {{"code","E_NO_DISPATCHER"},{"message","no dispatcher"}}}
                            };
                            out = er.dump();
                        }
                        sendLine(out);
                        continue;
                    }
                }
            } catch(...) {
                // parse hatası → alttaki genel hata zarfına düşecek
            }

            // V2 dışı her şey → tek tip hata
            {
                json er = {
                    {"type","response"},
                    {"action","unknown"},
                    {"status","error"},
                    {"error", {{"code","E_BAD_ENVELOPE"},{"message","v2_required"}}}
                };
                sendLine(er.dump());
                continue;
            }
        }
    }

    closeSocket();
}

// --- Tek atımlık RPC isteği ---
// Basit kullanım: Net::LineTcpClient cli("",0); cli.rpcRequest(...);

bool LineTcpClient::rpcRequest(const std::string& host,
                               int port,
                               const std::string& reqJson,
                               std::string& respJson,
                               int timeout_ms)
{
    respJson.clear();

    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return false;

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        ::close(s);
        return false;
    }

    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(s);
        return false;
    }

    // send request line
    std::string line = reqJson;
    line.push_back('\n');

    const char* buf = line.data();
    size_t left = line.size();
    while (left > 0) {
        ssize_t n = ::send(s, buf, left, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(s);
            return false;
        }
        if (n == 0) {
            ::close(s);
            return false;
        }
        buf  += n;
        left -= static_cast<size_t>(n);
    }

    // recv with timeout
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(s, &rfds);

    timeval tv {};
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int rv = ::select(s + 1, &rfds, nullptr, nullptr, &tv);
    if (rv <= 0) {
        ::close(s);
        return false;
    }

    std::string rbuf;
    rbuf.reserve(4096);
    char tmp[512];

    while (true) {
        ssize_t n = ::recv(s, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        rbuf.append(tmp, tmp + n);
        auto p = rbuf.find('\n');
        if (p != std::string::npos) {
            respJson = rbuf.substr(0, p);
            break;
        }
    }

    ::close(s);
    return !respJson.empty();
}

} // namespace Net
