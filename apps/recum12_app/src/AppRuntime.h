#pragma once

#include <atomic>
#include <thread>
#include <sigc++/connection.h>

#include <glibmm/dispatcher.h>
#include <string>
#include <cstdint>
#include "utils/Settings.h"
#include "comm/NetworkManager.h"

#include "gui/MainWindow.h"
#include "gui/StatusMessageController.h"
#include "gui/rs485_gui_adapter.h"
#include "core/PumpRuntimeState.h"
#include "core/RfidAuthController.h"
#include "core/UserManager.h"
#include "hw/PumpInterfaceLvl3.h"
#include "rfid/Pn532Reader.h"
#include "utils/LogManager.h"

namespace recum12::gui {

// RS485 + RFID worker thread'lerinin yaşam döngüsünü yöneten küçük yardımcı yapı.
//  - running: her iki worker için ortak "çalışıyor mu" bayrağı
//  - start(): pump açıksa RS485 worker'ı, her durumda RFID worker'ı başlatır
//  - stop(): bayrağı kapatır ve join() çağırarak thread'leri toplar
struct RuntimeWorkers {
    recum12::hw::PumpInterfaceLvl3& pump;
    recum12::rfid::Pn532Reader&     rfid_reader;
    std::atomic<bool>               running{false};
    std::thread                     rs485_thread;
    std::thread                     rfid_thread;

    RuntimeWorkers(recum12::hw::PumpInterfaceLvl3& p,
                   recum12::rfid::Pn532Reader&     r);

    void start();
    void stop();
};

// Uygulama runtime'ını temsil eden basit iskelet.
//  - PumpRuntimeStore, RS485 pump, RFID, UserManager, GUI adapter ve
//    dispatcher wiring'i burada toplanır.
//  - Worker thread'lerinin lifecycle'ı RuntimeWorkers üzerinden yönetilir.
struct AppRuntime {
    MainWindow&               ui;
    StatusMessageController   status_ctrl;
    Rs485GuiAdapter           rs485_adapter;

    ::core::PumpRuntimeStore  pump_store;

    // LogManager entegrasyonu (logs/log_user/logs.csv için)
    recum12::utils::LogManager log_manager;
    std::string                app_root;

    // Son başarılı AUTH için basit cache (usage log enrich)
    bool                       last_auth_ok{false};
    std::string                last_auth_uid;
    std::string                last_auth_first;
    std::string                last_auth_last;
    std::string                last_auth_plate;
    int                        last_auth_limit{0};
    
    recum12::comm::NetworkManager    net_manager;
    
    recum12::utils::Settings  settings;
    
    recum12::core::UserManager        user_manager;
    recum12::rfid::Pn532Reader        rfid_reader;
    recum12::core::RfidAuthController rfid_auth;

    recum12::hw::PumpInterfaceLvl3    pump;

    Glib::Dispatcher          disp_store;
    Glib::Dispatcher          disp_auth;

    RuntimeWorkers            workers;

    // AUTH OK (Authorized + nozzle_out) sonrası 10 sn level bekleme durumu
    bool                      auth_waiting_for_fill{false};
    sigc::connection          auth_timeout_conn;
    sigc::connection          unauth_timeout_conn; // "Yetkisiz Kullanıcı" 3 sn timeout

    // Sayaç / log durumu (configs/repo_log.json)
    std::string               repo_log_path;
    std::uint64_t             wait_recs{0};   // lblwaitrecs
    std::uint64_t             vhec_count{0};  // lblvechs
    double                    repo_fill{0.0}; // lblcounter (litre)

    // Tarih / saat label'ları için periyodik timer
    sigc::connection          clock_conn;
    sigc::connection          net_poll_conn;
    // Nozzle OUT→IN geçişini takip etmek için önceki nozzle durumu
    bool                      last_nozzle_out{false};

    // RS485 nozzle event log (GunOn/GunOff) için önceki durum
    bool                      nozzle_out_logged{false};

    // RS485 health durumu (ikon + mesaj için edge detection)
    bool                      last_rs485_ok{false};
    explicit AppRuntime(MainWindow& ui_);
    ~AppRuntime();

    // Yardımcılar (AppRuntime.cpp içinde tanımlı)
    void init_clock();
    void load_repo_log();
    void save_repo_log();
    void refresh_counters_on_ui();
    void init_network_poll();    
};

} // namespace recum12::gui
