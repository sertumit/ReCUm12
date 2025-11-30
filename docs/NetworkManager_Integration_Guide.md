# NetworkManager Entegrasyon Kılavuzu

Bu doküman, `modules/comm` altındaki **`NetworkManager.h/.cpp`** bileşenini bağımsız bir C++/CMake projesine entegre etmek için hazırlanmıştır. Bileşen, Linux üzerinde ağ arayüzlerinin bağlantı durumunu okumak için hafif bir yardımcı sınıf sağlar. fileciteturn0file2turn0file3

---

## 1. Genel Mimari

### 1.1. Sorumluluklar

- **Wi-Fi**, **Ethernet**, **GSM** ve **GPS** bağlantı durumlarını raporlayan bir soyutlama katmanı sağlar.
- Linux’un `/sys/class/net` altındaki dosyalarını okuyarak **Wi-Fi** ve **Ethernet** için gerçek bağlantı durumunu tespit eder. fileciteturn0file2L5-L27
- GSM ve GPS için şu an placeholder (hep `false`) döner; ileride gerçek implementasyon ile genişletilebilir. fileciteturn0file2L29-L36

### 1.2. Bağımlılıklar

- Standart C++ kütüphanesi:
  - `<fstream>`: `/sys` dosyalarını okumak için.
  - `<string>`
- Platform:
  - Linux (`/sys/class/net/<iface>/...` dizini).

C++ standardı:
- En az **C++11** yeterlidir (ileri seviye özellik yok).

---

## 2. Public API Referansı

### 2.1. Sınıf Tanımı

```cpp
namespace Comm {
class NetworkManager {
public:
  bool isWifiConnected();
  bool isEthernetConnected();
  bool isGsmConnected();  // Şimdilik hep false
  bool isGpsConnected();  // Şimdilik hep false
};
}
```

Kaynak: fileciteturn0file3L1-L9

### 2.2. `bool isWifiConnected()`

İmza:

```cpp
bool NetworkManager::isWifiConnected();
```

Davranış: fileciteturn0file2L5-L16

1. `/sys/class/net/wlan0/operstate` dosyasını okur.
2. Dosyadan tek kelimelik bir `state` okur (örn. `"up"`, `"down"`).
3. `state == "up"` ise `true`, aksi halde `false` döner.

Notlar:

- Arayüz ismi **`wlan0`** olarak sabitlenmiştir; farklı platformlarda (örneğin `wlp1s0`) kullanmak için bu sabit ismi parametrik hale getirmeniz gerekebilir.
- Dosya açılamaz veya okunamazsa `false` döner (bağlantı yok kabul edilir).

### 2.3. `bool isEthernetConnected()`

İmza:

```cpp
bool NetworkManager::isEthernetConnected();
```

Davranış: fileciteturn0file2L18-L32

1. `/sys/class/net/eth0/carrier` dosyasını okur.
   - İçerik integer olarak okunur (`0` veya `1`).
   - `1` ⇒ **link up**, `0` ⇒ **link down**.
2. Eğer `carrier` okunamazsa, fallback olarak `/sys/class/net/eth0/operstate` dosyasını okur.
   - Eğer `state == "up"` ise `true` döner.
3. Her iki adım da başarısız olursa `false` döner.

Notlar:

- Arayüz ismi **`eth0`** olarak sabittir; farklı isimler için parametreleştirilebilir.

### 2.4. `bool isGsmConnected()`

İmza:

```cpp
bool NetworkManager::isGsmConnected();
```

Davranış:

- Şu an **her zaman `false`** döner. İleride GSM modem katmanı eklendiğinde buraya gerçek implementasyon yazılacaktır. fileciteturn0file2L34-L36

### 2.5. `bool isGpsConnected()`

İmza:

```cpp
bool NetworkManager::isGpsConnected();
```

Davranış:

- Şu an **her zaman `false`** döner. İleride GPS modülü/daemon bağlantısı ile zenginleştirilebilir. fileciteturn0file2L34-L36

---

## 3. CMake Entegrasyonu

### 3.1. Kaynak ve header konumu

Yeni projede önerilen yerleşim:

```text
project_root/
  modules/
    comm/
      include/
        comm/
          NetworkManager.h
      src/
        NetworkManager.cpp
```

### 3.2. CMakeLists.txt örneği (library)

```cmake
# modules/comm/CMakeLists.txt

add_library(comm STATIC
    src/NetworkManager.cpp
)

target_include_directories(comm
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(comm PUBLIC cxx_std_11)
```

### 3.3. Uygulama tarafında link etme

```cmake
add_executable(my_app
    src/main.cpp
    # ...
)

target_link_libraries(my_app
    PRIVATE
        comm
)
```

---

## 4. Örnek Kullanım (GUI veya Service Seviyesi)

NetworkManager tipik olarak bir GUI veya servis içinde ikon/gösterge durumlarını belirlemek için kullanılabilir.

```cpp
#include "comm/NetworkManager.h"
#include <iostream>

int main() {
    Comm::NetworkManager nm;

    bool wifiUp = nm.isWifiConnected();
    bool ethUp  = nm.isEthernetConnected();

    std::cout << "WiFi: " << (wifiUp ? "UP" : "DOWN") << "\n";
    std::cout << "Ethernet: " << (ethUp ? "UP" : "DOWN") << "\n";

    // GSM / GPS şimdilik sabit false
    std::cout << "GSM: " << (nm.isGsmConnected() ? "UP" : "DOWN") << "\n";
    std::cout << "GPS: " << (nm.isGpsConnected() ? "UP" : "DOWN") << "\n";
}
```

GUI örneği (psödo kod):

```cpp
void MainWindow::updateNetworkIcons() {
    Comm::NetworkManager nm;

    wifiIcon.setVisible(nm.isWifiConnected());
    lanIcon.setVisible(nm.isEthernetConnected());

    // GSM / GPS ileride eklenecek
}
```

---

## 5. LineTcpClient ile İlişki

### 5.1. Mimari ilişki

- **NetworkManager**:
  - Aşağı seviyede **fiziksel/bağlantı** durumunu okur (Wi-Fi / Ethernet link up/down).
- **LineTcpClient**:
  - Uygulama seviyesinde **TCP oturumu** yönetir (JSON tabanlı client).

Doğrudan kod seviyesi bağımlılıkları yoktur, ancak pratikte şu şekilde birlikte kullanılırlar:

- GUI, **NetworkManager** ile `Wi-Fi` / `Ethernet` ikonlarını günceller.
- Aynı GUI veya servis, **LineTcpClient**’i başlatırken:
  - Örneğin, `isEthernetConnected()` veya `isWifiConnected()` sonuçlarına bakarak client’ı aktif/pasif hale getirebilir.
  - Bağlantı kesilirse kullanıcıya görsel uyarı verilebilir.

### 5.2. Örnek entegrasyon

```cpp
#include "comm/NetworkManager.h"
#include "net/LineTcpClient.h"
#include "net/CommandDispatcher.h"

class MyApp {
public:
    MyApp()
    : dispatcher_(std::make_shared<Net::CommandDispatcher>()),
      client_("127.0.0.1", 5051)
    {
        client_.setDispatcher(dispatcher_);
    }

    void start() {
        Comm::NetworkManager nm;
        if (nm.isEthernetConnected() || nm.isWifiConnected()) {
            client_.start();
        } else {
            // Ağ yok; GUI'de uyarı göster, daha sonra tekrar dene
        }
    }

    void stop() {
        client_.stop();
    }

private:
    std::shared_ptr<Net::CommandDispatcher> dispatcher_;
    Net::LineTcpClient client_;
};
```

Bu yapı sayesinde:

- NetworkManager **“altyapı hazır mı?”** sorusuna cevap verir.
- LineTcpClient ise **“uygulama protokolünü çalıştır”** rolünü üstlenir.

---

## 6. Genişletme Önerileri

1. **Arayüz isimlerini konfigüre edilebilir yapmak**:
   - `wlan0` / `eth0` sabitlerini, bir config dosyasından veya constructor parametresinden okuyacak şekilde güncelleyebilirsiniz.

2. **GSM / GPS implementasyonu**:
   - Örneğin bir GSM modem için AT komutları, GPS için NMEA parser veya bir daemon ile Unix domain socket üzerinden konuşulabilir.

3. **Hata raporlama**:
   - Şu an fonksiyonlar sadece `bool` döndürüyor. İleride enum veya error code ile `NO_SYSFS`, `IFACE_NOT_FOUND`, `PERMISSION_DENIED` gibi durumları detaylı raporlamak isteyebilirsiniz.