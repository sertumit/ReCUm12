# CommandDispatcher Entegrasyon ve Geliştirme Kılavuzu

Bu doküman, `modules/net` altındaki **`Net::CommandDispatcher`** bileşenini bağımsız bir C++/CMake projesine entegre etmek için hazırlanmıştır. Hedef, TCP/IP üzerinden JSON tabanlı istek/yanıt protokolünü çalıştıran, genişleyebilir bir komut sunucusudur.

Aşağıdaki tasarım:

- Mevcut **V2 genel zarf şeması** ile uyumludur (`type` / `action` / `payload` / `msgId`).  
- `NetworkManager` entegrasyon kılavuzundaki stil ve klasör yapısını referans alır.  
- Komut listesindeki **`!!PASİF` işaretli** komutların derleme dışında bırakılmasını önerir.  
- Ek bir **dosya transfer komutu** (`getFile`) tanımlar: `.csv`, `.txt`, `.log` dosyalarını gönderebilir.

---

## 1. Genel Mimari

### 1.1. Rol ve Sorumluluklar

`CommandDispatcher`:

- TCP üzerinden gelen **satır bazlı JSON** istekleri parse eder.
- İstek zarfını doğrular: `type=request`, `action=<KomutAdı>`.
- `action` alanına göre ilgili **handler callback**’ini çağırır (örn. `setPulse`, `getStatus`, `getLogs`, `getUsers`, …).
- Ürettiği yanıtı yine JSON olarak döndürür.
- İsteğe bağlı olarak:
  - GUI’ye metin mesajları göndermek için `GuiCb` kullanır.
  - Olay yayını için `eventSink` kullanır (ör. `alert` event’i).
  - `filling` bayrağı ile “dolum sırasında bazı komutları reddetme” koruması uygular.

Dış katman (ör. `TcpServer` / `LineTcpClient`) sadece şunları yapar:

1. Soketten bir satır okur (`
` ile biten JSON istek).
2. `CommandDispatcher::dispatch(...)` çağırır.
3. Üretilen `outJson` satırını tekrar sokete yazar.

### 1.2. Bağımlılıklar (özet)

Zorunlu:

- C++17 önerilir (en az C++14).  
- Standart kütüphane: `<string>`, `<vector>`, `<functional>`, `<memory>`, `<map>`, `<chrono>`, `<thread>`, `<fstream>`, `<filesystem>`, `<optional>` vb.
- **nlohmann::json** (tek header kullanan JSON kütüphanesi).
- POSIX fonksiyonları (Linux hedefi varsayılmış): `stat`, `time`, `getifaddrs`, `inet_ntop`, `popen`, vb.

Opsiyonel (örnek uygulamadaki gibi taşımak isterseniz):

- `recum::logs` (olay logu / `recumLogs.csv` yönetimi).
- `recum::backup` (yedek alma).
- `Core::LogManager` (işlem loglarında `sendOk` güncellemesi).
- `Utils::sumFuelLogsRange` (yakıt toplamı hesaplama).

Bu tür bağımlılıkları taşımak istemiyorsanız, ilgili fonksiyon çağrılarını:

- Ya kendi logging / backup katmanınızla değiştirin,
- Ya da `#if 0 ... #endif` ile geçici olarak devre dışı bırakın.

---

## 2. Public API Referansı

### 2.1. Namespace ve `PulseParams`

```cpp
namespace Net {

struct PulseParams {
    int    debounce_ms     = -1;
    int    pulses_per_unit = -1;
    double max_level       = -1.0;
    int    sim_period_ms   = -1;
    int    sim_width_ms    = -1;
    std::string edge;   // "rising" / "falling" / "" (boş = değiştirme)
    bool persist = false;
};
```

`PulseParams`, uzaktan gönderilen **SET_PULSE / setPulse** isteğinin cihaz tarafında yorumlanmış halidir:

- `debounce_ms`   : Darbe gürültü filtre süresi (ms).  
- `pulses_per_unit` / `k_factor_pl` köprüsü: 1 litre başına darbe sayısı.  
- `max_level`     : Ölçüm üst sınırı.  
- `sim_period_ms` / `sim_width_ms` : Test/simülasyon sinyali için darbe parametreleri.  
- `edge`          : `rising` veya `falling`.  
- `persist`       : `true` ise ayarların kalıcı (`settings.json`) yazılması beklenebilir.

### 2.2. Handler Tipleri

```cpp
class CommandDispatcher {
public:
    using GuiCb = std::function<void(const std::string&)>;

    using SetPulseFn  = std::function<bool(const PulseParams&)>;
    using GetStatusFn = std::function<std::string(void)>;

    using GetLogsFn = std::function<std::string(
        const std::string& from,
        const std::string& to,
        int limit,
        const std::string& rfid,
        bool rfidEmpty,
        const std::string& plate,
        const std::vector<std::string>& logCodes)>;

    using SetStatsFn  = std::function<bool(int sum_days, bool persist)>;
    using SetRemoteFn = std::function<bool(bool disabled)>;
    using GetIpsFn    = std::function<std::string(void)>;

    using GetSafetyFn = std::function<std::string(void)>;
    using PumpCtrlFn  = std::function<bool(const std::string& action,
                                           int duration_ms)>;

    // RS-485 ham okuma
    using Rs485ReadRawFn = std::function<bool(std::string& outLine,
                                              int timeout_ms)>;

    // Kullanıcı yönetimi
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
```

Her handler, tek bir komut türüne karşılık gelir:

- Örn. `setGetUsersHandler(GetUsersFn)` çağrısı yapıldıysa, gelen `"action":"getUsers"` istekleri bu fonksiyona yönlendirilir.
- Handler set edilmemişse, ilgili komut için **“not supported”** tipinde hata dönülmesi tavsiye edilir (mevcut implementasyon bazı komutlarda bunu yapıyor).

### 2.3. Temel Metodlar

#### 2.3.1. `dispatch`

```cpp
void CommandDispatcher::dispatch(const std::string& line,
                                 std::string& outJson,
                                 const GuiCb& guiCb,
                                 const std::string& remote_ip = "NA",
                                 int remote_port = 0);
```

- **Girdi**:  
  - `line` : Soketten gelen tek satır metin. PLAIN `"PING"` veya JSON istek olabilir.  
  - `guiCb`: İsteğe bağlı GUI mesaj callback’i. Örn. `guiCb("Kullanıma kapalı")`.  
  - `remote_ip` / `remote_port`: Loglama için istemci IP/port bilgisi.

- **Çıktı**:  
  - `outJson`: Gönderilecek JSON yanıt satırı. Boş bırakılırsa, üst katman sokete yazmayabilir.

Davranış (özet):

1. Gelen satırı loglar (`cmd_rx_raw` vb.).  
2. `"PING"` / `"ping"` için basit `{ "ok": true, "pong": 1 }` döner.  
3. JSON parse eder; `type` / `action` alanlarını okur.  
4. Genel zarf doğrulamasını yapar (`validate_v2_envelope`).  
5. `action` değerine göre ilgili blok/handler çalışır.  
6. Üretilen cevap da loglanarak `outJson`’a yazılır.

#### 2.3.2. Event hattı

```cpp
void setEventSink(std::function<void(const std::string&)> sink);
void publishEvent(const std::string& eventName,
                  const std::string& payloadJson);
```

- `setEventSink` ile NDJSON tarzı event hattı verilir.  
- `publishEvent`, `{"type":"event","action":eventName,"data":<payload>,"ts":...}` zarfını üretip `sink`’e yollar.  
- Mevcut kod, özellikle `"alert"` event’i için kullanır (`emitAlert`, `emitAlertTest`).

#### 2.3.3. Filling Guard

```cpp
void setFilling(bool b);
bool isFilling() const;
```

- `filling_=true` iken yalnızca belirli info komutlarına izin verilir (`handshake`, `getStatus`, `safetyStatus`, `getInfo` gibi).  
- Diğer komutlar için `"E_BUSY", "filling"` hatası üretilir.

---

## 3. Komut Seti ve `!!PASİF` Yönetimi

### 3.1. Genel V2 Zarf Şeması

İstek:

```json
{"type":"request","action":"<KomutAdı>","payload":{},"msgId":"<opsiyonel>"}
```

Yanıt:

```json
{"type":"response","action":"<KomutAdı>",
 "status":"success|error",
 "data":{},
 "error":{"code":"E_...","message":"..."},
 "msgId":"<echo>"}
```

### 3.2. Komut Özeti ve Handler Eşlemesi

Aşağıdaki tablo, **aktif** komutların özetini ve hangi handler ile eşleştiğini gösterir. `!!PASİF` işaretli olanlar, yeni projede **devre dışı bırakılması** önerilen komutlardır.

| Komut        | Tip       | Durum   | Açıklama (özet)                             | Handler / Dahili |
|-------------|-----------|---------|---------------------------------------------|------------------|
| `handshake` | info      | PASİF   | Cihaz yetenek listesi                       | Dahili JSON      |
| `getInfo`   | info      | PASİF   | Özet istatistik / durum                     | Dahili           |
| `getNetInfo`| info      | PASİF   | Arayüz & gateway listesi                    | Dahili           |
| `getHwInfo` | info      | AKTİF   | Donanım/OS/rol/port/uzak IP bilgisi         | Dahili           |
| `getLogs`   | data      | AKTİF   | İşlem logları (CSV)                         | `GetLogsFn`      |
| `logsQuery` | data      | AKTİF   | Sistem olay logları (recumLogs)             | Dahili           |
| `getStatus` | info      | PASİF   | Genel sağlık özeti                          | `GetStatusFn`    |
| `setStats`  | control   | PASİF   | İstatistik gün sayısı ayarı                 | `SetStatsFn`     |
| `setRemote` | control   | PASİF   | Uzak rol/host/port ayarları                 | `SetRemoteFn`    |
| `getIps`    | info      | PASİF   | Kısa IP listesi                             | `GetIpsFn`       |
| `safetyStatus`| info    | PASİF   | Güvenlik/durum bayrakları                   | `GetSafetyFn`    |
| `pumpCtrl`  | control   | PASİF   | Pompa kontrol (on/off/ack)                  | `PumpCtrlFn`     |
| `setPulse`  | control   | PASİF   | Pulser çalışma parametreleri                | `SetPulseFn`     |
| `getUsers`  | data      | AKTİF   | Kullanıcı listesi                           | `GetUsersFn`     |
| `addUser`   | control   | AKTİF   | Kullanıcı ekleme                            | `AddUserFn`      |
| `updateUser`| control   | AKTİF   | Kullanıcı güncelleme                        | `UpdateUserFn`   |
| `deleteUser`| control   | AKTİF   | Kullanıcı silme                             | `DeleteUserFn`   |
| `setUserRfid`| control  | AKTİF   | Kullanıcıya RFID atama                      | `SetUserRfidFn`  |
| `backupNow` | maint.    | PASİF   | Anında yedekleme                            | Dahili / backup  |
| `setUsagePendingRetryHours` | control | PASİF | Bekleyen ack tekrar deneme periyodu | Dahili           |
| `retryPendingNow` | maint. | PASİF | Bekleyen ack kayıtlarını yeniden dene       | Dahili           |
| `getSettings` | config  | PASİF   | Ayarları JSON olarak okur                   | Dahili           |
| `setSettings` | config  | PASİF   | Ayarları whitelist ile günceller            | Dahili           |
| `pumpOffAck` | ack      | PASİF   | Basit success, client-ACK için              | Dahili           |
| `sendIpMail` | maint.   | PASİF   | IP bilgisi mail gönderme                    | Dahili           |
| `fuelSum`    | info      | PASİF  | Yakıt toplamı                               | Dahili / `fuel_sum` |
| `rs485.readRaw` | info  | AKTİF   | RS-485 ham satır oku                        | `Rs485ReadRawFn` |
| `getFile`   | data      | YENİ    | Metin dosyalarını (csv/txt/log) gönderir    | Yeni handler     |

> Not: Tablo, CommandSet ve örnek implementasyondan türetilmiş bir özettir; yeni projede yalnızca ihtiyaç duyduğunuz komutlar aktif edilir.

### 3.3. `!!PASİF` Komutların Derleme Dışında Bırakılması

Yeni projede **aktif olmayacak** komutları iki seviyede pasif hale getirebilirsiniz:

1. **API Seviyesi**:  
   - İlgili handler’ları hiç set etmeyin (ör. `setSetStatsHandler` çağırmayın).

2. **Derleme Seviyesi** (önerilen, net davranış için):  
   - `dispatch` içindeki ilgili `if (v2action == "...") { ... }` bloklarını `#if 0 ... #endif` içine alın veya tamamen kaldırın.

Örnek şablon:

```cpp
// --- GETSTATUS (V2) ---
#if 0   // !!PASİF: bu komut yeni projede kullanılmıyor
if (act == "GETSTATUS") {
    // mevcut implementasyon
}
#endif
```

Böylece, protokolde komut ismi görünse bile cihaz bu komuta **cevap üretmez**; isterseniz üst katmanda “desteklenmiyor” cevabını siz oluşturabilirsiniz.

---

## 4. Yeni Komut Tasarımı: `getFile` (CSV / TXT / LOG)

### 4.1. Amaç

- Cihaz tarafındaki belirli klasörlerde bulunan **metin tabanlı** dosyaları (`.csv`, `.txt`, `.log`) uzaktan okumak.  
- Genel V2 zarfı ile uyumlu, basit ve genişleyebilir bir şema sağlamak.

### 4.2. Önerilen V2 Şeması

**İstek:**

```json
{
  "type": "request",
  "action": "getFile",
  "payload": {
    "kind": "csv",       // "csv" | "txt" | "log"
    "name": "logs.csv",  // dosya adı (beyaz liste)
    "maxBytes": 65536    // opsiyonel, varsayılan: 65536
  },
  "msgId": "gf-1"
}
```

**Başarılı yanıt:**

```json
{
  "type": "response",
  "action": "getFile",
  "status": "success",
  "data": {
    "name": "logs.csv",
    "size": 12345,
    "mime": "text/csv",
    "encoding": "base64",
    "content": "<BASE64-STRING>"
  },
  "msgId": "gf-1"
}
```

**Hata yanıtı (örnek):**

```json
{
  "type": "response",
  "action": "getFile",
  "status": "error",
  "error": {
    "code": "E_BAD_PAYLOAD",
    "message": "unsupported kind"
  },
  "msgId": "gf-1"
}
```

### 4.3. Dosya Yolu Çözümleme Kuralları

`NetworkManager` / mevcut kodun tarzına uyum için aşağıdaki strateji önerilir:

- Yalnızca **beyaz listelenmiş dizinlerden** okuma yapın:
  - `PROJECT_ROOT/configs/`
  - `PROJECT_ROOT/logs/`
- `kind` alanına göre alt klasör mapping’i yapılabilir:
  - `csv` → `configs/` veya `logs/`
  - `txt` → `configs/`
  - `log` → `logs/`

Örnek pseudo kod:

```cpp
static std::filesystem::path resolve_file_path(const std::string& kind,
                                               const std::string& name) {
    namespace fs = std::filesystem;
    fs::path base = fs::path(PROJECT_ROOT);

    fs::path rel;
    if (kind == "csv")      rel = "configs";
    else if (kind == "txt") rel = "configs";
    else if (kind == "log") rel = "logs";
    else throw std::runtime_error("unsupported kind");

    // Güvenlik: alt dizin kaçışı önlenmeli
    fs::path fname = fs::path(name).filename();
    return base / rel / fname;
}
```

### 4.4. `CommandDispatcher` İçinde `getFile` Bloğu

Yeni komut için `dispatch` içinde aşağıdaki gibi bir blok eklenebilir (taslak):

```cpp
if (v2action == "getFile") {
    const auto& pl = (j.contains("payload") && j["payload"].is_object())
                     ? j["payload"] : nlohmann::json::object();

    std::string kind = pl.value("kind", std::string{});
    std::string name = pl.value("name", std::string{});
    std::size_t maxBytes = pl.value("maxBytes", (int)65536);
    if (maxBytes == 0) maxBytes = 65536;

    if (kind.empty() || name.empty()) {
        outJson = make_v2_error(v2action, msgId, "E_BAD_PAYLOAD",
                                "kind/name required").dump();
        log_cmd_tx("getFile", outJson, remote_ip, remote_port);
        return;
    }

    try {
        auto path = resolve_file_path(kind, name);
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            outJson = make_v2_error(v2action, msgId, "E_NOT_FOUND",
                                    "file not found").dump();
            log_cmd_tx("getFile", outJson, remote_ip, remote_port);
            return;
        }

        std::string buf;
        buf.resize(maxBytes);
        in.read(&buf[0], (std::streamsize)maxBytes);
        std::streamsize got = in.gcount();
        buf.resize((std::size_t)got);

        std::string b64 = base64_encode(buf); // kendi helper'ınız

        nlohmann::json data{
            {"name", name},
            {"size", (long long)got},
            {"mime", (kind=="csv" ? "text/csv" : "text/plain")},
            {"encoding", "base64"},
            {"content", b64}
        };
        nlohmann::json reply{
            {"type","response"},
            {"action","getFile"},
            {"status","success"},
            {"data", data}
        };
        if (!msgId.empty()) reply["msgId"] = msgId;
        outJson = reply.dump();
        log_cmd_tx("getFile", outJson, remote_ip, remote_port);
        return;
    } catch (const std::exception& e) {
        outJson = make_v2_error(v2action, msgId, "E_IO", e.what()).dump();
        log_cmd_tx("getFile", outJson, remote_ip, remote_port);
        return;
    }
}
```

> Not: `base64_encode` fonksiyonunu kendi projenizde tanımlamanız gerekir (örneğin RFC 4648 standart implementasyonu).

---

## 5. TCP/IP Katmanı Entegrasyonu

### 5.1. Önerilen Klasör Yapısı (Yeni Proje)

```text
project_root/
  modules/
    net/
      include/
        net/
          CommandDispatcher.h
          TcpServer.h        // satır bazlı TCP server
          LineTcpClient.h    // opsiyonel client
      src/
        CommandDispatcher.cpp
        TcpServer.cpp
        LineTcpClient.cpp
```

`NetworkManager` için kullanılan yapıya benzer şekilde, `modules/net` dizinini yeni projeye taşıyabilirsiniz.

### 5.2. Basit Sunucu Örneği

```cpp
#include "net/CommandDispatcher.h"
#include "net/TcpServer.h"

class MyServer {
public:
    MyServer()
    : dispatcher_(std::make_shared<Net::CommandDispatcher>()),
      server_(5051) // örnek port
    {
        server_.setOnLine([this](const std::string& line,
                                 std::string& out,
                                 const std::string& ip,
                                 int port) {
            dispatcher_->dispatch(line, out,
                                  nullptr, // GUI callback yok
                                  ip, port);
        });

        // İhtiyaç duyulan handler’ları burada set edin:
        // dispatcher_->setGetStatusHandler(...);
        // dispatcher_->setGetLogsHandler(...);
        // dispatcher_->setRs485ReadRawHandler(...);
    }

    void start() { server_.start(); }
    void stop()  { server_.stop(); }

private:
    std::shared_ptr<Net::CommandDispatcher> dispatcher_;
    Net::TcpServer server_;
};
```

Bu yapı ile:

- TCP bağlantıları `TcpServer` tarafından yönetilir.
- Her gelen satır `CommandDispatcher::dispatch` ile işlenir.
- Cevap satırı sokete yazılır.

---

## 6. CMake Entegrasyon Adımları

### 6.1. Modül Kütüphanesi Tanımı

`modules/net/CMakeLists.txt` örneği:

```cmake
add_library(net STATIC
    src/CommandDispatcher.cpp
    src/TcpServer.cpp
    src/LineTcpClient.cpp
)

target_include_directories(net
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# JSON ve diğer bağımlılıklar (örnek)
target_link_libraries(net
    PUBLIC
        nlohmann_json::nlohmann_json
)
```

### 6.2. PROJECT_ROOT Tanımı

`CommandDispatcher` ve ilişkili kodlarda `PROJECT_ROOT` makrosu kullanılır (ayar / log dosya yolları için).

Ana `CMakeLists.txt` içinde, çalıştırılabilir program için (veya kütüphane için) aşağıdaki gibi tanımlayın:

```cmake
add_executable(my_app
    src/main.cpp
)

target_link_libraries(my_app
    PRIVATE net
)

# PROJECT_ROOT derleme zamanı sabiti
target_compile_definitions(my_app
    PRIVATE PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
)
```

Gerekiyorsa, `APP_VERSION`, `GIT_COMMIT_SHORT`, `DEVICE_ID` gibi makroları da benzer şekilde geçebilirsiniz.

### 6.3. Uygulama Tarafında Kullanım

```cpp
#include "net/CommandDispatcher.h"
#include "net/TcpServer.h"
#include "comm/NetworkManager.h"   // NetworkManager entegrasyonu

int main() {
    Comm::NetworkManager nm;
    if (!nm.isEthernetConnected() && !nm.isWifiConnected()) {
        // Ağ yok; isterseniz bekleyin veya log atın
    }

    MyServer srv;
    srv.start();

    // basit blok (örnek)
    for (;;)
        std::this_thread::sleep_for(std::chrono::seconds(1));

    return 0;
}
```

Burada `NetworkManager`, sadece fiziksel bağlantı durumunu kontrol etmek için kullanılır; `CommandDispatcher` tamamen JSON/TCP projeksiyonu ile çalışır.

---

## 7. Genişletme ve Uyarlama Önerileri

1. **Modüler Handler Kayıt Paneli**  
   - Uygulama başlangıcında tek bir fonksiyonda tüm handler set’lerini yapın (`register_handlers(dispatcher)`), böylece hangi komutların aktif olduğu tek yerden görülebilsin.

2. **Yetkilendirme Katmanı**  
   - `dispatch`’ten önce veya sonra, `action`’a göre basit rol tabanlı izleme/dokunma ekleyebilirsiniz (ör. sadece admin kullanıcı `getFile` çağırabilsin).

3. **Dosya Transfer Limitleri**  
   - `getFile` için `maxBytes` zorunlu kılınabilir, büyük dosyalarda paging veya parça parça okuma tasarlanabilir (örn. `offset` parametresi).

4. **Pasif Komutlar İçin Dokümantasyon Eki**  
   - Repo içinde `docs/CommandSet_v12-PASSIVE.md` benzeri bir dosyada **neden pasif** oldukları ve ileride nasıl etkinleştirilebilecekleri açıklanabilir.

Bu kılavuz, `CommandDispatcher` bileşenini başka C++/CMake projelerinde tekrar kullanılabilir ve genişleyebilir bir **TCP JSON komut/yanıt çekirdeği** haline getirmek için referans olarak kullanılabilir.
