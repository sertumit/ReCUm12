#include "gui/MainWindow.h"

#include <iostream>
#include <vector>
#include <sstream>
#include <gdkmm/pixbuf.h>

// Not: ikon yüklerken hata alırsak stderr'e loglayıp sessizce devam ediyoruz.
namespace recum12::gui {

MainWindow::MainWindow(const Glib::RefPtr<Gtk::Builder>& builder)
    : builder_(builder)
{
    if (!builder_) {
        std::cerr << "MainWindow: builder is null" << std::endl;
        return;
    }

    // Ana pencere: önce isimle dene, yoksa ilk Window nesnesine düş.
    builder_->get_widget("MainWindow", root_window_);
    if (!root_window_) {
        auto objects = builder_->get_objects();
        for (const auto& obj : objects) {
            if (!obj) {
                continue;
            }
            if (auto* win = dynamic_cast<Gtk::Window*>(obj.get())) {
                root_window_ = win;
                break;
            }
        }
    }

    if (!root_window_) {
        std::cerr << "MainWindow: root window bulunamadı." << std::endl;
        return;
    }

    // Pencereyi ekran ortasına yerleştir (480x320 UI için daha düzgün başlasın)
    root_window_->set_default_size(480, 320);
    root_window_->set_position(Gtk::WIN_POS_CENTER);
    // Sabit boyutlu çalışalım; ikon / label değişimleri pencereyi genişletmesin.
    root_window_->set_resizable(false);

    bind_widgets();
}

Gtk::Window* MainWindow::window() const noexcept
{
    return root_window_;
}

void MainWindow::bind_widgets()
{
    if (!builder_) {
        return;
    }

    builder_->get_widget("lblmsg", lblmsg_);
    builder_->get_widget("lbluserid", lbluserid_);
    builder_->get_widget("lblvers", vers_label_);

    // Tarih / saat
    builder_->get_widget("lbldate", lbldate_);
    builder_->get_widget("lbltime", lbltime_);

    // Sayaç label'ları
    builder_->get_widget("lblvechs",    lblvechs_);
    builder_->get_widget("lblcounter",  lblcounter_);
    builder_->get_widget("lblwaitrecs", lblwaitrecs_);
    builder_->get_widget("imgvhec", imgvhec_);
    builder_->get_widget("imgpump", imgpump_);
    builder_->get_widget("imggun", imggun_);
    builder_->get_widget("lbllevel", lbllevel_);
    builder_->get_widget("lastfuel", lbllastfuel_);
    builder_->get_widget("btnauth", btnauth_);

    // İkonlar için sabit boyut; böylece ON/OFF/SUSPEND ikonları değişse de
    // pencere yatayda zıplamaz.
    if (imgvhec_) imgvhec_->set_size_request(48, 48);  // truck icon
    if (imgpump_) imgpump_->set_size_request(64, 64);  // station icon
    if (imggun_)  imggun_->set_size_request(64, 64);   // gun icon
    // Auth butonu (şimdilik kart okuma simülasyonu gibi kullanıyoruz)
    if (btnauth_) {
        btnauth_->signal_clicked().connect(
            sigc::mem_fun(*this, &MainWindow::on_auth_clicked));
    }
}

bool MainWindow::apply_css_from_file(const std::string& css_path)
{
    auto css = Gtk::CssProvider::create();
    try {
        css->load_from_path(css_path);
    } catch (const Glib::Error& ex) {
        std::cerr << "CSS yüklenemedi (" << css_path << "): " << ex.what() << std::endl;
        return false;
    }

    auto screen = Gdk::Screen::get_default();
    if (!screen) {
        std::cerr << "CSS için ekran bulunamadı." << std::endl;
        return false;
    }

    Gtk::StyleContext::add_provider_for_screen(
        screen,
        css,
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION
    );

    return true;
}

void MainWindow::set_status_message(const Glib::ustring& text)
{
    if (lblmsg_) {
        lblmsg_->set_text(text);
    }

    // Test için loglayalım:
    std::cout << "[GUI][lblmsg] <= \"" << text << "\"" << std::endl;
}

void MainWindow::set_user_id(const Glib::ustring& text)
{
    if (lbluserid_) {
        lbluserid_->set_text(text);
    }
}

void MainWindow::set_version_text(const Glib::ustring& text)
{
    if (vers_label_) {
        vers_label_->set_text(text);
    }
}

void MainWindow::set_auth_handler(const std::function<void()>& handler)
{
    // Dış katmandan (main.cpp / adapter vb.) AUTH isteği için callback alır.
    // on_auth_clicked içinde, butona basıldığında bu handler tetiklenecek.
    auth_handler_ = handler;
}

// Küçük yardımcı: ikon dosyasını güvenli şekilde yükle
namespace {
    void set_image_safe(Gtk::Image* img, const std::string& path)
    {
        if (!img) {
            return;
        }
        try {
            auto pix = Gdk::Pixbuf::create_from_file(path);
            img->set(pix);
        }
        catch (const Glib::Error& ex) {
            std::cerr << "[GUI] icon yüklenemedi (" << path << "): "
                      << ex.what() << std::endl;
        }
    }
}

// A) Açılış + IDLE
//  - imgvhec : truck_mix_OFF_48x48
//  - imgpump : station_off_64x64 (error yoksa)
//  - imggun  : nozzle IN → gun_pump_off_64x64, OUT → gun_pump_suspend_64x64
//  - lblmsg  : IN  → "Tabancayı depoya yerleştiriniz ve kartı Okutunuz"
//              OUT → "Kartı Okutunuz"
//  - lbluserid : "-------"
//  - lbllevel  : 0.0
void MainWindow::apply_idle_view(bool nozzle_out)
{
    set_image_safe(imgvhec_, "modules/gui/resources/truck_mix_OFF_48x48.png");
    set_image_safe(imgpump_, "modules/gui/resources/station_off_64x64.png");

    if (nozzle_out) {
        set_image_safe(imggun_, "modules/gui/resources/gun_pump_suspend_64x64.png");
    } else {
        set_image_safe(imggun_, "modules/gui/resources/gun_pump_off_64x64.png");
    }

    if (lbllevel_) {
        lbllevel_->set_text("0.0");
    }
}

// B) Kart yetkili / AUTH OK
//  - imgvhec : truck_mix_ON_48x48
//  - imgpump : authorize gönderildiyse station_suspend_64x64
//  - imggun  : IN → off / OUT → suspend
//  - lblmsg  : IN → "Tabancayı depoya yerleştiriniz."
//              OUT → "Doluma başlayabilirsiniz."
//  - lbluserid : şimdilik placeholder
//  - lbllevel  : 0.0
void MainWindow::apply_auth_ok_view(bool nozzle_out)
{
    set_image_safe(imgvhec_, "modules/gui/resources/truck_mix_ON_48x48.png");
    set_image_safe(imgpump_, "modules/gui/resources/station_suspend_64x64.png");

    if (nozzle_out) {
        set_image_safe(imggun_, "modules/gui/resources/gun_pump_suspend_64x64.png");
    } else {
        set_image_safe(imggun_, "modules/gui/resources/gun_pump_off_64x64.png");
    }

    if (lbllevel_) {
        lbllevel_->set_text("0.0");
    }
}

// D) Dolum başladı (FILLING)
//  - imgvhec : truck_mix_ON_48x48
//  - imgpump : station_on_64x64
//  - imggun  : gun_pump_on_64x64
//  - lblmsg  : level > 0 ise "Dolum yapılıyor !!!"
//  - lbllevel: current_liters
void MainWindow::apply_filling_view(bool nozzle_out, double current_liters)
{
    (void) nozzle_out; // Dolumda zaten OUT bekleniyor; şimdilik kullanmıyoruz.

    set_image_safe(imgvhec_, "modules/gui/resources/truck_mix_ON_48x48.png");
    set_image_safe(imgpump_, "modules/gui/resources/station_on_64x64.png");
    set_image_safe(imggun_,  "modules/gui/resources/gun_pump_on_64x64.png");

    set_level_value(current_liters);
}

// Dolum bitti (FILLING COMPLETED / MAX AMOUNT/VOLUME)
//  - imgvhec : truck_mix_OFF_48x48
//  - imgpump : station_off_64x64
//  - imggun  : gun_pump_off_64x64
//  - lblmsg  : "Dolum Tamamlandı" (Rev02'de 5 sn göster → timer sonraki adım)
//  - lbluserid : "::::"
//  - lbllevel  : 0.0 (last_liters daha sonra lastfuel benzeri etikete aktarılabilir)
void MainWindow::apply_fill_done_view(bool nozzle_out, double last_liters)
{
    set_image_safe(imgvhec_, "modules/gui/resources/truck_mix_OFF_48x48.png");
    set_image_safe(imgpump_, "modules/gui/resources/station_off_64x64.png");
    // Burada artık gerçek nozzle durumuna göre ikon seçiyoruz:
    //  - nozzle_out == true  → tabanca hâlâ dışarıda: suspend ikonu
    //  - nozzle_out == false → tabanca pompada: off ikonu
    if (nozzle_out) {
        set_image_safe(imggun_, "modules/gui/resources/gun_pump_suspend_64x64.png");
    } else {
        set_image_safe(imggun_, "modules/gui/resources/gun_pump_off_64x64.png");
    }

    // Son dolum miktarını "lastfuel" alanına yaz.
    set_last_fuel_value(last_liters);

    if (lbllevel_) {
        lbllevel_->set_text("0.0");
    }
}

// Genel hata görünümü:
//  - imgpump : station_err_64x64
//  - lblmsg  : verilen hata metni
void MainWindow::apply_error_view(const Glib::ustring& message)
{
    set_image_safe(imgpump_, "modules/gui/resources/station_err_64x64.png");

    set_status_message(message);
}
void MainWindow::set_level_value(double liters)
{
    if (!lbllevel_) {
        return;
    }

    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(1);
    oss << liters;

    lbllevel_->set_text(oss.str());
}

void MainWindow::set_last_fuel_value(double liters)
{
    if (!lbllastfuel_) {
        return;
    }

    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(1);
    oss << liters;

    lbllastfuel_->set_text(oss.str());
}

void MainWindow::set_date_text(const Glib::ustring& text)
{
    if (lbldate_) {
        lbldate_->set_text(text);
    }
}

void MainWindow::set_time_text(const Glib::ustring& text)
{
    if (lbltime_) {
        lbltime_->set_text(text);
    }
}

void MainWindow::set_wait_recs(std::uint64_t value)
{
    if (!lblwaitrecs_) {
        return;
    }
    lblwaitrecs_->set_text(std::to_string(value));
}

void MainWindow::set_vehicle_count(std::uint64_t value)
{
    if (!lblvechs_) {
        return;
    }
    lblvechs_->set_text(std::to_string(value));
}

void MainWindow::set_repo_counter(double liters)
{
    if (!lblcounter_) {
        return;
    }

    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss.precision(1);
    oss << liters;

    lblcounter_->set_text(oss.str());
}
void MainWindow::on_auth_clicked()
{
    // Şimdilik sadece log + status label güncellemesi.
    // Gerçek akışta buradan state machine / pompa auth tetiklenecek.
    std::cout << "[GUI] AUTH butonuna basıldı (btnauth)" << std::endl;

    // Bu bir auth mesajı, pompa durumundan ayrı ele alınacak.
    set_status_message("Yetki isteği gönderiliyor (AUTH)..."); // geçici mesaj

    // Dışarıya haber ver (ör: pompa AUTHORIZE komutu gönderecek katman)
    if (auth_handler_) {
        try {
            auth_handler_();
        } catch (...) {
            // UI thread'i düşürmemek için exception yutuyoruz.
        }
    }
}

} // namespace recum12::gui