#pragma once

#include <memory>
#include <string>
#include <functional>
#include <cstdint>
#include <gtkmm.h>

namespace recum12::gui {

class MainWindow {
public:
    explicit MainWindow(const Glib::RefPtr<Gtk::Builder>& builder);

    // Glade içinden bulunan ana pencere
    Gtk::Window* window() const noexcept;

    // style.css dosyasını yükleyip uygular; başarı durumunu döndürür
    bool apply_css_from_file(const std::string& css_path);

    // Basit yardımcılar
    void set_status_message(const Glib::ustring& text);
    void set_user_id(const Glib::ustring& text);
    void set_version_text(const Glib::ustring& text);
    void set_level_value(double liters);
    void set_last_fuel_value(double liters);

    // Tarih / saat label'ları
    void set_date_text(const Glib::ustring& text);
    void set_time_text(const Glib::ustring& text);

    // Sayaç label'ları
    void set_wait_recs(std::uint64_t value);      // lblwaitrecs
    void set_vehicle_count(std::uint64_t value);  // lblvechs
    void set_repo_counter(double liters);         // lblcounter
    // AUTH butonu (btnauth) tıklandığında dışarıya haber veren handler
    void set_auth_handler(const std::function<void()>& handler);

    // GUI-RunTime-StateMachine-Rev02 referansına göre temel durum görünümleri
    // A) Açılış + IDLE (nozzle IN/OUT'a göre mesaj/ikon)
    void apply_idle_view(bool nozzle_out);
    // B) Kart yetkili / AUTH OK (AUTHORIZED state; nozzle'a göre mesaj/ikon)
    void apply_auth_ok_view(bool nozzle_out);
    // D) Dolum başladı (FILLING; level güncellenir)
    void apply_filling_view(bool nozzle_out, double current_liters);
    // Dolum bitti (FILLING COMPLETED / MAX AMOUNT → sonra IDLE’a dönecek)
    void apply_fill_done_view(bool nozzle_out, double last_liters);
    // Hata durumu (pompa error veya GUI tarafı hata mesajları)
    void apply_error_view(const Glib::ustring& message);

private:
    Glib::RefPtr<Gtk::Builder> builder_;
    Gtk::Window* root_window_ {nullptr};

    Gtk::Label* lblmsg_ {nullptr};
    Gtk::Label* lbluserid_ {nullptr};
    Gtk::Label* vers_label_ {nullptr};

    // Tarih / saat label'ları
    Gtk::Label* lbldate_ {nullptr};
    Gtk::Label* lbltime_ {nullptr};

    // Sayaç label'ları
    Gtk::Label* lblvechs_ {nullptr};
    Gtk::Label* lblcounter_ {nullptr};
    Gtk::Label* lblwaitrecs_ {nullptr};
    // Rev02 runtime state machine referansındaki ek widget'lar
    Gtk::Image* imgvhec_ {nullptr};
    Gtk::Image* imgpump_ {nullptr};
    Gtk::Image* imggun_ {nullptr};
    Gtk::Label* lbllevel_ {nullptr};
    Gtk::Label* lbllastfuel_ {nullptr};
    Gtk::Button* btnauth_ {nullptr};

    // AUTH butonu tıklandığında dışarıya haber veren handler
    std::function<void()> auth_handler_;

    void bind_widgets();
    void on_auth_clicked();    
};

} // namespace recum12::gui
