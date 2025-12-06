#ifndef CORE_PUMPRUNTIMESTATE_H
#define CORE_PUMPRUNTIMESTATE_H

#include <functional>
#include <string>

// Pompa tarafındaki temel tipler (PumpState, FillInfo, TotalCounters, NozzleEvent)
// hw modülündeki R07 protokol tanımlarından gelir.
#include "hw/PumpR07Protocol.h"

namespace core
{
// core katmanında doğrudan PumpState/FillInfo/TotalCounters/NozzleEvent
// isimlerini kullanabilmek için hw namespace'indeki tipleri yeniden adlandırıyoruz.
using recum12::hw::PumpState;
using recum12::hw::FillInfo;
using recum12::hw::TotalCounters;
using recum12::hw::NozzleEvent;

// RFID tarafının pompa state'ine enjekte edeceği bağlam
struct AuthContext
{
    bool        authorized{false};
    std::string uid_hex;
    std::string user_id;
    std::string plate;
    double      limit_liters{0.0};
};

// Uygulama genelinde "tek hakikat" olacak runtime state
struct PumpRuntimeState
{
    // Pompa ana durumu
    PumpState      pump_state{};      // Son STATUS frame'den gelen durum
    bool           nozzle_out{false}; // Son nozzle event'e göre

    // Dolum (satış) bilgileri
    //
    // Not:
    //  - Şimdilik gelen her FillInfo hem "anlık" hem de "son tamamlanmış"
    //    satış olarak ele alınıyor; ileride CD2 vs 3E ayrımı geldiğinde
    //    current/last mantığı netleştirilecek.
    FillInfo       last_fill{};         // Son satış / dolum bilgisi
    double         current_fill_volume_l{0.0};
    bool           has_current_fill{false};
    double         last_fill_volume_l{0.0};
    bool           has_last_fill{false};

    TotalCounters  totals{};            // TOTALIZER'dan gelen sayaçlar

    // RFID & AUTH
    std::string    last_card_uid;
    bool           last_card_auth_ok{false};
    std::string    last_card_user_id;
    std::string    last_card_plate;

    // Limit bilgisi (RFID karttan gelen)
    //  - limit_liters             : bu kart için tanımlı limit (0.0 ise limitsiz kabul)
    //  - has_limit                : limit tanımlı mı?
    //  - remaining_limit_liters   : devam eden satışta kalan limit (has_limit yoksa 0.0)
    double         limit_liters{0.0};
    bool           has_limit{false};
    double         remaining_limit_liters{0.0};
    // Latch’ler
    bool           auth_active{false};
    bool           sale_active{false};

    // R07 DC2 Generic Sale Detection için ek satış latch'leri
    bool           sale_armed{false};              // GunOut + AUTH sonrası, DC2 bekleniyor
    bool           fill_first_nonzero_seen{false}; // Bu satışta DC2 > 0 ilk kez görüldü mü?
};

// Thread-safe olmayan, basit bir store iskeleti.
// İlk etapta yalnızca RS485 callback'leri tarafından beslenecek,
// GUI/NET tarafı ise onStateChanged üzerinden dinleyecek.
class PumpRuntimeStore
{
public:
    PumpRuntimeStore() = default;

    // Mevcut state'e salt-okuma erişim
    const PumpRuntimeState& state() const noexcept;

    // Tüm state'i varsayılana sıfırlar ve değişikliği bildirir
    void reset();

    // RS485 protokol callback'lerinden çağrılacak güncelleyiciler
    void updateFromPumpStatus(PumpState status);
    void updateFromFill(const FillInfo& fill);
    void updateFromTotals(const TotalCounters& totals);
    void updateFromNozzle(const NozzleEvent& ev);
    void updateFromRfidAuth(const AuthContext& auth);

    // AUTH latch'ini temizle (timeout vb. durumlarda IDLE'a dönmek için)
    void clearAuth();
    // State değiştiğinde tetiklenecek callback.
    // Not: Şimdilik her update çağrısından sonra tetiklenir;
    // ileride gerekirse "değişti mi?" kontrolü eklenebilir.
    std::function<void(const PumpRuntimeState&)> onStateChanged;

private:
    PumpRuntimeState s_{};

    // FillInfo.volume_l totalizer gibi davrandığı durumlar için:
    //  - fill_baseline_volume_l_ : satış başlangıcındaki total seviye
    //  - have_fill_baseline_     : baseline alındı mı?
    //  - last_sale_volume_l_     : son satışın litre miktarı
    double fill_baseline_volume_l_{0.0};
    bool   have_fill_baseline_{false};
    double last_sale_volume_l_{0.0};
    void notifyStateChanged();
};

} // namespace core

#endif // CORE_PUMPRUNTIMESTATE_H
