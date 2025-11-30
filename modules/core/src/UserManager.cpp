// Basit CSV okuma için:
#include "core/UserManager.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace recum12::core {

namespace {

std::string trimCopy(std::string s)
{
    const char* ws = " \t\r\n";
    auto b = s.find_first_not_of(ws);
    auto e = s.find_last_not_of(ws);
    if (b == std::string::npos) {
        return {};
    }
    return s.substr(b, e - b + 1);
}

std::string lowerCopy(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string upperCopy(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return static_cast<char>(std::toupper(c)); });
    return s;
}

// Çok basit CSV splitter (virgüle göre böler, tırnaklı karmaşık durumları
// şimdilik desteklemiyoruz; users.csv buna göre düzenlenecek).
std::vector<std::string> splitCsvLine(const std::string& line)
{
    std::vector<std::string> out;
    std::string current;
    std::istringstream iss(line);
    while (std::getline(iss, current, ',')) {
        out.push_back(current);
    }
    return out;
}

} // namespace

UserManager::UserManager() = default;

std::string UserManager::normalize(const std::string& s)
{
    // UID normalizasyonu:
    //  - baştaki/sondaki whitespace'leri at
    //  - içerdeki boşluk, ':' ve '-' karakterlerini tamamen kaldır
    //  - kalanları UPPER case'e çevir
    //
    // Böylece aşağıdakilerin hepsi aynı değere normalize olur:
    //   "32A0AB04"
    //   "32 A0 AB 04"
    //   "32:A0:AB:04"
    //   "32-a0-ab-04"
    std::string cleaned;
    cleaned.reserve(s.size());
    for (char c : trimCopy(s)) {
        if (c == ' ' || c == ':' || c == '-') continue;
        cleaned.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }
    return cleaned;
}

bool UserManager::loadUsers(const std::string& path)
{
    path_ = path;
    users_.clear();

    std::ifstream in(path);
    if (!in.good()) {
        return false;
    }

    std::string headerLine;
    if (!std::getline(in, headerLine)) {
        return false;
    }

    auto hdr = splitCsvLine(headerLine);
    if (hdr.empty()) {
        return false;
    }

    int idxUserId  = -1;
    int idxLevel   = -1;
    int idxFirst   = -1;
    int idxLast    = -1;
    int idxPlate   = -1;
    int idxLimit   = -1;
    int idxRfid    = -1;

    for (size_t i = 0; i < hdr.size(); ++i) {
        const auto h = lowerCopy(trimCopy(hdr[i]));
        if (h == "userid"   || h == "user_id" || h == "idn")  idxUserId = static_cast<int>(i);
        else if (h == "level"   || h == "role")               idxLevel  = static_cast<int>(i);
        else if (h == "firstname" || h == "first_name")       idxFirst  = static_cast<int>(i);
        else if (h == "lastname"  || h == "last_name")        idxLast   = static_cast<int>(i);
        else if (h == "plate"     || h == "plate_no")         idxPlate  = static_cast<int>(i);
        else if (h == "limit"     || h == "quota" || h == "limit_liters")
                                                             idxLimit  = static_cast<int>(i);
        else if (h == "rfid"      || h == "uid")              idxRfid   = static_cast<int>(i);
    }

    // userId ve rfid sütunları olmadan bu projede iş yapamayız → yükleme başarısız.
    if (idxUserId < 0 || idxRfid < 0) {
        return false;
    }

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        auto cols = splitCsvLine(line);
        if (static_cast<int>(cols.size()) <= idxUserId) continue;

        UserRecord u{};
        try { u.userId = std::stoi(trimCopy(cols[idxUserId])); }
        catch (...) { u.userId = 0; }
        if (u.userId <= 0) continue;

        if (idxLevel >= 0 && idxLevel < static_cast<int>(cols.size())) {
            try { u.level = std::stoi(trimCopy(cols[idxLevel])); }
            catch (...) { u.level = 4; }
        }
        if (idxFirst >= 0 && idxFirst < static_cast<int>(cols.size()))  u.firstName = trimCopy(cols[idxFirst]);
        if (idxLast  >= 0 && idxLast  < static_cast<int>(cols.size()))  u.lastName  = trimCopy(cols[idxLast]);
        if (idxPlate >= 0 && idxPlate < static_cast<int>(cols.size()))  u.plate     = trimCopy(cols[idxPlate]);
        if (idxLimit >= 0 && idxLimit < static_cast<int>(cols.size())) {
            try {
                const auto limitStr = trimCopy(cols[idxLimit]);
                // Litre cinsinden double olarak oku
                u.limit_liters = std::stod(limitStr);
                // Eski kodlar için int limit'i de doldur
                u.limit        = static_cast<int>(u.limit_liters);
            } catch (...) {
                u.limit_liters = 0.0;
                u.limit        = 0;
            }
        }
        if (idxRfid >= 0 && idxRfid < static_cast<int>(cols.size())) {
            u.rfid = normalize(cols[idxRfid]); // UPPER normalize
        }

        users_.push_back(std::move(u));
    }

    return true;
}

std::optional<UserRecord> UserManager::findByRfid(const std::string& uidHex) const
{
    const std::string wanted = normalize(uidHex);
    if (wanted.empty()) {
        return std::nullopt;
    }
    for (const auto& u : users_) {
        if (!u.rfid.empty() && u.rfid == wanted) {
            return u;
        }
    }
    return std::nullopt;
}

} // namespace recum12::core