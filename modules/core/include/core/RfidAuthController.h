#pragma once

#include <functional>
#include <string>

#include "hw/PumpInterfaceLvl3.h"
#include "core/UserManager.h"
#include "rfid/Pn532Reader.h"
#include <chrono>

namespace recum12::core {

// RFID kart sonucu hakkında üst katmana iletilecek özet bilgi.
struct AuthContext
{
    std::string uid_hex;    // Okunan kart UID'si (HEX)
    bool        authorized{false}; // İleride UserManager ile doldurulacak
    std::string user_id;    // Kullanıcı ID / kısa isim
    std::string plate;      // Plaka vb. bilgi
    // Bu kart için tanımlı litre limiti (0.0 ise sınırsız)
    double      limit_liters{0.0};
};

class RfidAuthController
{
public:
    RfidAuthController() = default;
    ~RfidAuthController() = default;

    // Bağımlılıkları dışarıdan enjekte ediyoruz.
    void setPumpInterface(recum12::hw::PumpInterfaceLvl3* pump);
    void setReader(recum12::rfid::Pn532Reader* reader);
    void setUserManager(UserManager* users);

    // Bu fonksiyon, reader callback'lerini (onCardDetected / onError)
    // bu controller'a bağlar. setXXX çağrılarından sonra bir kez çağırılmalı.
    void attach();

    // --- Pompa tarafındaki nozzle olayları için üst katmandan çağrılacak API ---

    // Nozzle OUT olayı: pompa tabancasının yerinden alınması.
    // Bu durumda RFID kart okuma isteği başlatılır.
    void handleNozzleOut();

    // Nozzle IN veya satışın bittiği benzeri durumlar.
    // Kart okuma isteği iptal edilir, reader Idle'a döner.
    void handleNozzleInOrSaleFinished();

    // --- Üst katmana bilgi akışı için callback'ler ---

    // Kart sonucu: UID, (ileride) yetki bilgisi, kullanıcı/plaka gibi alanlar.
    std::function<void(const AuthContext&)> onAuthResult;

    // Kullanıcıya gösterilecek kısa mesajlar:
    // Ör: "Kart Okunuyor", "Yetkili Dolum Bekleniyor", "Yetkisiz Kart", "İşlem Yok" vb.
    std::function<void(const std::string&)> onAuthMessage;

    // Hata mesajları:
    // Ör: "RFID: nfc_init failed", "RFID: poll failed, will reconnect" vb.
    std::function<void(const std::string&)> onError;

private:
    recum12::hw::PumpInterfaceLvl3* pump_{nullptr};
    recum12::rfid::Pn532Reader*     reader_{nullptr};
    UserManager*                    users_{nullptr};

    bool waiting_for_card_{false};

    // Yetkili kart sonrası 10 sn boyunca yeni kart okuma isteğini engellemek için
    bool card_cooldown_active_{false};
    std::chrono::steady_clock::time_point card_cooldown_until_{};    
};

} // namespace recum12::core
