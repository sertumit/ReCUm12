#ifndef RECUM12_CORE_USERMANAGER_H
#define RECUM12_CORE_USERMANAGER_H

#pragma once
#include <string>
#include <vector>
#include <optional>

namespace recum12::core {

// ReCUm10 UserManager şemasından türetilmiş sade kullanıcı modeli:
// varsayılan users.csv header:
//   userId,level,firstName,lastName,plate,limit_liters,rfid
struct UserRecord
{
    int         userId   = 0;
    int         level    = 4;
    std::string firstName;
    std::string lastName;
    std::string plate;
    // Eski ReCUm10 uyumluluğu için int limit alanı:
    int         limit    = 0;
    // Bu projede asıl kullanılan alan (litre cinsinden limit):
    double      limit_liters {0.0};
    std::string rfid;        // UPPER normalize edilmiş kart UID'si
};

class UserManager
{
public:
    UserManager();

    // users.csv dosyasını yükler.
    // Başlık küçük-büyük harf duyarsız olarak çözülür, asgari:
    //   userId, level, firstName, lastName, plate, limit, rfid
    bool loadUsers(const std::string& path);

    const std::vector<UserRecord>& allUsers() const noexcept { return users_; }

    // RFID kart UID'si (hex) ile kullanıcı bulur.
    // Giriş case-insensitive; içerde UPPER normalize edilir.
    std::optional<UserRecord> findByRfid(const std::string& uidHex) const;

private:
    std::vector<UserRecord> users_;
    std::string             path_;

    static std::string normalize(const std::string& s);
};

} // namespace recum12::core

#endif // RECUM12_CORE_USERMANAGER_H