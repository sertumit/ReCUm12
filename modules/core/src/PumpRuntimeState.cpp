#include "core/PumpRuntimeState.h"

namespace core
{

const PumpRuntimeState& PumpRuntimeStore::state() const noexcept
{
    return s_;
}

void PumpRuntimeStore::reset()
{
    s_ = PumpRuntimeState{};
    fill_baseline_volume_l_ = 0.0;
    have_fill_baseline_     = false;
    last_sale_volume_l_     = 0.0;
    // limit alanları PumpRuntimeState default ctor'uyla zaten sıfırlanıyor
    notifyStateChanged();
}

void PumpRuntimeStore::updateFromPumpStatus(PumpState status)
{
    s_.pump_state = status;

    // pompa durumuna göre sale_active latch'ini güncelle
    switch (status) {
    case PumpState::Filling:
        // FILLING'e girerken yeni satış için baseline'ı sıfırla
        if (!s_.sale_active) {
            have_fill_baseline_ = false;
            last_sale_volume_l_ = 0.0;
        }
        s_.sale_active = true;
        break;
    case PumpState::FillingCompleted:
    case PumpState::MaxAmount:
    case PumpState::Reset:
    case PumpState::SwitchedOff:
        s_.sale_active = false;
        break;
    default:
        // Diğer durumlarda latch'i olduğu gibi bırak
        break;
    }

    notifyStateChanged();
}

void PumpRuntimeStore::updateFromFill(const FillInfo& fill)
{
    // Ham FillInfo'yu sakla (genellikle totalizer seviyesi)
    s_.last_fill = fill;

    const double total = fill.volume_l;

    if (s_.sale_active) {
        // İlk FillInfo geldiğinde baseline al
        if (!have_fill_baseline_) {
            fill_baseline_volume_l_ = total;
            have_fill_baseline_     = true;
        }

        double cur = total - fill_baseline_volume_l_;
        if (cur < 0.0) {
            cur = 0.0;
        }

        s_.current_fill_volume_l = cur;
        s_.has_current_fill      = true;

        // Son satışın litre miktarı
        last_sale_volume_l_   = cur;
        s_.last_fill_volume_l = cur;
        s_.has_last_fill      = true;


        // Limit takibi: limit_liters > 0 ise kalan litreyi hesapla
        if (s_.limit_liters > 0.0) {
            double remaining = s_.limit_liters - last_sale_volume_l_;
            if (remaining < 0.0) {
                remaining = 0.0;
            }
            s_.remaining_limit_liters = remaining;
        } else {
            s_.remaining_limit_liters = 0.0;
        }
    } else {
        // Aktif satış yokken gelen FillInfo'lar:
        // current_fill'i sıfırda tut, last_fill_volume_l önceki satış miktarı olsun.
        s_.current_fill_volume_l = 0.0;
        s_.has_current_fill      = false;
        // s_.last_fill_volume_l / has_last_fill'e dokunmuyoruz.
        // Aktif satış yokken:
        //  - limit tanımlıysa (has_limit) kalan limit = tam limit
        //  - aksi halde 0.0
        if (s_.has_limit) {
            s_.remaining_limit_liters = s_.limit_liters;
        } else {
            s_.remaining_limit_liters = 0.0;
        }
    }

    notifyStateChanged();
}

void PumpRuntimeStore::updateFromTotals(const TotalCounters& totals)
{
    s_.totals = totals;
    notifyStateChanged();
}

void PumpRuntimeStore::updateFromNozzle(const NozzleEvent& ev)
{
    const bool prev_nozzle_out = s_.nozzle_out;
    s_.nozzle_out = ev.nozzle_out;

    // Nozzle OUT → IN geçişi:
    //  - Dolum döngüsü kapanıyor kabul edip current_fill'i sıfırla.
    if (prev_nozzle_out && !s_.nozzle_out) {
        s_.current_fill_volume_l = 0.0;
        s_.has_current_fill      = false;
        // Bir sonraki satışta yeniden baseline alınacak
        have_fill_baseline_      = false;
    }

    notifyStateChanged();
}

void PumpRuntimeStore::updateFromRfidAuth(const AuthContext& auth)
{
    s_.last_card_uid       = auth.uid_hex;
    s_.last_card_user_id   = auth.user_id;
    s_.last_card_plate     = auth.plate;
    s_.last_card_auth_ok   = auth.authorized;
    s_.auth_active         = auth.authorized;

    // Karttan gelen limit bilgisini store'a taşı
    s_.limit_liters = auth.limit_liters;
    s_.has_limit    = (auth.limit_liters > 0.0);

    // Yeni AUTH sonrası, henüz satış başlamadığı için kalan limit = tam limit
    if (s_.has_limit) {
        s_.remaining_limit_liters = s_.limit_liters;
    } else {
        s_.remaining_limit_liters = 0.0;
    }

    notifyStateChanged();
}

void PumpRuntimeStore::clearAuth()
{
    // AUTH latch'ini kapat, son kartı "yetkisiz" say.
    s_.auth_active       = false;
    s_.last_card_auth_ok = false;

    // Limit bilgisini de sıfırla
    s_.limit_liters            = 0.0;
    s_.has_limit               = false;
    s_.remaining_limit_liters  = 0.0;
    // (uid / user_id / plate alanlarını şimdilik koruyoruz;
    //  GUI tarafı plaka label'ını zaten kendisi "-------" yapıyor.)

    notifyStateChanged();
}

void PumpRuntimeStore::notifyStateChanged()
{
    if (onStateChanged) {
        onStateChanged(s_);
    }
}

} // namespace core
