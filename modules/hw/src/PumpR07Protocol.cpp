#include "hw/PumpR07Protocol.h"
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <vector>
namespace recum12::hw {

std::uint16_t crc16Ibm(const std::uint8_t* data, std::size_t length, std::uint16_t init) noexcept
{
    // CRC16-IBM/Modbus (poly=0xA001), MSB,LSB.
    // Not: Mepsan sahada init=0x0000 kullanıyor (YAT logundan teyit).
    std::uint16_t crc = init;
    for (std::size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 0x0001) {
                crc = static_cast<std::uint16_t>((crc >> 1) ^ 0xA001u);
            } else {
                crc = static_cast<std::uint16_t>(crc >> 1);
            }
        }
        crc &= 0xFFFFu;
    }
    return crc;
}

std::uint16_t crc16Ibm(const std::vector<std::uint8_t>& data, std::uint16_t init) noexcept
{
    return crc16Ibm(data.data(), data.size(), init);
}

std::string hexLine(const std::uint8_t* data, std::size_t length)
{
    std::ostringstream oss;
    oss << std::uppercase << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < length; ++i) {
        oss << std::setw(2) << static_cast<unsigned int>(data[i]);
    }
    return oss.str();
}

std::string hexLine(const std::vector<std::uint8_t>& data)
{
    return hexLine(data.data(), data.size());
}

std::uint32_t bcd4ToInt(const std::array<std::uint8_t, 4>& bytes) noexcept
{
    // 4 byte (8 nibble) BCD'yi tamsayıya çevirir.
    // Örn: 0x00 0x00 0x01 0x23 -> 123
    // Geçersiz nibble'ları (>=0xA) 0 sayar.
    std::uint32_t val = 0;
    for (std::uint8_t b : bytes) {
        auto hi = static_cast<std::uint8_t>((b >> 4) & 0x0F);
        auto lo = static_cast<std::uint8_t>(b & 0x0F);
        val = val * 10u + (hi < 10u ? hi : 0u);
        val = val * 10u + (lo < 10u ? lo : 0u);
    }
    return val;
}

std::uint64_t bcd5ToInt(const std::array<std::uint8_t, 5>& bytes) noexcept
{
    // 5 byte (10 nibble) BCD'yi tamsayıya çevirir.
    // Örn: 0x00 0x00 0x00 0x01 0x23 -> 123
    // Geçersiz nibble'lar (>=0xA) 0 sayılır.
    std::uint64_t val = 0;
    for (std::uint8_t b : bytes) {
        auto hi = static_cast<std::uint8_t>((b >> 4) & 0x0F);
        auto lo = static_cast<std::uint8_t>(b & 0x0F);
        val = val * 10u + (hi < 10u ? hi : 0u);
        val = val * 10u + (lo < 10u ? lo : 0u);
    }
    return val;
}

std::array<std::uint8_t, 4> intToBcd4(std::uint32_t value) noexcept
{
    // Tamsayıyı 4 byte (8 haneli) BCD'e çevirir.
    // Örn: 800 -> 0x00 0x00 0x08 0x00 (8,00 L preset).
    // Geçerli aralık: 0..99_999_999 (dışına taşarsa kırpılır).
    if (value > 99999999u) {
        value = 99999999u;
    }

    char buf[9] = {};
    std::snprintf(buf, sizeof(buf), "%08u", value);

    std::array<std::uint8_t, 4> out{};
    for (std::size_t i = 0; i < 4; ++i) {
        auto hi = static_cast<std::uint8_t>(buf[2 * i] - '0');
        auto lo = static_cast<std::uint8_t>(buf[2 * i + 1] - '0');
        out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return out;
}
// --- R07 frame build/parse implementasyonları ---

std::vector<std::uint8_t> makeR07Frame(
    std::uint8_t addr,
    std::uint8_t cmd,
    std::uint8_t nozzle_or_trans,
    std::uint8_t len_header,
    const std::vector<std::uint8_t>& payload,
    R07CrcOrder crcOrder)
{
    // Python tarafındaki:
    //   frame_wo_crc = bytes([ADDR, CMD, NOZ/TRANS, LEN]) + payload
    //   crc = crc16_ibm(frame_wo_crc)
    //   out = frame_wo_crc + crc_bytes + bytes([ETX, TRAIL])
    std::vector<std::uint8_t> frame;
    frame.reserve(4u + payload.size() + 4u);

    frame.push_back(addr);
    frame.push_back(cmd);
    frame.push_back(nozzle_or_trans);
    frame.push_back(len_header);
    frame.insert(frame.end(), payload.begin(), payload.end());

    const auto crc = crc16Ibm(frame);
    const auto crc_lo = static_cast<std::uint8_t>(crc & 0xFFu);
    const auto crc_hi = static_cast<std::uint8_t>((crc >> 8) & 0xFFu);

    if (crcOrder == R07CrcOrder::HiLo) {
        frame.push_back(crc_hi);
        frame.push_back(crc_lo);
    } else {
        frame.push_back(crc_lo);
        frame.push_back(crc_hi);
    }

    frame.push_back(R07_ETX);
    frame.push_back(R07_TRAIL);

    return frame;
}

std::vector<std::uint8_t> makeR07MinFrame(
    std::uint8_t addr,
    std::uint8_t code)
{
    // Python:
    //   out = bytes([0x50, 0x20, TRAIL]) / bytes([0x50, 0xC0, TRAIL])
    std::vector<std::uint8_t> frame;
    frame.reserve(3u);
    frame.push_back(addr);
    frame.push_back(code);
    frame.push_back(R07_TRAIL);
    return frame;
}

// --- Yüksek seviye helper'lar: MIN-POLL / MIN-ACK / CD1 --------------------------
// Python tarafındaki _send_min_poll / _send_min_ack'e bire bir denk gelir.
std::vector<std::uint8_t> makeR07MinPoll(std::uint8_t addr)
{
    return makeR07MinFrame(addr, R07_MIN_POLL_CODE);
}

std::vector<std::uint8_t> makeR07MinAck(std::uint8_t addr)
{
    return makeR07MinFrame(addr, R07_MIN_ACK_CODE);
}

// Python _send_cd1(dcc_val) için C++ karşılığı.
std::vector<std::uint8_t> makeR07Cd1Frame(
    std::uint8_t addr,
    std::uint8_t dcc,
    R07CrcOrder crcOrder)
{
    constexpr std::uint8_t nozzle     = 0x01;  // Şimdilik sabit nozzle-1
    constexpr std::uint8_t len_header = 0x01;  // DCC alanı 1 byte

    std::vector<std::uint8_t> payload;
    payload.reserve(1);
    payload.push_back(dcc);

    return makeR07Frame(addr, 0x30, nozzle, len_header, payload, crcOrder);
}

R07ParseResult parseR07Frame(
    const std::vector<std::uint8_t>& frame,
    R07CrcOrder crcOrder) noexcept
{
    R07ParseResult r{};
    const auto len = frame.size();
    if (len == 0) {
        return r;
    }

    // MIN çerçeve: [0x50][0x20/0xC0/0x70][TRAIL]
    if (len == 3 && frame[0] == 0x50 && frame[2] == R07_TRAIL) {
        r.valid = true;
        r.is_min_frame = true;
        r.addr = frame[0];
        r.cmd  = frame[1]; // MIN code
        return r;
    }

    // Uzun çerçeve: [ADDR][CMD][NOZ/TRANS][LEN][PAYLOAD...][CRC][ETX][TRAIL]
    if (len < 8) {
        return r; // çok kısa
    }
    if (frame[len - 1] != R07_TRAIL || frame[len - 2] != R07_ETX) {
        return r; // son ikili ETX/TRAIL değil
    }

    r.addr            = frame[0];
    r.cmd             = frame[1];
    r.nozzle_or_trans = frame[2];
    r.len_header      = frame[3];

    // Son 4 bayt: CRC? / CRC? / ETX / TRAIL
    // DC ailesi (0x31–0x3F) ve 0x65: payload TRANS'tan başlar (fr[2:-4]).
    // Diğerleri: NOZ+LEN sonrası başlar (fr[4:-4]).
    if ((0x31u <= r.cmd && r.cmd <= 0x3Fu) || r.cmd == 0x65u) {
        r.payload.assign(frame.begin() + 2, frame.end() - 4);
    } else {
        r.payload.assign(frame.begin() + 4, frame.end() - 4);
    }
    r.len_actual = r.payload.size();

    const bool suppress_len_warn = (0x30u <= r.cmd && r.cmd <= 0x3Fu);
    r.len_header_mismatch = (!suppress_len_warn) && (r.len_header != r.len_actual);

    // CRC sırası (LoHi | HiLo)
    std::uint8_t crc_lo = 0;
    std::uint8_t crc_hi = 0;
    if (crcOrder == R07CrcOrder::HiLo) {
        crc_hi = frame[len - 4];
        crc_lo = frame[len - 3];
    } else {
        crc_lo = frame[len - 4];
        crc_hi = frame[len - 3];
    }

    r.crc_rx = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(crc_hi) << 8) | crc_lo);
    r.crc_calc = crc16Ibm(frame.data(), len - 4);
    r.crc_ok   = (r.crc_rx == r.crc_calc);

    r.valid        = true;
    r.is_min_frame = false;
    return r;
}

// --- PumpR07Protocol member functions (yüksek seviye sarmalayıcı) ---

PumpR07Protocol::Frame
PumpR07Protocol::makeStatusPollFrame(PumpR07Protocol::Byte addr,
                                     PumpR07Protocol::Byte dcc) const
{
    // Python'daki _send_cd1 karşılığı:
    // Addr + CD1 komutu ile pompa durum sorgulama (STATUS).
    // Burada low-level helper olan makeR07Cd1Frame'i kullanıyoruz.
    return makeR07Cd1Frame(addr, dcc, R07CrcOrder::LoHi);
}

PumpR07Protocol::Frame
PumpR07Protocol::makeStatusPollFrame(PumpR07Protocol::Byte dcc) const
{
    // Varsayılan istasyon adresi ile durum sorgulama (CD1).
    return makeStatusPollFrame(R07_DEFAULT_ADDR, dcc);
}

PumpR07Protocol::Frame
PumpR07Protocol::makePresetVolumeFrame(double liters,
                                       PumpR07Protocol::Byte addr,
                                       PumpR07Protocol::Byte /*nozzle*/) const
{
    // Python _send_cd3_preset_volume karşılığı:
    //   - Güvenli aralık: 0.1 .. 250.0 L
    //   - Protokol: 8,00 L → 00000800 (x100 ölçek, BCD, 4 byte)
    //
    // Örnek:
    //   5X 30 03 04 00 00 08 00 CRCLO CRCHI 03 FA

    // 1) Litre değerini güvenli aralığa sıkıştır
    if (liters < 0.1) {
        liters = 0.1;
    }
    if (liters > 250.0) {
        liters = 250.0;
    }

    // 2) Litreyi x100 ölçeğe çevir (8.00 L → 800) ve en yakın tam sayıya yuvarla
    const double scaled = liters * 100.0;
    const std::uint32_t raw =
        static_cast<std::uint32_t>(scaled + 0.5); // round(liters*100)

    // 3) 4-byte BCD'e çevir
    const auto vol_bcd = intToBcd4(raw);

    // 4) Payload: VOL_BCD (4 byte)
    std::vector<std::uint8_t> payload;
    payload.reserve(4);
    for (std::size_t i = 0; i < vol_bcd.size(); ++i) {
        payload.push_back(vol_bcd[i]);
    }

    // 5) Header alanları
    constexpr std::uint8_t cmd        = 0x30; // CD ailesi
    constexpr std::uint8_t trans      = 0x03; // TRANS=0x03 (preset volume)
    constexpr std::uint8_t len_header = 0x04; // 4 byte BCD

    // 6) R07 uzun frame helper'ı ile tam çerçeve üret
    return makeR07Frame(
        addr,
        cmd,
        trans,
        len_header,
        payload,
        R07CrcOrder::LoHi);
}

PumpR07Protocol::Frame
PumpR07Protocol::makePresetVolumeFrame(double liters) const
{
    // Python tarafında nozzle şu an sabit 0x01; burada da aynı varsayılanı
    // kullanarak kısayol sağlıyoruz.
    constexpr PumpR07Protocol::Byte nozzle = 0x01;
    return makePresetVolumeFrame(liters, R07_DEFAULT_ADDR, nozzle);
}

PumpR07Protocol::Frame
PumpR07Protocol::makeTotalCountersFrame(PumpR07Protocol::Byte addr,
                                        PumpR07Protocol::Byte nozzle) const
{
    // Python _send_cd101_total_counters karşılığı:
    //
    //   [ADDR][0x3C][0x65][0x01][NOZ] + CRC + ETX + TRAIL
    //
    // Burada:
    //   - 0x3C  → TRANS alanı (sabit)
    //   - 0x65  → CD101 / total counters kodu
    //   - 0x01  → LEN (nozzle için 1 byte)
    //   - NOZ   → seçilen nozzle

    constexpr std::uint8_t trans      = 0x3C; // TRANS (Python'daki 0x3C)
    constexpr std::uint8_t sub_cmd    = 0x65; // CD101 / total counters
    constexpr std::uint8_t len_header = 0x01; // 1 byte nozzle

    std::vector<std::uint8_t> payload;
    payload.reserve(1);
    payload.push_back(static_cast<std::uint8_t>(nozzle));

    // makeR07Frame:
    //   [ADDR][cmd][nozzle_or_trans][LEN][PAYLOAD...] + CRC + ETX + TRAIL
    // Parametreleri:
    //   addr            → ADDR
    //   cmd             → 0x3C
    //   nozzle_or_trans → 0x65
    //   len_header      → 0x01
    //   payload         → [NOZ]
    //
    // Böylece Python'daki frame_wo_crc ile aynı dizi elde edilir:
    //   [ADDR][0x3C][0x65][0x01][NOZ]
    return makeR07Frame(
        addr,
        trans,
        sub_cmd,
        len_header,
        payload,
        R07CrcOrder::LoHi);
}

PumpR07Protocol::Frame
PumpR07Protocol::makeTotalCountersFrame(PumpR07Protocol::Byte nozzle) const
{
    // Varsayılan istasyon adresi ile belirli nozzle için sayaç sorgusu.
    return makeTotalCountersFrame(R07_DEFAULT_ADDR, nozzle);
}

PumpR07Protocol::Frame
PumpR07Protocol::makeTotalCountersFrame() const
{
    // Python implementasyonunda nozzle şu an 0x01 sabit; burada da en yaygın
    // kullanım için addr+nozzle varsayılanı sağlıyoruz.
    constexpr PumpR07Protocol::Byte nozzle = 0x01;
    return makeTotalCountersFrame(R07_DEFAULT_ADDR, nozzle);
}

PumpR07Protocol::Frame
PumpR07Protocol::makeMinPoll(PumpR07Protocol::Byte addr) const
{
    // Python'daki _send_min_poll karşılığı olan MIN-POLL frame'i üret.
    return makeR07MinPoll(addr);
}

PumpR07Protocol::Frame
PumpR07Protocol::makeMinAck(PumpR07Protocol::Byte addr) const
{
    // Python'daki _send_min_ack karşılığı olan MIN-ACK frame'i üret.
    return makeR07MinAck(addr);
}

void PumpR07Protocol::parseFrame(const PumpR07Protocol::Frame& frame)
{
    // Düşük seviye çözümleme: ham frame'i R07ParseResult'a çevir.
    const auto res = parseR07Frame(frame, R07CrcOrder::LoHi);

    // Geçersiz veya çözülemeyen çerçeveleri sessizce yutuyoruz.
    // İleride buraya LogManager ile log ekleyebiliriz.
    if (!res.valid) {
        return;
    }

    // MIN çerçeveler (MIN-POLL / MIN-ACK) için şimdilik özel callback yok.
    // İleride istenirse "hb tick" gibi bir callback eklenebilir.
    if (res.is_min_frame) {
        return;
    }

    // CRC hatalı ise şimdilik sessizce bırakıyoruz.
    // İleride LogManager üzerinden CRC error log'u yazacağız.
    if (!res.crc_ok) {
        return;
    }

    // Buradan sonra: "gerçek" R07 komutları.
    // Python tarafındaki _parse_and_update / _update_dc_from_payload
    // mantığını buraya adım adım taşıyacağız.
    switch (res.cmd) {
    case 0x30: { // CD1 / RETURN_STATUS: DC1 durum byte'ı CD ailesi ile döner
        // Örn: 50 30 01 01 [ST] CRC CRC 03 FA
        // parseR07Frame sonrası payload sadece [ST] olacak.
        if (res.len_actual == 1 && res.payload.size() == 1) {
            const std::uint8_t st = res.payload[0];

            PumpState mapped = PumpState::Unknown;
            // Dokümandaki "Pump Status" değerleri:
            // 00h NOT PROGRAMMED
            // 01h RESET
            // 02h AUTHORIZED
            // 04h FILLING
            // 05h FILLING COMPLETED
            // 06h MAX AMOUNT/VOLUME REACHED
            // 07h SWITCHED OFF
            // 0Bh PAUSED (→ SUSPENDED)
            switch (st) {
            case 0x00: mapped = PumpState::NotProgrammed;      break;
            case 0x01: mapped = PumpState::Reset;              break;
            case 0x02: mapped = PumpState::Authorized;         break;
            case 0x04: mapped = PumpState::Filling;            break;
            case 0x05: mapped = PumpState::FillingCompleted;   break;
            case 0x06: mapped = PumpState::MaxAmount;          break;
            case 0x07: mapped = PumpState::SwitchedOff;        break;
            case 0x0B: mapped = PumpState::Suspended;          break;
            default:   mapped = PumpState::Unknown;            break;
            }

            if (onStatus) {
                onStatus(mapped);
            }
        }
        break;
    }    
    case 0x01: { // Gerçek pompa DC1 (Pump Status, TRANS=0x01, 1 byte durum)
        if (res.len_actual == 1 && res.payload.size() == 1) {
            const std::uint8_t st = res.payload[0];

            PumpState mapped = PumpState::Unknown;
            // Dokümandaki "Pump Status" değerleri:
            // 00h NOT PROGRAMMED
            // 01h RESET
            // 02h AUTHORIZED
            // 04h FILLING
            // 05h FILLING COMPLETED
            // 06h MAX AMOUNT/VOLUME REACHED
            // 07h SWITCHED OFF
            // 0Bh PAUSED (→ SUSPENDED)
            switch (st) {
            case 0x00: mapped = PumpState::NotProgrammed;      break;
            case 0x01: mapped = PumpState::Reset;              break;
            case 0x02: mapped = PumpState::Authorized;         break;
            case 0x04: mapped = PumpState::Filling;            break;
            case 0x05: mapped = PumpState::FillingCompleted;   break;
            case 0x06: mapped = PumpState::MaxAmount;          break;
            case 0x07: mapped = PumpState::SwitchedOff;        break;
            case 0x0B: mapped = PumpState::Suspended;          break;
            default:   mapped = PumpState::Unknown;            break;
            }

            if (onStatus) {
                onStatus(mapped);
            }
        }
        break;
    }
    case 0xD1: { // Simülasyon DC1 state çerçevesi
        if (res.len_actual == 1 && res.payload.size() == 1) {
            const std::uint8_t st = res.payload[0];

            PumpState mapped = PumpState::Unknown;
            // Python'daki mapping:
            // raw 0x00:"IDLE"      → canon "RESET"
            // raw 0x01:"AUTHORIZED"→ canon "AUTHORIZED"
            // raw 0x02:"FILLING"   → canon "FILLING"
            // raw 0x03:"PAUSED"    → canon "SUSPENDED"
            // raw 0x04:"COMPLETE"  → canon "FILLING COMPLETED"
            switch (st) {
            case 0x00: mapped = PumpState::Reset;              break; // IDLE
            case 0x01: mapped = PumpState::Authorized;         break;
            case 0x02: mapped = PumpState::Filling;            break;
            case 0x03: mapped = PumpState::Suspended;          break; // PAUSED
            case 0x04: mapped = PumpState::FillingCompleted;   break; // COMPLETE
            default:   mapped = PumpState::Unknown;            break;
            }

            if (onStatus) {
                onStatus(mapped);
            }
        }
        break;
    }
    case 0xD4: { // Nozzle event (Python'daki cmd == 0xD4 bloğu)
        // Python tarafında:
        //   nozzle_flag = (payload[0] != 0x00)
        //   self.on_nozzle_event(nozzle_flag)
        //
        // C++ tarafında aynı bilgiyi NozzleEvent yapısı ile dışarı veriyoruz.
        if (res.len_actual == 1 && res.payload.size() == 1) {
            const bool nozzle_out = (res.payload[0] != 0x00);
            if (onNozzle) {
                NozzleEvent ev{};
                ev.nozzle_out = nozzle_out;
                onNozzle(ev);
            }
        }
        break;
    }
    case 0x37: { // DC3: nozzle + unit price (gerçek pompa DC3 çerçevesi)
        //
        // Sim log örneği:
        //   50 37 03 04 00 10 00 1D CRC CRC 03 FA
        //
        // parseR07Frame sonrası payload:
        //   [TRANS=0x03][LNG=0x04][DATA0..3]
        //
        // DATA[0..3] tipik olarak:
        //   [PRICE_BCD0][PRICE_BCD1][PRICE_BCD2][NOZIO]
        //
        // NOZIO baytında:
        //   - Bit4 = 1 → nozzle OUT
        //   - Bit4 = 0 → nozzle IN
        //
        // Biz şimdilik sadece nozzle_out bilgisini NozzleEvent ile dışarı veriyoruz.
        if (onNozzle && !res.payload.empty()) {
            const auto&        p = res.payload;
            const std::size_t  n = p.size();
            if (n >= 6) {
                const std::uint8_t trans = p[0];
                const std::uint8_t lng   = p[1];

                const std::size_t data_start = 2;                 // DATA başlangıcı
                const std::size_t data_end   = data_start + lng;  // DATA sonu (exclusive)

                // TRANS=0x03 ve en az 4 byte DATA bekliyoruz (3 byte price + 1 byte NOZIO).
                if (trans == 0x03u && lng >= 4u && data_end <= n) {
                    const std::uint8_t nozio =
                        p[data_end - 1]; // son DATA baytı: NOZIO

                    // Bit4: 1 → OUT, 0 → IN
                    const bool nozzle_out = ((nozio & 0x10u) != 0u);

                    NozzleEvent ev{};
                    ev.nozzle_out = nozzle_out;

                    onNozzle(ev);
                }
            }
        }
        break;
    }
    case 0x36: { // DC2: incremental sale (anlık VOL/AMO, x100 BCD)
        // Sim logundaki örnek:
        //   50 36 02 08 00 00 00 10 00 00 01 00 CRC CRC 03 FA
        // parseR07Frame sonrası payload:
        //   [TRANS=0x02][LNG=0x08][VOL_BCD(4)][AMO_BCD(4)]
        if (onFill && !res.payload.empty()) {
            const auto& p = res.payload;
            const std::size_t n = p.size();
            std::size_t i = 0;
            bool emitted = false;
            while (i + 2 <= n) {
                const std::uint8_t trans = p[i];
                const std::uint8_t lng   = p[i + 1];
                const std::size_t end    = static_cast<std::size_t>(i + 2 + lng);
                if (end > n) {
                    break; // eksik blok; devam etmeyelim
                }
                if (!emitted && trans == 0x02u && lng >= 0x08u) {
                    // DATA[0:4] → VOL, DATA[4:8] → AMO (her ikisi de BCD x100)
                    std::array<std::uint8_t, 4> vol_bcd{};
                    std::array<std::uint8_t, 4> amo_bcd{};
                    for (std::size_t k = 0; k < 4; ++k) {
                        vol_bcd[k] = p[i + 2 + k];
                        amo_bcd[k] = p[i + 2 + 4 + k];
                    }
                    const std::uint32_t vol_raw = bcd4ToInt(vol_bcd); // x100
                    const std::uint32_t amo_raw = bcd4ToInt(amo_bcd); // x100

                    FillInfo fi{};
                    fi.volume_l = static_cast<double>(vol_raw) / 100.0;
                    fi.amount   = static_cast<double>(amo_raw) / 100.0;

                    onFill(fi);
                    emitted = true;
                }
                i = end;
            }
        }
        break;
    }

    case 0x3D: { // TOTALIZER (TRANS/LNG/DATA içinde BCD x100 totaller)
        // Python tarafındaki mantığa paralel olarak:
        //  - payload: [TRANS][LNG][DATA...] bloklarından oluşur.
        //  - TOTALIZER için tipik blok: TRANS=0x01, LNG=0x08, DATA[0:4]=TOT_VOL, DATA[4:8]=TOT_AMO
        if (onTotals && !res.payload.empty()) {
            const auto& p = res.payload;
            const std::size_t n = p.size();
            std::size_t i = 0;
            bool emitted = false;
            while (i + 2 <= n) {
                const std::uint8_t trans = p[i];
                const std::uint8_t lng   = p[i + 1];
                const std::size_t end    = static_cast<std::size_t>(i + 2 + lng);
                if (end > n) {
                    break; // bozuk blok, daha ilerisine bakmayalım
                }
                if (!emitted && trans == 0x01u && lng >= 0x08u) {
                    // DATA[0:4] → total volume (x100, BCD), DATA[4:8] → total amount (x100, BCD)
                    if (lng >= 0x08u) {
                        std::array<std::uint8_t, 4> vol_bcd{};
                        std::array<std::uint8_t, 4> amo_bcd{};
                        for (std::size_t k = 0; k < 4; ++k) {
                            vol_bcd[k] = p[i + 2 + k];
                            amo_bcd[k] = p[i + 2 + 4 + k];
                        }
                        const std::uint32_t vol_raw = bcd4ToInt(vol_bcd); // x100
                        const std::uint32_t amo_raw = bcd4ToInt(amo_bcd); // x100
                        TotalCounters tc{};
                        tc.total_volume_l = static_cast<double>(vol_raw) / 100.0;
                        tc.total_amount   = static_cast<double>(amo_raw) / 100.0;
                        onTotals(tc);
                        emitted = true;
                    }
                }
                i = end;
            }
        }
        break;
    }
    case 0x3E: { // FILL-RECORD (saha: 50 3E 01 01 04 ... 03 FA)
        // Python _update_dc_from_payload içindeki FILL-RECORD decode'ına paralel:
        //  - payload: [TRANS][LNG][DATA...] blokları
        //  - satış satırı için tipik blok: TRANS=0x02, LNG=0x08
        //      DATA[0:4] → VOL (ml x100, BCD)  → litre için /100
        //      DATA[4:8] → AMO (para x100, BCD)→ para birimi için /100
        if (onFill && !res.payload.empty()) {
            const auto& p = res.payload;
            const std::size_t n = p.size();
            std::size_t i = 0;
            bool emitted = false;
            while (i + 2 <= n) {
                const std::uint8_t trans = p[i];
                const std::uint8_t lng   = p[i + 1];
                const std::size_t end    = static_cast<std::size_t>(i + 2 + lng);
                if (end > n) {
                    break; // eksik blok; daha ilerisine bakmayalım
                }
                if (!emitted && trans == 0x02u && lng >= 0x08u) {
                    // DATA[0:4] → VOL, DATA[4:8] → AMO (her ikisi de BCD x100)
                    std::array<std::uint8_t, 4> vol_bcd{};
                    std::array<std::uint8_t, 4> amo_bcd{};
                    for (std::size_t k = 0; k < 4; ++k) {
                        vol_bcd[k] = p[i + 2 + k];
                        amo_bcd[k] = p[i + 2 + 4 + k];
                    }
                    const std::uint32_t vol_raw = bcd4ToInt(vol_bcd); // x100
                    const std::uint32_t amo_raw = bcd4ToInt(amo_bcd); // x100
                    FillInfo fi{};
                    fi.volume_l = static_cast<double>(vol_raw) / 100.0;
                    fi.amount   = static_cast<double>(amo_raw) / 100.0;
                    onFill(fi);
                    emitted = true;
                }
                i = end;
            }
        }
        break;
    }
    default:
        // Henüz diğer komutları ayrıntılı parse etmiyoruz.
        // Bir sonraki adımlarda:
        //  - STATUS/DC ailesi için ek detaylar
        //  - FILL-RECORD için onFill(...)
        //  - 0x65/total counters için onTotals(...)
        //  - nozzle olayları için onNozzle(...)
        // doldurulacak.
        break;
    }
}

} // namespace recum12::hw

