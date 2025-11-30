#pragma once
#include <cstdint>
#include <functional>
#include <string>

// libnfc ileri bildirimleri (header'a <nfc/nfc.h> taşımıyoruz)
struct nfc_context;
struct nfc_device;

namespace recum12::rfid {

enum class ReaderState {
    Idle,
    WaitingCard,
    CardPresent,
    Error
};

struct CardEvent {
    std::string uid_hex;   // Ör: "04AABBCCDD..."
    std::string source;    // Ör: "pn532"
};

class Pn532Reader {
public:
    Pn532Reader();
    ~Pn532Reader();

    // PN532 cihazını (I2C/SPI/UART) açar. Şimdilik her zaman true dönen stub.
    bool open(const std::string& device);

    // Cihazı kapatır, kaynakları serbest bırakır.
    void close();

    // Kart okuma isteği: dışarıdan talep gelirse çağrılır
    // (örneğin pompadan "nozzle OUT" olayı geldiğinde).
    void requestRead();

    // Okuma tamamlandıktan veya iptal/timeout sonrası tekrar Idle duruma geçmek için.
    void cancelRead();

    // Periyodik olarak çağrılacak fonksiyon.
    // NOT: Yalnızca state_ == WaitingCard iken kart arama yapar;
    // Idle durumunda hiçbir şey yapmaz (talep yoksa okuma yok).
    void pollOnce();

    // Okuyucunun mevcut durumu.
    ReaderState state() const noexcept;

    // Kart okunduğunda tetiklenecek callback.
    std::function<void(const CardEvent&)>    onCardDetected;

    // Hata durumunda tetiklenecek callback (örn. iletişim hatası).
    std::function<void(const std::string&)>  onError;

private:
    ReaderState   state_{ReaderState::Idle};
    std::string   device_;

    // libnfc context / device
    nfc_context*  ctx_{nullptr};
    nfc_device*   dev_{nullptr};
};

} // namespace recum12::rfid
