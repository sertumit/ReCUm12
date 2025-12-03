#pragma once
#include <functional>
#include <memory>
#include <atomic>
#include <thread>
#include <string>

// fwd decl
namespace Net { class CommandDispatcher; }

namespace Net {

class CommandDispatcher;

// Satır sonu '\n' çerçeveli, JSON (v2) istekleri okuyup yanıtlayan TCP **client**.
class LineTcpClient {
public:
    LineTcpClient(const std::string& host, int port);
    ~LineTcpClient();

    void start();
    void stop();
    bool isRunning() const { return running_.load(); }

    void setReconnectMs(int ms) { reconnect_ms_ = ms; }

    // Kısa GUI mesajları için callback (opsiyonel)
    void setGuiCallback(std::function<void(const std::string&)> cb) { guiCb_ = std::move(cb); }

    // dispatcher event'lerini upstream'e ilet
    void setDispatcher(std::shared_ptr<CommandDispatcher> d);

    // Tek atımlık RPC — geçici bağlantı açar, JSON satırı yollar,
    // timeout süresince ilk yanıt satırını okur ve döndürür.
    // Başarılıysa true ve respJson doludur; aksi halde false.
    bool rpcRequest(const std::string& host,
                    int port,
                    const std::string& reqJson,
                    std::string& respJson,
                    int timeout_ms = 1000);

private:
    void loop();
    int  openSocket();
    void closeSocket();
    bool sendLine(const std::string& s);

private:
    std::string host_;
    int         port_{0};
    int         sock_{-1};
    std::atomic<bool> running_{false};
    std::thread thread_;
    int         reconnect_ms_{3000};
    std::shared_ptr<CommandDispatcher> dispatcher_;
    std::function<void(const std::string&)> guiCb_;
};

} // namespace Net
