#pragma once

#include <functional>
#include <memory>
#include <string>

// Not:
// - Bu sınıfın tam davranışı CommandDispatcher_Integration_Guide.md
//   ve ReCUm12-Architecture-and-Sprint-Plan-2025-11-30.md içindeki
//   v12.220.01 sprint kapsamına göre kademeli olarak eklenecektir.
// - Eski projeden gelen bazı komut örnekleri sadece referans amaçlı
//   yorum blokları olarak tutulacaktır (bkz. [PASIF] stili).

namespace Net {

class CommandDispatcher
{
public:
    CommandDispatcher() = default;
    ~CommandDispatcher() = default;

    CommandDispatcher(const CommandDispatcher&)            = delete;
    CommandDispatcher& operator=(const CommandDispatcher&) = delete;
    CommandDispatcher(CommandDispatcher&&)                 = delete;
    CommandDispatcher& operator=(CommandDispatcher&&)      = delete;

    // TODO(v12.220.01-R1-R4):
    // - V2 JSON zarfına göre dispatch(...) API'si
    // - Filling guard (setFilling / setBusy vb.)
    // - Log / user / rs485 handler kayıt fonksiyonları
    //
    // Örnek PASIF yorum stili (eski projeden referans alırken):
    //
    // [PASIF] Eski projede kullanılan "getStatus" komutu.
    // // dispatcher.registerHandler("getStatus", [this](...) {
    // //     return handlers_.getStatus();
    // // });
};

} // namespace Net
