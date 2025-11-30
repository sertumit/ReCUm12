#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace recum12::hw {

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

struct FillInfo {
    double volume_l{0.0};
    double amount{0.0};
};

struct TotalCounters {
    double total_volume_l{0.0};
    double total_amount{0.0};
};

struct NozzleEvent {
    bool nozzle_out{false};
};
class PumpR07Protocol {
public:
    using Byte  = std::uint8_t;
    using Frame = std::vector<Byte>;

    PumpR07Protocol() = default;

    // ---- Frame oluşturma API'si ----

    // STATUS (CD1) – Python'daki _send_cd1 karşılığı olacak
    Frame makeStatusPollFrame(Byte addr, Byte dcc) const;
    // Varsayılan pompa adresi (R07_DEFAULT_ADDR) ile STATUS
    Frame makeStatusPollFrame(Byte dcc) const;
    // PRESET VOLUME (CD3) – Python _send_cd3_preset_volume
    Frame makePresetVolumeFrame(double liters,
                                Byte addr,
                                Byte nozzle) const;
    // Varsayılan addr + nozzle-1 ile preset volume (Python'daki tipik kullanım)
    Frame makePresetVolumeFrame(double liters) const;

    // TOTAL COUNTERS (CD101 / 0x65) – Python tarafındaki sayaç sorgusu
    Frame makeTotalCountersFrame(Byte addr,
                                 Byte nozzle) const;
    // Varsayılan addr ile belirli nozzle için sayaç sorgusu
    Frame makeTotalCountersFrame(Byte nozzle) const;
    // Varsayılan addr + nozzle-1 için sayaç sorgusu
    Frame makeTotalCountersFrame() const;
    // Minimum poll/ack (MIN-POLL / MIN-ACK)
    Frame makeMinPoll(Byte addr) const;
    Frame makeMinAck(Byte addr) const;

    // ---- Gelen frame'i çözümleme ----

    // Ham frame geldiğinde çağrılır; geçerli bir R07 frame ise
    // uygun callback'leri tetikleyecek (mantığı sonra dolduracağız).
    void parseFrame(const Frame& frame);

    // ---- Olay callback'leri ----

    // Pompa durum değişimi (AUTHORIZED, FILLING, COMPLETED vb.)
    std::function<void(PumpState)>             onStatus;

    // Satış (FILL-RECORD) güncellemesi
    std::function<void(const FillInfo&)>       onFill;

    // Toplam sayaç güncellemesi
    std::function<void(const TotalCounters&)>  onTotals;

    // Tabanca içeri/dışarı olayı
    std::function<void(const NozzleEvent&)>    onNozzle;

private:
    // Şimdilik iç durum tutmuyoruz; Python tarafındaki state machine
    // daha sonra buraya veya ayrı bir sınıfa taşınacak.
};

// Protocol-level helper functions shared with RS-485 controller.
// Bunlar Python tarafındaki R07 helper'larının bire bir C++ karşılıklarıdır.
std::uint16_t crc16Ibm(const std::uint8_t* data, std::size_t length, std::uint16_t init = 0x0000) noexcept;
std::uint16_t crc16Ibm(const std::vector<std::uint8_t>& data, std::uint16_t init = 0x0000) noexcept;

// Uppercase hex string (no separators), örn: "0201007BFA".
std::string hexLine(const std::uint8_t* data, std::size_t length);
std::string hexLine(const std::vector<std::uint8_t>& data);

// 4 veya 5 byte BCD alanını tamsayıya çevirir.
// Geçersiz nibble'lar (>=0xA) 0 kabul edilir.
std::uint32_t bcd4ToInt(const std::array<std::uint8_t, 4>& bytes) noexcept;
std::uint64_t bcd5ToInt(const std::array<std::uint8_t, 5>& bytes) noexcept;

// Tamsayıyı 4 byte (8 haneli) BCD'e çevirir (0..99_999_999, dışı kırpılır).
std::array<std::uint8_t, 4> intToBcd4(std::uint32_t value) noexcept;
// --- R07 frame sabitleri ve tipleri ---

// R07 uzun çerçeveler için son iki byte:
// ... [CRC_LO/HI][ETX][TRAIL]
constexpr std::uint8_t R07_ETX   = 0x03;
constexpr std::uint8_t R07_TRAIL = 0xFA;

constexpr std::uint8_t R07_DEFAULT_ADDR   = 0x50;  // Varsayılan istasyon adresi (YAT loglarına göre)
constexpr std::uint8_t R07_MIN_POLL_CODE  = 0x20;  // 50 20 FA → MIN-POLL
constexpr std::uint8_t R07_MIN_ACK_CODE   = 0xC0;  // 50 C0 FA → MIN-ACK

// CRC byte sırası:
//  - LoHi : [CRC_LO][CRC_HI]
//  - HiLo : [CRC_HI][CRC_LO]
enum class R07CrcOrder {
    LoHi,
    HiLo,
};

// Python _parse_and_update() eşleniği için basit sonuç yapısı.
// MIN çerçevesi: is_min_frame=true, addr/cmd dolu, payload boş.
// Uzun çerçeve: is_min_frame=false, CRC/LEN alanları dolu.
struct R07ParseResult {
    bool valid{false};               // temel yapı geçerli mi? (len, ETX, TRAIL)
    bool is_min_frame{false};        // 3 byte'lık MIN-POLL/MIN-ACK tipi mi?

    std::uint8_t addr{0};
    std::uint8_t cmd{0};             // MIN'de code, uzun çerçevede CMD
    std::uint8_t nozzle_or_trans{0}; // uzun çerçevede 3. byte (NOZ veya TRANS)
    std::uint8_t len_header{0};      // header LEN (fr[3])
    std::size_t  len_actual{0};      // gerçek payload uzunluğu

    std::vector<std::uint8_t> payload; // DC ailesinde TRANS+LNG+DATA, diğerlerinde NOZ+LEN+DATA

    std::uint16_t crc_rx{0};         // frame'den okunan CRC
    std::uint16_t crc_calc{0};       // yeniden hesaplanan CRC
    bool crc_ok{false};              // crc_rx == crc_calc?

    bool len_header_mismatch{false}; // LEN header != gerçek uzunluk ve DC ailesi DIŞI
};

// --- R07 frame build/parse API'si ---

// Uzun R07 çerçevesi üretir:
// [ADDR][CMD][NOZ/TRANS][LEN][PAYLOAD...] + CRC(LoHi/HiLo) + ETX + TRAIL
std::vector<std::uint8_t> makeR07Frame(
    std::uint8_t addr,
    std::uint8_t cmd,
    std::uint8_t nozzle_or_trans,
    std::uint8_t len_header,
    const std::vector<std::uint8_t>& payload,
    R07CrcOrder crcOrder = R07CrcOrder::LoHi);

// Kısa (MIN) çerçeve: [ADDR][CODE][TRAIL]
std::vector<std::uint8_t> makeR07MinFrame(
    std::uint8_t addr,
    std::uint8_t code);

// R07 çerçevesini çözer.
//  - MIN çerçevelerde CRC hesaplanmaz, sadece addr/cmd döner.
//  - Uzun çerçevelerde payload, CRC ve LEN bilgileri doldurulur.
R07ParseResult parseR07Frame(
    const std::vector<std::uint8_t>& frame,
    R07CrcOrder crcOrder = R07CrcOrder::LoHi) noexcept;
    
// --- Yüksek seviye frame helper'ları (Python _send_* fonksiyonlarına paralel) ---
// Kısa MIN çerçeveleri: 50 20 FA (POLL), 50 C0 FA (ACK)
std::vector<std::uint8_t> makeR07MinPoll(std::uint8_t addr = R07_DEFAULT_ADDR);
std::vector<std::uint8_t> makeR07MinAck (std::uint8_t addr = R07_DEFAULT_ADDR);

// CD1 komutu: [ADDR][0x30][NOZ=0x01][LEN=0x01][DCC] + CRC + ETX + TRAIL
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

} // namespace recum12::hw
