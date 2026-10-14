#pragma once

#include <cstdint>
#include <string>
#include <vector>

// One local player row plus the counters the HTTP handlers return.
// title_type, title_rank, and title_train_line are the in-memory bytes.
// The HTTP writer adds one because the game stores (json - 1) for those three.
struct PlayerRow {
    int64_t id = 0;
    std::string name;
    std::string titleName;
    std::string titleRuby;
    std::string titleDescription;
    int titleId = 1;
    int titleType = 0;
    int titleCategory = 0;
    int titleRank = 0;
    int titleTrainLine = 0;
    int titleYear = 2019;
    int credit = 0;
    int serviceCredit = 0;
    int webPoint = 0;
    int totalWebPoint = 0;
    int meisterPoint = 0;
    int mileage = 0;
    int64_t playlogId = 0;
    std::string configJson;
    double bgmVolume = 1;
    double driverVolume = 1;
    double conductorVolume = 1;
    double insideVolume = 1;
    double companionVolume = 1;
    bool changeCustomize = false;
    bool registeredWeb = true;
    std::string latestLoginAt = "0";
    bool found = false;
};

// One shopinitialize entry. The object key is lineupId. The nested id is itemId.
struct ShopLine {
    int lineupId = 0;
    std::string category;
    int itemId = 0;
    int price = 0;
    int status = 0;
    bool buyable = false;
    std::string name;
    std::string description;
    int cellType = 0;
    int maxNum = 1;
};

struct ItemCount {
    int itemId = 0;
    int num = 0;
};

struct BadgeEquip {
    int itemId = 0;
    int position = 0;
};

struct WappenEquip {
    int itemId = 0;
    int sizeType = 1;
    int positionX = 0;
    int positionY = 0;
};

struct TimsItem {
    int id = 0;
    int trainId = 0;
    int customId = 0;
    std::string name;
    std::string description;
};

struct SoundConfig {
    double bgm = 1;
    double driver = 1;
    double conductor = 1;
    double inside = 1;
    double companion = 1;
};

// Wire-facing leaderboard row. `rank` is the 1-based place stored in SQLite.
// The HTTP writer subtracts one because the game stores json.rank + 1.
struct RankRow {
    int rank = 1;
    int64_t playerId = 0;
    std::string name;
    std::string titleName;
    std::string titleRuby;
    int titleType = 0;
    int titleCategory = 0;
    int titleRarity = 0;
    int titleTrainLine = 0;
    int titleYear = 2019;
    int mode = 0;
    int value = 0;
    std::string storeName;
    int64_t startAt = 0;
    int64_t endAt = 0;
    int64_t updateAt = 0;
};

// Owns the SQLite connection. The DLL never opens this file.
class ContentDb {
public:
    ContentDb() = default;
    ~ContentDb();

    ContentDb(const ContentDb&) = delete;
    ContentDb& operator=(const ContentDb&) = delete;

    bool Open(const std::wstring& path);

    PlayerRow PlayerById(int64_t id);
    PlayerRow DefaultPlayer();
    // create == false looks up an existing card. An empty code returns the
    // seeded card. create == true inserts a card and player when missing.
    PlayerRow SelectCard(const std::string& code, bool create);
    PlayerRow AddCredit(int64_t playerId, int creditDelta, int serviceDelta, const std::string& card);
    PlayerRow GameStart(int64_t playerId);
    // purchasepoint adds web_point. The request carries the delta, not a new total.
    PlayerRow PurchasePoint(int64_t playerId, int webPointDelta);
    // purchaseshopitem spends meister_point (price * num) and equips the item;
    // the game reads the new meister_point back (sub_140213690).
    // bought is false when the lineup is missing or the balance is short.
    PlayerRow PurchaseItem(int64_t playerId, int lineupId, int num, bool& bought);
    // Raises meister_point to `minimum` when it is lower.
    bool TopUpMeisterPoint(int64_t playerId, int minimum);
    // replaceBadges clears the equipped badge rows before inserting `badges`.
    // resetWappen clears equipped wappen. playerbadge sends both.
    bool SetBadges(int64_t playerId, const std::vector<BadgeEquip>& badges, bool replaceBadges, bool resetWappen);
    bool SetWappens(int64_t playerId, const std::vector<WappenEquip>& wappens);
    bool SetSound(int64_t playerId, const SoundConfig& sound);
    bool SetCustomizeFlag(int64_t playerId, bool changeCustomize);
    void TouchLogin(int64_t playerId);
    // Copies documented player fields that are present in a request body:
    // name, title, tims, mileage / exp_mileage, meister_point, information.
    bool ApplyProgress(int64_t playerId, const std::string& body);
    // Raw JSON for a command whose response field list is not recovered.
    bool SaveCommandBody(int64_t playerId, const std::string& command, const std::string& body);
    std::string LoadCommandBody(int64_t playerId, const std::string& command);
    std::vector<ShopLine> Shop() const;
    std::vector<ItemCount> Owned(int64_t playerId, const char* category) const;
    std::vector<BadgeEquip> Badges(int64_t playerId) const;
    std::vector<WappenEquip> Wappens(int64_t playerId) const;
    std::vector<TimsItem> Tims(int64_t playerId) const;
    std::vector<RankRow> Leaderboard(const std::string& board, int offset, int count);

private:
    struct sqlite3* db_ = nullptr;

    bool EnsureSchema();
    bool SeedIfEmpty();
    bool EnsureContent();
    bool EnsureShop();
    bool AddColumn(const char* table, const char* column, const char* decl);
    PlayerRow LoadPlayer(int64_t id);
    bool LoadLineup(int lineupId, ShopLine& line) const;
};
