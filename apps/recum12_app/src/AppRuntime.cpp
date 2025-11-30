#include "AppRuntime.h"

#include <iostream>
#include <mutex>
#include <chrono>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <cctype>
#include <glibmm/main.h>

namespace {

// CORE PumpRuntimeStore -> GUI thread köprüsü için basit cache
struct PumpStoreGuiCache {
    std::mutex             mtx;
    ::core::PumpRuntimeState last_state{};
    bool                   has_state{false};
};

PumpStoreGuiCache g_pump_store_gui_cache;

// RFID/Auth mesajları için GUI thread köprüsü
struct AuthGuiCache {
    std::mutex   mtx;
    std::string  last_msg;
    bool         has_msg{false};
};

AuthGuiCache g_auth_gui_cache;

// Worker thread'i
//  - MIN-POLL (heart-beat) gönderiyor
//  - RX verisini parse ediyor
// UI güncellemeleri ise dispatcher üzerinden main thread'e aktarılıyor.
//
void rs485_worker(recum12::hw::PumpInterfaceLvl3& pump,
                  std::atomic<bool>&               running)
{
    std::cout << "[RS485] worker started" << std::endl;

    using namespace std::chrono_literals;

    auto last_poll = std::chrono::steady_clock::now();

    while (running.load(std::memory_order_relaxed)) {
        if (pump.isOpen()) {
            // Mevcut byte'ları oku + frame'leri parse et.
            const bool had_activity = pump.pollOnceRx();

            if (had_activity) {
                std::cout << "[RS485] rx activity" << std::endl;
            }

            // ~1000 ms'de bir MIN-POLL (heart-beat) gönder:
            //  50 20 FA  → pompadan 50 70 FA (MIN-ACK) beklenir.
            const auto now = std::chrono::steady_clock::now();
            if (now - last_poll >= 1000ms) {
                // Artık sürekli CD1 spam etmiyoruz; sadece MIN-POLL.
                pump.sendMinPoll();
                last_poll = now;
            }
        }

        std::this_thread::sleep_for(20ms);
    }
}

// RFID worker:
//  - Pn532Reader.pollOnce() çağırır
//  - Kart algılama / hata callback'leri RfidAuthController üzerinden çalışır
void rfid_worker(recum12::rfid::Pn532Reader& reader,
                 std::atomic<bool>&           running)
{
    using namespace std::chrono_literals;

    std::cout << "[RFID] worker started" << std::endl;

    while (running.load(std::memory_order_relaxed)) {
        reader.pollOnce();
        std::this_thread::sleep_for(100ms);
    }
}

} // namespace

namespace recum12::gui {

RuntimeWorkers::RuntimeWorkers(recum12::hw::PumpInterfaceLvl3& p,
                               recum12::rfid::Pn532Reader&     r)
    : pump(p)
    , rfid_reader(r)
{
}

void RuntimeWorkers::start()
{
    using namespace std::chrono_literals;
    running.store(true, std::memory_order_relaxed);

    if (pump.isOpen()) {
        rs485_thread = std::thread(rs485_worker,
                                   std::ref(pump),
                                   std::ref(running));
    }

    // RFID worker her durumda çalışsın; Pn532Reader.pollOnce() içinde
    // open() / reconnect mantığı zaten var.
    rfid_thread = std::thread(rfid_worker,
                              std::ref(rfid_reader),
                              std::ref(running));
}

void RuntimeWorkers::stop()
{
    running.store(false, std::memory_order_relaxed);

    if (rs485_thread.joinable()) {
        rs485_thread.join();
    }
    if (rfid_thread.joinable()) {
        rfid_thread.join();
    }
}

AppRuntime::AppRuntime(MainWindow& ui_)
    : ui(ui_)
    , status_ctrl(ui)
    , rs485_adapter(ui, status_ctrl)
    , workers(pump, rfid_reader)
{
    using recum12::gui::StatusMessageController;

    // Başlangıç mesajı ve IDLE görünümü
    status_ctrl.set_message(StatusMessageController::Channel::Pump,
                            "İşlem Yapılabilir");
    ui.apply_idle_view(false);

    // Tarih / saat label'ları için periyodik clock başlat
    init_clock();
    // users.csv yükleme
    const std::string user_paths[] = {
        "configs/users.csv",
        "../configs/users.csv"
    };

    bool users_loaded = false;
    for (const auto& upath : user_paths) {
        if (user_manager.loadUsers(upath)) {
            std::cout << "[UserManager] loaded user db from: "
                      << upath << std::endl;
            users_loaded = true;
            break;
        }
    }
    if (!users_loaded) {
        std::cerr << "[UserManager] WARNING: users.csv could not be loaded; "
                     "all cards will be treated as unauthorized."
                  << std::endl;
    }

    // Sayaç dosyasını (repo_log.json) oku / oluştur ve GUI'ye yansıt
    load_repo_log();
    // PumpRuntimeStore → GUI köprüsü
    pump_store.onStateChanged = [this](const ::core::PumpRuntimeState& s) {
        {
            std::lock_guard<std::mutex> lock(g_pump_store_gui_cache.mtx);
            g_pump_store_gui_cache.last_state = s;
            g_pump_store_gui_cache.has_state  = true;
        }
        disp_store.emit();
    };

    disp_store.connect([this]() {
        ::core::PumpRuntimeState snapshot{};
        {
            std::lock_guard<std::mutex> lock(g_pump_store_gui_cache.mtx);
            if (!g_pump_store_gui_cache.has_state) {
                return;
            }
            snapshot = g_pump_store_gui_cache.last_state;
        }

        using recum12::gui::StatusMessageController;

        // AUTH OK görünümü (Authorized + nozzle_out == true) → level artışı için 10 sn bekle.
        if (!auth_waiting_for_fill &&
            snapshot.pump_state == ::core::PumpState::Authorized &&
            snapshot.nozzle_out) {

            auth_waiting_for_fill = true;

            if (auth_timeout_conn.connected()) {
                auth_timeout_conn.disconnect();
            }

            auth_timeout_conn =
                Glib::signal_timeout().connect_seconds(
                    [this]() {
                        // Hâlâ dolum başlamadıysa IDLE'a dön.
                        if (auth_waiting_for_fill) {
                            auth_waiting_for_fill = false;

                            // Core tarafında AUTH latch'ini kapat → Authorized state GUI'de IDLE gibi ele alınsın
                            pump_store.clearAuth();

                            // GUI tarafını temizle
                            status_ctrl.clear_all();
                            status_ctrl.set_message(
                                StatusMessageController::Channel::Pump,
                                "İşlem Yapılabilir");
                            ui.apply_idle_view(false);
                            ui.set_user_id("-------");
                        }
                        return false; // one-shot
                    },
                    10);
        }

        // Dolum başladıysa (FILLING veya seviye > 0) bekleme modunu iptal et.
        if (auth_waiting_for_fill) {
            if (snapshot.pump_state == ::core::PumpState::Filling ||
                snapshot.current_fill_volume_l > 0.0) {
                auth_waiting_for_fill = false;
                if (auth_timeout_conn.connected()) {
                    auth_timeout_conn.disconnect();
                }
            }
        }
        // ---- Satış bitişini yakala:
        //  - Önceki frame'de nozzle_out == true
        //  - Şimdiki frame'de  nozzle_out == false  (tabanca pompaya girdi)
        //  - Pompa state'i FILLING COMPLETED / MAX AMOUNT
        //  - last_fill dolu ve > 0.0
        //  - Kart yetkili (last_card_auth_ok)
        const bool completed_state =
            (snapshot.pump_state == ::core::PumpState::FillingCompleted) ||
            (snapshot.pump_state == ::core::PumpState::MaxAmount);

        if (last_nozzle_out &&
            !snapshot.nozzle_out &&
            completed_state &&
            snapshot.has_last_fill &&
            snapshot.last_fill_volume_l > 0.0 &&
            snapshot.last_card_auth_ok) {

            const double sale_liters = snapshot.last_fill_volume_l; // ölçek bozulmadan

            wait_recs  += 1;
            vhec_count += 1;
            repo_fill  += sale_liters;

            save_repo_log();
           refresh_counters_on_ui();
        }

        // Bir sonraki frame için nozzle geçmişini güncelle
        last_nozzle_out = snapshot.nozzle_out;

        rs485_adapter.apply(snapshot);
    });

    // RFID/Auth → GUI status label (lblmsg) köprüsü
    disp_auth.connect([this]() {
        using recum12::gui::StatusMessageController;

        std::string msg;
        {
            std::lock_guard<std::mutex> lock(g_auth_gui_cache.mtx);
            if (!g_auth_gui_cache.has_msg) {
                return;
            }
            msg = g_auth_gui_cache.last_msg;
        }

        if (msg.empty()) {
            return;
        }

        if (msg == "Yetkisiz Kullanıcı") {
            status_ctrl.set_message(StatusMessageController::Channel::Auth, msg);

            // 3 sn boyunca "Yetkisiz Kullanıcı" göster, sonra tam IDLE'a dön.
            if (unauth_timeout_conn.connected()) {
                unauth_timeout_conn.disconnect();
            }

            unauth_timeout_conn =
                Glib::signal_timeout().connect_seconds(
                    [this]() {
                        status_ctrl.clear_all();
                        status_ctrl.set_message(
                            StatusMessageController::Channel::Pump,
                            "İşlem Yapılabilir");
                        ui.apply_idle_view(false);
                        ui.set_user_id("-------");
                        return false; // one-shot
                    },
                    3);
        } else if (msg == "RFID hatası") {
            status_ctrl.set_message(StatusMessageController::Channel::System, msg);
        } else if (msg == "Yetkili Kullanıcı") {
            status_ctrl.clear_channel(StatusMessageController::Channel::Auth);
        } else if (msg == "Kart bekleniyor" ||
                   msg == "İşlem yok") {
            // Bu mesajlar sadece terminal/log için; AUTH kanalına dokunma.
        }
    });

    // RFID / AUTH bileşenlerinin bağlanması
    rfid_reader.open("");
    rfid_auth.setReader(&rfid_reader);
    rfid_auth.setUserManager(&user_manager);

    rfid_auth.onAuthResult = [this](const recum12::core::AuthContext& a) {
        ::core::AuthContext ctx{};
        ctx.uid_hex      = a.uid_hex;
        ctx.authorized   = a.authorized;
        ctx.user_id      = a.user_id;
        ctx.plate        = a.plate;
        // RFID tarafındaki limit bilgisini core store'a taşı
        ctx.limit_liters = a.limit_liters;
        pump_store.updateFromRfidAuth(ctx);
    };

    rfid_auth.onAuthMessage = [this](const std::string& msg) {
        std::cout << "[RFID/AuthMsg] " << msg << std::endl;

        {
            std::lock_guard<std::mutex> lock(g_auth_gui_cache.mtx);
            g_auth_gui_cache.last_msg = msg;
            g_auth_gui_cache.has_msg  = true;
        }
        disp_auth.emit();
    };

    rfid_auth.onError = [this](const std::string& msg) {
        std::cerr << "[RFID] " << msg << std::endl;

        {
            std::lock_guard<std::mutex> lock(g_auth_gui_cache.mtx);
            g_auth_gui_cache.last_msg = "RFID hatası";
            g_auth_gui_cache.has_msg  = true;
        }
        disp_auth.emit();
    };

    // RS485 Pump arayüzü kurulumu
    pump.setDevice("/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_A5069RR4-if00-port0");

    if (!pump.open()) {
        std::cerr << "Uyarı: RS485 portu açılamadı ("
                  << pump.device() << ")." << std::endl;
        ui.apply_error_view("RS485 portu açılamadı, pompa bağlantısı yok.");
        status_ctrl.set_message(StatusMessageController::Channel::System,
                                "RS485 portu açılamadı, pompa yok.");
    } else {
        std::cout << "[RS485] port opened: "
                  << pump.device()
                  << std::endl;
    }

    rfid_auth.setPumpInterface(&pump);
    rfid_auth.attach();

    pump.onStatus = [this](recum12::hw::PumpState st) {
        pump_store.updateFromPumpStatus(st);
    };

    pump.onFill = [this](const recum12::hw::FillInfo& fi) {
        pump_store.updateFromFill(fi);
        std::cout << "[PUMP] fill volume_l=" << fi.volume_l
                  << " amount=" << fi.amount
                  << std::endl;
        // Sayaçlar artık dolum BİTİŞİNDE (nozzle_out 1→0) güncelleniyor.
    };

    pump.onTotals = [this](const recum12::hw::TotalCounters& tc) {
        pump_store.updateFromTotals(tc);
        std::cout << "[PUMP] totals volume_l=" << tc.total_volume_l
                  << " amount=" << tc.total_amount
                  << std::endl;
    };

    pump.onNozzle = [this](const recum12::hw::NozzleEvent& ev) {
        pump_store.updateFromNozzle(ev);

        if (ev.nozzle_out) {
            rfid_auth.handleNozzleOut();
        } else {
            rfid_auth.handleNozzleInOrSaleFinished();
        }
    };

    // AUTH butonu handler'ı
    ui.set_auth_handler([this]() {
        std::cout << "[APP] AUTH handler: AUTHORIZE (DCC=0x06) gönderiliyor" << std::endl;
        pump.sendStatusPoll(0x06);
    });

    // Worker thread'lerini başlat
    workers.start();
}

AppRuntime::~AppRuntime()
{
    // Worker thread'lerini kapat ve RFID reader'ı kapat
    workers.stop();
    rfid_reader.close();
}

void AppRuntime::init_clock()
{
    // dd.mm.yyyy ve hh:mm formatında tarih/saat üret
    auto tick = [this]() -> bool {
        std::time_t now = std::time(nullptr);
        std::tm tm {};
        if (auto* p = std::localtime(&now)) {
            tm = *p;
        }

        char date_buf[16] {};
        char time_buf[16] {};

        std::strftime(date_buf, sizeof(date_buf), "%d.%m.%Y", &tm);
        std::strftime(time_buf, sizeof(time_buf), "%H:%M", &tm);

        ui.set_date_text(date_buf);
        ui.set_time_text(time_buf);

        return true; // timer devam etsin
    };

    // İlk değerleri hemen yaz
    tick();

    // Dakikada bir güncelle
    clock_conn = Glib::signal_timeout().connect_seconds(
        sigc::slot<bool>(tick),
        60);
}

void AppRuntime::refresh_counters_on_ui()
{
    ui.set_wait_recs(wait_recs);
    ui.set_vehicle_count(vhec_count);
    ui.set_repo_counter(repo_fill);
}

void AppRuntime::load_repo_log()
{
    const std::string candidate_paths[] = {
        "configs/repo_log.json",
        "../configs/repo_log.json"
    };

    bool loaded = false;

    for (const auto& path : candidate_paths) {
        std::ifstream in(path);
        if (!in) {
            continue;
        }

       std::ostringstream oss;
        oss << in.rdbuf();
        const std::string content = oss.str();

        auto parse_number = [&content](const std::string& key, double def_val) -> double {
            auto pos = content.find(key);
            if (pos == std::string::npos) return def_val;
            pos = content.find(':', pos);
            if (pos == std::string::npos) return def_val;
            ++pos;
            while (pos < content.size() &&
                   (content[pos] == ' ' || content[pos] == '\t')) {
                ++pos;
            }
            std::size_t end = pos;
            while (end < content.size() &&
                   (std::isdigit(static_cast<unsigned char>(content[end])) ||
                    content[end] == '.')) {
                ++end;
            }
            if (end <= pos) return def_val;
            return std::stod(content.substr(pos, end - pos));
        };

        try {
            double wait_d = parse_number("wait_recs", 0.0);
            double vhec_d = parse_number("vhec_count", 0.0);
            double repo_d = parse_number("repo_fill", 0.0);

            wait_recs  = static_cast<std::uint64_t>(wait_d);
            vhec_count = static_cast<std::uint64_t>(vhec_d);
            repo_fill  = repo_d;

            repo_log_path = path;
            loaded = true;
            break;
        } catch (...) {
            // parse hatası → diğer path'lere bak
        }
    }

    if (!loaded) {
        // Varsayılanlar ve yeni dosya oluşturma
        wait_recs  = 0;
        vhec_count = 0;
        repo_fill  = 0.0;
        repo_log_path = candidate_paths[0];
        save_repo_log();
    }

    // GUI sayaçlarını güncelle
    refresh_counters_on_ui();
}

void AppRuntime::save_repo_log()
{
    if (repo_log_path.empty()) {
        repo_log_path = "configs/repo_log.json";
    }

    std::ofstream out(repo_log_path, std::ios::trunc);
    if (!out) {
        std::cerr << "[AppRuntime] WARNING: repo_log.json yazılamadı: "
                  << repo_log_path << std::endl;
        return;
    }

    std::time_t now = std::time(nullptr);
    std::tm tm {};
    if (auto* p = std::localtime(&now)) {
        tm = *p;
    }

    char date_buf[16] {};
    std::strftime(date_buf, sizeof(date_buf), "%d.%m.%Y", &tm);

    out << "{\n";
    out << "  \"date\": \"" << date_buf << "\",\n";
    out << "  \"wait_recs\": " << wait_recs << ",\n";
    out << "  \"vhec_count\": " << vhec_count << ",\n";
    out << "  \"repo_fill\": " << std::fixed << std::setprecision(1) << repo_fill << "\n";
    out << "}\n";
}
} // namespace recum12::gui
