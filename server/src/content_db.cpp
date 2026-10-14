#include "content_db.h"
#include "game_catalog.h"
#include "json_scan.h"

#include "sqlite3.h"

#include <ctime>
#include <iostream>
#include <iterator>

namespace {

// Shop lineup ids: a wappen keeps its own id, a badge is offset past them.
constexpr int kWappenLineupBase = 0;
constexpr int kBadgeLineupBase = 10000;
constexpr int kWappenPrice = 100;
constexpr int kBadgePrice = 300;

bool Exec(sqlite3* db, const char* sql)
{
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        std::cerr << "sqlite: " << (err ? err : sqlite3_errmsg(db)) << "\n";
        sqlite3_free(err);
        return false;
    }
    return true;
}

int64_t NowMs()
{
    return static_cast<int64_t>(std::time(nullptr)) * 1000;
}

std::string ColumnText(sqlite3_stmt* stmt, int col)
{
    const unsigned char* text = sqlite3_column_text(stmt, col);
    return text ? reinterpret_cast<const char*>(text) : std::string();
}

} // namespace

ContentDb::~ContentDb()
{
    if (db_)
        sqlite3_close(db_);
}

bool ContentDb::AddColumn(const char* table, const char* column, const char* decl)
{
    const std::string sql = std::string("ALTER TABLE ") + table + " ADD COLUMN " + column + " " + decl + ";";
    char* err = nullptr;
    const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
    if (rc == SQLITE_OK) {
        sqlite3_free(err);
        return true;
    }
    const std::string message = err ? err : "";
    sqlite3_free(err);
    // Existing databases already have the column after the first launch.
    return message.find("duplicate column") != std::string::npos;
}

bool ContentDb::Open(const std::wstring& path)
{
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
    const int rc = sqlite3_open16(path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::cerr << "sqlite open failed: " << (db_ ? sqlite3_errmsg(db_) : "no handle") << "\n";
        if (db_) {
            sqlite3_close(db_);
            db_ = nullptr;
        }
        return false;
    }
    if (!Exec(db_, "PRAGMA foreign_keys = ON;") || !EnsureSchema() || !SeedIfEmpty() || !EnsureContent())
        return false;
    return true;
}

bool ContentDb::EnsureSchema()
{
    return Exec(db_,
        "CREATE TABLE IF NOT EXISTS cards ("
        "  id INTEGER PRIMARY KEY,"
        "  card_code TEXT NOT NULL UNIQUE,"
        "  player_id INTEGER NOT NULL,"
        "  created_at INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS players ("
        "  id INTEGER PRIMARY KEY,"
        "  name TEXT NOT NULL,"
        "  title_name TEXT NOT NULL,"
        "  title_ruby TEXT NOT NULL,"
        "  title_description TEXT NOT NULL,"
        "  title_id INTEGER NOT NULL DEFAULT 1,"
        "  title_type INTEGER NOT NULL DEFAULT 0,"
        "  title_category INTEGER NOT NULL DEFAULT 0,"
        "  title_rank INTEGER NOT NULL DEFAULT 0,"
        "  title_train_line INTEGER NOT NULL DEFAULT 0,"
        "  title_year INTEGER NOT NULL DEFAULT 2019,"
        "  created_at INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS player_data ("
        "  player_id INTEGER PRIMARY KEY,"
        "  config_json TEXT NOT NULL,"
        "  credit INTEGER NOT NULL,"
        "  service_credit INTEGER NOT NULL,"
        "  web_point INTEGER NOT NULL,"
        "  total_web_point INTEGER NOT NULL DEFAULT 0,"
        "  meister_point INTEGER NOT NULL DEFAULT 0,"
        "  mileage INTEGER NOT NULL,"
        "  playlog_id INTEGER NOT NULL,"
        "  bgm_volume REAL NOT NULL DEFAULT 1,"
        "  driver_volume REAL NOT NULL DEFAULT 1,"
        "  conductor_volume REAL NOT NULL DEFAULT 1,"
        "  inside_volume REAL NOT NULL DEFAULT 1,"
        "  companion_volume REAL NOT NULL DEFAULT 1,"
        "  change_customize INTEGER NOT NULL DEFAULT 0,"
        "  registered_web INTEGER NOT NULL DEFAULT 1,"
        "  latest_login_at TEXT NOT NULL DEFAULT '0'"
        ");"
        "CREATE TABLE IF NOT EXISTS shop_lineup ("
        "  lineup_id INTEGER PRIMARY KEY,"
        "  category TEXT NOT NULL,"
        "  item_id INTEGER NOT NULL,"
        "  price INTEGER NOT NULL,"
        "  status INTEGER NOT NULL,"
        "  buyable INTEGER NOT NULL,"
        "  name TEXT NOT NULL,"
        "  description TEXT NOT NULL,"
        "  cell_type INTEGER NOT NULL,"
        "  max_num INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS owned_items ("
        "  player_id INTEGER NOT NULL,"
        "  category TEXT NOT NULL,"
        "  item_id INTEGER NOT NULL,"
        "  num INTEGER NOT NULL,"
        "  PRIMARY KEY (player_id, category, item_id)"
        ");"
        "CREATE TABLE IF NOT EXISTS equipped_badges ("
        "  player_id INTEGER NOT NULL,"
        "  position INTEGER NOT NULL,"
        "  item_id INTEGER NOT NULL,"
        "  PRIMARY KEY (player_id, position)"
        ");"
        "CREATE TABLE IF NOT EXISTS equipped_wappen ("
        "  player_id INTEGER NOT NULL,"
        "  item_id INTEGER NOT NULL,"
        "  size_type INTEGER NOT NULL,"
        "  position_x INTEGER NOT NULL,"
        "  position_y INTEGER NOT NULL,"
        "  PRIMARY KEY (player_id, item_id)"
        ");"
        "CREATE TABLE IF NOT EXISTS player_tims ("
        "  player_id INTEGER NOT NULL,"
        "  tims_id INTEGER NOT NULL,"
        "  train_id INTEGER NOT NULL,"
        "  custom_id INTEGER NOT NULL,"
        "  name TEXT NOT NULL,"
        "  description TEXT NOT NULL,"
        "  PRIMARY KEY (player_id, tims_id)"
        ");"
        "CREATE TABLE IF NOT EXISTS leaderboards ("
        "  id INTEGER PRIMARY KEY,"
        "  board TEXT NOT NULL,"
        "  rank INTEGER NOT NULL,"
        "  player_id INTEGER NOT NULL,"
        "  name TEXT NOT NULL,"
        "  title_name TEXT NOT NULL,"
        "  title_ruby TEXT NOT NULL,"
        "  title_type INTEGER NOT NULL,"
        "  title_category INTEGER NOT NULL,"
        "  title_rarity INTEGER NOT NULL,"
        "  title_train_line INTEGER NOT NULL,"
        "  title_year INTEGER NOT NULL,"
        "  mode INTEGER NOT NULL,"
        "  value INTEGER NOT NULL,"
        "  store_name TEXT NOT NULL,"
        "  start_at INTEGER NOT NULL,"
        "  end_at INTEGER NOT NULL,"
        "  update_at INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS credits ("
        "  id INTEGER PRIMARY KEY,"
        "  player_id INTEGER NOT NULL,"
        "  card_code TEXT,"
        "  amount INTEGER NOT NULL,"
        "  service_amount INTEGER NOT NULL,"
        "  kind TEXT NOT NULL,"
        "  created_at INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS command_payloads ("
        "  player_id INTEGER NOT NULL,"
        "  command TEXT NOT NULL,"
        "  body TEXT NOT NULL,"
        "  updated_at INTEGER NOT NULL,"
        "  PRIMARY KEY (player_id, command)"
        ");"
        "CREATE INDEX IF NOT EXISTS leaderboards_board ON leaderboards(board, rank);");
}

bool ContentDb::SeedIfEmpty()
{
    sqlite3_stmt* countStmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM cards;", -1, &countStmt, nullptr) != SQLITE_OK)
        return false;
    int cards = 0;
    if (sqlite3_step(countStmt) == SQLITE_ROW)
        cards = sqlite3_column_int(countStmt, 0);
    sqlite3_finalize(countStmt);
    if (cards > 0)
        return true;

    const int64_t now = NowMs();
    // Fixed 2026 window. The game reads these as decimal strings of unix milliseconds.
    const int64_t startAt = 1767225600000LL;
    const int64_t endAt = 1798675200000LL;

    if (!Exec(db_, "BEGIN IMMEDIATE;"))
        return false;

    const char* playerSql =
        "INSERT INTO players (id, name, title_name, title_ruby, title_description, created_at) "
        "VALUES (1, 'Local Player', 'Local', 'rokaru', 'Local player', ?);";
    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db_, playerSql, -1, &stmt, nullptr) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_int64(stmt, 1, now);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
    }
    sqlite3_finalize(stmt);
    if (!ok) {
        Exec(db_, "ROLLBACK;");
        return false;
    }

    const char* dataSql =
        "INSERT INTO player_data (player_id, config_json, credit, service_credit, web_point, total_web_point, mileage, playlog_id) "
        "VALUES (1, '{}', 5, 0, 1000, 1000, 0, 0);";
    if (!Exec(db_, dataSql)) {
        Exec(db_, "ROLLBACK;");
        return false;
    }

    const char* cardSql =
        "INSERT INTO cards (card_code, player_id, created_at) VALUES ('0123456789ABCDEF', 1, ?);";
    const char* card707Sql =
        "INSERT INTO cards (card_code, player_id, created_at) VALUES ('7070000000000001', 1, ?);";
    const char* cardInserts[] = {cardSql, card707Sql};
    for (const char* sql : cardInserts) {
        stmt = nullptr;
        ok = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_int64(stmt, 1, now);
            ok = sqlite3_step(stmt) == SQLITE_DONE;
        }
        sqlite3_finalize(stmt);
        if (!ok) {
            Exec(db_, "ROLLBACK;");
            return false;
        }
    }

    const char* boardSql =
        "INSERT INTO leaderboards ("
        "  board, rank, player_id, name, title_name, title_ruby, title_type, title_category,"
        "  title_rarity, title_train_line, title_year, mode, value, store_name, start_at, end_at, update_at"
        ") VALUES (?, 1, 1, 'Local Player', 'Local', 'rokaru', 0, 0, 0, 0, 2019, 0, 1000, 'Local', ?, ?, ?);";
    const char* boards[] = {"ranking", "getmissionranking", "getstoremileageranking", "ranking_reward"};
    for (const char* board : boards) {
        stmt = nullptr;
        ok = sqlite3_prepare_v2(db_, boardSql, -1, &stmt, nullptr) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_text(stmt, 1, board, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 2, startAt);
            sqlite3_bind_int64(stmt, 3, endAt);
            sqlite3_bind_int64(stmt, 4, now);
            ok = sqlite3_step(stmt) == SQLITE_DONE;
        }
        sqlite3_finalize(stmt);
        if (!ok) {
            Exec(db_, "ROLLBACK;");
            return false;
        }
    }

    return Exec(db_, "COMMIT;");
}

PlayerRow ContentDb::LoadPlayer(int64_t id)
{
    PlayerRow row;
    const char* sql =
        "SELECT p.id, p.name, p.title_name, p.title_ruby, p.title_description,"
        "       p.title_id, p.title_type, p.title_category, p.title_rank, p.title_train_line, p.title_year,"
        "       d.credit, d.service_credit, d.web_point, d.total_web_point, d.meister_point,"
        "       d.mileage, d.playlog_id, d.config_json,"
        "       d.bgm_volume, d.driver_volume, d.conductor_volume, d.inside_volume, d.companion_volume,"
        "       d.change_customize, d.registered_web, d.latest_login_at "
        "FROM players p JOIN player_data d ON d.player_id = p.id WHERE p.id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return row;
    sqlite3_bind_int64(stmt, 1, id);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        row.found = true;
        row.id = sqlite3_column_int64(stmt, 0);
        row.name = ColumnText(stmt, 1);
        row.titleName = ColumnText(stmt, 2);
        row.titleRuby = ColumnText(stmt, 3);
        row.titleDescription = ColumnText(stmt, 4);
        row.titleId = sqlite3_column_int(stmt, 5);
        row.titleType = sqlite3_column_int(stmt, 6);
        row.titleCategory = sqlite3_column_int(stmt, 7);
        row.titleRank = sqlite3_column_int(stmt, 8);
        row.titleTrainLine = sqlite3_column_int(stmt, 9);
        row.titleYear = sqlite3_column_int(stmt, 10);
        row.credit = sqlite3_column_int(stmt, 11);
        row.serviceCredit = sqlite3_column_int(stmt, 12);
        row.webPoint = sqlite3_column_int(stmt, 13);
        row.totalWebPoint = sqlite3_column_int(stmt, 14);
        row.meisterPoint = sqlite3_column_int(stmt, 15);
        row.mileage = sqlite3_column_int(stmt, 16);
        row.playlogId = sqlite3_column_int64(stmt, 17);
        row.configJson = ColumnText(stmt, 18);
        row.bgmVolume = sqlite3_column_double(stmt, 19);
        row.driverVolume = sqlite3_column_double(stmt, 20);
        row.conductorVolume = sqlite3_column_double(stmt, 21);
        row.insideVolume = sqlite3_column_double(stmt, 22);
        row.companionVolume = sqlite3_column_double(stmt, 23);
        row.changeCustomize = sqlite3_column_int(stmt, 24) != 0;
        row.registeredWeb = sqlite3_column_int(stmt, 25) != 0;
        row.latestLoginAt = ColumnText(stmt, 26);
        if (row.latestLoginAt.empty())
            row.latestLoginAt = "0";
    }
    sqlite3_finalize(stmt);
    return row;
}

bool ContentDb::EnsureContent()
{
    // Older databases were created before the shop columns existed.
    const char* playerCols[][2] = {
        {"title_id", "INTEGER NOT NULL DEFAULT 1"},
        {"title_type", "INTEGER NOT NULL DEFAULT 0"},
        {"title_category", "INTEGER NOT NULL DEFAULT 0"},
        {"title_rank", "INTEGER NOT NULL DEFAULT 0"},
        {"title_train_line", "INTEGER NOT NULL DEFAULT 0"},
        {"title_year", "INTEGER NOT NULL DEFAULT 2019"},
    };
    for (const auto& col : playerCols) {
        if (!AddColumn("players", col[0], col[1]))
            return false;
    }
    const char* dataCols[][2] = {
        {"total_web_point", "INTEGER NOT NULL DEFAULT 0"},
        {"meister_point", "INTEGER NOT NULL DEFAULT 0"},
        {"bgm_volume", "REAL NOT NULL DEFAULT 1"},
        {"driver_volume", "REAL NOT NULL DEFAULT 1"},
        {"conductor_volume", "REAL NOT NULL DEFAULT 1"},
        {"inside_volume", "REAL NOT NULL DEFAULT 1"},
        {"companion_volume", "REAL NOT NULL DEFAULT 1"},
        {"change_customize", "INTEGER NOT NULL DEFAULT 0"},
        {"registered_web", "INTEGER NOT NULL DEFAULT 1"},
        {"latest_login_at", "TEXT NOT NULL DEFAULT '0'"},
    };
    for (const auto& col : dataCols) {
        if (!AddColumn("player_data", col[0], col[1]))
            return false;
    }

    if (!EnsureShop())
        return false;

    const char* timsSql =
        "INSERT OR IGNORE INTO player_tims (player_id, tims_id, train_id, custom_id, name, description) "
        "VALUES (1, 1, 1, 1, 'Local TIMS', 'Local train monitor');";
    if (!Exec(db_, timsSql))
        return false;

    const char* pointSql =
        "UPDATE player_data SET web_point = 1000, "
        "  total_web_point = CASE WHEN total_web_point < 1000 THEN 1000 ELSE total_web_point END "
        "WHERE player_id = 1 AND web_point < 100;";
    if (!Exec(db_, pointSql))
        return false;

    // Existing databases keep 0123456789ABCDEF. The P-key id must resolve to player 1.
    sqlite3_stmt* cardStmt = nullptr;
    bool ok = sqlite3_prepare_v2(
                  db_,
                  "INSERT OR IGNORE INTO cards (card_code, player_id, created_at) "
                  "VALUES ('7070000000000001', 1, ?);",
                  -1, &cardStmt, nullptr) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_int64(cardStmt, 1, NowMs());
        ok = sqlite3_step(cardStmt) == SQLITE_DONE;
    }
    sqlite3_finalize(cardStmt);
    return ok;
}

bool ContentDb::LoadLineup(int lineupId, ShopLine& line) const
{
    const char* sql =
        "SELECT lineup_id, category, item_id, price, status, buyable, name, description, cell_type, max_num "
        "FROM shop_lineup WHERE lineup_id = ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_int(stmt, 1, lineupId);
    bool found = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        found = true;
        line.lineupId = sqlite3_column_int(stmt, 0);
        line.category = ColumnText(stmt, 1);
        line.itemId = sqlite3_column_int(stmt, 2);
        line.price = sqlite3_column_int(stmt, 3);
        line.status = sqlite3_column_int(stmt, 4);
        line.buyable = sqlite3_column_int(stmt, 5) != 0;
        line.name = ColumnText(stmt, 6);
        line.description = ColumnText(stmt, 7);
        line.cellType = sqlite3_column_int(stmt, 8);
        line.maxNum = sqlite3_column_int(stmt, 9);
    }
    sqlite3_finalize(stmt);
    return found;
}

// The meister shop sells every badge and wappen of the game's data tables
// (game_catalog.h). Names, textures and texts come from the game's own data,
// so the rows carry ids and prices only. Prices are meister points.
bool ContentDb::EnsureShop()
{
    const char* sql =
        "INSERT INTO shop_lineup ("
        "  lineup_id, category, item_id, price, status, buyable, name, description, cell_type, max_num"
        ") VALUES (?, ?, ?, ?, 0, 1, '', '', 0, 1) "
        "ON CONFLICT(lineup_id) DO UPDATE SET category = excluded.category, item_id = excluded.item_id, "
        "  price = excluded.price, status = 0, buyable = 1, name = '', description = '';";
    struct Category {
        const char* name;
        const int* ids;
        size_t count;
        int lineupBase;
        int price;
    };
    const Category categories[] = {
        {"wappen", kGameWappenIds, std::size(kGameWappenIds), kWappenLineupBase, kWappenPrice},
        {"badge", kGameBadgeIds, std::size(kGameBadgeIds), kBadgeLineupBase, kBadgePrice},
    };
    if (!Exec(db_, "BEGIN IMMEDIATE;"))
        return false;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        Exec(db_, "ROLLBACK;");
        return false;
    }
    bool ok = true;
    for (const Category& category : categories) {
        for (size_t i = 0; ok && i < category.count; ++i) {
            sqlite3_reset(stmt);
            sqlite3_bind_int(stmt, 1, category.lineupBase + category.ids[i]);
            sqlite3_bind_text(stmt, 2, category.name, -1, SQLITE_STATIC);
            sqlite3_bind_int(stmt, 3, category.ids[i]);
            sqlite3_bind_int(stmt, 4, category.price);
            ok = sqlite3_step(stmt) == SQLITE_DONE;
        }
    }
    sqlite3_finalize(stmt);
    if (!ok) {
        Exec(db_, "ROLLBACK;");
        return false;
    }
    return Exec(db_, "COMMIT;");
}

bool ContentDb::TopUpMeisterPoint(int64_t playerId, int minimum)
{
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE player_data SET meister_point = ? WHERE player_id = ? AND meister_point < ?;",
                           -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_int(stmt, 1, minimum);
    sqlite3_bind_int64(stmt, 2, playerId);
    sqlite3_bind_int(stmt, 3, minimum);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

PlayerRow ContentDb::PlayerById(int64_t id)
{
    if (id > 0) {
        PlayerRow row = LoadPlayer(id);
        if (row.found)
            return row;
    }
    return DefaultPlayer();
}

PlayerRow ContentDb::DefaultPlayer()
{
    PlayerRow row = LoadPlayer(1);
    if (row.found)
        return row;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT id FROM players ORDER BY id LIMIT 1;", -1, &stmt, nullptr) != SQLITE_OK)
        return row;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        row = LoadPlayer(sqlite3_column_int64(stmt, 0));
    sqlite3_finalize(stmt);
    return row;
}

PlayerRow ContentDb::SelectCard(const std::string& code, bool create)
{
    if (code.empty())
        return DefaultPlayer();

    sqlite3_stmt* stmt = nullptr;
    const char* lookup = "SELECT player_id FROM cards WHERE card_code = ?;";
    if (sqlite3_prepare_v2(db_, lookup, -1, &stmt, nullptr) != SQLITE_OK)
        return {};
    sqlite3_bind_text(stmt, 1, code.c_str(), -1, SQLITE_TRANSIENT);
    int64_t playerId = 0;
    bool foundCard = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        foundCard = true;
        playerId = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    if (foundCard)
        return LoadPlayer(playerId);
    if (!create) {
        PlayerRow missing;
        missing.found = false;
        return missing;
    }

    const int64_t now = NowMs();
    if (!Exec(db_, "BEGIN IMMEDIATE;"))
        return {};

    const char* insertPlayer =
        "INSERT INTO players (name, title_name, title_ruby, title_description, created_at) "
        "VALUES ('Local Player', 'Local', 'rokaru', 'Local player', ?);";
    stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db_, insertPlayer, -1, &stmt, nullptr) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_int64(stmt, 1, now);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
    }
    sqlite3_finalize(stmt);
    const int64_t newId = sqlite3_last_insert_rowid(db_);
    if (!ok) {
        Exec(db_, "ROLLBACK;");
        return {};
    }

    const char* insertData =
        "INSERT INTO player_data (player_id, config_json, credit, service_credit, web_point, total_web_point, mileage, playlog_id) "
        "VALUES (?, '{}', 5, 0, 1000, 1000, 0, 0);";
    stmt = nullptr;
    ok = sqlite3_prepare_v2(db_, insertData, -1, &stmt, nullptr) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_int64(stmt, 1, newId);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
    }
    sqlite3_finalize(stmt);
    if (!ok) {
        Exec(db_, "ROLLBACK;");
        return {};
    }

    const char* insertCard = "INSERT INTO cards (card_code, player_id, created_at) VALUES (?, ?, ?);";
    stmt = nullptr;
    ok = sqlite3_prepare_v2(db_, insertCard, -1, &stmt, nullptr) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_text(stmt, 1, code.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, newId);
        sqlite3_bind_int64(stmt, 3, now);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
    }
    sqlite3_finalize(stmt);
    if (!ok || !Exec(db_, "COMMIT;")) {
        Exec(db_, "ROLLBACK;");
        return {};
    }
    return LoadPlayer(newId);
}

PlayerRow ContentDb::AddCredit(int64_t playerId, int creditDelta, int serviceDelta, const std::string& card)
{
    PlayerRow row = PlayerById(playerId);
    if (!row.found)
        return row;
    const int nextCredit = row.credit + creditDelta;
    const int nextService = row.serviceCredit + serviceDelta;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE player_data SET credit = ?, service_credit = ? WHERE player_id = ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return row;
    sqlite3_bind_int(stmt, 1, nextCredit);
    sqlite3_bind_int(stmt, 2, nextService);
    sqlite3_bind_int64(stmt, 3, row.id);
    const bool updated = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (!updated)
        return row;

    const char* logSql =
        "INSERT INTO credits (player_id, card_code, amount, service_amount, kind, created_at) "
        "VALUES (?, ?, ?, ?, 'addcredit', ?);";
    stmt = nullptr;
    if (sqlite3_prepare_v2(db_, logSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, row.id);
        if (card.empty())
            sqlite3_bind_null(stmt, 2);
        else
            sqlite3_bind_text(stmt, 2, card.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, creditDelta);
        sqlite3_bind_int(stmt, 4, serviceDelta);
        sqlite3_bind_int64(stmt, 5, NowMs());
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    return LoadPlayer(row.id);
}

PlayerRow ContentDb::GameStart(int64_t playerId)
{
    PlayerRow row = PlayerById(playerId);
    if (!row.found)
        return row;
    const int spent = row.credit > 0 ? 1 : 0;
    const int nextCredit = row.credit - spent;
    const int64_t nextLog = row.playlogId + 1;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE player_data SET credit = ?, playlog_id = ? WHERE player_id = ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return row;
    sqlite3_bind_int(stmt, 1, nextCredit);
    sqlite3_bind_int64(stmt, 2, nextLog);
    sqlite3_bind_int64(stmt, 3, row.id);
    const bool updated = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (!updated)
        return row;

    const char* logSql =
        "INSERT INTO credits (player_id, card_code, amount, service_amount, kind, created_at) "
        "VALUES (?, NULL, ?, 0, 'gamestart', ?);";
    stmt = nullptr;
    if (sqlite3_prepare_v2(db_, logSql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, row.id);
        sqlite3_bind_int(stmt, 2, -spent);
        sqlite3_bind_int64(stmt, 3, NowMs());
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    return LoadPlayer(row.id);
}

std::vector<RankRow> ContentDb::Leaderboard(const std::string& board, int offset, int count)
{
    std::vector<RankRow> rows;
    if (offset < 0)
        offset = 0;
    if (count <= 0)
        count = 20;
    if (count > 100)
        count = 100;

    const char* sql =
        "SELECT rank, player_id, name, title_name, title_ruby, title_type, title_category,"
        "       title_rarity, title_train_line, title_year, mode, value, store_name,"
        "       start_at, end_at, update_at "
        "FROM leaderboards WHERE board = ? ORDER BY rank LIMIT ? OFFSET ?;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return rows;
    sqlite3_bind_text(stmt, 1, board.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, count);
    sqlite3_bind_int(stmt, 3, offset);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        RankRow row;
        row.rank = sqlite3_column_int(stmt, 0);
        row.playerId = sqlite3_column_int64(stmt, 1);
        row.name = ColumnText(stmt, 2);
        row.titleName = ColumnText(stmt, 3);
        row.titleRuby = ColumnText(stmt, 4);
        row.titleType = sqlite3_column_int(stmt, 5);
        row.titleCategory = sqlite3_column_int(stmt, 6);
        row.titleRarity = sqlite3_column_int(stmt, 7);
        row.titleTrainLine = sqlite3_column_int(stmt, 8);
        row.titleYear = sqlite3_column_int(stmt, 9);
        row.mode = sqlite3_column_int(stmt, 10);
        row.value = sqlite3_column_int(stmt, 11);
        row.storeName = ColumnText(stmt, 12);
        row.startAt = sqlite3_column_int64(stmt, 13);
        row.endAt = sqlite3_column_int64(stmt, 14);
        row.updateAt = sqlite3_column_int64(stmt, 15);
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(stmt);
    return rows;
}

PlayerRow ContentDb::PurchasePoint(int64_t playerId, int webPointDelta)
{
    PlayerRow row = PlayerById(playerId);
    if (!row.found || webPointDelta == 0)
        return row;
    const int nextPoint = row.webPoint + webPointDelta;
    const int nextTotal = row.totalWebPoint + (webPointDelta > 0 ? webPointDelta : 0);
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE player_data SET web_point = ?, total_web_point = ? WHERE player_id = ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return row;
    sqlite3_bind_int(stmt, 1, nextPoint);
    sqlite3_bind_int(stmt, 2, nextTotal);
    sqlite3_bind_int64(stmt, 3, row.id);
    const bool updated = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    if (!updated)
        return row;
    return LoadPlayer(row.id);
}

PlayerRow ContentDb::PurchaseItem(int64_t playerId, int lineupId, int num, bool& bought)
{
    bought = false;
    PlayerRow row = PlayerById(playerId);
    if (!row.found || num <= 0)
        return row;
    ShopLine line;
    if (!LoadLineup(lineupId, line) || !line.buyable || line.price < 0)
        return row;
    const int64_t cost64 = static_cast<int64_t>(line.price) * static_cast<int64_t>(num);
    if (cost64 < 0 || cost64 > row.meisterPoint)
        return row;
    const int cost = static_cast<int>(cost64);

    if (!Exec(db_, "BEGIN IMMEDIATE;"))
        return row;

    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db_,
                                 "UPDATE player_data SET meister_point = meister_point - ? "
                                 "WHERE player_id = ? AND meister_point >= ?;",
                                 -1, &stmt, nullptr) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_int(stmt, 1, cost);
        sqlite3_bind_int64(stmt, 2, row.id);
        sqlite3_bind_int(stmt, 3, cost);
        ok = sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db_) == 1;
    }
    sqlite3_finalize(stmt);
    if (!ok) {
        Exec(db_, "ROLLBACK;");
        return row;
    }

    const char* ownSql =
        "INSERT INTO owned_items (player_id, category, item_id, num) VALUES (?, ?, ?, ?) "
        "ON CONFLICT(player_id, category, item_id) DO UPDATE SET num = num + excluded.num;";
    stmt = nullptr;
    ok = sqlite3_prepare_v2(db_, ownSql, -1, &stmt, nullptr) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_int64(stmt, 1, row.id);
        sqlite3_bind_text(stmt, 2, line.category.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, line.itemId);
        sqlite3_bind_int(stmt, 4, num);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
    }
    sqlite3_finalize(stmt);
    if (!ok) {
        Exec(db_, "ROLLBACK;");
        return row;
    }

    // A purchase equips the item so the next badge or wappen payload includes it.
    if (line.category == "badge") {
        const char* equipSql =
            "INSERT INTO equipped_badges (player_id, position, item_id) VALUES (?, 0, ?) "
            "ON CONFLICT(player_id, position) DO UPDATE SET item_id = excluded.item_id;";
        stmt = nullptr;
        ok = sqlite3_prepare_v2(db_, equipSql, -1, &stmt, nullptr) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_int64(stmt, 1, row.id);
            sqlite3_bind_int(stmt, 2, line.itemId);
            ok = sqlite3_step(stmt) == SQLITE_DONE;
        }
        sqlite3_finalize(stmt);
    } else if (line.category == "wappen") {
        const char* equipSql =
            "INSERT INTO equipped_wappen (player_id, item_id, size_type, position_x, position_y) "
            "VALUES (?, ?, 1, 0, 0) "
            "ON CONFLICT(player_id, item_id) DO NOTHING;";
        stmt = nullptr;
        ok = sqlite3_prepare_v2(db_, equipSql, -1, &stmt, nullptr) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_int64(stmt, 1, row.id);
            sqlite3_bind_int(stmt, 2, line.itemId);
            ok = sqlite3_step(stmt) == SQLITE_DONE;
        }
        sqlite3_finalize(stmt);
    }
    if (!ok || !Exec(db_, "COMMIT;")) {
        Exec(db_, "ROLLBACK;");
        return row;
    }
    bought = true;
    return LoadPlayer(row.id);
}

bool ContentDb::SetBadges(int64_t playerId, const std::vector<BadgeEquip>& badges, bool replaceBadges, bool resetWappen)
{
    PlayerRow row = PlayerById(playerId);
    if (!row.found)
        return false;
    if (!Exec(db_, "BEGIN IMMEDIATE;"))
        return false;
    sqlite3_stmt* stmt = nullptr;
    bool ok = true;
    if (replaceBadges) {
        ok = sqlite3_prepare_v2(db_, "DELETE FROM equipped_badges WHERE player_id = ?;", -1, &stmt, nullptr) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_int64(stmt, 1, row.id);
            ok = sqlite3_step(stmt) == SQLITE_DONE;
        }
        sqlite3_finalize(stmt);
    }
    if (ok && resetWappen) {
        stmt = nullptr;
        ok = sqlite3_prepare_v2(db_, "DELETE FROM equipped_wappen WHERE player_id = ?;", -1, &stmt, nullptr) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_int64(stmt, 1, row.id);
            ok = sqlite3_step(stmt) == SQLITE_DONE;
        }
        sqlite3_finalize(stmt);
    }
    const char* insertSql = "INSERT OR REPLACE INTO equipped_badges (player_id, position, item_id) VALUES (?, ?, ?);";
    for (const BadgeEquip& badge : badges) {
        if (!ok || !replaceBadges)
            break;
        stmt = nullptr;
        ok = sqlite3_prepare_v2(db_, insertSql, -1, &stmt, nullptr) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_int64(stmt, 1, row.id);
            sqlite3_bind_int(stmt, 2, badge.position);
            sqlite3_bind_int(stmt, 3, badge.itemId);
            ok = sqlite3_step(stmt) == SQLITE_DONE;
        }
        sqlite3_finalize(stmt);
    }
    if (!ok || !Exec(db_, "COMMIT;")) {
        Exec(db_, "ROLLBACK;");
        return false;
    }
    return true;
}

bool ContentDb::SetWappens(int64_t playerId, const std::vector<WappenEquip>& wappens)
{
    PlayerRow row = PlayerById(playerId);
    if (!row.found)
        return false;
    if (!Exec(db_, "BEGIN IMMEDIATE;"))
        return false;
    sqlite3_stmt* stmt = nullptr;
    bool ok = sqlite3_prepare_v2(db_, "DELETE FROM equipped_wappen WHERE player_id = ?;", -1, &stmt, nullptr) == SQLITE_OK;
    if (ok) {
        sqlite3_bind_int64(stmt, 1, row.id);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
    }
    sqlite3_finalize(stmt);
    const char* insertSql =
        "INSERT INTO equipped_wappen (player_id, item_id, size_type, position_x, position_y) "
        "VALUES (?, ?, ?, ?, ?);";
    for (const WappenEquip& wappen : wappens) {
        if (!ok)
            break;
        if (wappen.itemId <= 0)
            continue;
        stmt = nullptr;
        ok = sqlite3_prepare_v2(db_, insertSql, -1, &stmt, nullptr) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_int64(stmt, 1, row.id);
            sqlite3_bind_int(stmt, 2, wappen.itemId);
            sqlite3_bind_int(stmt, 3, wappen.sizeType);
            sqlite3_bind_int(stmt, 4, wappen.positionX);
            sqlite3_bind_int(stmt, 5, wappen.positionY);
            ok = sqlite3_step(stmt) == SQLITE_DONE;
        }
        sqlite3_finalize(stmt);
    }
    if (!ok || !Exec(db_, "COMMIT;")) {
        Exec(db_, "ROLLBACK;");
        return false;
    }
    return true;
}

bool ContentDb::SetSound(int64_t playerId, const SoundConfig& sound)
{
    PlayerRow row = PlayerById(playerId);
    if (!row.found)
        return false;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "UPDATE player_data SET bgm_volume = ?, driver_volume = ?, conductor_volume = ?, "
        "inside_volume = ?, companion_volume = ? WHERE player_id = ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_double(stmt, 1, sound.bgm);
    sqlite3_bind_double(stmt, 2, sound.driver);
    sqlite3_bind_double(stmt, 3, sound.conductor);
    sqlite3_bind_double(stmt, 4, sound.inside);
    sqlite3_bind_double(stmt, 5, sound.companion);
    sqlite3_bind_int64(stmt, 6, row.id);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

bool ContentDb::SetCustomizeFlag(int64_t playerId, bool changeCustomize)
{
    PlayerRow row = PlayerById(playerId);
    if (!row.found)
        return false;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE player_data SET change_customize = ? WHERE player_id = ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_int(stmt, 1, changeCustomize ? 1 : 0);
    sqlite3_bind_int64(stmt, 2, row.id);
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

void ContentDb::TouchLogin(int64_t playerId)
{
    PlayerRow row = PlayerById(playerId);
    if (!row.found)
        return;
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "UPDATE player_data SET latest_login_at = ? WHERE player_id = ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return;
    const std::string stamp = std::to_string(NowMs());
    sqlite3_bind_text(stmt, 1, stamp.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, row.id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::vector<ShopLine> ContentDb::Shop() const
{
    std::vector<ShopLine> rows;
    const char* sql =
        "SELECT lineup_id, category, item_id, price, status, buyable, name, description, cell_type, max_num "
        "FROM shop_lineup ORDER BY lineup_id;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return rows;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ShopLine line;
        line.lineupId = sqlite3_column_int(stmt, 0);
        line.category = ColumnText(stmt, 1);
        line.itemId = sqlite3_column_int(stmt, 2);
        line.price = sqlite3_column_int(stmt, 3);
        line.status = sqlite3_column_int(stmt, 4);
        line.buyable = sqlite3_column_int(stmt, 5) != 0;
        line.name = ColumnText(stmt, 6);
        line.description = ColumnText(stmt, 7);
        line.cellType = sqlite3_column_int(stmt, 8);
        line.maxNum = sqlite3_column_int(stmt, 9);
        rows.push_back(std::move(line));
    }
    sqlite3_finalize(stmt);
    return rows;
}

std::vector<ItemCount> ContentDb::Owned(int64_t playerId, const char* category) const
{
    std::vector<ItemCount> rows;
    PlayerRow player = const_cast<ContentDb*>(this)->PlayerById(playerId);
    if (!player.found || !category)
        return rows;
    const char* sql =
        "SELECT item_id, num FROM owned_items WHERE player_id = ? AND category = ? ORDER BY item_id;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return rows;
    sqlite3_bind_int64(stmt, 1, player.id);
    sqlite3_bind_text(stmt, 2, category, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        ItemCount item;
        item.itemId = sqlite3_column_int(stmt, 0);
        item.num = sqlite3_column_int(stmt, 1);
        rows.push_back(item);
    }
    sqlite3_finalize(stmt);
    return rows;
}

std::vector<BadgeEquip> ContentDb::Badges(int64_t playerId) const
{
    std::vector<BadgeEquip> rows;
    PlayerRow player = const_cast<ContentDb*>(this)->PlayerById(playerId);
    if (!player.found)
        return rows;
    const char* sql = "SELECT item_id, position FROM equipped_badges WHERE player_id = ? ORDER BY position;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return rows;
    sqlite3_bind_int64(stmt, 1, player.id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        BadgeEquip badge;
        badge.itemId = sqlite3_column_int(stmt, 0);
        badge.position = sqlite3_column_int(stmt, 1);
        rows.push_back(badge);
    }
    sqlite3_finalize(stmt);
    return rows;
}

std::vector<WappenEquip> ContentDb::Wappens(int64_t playerId) const
{
    std::vector<WappenEquip> rows;
    PlayerRow player = const_cast<ContentDb*>(this)->PlayerById(playerId);
    if (!player.found)
        return rows;
    const char* sql =
        "SELECT item_id, size_type, position_x, position_y FROM equipped_wappen WHERE player_id = ? ORDER BY item_id;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return rows;
    sqlite3_bind_int64(stmt, 1, player.id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        WappenEquip wappen;
        wappen.itemId = sqlite3_column_int(stmt, 0);
        wappen.sizeType = sqlite3_column_int(stmt, 1);
        wappen.positionX = sqlite3_column_int(stmt, 2);
        wappen.positionY = sqlite3_column_int(stmt, 3);
        rows.push_back(wappen);
    }
    sqlite3_finalize(stmt);
    return rows;
}

std::vector<TimsItem> ContentDb::Tims(int64_t playerId) const
{
    std::vector<TimsItem> rows;
    PlayerRow player = const_cast<ContentDb*>(this)->PlayerById(playerId);
    if (!player.found)
        return rows;
    const char* sql =
        "SELECT tims_id, train_id, custom_id, name, description FROM player_tims WHERE player_id = ? ORDER BY tims_id;";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return rows;
    sqlite3_bind_int64(stmt, 1, player.id);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        TimsItem item;
        item.id = sqlite3_column_int(stmt, 0);
        item.trainId = sqlite3_column_int(stmt, 1);
        item.customId = sqlite3_column_int(stmt, 2);
        item.name = ColumnText(stmt, 3);
        item.description = ColumnText(stmt, 4);
        rows.push_back(std::move(item));
    }
    sqlite3_finalize(stmt);
    return rows;
}

namespace {

int StoredRankByte(const JsonField& field, int current)
{
    if (!field.present)
        return current;
    const int value = static_cast<int>(field.number) - 1;
    return value > 0 ? value : 0;
}

} // namespace

bool ContentDb::ApplyProgress(int64_t playerId, const std::string& body)
{
    PlayerRow row = PlayerById(playerId);
    if (!row.found || body.empty())
        return false;

    const JsonField playerName = JsonLookup(body, "player_name");
    if (playerName.present && playerName.isString && !playerName.text.empty()) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, "UPDATE players SET name = ? WHERE id = ?;", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, playerName.text.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 2, row.id);
            sqlite3_step(stmt);
        }
        sqlite3_finalize(stmt);
    }

    JsonField mileage = JsonLookup(body, "exp_mileage");
    if (!mileage.present)
        mileage = JsonLookup(body, "mileage");
    if (mileage.present) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, "UPDATE player_data SET mileage = ? WHERE player_id = ?;", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, static_cast<int>(mileage.number));
            sqlite3_bind_int64(stmt, 2, row.id);
            sqlite3_step(stmt);
        }
        sqlite3_finalize(stmt);
    }

    const JsonField meister = JsonLookup(body, "meister_point");
    if (meister.present) {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, "UPDATE player_data SET meister_point = ? WHERE player_id = ?;", -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, static_cast<int>(meister.number));
            sqlite3_bind_int64(stmt, 2, row.id);
            sqlite3_step(stmt);
        }
        sqlite3_finalize(stmt);
    }

    std::string information;
    if (JsonExtract(body, "information", information)) {
        const JsonField infoName = JsonLookup(information, "name");
        if (infoName.present && infoName.isString && !infoName.text.empty()) {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db_, "UPDATE players SET name = ? WHERE id = ?;", -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, infoName.text.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(stmt, 2, row.id);
                sqlite3_step(stmt);
            }
            sqlite3_finalize(stmt);
        }
        const JsonField infoMileage = JsonLookup(information, "mileage");
        if (infoMileage.present) {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db_, "UPDATE player_data SET mileage = ? WHERE player_id = ?;", -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(stmt, 1, static_cast<int>(infoMileage.number));
                sqlite3_bind_int64(stmt, 2, row.id);
                sqlite3_step(stmt);
            }
            sqlite3_finalize(stmt);
        }
        const JsonField registered = JsonLookup(information, "registered_web");
        if (registered.present) {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db_, "UPDATE player_data SET registered_web = ? WHERE player_id = ?;", -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(stmt, 1, JsonBool(registered, true) ? 1 : 0);
                sqlite3_bind_int64(stmt, 2, row.id);
                sqlite3_step(stmt);
            }
            sqlite3_finalize(stmt);
        }
        const JsonField customize = JsonLookup(information, "change_customize");
        if (customize.present)
            SetCustomizeFlag(row.id, JsonBool(customize, false));
    }

    std::string title;
    if (JsonExtract(body, "title", title)) {
        row = LoadPlayer(row.id);
        const int titleId = JsonInt(JsonLookup(title, "id"), row.titleId);
        const int titleType = StoredRankByte(JsonLookup(title, "type"), row.titleType);
        const int titleCategory = JsonInt(JsonLookup(title, "category"), row.titleCategory);
        const int titleRank = StoredRankByte(JsonLookup(title, "rank"), row.titleRank);
        const int titleTrain = StoredRankByte(JsonLookup(title, "train_line"), row.titleTrainLine);
        const int titleYear = JsonInt(JsonLookup(title, "year"), row.titleYear);
        const JsonField titleName = JsonLookup(title, "name");
        const JsonField titleRuby = JsonLookup(title, "ruby");
        const JsonField titleDescription = JsonLookup(title, "description");
        const std::string name = titleName.present && titleName.isString ? titleName.text : row.titleName;
        const std::string ruby = titleRuby.present && titleRuby.isString ? titleRuby.text : row.titleRuby;
        const std::string description = titleDescription.present && titleDescription.isString
            ? titleDescription.text
            : row.titleDescription;
        sqlite3_stmt* stmt = nullptr;
        const char* sql =
            "UPDATE players SET title_id = ?, title_type = ?, title_category = ?, title_rank = ?, "
            "title_train_line = ?, title_year = ?, title_name = ?, title_ruby = ?, title_description = ? "
            "WHERE id = ?;";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, titleId);
            sqlite3_bind_int(stmt, 2, titleType);
            sqlite3_bind_int(stmt, 3, titleCategory);
            sqlite3_bind_int(stmt, 4, titleRank);
            sqlite3_bind_int(stmt, 5, titleTrain);
            sqlite3_bind_int(stmt, 6, titleYear);
            sqlite3_bind_text(stmt, 7, name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 8, ruby.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 9, description.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 10, row.id);
            sqlite3_step(stmt);
        }
        sqlite3_finalize(stmt);
    }

    std::string tims;
    if (JsonExtract(body, "tims", tims)) {
        if (!Exec(db_, "BEGIN IMMEDIATE;"))
            return false;
        sqlite3_stmt* stmt = nullptr;
        bool ok = sqlite3_prepare_v2(db_, "DELETE FROM player_tims WHERE player_id = ?;", -1, &stmt, nullptr) == SQLITE_OK;
        if (ok) {
            sqlite3_bind_int64(stmt, 1, row.id);
            ok = sqlite3_step(stmt) == SQLITE_DONE;
        }
        sqlite3_finalize(stmt);
        const char* insertSql =
            "INSERT INTO player_tims (player_id, tims_id, train_id, custom_id, name, description) "
            "VALUES (?, ?, ?, ?, ?, ?);";
        for (const std::string& element : JsonElements(tims)) {
            if (!ok)
                break;
            const int id = JsonInt(JsonLookup(element, "id"), 0);
            if (id <= 0)
                continue;
            const JsonField name = JsonLookup(element, "name");
            const JsonField description = JsonLookup(element, "description");
            stmt = nullptr;
            ok = sqlite3_prepare_v2(db_, insertSql, -1, &stmt, nullptr) == SQLITE_OK;
            if (ok) {
                sqlite3_bind_int64(stmt, 1, row.id);
                sqlite3_bind_int(stmt, 2, id);
                sqlite3_bind_int(stmt, 3, JsonInt(JsonLookup(element, "train_id"), 0));
                sqlite3_bind_int(stmt, 4, JsonInt(JsonLookup(element, "custom_id"), 0));
                const std::string itemName = name.present && name.isString ? name.text : "";
                const std::string itemDescription = description.present && description.isString ? description.text : "";
                sqlite3_bind_text(stmt, 5, itemName.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 6, itemDescription.c_str(), -1, SQLITE_TRANSIENT);
                ok = sqlite3_step(stmt) == SQLITE_DONE;
            }
            sqlite3_finalize(stmt);
        }
        if (!ok || !Exec(db_, "COMMIT;")) {
            Exec(db_, "ROLLBACK;");
            return false;
        }
    }
    return true;
}

bool ContentDb::SaveCommandBody(int64_t playerId, const std::string& command, const std::string& body)
{
    if (command.empty() || body.empty())
        return false;
    PlayerRow row = PlayerById(playerId);
    if (!row.found)
        return false;
    sqlite3_stmt* stmt = nullptr;
    const char* sql =
        "INSERT INTO command_payloads (player_id, command, body, updated_at) VALUES (?, ?, ?, ?) "
        "ON CONFLICT(player_id, command) DO UPDATE SET body = excluded.body, updated_at = excluded.updated_at;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_int64(stmt, 1, row.id);
    sqlite3_bind_text(stmt, 2, command.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, body.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, NowMs());
    const bool ok = sqlite3_step(stmt) == SQLITE_DONE;
    sqlite3_finalize(stmt);
    return ok;
}

std::string ContentDb::LoadCommandBody(int64_t playerId, const std::string& command)
{
    PlayerRow row = PlayerById(playerId);
    if (!row.found || command.empty())
        return {};
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT body FROM command_payloads WHERE player_id = ? AND command = ?;";
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return {};
    sqlite3_bind_int64(stmt, 1, row.id);
    sqlite3_bind_text(stmt, 2, command.c_str(), -1, SQLITE_TRANSIENT);
    std::string body;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        body = ColumnText(stmt, 0);
    sqlite3_finalize(stmt);
    return body;
}
