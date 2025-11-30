#include "rfid/Pn532Reader.h"

#include <sstream>
#include <iomanip>
#include <iostream>

// libnfc
#include <nfc/nfc.h>

namespace recum12::rfid {

namespace {

// Eski RFIDReader::to_hex ile aynı mantık:
std::string to_hex(const unsigned char* data, size_t len)
{
    std::ostringstream ss;
    ss << std::uppercase << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i) {
        ss << std::setw(2) << static_cast<unsigned int>(data[i]);
    }
    return ss.str();
}

} // namespace

Pn532Reader::Pn532Reader() = default;

Pn532Reader::~Pn532Reader()
{
    close();
}

bool Pn532Reader::open(const std::string& device)
{
    device_ = device;

    // 1) Context yoksa kur
    if (!ctx_) {
        std::cout << "[PN532] nfc_init()\n";
        nfc_init(&ctx_);
        if (!ctx_) {
            if (onError) onError("RFID: nfc_init failed");
            state_ = ReaderState::Error;
            return false;
        }
    }

    // 2) Device yoksa aç
    if (!dev_) {
        dev_ = nfc_open(ctx_, device_.empty() ? nullptr : device_.c_str());
        if (!dev_) {
            if (onError) onError("RFID: nfc_open failed");
            state_ = ReaderState::Error;
            return false;
        }

        if (nfc_initiator_init(dev_) < 0) {
            if (onError) onError("RFID: nfc_initiator_init failed, closing device");
            nfc_close(dev_);
            dev_ = nullptr;
            state_ = ReaderState::Error;
            return false;
        }

        // PN532/libnfc ayarları – eski RFIDReader.cpp ile uyumlu
        nfc_device_set_property_bool(dev_, NP_AUTO_ISO14443_4, false);
        nfc_device_set_property_bool(dev_, NP_HANDLE_CRC,       true);
        nfc_device_set_property_bool(dev_, NP_HANDLE_PARITY,    true);
        nfc_device_set_property_bool(dev_, NP_ACTIVATE_FIELD,   true);
        nfc_device_set_property_bool(dev_, NP_INFINITE_SELECT,  false);

        std::cout << "[PN532] device opened OK\n";
    }

    // Başarılı init → Idle'a dön
    state_ = ReaderState::Idle;
    return true;
}

void Pn532Reader::close()
{
    if (dev_) {
        nfc_close(dev_);
        dev_ = nullptr;
    }
    if (ctx_) {
        nfc_exit(ctx_);
        ctx_ = nullptr;
    }
    state_ = ReaderState::Idle;
}

void Pn532Reader::requestRead()
{
    // Yalnızca Idle durumundan kart bekleme moduna geç.
    // Hata durumunda (Error) üst katman yeniden open() çağırmalıdır.
    if (state_ == ReaderState::Idle) {
        state_ = ReaderState::WaitingCard;
    }
}

void Pn532Reader::cancelRead()
{
    // Kart bekleme veya kart bulundu durumundan tekrar Idle'a dön.
    if (state_ == ReaderState::WaitingCard ||
        state_ == ReaderState::CardPresent) {
        state_ = ReaderState::Idle;
    }
}

void Pn532Reader::pollOnce()
{
    // 1) Kart okuma isteği yoksa hiçbir şey yapma
    if (state_ != ReaderState::WaitingCard) {
        return;
    }

    // 2) Gerekirse cihazı yeniden açmayı dene
    if (!ctx_ || !dev_) {
        if (!open(device_)) {
            // open() hata mesajını onError ile iletmiş olacak
            return;
        }
    }

    // 3) Kart seç – önce ISO14443A, sonra ISO14443B
    nfc_target nt{};
    const nfc_modulation nmA = { NMT_ISO14443A, NBR_106 };
    const nfc_modulation nmB = { NMT_ISO14443B, NBR_106 };

    int res = nfc_initiator_select_passive_target(dev_, nmA, nullptr, 0, &nt);
    if (res == 0) {
        res = nfc_initiator_select_passive_target(dev_, nmB, nullptr, 0, &nt);
    }

    if (res < 0) {
        // Hata: cihazı kapat, durumu Error yap ve üst kata haber ver
        if (onError) onError("RFID: poll failed, will reconnect");
        nfc_close(dev_);
        dev_ = nullptr;
        state_ = ReaderState::Error;
        return;
    }

    if (res == 0) {
        // Kart yok → WaitingCard durumunda kal, sadece sessizce çık
        return;
    }

    // 4) Kart bulundu → UID çek
    std::string uid;
    switch (nt.nm.nmt) {
        case NMT_ISO14443A:
            if (nt.nti.nai.szUidLen > 0) {
                uid = to_hex(nt.nti.nai.abtUid, nt.nti.nai.szUidLen);
            }
            break;
        case NMT_ISO14443B:
            // PUPI genelde 4 byte
            uid = to_hex(nt.nti.nbi.abtPupi, 4);
            break;
        default:
            uid = "UNKNOWN";
            break;
    }

    if (!uid.empty()) {
        CardEvent ev;
        ev.uid_hex = uid;
        ev.source  = "pn532";

        // GUI / üst katman burada:
        //  - user listesinde yetki kontrolü yapacak
        //  - lblmsg / lbluserid / imgvhec güncelleyecek
        if (onCardDetected) {
            onCardDetected(ev);
        }

        // Kart bulundu → CardPresent durumuna geç.
        // Üst katman işini bitirince cancelRead() çağırıp Idle'a döndürecek.
        state_ = ReaderState::CardPresent;
    }

    // 5) Seçimi bırak
    nfc_initiator_deselect_target(dev_);
}

ReaderState Pn532Reader::state() const noexcept
{
    return state_;
}

} // namespace recum12::rfid
