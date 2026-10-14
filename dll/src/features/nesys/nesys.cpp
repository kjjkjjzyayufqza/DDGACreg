#include "features/module_api.h"
#include "features/content/content.h"
#include "features/nesys/operator_settings.h"
#include "features/nesys/state_watch.h"

#include "log.h"

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

// Local NESYS service for TG4AC 5.80.02 (image base 0x140000000).
//
// The cabinet runs a NESYS service process. The game's NESYS client library
// (sub_140001000 NESYS_INIT, sub_140005260 connect) opens the message pipe
// \\.\pipe\nesys_games. Without that pipe Code stays null, so
// NesysManager_IsConnected (sub_140228680) is false, the NESiCA mode button
// stays grey, NESYS_CARD_SELECT (sub_140002930) returns -1, and every content
// command fails before it is sent: the game does not speak HTTP itself, it
// hands NESYS_HTTPACCESS_POST (sub_140001FB0) to the service.
//
// This module is that service, inside the game process. Each pipe message is
// [u32 command][u32 payload length][payload]. Replies use the codes that
// sub_140005F40 maps to NesysMessageCallbackFunction events:
//   CONNECT        -> CONNECT_REPLY, NWRECOVER_NOTICE (event 6 sets
//                     NesysManager+5), CERT_INIT_NOTICE (event 1 clears +6,
//                     sets Code+8300 and resolves the "game" server profile)
//   HTTPACCESS_*   -> the request is forwarded to the local content server and
//                     the body comes back in HTTPACCESS_REPLY. A ping that
//                     succeeds sets NesysManager+9 (NesysManager_IsAbleSend).
//   CARD_*         -> forwarded to the content server card routes; the JSON
//                     comes back as the card data of the matching *_REPLY. A
//                     card select or registration that succeeds also marks
//                     the play as a NESiCA play (state_watch.cpp).
//   HTTPSACCESS_*  -> handled like HTTPACCESS_* with their own layout. Only the
//                     game API is served; other hosts get an error reply, as
//                     an offline cabinet would.
//
// Message codes and payload layouts were read from this exe: sub_1400070E0
// builds the requests and sub_140005F40 parses the replies.
//
// The service only answers once the game starts NESYS, which the cabinet
// config disables (operator_settings.cpp lifts that). [Nesys] Trace=1 logs
// every pipe message both ways.

namespace {

constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\nesys_games";
constexpr DWORD kPipeBufferBytes = 0x10000;
constexpr std::size_t kMessageHeaderBytes = 8;

// Game -> service (sub_1400070E0).
enum Command : std::uint32_t {
    kClientStart = 0x01,
    kConnect = 0x02,
    kDisconnect = 0x03,
    kGameStart = 0x04,
    kGameEnd = 0x05,
    kGameContinue = 0x06,
    kEventDownload = 0x07,
    kEventRequest = 0x08,
    kCardSelect = 0x09,
    kCardInsert = 0x0A,
    kCardUpdate = 0x0B,
    kCardBuysItem = 0x0C,
    kCardTakeover = 0x0D,
    kCardForceTakeover = 0x0E,
    kCardDecrease = 0x0F,
    kCardReissueTest = 0x10,
    kCardReissue = 0x11,
    kCardPlayedList = 0x12,
    kRankingData = 0x13,
    kLocalNwInfo = 0x14,
    kGlobalAddr = 0x15,
    kEcho = 0x16,
    kAdapterInfo = 0x17,
    kServiceVersion = 0x18,
    kDhcpRenew = 0x19,
    kHttpGet = 0x1A,
    kHttpPost = 0x1B,
    kUploadConfig = 0x1C,
    kIncomeStart = 0x1D,
    kIncomeEnd = 0x1E,
    kIncomeContinue = 0x1F,
    kSetIncomeMode = 0x20,
    kDestroyService = 0x21,
    kIncomePoint = 0x22,
    kGameStatusReset = 0x23,
    kRowEventDataList = 0x24,
    kShopping = 0x25,
    kHttpsGet = 0x26,
    kHttpsPost = 0x27,
    kFreeTicket = 0x28,
    kGameFreeStart = 0x29,
    kGameFreeEnd = 0x2A,
    kIncomeFreeStart = 0x2B,
    kIncomeFreeEnd = 0x2C,
    kClientEnd = 0xFF,
};

// Service -> game (sub_140005F40).
enum Reply : std::uint32_t {
    kNwRecoverNotice = 0x103,
    kCertInitNotice = 0x107,
    kEffectiveEventNotice = 0x109,
    kDhcpRenewStart = 0x10B,
    kDhcpCompleteNotice = 0x10C,
    kClientStartReply = 0x10D,
    kConnectReply = 0x10E,
    kDisconnectReply = 0x10F,
    kGameStatusReply = 0x110,
    kCardSelectReply = 0x111,
    kCardInsertReply = 0x112,
    kCardUpdateReply = 0x113,
    kCardBuysItemReply = 0x114,
    kCardTakeoverReply = 0x115,
    kCardDecreaseReply = 0x116,
    kCardReissueTestReply = 0x117,
    kCardReissueReply = 0x118,
    kCardPlayedListReply = 0x119,
    kRankingDataReply = 0x11A,
    kLocalNwInfoReply = 0x11B,
    kLocalNwInfoNotice = 0x11C,
    kGlobalAddrReply = 0x11D,
    kEchoReply = 0x11E,
    kAdapterInfoReply = 0x11F,
    kServiceVersionReply = 0x120,
    kHttpAccessStart = 0x121,
    kHttpAccessReply = 0x122,
    kUploadConfigReply = 0x123,
    kIncomeStatusReply = 0x124,
    kSetIncomeModeReply = 0x125,
    kDestroyServiceReply = 0x126,
    kGameStatusResetReply = 0x127,
    kRowEventDataListReply = 0x128,
    kShoppingReply = 0x129,
    kFreeTicketReply = 0x12A,
    kClientEndReply = 0x1FF,
};

// HTTP(S) request payloads. Every one starts with URL[1025]; GETs end after
// the fixed part, POSTs carry data size, header[1025] and the data.
constexpr std::size_t kHttpUrlBytes = 1025;
constexpr std::size_t kHttpHeaderBytes = 1025;

struct HttpLayout
{
    const wchar_t* method;
    bool post;
    std::size_t sequence;
    std::size_t fixedBytes;
    std::size_t dataSize;
    std::size_t header;
};

// HTTPACCESS_GET / _POST (sub_140001FB0): sequence at 2120, POST data at 3161.
constexpr HttpLayout kHttpGetLayout{L"GET", false, 2120, 2132, 0, 0};
constexpr HttpLayout kHttpPostLayout{L"POST", true, 2120, 3161, 2132, 2136};
// HTTPSACCESS_GET (sub_140001D30) / _POST (sub_140002270): client certificate
// path at 2116, sequence at 3148, POST data at 4189.
constexpr HttpLayout kHttpsGetLayout{L"GET", false, 3148, 3160, 0, 0};
constexpr HttpLayout kHttpsPostLayout{L"POST", true, 3148, 4189, 3160, 3164};

// HTTPACCESS_REPLY payload (case 290 in sub_140005F40).
constexpr std::size_t kReplyHeaderBytes = 1028;

// CARD_SELECT payload (sub_140002930): id, data type, param size, param.
// CARD_INSERT / _UPDATE / _BUYS_ITEM payload (sub_140002A30): id, frequency,
// transfer mode, data type, data size, data.
constexpr std::size_t kCardIdBytes = 16;
constexpr std::size_t kCardSelectParamSizeOffset = 20;
constexpr std::size_t kCardSelectParamOffset = 24;
constexpr std::size_t kCardSendDataTypeOffset = 24;
constexpr std::size_t kCardSendDataSizeOffset = 28;
constexpr std::size_t kCardSendDataOffset = 32;
constexpr std::uint32_t kCardStatusOk = 1;

// RANKING_DATA payload (sub_1400070E0 case 19): ranking type, mode, data size,
// data. RANKING_DATA_REPLY (case 282): status, data size, data.
constexpr std::size_t kRankingDataSizeOffset = 8;
constexpr std::size_t kRankingDataOffset = 12;

// Every game API accessor names its command (vtable +576) and its
// CardUpdateType (vtable +584). Accessors with a type are sent as CARD_UPDATE
// (sub_14023F0F0) or, for rankings, RANKING_DATA with that type; the cabinet
// service maps the type back to the command. Type 40 is the machine unlock
// check, whose command name is empty in the game.
struct CardRoute
{
    std::uint32_t type;
    const char* command;
};

constexpr CardRoute kCardRoutes[] = {
    {20, "init"},
    {21, "mission"},
    {22, "login"},
    {23, "playend"},
    {24, "addcredit"},
    {25, "config"},
    {26, "purchasepoint"},
    {27, "gamestart"},
    {28, "gameresult"},
    {29, "gametotalresult"},
    {30, "getmissionranking"},
    {31, "getstoremileageranking"},
    {40, "machineunlock"},
    {50, "shopinitialize"},
    {51, "purchaseshopitem"},
    {52, "playerbadge"},
    {53, "playerwappen"},
    {55, "iteminitialize"},
    {60, "discontinuation"},
};

const char* CardRouteCommand(std::uint32_t type)
{
    for (const CardRoute& route : kCardRoutes) {
        if (route.type == type)
            return route.command;
    }
    return nullptr;
}

// CERT_INIT_NOTICE payload (case 263): tenpo table, news table, server text.
constexpr std::size_t kTenpoBytes = 120;
constexpr std::size_t kTenpoNameOffset = 4;
constexpr std::size_t kTenpoNameBytes = 31;
constexpr std::size_t kTenpoAddrOffset = 35;
constexpr std::size_t kTenpoAddrBytes = 33;
constexpr std::size_t kTenpoTicketOffset = 68;
constexpr std::size_t kTenpoTicketBytes = 33;
constexpr std::size_t kTenpoPrefOffset = 101;
constexpr std::size_t kTenpoPrefBytes = 17;
constexpr std::size_t kNewsBytes = 1032;
// sub_140228CF0 matches this against its server profile table.
constexpr char kServerProfile[] = "game";

// ADAPTER_INFO_REPLY (232 bytes), ECHO_REPLY (84 bytes),
// ROW_EVENTDATA_LIST_REPLY entries (0x840 bytes each, an all-zero entry is "none").
constexpr std::size_t kAdapterTextBytes = 32;
constexpr std::size_t kAdapterMacBytes = 64;
constexpr std::size_t kEchoBytes = 84;
constexpr std::size_t kRowEventEntryBytes = 0x840;
constexpr std::size_t kVersionBytes = 33;
constexpr std::size_t kGlobalAddrTextBytes = 32;

constexpr DWORD kResolveTimeoutMs = 2000;
constexpr DWORD kConnectTimeoutMs = 2000;
constexpr DWORD kTransferTimeoutMs = 15000;
constexpr char kApiRoot[] = "/ahac-api/tg4gs/gs";

int g_contentPort = 8750;
HINTERNET g_http = nullptr;
std::atomic<bool> g_loggedPingOk{false};
std::atomic<bool> g_loggedPingFailed{false};
std::atomic<bool> g_loggedCommand[256] = {};
bool g_trace = false;

constexpr std::size_t kTraceHexBytes = 64;
constexpr std::size_t kTraceTextBytes = 1024;

class Payload
{
public:
    void U32(std::uint32_t value)
    {
        Raw(&value, sizeof(value));
    }

    void Raw(const void* data, std::size_t size)
    {
        const auto* bytes = static_cast<const unsigned char*>(data);
        bytes_.insert(bytes_.end(), bytes, bytes + size);
    }

    void Zeros(std::size_t size)
    {
        bytes_.insert(bytes_.end(), size, 0);
    }

    // Fixed-size, NUL-padded text field.
    void Text(const char* text, std::size_t fieldBytes)
    {
        const std::size_t length = strnlen(text, fieldBytes - 1);
        Raw(text, length);
        Zeros(fieldBytes - length);
    }

    void PutText(std::size_t offset, const char* text, std::size_t fieldBytes)
    {
        const std::size_t length = strnlen(text, fieldBytes - 1);
        std::memcpy(bytes_.data() + offset, text, length);
    }

    const std::vector<unsigned char>& Bytes() const { return bytes_; }

private:
    std::vector<unsigned char> bytes_;
};

std::uint32_t ReadU32(const unsigned char* data, std::size_t offset)
{
    std::uint32_t value = 0;
    std::memcpy(&value, data + offset, sizeof(value));
    return value;
}

std::string FixedText(const unsigned char* data, std::size_t bytes)
{
    const auto* text = reinterpret_cast<const char*>(data);
    return std::string(text, strnlen(text, bytes));
}

std::wstring Widen(const std::string& text)
{
    if (text.empty())
        return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), count);
    return wide;
}

std::string JsonEscape(const std::string& text)
{
    std::string out;
    for (const char ch : text) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                char escaped[8];
                sprintf_s(escaped, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(ch)));
                out += escaped;
            } else {
                out += ch;
            }
        }
    }
    return out;
}

// First bytes of a payload as hex, for the trace.
std::string HexPreview(const unsigned char* data, std::size_t size)
{
    static const char kDigits[] = "0123456789ABCDEF";
    const std::size_t shown = size < kTraceHexBytes ? size : kTraceHexBytes;
    std::string out;
    for (std::size_t i = 0; i < shown; ++i) {
        if (i != 0)
            out += ' ';
        out += kDigits[data[i] >> 4];
        out += kDigits[data[i] & 0xF];
    }
    if (shown < size)
        out += " ...";
    return out;
}

// Body text for the trace: control characters escaped, long bodies cut.
std::string TextPreview(const std::string& text)
{
    std::string out = JsonEscape(text.substr(0, kTraceTextBytes));
    if (text.size() > kTraceTextBytes)
        out += "...";
    return out;
}

// --- HTTP to the local content server ------------------------------------

struct HttpResult
{
    DWORD status = 0;   // HTTP status, 0 when the request did not complete
    DWORD error = 0;    // WinHTTP error, 0 on success
    std::string body;
};

bool IsLoopbackHost(const std::wstring& host)
{
    return host == L"127.0.0.1" || _wcsicmp(host.c_str(), L"localhost") == 0;
}

// Header lines from the game are separated by '\n'. WinHTTP sets
// Content-Length from the body itself, so that line is dropped.
std::wstring RequestHeaders(const std::string& header)
{
    std::wstring out;
    std::size_t start = 0;
    while (start < header.size()) {
        std::size_t end = header.find('\n', start);
        if (end == std::string::npos)
            end = header.size();
        std::string line = header.substr(start, end - start);
        start = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line.empty() || _strnicmp(line.c_str(), "content-length:", 15) == 0)
            continue;
        out += Widen(line);
        out += L"\r\n";
    }
    return out;
}

bool ReadBody(HINTERNET request, std::string& body)
{
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available))
            return false;
        if (available == 0)
            return true;
        const std::size_t offset = body.size();
        body.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request, body.data() + offset, available, &read))
            return false;
        body.resize(offset + read);
    }
}

HttpResult SendHttp(const wchar_t* method, const std::string& url, const std::string& header,
                    const unsigned char* data, std::size_t dataSize)
{
    HttpResult result;
    const std::wstring wideUrl = Widen(url);
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts)) {
        result.error = GetLastError();
        LOG("ERROR: NESYS could not parse URL %s", url.c_str());
        return result;
    }
    if (!IsLoopbackHost(host)) {
        result.error = ERROR_ACCESS_DENIED;
        LOG("ERROR: NESYS refused non-local URL %s", url.c_str());
        return result;
    }

    HINTERNET connection = WinHttpConnect(g_http, host, parts.nPort, 0);
    HINTERNET request = connection != nullptr
        ? WinHttpOpenRequest(connection, method, path, nullptr, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, 0)
        : nullptr;
    const std::wstring headers = RequestHeaders(header);
    const bool sent = request != nullptr
        && WinHttpSendRequest(request,
                              headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                              headers.empty() ? 0 : static_cast<DWORD>(-1),
                              dataSize > 0 ? const_cast<unsigned char*>(data) : WINHTTP_NO_REQUEST_DATA,
                              static_cast<DWORD>(dataSize), static_cast<DWORD>(dataSize), 0)
        && WinHttpReceiveResponse(request, nullptr);
    DWORD statusSize = sizeof(result.status);
    if (!sent
        || !WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &result.status, &statusSize, WINHTTP_NO_HEADER_INDEX)
        || !ReadBody(request, result.body)) {
        result.error = GetLastError();
        result.status = 0;
        LOG("ERROR: NESYS %ls %s failed (WinHTTP %lu). Is ddgac-content running?",
            method, url.c_str(), result.error);
    }
    if (request != nullptr)
        WinHttpCloseHandle(request);
    if (connection != nullptr)
        WinHttpCloseHandle(connection);
    return result;
}

std::string ContentUrl(const char* command)
{
    return "http://127.0.0.1:" + std::to_string(g_contentPort) + kApiRoot + "/" + command;
}

// The game addresses the NESiCA hosts (game.ahac.nesica.net and its photo
// host; the profile setup in sub_140228CF0 picks them). Their game API lives
// under kApiRoot and is served by the local content server, whatever host the
// game put in front of it. Nothing else is forwarded.
bool LocalContentUrl(const std::string& url, std::string& local)
{
    const std::size_t scheme = url.find("://");
    if (scheme == std::string::npos)
        return false;
    const std::size_t path = url.find('/', scheme + 3);
    const std::size_t rootLength = std::strlen(kApiRoot);
    if (path == std::string::npos || url.compare(path, rootLength, kApiRoot) != 0)
        return false;
    const char next = path + rootLength < url.size() ? url[path + rootLength] : '\0';
    if (next != '\0' && next != '/' && next != '?')
        return false;
    local = "http://127.0.0.1:" + std::to_string(g_contentPort) + url.substr(path);
    return true;
}

// The content server answers a result line ("1" success, "0" no data for the
// command) followed by JSON, the same body the game parses after an
// HTTPACCESS reply. Card and ranking data carry the JSON only.
bool SplitResultLine(const std::string& body, std::string& code, std::string& json)
{
    const std::size_t newline = body.find('\n');
    if (newline == std::string::npos)
        return false;
    code = body.substr(0, newline);
    while (!code.empty() && (code.back() == '\r' || code.back() == ' '))
        code.pop_back();
    json = body.substr(newline + 1);
    return true;
}

// --- pipe session ------------------------------------------------------------

const char* CommandName(std::uint32_t command)
{
    switch (command) {
    case kClientStart: return "CLIENT_START";
    case kConnect: return "CONNECT";
    case kDisconnect: return "DISCONNECT";
    case kGameStart: return "GAME_START";
    case kGameEnd: return "GAME_END";
    case kGameContinue: return "GAME_CONTINUE";
    case kEventDownload: return "EVENT_DOWNLOAD";
    case kEventRequest: return "EVENT_REQUEST";
    case kCardSelect: return "CARD_SELECT";
    case kCardInsert: return "CARD_INSERT";
    case kCardUpdate: return "CARD_UPDATE";
    case kCardBuysItem: return "CARD_BUYS_ITEM";
    case kCardTakeover: return "CARD_TAKEOVER";
    case kCardForceTakeover: return "CARD_FORCE_TAKEOVER";
    case kCardDecrease: return "CARD_DECREASE";
    case kCardReissueTest: return "CARD_REISSUE_TEST";
    case kCardReissue: return "CARD_REISSUE";
    case kCardPlayedList: return "CARD_PLAYED_LIST";
    case kRankingData: return "RANKING_DATA";
    case kLocalNwInfo: return "LOCALNW_INFO";
    case kGlobalAddr: return "GLOBALADDR";
    case kEcho: return "ECHO";
    case kAdapterInfo: return "ADAPTER_INFO";
    case kServiceVersion: return "SERVICE_VERSION";
    case kDhcpRenew: return "DHCP_RENEW";
    case kHttpGet: return "HTTPACCESS_GET";
    case kHttpPost: return "HTTPACCESS_POST";
    case kUploadConfig: return "UPLOAD_CONFIG";
    case kIncomeStart: return "INCOME_START";
    case kIncomeEnd: return "INCOME_END";
    case kIncomeContinue: return "INCOME_CONTINUE";
    case kSetIncomeMode: return "SET_INCOME_MODE";
    case kDestroyService: return "DESTROY_SERVICE";
    case kIncomePoint: return "INCOME_POINT";
    case kGameStatusReset: return "GAME_STATUS_RESET";
    case kRowEventDataList: return "ROW_EVENTDATA_LIST";
    case kShopping: return "SHOPPING";
    case kHttpsGet: return "HTTPSACCESS_GET";
    case kHttpsPost: return "HTTPSACCESS_POST";
    case kFreeTicket: return "FREE_TICKET";
    case kGameFreeStart: return "GAME_FREE_START";
    case kGameFreeEnd: return "GAME_FREE_END";
    case kIncomeFreeStart: return "INCOME_FREE_START";
    case kIncomeFreeEnd: return "INCOME_FREE_END";
    case kClientEnd: return "CLIENT_END";
    default: return "unknown command";
    }
}

const char* ReplyName(std::uint32_t reply)
{
    switch (reply) {
    case kNwRecoverNotice: return "NWRECOVER_NOTICE";
    case kCertInitNotice: return "CERT_INIT_NOTICE";
    case kEffectiveEventNotice: return "EFFECTIVE_EVENT_NOTICE";
    case kDhcpRenewStart: return "DHCP_RENEW_START";
    case kDhcpCompleteNotice: return "DHCP_COMPLETE_NOTICE";
    case kClientStartReply: return "CLIENT_START_REPLY";
    case kConnectReply: return "CONNECT_REPLY";
    case kDisconnectReply: return "DISCONNECT_REPLY";
    case kGameStatusReply: return "GAME_STATUS_REPLY";
    case kCardSelectReply: return "CARD_SELECT_REPLY";
    case kCardInsertReply: return "CARD_INSERT_REPLY";
    case kCardUpdateReply: return "CARD_UPDATE_REPLY";
    case kCardBuysItemReply: return "CARD_BUYS_ITEM_REPLY";
    case kCardTakeoverReply: return "CARD_TAKEOVER_REPLY";
    case kCardDecreaseReply: return "CARD_DECREASE_REPLY";
    case kCardReissueTestReply: return "CARD_REISSUE_TEST_REPLY";
    case kCardReissueReply: return "CARD_REISSUE_REPLY";
    case kCardPlayedListReply: return "CARD_PLAYED_LIST_REPLY";
    case kRankingDataReply: return "RANKING_DATA_REPLY";
    case kLocalNwInfoReply: return "LOCALNW_INFO_REPLY";
    case kLocalNwInfoNotice: return "LOCALNW_INFO_NOTICE";
    case kGlobalAddrReply: return "GLOBALADDR_REPLY";
    case kEchoReply: return "ECHO_REPLY";
    case kAdapterInfoReply: return "ADAPTER_INFO_REPLY";
    case kServiceVersionReply: return "SERVICE_VERSION_REPLY";
    case kHttpAccessStart: return "HTTPACCESS_START";
    case kHttpAccessReply: return "HTTPACCESS_REPLY";
    case kUploadConfigReply: return "UPLOAD_CONFIG_REPLY";
    case kIncomeStatusReply: return "INCOME_STATUS_REPLY";
    case kSetIncomeModeReply: return "SET_INCOME_MODE_REPLY";
    case kDestroyServiceReply: return "DESTROY_SERVICE_REPLY";
    case kGameStatusResetReply: return "GAME_STATUS_RESET_REPLY";
    case kRowEventDataListReply: return "ROW_EVENTDATA_LIST_REPLY";
    case kShoppingReply: return "SHOPPING_REPLY";
    case kFreeTicketReply: return "FREE_TICKET_REPLY";
    case kClientEndReply: return "CLIENT_END_REPLY";
    default: return "unknown reply";
    }
}

class Session
{
public:
    explicit Session(HANDLE pipe) : pipe_(pipe) {}

    void Run()
    {
        std::vector<unsigned char> message;
        while (ReadMessage(message)) {
            std::size_t offset = 0;
            while (offset + kMessageHeaderBytes <= message.size()) {
                const std::uint32_t command = ReadU32(message.data(), offset);
                const std::uint32_t length = ReadU32(message.data(), offset + 4);
                const std::size_t payload = offset + kMessageHeaderBytes;
                if (length > message.size() - payload) {
                    LOG("ERROR: NESYS message 0x%X claims %u bytes, %zu left", command, length,
                        message.size() - payload);
                    break;
                }
                Dispatch(command, message.data() + payload, length);
                offset = payload + length;
            }
        }
    }

private:
    bool ReadMessage(std::vector<unsigned char>& message)
    {
        message.clear();
        unsigned char chunk[4096];
        for (;;) {
            DWORD read = 0;
            const BOOL ok = ReadFile(pipe_, chunk, sizeof(chunk), &read, nullptr);
            message.insert(message.end(), chunk, chunk + read);
            if (ok)
                return true;
            if (GetLastError() != ERROR_MORE_DATA)
                return false;
        }
    }

    void Send(std::uint32_t reply, const Payload& payload = Payload())
    {
        const std::vector<unsigned char>& bytes = payload.Bytes();
        if (g_trace)
            LOG("NESYS trace < %s (0x%03X), %zu bytes: %s", ReplyName(reply), reply, bytes.size(),
                HexPreview(bytes.data(), bytes.size()).c_str());
        std::vector<unsigned char> message(kMessageHeaderBytes + bytes.size());
        const auto length = static_cast<std::uint32_t>(bytes.size());
        std::memcpy(message.data(), &reply, sizeof(reply));
        std::memcpy(message.data() + 4, &length, sizeof(length));
        if (!bytes.empty())
            std::memcpy(message.data() + kMessageHeaderBytes, bytes.data(), bytes.size());
        DWORD written = 0;
        if (!WriteFile(pipe_, message.data(), static_cast<DWORD>(message.size()), &written, nullptr)
            || written != message.size())
            LOG("ERROR: NESYS reply 0x%X was not written (%lu)", reply, GetLastError());
    }

    void Dispatch(std::uint32_t command, const unsigned char* data, std::size_t size)
    {
        if (g_trace)
            LOG("NESYS trace > %s (0x%02X), %zu bytes: %s", CommandName(command), command, size,
                HexPreview(data, size).c_str());
        else if (command < 256 && !g_loggedCommand[command].exchange(true))
            LOG("NESYS: %s (0x%02X), %zu bytes", CommandName(command), command, size);

        switch (command) {
        case kClientStart: Send(kClientStartReply); break;
        case kConnect: Connect(); break;
        case kDisconnect: Send(kDisconnectReply); break;
        case kGameStart:
        case kGameEnd:
        case kGameContinue:
        case kGameFreeStart:
        case kGameFreeEnd: Send(kGameStatusReply); break;
        case kEventDownload:
        case kEventRequest: SendSizedEmpty(kEffectiveEventNotice); break;
        case kCardSelect: CardSelect(data, size); break;
        case kCardInsert: CardSend(kCardInsertReply, "registercard", data, size); break;
        case kCardUpdate: CardUpdate(data, size); break;
        case kCardBuysItem: CardSend(kCardBuysItemReply, "cardbuysitem", data, size); break;
        case kCardTakeover:
        case kCardForceTakeover: CardPlain(kCardTakeoverReply, "cardtakeover", data, size); break;
        case kCardDecrease: CardPlain(kCardDecreaseReply, "carddecrease", data, size); break;
        case kCardReissueTest: CardPlain(kCardReissueTestReply, "cardreissuetest", data, size); break;
        case kCardReissue: CardPlain(kCardReissueReply, "reissuecard", data, size); break;
        case kCardPlayedList: CardPlain(kCardPlayedListReply, "cardplayedlist", data, size); break;
        case kRankingData: RankingData(data, size); break;
        case kLocalNwInfo:
            Send(kLocalNwInfoReply);
            SendStatusAndSize(kLocalNwInfoNotice);
            break;
        case kGlobalAddr: GlobalAddr(); break;
        case kEcho: Echo(); break;
        case kAdapterInfo: AdapterInfo(); break;
        case kServiceVersion: ServiceVersion(); break;
        case kDhcpRenew:
            Send(kDhcpRenewStart);
            Send(kDhcpCompleteNotice);
            break;
        case kHttpGet: Http(kHttpGetLayout, "HTTP", data, size); break;
        case kHttpPost: Http(kHttpPostLayout, "HTTP", data, size); break;
        case kHttpsGet: Http(kHttpsGetLayout, "HTTPS", data, size); break;
        case kHttpsPost: Http(kHttpsPostLayout, "HTTPS", data, size); break;
        case kUploadConfig: SendStatus(kUploadConfigReply); break;
        case kIncomeStart:
        case kIncomeEnd:
        case kIncomeContinue:
        case kIncomePoint:
        case kIncomeFreeStart:
        case kIncomeFreeEnd: IncomeStatus(); break;
        case kSetIncomeMode: Send(kSetIncomeModeReply); break;
        case kDestroyService: Send(kDestroyServiceReply); break;
        case kGameStatusReset: Send(kGameStatusResetReply); break;
        case kRowEventDataList: RowEventDataList(); break;
        case kShopping: SendStatus(kShoppingReply); break;
        case kFreeTicket: FreeTicket(); break;
        case kClientEnd: Send(kClientEndReply); break;
        default:
            LOG("ERROR: NESYS command 0x%X (%zu bytes) has no reply", command, size);
            break;
        }
    }

    void Connect()
    {
        Send(kConnectReply);
        // Event 6: NesysManager+5 (network up). It must precede CERT_INIT,
        // whose event 1 reads the server profile only while +5 is set.
        Send(kNwRecoverNotice);

        Payload cert;
        cert.U32(1);  // tenpo_id
        cert.Zeros(kTenpoBytes - sizeof(std::uint32_t));
        cert.PutText(kTenpoNameOffset, "DDGACreg", kTenpoNameBytes);
        cert.PutText(kTenpoAddrOffset, "Local", kTenpoAddrBytes);
        cert.PutText(kTenpoTicketOffset, "0000", kTenpoTicketBytes);
        cert.PutText(kTenpoPrefOffset, "Tokyo", kTenpoPrefBytes);
        cert.Zeros(kNewsBytes);  // news type 0, size 0, no file
        cert.U32(static_cast<std::uint32_t>(std::strlen(kServerProfile)));
        cert.Raw(kServerProfile, std::strlen(kServerProfile));
        Send(kCertInitNotice, cert);
        LOG("NESYS: connected (server profile \"%s\", content port %d)", kServerProfile, g_contentPort);
    }

    void SendStatus(std::uint32_t reply)
    {
        Payload payload;
        payload.U32(1);
        Send(reply, payload);
    }

    void SendStatusAndSize(std::uint32_t reply)
    {
        Payload payload;
        payload.U32(1);
        payload.U32(0);
        Send(reply, payload);
    }

    void SendSizedEmpty(std::uint32_t reply)
    {
        Payload payload;
        payload.U32(0);
        Send(reply, payload);
    }

    void GlobalAddr()
    {
        Payload payload;
        payload.U32(1);  // status
        payload.U32(1);  // response time
        payload.Text("127.0.0.1", kGlobalAddrTextBytes);
        Send(kGlobalAddrReply, payload);
    }

    void Echo()
    {
        Payload payload;
        payload.U32(1);
        payload.Zeros(kEchoBytes - sizeof(std::uint32_t));
        Send(kEchoReply, payload);
    }

    void AdapterInfo()
    {
        Payload payload;
        payload.U32(1);  // interface status
        payload.U32(0);  // DHCP disabled
        payload.Text("127.0.0.1", kAdapterTextBytes);  // gateway
        payload.Text("127.0.0.1", kAdapterTextBytes);  // address
        payload.Text("255.0.0.0", kAdapterTextBytes);  // mask
        payload.Text("", kAdapterTextBytes);           // DHCP servers
        payload.Text("127.0.0.1", kAdapterTextBytes);  // DNS servers
        payload.Text("000000000000", kAdapterMacBytes);
        Send(kAdapterInfoReply, payload);
    }

    void ServiceVersion()
    {
        Payload payload;
        payload.Text("DDGACreg NESYS 1.0", kVersionBytes);
        Send(kServiceVersionReply, payload);
    }

    void IncomeStatus()
    {
        Payload payload;
        payload.U32(0);  // player id
        payload.U32(1);  // status
        Send(kIncomeStatusReply, payload);
    }

    void RowEventDataList()
    {
        Payload payload;
        payload.U32(static_cast<std::uint32_t>(kRowEventEntryBytes));
        payload.Zeros(kRowEventEntryBytes);
        Send(kRowEventDataListReply, payload);
    }

    void FreeTicket()
    {
        Payload payload;
        payload.U32(0);  // no ticket list
        Send(kFreeTicketReply, payload);
    }

    // HTTP and HTTPS requests share the reply: HTTPACCESS_START, then
    // HTTPACCESS_REPLY with the status, the error, the request's sequence and
    // the body (event 35, sub_140239800).
    void Http(const HttpLayout& layout, const char* scheme, const unsigned char* data, std::size_t size)
    {
        if (size < layout.fixedBytes) {
            LOG("ERROR: NESYS %s %ls request is %zu bytes, expected at least %zu", scheme, layout.method, size,
                layout.fixedBytes);
            return;
        }
        const std::string url = FixedText(data, kHttpUrlBytes);
        const std::uint32_t sequence = ReadU32(data, layout.sequence);
        std::string header;
        const unsigned char* body = nullptr;
        std::size_t bodySize = 0;
        if (layout.post) {
            header = FixedText(data + layout.header, kHttpHeaderBytes);
            bodySize = ReadU32(data, layout.dataSize);
            if (bodySize > size - layout.fixedBytes) {
                LOG("ERROR: NESYS %s POST %s claims %zu body bytes, %zu present", scheme, url.c_str(), bodySize,
                    size - layout.fixedBytes);
                return;
            }
            body = data + layout.fixedBytes;
        }

        Payload start;
        start.U32(1);
        Send(kHttpAccessStart, start);

        std::string target;
        HttpResult result;
        if (LocalContentUrl(url, target)) {
            result = SendHttp(layout.method, target, header, body, bodySize);
        } else {
            result.error = ERROR_ACCESS_DENIED;
            LOG("NESYS: %s %ls %s is outside the game API; answered with error %lu as an offline cabinet would",
                scheme, layout.method, url.c_str(), result.error);
        }
        const bool ping = url.size() >= 5 && url.compare(url.size() - 5, 5, "/ping") == 0;
        const bool ok = result.error == 0 && result.status == 200;
        std::atomic<bool>& pingLogged = ok ? g_loggedPingOk : g_loggedPingFailed;
        if (g_trace || !ping || !pingLogged.exchange(true))
            LOG("NESYS: %s %ls %s -> %s %lu (%zu bytes)", scheme, layout.method, url.c_str(), target.c_str(),
                result.status, result.body.size());
        if (g_trace) {
            if (bodySize > 0)
                LOG("NESYS trace   request seq %u: %s", sequence,
                    TextPreview(std::string(reinterpret_cast<const char*>(body), bodySize)).c_str());
            LOG("NESYS trace   response seq %u: %s", sequence, TextPreview(result.body).c_str());
        }

        Payload reply;
        char statusLine[64];
        sprintf_s(statusLine, "HTTP/1.1 %lu", result.status);
        reply.Text(statusLine, kReplyHeaderBytes);
        reply.U32(result.status);
        reply.U32(result.error);
        reply.U32(sequence);
        reply.U32(static_cast<std::uint32_t>(result.body.size()));
        reply.Raw(result.body.data(), result.body.size());
        Send(kHttpAccessReply, reply);
    }

    // Wraps the game's card document with the card id the content server
    // keys the player on. A JSON object keeps its own fields.
    static std::string CardBody(const std::string& cardId, const std::string& text)
    {
        std::size_t begin = 0;
        while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r'
                                       || text[begin] == '\n'))
            ++begin;
        const std::string id = "{\"card_id\":\"" + JsonEscape(cardId) + "\"";
        if (begin < text.size() && text[begin] == '{') {
            std::size_t next = begin + 1;
            while (next < text.size() && (text[next] == ' ' || text[next] == '\t' || text[next] == '\r'
                                          || text[next] == '\n'))
                ++next;
            if (next < text.size() && text[next] == '}')
                return id + "}";
            return id + "," + text.substr(begin + 1);
        }
        if (text.empty())
            return id + "}";
        return id + ",\"param\":\"" + JsonEscape(text) + "\"}";
    }

    // Posts a document to a content route. True when the server answers with
    // result 1; result 0 means it has no data for the command, which the game
    // gets as a failed request.
    bool ContentPost(const char* route, const std::string& subject, const std::string& body, std::string& json)
    {
        const HttpResult result = SendHttp(L"POST", ContentUrl(route), "Content-type: application/json",
                                           reinterpret_cast<const unsigned char*>(body.data()), body.size());
        std::string code;
        const bool answered = result.error == 0 && result.status == 200 && SplitResultLine(result.body, code, json);
        const bool ok = answered && code == "1";
        if (result.error == 0 && !answered)
            LOG("ERROR: NESYS %s for %s got HTTP %lu: %.120s", route, subject.c_str(), result.status,
                result.body.c_str());
        else if (answered && !ok)
            LOG("NESYS: %s for %s declined by the content server (result %s)", route, subject.c_str(),
                code.c_str());
        LOG("NESYS: %s %s -> %s (%zu bytes)", route, subject.c_str(), ok ? "ok" : "failed", ok ? json.size() : 0);
        if (g_trace) {
            LOG("NESYS trace   request: %s", TextPreview(body).c_str());
            LOG("NESYS trace   response: %s", TextPreview(result.body).c_str());
        }
        if (!ok)
            json.clear();
        return ok;
    }

    void CardReply(std::uint32_t reply, const char* route, const std::string& cardId, const std::string& text)
    {
        std::string json;
        const bool ok = ContentPost(route, "card " + cardId, CardBody(cardId, text), json);
        if (ok && (reply == kCardSelectReply || reply == kCardInsertReply)) {
            if (nesys::MarkNesicaUsed())
                LOG("NESYS: card %s marked this play as a NESiCA play", cardId.c_str());
            else
                LOG("ERROR: NESYS card %s could not mark the NESiCA play (no NesysManager)", cardId.c_str());
        }
        SendCardReply(reply, ok, json);
    }

    void SendCardReply(std::uint32_t reply, bool ok, const std::string& json)
    {
        Payload payload;
        payload.U32(ok ? kCardStatusOk : 0);
        payload.U32(0);  // times
        payload.U32(0);  // days
        payload.U32(static_cast<std::uint32_t>(json.size()));
        payload.Raw(json.data(), json.size());
        Send(reply, payload);
    }

    void CardUpdate(const unsigned char* data, std::size_t size)
    {
        if (size < kCardSendDataOffset) {
            LOG("ERROR: NESYS CARD_UPDATE request is %zu bytes", size);
            return;
        }
        const std::uint32_t type = ReadU32(data, kCardSendDataTypeOffset);
        const char* route = CardRouteCommand(type);
        if (route == nullptr) {
            LOG("ERROR: NESYS CARD_UPDATE type %u has no content route; answered as failed", type);
            SendCardReply(kCardUpdateReply, false, std::string());
            return;
        }
        CardSend(kCardUpdateReply, route, data, size);
    }

    void RankingData(const unsigned char* data, std::size_t size)
    {
        if (size < kRankingDataOffset) {
            LOG("ERROR: NESYS RANKING_DATA request is %zu bytes", size);
            return;
        }
        const std::uint32_t type = ReadU32(data, 0);
        const std::uint32_t dataSize = ReadU32(data, kRankingDataSizeOffset);
        const std::size_t available = size - kRankingDataOffset;
        const std::string document(reinterpret_cast<const char*>(data + kRankingDataOffset),
                                   dataSize < available ? dataSize : available);
        const char* route = CardRouteCommand(type);
        std::string json;
        bool ok = false;
        if (route == nullptr)
            LOG("NESYS: RANKING_DATA type %u has no content route; answered as failed", type);
        else
            ok = ContentPost(route, "ranking type " + std::to_string(type), document, json);

        Payload payload;
        payload.U32(ok ? 1 : 0);
        payload.U32(static_cast<std::uint32_t>(json.size()));
        payload.Raw(json.data(), json.size());
        Send(kRankingDataReply, payload);
    }

    void CardSelect(const unsigned char* data, std::size_t size)
    {
        if (size < kCardSelectParamOffset) {
            LOG("ERROR: NESYS CARD_SELECT is %zu bytes", size);
            return;
        }
        const std::string cardId = FixedText(data, kCardIdBytes);
        const std::uint32_t paramSize = ReadU32(data, kCardSelectParamSizeOffset);
        const std::size_t available = size - kCardSelectParamOffset;
        const std::string param = FixedText(data + kCardSelectParamOffset,
                                            paramSize < available ? paramSize : available);
        CardReply(kCardSelectReply, "selectcard", cardId, param);
    }

    void CardSend(std::uint32_t reply, const char* route, const unsigned char* data, std::size_t size)
    {
        if (size < kCardSendDataOffset) {
            LOG("ERROR: NESYS card %s request is %zu bytes", route, size);
            return;
        }
        const std::string cardId = FixedText(data, kCardIdBytes);
        const std::uint32_t dataSize = ReadU32(data, kCardSendDataSizeOffset);
        const std::size_t available = size - kCardSendDataOffset;
        const std::size_t used = dataSize < available ? dataSize : available;
        const std::string document(reinterpret_cast<const char*>(data + kCardSendDataOffset), used);
        CardReply(reply, route, cardId, document);
    }

    void CardPlain(std::uint32_t reply, const char* route, const unsigned char* data, std::size_t size)
    {
        if (size < kCardIdBytes) {
            LOG("ERROR: NESYS card %s request is %zu bytes", route, size);
            return;
        }
        CardReply(reply, route, FixedText(data, kCardIdBytes), std::string());
    }

    HANDLE pipe_;
};

DWORD WINAPI SessionThread(LPVOID parameter)
{
    const HANDLE pipe = static_cast<HANDLE>(parameter);
    LOG("NESYS: game client connected");
    Session(pipe).Run();
    LOG("NESYS: game client disconnected");
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    return 0;
}

HANDLE CreatePipeInstance(bool first)
{
    const DWORD openMode = PIPE_ACCESS_DUPLEX | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
    return CreateNamedPipeW(kPipeName, openMode, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                            PIPE_UNLIMITED_INSTANCES, kPipeBufferBytes, kPipeBufferBytes, 0, nullptr);
}

DWORD WINAPI ListenThread(LPVOID parameter)
{
    HANDLE pipe = static_cast<HANDLE>(parameter);
    for (;;) {
        const bool connected = ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
        if (connected) {
            const HANDLE session = CreateThread(nullptr, 0, &SessionThread, pipe, 0, nullptr);
            if (session != nullptr) {
                CloseHandle(session);
            } else {
                LOG("ERROR: NESYS session thread was not created (%lu)", GetLastError());
                CloseHandle(pipe);
            }
        } else {
            LOG("ERROR: NESYS ConnectNamedPipe failed (%lu)", GetLastError());
            CloseHandle(pipe);
        }
        pipe = CreatePipeInstance(false);
        if (pipe == INVALID_HANDLE_VALUE) {
            LOG("ERROR: NESYS pipe instance was not created (%lu)", GetLastError());
            return 1;
        }
    }
}

std::wstring ConfigPath(const wchar_t* moduleDir)
{
    std::wstring ini = moduleDir;
    ini += L"config.ini";
    return ini;
}

} // namespace

namespace features {

void Nesys_Install(const wchar_t* moduleDir)
{
    if (moduleDir == nullptr || moduleDir[0] == L'\0')
        return;
    const std::wstring ini = ConfigPath(moduleDir);
    if (GetPrivateProfileIntW(L"Nesys", L"Enabled", 1, ini.c_str()) == 0) {
        LOG("NESYS service disabled");
        return;
    }
    g_trace = GetPrivateProfileIntW(L"Nesys", L"Trace", 0, ini.c_str()) != 0;
    g_contentPort = Content_Port(moduleDir);

    g_http = WinHttpOpen(L"DDGACreg-NESYS/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                         WINHTTP_NO_PROXY_BYPASS, 0);
    if (g_http == nullptr) {
        LOG("ERROR: NESYS WinHttpOpen failed (%lu)", GetLastError());
        return;
    }
    WinHttpSetTimeouts(g_http, kResolveTimeoutMs, kConnectTimeoutMs, kTransferTimeoutMs, kTransferTimeoutMs);

    // Created here, before the game's NESYS_INIT, so its WaitNamedPipe finds it.
    const HANDLE pipe = CreatePipeInstance(true);
    if (pipe == INVALID_HANDLE_VALUE) {
        LOG("ERROR: NESYS pipe %ls was not created (%lu). Another NESYS service owns it.",
            kPipeName, GetLastError());
        return;
    }
    const HANDLE listener = CreateThread(nullptr, 0, &ListenThread, pipe, 0, nullptr);
    if (listener == nullptr) {
        LOG("ERROR: NESYS listener thread was not created (%lu)", GetLastError());
        CloseHandle(pipe);
        return;
    }
    CloseHandle(listener);
    LOG("NESYS service on %ls, content at 127.0.0.1:%d%s", kPipeName, g_contentPort,
        g_trace ? ", tracing every message" : "");

    if (!nesys::StartOperatorSettings())
        LOG("ERROR: NESYS operator settings were not installed; the game will not start NESYS");
    if (!nesys::StartStateWatch())
        LOG("ERROR: NESYS state watch was not installed; connection changes are not logged");
}

} // namespace features
