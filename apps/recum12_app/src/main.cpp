#include <iostream>
#include <string>
#include <gtkmm.h>
#include <cstdint>

#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>
 
#include <glibmm/dispatcher.h>

#include "AppRuntime.h"
#include "hw/PumpInterfaceLvl3.h"
#include "hw/PumpR07Protocol.h"

#include "gui/MainWindow.h"
#include "gui/rs485_gui_adapter.h"
#include "gui/StatusMessageController.h"
#include "utils/Version.h"
#include "core/PumpRuntimeState.h"
#include "core/RfidAuthController.h"
#include "core/UserManager.h"
#include "rfid/Pn532Reader.h"

int main(int argc, char* argv[])
{
    auto app = Gtk::Application::create("tr.recum12.gui");

    Glib::RefPtr<Gtk::Builder> builder;
    const std::string glade_paths[] = {
        "modules/gui/resources/MainWindow12.glade",   // kaynak kökten çalıştırma
        "../modules/gui/resources/MainWindow12.glade" // build/ dizininden çalıştırma
    };

    bool glade_loaded = false;
    Glib::Error load_error;

    for (const auto& path : glade_paths) {
        try {
            builder = Gtk::Builder::create_from_file(path);
            glade_loaded = true;
            break;
        } catch (const Glib::Error& ex) {
            load_error = ex;
        }
    }

    if (!glade_loaded || !builder) {
        std::cerr << "Glade yüklenemedi: ";
        if (load_error.what().size() > 0) {
            std::cerr << load_error.what();
        } else {
            std::cerr << "bilinmeyen hata";
        }
        std::cerr << std::endl;
        return 1;
    }

    recum12::gui::MainWindow ui(builder);
    recum12::gui::StatusMessageController status_ctrl(ui);

    status_ctrl.set_message(recum12::gui::StatusMessageController::Channel::Pump,
                            "İşlem Yapılabilir");
    // Başlangıç görünümü: nozzle IN varsayarak IDLE view yükle
    ui.apply_idle_view(false);

    auto* window = ui.window();
    if (!window) {
        std::cerr << "Ana pencere oluşturulamadı." << std::endl;
        return 1;
    }

    // CSS: hem kaynak kökten hem build/ içinden çalıştırmaya göre path dene
    const std::string css_paths[] = {
        "modules/gui/resources/style.css",        // ReCUm12/ kökünden çalıştırma
        "modules/gui/resources/style_org.css",    // bozuk style.css için fallback
        "../modules/gui/resources/style.css",     // ReCUm12/build/ içinden çalıştırma
        "../modules/gui/resources/style_org.css"  // build/ için fallback
    };

    bool css_applied = false;
    for (const auto& css_path : css_paths) {
        if (ui.apply_css_from_file(css_path)) {
            css_applied = true;
            break;
        }
    }

    if (!css_applied) {
        std::cerr << "Uyarı: style.css uygulanamadı (hiçbir path çalışmadı)." << std::endl;
    }

    {
        using namespace recum12::utils;
        std::string version_text =
            std::string(APP_NAME) +
            " " +
            std::string(APP_VERSION);

        ui.set_version_text(version_text);
    }

    // App runtime wiring'ini AppRuntime içine taşı
    recum12::gui::AppRuntime runtime(ui);

    // Gtk main-loop (blocking)
    const int exit_code = app->run(*window);

    return exit_code;
}
