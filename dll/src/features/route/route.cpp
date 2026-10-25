#include "features/module_api.h"

#include <MinHook.h>

#include "log.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>

// Keeps every streaming level of the line in play loaded and visible.
//
// A mission asks ALoadLevelManager for the current station plus its previous
// and next neighbors, then drops whatever left that set. Three paths do that:
//   LevelRelease (sub_1401FDBC0) is the only caller of the manager's map drop
//   (sub_140201DA0); LevelReleaseStart (sub_1401FDE30) queues the same names
//   for the serial release worker.
//   UGameplayStatics::UnloadStreamLevel (sub_1412DC5B0, exec sub_1417ABF80)
//   unloads a streaming level from the blueprint.
//   UGameplayStatics::LoadStreamLevel (sub_1412D0AC0, exec sub_14177B050)
//   with bMakeVisibleAfterLoad false keeps a preloaded neighbor hidden.
// LevelLoadStart (sub_1401FDA20) is the queue entry.
//
// The old Yamanote missions (HRJ, SBY, EBS, and the rest) pass those short
// station codes to LevelLoadStart before UWorld::StreamingLevels can be read.
// The first such code queues the old-line packages selected by [Route] Load.
// The YMTc loop still waits until its world lists streaming levels, then the
// same selection is loaded and shown and the matching YMTc packages are queued.
// Load=all keeps every streaming level. Load=scenery keeps buildings, ground,
// rails, facilities, and track sections, and leaves plants, crowds, gimmicks,
// cameras, stages, and volumes with the mission. From then on a selected
// release is refused, an unload of a selected level is redirected to
// NAME_None (its latent action finishes at once and nothing unloads), and a
// hidden preload of a selected level is made visible. Weather and section
// lighting levels ("Lighting" in the name) stay under the game's control so
// only the mission's weather is shown. Loading OutGame resets this for the
// next mission.
// Image base in IDA is 0x140000000.

namespace {

constexpr unsigned int kLevelLoadStartRva = 0x1FDA20;    // sub_1401FDA20
constexpr unsigned int kLevelReleaseRva = 0x1FDBC0;      // sub_1401FDBC0
constexpr unsigned int kLevelReleaseStartRva = 0x1FDE30; // sub_1401FDE30
constexpr unsigned int kLoadStreamRva = 0x12D0AC0;       // sub_1412D0AC0 UGameplayStatics::LoadStreamLevel
constexpr unsigned int kUnloadStreamRva = 0x12DC5B0;     // sub_1412DC5B0 UGameplayStatics::UnloadStreamLevel
constexpr unsigned int kMakeNameRva = 0x52EC80;          // sub_14052EC80
constexpr unsigned int kNameToStringRva = 0x533220;      // sub_140533220
constexpr unsigned int kWideFreeRva = 0x489F70;          // sub_140489F70

constexpr int kStreamingData = 136;
constexpr int kStreamingCount = 144;
constexpr int kLevelNameOffset = 80;
constexpr int kStreamFlags = 0xB0;
constexpr unsigned int kLoadAndShow = 0x60;
constexpr unsigned int kGroupStation = 1u << 0;
constexpr unsigned int kGroupFacility = 1u << 1;
constexpr unsigned int kGroupGround = 1u << 2;
constexpr unsigned int kGroupLine = 1u << 3;
constexpr unsigned int kGroupRail = 1u << 4;
constexpr unsigned int kGroupSection = 1u << 5;
constexpr unsigned int kGroupPlant = 1u << 6;
constexpr unsigned int kGroupLife = 1u << 7;
constexpr unsigned int kGroupGimmick = 1u << 8;
constexpr unsigned int kLoadScenery =
    kGroupStation | kGroupFacility | kGroupGround | kGroupLine | kGroupRail | kGroupSection;
constexpr unsigned int kLoadAll = ~0u;
constexpr unsigned int kGWorldRva = 0x2FFDDC8;
constexpr int kMaxStreaming = 512;

constexpr int kFindNameAdd = 1;
constexpr std::uint64_t kNameNone = 0;

const unsigned char kLoadPrologue[] = {0x48, 0x83, 0xEC, 0x38, 0xC6, 0x44, 0x24, 0x28};
const unsigned char kReleasePrologue[] = {0x48, 0x83, 0xEC, 0x38, 0x8B, 0x81, 0x90, 0x03};
const unsigned char kReleaseStartPrologue[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x99};
const unsigned char kLoadStreamPrologue[] = {0x48, 0x89, 0x54, 0x24, 0x10, 0x53, 0x55, 0x41};
const unsigned char kUnloadStreamPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x54};

// UE 4.15 FName. ComparisonIndex 0 and Number 0 is NAME_None.
struct FName {
    unsigned int comparisonIndex;
    unsigned int number;
};

// Engine wide string. count includes the trailing NUL when the buffer is live.
struct WideStr {
    wchar_t* data;
    int count;
    int capacity;
};

using LevelLoadStartFn = std::int64_t(__fastcall*)(void* manager, FName* name, int prevFlag);
using LevelReleaseFn = std::int64_t(__fastcall*)(void* manager, FName* name);
using LevelReleaseStartFn = bool(__fastcall*)(void* manager, FName* name);
using LoadStreamFn = std::int64_t(__fastcall*)(
    void* context, std::uint64_t name, std::uint64_t makeVisible, unsigned char blockOnLoad, void* latent);
using UnloadStreamFn = std::int64_t(__fastcall*)(void* context, std::uint64_t name, void* latent);
using MakeNameFn = FName*(__fastcall*)(FName* outName, const wchar_t* text, int findType);
using NameToStringFn = WideStr*(__fastcall*)(const FName* name, WideStr* outText);
using WideFreeFn = void(__fastcall*)(void* ptr);

// Empty latent info. Linkage and UUID -1 mean "no blueprint callback".
struct LatentAction {
    int linkage;
    int uuid;
    std::uint64_t function;
    void* target;
};

#include "route_packages.inc"
#include "route_ymt_packages.inc"

LevelLoadStartFn g_originalLoad = nullptr;
LevelReleaseFn g_originalRelease = nullptr;
LevelReleaseStartFn g_originalReleaseStart = nullptr;
LoadStreamFn g_originalLoadStream = nullptr;
UnloadStreamFn g_originalUnloadStream = nullptr;
MakeNameFn g_makeName = nullptr;
NameToStringFn g_nameToString = nullptr;
WideFreeFn g_freeWide = nullptr;
unsigned char* g_game = nullptr;
unsigned char* g_gameEnd = nullptr;
void* g_manager = nullptr;

// 0 = not expanded, 1 = expanding, 2 = the line is loaded and kept.
std::atomic<int> g_expandState{0};
std::atomic<int> g_oldLine{0};
unsigned int g_loadMask = kLoadScenery;
char g_loadLabel[96] = "scenery";
std::atomic<int> g_loggedRelease{0};
std::atomic<int> g_loggedUnload{0};
std::atomic<int> g_loggedVisible{0};
std::atomic<int> g_loggedNames{0};
std::atomic<int> g_loggedMiss{0};
std::uint64_t g_shownNames[kMaxStreaming]{};
int g_shownCount = 0;
int g_listLogged = 0;
int g_emptyLogged = 0;

bool HasPrefix(const unsigned char* bytes, const unsigned char* prefix, std::size_t count)
{
    return std::memcmp(bytes, prefix, count) == 0;
}

void LogWide(const char* prefix, const wchar_t* text)
{
    char narrow[512];
    std::size_t i = 0;
    if (text != nullptr)
    {
        for (; text[i] != L'\0' && i + 1 < sizeof(narrow); ++i)
            narrow[i] = text[i] < 128 ? static_cast<char>(text[i]) : '?';
    }
    narrow[i] = '\0';
    LOG("%s %s", prefix, narrow);
}

std::wstring NameText(const FName* name)
{
    if (name == nullptr || g_nameToString == nullptr)
        return {};
    if (name->comparisonIndex == 0 && name->number == 0)
        return {};
    WideStr text{};
    g_nameToString(name, &text);
    std::wstring out = text.data != nullptr && text.count > 0 ? std::wstring(text.data) : std::wstring();
    if (text.data != nullptr && g_freeWide != nullptr)
        g_freeWide(text.data);
    return out;
}

std::wstring NameText(std::uint64_t bits)
{
    FName name{};
    std::memcpy(&name, &bits, sizeof(name));
    return NameText(&name);
}

bool ContainsNoCase(const std::wstring& text, const wchar_t* part)
{
    const std::size_t length = wcslen(part);
    if (length == 0 || text.size() < length)
        return false;
    for (std::size_t i = 0; i + length <= text.size(); ++i)
    {
        if (_wcsnicmp(text.c_str() + i, part, length) == 0)
            return true;
    }
    return false;
}

const wchar_t* Leaf(const wchar_t* text)
{
    const wchar_t* leaf = text;
    if (text == nullptr)
        return L"";
    for (const wchar_t* cursor = text; *cursor != L'\0'; ++cursor)
    {
        if (*cursor == L'/' || *cursor == L'\\')
            leaf = cursor + 1;
    }
    return leaf;
}

bool LeafIsYamanotePackage(const wchar_t* text)
{
    const wchar_t* leaf = Leaf(text);
    if (leaf[0] == L'\0')
        return false;
    // The persistent world itself is not in the queue, but its short name is.
    if (wcscmp(leaf, L"YMTc") == 0)
        return true;
    for (int i = 0; i < kPackageCount; ++i)
    {
        if (wcscmp(leaf, Leaf(kPackages[i])) == 0)
            return true;
    }
    return false;
}

bool LeafIsOldYamanotePackage(const wchar_t* text)
{
    const wchar_t* leaf = Leaf(text);
    if (leaf[0] == L'\0')
        return false;
    for (int i = 0; i < kOldPackageCount; ++i)
    {
        if (wcscmp(leaf, Leaf(kOldPackages[i])) == 0)
            return true;
    }
    return false;
}

// The 5.80 missions that still drive the partial YMT slice pass a short
// station code (HRJ, SBY) or a /Maps/InGame/YMT/ path. YMTc contains the
// letters YMT, so that loop is excluded here.
bool TextIsOldYamanote(const std::wstring& text)
{
    if (text.empty() || text.find(L"YMTc") != std::wstring::npos)
        return false;
    if (text.find(L"/Maps/InGame/YMT/") != std::wstring::npos)
        return true;
    return LeafIsOldYamanotePackage(text.c_str());
}

// True for a YMTc path or a short station code such as SINc. The older /YMT/
// slice is a different line and is not matched here.
bool TextIsYamanote(const std::wstring& text)
{
    if (text.empty() || TextIsOldYamanote(text))
        return false;
    if (text.find(L"YMTc") != std::wstring::npos)
        return true;
    return LeafIsYamanotePackage(text.c_str());
}

// A driving world lists its stations and sections under /Maps/InGame/.
bool TextIsInGame(const std::wstring& text)
{
    return ContainsNoCase(text, L"/Maps/InGame/") || TextIsYamanote(text) || TextIsOldYamanote(text);
}

bool TextIsLighting(const std::wstring& text)
{
    return ContainsNoCase(text, L"Lighting");
}

bool TextIsOutGame(const std::wstring& text)
{
    return ContainsNoCase(text, L"OutGame");
}

bool TextIsSelectedDetail(const std::wstring& text);

// Names the route keeps loaded and visible once the line is expanded.
// Load=all keeps every non-lighting name. A narrower Load keeps only the
// groups named in config, so plants and crowds can still unload.
bool ShouldKeep(const std::wstring& text)
{
    if (g_expandState.load(std::memory_order_acquire) != 2 || text.empty() || TextIsLighting(text)
        || TextIsOutGame(text))
        return false;
    if (g_oldLine.load(std::memory_order_acquire) == 1 && g_loadMask != kLoadAll)
        return TextIsSelectedDetail(text);
    return true;
}

void LogSeenName(const char* prefix, const std::wstring& text)
{
    if (g_loggedNames.fetch_add(1, std::memory_order_relaxed) < 16)
        LogWide(prefix, text.c_str());
}

void QueueYamanotePackages(void* manager)
{
    if (manager == nullptr || g_originalLoad == nullptr || g_makeName == nullptr)
        return;

    int queued = 0;
    for (int i = 0; i < kPackageCount; ++i)
    {
        if (g_loadMask != kLoadAll && !TextIsSelectedDetail(kPackages[i]))
            continue;
        FName made{};
        g_makeName(&made, kPackages[i], kFindNameAdd);
        if (made.comparisonIndex == 0 && made.number == 0)
        {
            LogWide("Route: name failed", kPackages[i]);
            continue;
        }
        // prevFlag 0 keeps the package in the current set. The release hooks
        // below refuse to drop it when the mission moves on.
        g_originalLoad(manager, &made, 0);
        ++queued;
    }
    LOG("Route: queued %d Yamanote packages", queued);
}

// LevelLoadStart on this line is given the short asset name (HRJ, HRJ_Stage),
// which is the form the mission itself uses. Full paths are what LoadStreamLevel
// receives, and that list was still empty when these missions loaded.
void QueueOldYamanote(void* manager)
{
    if (manager == nullptr || g_originalLoad == nullptr || g_makeName == nullptr)
        return;

    int queued = 0;
    for (int i = 0; i < kOldPackageCount; ++i)
    {
        if (!TextIsSelectedDetail(kOldPackages[i]))
            continue;
        const wchar_t* leaf = Leaf(kOldPackages[i]);
        FName made{};
        g_makeName(&made, leaf, kFindNameAdd);
        if (made.comparisonIndex == 0 && made.number == 0)
        {
            LogWide("Route: name failed", leaf);
            continue;
        }
        g_originalLoad(manager, &made, 0);
        // The station code loads the shell. The same package path is what the
        // streaming level is registered under, so queue that too.
        if (std::wcscmp(leaf, kOldPackages[i]) != 0)
        {
            FName pathName{};
            g_makeName(&pathName, kOldPackages[i], kFindNameAdd);
            if (pathName.comparisonIndex != 0 || pathName.number != 0)
                g_originalLoad(manager, &pathName, 0);
        }
        ++queued;
    }
    LOG("Route: queued %d old Yamanote packages", queued);
}

// Marks the line kept before any package is queued, so a nested release cannot
// drop a station while the queue is still running. Returns false if another
// caller already expanded this mission.
bool ClaimOldYamanote(void* manager)
{
    if (manager == nullptr)
        return false;
    int expected = 0;
    if (!g_expandState.compare_exchange_strong(expected, 2, std::memory_order_acq_rel))
        return false;
    g_oldLine.store(1, std::memory_order_release);
    QueueOldYamanote(manager);
    return true;
}

bool PointerInGame(const void* pointer)
{
    auto* bytes = static_cast<const unsigned char*>(pointer);
    return g_game != nullptr && bytes >= g_game && bytes < g_gameEnd;
}

struct NameList {
    std::uint64_t names[kMaxStreaming];
    int count;
};

int CopyStreamingNames(void* owner, NameList* out)
{
    out->count = 0;
    if (owner == nullptr)
        return 0;
    __try
    {
        auto* data = *reinterpret_cast<unsigned char**>(static_cast<unsigned char*>(owner) + kStreamingData);
        const int count = *reinterpret_cast<int*>(static_cast<unsigned char*>(owner) + kStreamingCount);
        if (data == nullptr || count < 1 || count > 2000)
            return 0;
        const int limit = count < kMaxStreaming ? count : kMaxStreaming;
        int kept = 0;
        for (int i = 0; i < limit; ++i)
        {
            auto* level = *reinterpret_cast<unsigned char**>(data + static_cast<std::size_t>(i) * sizeof(void*));
            if (level == nullptr)
                continue;
            auto** vtable = *reinterpret_cast<void***>(level);
            if (!PointerInGame(vtable))
                continue;
            std::uint64_t bits = 0;
            std::memcpy(&bits, level + kLevelNameOffset, sizeof(bits));
            if (bits == 0)
                continue;
            out->names[kept++] = bits;
        }
        out->count = kept;
        return kept;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        out->count = 0;
        return 0;
    }
}

void* WorldOf(void* object)
{
    if (object == nullptr)
        return nullptr;
    __try
    {
        auto** vtable = *reinterpret_cast<void***>(object);
        if (!PointerInGame(vtable))
            return nullptr;
        auto* fn = reinterpret_cast<void*(__fastcall*)(void*)>(vtable[36]);
        if (!PointerInGame(reinterpret_cast<void*>(fn)))
            return nullptr;
        return fn(object);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

struct ListKind {
    bool inGame;
    bool yamanote;
};

ListKind ClassifyList(const NameList& list)
{
    ListKind kind{false, false};
    for (int i = 0; i < list.count && !kind.yamanote; ++i)
    {
        const std::wstring text = NameText(list.names[i]);
        kind.inGame = kind.inGame || TextIsInGame(text);
        kind.yamanote = kind.yamanote || TextIsYamanote(text);
    }
    return kind;
}

// The older partial /YMT/ slice (InGame.umap) overlaps the YMTc loop, so it is
// not forced in when the YMTc packages are queued.
void ShowListed(void* owner, const NameList& list, bool skipOldYamanote)
{
    LatentAction latent{-1, -1, 0, nullptr};
    int shown = 0;
    int skipped = 0;
    for (int i = 0; i < list.count; ++i)
    {
        const std::wstring text = NameText(list.names[i]);
        const bool filtered = g_loadMask != kLoadAll && !TextIsSelectedDetail(text);
        if (TextIsLighting(text) || filtered || (skipOldYamanote && text.find(L"/YMT/") != std::wstring::npos))
        {
            ++skipped;
            continue;
        }
        g_originalLoadStream(owner, list.names[i], 1, 1, &latent);
        ++shown;
    }
    LOG("Route: showed %d streaming levels, left %d lighting or old-slice levels to the mission", shown, skipped);
}

// Loads and shows every level of the context object's world. Returns false
// when that world is not a driving world yet. Callers run on the game thread.
bool TryExpand(void* context)
{
    if (g_expandState.load(std::memory_order_acquire) != 0)
        return false;

    // UObject::GetWorld (vtable slot 36) gives the world for the manager, a
    // blueprint actor, or the world itself. Only UWorld has StreamingLevels.
    void* world = WorldOf(context);
    static NameList list{};
    if (world == nullptr || CopyStreamingNames(world, &list) == 0)
        return false;
    const ListKind kind = ClassifyList(list);
    if (!kind.inGame)
        return false;

    int expected = 0;
    if (!g_expandState.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
        return false;

    const bool queueYamanote = kind.yamanote && g_manager != nullptr;
    if (queueYamanote)
        QueueYamanotePackages(g_manager);
    ShowListed(world, list, queueYamanote);

    g_expandState.store(2, std::memory_order_release);
    LOG("Route: whole line requested (%d streaming names%s)", list.count, kind.yamanote ? ", Yamanote" : "");
    return true;
}

bool TextIsCoarse(const std::wstring& text)
{
    return TextIsLighting(text) || ContainsNoCase(text, L"RefCap") || ContainsNoCase(text, L"LS_")
        || ContainsNoCase(text, L"Snow") || ContainsNoCase(text, L"Rain");
}

bool LeafEnds(const wchar_t* leaf, const wchar_t* suffix)
{
    const std::size_t name = leaf == nullptr ? 0 : wcslen(leaf);
    const std::size_t tail = wcslen(suffix);
    if (name < tail)
        return false;
    return _wcsnicmp(leaf + (name - tail), suffix, tail) == 0;
}

// 0 means the name is not one of the selectable groups (camera, stage, volume,
// weather). Load=all still takes those through TextIsSelectedDetail.
unsigned int GroupOf(const std::wstring& text)
{
    const wchar_t* leaf = Leaf(text.c_str());
    if (ContainsNoCase(text, L"Enlivening"))
        return kGroupLife;
    if (ContainsNoCase(text, L"Gimmick"))
        return kGroupGimmick;
    if (ContainsNoCase(text, L"Camera") || ContainsNoCase(text, L"Stage") || ContainsNoCase(text, L"Volume")
        || ContainsNoCase(text, L"Snow") || ContainsNoCase(text, L"StaticLight") || ContainsNoCase(text, L"Test")
        || ContainsNoCase(text, L"HLOD") || ContainsNoCase(text, L"RefCap") || ContainsNoCase(text, L"Lighting"))
        return 0;
    if (ContainsNoCase(text, L"/Plant/") || LeafEnds(leaf, L"_P"))
        return kGroupPlant;
    if (ContainsNoCase(text, L"/Rail/") || LeafEnds(leaf, L"_R"))
        return kGroupRail;
    if (ContainsNoCase(text, L"/Ground/") || LeafEnds(leaf, L"_G"))
        return kGroupGround;
    if (ContainsNoCase(text, L"/Facility/") || LeafEnds(leaf, L"_F"))
        return kGroupFacility;
    if (ContainsNoCase(text, L"/Line/") || LeafEnds(leaf, L"_L"))
        return kGroupLine;
    if (ContainsNoCase(text, L"/Building/") || ContainsNoCase(text, L"/Station/") || LeafEnds(leaf, L"_S"))
        return kGroupStation;
    if (LeafEnds(leaf, L"_A") || LeafEnds(leaf, L"_B") || LeafEnds(leaf, L"_C") || wcschr(leaf, L'_') == nullptr)
        return kGroupSection;
    return 0;
}

bool TextIsSelectedDetail(const std::wstring& text)
{
    if (text.empty() || TextIsCoarse(text))
        return false;
    if (g_loadMask == kLoadAll)
        return true;
    const unsigned int group = GroupOf(text);
    return group != 0 && (g_loadMask & group) != 0;
}

void* ReadGWorld()
{
    if (g_game == nullptr)
        return nullptr;
    __try
    {
        return *reinterpret_cast<void**>(g_game + kGWorldRva);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

struct LevelRef
{
    unsigned char* level;
    std::uint64_t name;
};

int CollectStreaming(void* world, LevelRef* out, int cap, int* listed)
{
    *listed = 0;
    if (world == nullptr || out == nullptr || cap < 1)
        return 0;
    __try
    {
        auto* data = *reinterpret_cast<unsigned char**>(static_cast<unsigned char*>(world) + kStreamingData);
        const int count = *reinterpret_cast<int*>(static_cast<unsigned char*>(world) + kStreamingCount);
        if (data == nullptr || count < 1 || count > 2000)
            return 0;
        *listed = count;
        const int limit = count < cap ? count : cap;
        int kept = 0;
        for (int i = 0; i < limit; ++i)
        {
            auto* level = *reinterpret_cast<unsigned char**>(data + static_cast<std::size_t>(i) * sizeof(void*));
            if (level == nullptr)
                continue;
            auto** vtable = *reinterpret_cast<void***>(level);
            if (!PointerInGame(vtable))
                continue;
            std::uint64_t bits = 0;
            std::memcpy(&bits, level + kLevelNameOffset, sizeof(bits));
            out[kept].level = level;
            out[kept].name = bits;
            ++kept;
        }
        return kept;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

bool MarkLoadedAndVisible(unsigned char* level)
{
    __try
    {
        auto* flags = reinterpret_cast<unsigned int*>(level + kStreamFlags);
        const unsigned int next = (*flags | kLoadAndShow) & ~0x100u;
        if (*flags == next)
            return false;
        *flags = next;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// bShouldBeLoaded is bit 5 and bShouldBeVisible is bit 6 of ULevelStreaming+0xB0.
// The distant building shell is the HLOD of a sublevel whose bits are still clear.
// The rail mesh sits in the station level that LevelLoadStart already queued.
int ForceListedDetail(void* world, int* listed)
{
    static LevelRef levels[kMaxStreaming];
    const int kept = CollectStreaming(world, levels, kMaxStreaming, listed);
    int forced = 0;
    for (int i = 0; i < kept; ++i)
    {
        if (!TextIsSelectedDetail(NameText(levels[i].name)))
            continue;
        if (MarkLoadedAndVisible(levels[i].level))
            ++forced;
    }
    return forced;
}

// The manager's GetWorld did not list these sublevels. GWorld does.
void ShowManagerStreaming(void* streamContext)
{
    if (g_oldLine.load(std::memory_order_acquire) != 1)
        return;

    void* world = ReadGWorld();
    int listed = 0;
    int forced = ForceListedDetail(world, &listed);
    if (listed == 0)
    {
        void* other = WorldOf(g_manager);
        if (other == nullptr)
            other = WorldOf(streamContext);
        forced = ForceListedDetail(other, &listed);
    }
    if (listed == 0)
    {
        if (g_emptyLogged == 0)
        {
            g_emptyLogged = 1;
            LOG("Route: streaming list is empty");
        }
        return;
    }
    if (g_listLogged == 0)
    {
        g_listLogged = 1;
        LOG("Route: streaming list has %d names, forced %d selected levels to load", listed, forced);
    }
    else if (forced > 0)
    {
        LOG("Route: forced %d more streaming levels (%d listed)", forced, listed);
    }
}

void ResetForOutGame(const std::wstring& text)
{
    if (!TextIsOutGame(text))
        return;
    int expanded = 2;
    if (g_expandState.compare_exchange_strong(expanded, 0, std::memory_order_acq_rel))
    {
        g_loggedMiss.store(0, std::memory_order_relaxed);
        g_oldLine.store(0, std::memory_order_relaxed);
        g_shownCount = 0;
        g_listLogged = 0;
        g_emptyLogged = 0;
        LOG("Route: OutGame loading, the next mission expands its own line");
    }
}

std::int64_t __fastcall LoadStreamDetour(
    void* context, std::uint64_t name, std::uint64_t makeVisible, unsigned char blockOnLoad, void* latent)
{
    const std::wstring text = NameText(name);
    if ((makeVisible & 0xFF) == 0 && ShouldKeep(text))
    {
        if (g_loggedVisible.exchange(1, std::memory_order_acq_rel) == 0)
            LogWide("Route: preloaded level kept visible:", text.c_str());
        makeVisible = 1;
    }
    const std::int64_t result = g_originalLoadStream(context, name, makeVisible, blockOnLoad, latent);
    if (g_oldLine.load(std::memory_order_acquire) == 1)
        ShowManagerStreaming(context);
    if (g_expandState.load(std::memory_order_acquire) != 0 || !TextIsInGame(text))
        return result;

    LogSeenName("Route: LoadStreamLevel", text);
    if (!TryExpand(context) && g_loggedMiss.exchange(1, std::memory_order_acq_rel) == 0)
        LOG("Route: InGame level shown, streaming list not ready");
    return result;
}

std::int64_t __fastcall UnloadStreamDetour(void* context, std::uint64_t name, void* latent)
{
    const std::wstring text = NameText(name);
    if (!ShouldKeep(text))
        return g_originalUnloadStream(context, name, latent);
    if (g_loggedUnload.exchange(1, std::memory_order_acq_rel) == 0)
        LogWide("Route: UnloadStreamLevel kept", text.c_str());
    // NAME_None finds no streaming level, so the latent action completes on
    // its next update and the blueprint continues without unloading.
    return g_originalUnloadStream(context, kNameNone, latent);
}

std::int64_t __fastcall LevelLoadStartDetour(void* manager, FName* name, int prevFlag)
{
    if (manager != nullptr)
        g_manager = manager;
    const std::wstring text = NameText(name);
    LogSeenName("Route: LevelLoadStart", text);
    ResetForOutGame(text);
    const std::int64_t result = g_originalLoad(manager, name, prevFlag);
    if (g_expandState.load(std::memory_order_acquire) != 0)
        return result;

    // Old Yamanote never fills StreamingLevels before this call. Queue from the
    // station code itself; the world-list path below stays for YMTc and others.
    if (TextIsOldYamanote(text))
    {
        if (ClaimOldYamanote(manager))
            LogWide("Route: old Yamanote line requested from", text.c_str());
        return result;
    }

    if (!TryExpand(manager) && TextIsInGame(text) && g_loggedMiss.exchange(1, std::memory_order_acq_rel) == 0)
        LOG("Route: InGame name seen, streaming list not ready");
    return result;
}

std::int64_t __fastcall LevelReleaseDetour(void* manager, FName* name)
{
    if (name != nullptr && ShouldKeep(NameText(name)))
    {
        if (g_loggedRelease.exchange(1, std::memory_order_acq_rel) == 0)
            LOG("Route: LevelRelease ignored while the line is kept");
        // Same value LevelRelease returns when the name is not in the drop map.
        std::uint64_t bits = 0;
        std::memcpy(&bits, name, sizeof(bits));
        return static_cast<std::int64_t>(bits);
    }
    return g_originalRelease(manager, name);
}

bool __fastcall LevelReleaseStartDetour(void* manager, FName* name)
{
    if (name != nullptr && ShouldKeep(NameText(name)))
    {
        if (g_loggedRelease.exchange(1, std::memory_order_acq_rel) == 0)
            LOG("Route: LevelRelease ignored while the line is kept");
        // Tell the blueprint the release slot was free, without enqueueing.
        return true;
    }
    return g_originalReleaseStart(manager, name);
}

std::string LowerAscii(std::string text)
{
    for (char& c : text)
    {
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

unsigned int MaskOfLoadToken(const std::string& token)
{
    if (token == "all" || token == "full")
        return kLoadAll;
    if (token == "scenery" || token == "scene")
        return kLoadScenery;
    if (token == "station" || token == "building" || token == "buildings")
        return kGroupStation;
    if (token == "facility")
        return kGroupFacility;
    if (token == "ground")
        return kGroupGround;
    if (token == "line")
        return kGroupLine;
    if (token == "rail" || token == "rails")
        return kGroupRail;
    if (token == "section" || token == "sections")
        return kGroupSection;
    if (token == "plant" || token == "plants" || token == "tree" || token == "trees")
        return kGroupPlant;
    if (token == "life" || token == "crowd" || token == "crowds" || token == "enlivening")
        return kGroupLife;
    if (token == "gimmick" || token == "gimmicks")
        return kGroupGimmick;
    return 0;
}

void ApplyLoadValue(const std::string& value)
{
    unsigned int mask = 0;
    bool any = false;
    bool unknown = false;
    std::size_t i = 0;
    while (i < value.size())
    {
        while (i < value.size() && (value[i] == ' ' || value[i] == '\t' || value[i] == ',' || value[i] == '|'))
            ++i;
        std::size_t end = i;
        while (end < value.size() && value[end] != ',' && value[end] != '|' && value[end] != ' ' && value[end] != '\t')
            ++end;
        if (end == i)
            break;
        const std::string token = LowerAscii(value.substr(i, end - i));
        i = end;
        const unsigned int bit = MaskOfLoadToken(token);
        if (bit == 0)
        {
            unknown = true;
            LOG("WARN: Route Load token \"%s\" is not a group", token.c_str());
            continue;
        }
        any = true;
        if (bit == kLoadAll)
        {
            mask = kLoadAll;
            break;
        }
        mask |= bit;
    }
    if (!any)
    {
        g_loadMask = kLoadScenery;
        std::snprintf(g_loadLabel, sizeof(g_loadLabel), "scenery");
        if (unknown)
            LOG("WARN: Route Load fell back to scenery");
        return;
    }
    g_loadMask = mask;
    if (mask == kLoadAll)
        std::snprintf(g_loadLabel, sizeof(g_loadLabel), "all");
    else if (mask == kLoadScenery)
        std::snprintf(g_loadLabel, sizeof(g_loadLabel), "scenery");
    else
        std::snprintf(g_loadLabel, sizeof(g_loadLabel), "custom");
}

bool ReadRouteEnabled(const wchar_t* moduleDir)
{
    if (moduleDir == nullptr || moduleDir[0] == L'\0')
        return false;
    std::wstring path = moduleDir;
    path += L"\\config.ini";
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr)
        return false;

    std::string bytes;
    char buf[512];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), file)) > 0)
        bytes.append(buf, n);
    std::fclose(file);
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF)
        bytes.erase(0, 3);

    bool inRoute = false;
    bool enabled = false;
    bool sawEnabled = false;
    std::size_t i = 0;
    while (i < bytes.size())
    {
        std::size_t end = bytes.find('\n', i);
        if (end == std::string::npos)
            end = bytes.size();
        std::string line = bytes.substr(i, end - i);
        i = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        std::size_t start = 0;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
            ++start;
        line = line.substr(start);
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        if (line.front() == '[' && line.back() == ']')
        {
            inRoute = line == "[Route]";
            continue;
        }
        if (!inRoute)
            continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = line.substr(0, eq);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
            key.pop_back();
        std::string value = line.substr(eq + 1);
        std::size_t v = 0;
        while (v < value.size() && (value[v] == ' ' || value[v] == '\t'))
            ++v;
        value = value.substr(v);
        if (key == "Enabled")
        {
            sawEnabled = true;
            enabled = value == "1";
        }
        else if (key == "Load")
        {
            ApplyLoadValue(value);
        }
    }
    return sawEnabled && enabled;
}

struct HookTarget {
    unsigned int rva;
    const unsigned char* prologue;
    std::size_t prologueBytes;
    void* detour;
    void** original;
    const char* name;
};

bool InstallRouteHooks()
{
    HMODULE game = GetModuleHandleW(nullptr);
    if (game == nullptr)
        return false;

    auto* base = reinterpret_cast<unsigned char*>(game);
    g_makeName = reinterpret_cast<MakeNameFn>(base + kMakeNameRva);
    g_nameToString = reinterpret_cast<NameToStringFn>(base + kNameToStringRva);
    g_freeWide = reinterpret_cast<WideFreeFn>(base + kWideFreeRva);
    g_game = base;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    g_gameEnd = base + nt->OptionalHeader.SizeOfImage;

    const HookTarget targets[] = {
        {kLevelLoadStartRva, kLoadPrologue, sizeof(kLoadPrologue), reinterpret_cast<void*>(&LevelLoadStartDetour),
         reinterpret_cast<void**>(&g_originalLoad), "LevelLoadStart"},
        {kLevelReleaseRva, kReleasePrologue, sizeof(kReleasePrologue), reinterpret_cast<void*>(&LevelReleaseDetour),
         reinterpret_cast<void**>(&g_originalRelease), "LevelRelease"},
        {kLevelReleaseStartRva, kReleaseStartPrologue, sizeof(kReleaseStartPrologue),
         reinterpret_cast<void*>(&LevelReleaseStartDetour), reinterpret_cast<void**>(&g_originalReleaseStart),
         "LevelReleaseStart"},
        {kLoadStreamRva, kLoadStreamPrologue, sizeof(kLoadStreamPrologue), reinterpret_cast<void*>(&LoadStreamDetour),
         reinterpret_cast<void**>(&g_originalLoadStream), "LoadStreamLevel"},
        {kUnloadStreamRva, kUnloadStreamPrologue, sizeof(kUnloadStreamPrologue),
         reinterpret_cast<void*>(&UnloadStreamDetour), reinterpret_cast<void**>(&g_originalUnloadStream),
         "UnloadStreamLevel"},
    };
    for (const HookTarget& target : targets)
    {
        if (!HasPrefix(base + target.rva, target.prologue, target.prologueBytes))
        {
            LOG("ERROR: route %s prologue mismatch at RVA 0x%X", target.name, target.rva);
            return false;
        }
    }

    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
        return false;

    // All or nothing: a refused release without the matching unload hook would
    // leave the blueprint and the manager disagreeing about what is loaded.
    int created = 0;
    for (const HookTarget& target : targets)
    {
        if (MH_CreateHook(base + target.rva, target.detour, target.original) != MH_OK)
        {
            LOG("ERROR: route %s hook was not created", target.name);
            break;
        }
        ++created;
    }
    bool enabled = created == static_cast<int>(std::size(targets));
    for (int i = 0; enabled && i < created; ++i)
        enabled = MH_EnableHook(base + targets[i].rva) == MH_OK;
    if (!enabled)
    {
        for (int i = 0; i < created; ++i)
        {
            MH_DisableHook(base + targets[i].rva);
            MH_RemoveHook(base + targets[i].rva);
        }
        return false;
    }
    LOG("Route: whole-line hooks installed (%d YMTc packages, %d old YMT packages, load %s)", kPackageCount,
        kOldPackageCount, g_loadLabel);
    return true;
}

} // namespace

namespace features {

void Route_Install(const wchar_t* moduleDir)
{
    static std::atomic<long> installed{0};
    if (installed.exchange(1) != 0)
        return;
    if (!ReadRouteEnabled(moduleDir))
    {
        LOG("Route: disabled");
        return;
    }
    if (!InstallRouteHooks())
        LOG("ERROR: route hook was not installed");
}

} // namespace features
