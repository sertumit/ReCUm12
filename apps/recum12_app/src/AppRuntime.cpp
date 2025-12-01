#include "AppRuntime.h"

#include <iostream>
#include <mutex>
#include <chrono>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <cctype>
#include <filesystem>
#include <glibmm/main.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <dirent.h>
#include <cstring>
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
// Belirli bir network arayüzü (örn: "eth0", "wlan0") için IPv4 adresini bulur.
// Bulamazsa "0.0.0.0" döner.
std::string get_ip_for_iface(const std::string& iface_name)
{
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == -1) {
        return "0.0.0.0";
    }

    std::string result = "0.0.0.0";

    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) {
            continue;
        }

        if (ifa->ifa_addr->sa_family != AF_INET) {
            continue; // sadece IPv4
        }

        if (!ifa->ifa_name) {
            continue;
        }

        if (iface_name != ifa->ifa_name) {
            continue;
        }

        char host[INET_ADDRSTRLEN] {};
        auto* sa = reinterpret_cast<sockaddr_in*>(ifa->ifa_addr);
        if (inet_ntop(AF_INET, &(sa->sin_addr), host, sizeof(host))) {
            result = host;
            break;
        }
    }

    freeifaddrs(ifaddr);
    return result;
}
// RS485 device fiziksel olarak var mı?
// 1) Önce verilen path'e bak (örn: /dev/ttyUSB0 veya by-id)
// 2) O yoksa /dev altında herhangi bir ttyUSB* görürsek "var" say.
bool is_rs485_device_present(const std::string& dev_path)
{
    // 1) Konfigüre edilen path
    if (!dev_path.empty()) {
        if (::access(dev_path.c_str(), F_OK) == 0) {
            return true;
        }
    }

    // 2) /dev altında genel ttyUSB taraması (sadece fiziksel varlık için)
    DIR* dir = ::opendir("/dev");
    if (!dir) {
        return false;
    }

    bool found = false;
    while (auto* ent = ::readdir(dir)) {
        if (!ent->d_name) {
            continue;
        }
        // Örn: ttyUSB0, ttyUSB1 ...
        if (std::strncmp(ent->d_name, "ttyUSB", 6) == 0) {
            found = true;
            break;
        }
    }

    ::closedir(dir);
    return found;
}

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
    using recum12::utils::Settings;
    using recum12::comm::NetworkStatus;

    // Uygulama ayarlarını (remote + rs485) yükle
    settings = Settings::loadDefault();

    // Başlangıçta network durumunu ve IP'leri bir kez oku
    NetworkStatus net = net_manager.queryStatus();

    const std::string eth_ip_str  = get_ip_for_iface("eth0");
    const std::string wifi_ip_str = get_ip_for_iface("wlan0");

    Glib::ustring eth_ip  = eth_ip_str.empty()  ? "0.0.0.0" : eth_ip_str;
    Glib::ustring wifi_ip = wifi_ip_str.empty() ? "0.0.0.0" : wifi_ip_str;

    // Debug için network durumunu logla
    std::cout << "[NET] status"
              << " eth="  << (net.ethernet_connected ? "UP" : "DOWN")
              << " wifi=" << (net.wifi_connected     ? "UP" : "DOWN")
              << " gsm="  << (net.gsm_connected      ? "UP" : "DOWN")
              << " gps="  << (net.gps_connected      ? "UP" : "DOWN")
              << std::endl;

    // İleride: bu bilgiler MainWindow ikonları ve IP label'larına yansıtılacak
    // (NetworkManager_Integration_Guide planına göre).
    // Başlangıç mesajı ve IDLE görünümü
    status_ctrl.set_message(StatusMessageController::Channel::Pump,
                            "İşlem Yapılabilir");
    ui.apply_idle_view(false);

    // Tarih / saat label'ları için periyodik clock başlat
    init_clock();

    // LogManager: appRoot tespiti ve scaffold oluşturma
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path cwd = fs::current_path(ec);
        if (!ec) {
            app_root = cwd.string();
        } else {
            app_root = recum12::utils::LogManager::detectAppRoot();
        }
    }
    std::cout << "[LogManager] app_root = " << app_root << std::endl;

    const bool scaffold_ok = recum12::utils::LogManager::ensureScaffold(app_root);
    std::cout << "[LogManager] ensureScaffold -> "
              << (scaffold_ok ? "OK" : "FAIL") << std::endl;

    // İlk test log kaydı: uygulama runtime'ı başladı.
    if (scaffold_ok) {
        recum12::utils::LogManager::UsageEntry e{};
        e.processId = 0;          // PC tarafı başlangıç kaydı
        e.logCode   = "APP_START";
        // timeStamp boş bırakılırsa appendUsage içinde ISO-8601 UTC doldurulur.
        e.sendOk    = "NA";

        const bool logged = log_manager.appendUsage(app_root, e);
        std::cout << "[LogManager] appendUsage(APP_START) -> "
                  << (logged ? "OK" : "FAIL") << std::endl;
        if (!logged) {
            std::cerr << "[LogManager] ERROR: APP_START satırı yazılamadı."
                      << " (logs/log_user/logs.csv)" << std::endl;
        }
    } else {
        std::cerr << "[LogManager] ERROR: ensureScaffold başarısız,"
                  << " APP_START log'u atlanıyor." << std::endl;
    }
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

            // PC tarafı usage log: satış bitişi (PumpOff_PC)
            {
                recum12::utils::LogManager::UsageEntry e{};
                // Şimdilik transaction id bilmiyoruz → 0 sabit
                e.processId = 0;

                // Son başarılı AUTH bilgisi varsa kullanıcı alanlarını doldur
                if (last_auth_ok) {
                    e.rfid      = last_auth_uid;
                    e.firstName = last_auth_first;
                    e.lastName  = last_auth_last;
                    e.plate     = last_auth_plate;
                    e.limit     = last_auth_limit;
                }

                // Dolum miktarı (litre)
                e.fuel    = sale_liters;
                e.logCode = "PumpOff_PC";
                // timeStamp boş → appendUsage içinde ISO-8601 UTC doldurulacak
                e.sendOk  = "NA";

                const bool ok = log_manager.appendUsage(app_root, e);
                if (!ok) {
                    std::cerr << "[LogManager] WARNING: PumpOff_PC usage log yazılamadı"
                              << " (fuel_l=" << sale_liters << ")\n";
                }
            }

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

        // AUTH cache'i güncelle (sadece başarılı AUTH için user bilgisi tut)
        last_auth_ok    = false;
        last_auth_uid.clear();
        last_auth_first.clear();
        last_auth_last.clear();
        last_auth_plate.clear();
        last_auth_limit = 0;

        // PC tarafı usage log: AUTH sonucu (AuthOK_PC / NoAuth_PC)
        recum12::utils::LogManager::UsageEntry e{};

        // Şimdilik transaction id bilgisi yok → 0 sabit
        e.processId = 0;

        // Kart UID (hex)
        e.rfid = a.uid_hex;

        // users.csv ile eşleşen kayıt varsa isim/plaka/limit'i doldur
        if (auto urec = user_manager.findByRfid(a.uid_hex)) {
            e.firstName = urec->firstName;
            e.lastName  = urec->lastName;
            e.plate     = urec->plate;
            e.limit     = urec->limit; // litre cinsinden int limit (users.csv ile uyumlu)

            if (a.authorized) {
                last_auth_ok    = true;
                last_auth_uid   = a.uid_hex;
                last_auth_first = urec->firstName;
                last_auth_last  = urec->lastName;
                last_auth_plate = urec->plate;
                last_auth_limit = urec->limit;
            }
        }

        // AUTH anında henüz dolum yok
        e.fuel    = 0.0;
        e.logCode = a.authorized ? "AuthOK_PC" : "NoAuth_PC";
        // timeStamp boş → appendUsage içinde ISO-8601 UTC doldurulacak
        e.sendOk  = "NA";

        const bool ok = log_manager.appendUsage(app_root, e);
        if (!ok) {
            std::cerr << "[LogManager] WARNING: AUTH usage log yazılamadı ("
                      << (a.authorized ? "AuthOK_PC" : "NoAuth_PC")
                      << ", uid=" << a.uid_hex << ")\n";
        }
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
    // Varsayılan path: eski by-id port (geri düşme için)
    std::string rs485_port =
        "/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_A5069RR4-if00-port0";

    // settings.rs485() içinden name == "pump" olan kaydı bul ve port'u kullan
    const auto& rs485_list = settings.rs485();
    for (const auto& cfg : rs485_list) {
        if (cfg.name == "pump" && !cfg.port.empty()) {
            rs485_port = cfg.port;
            break;
        }
    }

    pump.setDevice(rs485_port);

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
    // Başlangıç network + RS485 durumunu GUI'deki ikonlara ve IP label'larına yansıt
    const bool rs485_ok =
        pump.isOpen() && is_rs485_device_present(pump.device());

    ui.apply_network_status(
        net.ethernet_connected,
        net.wifi_connected,
        false,            // gsm_connected (şimdilik her zaman OFF)
        rs485_ok,
        eth_ip,
        wifi_ip,
        "0.0.0.0"         // GPRS IP: şimdilik her zaman 0.0.0.0
    );

    // RS485 başlangıç durumunu cache'le (hotplug mesajları için)
    last_rs485_ok = rs485_ok;
    // Network + RS485 durumunu 2 sn'de bir yenile
    init_network_poll();

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

        // PC tarafı usage log: GunOn_PC / GunOff_PC (sadece OUT/IN transition)
        {
            const bool prev = nozzle_out_logged;
            const bool curr = ev.nozzle_out;

            if (curr != prev) {
                recum12::utils::LogManager::UsageEntry e{};
                e.processId = 0;   // transaction id yok
                e.fuel      = 0.0; // sadece event

                // Son başarılı AUTH varsa user bilgilerini doldur
                if (last_auth_ok) {
                    e.rfid      = last_auth_uid;
                    e.firstName = last_auth_first;
                    e.lastName  = last_auth_last;
                    e.plate     = last_auth_plate;
                    e.limit     = last_auth_limit;
                }

                e.logCode = curr ? "GunOn_PC" : "GunOff_PC";
                e.sendOk  = "NA";

                const bool ok = log_manager.appendUsage(app_root, e);
                if (!ok) {
                    std::cerr << "[LogManager] WARNING: "
                              << (curr ? "GunOn_PC" : "GunOff_PC")
                              << " usage log yazılamadı\n";
                }

                nozzle_out_logged = curr;
            }
        }

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
    // Network polling timer'ını kapat
    if (net_poll_conn.connected()) {
        net_poll_conn.disconnect();
    }

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
void AppRuntime::init_network_poll()
{
    using recum12::comm::NetworkStatus;

    // 2 saniyede bir network + RS485 durumunu yenile
    net_poll_conn = Glib::signal_timeout().connect_seconds(
        sigc::slot<bool>([this]() {
            using recum12::gui::StatusMessageController;

            // Network durumu
            NetworkStatus net = net_manager.queryStatus();

            const std::string eth_ip_str  = get_ip_for_iface("eth0");
            const std::string wifi_ip_str = get_ip_for_iface("wlan0");

            Glib::ustring eth_ip  = eth_ip_str.empty()  ? "0.0.0.0" : eth_ip_str;
            Glib::ustring wifi_ip = wifi_ip_str.empty() ? "0.0.0.0" : wifi_ip_str;

            // RS485 durumu (adaptör çıkarılıp takıldığında device node da kaybolur)
            const bool rs485_ok =
                pump.isOpen() && is_rs485_device_present(pump.device());

            // İkon + IP label'larını güncelle
            ui.apply_network_status(
                net.ethernet_connected,
                net.wifi_connected,
                false,            // gsm_connected (şimdilik her zaman OFF)
                rs485_ok,
                eth_ip,
                wifi_ip,
                "0.0.0.0"         // GPRS IP: şimdilik hep 0.0.0.0
            );

            // RS485 hata / toparlama edge'leri:
            //  - true -> false : "Pompa Haberleşme Hatası"
            //  - false -> true : System kanalını temizle (önceki mesaj yapısına dön)
            if (!rs485_ok && last_rs485_ok) {
                status_ctrl.set_message(
                    StatusMessageController::Channel::System,
                    "Pompa Haberleşme Hatası");
            } else if (rs485_ok && !last_rs485_ok) {
                status_ctrl.clear_channel(StatusMessageController::Channel::System);
            }

            last_rs485_ok = rs485_ok;

            return true; // timer devam etsin
        }),
        2);
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
