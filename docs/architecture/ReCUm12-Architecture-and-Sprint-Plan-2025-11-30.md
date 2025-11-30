# ReCUm12 Mimari Özeti ve Sprint Planı  
_Tarih: 30.11.2025_

Bu doküman, 30.11.2025 itibarıyla ReCUm12 projesinin mimari fotoğrafını ve sonraki sprintler için genişletilmiş planı özetler. Log yönetimi, network bağlantı yönetimi ve uzaktan komut yönetimi, mevcut yapıya entegre edilmiş olarak ele alınmıştır.

---

## 1. Mevcut Mimari Fotoğrafı (R2 Snapshot)

### 1.1. Ana Modüller

- **apps/**
  - `recum12_pump_gui`
    - GTK+ tabanlı ana uygulama.
    - `AppRuntime`
      - GUI ile core/hw katmanı arasında “orkestra” görevi görüyor.
      - `PumpRuntimeStore` ile pompa durumunu takip ediyor.
      - `RfidAuthController`, `UserManager`, `PumpInterfaceLvl3` gibi bileşenleri bağlıyor.
      - Sayaç dosyası (`repo_log.json`) yükleme/kaydetme ve GUI sayaçlarını güncelleme.
    - `MainWindow`
      - glade tanımına dayalı ekran, butonlar, label’lar vs.
      - Kullanıcı ID / plaka görünümü, pompa state görünümleri.
    - `Rs485GuiAdapter`
      - `PumpRuntimeState` → GUI dönüşüm katmanı.
      - PumpState/ nozzle_out / current & last litre bilgisine göre:
        - Idle, Authorized, Filling, FillingCompleted vb. ekranlardan uygun olanı seçer.
        - `lblmsg` içeriklerini Excel senaryosuna uygun olarak set eder.

- **modules/core/**
  - `PumpRuntimeState` & `PumpRuntimeStore`
    - Tek hakikat pompa runtime state’i.
    - Sözleşme:
      - `pump_state`, `nozzle_out`
      - `last_fill` (ham FillInfo, genelde totalizer seviyesi)
      - `current_fill_volume_l` / `has_current_fill`
      - `last_fill_volume_l` / `has_last_fill`
      - `totals`
      - RFID alanları: `last_card_uid`, `last_card_auth_ok`, `last_card_user_id`, `last_card_plate`
      - Limit alanları: `limit_liters`, `has_limit`, `remaining_limit_liters`
      - Latch’ler: `auth_active`, `sale_active`
    - İç mantık:
      - `updateFromPumpStatus`:
        - `sale_active` latch’ini PumpState’e göre yönetir.
        - FILLING’e yeni girerken baseline (fill_baseline_volume_l_) sıfırlanır.
      - `updateFromFill`:
        - FillInfo.volume_l → totalizer gibi ele alınır.
        - İlk satışta baseline alınıp, `current_fill_volume_l = total - baseline` hesaplanır.
        - `last_fill_volume_l` ve `has_last_fill` aynı cur değeriyle güncellenir.
        - Limit varsa, `remaining_limit_liters = limit_liters - last_sale_volume_l_` hesaplanır (0 altına düşmez).
        - Aktif satış yokken gelen FillInfo’da current 0 yapılır, varsa kalan limit full limite geri alınır.
      - `updateFromNozzle`:
        - Nozzle OUT → IN geçişinde current_fill sıfırlanır, bir sonraki satış için baseline bayrağı temizlenir.
      - `updateFromRfidAuth`:
        - Kart bilgisi + limit state’e işlenir.
        - `has_limit` ve `remaining_limit_liters` başlangıçta limit’e eşitlenir.
      - `clearAuth`:
        - AUTH latch’i kapatılır, limit state sıfırlanır.
  - `RfidAuthController`
    - Pn532Reader olaylarına dayalı RFID akışı:
      - `handleNozzleOut` → kart okuma isteği.
      - `handleNozzleInOrSaleFinished` → kart işlemini iptal.
    - `UserManager` ile entegrasyon:
      - `findByRfid(uid)` ile kayıtlı kullanıcıyı bulur.
      - `AuthContext` içinde: `uid_hex`, `authorized`, `user_id`, `plate`, `limit_liters` gibi alanlar doldurulur.
    - Pompa entegrasyonu:
      - Yetkili kart için `pump.sendStatusPoll(0x06)` (AUTHORIZE) gönderir.
      - 10 sn cooldown penceresi içinde yeni nozzle-out isteklerini ignore eder.
  - `UserManager`
    - `users.csv` şeması:
      - `userId, level, firstName, lastName, plate, limit_liters, rfid`
    - `UserRecord`:
      - `userId`, `level`, `firstName`, `lastName`, `plate`
      - `limit` (eski uyumluluk)
      - `limit_liters` (asıl kullanılan alan)
      - `rfid` (normalize edilmiş, uppercase, iki nokta / tire / boşluksuz)
    - `loadUsers`:
      - Başlığı küçük-büyük harf duyarsız okur (`limit`, `quota`, `limit_liters` hepsini destekler).
      - Limit sütununu double olarak okuyup `limit_liters` + `limit` doldurur.
    - `findByRfid`:
      - Normalized uid’e göre kayıt arar.
- **modules/hw/**
  - `PumpInterfaceLvl3`
    - R07 protokolüne göre RS485 haberleşme.
    - Callback’ler:
      - `onStatus(PumpState)`
      - `onFill(FillInfo)`
      - `onTotals(TotalCounters)`
      - `onNozzle(NozzleEvent)`
    - `sendMinPoll`, `sendStatusPoll(DCC)` gibi fonksiyonlar.
- **configs/**
  - `users.csv` (RFID → kullanıcı/limit eşlemesi)
  - `repo_log.json` (toplam dolum logu, UI sayaçları)
  - log klasörleri (R2 snapshot’ta kısmi)

---

## 2. Yeni Ana Bloklar: Roller ve Entegrasyon Noktaları

Bu bölüm, 3 yeni ana bileşeni mevcut mimariye yerleştirir:

1. **Core::LogManager** – _tek log merkezi_
2. **Comm::NetworkManager** – _network bağlantı ve IP yönetimi_
3. **Net::CommandDispatcher** – _uzaktan komut/yanıt motoru_

### 2.1. Core::LogManager – Log Yönetimi

**Rol:**

- Usage log’ları yönetmek:
  - Örnek: `configs/logs.csv` (processId, rfid, fuel, logCode, sendOk, …)
- Infra log’ları yönetmek:
  - Örnek: `logs/recumLogs.csv` (cmd_rx, cmd_tx, service_start, network event, vs.)
- Retention:
  - Belirlenmiş klasörlerdeki log dosyaları için “X günden eski kayıtları sil” API’si.

**Entegrasyon Noktaları:**

- `apps/recum12_pump_gui / AppRuntime`
  - Satış bittiğinde / repo_log güncellendiğinde usage log satırı açar.
  - RS485 ve RFID kritik hatalarını infra log’a yazar.
- `Net::CommandDispatcher`
  - `getLogs`, `logsQuery`, `getFile`, `runRetention` gibi komutlar LogManager üzerinden çalışır.
- Arka plan iş (cron veya daemon)
  - Günlük `runRetention("all", N)` çağrıları ile log boyutları kontrol altında tutulur.

### 2.2. Comm::NetworkManager – Network Bağlantı Yönetimi

**Rol:**

- Ethernet/WiFi link UP/DOWN tespiti.
- IP bilgisi sağlamak:
  - Ether IP, WiFi IP, GPRS IP (şimdilik GPRS sabit `0.0.0.0`)
- Önemli network olaylarını (link up/down, ip değişimi) infra log’a geçmek.

**Entegrasyon Noktaları:**

- `MainWindow` / `AppRuntime`
  - Yeni widget’lar:

    - `imgwifi`:  
      - Bağlantı var → `modules/gui/resources/wifi_on_48x48.png`  
      - Bağlantı yok → `modules/gui/resources/wifi_off_48x48.png`
    - `imglan`:  
      - Bağlantı var → `modules/gui/resources/lan_on_48x48.png`  
      - Bağlantı yok → `modules/gui/resources/lan_off_48x48.png`
    - `imggsm`:  
      - Şimdilik her zaman `modules/gui/resources/gsm_off_48x48.png`
    - `imgrs485`:  
      - RS485 cihazı açılabildi ve sorunsuz çalışıyor ise → `modules/gui/resources/lRs485_on_48x48.png`  
      - Aksi durumda → `modules/gui/resources/lRs485_off_48x48.png`
    - `lblEthIP`:  
      - Ether IP veya bağlantı yoksa `0.0.0.0`
    - `lblWiFiIP`:  
      - WiFi IP veya bağlantı yoksa `0.0.0.0`
    - `lblGprsIP`:  
      - Şimdilik her zaman `0.0.0.0`

  - `NetworkManager` periyodik sorgu ile bu widget’ların durumunu güncelleyecek.
- `Core::LogManager` entegrasyonu
  - `NetworkManager` önemli bir olay gördüğünde:
    - `LogManager::appendInfra` ile `event=net_link_up/down`, `mode=net:server`, `msg` içinde interface + ip bilgisi yazar.

### 2.3. Net::CommandDispatcher – Uzaktan Komut Yönetimi

**Rol:**

- TCP üzerinden gelen satır bazlı JSON istekleri okuyup, `action` alanına göre handler’lara yönlendirir.
- V2 zarf yapısı:
  - `type` (request/response)
  - `msgId`
  - `action`
  - `payload`

**Öngörülen Komutlar (MVP):**

- Log tabanlı:
  - `getLogs` → Usage/infra log’ları döndürür (LogManager backend).
  - `logsQuery` → Belirli filtrelerle infra log sorgusu.
  - `getFile` → `configs/*.csv`, `logs/*.csv` gibi dosyaları base64 encode edip gönderir.
- Kullanıcı tabanlı:
  - `getUsers`, `addUser`, `updateUser`, `deleteUser`, `setUserRfid`
  - Arka planda `UserManager` kullanır.
- RS485 debug:
  - `rs485.readRaw` → PumpInterfaceLvl3 üzerinden ham satır okuma.

**Filling Guard:**

- Dolum sırasında bazı komutların reddedilmesi için:
  - `dispatcher.setFilling(true/false)` API’si.
  - Eğer filling aktif ise, riskli komutlar `E_BUSY, "filling"` gibi bir hata ile geri döner.

**Entegrasyon Noktaları:**

- `modules/net` altında:
  - `CommandDispatcher.*`
  - `TcpServer.*`
- `apps/recum12_pump_gui`:
  - TCP sunucu thread’i:
    - Her satır için `dispatcher.dispatch(line, out, guiCallback, ip, port)` çağırır.
  - `guiCallback` ile GUI’de kısa durum mesajları gösterilebilir.
- Core bağlantıları:
  - `UserManager` → user yönetim komutları.
  - `PumpRuntimeStore` + `PumpInterfaceLvl3` → status/rs485 debug komutları.
  - `Core::LogManager` → log okuma ve retention komutları.

---

## 3. Genişletilmiş Gelişim Planı

Bu plan, önceliği “log yönetimi, network bağlantı yönetimi, uzaktan komut yönetimi” üzerine çekerek eski adımları yeniden sıralar.

### Faz 0 – Fotoğraf & Freeze (BUGÜN – Tamamlandı)

- 30.11.2025 R2 snapshot’ı referans alındı.
- RS485 + RFID + limit mantığı “gözlem” modunda, daha sonra tekrar ele alınmak üzere bırakıldı.

### Faz 1 – Log Altyapısı: Core::LogManager

1. **L1 – LogManager dosyalarını projeye ekle**
   - `modules/core/include/core/LogManager.h`
   - `modules/core/src/LogManager.cpp`
2. **L2 – Eski log noktalarının LogManager ile sarılması**
   - Satış bitişleri, repo_log güncellemeleri → usage log.
   - RS485/RFID kritik hatalar → infra log.
3. **L3 – Retention / scaffold**
   - Startup:
     - `LogManager::detectAppRoot()`
     - `LogManager::ensureScaffold(...)`
   - Gerektiğinde `runRetention` çağrıları ile log temizliği.

### Faz 2 – Network Bağlantı Yönetimi + GUI İkonları

1. **N1 – NetworkManager modülü**
   - Eth/WiFi link + IP API’si.
2. **N2 – AppRuntime entegrasyonu**
   - Periyodik poll ile:
     - `set_eth_icon_connected(bool)`
     - `set_wifi_icon_connected(bool)`
     - `set_rs485_icon_connected(bool)`
     - `set_eth_ip(string)`
     - `set_wifi_ip(string)`
     - `set_gprs_ip(string)`
3. **N3 – MainWindow / Glade güncellemeleri**
   - `imgwifi`, `imglan`, `imggsm`, `imgrs485`
   - `lblEthIP`, `lblWiFiIP`, `lblGprsIP`
4. **N4 – NetworkManager + LogManager entegrasyonu**
   - Network event’lerini infra log’a yazmak.

### Faz 3 – Uzaktan Komut Yönetimi: Net::CommandDispatcher

1. **R1 – modules/net altyapısı**
   - CommandDispatcher + TcpServer sınıfları.
2. **R2 – AppRuntime içinde TCP sunucu**
   - Arka planda port dinleyip gelen JSON satırlarını CommandDispatcher’a yönlendirmek.
3. **R3 – İlk handler set’i (MVP)**
   - `getLogs`, `logsQuery`, `getFile` (LogManager)
   - `getUsers`, `addUser`, `updateUser`, `deleteUser`, `setUserRfid` (UserManager)
   - `rs485.readRaw` (PumpInterfaceLvl3)
4. **R4 – Filling guard**
   - `PumpRuntimeStore` state’ine göre:
     - Dolum esnasında riskli komutların reddedilmesi.

### Faz 4 – Pompa/RFID/Limit Planı ile Yeniden Hizalama

- Limit hesaplaması LogManager/CommandDispatcher ile hizalanacak:
  - Limit aşımı hem GUI’de hem usage log’da hem de uzaktan erişilebilir olacak.
- `RfidAuthController` + `UserManager`:
  - Uzaktan komutlarla kullanıcı/limit yönetimi yapılabilir hale getirilecek.

### Faz 5 – İleri Geliştirmeler

- Uzaktan komutlar için kimlik doğrulama / rol bazlı yetki.
- GUI içinde log viewer / network debug sayfaları.
- Uzaktan konfigürasyon ve firmware/ayar dosyası güncelleme.

---

## 4. Gelecekte Bu Dokümanı Nasıl Kullanacağız?

- Bu dokümanı repoda kalıcı bir yere koyman ideal:
  - Öneri: `docs/architecture/ReCUm12-Architecture-and-Sprint-Plan.md`
- Her yeni sprintte:
  1. Sprint başında bu dokümanı referans alacaksın.
  2. Gerekirse “Faz X’in şu adımları tamamlandı, şuralar güncel değil” diye üstüne küçük bir **“Sprint Notları”** bölümü ekleyebilirsin.
  3. Yeni plan değişiklikleri olduğunda yine bu dosya üzerinde revizyon yapacağız (örneğin Faz sırası değişirse, burayı güncelleyeceğiz).

ChatGPT ile yeni sprint başlatırken:

- Bana şöyle bir cümleyle başlayabilirsin:
  - “En güncel mimari/plan dokümanı `docs/architecture/ReCUm12-Architecture-and-Sprint-Plan.md`. Bu dokümana göre son sprintte L1-L2 tamamlandı, L3 ve N1 sırada. Buradan devam edelim.”
- İstersen sadece ilgili bölümü kopyalayıp buraya yapıştırman da yeterli. Böylece her seferinde tüm dosyayı aktarmana gerek kalmaz, ama repo tarafında tek kaynak olarak bu doküman yaşamaya devam eder.

