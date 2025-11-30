#pragma once

#include <gtkmm.h>
#include <string>

namespace recum12::gui {

class MainWindow;

// Ortak lblmsg alanını yönetecek temel controller.
// Şimdilik 4 kanal tanımlıyoruz; ileride genişletmesi kolay.
class StatusMessageController {
public:
    enum class Channel {
        Pump,    // Pompa / runtime state makinesi
        Auth,    // RFID / kullanıcı yetki mesajları
        System,  // Genel sistem mesajları (hata, uyarı vb.)
        Network  // Ağ / backend bağlantı mesajları
    };

    explicit StatusMessageController(MainWindow& ui);

    // Kanal bazında mesaj set/clear
    void set_message(Channel ch, const Glib::ustring& text);
    void clear_channel(Channel ch);

    // Tüm kanalları temizle
    void clear_all();

private:
    MainWindow&   ui_;
    Glib::ustring pump_msg_;
    Glib::ustring auth_msg_;
    Glib::ustring system_msg_;
    Glib::ustring network_msg_;

    // Kanal -> referans döndürmek için küçük yardımcı
    Glib::ustring& ref_for(Channel ch);

    // lblmsg içeriğini yeniden hesaplar.
    void update_label();
};

} // namespace recum12::gui
