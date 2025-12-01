#include "utils/LogManager.h"

#include <chrono>
#include <cstdlib>   // std::getenv
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace {

namespace fs = std::filesystem;

// ISO-8601 UTC timestamp üretir (örn: 2025-12-01T01:23:45Z)
std::string isoNowUtc()
{
    using clock = std::chrono::system_clock;
    const auto now = clock::now();
    const auto t   = clock::to_time_t(now);

    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

// Basit CSV escape:
// - İçinde virgül veya çift tırnak yoksa direkt döner
// - Aksi halde string'i "..." içine alır ve içteki " karakterlerini "" ile kaçışlar
std::string csvEscape(const std::string& value)
{
    bool needsQuotes = false;
    for (char c : value) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') {
            needsQuotes = true;
            break;
        }
    }

    if (!needsQuotes) {
        return value;
    }

    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (char c : value) {
        if (c == '"') {
            escaped.push_back('"'); // çift tırnak kaçışı
        }
        escaped.push_back(c);
    }
    escaped.push_back('"');
    return escaped;
}

// Basit CSV satırı parse edici.
// - Çift tırnaklı alanları destekler
// - "" içindeki çift tırnakları tek tırnak olarak yorumlar
std::vector<std::string> parseCsvLine(const std::string& line)
{
    std::vector<std::string> cols;
    std::string current;
    current.reserve(line.size());

    bool inQuotes = false;

    for (std::size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (inQuotes) {
            if (c == '"') {
                // "" ise gerçek bir " karakteri
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    current.push_back('"');
                    ++i;
                } else {
                    inQuotes = false;
                }
            } else {
                current.push_back(c);
            }
        } else {
            if (c == '"') {
                inQuotes = true;
            } else if (c == ',') {
                cols.push_back(current);
                current.clear();
            } else {
                current.push_back(c);
            }
        }
    }

    cols.push_back(current);
    return cols;
}

// Klasör mevcut değilse oluşturur
bool ensureDir(const fs::path& p)
{
    std::error_code ec;
    if (fs::exists(p, ec)) {
        return fs::is_directory(p, ec);
    }
    return fs::create_directories(p, ec);
}

// Dosya yoksa header ile oluşturur
bool ensureFileWithHeader(const fs::path& filePath, const std::string& headerLine)
{
    std::error_code ec;
    if (fs::exists(filePath, ec)) {
        return true;
    }

    std::ofstream ofs(filePath);
    if (!ofs.is_open()) {
        return false;
    }

    ofs << headerLine << '\n';
    return static_cast<bool>(ofs);
}

} // anonymous namespace

namespace recum12::utils {

// ---------------------------------------------------------------------
// 1) Common appRoot & scaffold
// ---------------------------------------------------------------------

std::string LogManager::detectAppRoot()
{
    // 1) RECUM_APPROOT ortam değişkeni
    if (const char* env = std::getenv("RECUM_APPROOT")) {
        if (env[0] != '\0') {
            fs::path candidate(env);
            std::error_code ec;
            if (fs::exists(candidate, ec) && fs::is_directory(candidate, ec)) {
                return candidate.string();
            }
        }
    }

    // 2) current_path()'ten yukarı "configs/default_settings.json" araması
    std::error_code ec;
    fs::path current = fs::current_path(ec);
    if (ec) {
        // current_path alınamazsa env veya başka bir şey de bulamadık; fallback
        return fs::path(".").string();
    }

    fs::path probe = current;
    for (int i = 0; i < 5; ++i) {
        fs::path cfg = probe / "configs" / "default_settings.json";
        if (fs::exists(cfg, ec) && fs::is_regular_file(cfg, ec)) {
            return probe.string();
        }
        if (!probe.has_parent_path()) {
            break;
        }
        probe = probe.parent_path();
    }

    // 3) Bulunamazsa current_path
    return current.string();
}

bool LogManager::ensureScaffold(const std::string& appRoot)
{
    try {
        fs::path root(appRoot);

        fs::path logsDir     = root / "logs";
        fs::path logsUserDir = logsDir / "log_user";
        fs::path configsDir  = root / "configs";

        if (!ensureDir(logsDir)) {
            return false;
        }
        if (!ensureDir(logsUserDir)) {
            return false;
        }
        if (!ensureDir(configsDir)) {
            return false;
        }

        // infra log dosyası (şema dokümanda daha sonra zenginleşecek)
        const fs::path infraCsv = logsDir / "recumLogs.csv";
        const std::string infraHeader = "timeStamp,level,code,message,details";
        if (!ensureFileWithHeader(infraCsv, infraHeader)) {
            return false;
        }

        // usage log dosyası (pompa user transaction log'u)
        const fs::path usageCsv = logsUserDir / "logs.csv";
        const std::string usageHeader =
            "processId,rfid,firstName,lastName,plate,limit,fuel,logCode,timeStamp,sendOk";
        if (!ensureFileWithHeader(usageCsv, usageHeader)) {
            return false;
        }

        return true;
    } catch (...) {
        return false;
    }
}

// ---------------------------------------------------------------------
// 2) Usage logs: <appRoot>/configs/logs.csv
// ---------------------------------------------------------------------

void LogManager::setOnUsageAppended(UsageAppendCb cb)
{
    std::lock_guard<std::mutex> lock(usageMtx_);
    onUsageAppended_ = std::move(cb);
}

bool LogManager::appendUsage(const std::string& appRoot, const UsageEntry& e)
{
    if (!ensureScaffold(appRoot)) {
        return false;
    }

    UsageEntry entry = e; // lokal kopya: timestamp ve sendOk normalize edeceğiz
    if (entry.timeStamp.empty()) {
        entry.timeStamp = isoNowUtc();
    }
    if (entry.sendOk.empty()) {
        entry.sendOk = "NA";
    }

    fs::path filePath = fs::path(appRoot) / "logs" / "log_user" / "logs.csv";

    std::ofstream ofs(filePath, std::ios::app);
    if (!ofs.is_open()) {
        return false;
    }

    ofs
        << entry.processId << ','
        << csvEscape(entry.rfid) << ','
        << csvEscape(entry.firstName) << ','
        << csvEscape(entry.lastName) << ','
        << csvEscape(entry.plate) << ','
        << entry.limit << ','
        << entry.fuel << ','
        << csvEscape(entry.logCode) << ','
        << csvEscape(entry.timeStamp) << ','
        << csvEscape(entry.sendOk)
        << '\n';

    if (!ofs) {
        return false;
    }

    UsageAppendCb cbCopy;
    {
        std::lock_guard<std::mutex> lock(usageMtx_);
        usageRows_.push_back(entry);
        cbCopy = onUsageAppended_;
    }

    if (cbCopy) {
        cbCopy(entry);
    }

    return true;
}

bool LogManager::loadUsage(const std::string& appRoot,
                           std::vector<UsageEntry>& out) const
{
    fs::path filePath = fs::path(appRoot) / "logs" / "log_user" / "logs.csv";
    std::ifstream ifs(filePath);
    if (!ifs.is_open()) {
        return false;
    }

    std::string line;

    // İlk satır header, atla
    if (!std::getline(ifs, line)) {
        return false;
    }

    std::vector<UsageEntry> loaded;

    while (std::getline(ifs, line)) {
        if (line.empty()) {
            continue;
        }

        auto cols = parseCsvLine(line);
        if (cols.size() < 9) {
            // Beklenmeyen satır; şimdilik atla
            continue;
        }

        // 9 kolonlu eski format: sendOk yok → "NA"
        // 10 kolonlu yeni format: sendOk 9. indeks
        const bool hasSendOk = (cols.size() >= 10);

        UsageEntry e;
        try {
            e.processId = std::stoi(cols[0]);
        } catch (...) {
            e.processId = 0;
        }

        e.rfid       = (cols.size() > 1) ? cols[1] : std::string{};
        e.firstName  = (cols.size() > 2) ? cols[2] : std::string{};
        e.lastName   = (cols.size() > 3) ? cols[3] : std::string{};
        e.plate      = (cols.size() > 4) ? cols[4] : std::string{};

        try {
            e.limit = (cols.size() > 5) ? std::stoi(cols[5]) : 0;
        } catch (...) {
            e.limit = 0;
        }

        try {
            e.fuel = (cols.size() > 6) ? std::stod(cols[6]) : 0.0;
        } catch (...) {
            e.fuel = 0.0;
        }

        e.logCode   = (cols.size() > 7) ? cols[7] : std::string{};
        e.timeStamp = (cols.size() > 8) ? cols[8] : std::string{};
        e.sendOk    = hasSendOk ? cols[9] : std::string{"NA"};

        loaded.push_back(std::move(e));
    }

    {
        std::lock_guard<std::mutex> lock(usageMtx_);
        usageRows_ = loaded;
    }

    out = loaded;
    return true;
}

bool LogManager::updateUsageSendOk(const std::string& appRoot,
                                   int processId,
                                   const std::string& timeStamp,
                                   const std::string& sendOk)
{
    fs::path filePath = fs::path(appRoot) / "logs" / "log_user" / "logs.csv";

    std::ifstream ifs(filePath);
    if (!ifs.is_open()) {
        return false;
    }

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(ifs, line)) {
        lines.push_back(line);
    }

    if (lines.empty()) {
        return false;
    }

    const std::string normalizedSendOk = sendOk.empty() ? "NA" : sendOk;

    bool headerUpdated = false;
    bool anyRowUpdated = false;

    // Header
    {
        auto headerCols = parseCsvLine(lines[0]);
        if (headerCols.size() == 9) {
            headerCols.push_back("sendOk");

            std::ostringstream oss;
            for (std::size_t i = 0; i < headerCols.size(); ++i) {
                if (i > 0) {
                    oss << ',';
                }
                oss << csvEscape(headerCols[i]);
            }
            lines[0] = oss.str();
            headerUpdated = true;
        }
    }

    // Data satırları
    for (std::size_t i = 1; i < lines.size(); ++i) {
        if (lines[i].empty()) {
            continue;
        }

        auto cols = parseCsvLine(lines[i]);
        if (cols.size() < 9) {
            continue;
        }

        int rowProcId = 0;
        try {
            rowProcId = std::stoi(cols[0]);
        } catch (...) {
            rowProcId = 0;
        }

        const std::string rowTs = (cols.size() > 8) ? cols[8] : std::string{};

        if (rowProcId == processId && rowTs == timeStamp) {
            if (cols.size() == 9) {
                cols.push_back(normalizedSendOk);
            } else if (cols.size() >= 10) {
                cols[9] = normalizedSendOk;
            }

            std::ostringstream oss;
            for (std::size_t c = 0; c < cols.size(); ++c) {
                if (c > 0) {
                    oss << ',';
                }
                oss << csvEscape(cols[c]);
            }
            lines[i] = oss.str();
            anyRowUpdated = true;
        }
    }

    if (!anyRowUpdated && !headerUpdated) {
        return false;
    }

    // Tüm dosyayı tekrar yaz
    std::ofstream ofs(filePath, std::ios::trunc);
    if (!ofs.is_open()) {
        return false;
    }

    for (const auto& l : lines) {
        ofs << l << '\n';
    }

    if (!ofs) {
        return false;
    }

    // Bellek cache'ini de güncelle
    {
        std::lock_guard<std::mutex> lock(usageMtx_);
        for (auto& e : usageRows_) {
            if (e.processId == processId && e.timeStamp == timeStamp) {
                e.sendOk = normalizedSendOk;
            }
        }
    }

    return true;
}

} // namespace recum12::utils
