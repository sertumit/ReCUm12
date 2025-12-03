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

    // Trimlenmiş versiyon
    auto firstNonWs = [&]() -> std::string::size_type {
        for (std::string::size_type i = 0; i < line.size(); ++i) {
            unsigned char c = static_cast<unsigned char>(line[i]);
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return i;
        }
        return std::string::npos;
    }();

    if (firstNonWs == std::string::npos) {
        outJson.clear();
        return;
    }

    std::string::size_type lastNonWs = line.size();
    while (lastNonWs > 0) {
        unsigned char c = static_cast<unsigned char>(line[lastNonWs - 1]);
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
        --lastNonWs;
    }

    std::string trimmed = line.substr(firstNonWs, lastNonWs - firstNonWs);

    // Basit PING/PONG testi (PLAIN)
    if (trimmed == "PING") {
        outJson = "PONG";
        return;
    }

    bool looksJson = !trimmed.empty() && trimmed.front() == '{';

    if (!looksJson) {
        // JSON olmayan satırlar için temel hata cevabı.
        outJson = R"({"type":"response","action":"plain","status":"error",)"
                  R"("error":{"code":"E_BAD_FORMAT","message":"expected JSON request"}})";
        return;
    }

    // Basit action alanı çıkartıcı (tam JSON parser değil).
    auto extractStringField = [](const std::string& json,
                                 const std::string& key) -> std::string {
        const std::string pattern = "\"" + key + "\"";
        std::string::size_type pos = json.find(pattern);
        if (pos == std::string::npos) return std::string{};

        pos = json.find(':', pos + pattern.size());
        if (pos == std::string::npos) return std::string{};

        std::string::size_type q1 = json.find('"', pos + 1);
        if (q1 == std::string::npos) return std::string{};
        std::string::size_type q2 = json.find('"', q1 + 1);
        if (q2 == std::string::npos) return std::string{};

        return json.substr(q1 + 1, q2 - q1 - 1);
    };

    auto makeErrorResponse = [](const std::string& action,
                                const std::string& code,
                                const std::string& message) -> std::string {
        std::string a = action.empty() ? "unknown" : action;
        // Not: message içi tırnak/kaçış kontrolü yapılmıyor; sabit metinler kullanıyoruz.
        return std::string("{\"type\":\"response\",\"action\":\"") + a +
               "\",\"status\":\"error\",\"error\":{\"code\":\"" + code +
               "\",\"message\":\"" + message + "\"}}";
    };

    auto makeOkResponseWithPayload = [](const std::string& action,
                                        const std::string& payloadJson) -> std::string {
        std::string a = action.empty() ? "unknown" : action;
        // payloadJson'ın zaten geçerli JSON olduğu varsayılır.
        std::string out = "{\"type\":\"response\",\"action\":\"" + a +
                          "\",\"status\":\"ok\",\"payload\":";
        out += payloadJson;
        out += "}";
        return out;
    };

    // JSON string içinde kullanılacak metinler için basit kaçış fonksiyonu.
    // Özellikle logs.csv içeriğini "content" alanına gömebilmek için kullanılır.
    auto escapeJsonString = [](const std::string& in) -> std::string {
        std::string out;
        out.reserve(in.size() + 16);
        for (unsigned char c : in) {
            switch (c) {
            case '\\': out += "\\\\"; break;
            case '\"': out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                out += static_cast<char>(c);
                break;
            }
        }
        return out;
    };

    const std::string action = extractStringField(trimmed, "action");

    if (action.empty()) {
        outJson = makeErrorResponse("unknown",
                                    "E_BAD_FORMAT",
                                    "missing action");
        return;
    }

    // ---- getLogsRaw: YAT / debug için düz CSV çıktısı ----
    //
    // - V2 JSON zarfı YOK; doğrudan logs.csv içeriği gönderilir.
    // - LineTcpClient::sendLine(outJson), içindeki '\n' karakterlerini
    //   olduğu gibi kabloya yazar; YAT her satırı ayrı satır olarak görür.
    // - Aynı GetLogsFn handler'ını kullanır (son 100 satır vs. mantığı
    //   AppRuntime içindeki handler'da duruyor).
    if (action == "getLogsRaw") {
        if (!getLogsHandler_) {
            outJson = makeErrorResponse(action,
                                        "E_NO_HANDLER",
                                        "getLogs handler not set");
            return;
        }

        const std::string from;
        const std::string to;
        int               limit     = -1;
        const std::string rfid;
        bool              rfidEmpty = true;
        const std::string plate;
        const std::vector<std::string> logCodes;

        // Handler'dan gelen CSV içeriğini olduğu gibi döndür.
        outJson = getLogsHandler_(from, to, limit, rfid, rfidEmpty, plate, logCodes);
        return;
    }    
    
    // Log sorguları: getLogs / logsQuery
    //
    // Not:
    // - Şimdilik JSON içinden ayrıntılı filtre alanları parse edilmiyor.
    // - Handler'a "varsayılan" filtrelerle gidiliyor:
    //     from="", to="", limit=-1, rfid="", rfidEmpty=true,
    //     plate="", logCodes=[].
    // - İleride 'from' / 'to' alanları için beklenen tarih formatı:
    //     "gg.aa.yyyy - hh:mm"
    //   (örnek: "02.12.2025 - 07:30") şeklinde JSON payload'dan
    //   okunacak ve GetLogsFn'e bu formatta iletilecek.
    // - GetLogsFn'den dönen string, başarılı dolum satırlarının
    //   CSV içeriği olarak varsayılır ve payload içinde "content"
    //   alanına gömülür.
    // - Bu, kablo/entegrasyon testleri için yeterli; ileride
    //   CommandDispatcher_Integration_Guide'e göre ayrıntılı
    //   parametre parse eklenecek.
    if (action == "getLogs" || action == "logsQuery") {
        if (!getLogsHandler_) {
            outJson = makeErrorResponse(action,
                                        "E_NO_HANDLER",
                                        "getLogs handler not set");
            return;
        }

        // filling_ guard:
        // - Log okuma işlemi olduğu için dolum sırasında da
        //   şimdilik izin veriyoruz. Gerekirse E_BUSY'e
        //   çevrilebilir.

        const std::string from;
        const std::string to;
        int limit = -1;
        const std::string rfid;
        bool rfidEmpty = true;
        const std::string plate;
        const std::vector<std::string> logCodes;

        // Handler'dan gelen içerik: başarılı dolum satırları için CSV.
        const std::string csvContent =
            getLogsHandler_(from, to, limit, rfid, rfidEmpty, plate, logCodes);

        // logs.csv zarfı: format + fileName + content
        std::string payloadJson = "{\"format\":\"csv\",\"fileName\":\"logs.csv\",\"content\":\"";
        payloadJson += escapeJsonString(csvContent);
        payloadJson += "\"}";

        outJson = makeOkResponseWithPayload(action, payloadJson);
        return;
    }

    // Kullanıcı listesi: users.csv içeriği
    if (action == "getUsers") {
        if (!getUsersHandler_) {
            outJson = makeErrorResponse(action,
                                        "E_NO_HANDLER",
                                        "getUsers handler not set");
            return;
        }

        // filling_ guard: kullanıcı okuma komutu olduğu için şimdilik
        // dolum sırasında da izin veriyoruz. İleride politika değişirse
        // burada E_BUSY dönebiliriz.

        // Handler'dan gelen içerik: users.csv içeriği (tam dosya veya
        // belirli bir alt küme). logs.csv'deki gibi CSV formatında
        // varsayılır ve payload içinde "content" alanına gömülür.
        const std::string csvContent = getUsersHandler_();

        std::string payloadJson = "{\"format\":\"csv\",\"fileName\":\"users.csv\",\"content\":\"";
        payloadJson += escapeJsonString(csvContent);
        payloadJson += "\"}";

        outJson = makeOkResponseWithPayload(action, payloadJson);
        return;
    }

    // Diğer action'lar şimdilik iskelet aşamasında.
    outJson = makeErrorResponse(action,
                                "E_NOT_IMPLEMENTED",
                                "dispatcher skeleton");
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
