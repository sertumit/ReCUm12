#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

// Not:
// - Bu sınıfın tam davranışı CommandDispatcher_Integration_Guide.md
//   ve ReCUm12-Architecture-and-Sprint-Plan-2025-11-30.md içindeki
//   v12.220.01 sprint kapsamına göre kademeli olarak eklenecektir.
// - Eski projeden gelen bazı komut örnekleri sadece referans amaçlı
//   yorum blokları olarak tutulacaktır (bkz. [PASIF] stili).

namespace Net {

class CommandDispatcher
{
public:
    using GuiCb = std::function<void(const std::string&)>;

    // Olay yayını için (örn. alert, info vb. JSON event satırları).
    using EventSink = std::function<void(const std::string& jline)>;

    // Log sorguları
    // - 'from' / 'to' alanları için beklenen tarih/zaman formatı:
    //     "gg.aa.yyyy - hh:mm"
    //   (örnek: "02.12.2025 - 07:30")
    //   veya boş string => ilgili sınır yok.    
    using GetLogsFn = std::function<std::string(
        const std::string& from,
        const std::string& to,
        int limit,
        const std::string& rfid,
        bool rfidEmpty,
        const std::string& plate,
        const std::vector<std::string>& logCodes)>;

    // RS-485 ham okuma
    using Rs485ReadRawFn = std::function<bool(std::string& outLine,
                                              int timeout_ms)>;

    // Kullanıcı yönetimi
    // - GetUsersFn'den dönen string, users.csv içeriği olarak
    //   kabul edilir ve CommandDispatcher tarafında JSON response
    //   payload'ı içinde "content" alanına gömülür.
    using GetUsersFn = std::function<std::string(void)>;
    using AddUserFn  = std::function<bool(
        int userId,int level,
        const std::string& first,const std::string& last,
        const std::string& plate,int limit,std::string& err)>;

    using UpdateUserFn = std::function<bool(
        int userId,int level,
        const std::string& first,const std::string& last,
        const std::string& plate,int limit,const std::string& rfid,
        std::string& err)>;

    using DeleteUserFn = std::function<bool(int userId,std::string& err)>;
    using SetUserRfidFn = std::function<bool(
        int userId,const std::string& rfidUpper,std::string& err)>;

public:
    CommandDispatcher() = default;
    ~CommandDispatcher() = default;

    CommandDispatcher(const CommandDispatcher&)            = delete;
    CommandDispatcher& operator=(const CommandDispatcher&) = delete;
    CommandDispatcher(CommandDispatcher&&)                 = delete;
    CommandDispatcher& operator=(CommandDispatcher&&)      = delete;

    // V2 JSON zarfına göre satır bazlı komut dispatch fonksiyonu.
    // - line      : Soketten gelen tek satır (JSON veya PLAIN).
    // - outJson   : Gönderilecek yanıt satırı (boş kalabilir).
    // - guiCb     : Opsiyonel GUI mesaj callback'i.
    // - remote_ip : Loglama için istemci IP bilgisi.
    // - remote_port: Loglama için istemci port bilgisi.
    void dispatch(const std::string& line,
                  std::string& outJson,
                  const GuiCb& guiCb,
                  const std::string& remote_ip = "NA",
                  int remote_port = 0);

    // Dolum koruması: filling=true iken riskli komutlar reddedilebilir.
    void setFilling(bool filling);

    // Olay yayın callback'i (opsiyonel).
    void setEventSink(EventSink sink);

    // Handler kayıt fonksiyonları (R1–R4 kapsamı)
    void setGetLogsHandler(GetLogsFn fn);
    void setRs485ReadRawHandler(Rs485ReadRawFn fn);

    void setGetUsersHandler(GetUsersFn fn);
    void setAddUserHandler(AddUserFn fn);
    void setUpdateUserHandler(UpdateUserFn fn);
    void setDeleteUserHandler(DeleteUserFn fn);
    void setSetUserRfidHandler(SetUserRfidFn fn);

    // TODO(v12.220.01-R1-R4):
    // - V2 JSON zarfına göre tam dispatch mantığı
    // - Filling guard ile action bazlı izin/ret politikası
    // - getFile, getLogs/logsQuery, fuelSum vb. komutların entegrasyonu
    // - Log / user / rs485 handler'larının çağrılacağı noktalar
    //
    // Örnek PASIF yorum stili (eski projeden referans alırken):
    //
    // [PASIF] Eski projede kullanılan "getStatus" komutu.
    // // dispatcher.registerHandler("getStatus", [this](...) {
    // //     return handlers_.getStatus();
    // // });

private:
    bool filling_{false};

    EventSink eventSink_;

    GetLogsFn       getLogsHandler_;
    Rs485ReadRawFn  rs485ReadRawHandler_;
    GetUsersFn      getUsersHandler_;
    AddUserFn       addUserHandler_;
    UpdateUserFn    updateUserHandler_;
    DeleteUserFn    deleteUserHandler_;
    SetUserRfidFn   setUserRfidHandler_;
};

} // namespace Net
