#include "gui/rs485_gui_adapter.h"
#include "gui/MainWindow.h"
#include "gui/StatusMessageController.h"
#include "core/PumpRuntimeState.h"
#include "hw/PumpR07Protocol.h"

#include <glibmm/ustring.h>
#include <iostream>

namespace recum12::gui {

using recum12::hw::PumpState;
using Channel = StatusMessageController::Channel;

Rs485GuiAdapter::Rs485GuiAdapter(MainWindow& ui,
                                 StatusMessageController& status)
    : ui_(ui)
    , status_(status)
{
}

void Rs485GuiAdapter::apply(const ::core::PumpRuntimeState& s)
{
    const PumpState st          = s.pump_state;
    const bool      nozzle_out  { s.nozzle_out };
    const bool      auth_active { s.auth_active };

    // CORE sözleşmesi:
    //  - current_fill_volume_l / has_current_fill : devam eden satış seviyesi
    //  - last_fill_volume_l   / has_last_fill    : son tamamlanmış satış seviyesi
    const double    cur_l      = s.current_fill_volume_l;
    const bool      has_cur    = s.has_current_fill;
    const double    last_l     = s.last_fill_volume_l;
    const bool      has_last   = s.has_last_fill;

    // RFID / AUTH bilgileri
    const bool      auth_ok          = s.last_card_auth_ok;
    const std::string& user_id       = s.last_card_user_id;
    const std::string& plate         = s.last_card_plate;
    std::cout << "[GUI][PumpState] st=" << static_cast<int>(st)
              << " nozzle_out=" << nozzle_out
              << " cur_l=" << (has_cur ? cur_l : -1.0)
              << " last_l=" << (has_last ? last_l : -1.0)
              << std::endl;

    switch (st) {
    case PumpState::NotProgrammed:
    case PumpState::Reset:
    case PumpState::SwitchedOff:
    case PumpState::Suspended:
    case PumpState::Unknown:
    default:
        ui_.apply_idle_view(nozzle_out);
        if (nozzle_out) {
            // Excel satır 1
            status_.set_message(Channel::Pump,
                "Tabancayı depoya yerleştiriniz ve kartı Okutunuz");
        } else {
            // Excel satır 0
            status_.set_message(Channel::Pump,
                "İşlem Yapılabilir");
        }
        break;

    case PumpState::Authorized:
        // AUTH latch'i kapalı ise (timeout vb.) bu durumu IDLE gibi ele al.
        if (!auth_active) {
            ui_.apply_idle_view(nozzle_out);
            if (nozzle_out) {
                // Excel satır 1
                status_.set_message(Channel::Pump,
                    "Tabancayı depoya yerleştiriniz ve kartı Okutunuz");
            } else {
                // Excel satır 0
                status_.set_message(Channel::Pump,
                    "İşlem Yapılabilir");
            }
        } else {
            ui_.apply_auth_ok_view(nozzle_out);
            if (nozzle_out) {
                // Excel satır 3
                status_.set_message(Channel::Pump,
                    "Doluma başlayabilirsiniz.");
            } else {
                // Excel satır 2
                status_.set_message(Channel::Pump,
                    "Dolum Bekleniyor");
            }
        }
        break;

    case PumpState::Filling: {
        // Devam eden satış: mümkünse current, yoksa last
        const double use_l = has_cur ? cur_l : (has_last ? last_l : 0.0);
        ui_.apply_filling_view(nozzle_out, use_l);

        if (use_l > 0.0) {
            // Excel satır 4
            status_.set_message(Channel::Pump,
                "Dolum yapılıyor !!!");
        } else {
            // Excel satır 5
            status_.set_message(Channel::Pump,
                "Dolum başlatıldı...");
        }
        break;
    }

    case PumpState::FillingCompleted:
    case PumpState::MaxAmount: {
        // ÖZEL KURAL:
        //  - Eğer nozzle OUT ve current_fill mevcutsa (cur_l > 0),
        //    protokol state'i 5 olsa bile GUI'de "dolum devam ediyor"
        //    gibi göster. Bu, sahadaki pompanın DC1'i 5'e kilitlemesi
        //    durumunda da canlı level/ikon görmemizi sağlar.
        if (nozzle_out && has_cur && cur_l > 0.0) {
            const double use_l = cur_l;
            ui_.apply_filling_view(true, use_l);

            if (use_l > 0.0) {
                // Excel satır 4
                status_.set_message(Channel::Pump,
                    "Dolum yapılıyor !!!");
            } else {
                // Excel satır 5
                status_.set_message(Channel::Pump,
                    "Dolum başlatıldı...");
            }
        } else {
            // Satış tamamlandı: son tamamlanmış satışın litresini göster
            const double use_l = has_last ? last_l : 0.0;
            if (nozzle_out) {
                // Nozzle hâlâ dışarıda → kullanıcıya tabancayı depoya koy mesajı.
                ui_.apply_fill_done_view(true, use_l);
                // Excel satır 6
                status_.set_message(Channel::Pump,
                    "Dolum tamamlandı, tabancayı depoya yerleştiriniz.");
            } else {
                // Nozzle IN → satış tamamen bitti, sistemi IDLE kabul et.
                //  - last_l, fill_done_view ile lastfuel etiketine yazılıyor
                //  - level her zamanki gibi 0.0 yapılıyor
                ui_.apply_fill_done_view(false, use_l);
                // Artık yeni işleme hazırız.
                status_.set_message(Channel::Pump,
                    "İşlem Yapılabilir");
            }
        }
        break;
    }
    }

    // --- Kullanıcı / plaka bilgisini lbluserid'e yansıt ------------------
    //
    // Kural:
    //  - auth_active && auth_ok  ise:
    //      * user_id ve plate varsa: "USER / PLAKA"
    //      * sadece user_id varsa:   "USER"
    //      * sadece plate varsa:     "PLAKA"
    //  - aksi halde:
    //      * FillingCompleted / MaxAmount durumunda  → "::::"
    //      * diğer tüm durumlarda                    → "-------"
    Glib::ustring user_text;

    // Sadece plaka göster: yetkili bir auth varsa ve plate doluysa
    if (auth_active && auth_ok && !plate.empty()) {
        user_text = plate; // örn: "06TT987"
    }

    if (user_text.empty()) {
        if (st == PumpState::FillingCompleted || st == PumpState::MaxAmount) {
            user_text = "::::";
        } else {
            user_text = "-------";
        }
    }

    ui_.set_user_id(user_text);
}

} // namespace recum12::gui
