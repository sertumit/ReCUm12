# PumpR07Protocol Kullanım Rehberi

Bu doküman, **PumpR07Protocol.h / PumpR07Protocol.cpp** modülünün projede nasıl kullanılacağına dair kısa bir entegrasyon rehberidir. Amaç, RS‑485 hattı üzerinden Mepsan R07 pompalarla konuşan modüller için **ortak, modüler ve yeniden kullanılabilir** bir katman sağlamaktır.

---

## 1. Genel Mimari

`PumpR07Protocol` modülü üç ana parçadan oluşur:

1. **Düşük seviye yardımcı fonksiyonlar**
   - CRC hesaplama, hex string üretme, BCD çevirme gibi protokol ortak fonksiyonları.
2. **Frame build / parse API’si**
   - R07 frame’lerini üretip çözmek için `makeR07Frame / parseR07Frame` ve kısa MIN frame yardımcıları.
3. **Yüksek seviye protokol sınıfı**
   - `class PumpR07Protocol`: Gelen frame’i parse edip, uygun **callback’leri** (`onStatus`, `onFill`, `onTotals`, `onNozzle`) tetikler.
   - Frame üretimi için yüksek seviye fonksiyonlar (`makeStatusPollFrame`, `makePresetVolumeFrame`, `makeTotalCountersFrame`, `makeMinPoll`, `makeMinAck`).

Bu katman **seri portu veya RS‑485 hattını bilmez**. Yani:
- Byte’ları hatta yazmak ve hattan okumak, üst seviyedeki bir modülün (ör: `PumpInterfaceLvl3`, Qt controller, service, vs.) sorumluluğundadır.
- Bu modül sadece **frame nesnesi** (`std::vector<uint8_t>`) üretir ve çözümler.

---

## 2. Temel Tipler ve Enum’lar

### 2.1. Pompa durumu

```cpp
enum class PumpState {
    Unknown = 0,
    NotProgrammed,
    Reset,
    Authorized,
    Filling,
    FillingCompleted,
    MaxAmount,
    SwitchedOff,
    Suspended
};
```

Python tarafındaki `on_pump_status` canonical string’lerinin C++ karşılığıdır.

### 2.2. Satış ve toplam sayaç bilgileri

```cpp
struct FillInfo {
    double volume_l{0.0};   // Litre cinsinden hacim (x1.00)
    double amount{0.0};     // Para birimi (x1.00)
};

struct TotalCounters {
    double total_volume_l{0.0}; // Toplam litre
    double total_amount{0.0};   // Toplam tutar
};
```

### 2.3. Tabanca olayı

```cpp
struct NozzleEvent {
    bool nozzle_out{false}; // true: OUT, false: IN
};
```

### 2.4. Frame tipi

Sınıf içinden kullanılabilecek type alias’lar:

```cpp
using Byte  = std::uint8_t;
using Frame = std::vector<Byte>;
```

---

## 3. Düşük Seviye Yardımcılar

### 3.1. CRC16‑IBM

```cpp
std::uint16_t crc16Ibm(const std::uint8_t* data,
                       std::size_t length,
                       std::uint16_t init = 0x0000) noexcept;

std::uint16_t crc16Ibm(const std::vector<std::uint8_t>& data,
                       std::uint16_t init = 0x0000) noexcept;
```

- Mepsan sahaya uygun olarak **init = 0x0000** kullanır.
- Polinom: `0xA001` (Modbus / IBM türevi).

### 3.2. Hex string

```cpp
std::string hexLine(const std::uint8_t* data, std::size_t length);
std::string hexLine(const std::vector<std::uint8_t>& data);
```

- Log yazarken, frame’leri `"5020FA"` gibi büyük harf hex string’e çevirmek için kullanılır.

### 3.3. BCD ↔ integer

```cpp
std::uint32_t bcd4ToInt(const std::array<std::uint8_t, 4>& bytes) noexcept;
std::uint64_t bcd5ToInt(const std::array<std::uint8_t, 5>& bytes) noexcept;

std::array<std::uint8_t, 4> intToBcd4(std::uint32_t value) noexcept;
```

- Preset volume, totalizer ve sale record BCD alanları için kullanılır.
- `intToBcd4`: 0..99_999_999 aralığında, 8 haneli BCD üretir.

---

## 4. R07 Frame API’si

### 4.1. Ortak sabitler

```cpp
constexpr std::uint8_t R07_ETX   = 0x03;
constexpr std::uint8_t R07_TRAIL = 0xFA;

constexpr std::uint8_t R07_DEFAULT_ADDR  = 0x50;
constexpr std::uint8_t R07_MIN_POLL_CODE = 0x20;
constexpr std::uint8_t R07_MIN_ACK_CODE  = 0xC0;
```

### 4.2. CRC byte sırası

```cpp
enum class R07CrcOrder {
    LoHi,  // [CRC_LO][CRC_HI]
    HiLo,  // [CRC_HI][CRC_LO]
};
```

- Şu an tüm üretim ve parse işlemleri **LoHi** (saha ile uyumlu) kullanacak şekilde yapılandırıldı.

### 4.3. Frame üreticiler

#### Uzun frame

```cpp
std::vector<std::uint8_t> makeR07Frame(
    std::uint8_t addr,
    std::uint8_t cmd,
    std::uint8_t nozzle_or_trans,
    std::uint8_t len_header,
    const std::vector<std::uint8_t>& payload,
    R07CrcOrder crcOrder = R07CrcOrder::LoHi);
```

- Çıktı: `[ADDR][CMD][NOZ/TRANS][LEN][PAYLOAD...][CRC][ETX][TRAIL]`

#### Kısa (MIN) frame

```cpp
std::vector<std::uint8_t> makeR07MinFrame(
    std::uint8_t addr,
    std::uint8_t code);
```

- Çıktı: `[ADDR][CODE][TRAIL]`

#### Yüksek seviye MIN helper’ları

```cpp
std::vector<std::uint8_t> makeR07MinPoll(std::uint8_t addr = R07_DEFAULT_ADDR);
std::vector<std::uint8_t> makeR07MinAck (std::uint8_t addr = R07_DEFAULT_ADDR);
```

#### CD1 helper’ı

```cpp
std::vector<std::uint8_t> makeR07Cd1Frame(
    std::uint8_t addr,
    std::uint8_t dcc,
    R07CrcOrder crcOrder = R07CrcOrder::LoHi);

inline std::vector<std::uint8_t> makeR07Cd1Frame(
    std::uint8_t dcc,
    R07CrcOrder crcOrder = R07CrcOrder::LoHi)
{
    return makeR07Cd1Frame(R07_DEFAULT_ADDR, dcc, crcOrder);
}
```

- Python `_send_cd1(dcc_val)` ile bire bir uyumludur.

### 4.4. Frame parse sonucu

```cpp
struct R07ParseResult {
    bool valid{false};
    bool is_min_frame{false};

    std::uint8_t addr{0};
    std::uint8_t cmd{0};
    std::uint8_t nozzle_or_trans{0};
    std::uint8_t len_header{0};
    std::size_t  len_actual{0};

    std::vector<std::uint8_t> payload;

    std::uint16_t crc_rx{0};
    std::uint16_t crc_calc{0};
    bool          crc_ok{false};

    bool len_header_mismatch{false};
};
```

Parse fonksiyonu:

```cpp
R07ParseResult parseR07Frame(
    const std::vector<std::uint8_t>& frame,
    R07CrcOrder crcOrder = R07CrcOrder::LoHi) noexcept;
```

- Kısa MIN frame’ler için:
  - `is_min_frame = true`, `crc_ok` kullanılmaz, `payload` boştur.
- Uzun frame’ler için:
  - `payload` alanı, Python’daki `_parse_and_update` ile aynı kurala göre doldurulur:
    - `0x31–0x3F` ve `0x65`: payload `TRANS+LNG+DATA...` (`frame[2:-4]`)
    - Diğerleri: payload `NOZ+LEN+DATA...` (`frame[4:-4]`)

---

## 5. PumpR07Protocol Sınıfı

### 5.1. Genel API

```cpp
class PumpR07Protocol {
public:
    using Byte  = std::uint8_t;
    using Frame = std::vector<Byte>;

    PumpR07Protocol() = default;

    // Frame üretim API’si
    Frame makeStatusPollFrame(Byte addr, Byte dcc) const;
    Frame makeStatusPollFrame(Byte dcc) const;

    Frame makePresetVolumeFrame(double liters,
                                Byte addr,
                                Byte nozzle) const;
    Frame makePresetVolumeFrame(double liters) const;

    Frame makeTotalCountersFrame(Byte addr,
                                 Byte nozzle) const;
    Frame makeTotalCountersFrame(Byte nozzle) const;
    Frame makeTotalCountersFrame() const;

    Frame makeMinPoll(Byte addr) const;
    Frame makeMinAck(Byte addr) const;

    // Gelen frame’i çözümle
    void parseFrame(const Frame& frame);

    // Olay callback’leri
    std::function<void(PumpState)>             onStatus;
    std::function<void(const FillInfo&)>       onFill;
    std::function<void(const TotalCounters&)>  onTotals;
    std::function<void(const NozzleEvent&)>    onNozzle;
};
```

### 5.2. Kullanım modeli

Üst seviye modül (ör: `PumpInterfaceLvl3` veya Qt controller):

1. **Seri port / RS‑485 hattını açar** (Linux: `open` + `termios`).
2. R07 protokolü için bir örnek oluşturur:

```cpp
recum12::hw::PumpR07Protocol proto;
```

3. **Callback’leri bağlar**:

```cpp
proto.onStatus = [](recum12::hw::PumpState st) {
    // GUI update, log, state machine...
};

proto.onFill = [](const recum12::hw::FillInfo& fi) {
    // Son satış / dolum bilgisi
};

proto.onTotals = [](const recum12::hw::TotalCounters& tc) {
    // Totalizer bilgisi
};

proto.onNozzle = [](const recum12::hw::NozzleEvent& ev) {
    // Nozzle IN/OUT olayı
};
```

4. **Komut göndermek istediğinde frame üretir ve seri porta yazar**:

```cpp
// Ör: AUTHORIZE (0x06) için CD1
auto frame = proto.makeStatusPollFrame(/*dcc=*/0x06);
// frame vektöründeki byte’ları /dev/ttyUSB0’a yaz
write(fd, frame.data(), frame.size());
```

5. **RS‑485’ten gelen cevabı aldığında**:

- Byte’ları okuyan katman (`read` loop, thread, event loop) tam R07 frame’i toplar.
- Sonra:

```cpp
recum12::hw::PumpR07Protocol::Frame rx_frame = ...; // gelen frame
proto.parseFrame(rx_frame);
```

- `parseFrame`, içerde `parseR07Frame` çağırır ve komuta göre doğru callback’leri tetikler.

### 5.3. Şu anda desteklenen komutlar

`parseFrame` içinde şu komutlar decode edilir:

- `cmd == 0x01`  
  - Gerçek pompa DC1 (Pump Status).
  - 1 byte status → `PumpState` map edilip `onStatus` çağrılır.
- `cmd == 0xD1`  
  - Simülasyon DC1 (simulator’dan gelen frame’ler).
  - Python sim mapping’ine bire bir paralel.

- `cmd == 0xD4`  
  - Nozzle event (sim veya gerçek, 1 byte: 0x00=IN, !=0=OUT).
  - `NozzleEvent{nozzle_out}` ile `onNozzle` çağrılır.

- `cmd == 0x3D` (TOTALIZER)  
  - TRANS/LNG/DATA blokları içinde `TRANS=0x01` ve `LEN>=0x08` ilk blok bulunur.
  - DATA[0:4] = total volume, DATA[4:8] = total amount (ikisi de BCD x100).
  - `TotalCounters` doldurulur, `onTotals` çağrılır.

- `cmd == 0x3E` (FILL‑RECORD)  
  - TRANS/LNG/DATA blokları içinde `TRANS=0x02` ve `LEN>=0x08` ilk blok bulunur.
  - DATA[0:4] = volume, DATA[4:8] = amount (BCD x100).
  - `FillInfo` doldurulur, `onFill` çağrılır.

Diğer komutlar için şimdilik sadece `switch` içindeki `default:` kolu çalışır (no‑op). Gerekirse ileride yeni case’ler eklenebilir.

---

## 6. CMake Entegrasyonu

`PumpR07Protocol` tipik olarak `hw` isimli bir static/shared library içinde yer alabilir.

### 6.1. Örnek CMakeLists.txt (hw kütüphanesi)

```cmake
add_library(hw
    PumpR07Protocol.cpp
    PumpInterfaceLvl3.cpp
    # diğer hw kaynakları...
)

target_include_directories(hw
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR} # içinde "hw/" klasörü varsa
)
```

Projedeki başka bir modül bu kütüphaneyi şöyle kullanır:

```cmake
target_link_libraries(MyControllerModule
    PRIVATE
        hw
)
```

Kaynakta:

```cpp
#include "hw/PumpR07Protocol.h"
#include "hw/PumpInterfaceLvl3.h"
```

---

## 7. PumpInterfaceLvl3 ile Birlikte Kullanım

`PumpInterfaceLvl3`, `PumpR07Protocol`’u içerde kullanarak:

- RS‑485 port açma/kapama (`/dev/ttyUSB0`, 9600 8N1)
- Frame send fonksiyonları (StatusPoll, PresetVolume, TotalCounters)
- Gelen frame’leri `parseFrame`’e paslamak
- Callback’leri bir üst seviyeye taşımak için tasarlanmıştır.

Tipik kullanım:

```cpp
recum12::hw::PumpInterfaceLvl3 pump;

pump.setDevice("/dev/ttyUSB0");
if (!pump.open()) {
    // hata yönetimi
}

// Callback’leri bağla
pump.onStatus = [](recum12::hw::PumpState st) {
    // GUI / log / state machine
};

pump.onFill = [](const recum12::hw::FillInfo& fi) {
    // Satış bilgisi
};

pump.onTotals = [](const recum12::hw::TotalCounters& tc) {
    // Totalizer bilgisi
};

pump.onNozzle = [](const recum12::hw::NozzleEvent& ev) {
    // Nozzle IN/OUT
};

// Örnek komutlar
pump.sendStatusPoll(0x06);     // AUTHORIZE
pump.sendPresetVolume(8.00);   // 8.00 litre preset
pump.sendTotalCounters();      // totalizer isteği
```

**Not:** RX tarafında byte’ları `read()` ile okuyup, R07 frame sınırlarını keşfetmek (addr/cmd/len/CRC/ETX/FA’ya göre framing) üst seviyedeki okuma döngüsünün sorumluluğundadır. Frame tamamlandığında sadece:

```cpp
pump.handleReceivedFrame(frame);
```

demek yeterlidir.

---

## 8. Threading ve Sorumluluklar

- `PumpR07Protocol` kendi içinde **thread‑safe değildir**. Eğer:
  - Farklı thread’ler hem `parseFrame` çağırıyor,
  - Hem de callback’leri işliyorsa,
  uygun senkronizasyon (mutex / queue) üst seviyede düşünülmelidir.
- Tipik pattern:
  - **I/O thread**: RS‑485’e yazıp okur, gelen frame’leri `parseFrame`’e verir.
  - **UI / logic thread**: Callback’lerde gelen bilgileri GUI’ye veya state machine’e aktarır.

---

## 9. Modülerlik ve Gelecek Adımlar

Bu modülün görevi:

- Protokol detaylarını (CRC, BCD, TRANS/LNG yapısı, DC1/FILL‑REC/TOTALIZER) soyutlamak.
- Diğer modüllerin sadece **PumpState / FillInfo / TotalCounters / NozzleEvent** gibi domain tipleriyle çalışmasını sağlamak.

Gelecekte eklenebilecekler:

- Daha fazla R07 komutunun parse edilmesi (0x31–0x33 statik total, 0x34–0x38 DC‑FILL event’leri vb.).
- Dahili bir runtime state struct’ı (örn. `PumpRuntimeState`) ile son durumu class içinde tutma.
- Gelişmiş logging (R07 hex dump, CRC hataları, LEN uyuşmazlıkları).

Bu haliyle, `PumpR07Protocol` projedeki diğer modüller tarafından **stabil bir entegrasyon noktası** olarak kabul edilebilir.
