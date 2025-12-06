#include "core/RfidAuthController.h"
#include <iostream>
#include <chrono>

namespace recum12::core {

// Not: UserManager entegrasyonu eklenmiştir; artık users_ set ise kartlar
// gerçekten users.csv üzerinden doğrulanır.
using recum12::hw::PumpInterfaceLvl3;
using recum12::rfid::Pn532Reader;
using recum12::rfid::CardEvent;

void RfidAuthController::setPumpInterface(PumpInterfaceLvl3* pump)
{
    pump_ = pump;
}

void RfidAuthController::setReader(Pn532Reader* reader)
{
    reader_ = reader;
}

void RfidAuthController::setUserManager(UserManager* users)
{
    users_ = users;
}

void RfidAuthController::attach()
{
    if (!reader_) {
        if (onError) {
            onError("RfidAuthController: reader is not set");
        }
        return;
    }

    // Pn532Reader kart okuma callback'i:
    reader_->onCardDetected = [this](const CardEvent& ev) {
        // Bu kart okuma olayı, pompa tarafından gerçekten talep edilmiş mi?
        // (handleNozzleOut → requestRead sonrası) sadece bu durumda GUI'de
        // kart mesajı göstereceğiz; diğer tüm kart okumaları yalnızca log'a gider.
        const bool gui_auth_flow = waiting_for_card_;
        waiting_for_card_ = false;

        // Debug: kart UID'sini ham haliyle logla
        std::cout << "[RFID/Auth] card detected, raw uid_hex="
                  << ev.uid_hex << std::endl;

        AuthContext ctx;
        ctx.uid_hex = ev.uid_hex;

        // UserManager varsa: gerçek kullanıcı doğrulaması
        if (users_) {
            auto u = users_->findByRfid(ev.uid_hex);
            if (u) {
                ctx.authorized = true;
                // İş kuralı: user_id alanına numeric userId string olarak yazıyoruz.
                ctx.user_id    = std::to_string(u->userId);
                ctx.plate      = u->plate;
                // users.csv'deki litre limitini bağlama taşı.
                // 0 veya negatifse "limitsiz" kabul edilecek.
                ctx.limit_liters = u->limit_liters;
            } else {
                ctx.authorized = false;
                ctx.user_id.clear();
                ctx.plate.clear();
                std::cout << "[RFID/Auth] uid not found in users.csv"
                          << std::endl;
            }
        } else {
            // UserManager henüz bağlanmamışsa eski saha test davranışı:
            // tüm kartları "yetkili" kabul et.
            ctx.authorized = true;
            }

        // Not: Auth sonucu her durumda üst katmana bildirilir; böylece
        // logs.csv gibi katmanlar kart okumasını kaydedebilir.
        if (onAuthResult) { onAuthResult(ctx); }
        //}

        if (onAuthMessage && gui_auth_flow) {
            if (ctx.authorized) {
                // Üst katman bu mesajı lblmsg vb. alana basabilir.
                onAuthMessage("Yetkili Kullanıcı");
            } else {
                onAuthMessage("Yetkisiz Kullanıcı");
            }
        }

        // Kart yetkili ise pompaya AUTHORIZE (CD1, DCC=0x06) isteği gönder.
        // Not: Bu istek GUI tarafında sadece pompa gerçekten kart talep etmişse
        // (gui_auth_flow==true) mesajlanır; aksi halde yalnızca protokol düzeyinde kalır.
        if (ctx.authorized && pump_) {
            std::cout << "[RFID/Auth] authorized → sending AUTHORIZE (DCC=0x06)"
                      << std::endl;
            pump_->sendStatusPoll(0x06);

            // Ekrana "pompa authorize edildi" mesajı da yalnızca gerçek auth
            // akışında gösterilir.
            if (onAuthMessage && gui_auth_flow) {
                onAuthMessage("Yetkili kart → pompa AUTHORIZE edildi");
            }

            // 10 sn boyunca tekrar kart okuma kapalı (cooldown başlat)
            card_cooldown_active_ = true;
            card_cooldown_until_ =
                std::chrono::steady_clock::now() + std::chrono::seconds(10);
        }
    };

    // Pn532Reader hata callback'i:
    reader_->onError = [this](const std::string& msg) {
        waiting_for_card_ = false;
        if (onError) {
            onError(msg);
        }
        if (onAuthMessage) {
            onAuthMessage("RFID hatası");
        }
    };
}

void RfidAuthController::handleNozzleOut()
{
    if (!reader_) {
        if (onError) {
            onError("RfidAuthController::handleNozzleOut: reader is not set");
        }
        return;
    }

    // Yetkili bir karttan sonra 10 sn boyunca yeni kart okuma kapalı (cooldown).
    if (card_cooldown_active_) {
        auto now = std::chrono::steady_clock::now();
        if (now < card_cooldown_until_) {
            std::cout << "[RFID/Auth] handleNozzleOut ignored (cooldown active)"
                      << std::endl;
            return;
        } else {
            // Cooldown süresi doldu, normal akışa devam edebiliriz.
            card_cooldown_active_ = false;
        }
    }

    // Nozzle OUT → kart okuma isteği başlat.
    reader_->requestRead();
    waiting_for_card_ = true;

    if (onAuthMessage) {
        // Üst katman bunu "Kart Okutun" veya benzeri bir mesaja çevirebilir.
        onAuthMessage("Kart bekleniyor");
    }
}

void RfidAuthController::handleNozzleInOrSaleFinished()
{
    if (reader_) {
        reader_->cancelRead();
    }
    waiting_for_card_ = false;

    if (onAuthMessage) {
        // Tipik idle mesajı: "İşlem Yok".
        onAuthMessage("İşlem yok");
    }
}

} // namespace recum12::core
