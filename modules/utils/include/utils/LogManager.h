#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace recum12::utils {

class LogManager {
public:
    LogManager() = default;

    // ------------------------------------------------------------------
    // 1) Common appRoot & scaffold
    // ------------------------------------------------------------------

    // Uygulama kök klasörünü tespit eder:
    // 1) $RECUM_APPROOT varsa onu kullanır
    // 2) current_path()'ten yukarı doğru "configs/default_settings.json"
    //    arar (maks 5 seviye)
    // 3) bulunamazsa current_path() döner
    static std::string detectAppRoot();

    // Zorunlu klasör ve dosyaları oluşturur:
    // - <appRoot>/logs/recumLogs.csv
    // - <appRoot>/logs/log_user/logs.csv (yoksa header yazar)
    // fuel.csv yalnızca retention'da kullanılır; scaffold zorunlu değildir.
    static bool ensureScaffold(const std::string& appRoot);

    // ------------------------------------------------------------------
    // 2) Usage logs: <appRoot>/logs/log_user/logs.csv
    // ------------------------------------------------------------------

    struct UsageEntry {
        // Şema sırası:
        // processId,rfid,firstName,lastName,plate,limit,fuel,logCode,timeStamp,sendOk
        int         processId{0};
        std::string rfid;
        std::string firstName;
        std::string lastName;
        std::string plate;
        int         limit{0};
        double      fuel{0.0};
        std::string logCode;
        std::string timeStamp; // ISO-8601 UTC
        std::string sendOk;    // "Yes" | "No" | "NA"
    };

    using UsageAppendCb = std::function<void(const UsageEntry&)>;

    // Yeni bir log satırı eklendiğinde çalışacak opsiyonel callback.
    void setOnUsageAppended(UsageAppendCb cb);

    // Bellek cache'ine ekler + configs/logs.csv dosyasına tek satır append eder.
    bool appendUsage(const std::string& appRoot, const UsageEntry& e);

    // Tüm usage loglarını configs/logs.csv'den okur ve out'a doldurur.
    // Eski 9 kolonlu satırlarda sendOk alanını "NA" kabul eder.
    bool loadUsage(const std::string& appRoot,
                   std::vector<UsageEntry>& out) const;

    // Belirli bir satırda (processId + timeStamp eşleşmesi) sendOk alanını günceller.
    // 9 kolonlu satırlarda sendOk yoksa 10. kolon olarak eklenir.
    bool updateUsageSendOk(const std::string& appRoot,
                           int processId,
                           const std::string& timeStamp,
                           const std::string& sendOk);

private:
    // Usage cache + callback
    mutable std::mutex usageMtx_;
    mutable std::vector<UsageEntry> usageRows_;
    UsageAppendCb onUsageAppended_{};
};

} // namespace recum12::utils