# LogManager – Central Logging Design & Usage

> Amaç: `users.csv` hariç tüm log dosyalarını (**usage**, **infra**, **fuel**) tek bir
> `core/LogManager.h / .cpp` seti üzerinden yönetmek; CMake tarafında tek bağımsız
> ve genişleyebilir log altyapısı oluşturmak.

---

## 1. Log Families & File Formats

### 1.1. Usage logs – `configs/logs.csv`

Bu dosya, pompa kullanım / transaction log’u tutar.

**CSV şeması (V2 R2)**

```text
processId,rfid,firstName,lastName,plate,limit,fuel,logCode,timeStamp,sendOk
```

Alanlar:

- `processId` : `int` – İşlemi tekil tanımlayan ID.
- `rfid`      : `std::string` – Kart / tag ID.
- `firstName` : `std::string` – Kullanıcı adı.
- `lastName`  : `std::string` – Kullanıcı soyadı.
- `plate`     : `std::string` – Araç plakası.
- `limit`     : `int`         – İzin verilen maksimum yakıt.
- `fuel`      : `double`      – Gerçek çekilen yakıt.
- `logCode`   : `std::string` – İşlem kodu (`PUMP_ON`, `PUMP_OFF`, `CANCEL`, vb.).
- `timeStamp` : `std::string` – ISO-8601 UTC (`2025-09-09T14:45:12Z`).
- `sendOk`    : `std::string` – `"Yes"`, `"No"` veya `"NA"`.

Geriye dönük uyum:

- Eski satırlarda `sendOk` kolonunun olmaması (9 kolon) durumunda, runtime’da
  `sendOk = "NA"` olarak kabul edilir.
- `LogManager` dosya yazarken header’ı her zaman 10 kolonlu yazar.

---

### 1.2. Infra logs – `logs/recumLogs.csv`

Bu dosya, uygulama / servis / network olaylarını tutar.

**CSV şeması**

```text
ts_iso,event,role,remote_ip,remote_port,mode,action,seq,msg,status
```

Alanlar:

- `ts_iso`      : ISO-8601 UTC timestamp.
- `event`       : `cmd_rx`, `cmd_tx`, `service_start`, `service_stop`,
                  `backup_ok`, `backup_fail`, `file_created`, ...
- `role`        : `client` | `server` | `NA`.
- `remote_ip`   : Karşı uç IP, boş olabilir.
- `remote_port` : TCP port, 0 olabilir.
- `mode`        : `net:server` | `net:client` | `offline` | `NA`.
- `action`      : Opsiyonel action adı.
- `seq`         : İstek sequence (yoksa `-1`).
- `msg`         : Kısa açıklama.
- `status`      : `OK` | `FAIL` | `NA`.

Notlar:

- Header satırı her zaman yazılır.
- String alanlar CSV içerisinde virgül, çift tırnak veya newline içeriyorsa,
  RFC tarzı `"..."` içinde tutulur ve `"` karakteri `""` olarak kaçışlanır.

---

### 1.3. Fuel logs – `configs/fuel.csv`

Bu dosya domain’e özgü; `LogManager` sadece **retention** sırasında dokunur.

Varsayılan varsayım:

- Son kolon tarih alanıdır (`F_TIMESTAMP` veya eşdeğeri).
- Tarih alanı ya ISO (`YYYY-MM-DD...`) ya da `DD.MM.YYYY` formatındadır.
- Retention işlemi, son kolonun tarihine göre satırları temizler.

Şema ayrıntıları domain kodunda tutulur; `LogManager` sadece şu garantiyi kullanır:

> Bu dosyada tarih son kolondadır; tek yaptığı, belirlenen cutoff tarihinden
> eski satırları silmek, tarihi parse edemediği satırları ise korumaktır.

---

### 1.4. users.csv – Hariç tutulan dosya

`users.csv` bu dokümanın kapsamı dışındadır:

- Kullanıcı / konfig tablosu olarak ele alınır, **log** değil.
- `LogManager` bu dosyaya dokunmaz, retention da yapmaz.

---

## 2. Public API – `core/LogManager.h`

Bu başlık, merkezi log yöneticisinin tek public arayüzüdür.

```cpp
#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <functional>

namespace Core {

class LogManager {
public:
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
    // - <appRoot>/configs/logs.csv (yoksa header yazar)
    // fuel.csv yalnızca retention'da kullanılır; scaffold zorunlu değildir.
    static bool ensureScaffold(const std::string& appRoot);

    // ------------------------------------------------------------------
    // 2) Usage logs: <appRoot>/configs/logs.csv
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
        std::string timeStamp; // ISO-8601 UTC (e.g. 2025-09-09T14:45:12Z)
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
    // Geriye dönük uyum:
    // - 9 kolonlu satırlarda sendOk yoksa 10. kolon olarak eklenir.
    // - Header 10 kolona normalize edilir.
    bool updateUsageSendOk(const std::string& appRoot,
                           int processId,
                           const std::string& timeStamp,
                           const std::string& sendOk);

    // ------------------------------------------------------------------
    // 3) Infra logs: <appRoot>/logs/recumLogs.csv
    // ------------------------------------------------------------------

    struct InfraEntry {
        std::string ts_iso;     // ISO-8601, UTC (boşsa runtime'da üretilebilir)
        std::string event;      // cmd_rx|cmd_tx|service_start|backup_ok|...
        std::string role;       // client|server|NA
        std::string remote_ip;  // boş olabilir
        int         remote_port = 0;
        std::string mode;       // net:server|net:client|offline|NA
        std::string action;     // opsiyonel action adı
        long        seq = -1;   // opsiyonel request seq
        std::string msg;        // kısa açıklama
        std::string status;     // OK|FAIL|NA
    };

    // logs/recumLogs.csv'ye tek satır ekler (append).
    bool appendInfra(const std::string& appRoot,
                     const InfraEntry& r);

    // recumLogs.csv'yi sorgular:
    // - fromISO/toISO: "YYYY-MM-DD" gün kesimi, boş bırakılabilir.
    // - kw3: en az 3 karakterlik case-insensitive keyword (satır tekstinde aranır).
    // Dönüş: header hariç, eşleşen ham CSV satırları.
    std::vector<std::string> queryInfra(const std::string& appRoot,
                                        const std::string& fromISO,
                                        const std::string& toISO,
                                        const std::string& kw3) const;

    // ------------------------------------------------------------------
    // 4) Retention: recumLogs + logs.csv + fuel.csv
    // ------------------------------------------------------------------

    // kind:
    //   "infra"      → logs/recumLogs.csv
    //   "usage"      → configs/logs.csv + configs/fuel.csv
    //   "logs"       → configs/logs.csv
    //   "fuel"       → configs/fuel.csv
    //   "all" veya ""→ hepsi
    //
    // days: Son kaç günü tutacağın (ör: 30 → bugün + 29 gün geriye).
    // outErr: Hata mesajı (fail durumunda doldurulur).
    bool runRetention(const std::string& appRoot,
                      const std::string& kind,
                      int days,
                      std::string& outErr) const;

private:
    // Usage cache + callback
    mutable std::mutex usageMtx_;
    std::vector<UsageEntry> usageRows_;
    UsageAppendCb onUsageAppended_{};
};

} // namespace Core
```

---

## 3. Implementation Skeleton – `core/LogManager.cpp`

Bu dosya, yukarıdaki arayüzün implementasyon iskeletidir.
İçindeki bloklar mevcut `core/LogManager.cpp` ve `utils/logs.cpp` davranışını temel alır.

```cpp
#include "core/LogManager.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <cstdlib>   // std::getenv
#include <system_error>

namespace Core {

// -----------------------------
// Local helpers (anon namespace)
// -----------------------------
namespace {

namespace fs = std::filesystem;

static std::string isoNowUTC() {
    using namespace std::chrono;
    auto now = system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return os.str();
}

// Simple trim helper
static std::string trim(std::string s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b-1]))) --b;
    return s.substr(a, b - a);
}

// CSV field helpers (recumLogs & retention için)
static std::string csv_first_field(const std::string& line) {
    auto p = line.find(',');
    return (p == std::string::npos) ? line : line.substr(0, p);
}
static std::string csv_last_field(const std::string& line) {
    auto p = line.rfind(',');
    return (p == std::string::npos) ? line : line.substr(p + 1);
}

// CSV escape helper (infra logs)
static void csvEscape(std::string& s) {
    if (s.find_first_of(",\"\n") != std::string::npos) {
        std::string out = "\"";
        for (char c : s) {
            out += (c == '\"') ? "\"\"" : std::string(1, c);
        }
        out += "\"";
        s.swap(out);
    }
}

// "YYYY-MM-DD" üret
static std::string today_ymd_utc() {
    using namespace std::chrono;
    auto now = system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{}; gmtime_r(&t, &tm);
    char buf[16]; std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return std::string(buf);
}

static bool parse_ymd(const std::string& s, int& Y,int& M,int& D) {
    if (s.size()!=10 || s[4]!='-'||s[7]!='-') return false;
    try {
        Y=std::stoi(s.substr(0,4));
        M=std::stoi(s.substr(5,2));
        D=std::stoi(s.substr(8,2));
    } catch (...) { return false; }
    return (Y>=1970 && M>=1 && M<=12 && D>=1 && D<=31);
}

static bool parse_ddmmyyyy(const std::string& s, int& Y,int& M,int& D) {
    if (s.size()!=10 || s[2]!='.'||s[5]!='.') return false;
    try {
        D=std::stoi(s.substr(0,2));
        M=std::stoi(s.substr(3,2));
        Y=std::stoi(s.substr(6,4));
    } catch (...) { return false; }
    return (Y>=1970 && M>=1 && M<=12 && D>=1 && D<=31);
}

// Retention: satır >= cutoffYMD ?
static bool line_is_on_or_after(const std::string& line,
                                const std::string& cutoffYMD)
{
    if (cutoffYMD.empty()) return true;
    // 1) ISO ts önde (recumLogs.csv)
    if (line.size()>=10 && line[4]=='-' && line[7]=='-') {
        return line.substr(0,10) >= cutoffYMD;
    }
    // 2) DD.MM.YYYY ara (logs.csv / fuel.csv ihtimali)
    auto p = line.find_first_of("0123456789");
    while (p!=std::string::npos && p+10<=line.size()) {
        std::string cand = line.substr(p, 10);
        int Y,M,D;
        if (parse_ddmmyyyy(cand, Y,M,D)) {
            char ymd[11]; std::snprintf(ymd, sizeof(ymd), "%04d-%02d-%02d", Y,M,D);
            return std::string(ymd) >= cutoffYMD;
        }
        p = line.find_first_of("0123456789", p+1);
    }
    // tarih bulunamadı → tut (korumacı davran)
    return true;
}

// Retention core: recumLogs.csv / logs.csv / fuel.csv üzerinde çalışır
static bool filter_csv_file_keep_last_days(const fs::path& inFile,
                                           const fs::path& outFile,
                                           int keepDays)
{
    std::ifstream in(inFile);
    if (!in.good()) return false;
    std::ofstream out(outFile, std::ios::trunc);
    if (!out.good()) return false;

    std::string header;
    // header varsa koru
    std::streampos pos0 = in.tellg();
    if (std::getline(in, header)) {
        if (header.find(',') != std::string::npos &&
            header.find("ts_iso") != std::string::npos) {
            out << header << "\n";
        } else if (header.find(',') != std::string::npos &&
                   header.find("F_TIMESTAMP") != std::string::npos) {
            out << header << "\n";
        } else if (header.find(',') != std::string::npos &&
                   header.find("F_PROCESS_ID") != std::string::npos) {
            out << header << "\n";
        } else {
            // veri satırı olabilir → geri sar
            in.clear(); in.seekg(pos0);
        }
    }

    const std::string today  = today_ymd_utc();
    const std::string cutoff = [&]{
        if (keepDays <= 0) return today;
        int Y,M,D; if (!parse_ymd(today, Y,M,D)) return today;
        std::tm tm{}; tm.tm_year=Y-1900; tm.tm_mon=M-1; tm.tm_mday=D;
        std::time_t t = timegm(&tm);
        t -= (std::time_t)(keepDays-1)*24*3600;
        std::tm outTm{}; gmtime_r(&t, &outTm);
        char buf[16]; std::strftime(buf, sizeof(buf), "%Y-%m-%d", &outTm);
        return std::string(buf);
    }();

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (line_is_on_or_after(line, cutoff)) {
            out << line << "\n";
        }
    }
    return true;
}

} // anon namespace

// ---------------------------------------------------------
// 1) Common appRoot & scaffold
// ---------------------------------------------------------

std::string LogManager::detectAppRoot() {
    // 1) ENV
    if (const char* e = std::getenv("RECUM_APPROOT")) {
        if (*e) return std::string(e);
    }
    // 2) Yukarı doğru "configs/default_settings.json" araması (max 5 seviye)
    fs::path p = fs::current_path();
    for (int i = 0; i < 5; ++i) {
        if (fs::exists(p / "configs" / "default_settings.json"))
            return p.string();
        if (!p.has_parent_path()) break;
        p = p.parent_path();
    }
    // 3) Fallback: current_path
    return fs::current_path().string();
}

bool LogManager::ensureScaffold(const std::string& appRoot) {
    try {
        const fs::path root(appRoot);

        // 1) logs/recumLogs.csv scaffold
        fs::create_directories(root / "logs");
        {
            const fs::path f = root / "logs" / "recumLogs.csv";
            if (!fs::exists(f)) {
                std::ofstream ofs(f, std::ios::binary | std::ios::app);
                if (!ofs) return false;
                ofs << "ts_iso,event,role,remote_ip,remote_port,mode,action,seq,msg,status\n";
                ofs << isoNowUTC()
                    << ",file_created,NA,NA,0,NA,NA,-1,created,OK\n";
            }
        }

        // 2) configs/logs.csv header (usage log)
        fs::create_directories(root / "configs");
        {
            const fs::path f = root / "configs" / "logs.csv";
            if (!fs::exists(f)) {
                std::ofstream ofs(f, std::ios::app);
                if (!ofs) return false;
                ofs << "processId,rfid,firstName,lastName,plate,limit,fuel,"
                       "logCode,timeStamp,sendOk\n";
            }
        }

        return true;
    } catch (...) {
        return false;
    }
}

// ---------------------------------------------------------
// 2) Usage logs
// ---------------------------------------------------------

void LogManager::setOnUsageAppended(UsageAppendCb cb) {
    std::lock_guard<std::mutex> lk(usageMtx_);
    onUsageAppended_ = std::move(cb);
}

bool LogManager::appendUsage(const std::string& appRoot,
                             const UsageEntry& e)
{
    const fs::path path = fs::path(appRoot) / "configs" / "logs.csv";

    {
        std::lock_guard<std::mutex> lk(usageMtx_);
        usageRows_.push_back(e);
    }

    const bool fileExists = static_cast<bool>(std::ifstream(path));
    std::ofstream ofs(path, std::ios::app);
    if (!ofs.is_open()) return false;

    if (!fileExists) {
        ofs << "processId,rfid,firstName,lastName,plate,limit,fuel,"
               "logCode,timeStamp,sendOk\n";
    }

    ofs << e.processId << ','
        << e.rfid      << ','
        << e.firstName << ','
        << e.lastName  << ','
        << e.plate     << ','
        << e.limit     << ','
        << e.fuel      << ','
        << e.logCode   << ','
        << e.timeStamp << ','
        << (e.sendOk.empty() ? "NA" : e.sendOk) << '\n';

    if (onUsageAppended_) {
        onUsageAppended_(e);
    }
    return true;
}

bool LogManager::loadUsage(const std::string& appRoot,
                           std::vector<UsageEntry>& out) const
{
    const fs::path path = fs::path(appRoot) / "configs" / "logs.csv";
    std::ifstream ifs(path);
    if (!ifs.is_open()) return false;

    std::vector<UsageEntry> tmp;
    std::string line;

    // Header'ı atla (varsa)
    if (std::getline(ifs, line)) {
        // opsiyonel: header kontrolü
    }

    while (std::getline(ifs, line)) {
        if (line.empty() || line == "...") continue;
        std::stringstream ss(line);
        std::string f[10];
        int readN = 0;
        for (int i = 0; i < 10; ++i) {
            if (!std::getline(ss, f[i], ',')) { f[0].clear(); break; }
            f[i] = trim(f[i]);
            ++readN;
        }
        if (f[0].empty()) continue;

        UsageEntry e{};
        try { e.processId = std::stoi(f[0]); } catch (...) { e.processId = 0; }
        e.rfid      = f[1];
        e.firstName = f[2];
        e.lastName  = f[3];
        e.plate     = f[4];
        try { e.limit = std::stoi(f[5]); } catch (...) { e.limit = 0; }
        try { e.fuel  = std::stod(f[6]); } catch (...) { e.fuel  = 0.0; }
        e.logCode   = f[7];
        e.timeStamp = f[8];
        e.sendOk    = (readN >= 10 && !f[9].empty()) ? f[9] : "NA";

        tmp.push_back(std::move(e));
    }

    {
        std::lock_guard<std::mutex> lk(usageMtx_);
        usageRows_ = tmp;
        out        = usageRows_;
    }
    return true;
}

bool LogManager::updateUsageSendOk(const std::string& appRoot,
                                   int keyProcId,
                                   const std::string& keyTs,
                                   const std::string& sendOkValue)
{
    const fs::path path    = fs::path(appRoot) / "configs" / "logs.csv";
    const std::string in   = path.string();
    const std::string tmp  = in + ".tmp";

    std::ifstream ifs(in);
    if (!ifs.is_open()) return false;
    std::ofstream ofs(tmp, std::ios::trunc);
    if (!ofs.is_open()) return false;

    const std::string keyPidStr = std::to_string(keyProcId);
    std::string line;
    bool first   = true;
    bool updated = false;

    std::lock_guard<std::mutex> lk(usageMtx_);

    while (std::getline(ifs, line)) {
        if (first) {
            first = false;
            // Header'ı normalize et: 10 kolonlu hale getir
            std::stringstream sh(line);
            std::vector<std::string> cols;
            std::string t;
            while (std::getline(sh, t, ',')) cols.push_back(trim(t));
            if (cols.size() < 10) {
                ofs << "processId,rfid,firstName,lastName,plate,limit,fuel,"
                       "logCode,timeStamp,sendOk\n";
            } else {
                ofs << line << "\n";
            }
            continue;
        }

        std::stringstream ss(line);
        std::vector<std::string> f;
        std::string cell;
        while (std::getline(ss, cell, ',')) f.push_back(trim(cell));
        if (f.empty()) { ofs << "\n"; continue; }

        // 9 kolonluysa sendOk ekle
        if (f.size() == 9) f.push_back("NA");
        if (f.size() < 10) { ofs << line << "\n"; continue; }

        // 0:processId, 8:timeStamp, 9:sendOk
        if (f[0] == keyPidStr && f[8] == keyTs) {
            f[9] = sendOkValue;
            updated = true;
        }

        for (size_t i = 0; i < f.size(); ++i) {
            if (i) ofs << ',';
            ofs << f[i];
        }
        ofs << "\n";
    }

    ofs.flush();
    ofs.close();
    ifs.close();

    std::error_code ec;
    fs::rename(tmp, in, ec);
    if (ec) {
        fs::remove(tmp);
        return false;
    }

    return updated;
}

// ---------------------------------------------------------
// 3) Infra logs – recumLogs.csv
// ---------------------------------------------------------

bool LogManager::appendInfra(const std::string& appRoot,
                             const InfraEntry& r)
{
    const fs::path path = fs::path(appRoot) / "logs" / "recumLogs.csv";
    std::ofstream ofs(path, std::ios::binary | std::ios::app);
    if (!ofs) return false;

    auto ts = r.ts_iso.empty() ? isoNowUTC() : r.ts_iso;

    auto writeField = [&](std::string v) {
        csvEscape(v);
        ofs << v;
    };

    writeField(ts);              ofs << ",";
    writeField(r.event);         ofs << ",";
    writeField(r.role);          ofs << ",";
    writeField(r.remote_ip);     ofs << ",";
    ofs << r.remote_port;        ofs << ",";
    writeField(r.mode);          ofs << ",";
    writeField(r.action);        ofs << ",";
    ofs << r.seq;                ofs << ",";
    writeField(r.msg);           ofs << ",";
    writeField(r.status);        ofs << "\n";

    return true;
}

std::vector<std::string> LogManager::queryInfra(const std::string& appRoot,
                                                const std::string& fromISO,
                                                const std::string& toISO,
                                                const std::string& kw3) const
{
    const fs::path path = fs::path(appRoot) / "logs" / "recumLogs.csv";
    std::vector<std::string> out;

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return out;

    auto date_on_or_after = [](const std::string& ts, const std::string& fromYMD){
        if (fromYMD.empty()) return true;
        if (ts.size() < 10)  return false;
        return ts.substr(0,10) >= fromYMD;
    };
    auto date_on_or_before = [](const std::string& ts, const std::string& toYMD){
        if (toYMD.empty()) return true;
        if (ts.size() < 10) return false;
        return ts.substr(0,10) <= toYMD;
    };
    auto ci_contains = [](std::string hay, std::string needle){
        if (needle.size() < 3) return true; // <3 → filtre yok
        std::transform(hay.begin(), hay.end(), hay.begin(),
                       [](unsigned char c){ return (char)std::tolower(c); });
        std::transform(needle.begin(), needle.end(), needle.begin(),
                       [](unsigned char c){ return (char)std::tolower(c); });
        return hay.find(needle) != std::string::npos;
    };

    std::string line;
    // header'ı atla
    if (!std::getline(ifs, line)) return out;

    while (std::getline(ifs, line)) {
        if (line.empty()) continue;
        std::string ts = line.substr(0, line.find(','));
        if (!date_on_or_after(ts, fromISO))  continue;
        if (!date_on_or_before(ts, toISO))   continue;
        if (!ci_contains(line, kw3))         continue;
        out.push_back(line);
    }
    return out;
}

// ---------------------------------------------------------
// 4) Retention – recumLogs + logs.csv + fuel.csv
// ---------------------------------------------------------

bool LogManager::runRetention(const std::string& appRoot,
                              const std::string& kind,
                              int days,
                              std::string& outErr) const
{
    try {
        const fs::path root(appRoot);
        const fs::path recum = root / "logs"    / "recumLogs.csv";
        const fs::path logs  = root / "configs" / "logs.csv";
        const fs::path fuel  = root / "configs" / "fuel.csv";

        const std::string k = (kind.empty() ? "all" : kind);
        auto run_one = [&](const fs::path& f)->bool {
            if (!fs::exists(f)) return true;
            fs::path tmp = f; tmp += ".tmp";
            if (!filter_csv_file_keep_last_days(f, tmp, days)) {
                outErr = "filter failed: " + f.string();
                return false;
            }
            std::error_code ec;
            fs::rename(tmp, f, ec);
            if (ec) {
                outErr = "rename failed: " + f.string();
                return false;
            }
            return true;
        };

        if (k == "infra") return run_one(recum);
        if (k == "usage") return run_one(logs) && run_one(fuel);
        if (k == "logs")  return run_one(logs);
        if (k == "fuel")  return run_one(fuel);

        // "all" veya bilinmeyen → hepsi
        return run_one(recum) && run_one(logs) && run_one(fuel);
    } catch (const std::exception& e) {
        outErr = e.what();
        return false;
    }
}

} // namespace Core
```

---

## 4. Usage Examples

### 4.1. App startup

```cpp
#include "core/LogManager.h"

int main() {
    std::string appRoot = Core::LogManager::detectAppRoot();
    Core::LogManager::ensureScaffold(appRoot);

    Core::LogManager lm;

    // Infra: service_start
    Core::LogManager::InfraEntry ev;
    ev.event  = "service_start";
    ev.role   = "server";
    ev.mode   = "offline";
    ev.msg    = "recum service started";
    ev.status = "OK";
    lm.appendInfra(appRoot, ev);

    // ...
}
```

### 4.2. Usage log – pump OFF transaction

```cpp
void on_pump_off(Core::LogManager& lm,
                 const std::string& appRoot,
                 int processId,
                 const std::string& rfid,
                 const std::string& first,
                 const std::string& last,
                 const std::string& plate,
                 int limit,
                 double fuel,
                 const std::string& ts_iso)
{
    Core::LogManager::UsageEntry e;
    e.processId = processId;
    e.rfid      = rfid;
    e.firstName = first;
    e.lastName  = last;
    e.plate     = plate;
    e.limit     = limit;
    e.fuel      = fuel;
    e.logCode   = "PUMP_OFF";
    e.timeStamp = ts_iso;
    e.sendOk    = "NA"; // ilk yazımda genelde NA

    lm.appendUsage(appRoot, e);
}
```

Gönderim sonrası `sendOk` güncelleme:

```cpp
bool mark_usage_sent(Core::LogManager& lm,
                     const std::string& appRoot,
                     int processId,
                     const std::string& ts_iso)
{
    return lm.updateUsageSendOk(appRoot, processId, ts_iso, "Yes");
}
```

### 4.3. Infra log – command RX

```cpp
void log_cmd_rx(Core::LogManager& lm,
                const std::string& appRoot,
                const std::string& fromIp,
                int fromPort,
                long seq,
                const std::string& msg)
{
    Core::LogManager::InfraEntry r;
    r.event       = "cmd_rx";
    r.role        = "server";
    r.remote_ip   = fromIp;
    r.remote_port = fromPort;
    r.mode        = "net:server";
    r.seq         = seq;
    r.msg         = msg;
    r.status      = "OK";

    lm.appendInfra(appRoot, r);
}
```

### 4.4. Retention – cron job

```cpp
void cron_retention(Core::LogManager& lm,
                    const std::string& appRoot)
{
    std::string err;
    if (!lm.runRetention(appRoot, "all", 30, err)) {
        std::cerr << "Retention failed: " << err << "\n";
    }
}
```

---

## 5. Migration Plan

1. **Yeni `core/LogManager.h` ile mevcut olanı değiştir**  
   - Mevcut `LogEntry` → `UsageEntry` içine gömülmüş durumda.
   - Eski `appendLog/loadLogs/updateSendOk` imzalarına denk gelen
     `appendUsage/loadUsage/updateUsageSendOk` fonksiyonları var.

2. **`core/LogManager.cpp`’yi yukarıdaki iskelete göre doldur**  
   - Usage fonksiyonları için mevcut `appendLog`, `appendLogLine`,
     `loadLogs`, `updateSendOk` kodlarını kullan.
   - Infra fonksiyonları ve retention için `utils/logs.cpp` içeriğini taşı.

3. **`utils/logs.h/.cpp` için**  
   - Kademeli geçişte istersen eski API’yi, yeni `Core::LogManager`
     fonksiyonlarına ince wrapper olarak bırakabilirsin.
   - Stabil hale geldikten sonra bu wrapper’ları ve dosyayı tamamen
     kaldırabilirsin.

4. **Çağrı noktalarını güncelle**  
   - `Core::LogManager::appendLog` → `appendUsage`
   - `Core::LogManager::loadLogs` → `loadUsage`
   - `Core::LogManager::updateSendOk` → `updateUsageSendOk`
   - `recum::logs::detectAppRoot` → `Core::LogManager::detectAppRoot`
   - `recum::logs::ensureLogsScaffold` → `Core::LogManager::ensureScaffold`
   - `recum::logs::logEvent` → `Core::LogManager::appendInfra`
   - `recum::logs::queryRecumLogs` → `Core::LogManager::queryInfra`
   - `recum::logs::runRetention` → `Core::LogManager::runRetention`

5. **CMake sadeleştirme**  
   - Log altyapısı için tek hedef:
     - `core/LogManager.h`
     - `core/LogManager.cpp`
   - `utils/logs.*` hedeflerden kaldırılabilir (geri uyum wrapper’ları
     bitince tamamen silinebilir).

Bu adımlar tamamlandığında:

- `users.csv` dışındaki tüm log dosyaları için tek merkez:
  **`Core::LogManager`** olur.
- Log formatları korunur, geri uyumlu davranış bozulmaz.
- Yeni log family eklemek (örneğin `audit.csv`) sadece yeni bir
  `struct` + küçük append/load fonksiyonları eklemek anlamına gelir.
