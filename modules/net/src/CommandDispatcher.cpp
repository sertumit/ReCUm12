#include "net/CommandDispatcher.h"

// Not:
// - Bu dosya şu anda sadece sınıf iskeletini içerir.
// - Dispatch mantığı, handler kayıtları ve filling guard
//   v12.220.01 sprint adımlarında kademeli olarak eklenecektir.

namespace Net {

void CommandDispatcher::dispatch(const std::string& line,
                                 std::string& outJson,
                                 const GuiCb& guiCb,
                                 const std::string& remote_ip,
                                 int remote_port)
{
    (void)guiCb;
    (void)remote_ip;
    (void)remote_port;

    // Şimdilik sadece iskelet:
    // - V2 JSON parse ve gerçek komut işleme mantığı
    //   sonraki sprint adımlarında eklenecektir.
    //
    // Geçici olarak, boş veya sadece whitespace gelen satırlar için
    // outJson boş bırakılır; diğer tüm durumlarda basit bir hata
    // mesajı döndürülür. Böylece üst katman, dispatcher'ın ulaşılabildiğini
    // görebilir.

    auto isWhitespaceOnly = [](const std::string& s) {
        for (unsigned char c : s) {
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return false;
        }
        return true;
    };

    if (line.empty() || isWhitespaceOnly(line)) {
        outJson.clear();
        return;
    }

    // TODO(v12.220.01-R1-R4): Burada V2 zarfına göre ayrıntılı
    // JSON parse ve handler çağrı mantığı eklenecek.
    outJson = R"({"type":"response","action":"unknown","status":"error",)"
              R"("error":{"code":"E_NOT_IMPLEMENTED","message":"dispatcher skeleton"}})";
}

void CommandDispatcher::setFilling(bool filling)
{
    filling_ = filling;
}

void CommandDispatcher::setEventSink(EventSink sink)
{
    eventSink_ = std::move(sink);
}

void CommandDispatcher::setGetLogsHandler(GetLogsFn fn)
{
    getLogsHandler_ = std::move(fn);
}

void CommandDispatcher::setRs485ReadRawHandler(Rs485ReadRawFn fn)
{
    rs485ReadRawHandler_ = std::move(fn);
}

void CommandDispatcher::setGetUsersHandler(GetUsersFn fn)         { getUsersHandler_      = std::move(fn); }
void CommandDispatcher::setAddUserHandler(AddUserFn fn)           { addUserHandler_       = std::move(fn); }
void CommandDispatcher::setUpdateUserHandler(UpdateUserFn fn)     { updateUserHandler_    = std::move(fn); }
void CommandDispatcher::setDeleteUserHandler(DeleteUserFn fn)     { deleteUserHandler_    = std::move(fn); }
void CommandDispatcher::setSetUserRfidHandler(SetUserRfidFn fn)   { setUserRfidHandler_   = std::move(fn); }

} // namespace Net
