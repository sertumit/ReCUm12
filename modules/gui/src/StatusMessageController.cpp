#include "gui/StatusMessageController.h"
#include "gui/MainWindow.h"

namespace recum12::gui {

StatusMessageController::StatusMessageController(MainWindow& ui)
    : ui_(ui)
{
}

Glib::ustring& StatusMessageController::ref_for(Channel ch)
{
    switch (ch) {
    case Channel::Pump:    return pump_msg_;
    case Channel::Auth:    return auth_msg_;
    case Channel::System:  return system_msg_;
    case Channel::Network: return network_msg_;
    }
    // Derleyiciyi susturmak için; buraya normalde düşmemeliyiz.
    return system_msg_;
}

void StatusMessageController::set_message(Channel ch, const Glib::ustring& text)
{
    // Önce ilgili kanaldaki cache'i güncelle
    Glib::ustring& slot = ref_for(ch);
    slot = text;

    // Özel kural:
    //  - AUTH kanalında "Yetkisiz Kullanıcı" aktifken,
    //    PUMP kanalından gelen mesajlar lblmsg içeriğini güncellemesin.
    //
    //  Böylece log'da ve ekranda:
    //    "Dolum tamamlandı..." → ardından "Yetkisiz Kullanıcı"
    //  sıralamasından sonra tekrar pompa mesajları görünmez.
    if (ch == Channel::Pump && auth_msg_ == "Yetkisiz Kullanıcı") {
        // Cache güncel; ama label'i elleme.
        return;
    }

    update_label();
}

void StatusMessageController::clear_channel(Channel ch)
{
    ref_for(ch).clear();
    update_label();
}

void StatusMessageController::clear_all()
{
    pump_msg_.clear();
    auth_msg_.clear();
    system_msg_.clear();
    network_msg_.clear();
    update_label();
}

void StatusMessageController::update_label()
{
    // Öncelik sırası (Excel’e göre güncellenmiş):
    //
    //  0) AUTH: "Yetkisiz Kullanıcı" → tam override, ekranda sadece bu yazılır.
    //  1) System  → kritik hata / uyarı
    //  2) Network → bağlantı problemleri vb.
    //  3) Pump + (varsa) Auth birleşimi

    Glib::ustring final_text;

    // 0) "Yetkisiz Kullanıcı" tüm diğer kanalları ezer
    if (auth_msg_ == "Yetkisiz Kullanıcı") {
        final_text = auth_msg_;
    }
    else if (!system_msg_.empty()) {
        final_text = system_msg_;
    }
    else if (!network_msg_.empty()) {
        final_text = network_msg_;
    }
    else {
        // Pump + Auth birlikte gösterilsin:
        if (!pump_msg_.empty() && !auth_msg_.empty()) {
            final_text = pump_msg_ + " | " + auth_msg_;
        } else if (!pump_msg_.empty()) {
            final_text = pump_msg_;
        } else if (!auth_msg_.empty()) {
            final_text = auth_msg_;
        } else {
            final_text.clear();
        }
    }

    ui_.set_status_message(final_text);
}

} // namespace recum12::gui
