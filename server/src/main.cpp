#include "content_db.h"
#include "game_catalog.h"
#include "json_scan.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>
#include <vector>

namespace {

constexpr int kDefaultPort = 8750;
constexpr const char* kApiMarker = "/ahac-api/tg4gs/gs";
const std::string kResultLine = "1\n";
// Result 0: the server has no data for the command. The game treats the
// request as failed and keeps its own data.
const std::string kDeclinedResultLine = "0\n";
// EDifficultyType: Easy, Normal, Hard, Professional.
constexpr int kDifficultyCount = 4;
// Meister points granted by [Content] UnlockAll; the shop sells for them.
constexpr int kUnlockedMeisterPoint = 99999;

struct ServerOptions {
    // [Content] UnlockAll=1 releases every mission in every difficulty and
    // real mode at login and keeps meister_point topped up.
    bool unlockAll = false;
};

struct BaseUrl {
    std::string host = "127.0.0.1";
    int port = kDefaultPort;
    bool loopback = true;
};

struct Fields {
    int64_t playerId = 0;
    bool hasPlayerId = false;
    int creditNum = 0;
    bool hasCredit = false;
    int serviceCredit = 0;
    bool hasService = false;
    int missionId = 0;
    int difficulty = 0;
    int offset = 0;
    int count = 0;
    bool hasCount = false;
    int lineupId = 0;
    bool hasLineup = false;
    int num = 0;
    bool hasNum = false;
    int webPointDelta = 0;
    bool hasWebPoint = false;
    bool wappenReset = false;
    bool hasWappenReset = false;
    bool changeCustomize = false;
    bool hasChangeCustomize = false;
    std::string card;
    std::string body;
};

std::wstring ExeDirectory()
{
    wchar_t path[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        return L".";
    std::wstring dir(path, len);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return L".";
    dir.resize(slash);
    return dir;
}

std::wstring ParentDir(std::wstring dir)
{
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/'))
        dir.pop_back();
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos)
        return L"";
    dir.resize(slash);
    return dir;
}

bool FileExists(const std::wstring& path)
{
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring FindConfigIni()
{
    std::wstring dir = ExeDirectory();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        const std::wstring candidate = dir + L"\\config.ini";
        if (FileExists(candidate))
            return candidate;
        dir = ParentDir(dir);
    }
    return L"";
}

std::wstring FindDatabasePath()
{
    std::wstring dir = ExeDirectory();
    for (int i = 0; i < 8 && !dir.empty(); ++i) {
        if (FileExists(dir + L"\\server\\CMakeLists.txt"))
            return dir + L"\\server\\data\\content.sqlite";
        dir = ParentDir(dir);
    }
    return ExeDirectory() + L"\\data\\content.sqlite";
}

std::string ReadFileBytes(const std::wstring& path)
{
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file)
        return {};
    std::string data;
    char buf[1024];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), file)) > 0)
        data.append(buf, n);
    fclose(file);
    if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xEF)
        data.erase(0, 3);
    return data;
}

std::string Trim(std::string text)
{
    size_t begin = 0;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r'))
        ++begin;
    size_t end = text.size();
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r'))
        --end;
    return text.substr(begin, end - begin);
}

std::string ReadContentSetting(const std::string& key, const std::string& defaultValue)
{
    const std::wstring path = FindConfigIni();
    const std::string text = path.empty() ? std::string() : ReadFileBytes(path);
    bool inContent = false;
    size_t i = 0;
    while (i < text.size()) {
        size_t end = text.find('\n', i);
        if (end == std::string::npos)
            end = text.size();
        std::string line = Trim(text.substr(i, end - i));
        i = end + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        if (line.front() == '[' && line.back() == ']') {
            inContent = line == "[Content]";
            continue;
        }
        if (!inContent)
            continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        if (Trim(line.substr(0, eq)) == key)
            return Trim(line.substr(eq + 1));
    }
    return defaultValue;
}

bool IsLoopbackHost(const std::string& host)
{
    return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

BaseUrl ParseBaseUrl(const std::string& raw)
{
    BaseUrl parsed;
    std::string url = Trim(raw);
    if (url.empty())
        return parsed;
    const size_t scheme = url.find("://");
    std::string rest = scheme == std::string::npos ? url : url.substr(scheme + 3);
    const size_t slash = rest.find('/');
    if (slash != std::string::npos)
        rest.resize(slash);
    if (!rest.empty() && rest.front() == '[') {
        const size_t bracket = rest.find(']');
        parsed.host = bracket == std::string::npos ? rest : rest.substr(1, bracket - 1);
        if (bracket != std::string::npos && bracket + 2 < rest.size() && rest[bracket + 1] == ':')
            parsed.port = std::atoi(rest.c_str() + bracket + 2);
    } else {
        const size_t colon = rest.rfind(':');
        if (colon != std::string::npos && rest.find(':') == colon) {
            parsed.host = rest.substr(0, colon);
            parsed.port = std::atoi(rest.c_str() + colon + 1);
        } else {
            parsed.host = rest;
        }
    }
    if (parsed.host.empty())
        parsed.host = "127.0.0.1";
    if (parsed.port <= 0 || parsed.port > 65535)
        parsed.port = kDefaultPort;
    for (char& c : parsed.host) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    parsed.loopback = IsLoopbackHost(parsed.host);
    return parsed;
}

std::string UrlDecode(const std::string& text)
{
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '+' ) {
            out.push_back(' ');
        } else if (text[i] == '%' && i + 2 < text.size()) {
            auto hex = [](char c) {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            const int hi = hex(text[i + 1]);
            const int lo = hex(text[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
            } else {
                out.push_back(text[i]);
            }
        } else {
            out.push_back(text[i]);
        }
    }
    return out;
}

void ApplyField(Fields& fields, const std::string& key, const JsonField& value)
{
    if (!value.present)
        return;
    if (key == "player_id") {
        fields.hasPlayerId = true;
        fields.playerId = static_cast<int64_t>(value.number);
    } else if (key == "credit_num") {
        fields.hasCredit = true;
        fields.creditNum = static_cast<int>(value.number);
    } else if (key == "service_credit_num") {
        fields.hasService = true;
        fields.serviceCredit = static_cast<int>(value.number);
    } else if (key == "mission_id") {
        fields.missionId = JsonInt(value, 0);
    } else if (key == "difficulty") {
        fields.difficulty = JsonInt(value, 0);
    } else if (key == "offset") {
        fields.offset = JsonInt(value, 0);
    } else if (key == "count") {
        fields.hasCount = true;
        fields.count = JsonInt(value, 0);
    } else if (key == "lineup_id") {
        fields.hasLineup = true;
        fields.lineupId = JsonInt(value, 0);
    } else if (key == "num") {
        fields.hasNum = true;
        fields.num = JsonInt(value, 0);
    } else if (key == "web_point") {
        fields.hasWebPoint = true;
        fields.webPointDelta = JsonInt(value, 0);
    } else if (key == "wappen_reset") {
        fields.hasWappenReset = true;
        fields.wappenReset = JsonBool(value, false);
    } else if (key == "change_customize") {
        fields.hasChangeCustomize = true;
        fields.changeCustomize = JsonBool(value, false);
    } else if (key == "card" || key == "card_code" || key == "cardid" || key == "card_id" || key == "raw"
               || key == "access_code" || key == "nesica_id") {
        if (value.isString)
            fields.card = value.text;
        else if (value.number != 0)
            fields.card = std::to_string(static_cast<int64_t>(value.number));
    }
}

Fields ParseFields(const std::string& body)
{
    Fields fields;
    fields.body = body;
    size_t i = 0;
    JsonSkipWs(body, i);
    if (i < body.size() && body[i] == '{') {
        const char* keys[] = {
            "player_id", "credit_num", "service_credit_num", "mission_id", "difficulty",
            "offset", "count", "lineup_id", "num", "item_category", "web_point", "wappen_reset",
            "change_customize", "card", "card_code", "cardid", "card_id", "raw", "access_code", "nesica_id"
        };
        for (const char* key : keys)
            ApplyField(fields, key, JsonLookup(body, key));
        return fields;
    }
    size_t cursor = 0;
    while (cursor < body.size()) {
        size_t amp = body.find('&', cursor);
        if (amp == std::string::npos)
            amp = body.size();
        const std::string pair = body.substr(cursor, amp - cursor);
        cursor = amp + 1;
        const size_t eq = pair.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = UrlDecode(pair.substr(0, eq));
        const std::string text = UrlDecode(pair.substr(eq + 1));
        JsonField value;
        value.present = true;
        value.isString = true;
        value.text = text;
        char* end = nullptr;
        value.number = std::strtod(text.c_str(), &end);
        ApplyField(fields, key, value);
    }
    return fields;
}

std::string FmtNum(double value)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%.6g", value);
    return buf;
}

std::string TitleObject(const PlayerRow& player)
{
    // type, rank, and train_line are stored by the game as (json - 1).
    // category, year, and id are stored as-is.
    return std::string("{\"id\":") + std::to_string(player.titleId)
        + ",\"type\":" + std::to_string(player.titleType + 1)
        + ",\"category\":" + std::to_string(player.titleCategory)
        + ",\"rank\":" + std::to_string(player.titleRank + 1)
        + ",\"train_line\":" + std::to_string(player.titleTrainLine + 1)
        + ",\"name\":\"" + JsonEscape(player.titleName)
        + "\",\"ruby\":\"" + JsonEscape(player.titleRuby)
        + "\",\"description\":\"" + JsonEscape(player.titleDescription)
        + "\",\"year\":" + std::to_string(player.titleYear) + "}";
}

std::string SoundObject(const PlayerRow& player)
{
    return std::string("{\"bgm_volume\":") + FmtNum(player.bgmVolume)
        + ",\"driver_volume\":" + FmtNum(player.driverVolume)
        + ",\"conductor_volume\":" + FmtNum(player.conductorVolume)
        + ",\"inside_volume\":" + FmtNum(player.insideVolume)
        + ",\"companion_volume\":" + FmtNum(player.companionVolume) + "}";
}

std::string CountArray(const std::vector<ItemCount>& items)
{
    std::string body = "[";
    for (size_t n = 0; n < items.size(); ++n) {
        if (n)
            body += ',';
        body += std::string("{\"id\":") + std::to_string(items[n].itemId)
            + ",\"num\":" + std::to_string(items[n].num) + "}";
    }
    body += ']';
    return body;
}

std::string BadgeArray(const std::vector<BadgeEquip>& items)
{
    std::string body = "[";
    for (size_t n = 0; n < items.size(); ++n) {
        if (n)
            body += ',';
        body += std::string("{\"id\":") + std::to_string(items[n].itemId)
            + ",\"position\":" + std::to_string(items[n].position) + "}";
    }
    body += ']';
    return body;
}

std::string WappenArray(const std::vector<WappenEquip>& items)
{
    std::string body = "[";
    for (size_t n = 0; n < items.size(); ++n) {
        if (n)
            body += ',';
        body += std::string("{\"id\":") + std::to_string(items[n].itemId)
            + ",\"size_type\":" + std::to_string(items[n].sizeType)
            + ",\"position_x\":" + std::to_string(items[n].positionX)
            + ",\"position_y\":" + std::to_string(items[n].positionY) + "}";
    }
    body += ']';
    return body;
}

std::string TimsArray(const std::vector<TimsItem>& items)
{
    std::string body = "[";
    for (size_t n = 0; n < items.size(); ++n) {
        if (n)
            body += ',';
        const TimsItem& item = items[n];
        body += std::string("{\"id\":") + std::to_string(item.id)
            + ",\"train_id\":" + std::to_string(item.trainId)
            + ",\"custom_id\":" + std::to_string(item.customId)
            + ",\"name\":\"" + JsonEscape(item.name)
            + "\",\"description\":\"" + JsonEscape(item.description) + "\"}";
    }
    body += ']';
    return body;
}

std::string ShopCategoryObject(const std::vector<ShopLine>& lines, const char* category)
{
    std::string body = "{";
    bool first = true;
    for (const ShopLine& line : lines) {
        if (line.category != category)
            continue;
        if (!first)
            body += ',';
        first = false;
        body += std::string("\"") + std::to_string(line.lineupId) + "\":{\"id\":" + std::to_string(line.itemId)
            + ",\"price\":" + std::to_string(line.price)
            + ",\"status\":" + std::to_string(line.status)
            + ",\"buyable\":" + (line.buyable ? "true" : "false") + "}";
    }
    body += '}';
    return body;
}

std::vector<BadgeEquip> ParseBadges(const std::string& body)
{
    std::vector<BadgeEquip> badges;
    std::string raw;
    if (!JsonExtract(body, "badge", raw))
        return badges;
    for (const std::string& element : JsonElements(raw)) {
        BadgeEquip badge;
        badge.itemId = JsonInt(JsonLookup(element, "id"), 0);
        badge.position = JsonInt(JsonLookup(element, "position"), 0);
        badges.push_back(badge);
    }
    return badges;
}

std::vector<WappenEquip> ParseWappens(const std::string& body)
{
    std::vector<WappenEquip> wappens;
    std::string raw;
    if (!JsonExtract(body, "wappen", raw))
        return wappens;
    for (const std::string& element : JsonElements(raw)) {
        WappenEquip wappen;
        wappen.itemId = JsonInt(JsonLookup(element, "id"), 0);
        wappen.sizeType = JsonInt(JsonLookup(element, "size_type"), 1);
        wappen.positionX = JsonInt(JsonLookup(element, "position_x"), 0);
        wappen.positionY = JsonInt(JsonLookup(element, "position_y"), 0);
        if (wappen.itemId > 0)
            wappens.push_back(wappen);
    }
    return wappens;
}

SoundConfig ParseSound(const std::string& body, bool& present)
{
    SoundConfig sound;
    present = false;
    std::string raw;
    if (!JsonExtract(body, "sound_config", raw))
        return sound;
    present = true;
    const JsonField bgm = JsonLookup(raw, "bgm_volume");
    const JsonField driver = JsonLookup(raw, "driver_volume");
    const JsonField conductor = JsonLookup(raw, "conductor_volume");
    const JsonField inside = JsonLookup(raw, "inside_volume");
    const JsonField companion = JsonLookup(raw, "companion_volume");
    if (bgm.present)
        sound.bgm = bgm.number;
    if (driver.present)
        sound.driver = driver.number;
    if (conductor.present)
        sound.conductor = conductor.number;
    if (inside.present)
        sound.inside = inside.number;
    if (companion.present)
        sound.companion = companion.number;
    return sound;
}

std::string PlayerJson(ContentDb& db, const PlayerRow& player, bool exist, const std::string& extraFields = {})
{
    if (!exist || !player.found)
        return "{\"exist\":false}";
    std::string config = "{}";
    if (!player.configJson.empty()) {
        size_t begin = 0;
        JsonSkipWs(player.configJson, begin);
        size_t end = player.configJson.size();
        while (end > begin && (player.configJson[end - 1] == ' ' || player.configJson[end - 1] == '\t'
                               || player.configJson[end - 1] == '\r' || player.configJson[end - 1] == '\n'))
            --end;
        if (begin < end && player.configJson[begin] == '{' && player.configJson[end - 1] == '}')
            config = player.configJson.substr(begin, end - begin);
    }
    // sub_140213E60 reads exist, player_id, player_name, tims, and title.
    // sub_140214460 reads the point fields, information, sound_config, and the item lists.
    return std::string("{\"exist\":true,\"player_id\":") + std::to_string(player.id)
        + ",\"player_name\":\"" + JsonEscape(player.name) + "\""
        + ",\"credit_num\":" + std::to_string(player.credit)
        + ",\"service_credit_num\":" + std::to_string(player.serviceCredit)
        + ",\"need_credit\":1,\"category_id\":0,\"web_point\":" + std::to_string(player.webPoint)
        + ",\"total_web_point\":" + std::to_string(player.totalWebPoint)
        + ",\"meister_point\":" + std::to_string(player.meisterPoint)
        + ",\"latest_login_at\":\"" + JsonEscape(player.latestLoginAt) + "\""
        + ",\"web_point_limit\":99999,\"game_version\":\"5.80.02\""
        + ",\"playlog_id\":" + std::to_string(player.playlogId)
        + ",\"exp_mileage\":" + std::to_string(player.mileage)
        + ",\"information\":{\"name\":\"" + JsonEscape(player.name)
        + "\",\"rank\":0,\"mileage\":" + std::to_string(player.mileage)
        + ",\"change_customize\":" + (player.changeCustomize ? "true" : "false")
        + ",\"registered_web\":" + (player.registeredWeb ? "true" : "false") + "}"
        + ",\"sound_config\":" + SoundObject(player)
        + ",\"tims\":" + TimsArray(db.Tims(player.id))
        + ",\"title\":" + TitleObject(player)
        + ",\"badge_list\":" + CountArray(db.Owned(player.id, "badge"))
        + ",\"badge\":" + BadgeArray(db.Badges(player.id))
        + ",\"wappen_list\":" + CountArray(db.Owned(player.id, "wappen"))
        + ",\"wappen\":" + WappenArray(db.Wappens(player.id))
        + ",\"mission_ticket\":" + CountArray(db.Owned(player.id, "mission_ticket"))
        + ",\"config\":" + config + extraFields + "}";
}

// Login (sub_140211750) reads player_id, playable_mission (sub_140231590, one
// array per released mission and difficulty: mission number, difficulty + 1,
// play count, clear count, rank + 1, total score, near stop, perfect stop,
// perfect double) and playable_realmode (sub_1402306A0, mission numbers).
// NativePlayerDataObject.GetPlayableMission gates every mission on these.
std::string UnlockedMissionFields()
{
    std::string playable = "[";
    std::string realmode = "[";
    for (const int mission : kGameMissionNumbers) {
        const std::string number = std::to_string(mission);
        for (int difficulty = 1; difficulty <= kDifficultyCount; ++difficulty) {
            if (playable.size() > 1)
                playable += ',';
            // Rank 1 is EScoreRankType D; nothing is played yet.
            playable += "[" + number + "," + std::to_string(difficulty) + ",0,0,1,0,0,0,0]";
        }
        if (realmode.size() > 1)
            realmode += ',';
        realmode += number;
    }
    return ",\"playable_mission\":" + playable + "],\"playable_realmode\":" + realmode + "]";
}

std::string RankingJson(const std::vector<RankRow>& rows, bool includeReward)
{
    int64_t startAt = 1767225600000LL;
    int64_t endAt = 1798675200000LL;
    int64_t updateAt = startAt;
    if (!rows.empty()) {
        startAt = rows.front().startAt;
        endAt = rows.front().endAt;
        updateAt = rows.front().updateAt;
    }
    // start_at / end_at / update_at are strings. Ranking entry numbers are JSON numbers.
    // rank, title_type, title_rarity, title_train_line, and mode are adjusted by the client.
    std::string body = std::string("{\"start_at\":\"") + std::to_string(startAt)
        + "\",\"end_at\":\"" + std::to_string(endAt)
        + "\",\"update_at\":\"" + std::to_string(updateAt) + "\",\"ranking\":[";
    for (size_t n = 0; n < rows.size(); ++n) {
        const RankRow& row = rows[n];
        if (n)
            body += ',';
        const int wireRank = row.rank > 0 ? row.rank - 1 : 0;
        body += std::string("{\"rank\":") + std::to_string(wireRank)
            + ",\"player_id\":" + std::to_string(row.playerId)
            + ",\"name\":\"" + JsonEscape(row.name) + "\""
            + ",\"title_name\":\"" + JsonEscape(row.titleName) + "\""
            + ",\"title_type\":" + std::to_string(row.titleType + 1)
            + ",\"title_category\":" + std::to_string(row.titleCategory)
            + ",\"title_rarity\":" + std::to_string(row.titleRarity + 1)
            + ",\"title_train_line\":" + std::to_string(row.titleTrainLine + 1)
            + ",\"title_ruby\":\"" + JsonEscape(row.titleRuby) + "\""
            + ",\"title_year\":" + std::to_string(row.titleYear)
            + ",\"mode\":" + std::to_string(row.mode + 1)
            + ",\"value\":" + std::to_string(row.value)
            + ",\"store_name\":\"" + JsonEscape(row.storeName) + "\"}";
    }
    body += ']';
    if (includeReward)
        body += ",\"ranking_reward\":[]";
    body += '}';
    return body;
}

std::string InitJson()
{
    return "{\"game_version\":\"5.80.02\",\"web_point_limit\":99999,"
           "\"rank_border\":{\"D\":0,\"C\":100,\"B\":500,\"A\":1000,\"S\":5000,\"SS\":10000}}";
}

std::string Lower(std::string text)
{
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

std::string CommandFromPath(const std::string& path)
{
    const size_t marker = path.find(kApiMarker);
    if (marker == std::string::npos)
        return {};
    std::string rest = path.substr(marker + std::strlen(kApiMarker));
    const size_t query = rest.find('?');
    if (query != std::string::npos)
        rest.resize(query);
    while (!rest.empty() && rest.front() == '/')
        rest.erase(rest.begin());
    while (!rest.empty() && rest.back() == '/')
        rest.pop_back();
    const size_t slash = rest.find('/');
    if (slash != std::string::npos)
        rest.resize(slash);
    return Lower(rest);
}

bool UnderApi(const std::string& path)
{
    return path.find(kApiMarker) != std::string::npos;
}

std::string InventoryJson(ContentDb& db, const PlayerRow& player)
{
    return std::string("{\"meister_point\":") + std::to_string(player.meisterPoint)
        + ",\"badge_list\":" + CountArray(db.Owned(player.id, "badge"))
        + ",\"wappen_list\":" + CountArray(db.Owned(player.id, "wappen"))
        + ",\"mission_ticket\":" + CountArray(db.Owned(player.id, "mission_ticket")) + "}";
}

bool IsRegisterableCard(const std::string& code)
{
    if (code.size() != 16)
        return false;
    if (code.compare(0, 3, "707") != 0 && code.compare(0, 3, "709") != 0)
        return false;
    for (char ch : code) {
        const bool digit = ch >= '0' && ch <= '9';
        const bool upper = ch >= 'A' && ch <= 'Z';
        if (!digit && !upper)
            return false;
    }
    return true;
}

bool IsRawCommand(const std::string& command)
{
    return command == "getpicture" || command == "getpicturelist" || command == "postpicture"
        || command == "discontinuation";
}

// mission (CardUpdateType 21) would replace the game's mission tables
// (ApplyMissionData, sub_1401C6C60); this server has no mission catalog, so
// the game keeps the missions built into it.
bool IsDeclinedCommand(const std::string& command)
{
    return command == "mission";
}

const char* kRunRank =
    "{\"StopPoint\":\"A\",\"ArrivalTime\":\"A\",\"SafetyOperation\":\"A\","
    "\"SpeedRegulation\":\"A\",\"TechnicalSkill\":\"A\",\"Total\":\"A\"}";

const char* kMissionRank =
    "{\"StopPoint\":\"A\",\"ArrivalTime\":\"A\",\"SafetyOperation\":\"A\","
    "\"SpeedRegulation\":\"A\",\"TechnicalSkill\":\"A\",\"DelayRecoveryTime\":\"A\",\"Total\":\"A\"}";

const char* kMissionClear =
    "{\"StopPoint\":false,\"ArrivalTime\":false,\"SafetyOperation\":false,"
    "\"SpeedRegulation\":false,\"TechnicalSkill\":false,\"DelayRecoveryTime\":false,\"Total\":false}";

std::string GameResultJson()
{
    // sub_140214080 reads rank grade strings and clear_flag. Grades are SS/S/A/B/C.
    return std::string("{\"rank\":") + kRunRank + ",\"clear_flag\":true}";
}

std::string MissionResultJson()
{
    // sub_1402122A0 reads rank grade strings, a clear bool object, open_high, and cl_mission.
    return std::string("{\"rank\":") + kMissionRank + ",\"clear\":" + kMissionClear
        + ",\"open_high\":false,\"cl_mission\":false}";
}

std::string GameTotalJson(ContentDb& db, int64_t playerId)
{
    // sub_140214D50 reads these fields. Nested parsers after meister_point were not
    // recovered, so those keys are omitted. Mileage and meister come from the player row.
    const PlayerRow player = db.PlayerById(playerId);
    return std::string("{\"rank\":") + kRunRank
        + ",\"clear_flag\":true,\"high_score\":false,\"realmode\":false,\"change_title\":false"
        + ",\"driver_rank\":0,\"exp_mileage\":" + std::to_string(player.mileage)
        + ",\"meister_point\":" + std::to_string(player.meisterPoint)
        + ",\"meister_rank_up\":false}";
}

std::string RawOrResult(ContentDb& db, int64_t playerId, const std::string& command)
{
    const std::string stored = db.LoadCommandBody(playerId, command);
    size_t begin = 0;
    JsonSkipWs(stored, begin);
    if (begin < stored.size() && (stored[begin] == '{' || stored[begin] == '['))
        return stored;
    return "{\"result\":1}";
}

std::string Dispatch(ContentDb& db, const std::string& command, const Fields& fields, const ServerOptions& options)
{
    if (command == "ping")
        return "{\"result\":1}";
    if (command == "init")
        return InitJson();
    if (command.empty() || command == "login" || command == "relogin" || command == "logout") {
        if (command == "login" || command == "relogin") {
            if (fields.hasChangeCustomize)
                db.SetCustomizeFlag(fields.playerId, fields.changeCustomize);
            db.TouchLogin(fields.playerId);
            if (options.unlockAll) {
                db.TopUpMeisterPoint(fields.playerId, kUnlockedMeisterPoint);
                return PlayerJson(db, db.PlayerById(fields.playerId), true, UnlockedMissionFields());
            }
        }
        return PlayerJson(db, db.PlayerById(fields.playerId), true);
    }
    if (command == "machineunlock") {
        // CardUpdateType 40 (machine_unlock). The game marks the cabinet
        // unlocked when the request does not fail (sub_140211B20).
        return "{\"result\":1}";
    }
    if (command == "addcredit" || command == "insertcredit") {
        const int creditDelta = fields.hasCredit ? fields.creditNum : 0;
        const int serviceDelta = fields.hasService ? fields.serviceCredit : 0;
        return PlayerJson(db, db.AddCredit(fields.playerId, creditDelta, serviceDelta, fields.card), true);
    }
    if (command == "purchasepoint") {
        const int delta = fields.hasWebPoint ? fields.webPointDelta : 0;
        const PlayerRow player = db.PurchasePoint(fields.playerId, delta);
        return std::string("{\"web_point\":") + std::to_string(player.webPoint)
            + ",\"total_web_point\":" + std::to_string(player.totalWebPoint) + "}";
    }
    if (command == "shopinitialize") {
        const std::vector<ShopLine> lines = db.Shop();
        return std::string("{\"badge\":") + ShopCategoryObject(lines, "badge")
            + ",\"wappen\":" + ShopCategoryObject(lines, "wappen")
            + ",\"mission_ticket\":" + ShopCategoryObject(lines, "mission_ticket") + "}";
    }
    if (command == "iteminitialize") {
        // ApplyWappenData / ApplyWappenSetData (sub_1402000F0) merge entries over
        // the game's own tables, so empty lists keep the game's names and texts.
        return "{\"mission_ticket\":[],\"wappen\":[],\"wappen_set\":[]}";
    }
    if (command == "purchaseshopitem") {
        bool bought = false;
        const int num = fields.hasNum ? fields.num : 1;
        const PlayerRow player = db.PurchaseItem(fields.playerId, fields.lineupId, num, bought);
        (void)bought;
        return InventoryJson(db, player);
    }
    if (command == "playerbadge") {
        std::string badgeRaw;
        const bool hasBadge = JsonExtract(fields.body, "badge", badgeRaw);
        db.SetBadges(fields.playerId, ParseBadges(fields.body), hasBadge, fields.wappenReset);
        const PlayerRow player = db.PlayerById(fields.playerId);
        return std::string("{\"badge\":") + BadgeArray(db.Badges(player.id))
            + ",\"wappen\":" + WappenArray(db.Wappens(player.id)) + "}";
    }
    if (command == "playerwappen") {
        std::string raw;
        if (JsonExtract(fields.body, "wappen", raw))
            db.SetWappens(fields.playerId, ParseWappens(fields.body));
        const PlayerRow player = db.PlayerById(fields.playerId);
        return std::string("{\"wappen\":") + WappenArray(db.Wappens(player.id)) + "}";
    }
    if (command == "config") {
        bool present = false;
        const SoundConfig sound = ParseSound(fields.body, present);
        if (present)
            db.SetSound(fields.playerId, sound);
        const PlayerRow player = db.PlayerById(fields.playerId);
        return std::string("{\"sound_config\":") + SoundObject(player) + "}";
    }
    if (command == "gamestart")
        return PlayerJson(db, db.GameStart(fields.playerId), true);
    if (command == "gameresult" || command == "gameover" || command == "result") {
        db.ApplyProgress(fields.playerId, fields.body);
        db.SaveCommandBody(fields.playerId, command, fields.body);
        if (command == "gameresult")
            return GameResultJson();
        return PlayerJson(db, db.PlayerById(fields.playerId), true);
    }
    if (command == "gametotalresult" || command == "totalresult") {
        db.ApplyProgress(fields.playerId, fields.body);
        db.SaveCommandBody(fields.playerId, command, fields.body);
        if (command == "gametotalresult")
            return GameTotalJson(db, fields.playerId);
        return PlayerJson(db, db.PlayerById(fields.playerId), true);
    }
    if (command == "playend") {
        db.ApplyProgress(fields.playerId, fields.body);
        db.SaveCommandBody(fields.playerId, command, fields.body);
        return "{\"voice\":0,\"priority\":0}";
    }
    if (command == "gamemissionresult") {
        db.ApplyProgress(fields.playerId, fields.body);
        db.SaveCommandBody(fields.playerId, command, fields.body);
        return MissionResultJson();
    }
    if (command == "getmissionranking" || command == "ranking" || command == "getstoremileageranking"
        || command == "ranking_reward") {
        const int count = fields.hasCount ? fields.count : (command == "getstoremileageranking" ? 20 : 0);
        return RankingJson(db.Leaderboard(command, fields.offset, count), command == "ranking_reward");
    }
    if (command == "selectcard" || command == "select_card" || command == "cardselect"
        || command == "card_select") {
        // A new 16-character 707/709 code is registered here. Other unknown codes stay exist:false.
        const PlayerRow player = db.SelectCard(fields.card, IsRegisterableCard(fields.card));
        return PlayerJson(db, player, player.found);
    }
    if (command == "registercard" || command == "register_card" || command == "cardregister"
        || command == "reissuecard" || command == "cardunlock") {
        // NESYS CARD_INSERT carries the new player's document next to card_id.
        const PlayerRow player = db.SelectCard(fields.card.empty() ? "0123456789ABCDEF" : fields.card, true);
        if (player.found)
            db.ApplyProgress(player.id, fields.body);
        return PlayerJson(db, db.PlayerById(player.id), player.found);
    }
    if (command == "photo" || command.rfind("photo", 0) == 0)
        return "{\"result\":1}";
    if (IsRawCommand(command)) {
        db.ApplyProgress(fields.playerId, fields.body);
        if (!fields.body.empty() && fields.body.find('{') != std::string::npos)
            db.SaveCommandBody(fields.playerId, command, fields.body);
        return RawOrResult(db, fields.playerId, command);
    }
    // Confirmed commands are handled above. Anything else under the API prefix
    // still returns an object so the client does not treat the route as a 404.
    return "{\"result\":1}";
}

// NESYS card requests name the card, not the player. Commands that act on a
// player without naming one use the player that owns that card.
void ResolveCardPlayer(ContentDb& db, Fields& fields)
{
    if (fields.hasPlayerId || fields.card.empty())
        return;
    const PlayerRow player = db.SelectCard(fields.card, false);
    if (!player.found)
        return;
    fields.playerId = player.id;
    fields.hasPlayerId = true;
}

bool SendAll(SOCKET socket, const char* data, int size)
{
    int sent = 0;
    while (sent < size) {
        const int n = send(socket, data + sent, size - sent, 0);
        if (n <= 0)
            return false;
        sent += n;
    }
    return true;
}

void Reply(SOCKET socket, int status, const char* reason, const std::string& body)
{
    const std::string head = std::string("HTTP/1.1 ") + std::to_string(status) + " " + reason
        + "\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: "
        + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    SendAll(socket, head.data(), static_cast<int>(head.size()));
}

bool RecvRequest(SOCKET socket, std::string& method, std::string& path, std::string& body)
{
    std::string data;
    char buf[4096];
    while (data.find("\r\n\r\n") == std::string::npos) {
        if (data.size() > 65536)
            return false;
        const int n = recv(socket, buf, sizeof(buf), 0);
        if (n <= 0)
            return false;
        data.append(buf, n);
    }
    const size_t headerEnd = data.find("\r\n\r\n");
    const std::string head = data.substr(0, headerEnd);
    body = data.substr(headerEnd + 4);
    const size_t lineEnd = head.find("\r\n");
    const std::string requestLine = head.substr(0, lineEnd);
    const size_t sp1 = requestLine.find(' ');
    const size_t sp2 = requestLine.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos)
        return false;
    method = requestLine.substr(0, sp1);
    path = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);

    int contentLength = 0;
    size_t scan = 0;
    while (scan < head.size()) {
        size_t end = head.find("\r\n", scan);
        if (end == std::string::npos)
            end = head.size();
        const std::string line = head.substr(scan, end - scan);
        scan = end + 2;
        const std::string lower = Lower(line);
        const std::string prefix = "content-length:";
        if (lower.rfind(prefix, 0) == 0)
            contentLength = std::atoi(Trim(line.substr(prefix.size())).c_str());
    }
    if (contentLength < 0)
        contentLength = 0;
    if (contentLength > 1024 * 1024)
        contentLength = 1024 * 1024;
    while (static_cast<int>(body.size()) < contentLength) {
        const int n = recv(socket, buf, sizeof(buf), 0);
        if (n <= 0)
            break;
        body.append(buf, n);
    }
    if (static_cast<int>(body.size()) > contentLength)
        body.resize(static_cast<size_t>(contentLength));
    return true;
}

int Serve(ContentDb& db, int port, const ServerOptions& options)
{
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        std::cerr << "socket() failed\n";
        return 1;
    }
    BOOL exclusive = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::cerr << "bind 127.0.0.1:" << port << " failed\n";
        closesocket(listener);
        return 1;
    }
    sockaddr_in bound{};
    int boundLen = sizeof(bound);
    if (getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &boundLen) != 0
        || bound.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
        std::cerr << "Refusing non-loopback content bind\n";
        closesocket(listener);
        return 1;
    }
    if (listen(listener, 8) != 0) {
        std::cerr << "listen failed\n";
        closesocket(listener);
        return 1;
    }
    std::cout << "ddgac-content listening on 127.0.0.1:" << port << "\n" << std::flush;

    for (;;) {
        SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET)
            continue;
        DWORD timeout = 5000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        std::string method;
        std::string path;
        std::string body;
        if (!RecvRequest(client, method, path, body)) {
            closesocket(client);
            continue;
        }
        const size_t query = path.find('?');
        const std::string pathOnly = query == std::string::npos ? path : path.substr(0, query);
        if (!UnderApi(pathOnly)) {
            Reply(client, 404, "Not Found", "{\"result\":0}");
            std::cout << method << " " << pathOnly << " 404\n" << std::flush;
            closesocket(client);
            continue;
        }
        const std::string command = CommandFromPath(pathOnly);
        // The game reads the first body line as the result code (1 is success,
        // sub_140239800) and parses the JSON after it.
        if (IsDeclinedCommand(command)) {
            Reply(client, 200, "OK", kDeclinedResultLine + "{\"result\":0}");
            std::cout << method << " " << pathOnly << " 200 (no data, result 0)\n" << std::flush;
            closesocket(client);
            continue;
        }
        Fields fields = ParseFields(body);
        ResolveCardPlayer(db, fields);
        Reply(client, 200, "OK", kResultLine + Dispatch(db, command, fields, options));
        std::cout << method << " " << pathOnly << " 200\n" << std::flush;
        closesocket(client);
    }
    return 0;
}

} // namespace

int main()
{
    const BaseUrl base = ParseBaseUrl(ReadContentSetting("BaseUrl", "http://127.0.0.1:8750"));
    ServerOptions options;
    options.unlockAll = ReadContentSetting("UnlockAll", "0") == "1";
    if (!base.loopback) {
        std::cerr << "Refusing non-loopback content bind (" << base.host << ")\n";
        return 1;
    }

    const std::wstring dbPath = FindDatabasePath();
    std::error_code mkdirError;
    std::filesystem::create_directories(std::filesystem::path(dbPath).parent_path(), mkdirError);
    if (mkdirError) {
        std::cerr << "Could not create the database directory\n";
        return 1;
    }

    ContentDb db;
    if (!db.Open(dbPath)) {
        std::cerr << "Could not open the content database\n";
        return 1;
    }
    std::wcout << L"database " << dbPath << L"\n";
    if (options.unlockAll)
        std::cout << "UnlockAll: every mission, difficulty and real mode is released at login\n";

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
    const int rc = Serve(db, base.port, options);
    WSACleanup();
    return rc;
}
