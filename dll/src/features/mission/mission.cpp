#include "features/module_api.h"

#include <MinHook.h>

#include "game_catalog.h"
#include "log.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>

// A card swipe logs in before the mode buttons. Login writes
// playable_mission at +120 (count +128) and leaves the release map at +200
// empty. CheckReleaseMission and GetPlayableMission search only that map.
// The line screen does not call them. After a swipe, Normal mode still shows
// the line buttons and returns from an empty station list without either
// hook running.
//
// The station list is GetNormalMission (sub_1401C2690) and
// GetNormalMissionByCondition (sub_1401C26B0). Both read the table at +360,
// count at +368, stride 0xA0. The 00:28 log had 106 built-in rows, then 0
// after the eight startup mission replies. ApplyMissionData (sub_1401C6C60)
// copies incoming+360 over that table, so an empty reply clears the line.
// The hook below returns without copying when the incoming count is 0.
// Recorded in docs/process/2026-10-30-normal-mode-missions.md.
//
// GetReleaseMissionList was not called on that screen. Its copy
// (sub_140226000) reads mission+968, count +976, stride 0x30. An empty copy
// is still filled from the built-in catalog. The caller's copy
// (sub_140263000) and destroy (sub_140261660) own the buffer from
// sub_141224070. Nothing here writes back into the player.
//
// GetPlayableMissionData (sub_140225870) and the real-mode list
// (sub_140225930) copy the login arrays with the same allocators. Those two
// are synthesized only while both counts are zero.
//
// MissionData also has 37 Debug rows. The blueprint calls SetNormalMission
// only for isNormalMode rows, so those 37 never reach the line list. After
// SetMission stores one, the hook calls SetNormalMission (sub_1401D0530).
// That copies the game's own row, station text included. The line screen
// filters on the category byte, so the new row then takes the category of
// another mission on the same line and is marked normal.
//
// Image base in IDA is 0x140000000. Runtime addresses are module base + RVA.

namespace {

constexpr unsigned int kGetPlayableMissionRva = 0x1C34B0;     // sub_1401C34B0
constexpr unsigned int kGetPlayableMissionDataRva = 0x225870; // sub_140225870
constexpr unsigned int kGetPlayableRealModeRva = 0x1C36C0;    // sub_1401C36C0
constexpr unsigned int kGetPlayableRealModeListRva = 0x225930; // sub_140225930
constexpr unsigned int kCheckReleaseMissionRva = 0x1B5B30;    // sub_1401B5B30
constexpr unsigned int kApplyMissionDataRva = 0x1C6C60;       // sub_1401C6C60
constexpr unsigned int kSetMissionRva = 0x1CF9C0;             // sub_1401CF9C0
constexpr unsigned int kSetNormalMissionRva = 0x1D0530;       // sub_1401D0530
constexpr unsigned int kGetNormalMissionInnerRva = 0x1C2690;  // sub_1401C2690
constexpr int kNormalMissionStride = 0xA0;
constexpr int kMissionNumberOffset = 8;
constexpr int kCategoryOffset = 0xD;
constexpr int kLineTypeOffset = 0x6D;
constexpr int kIsNormalModeOffset = 0x94;
constexpr unsigned int kGetReleaseMissionListRva = 0x225F20;  // sub_140225F20
constexpr unsigned int kCopyReleaseMissionListRva = 0x226000; // sub_140226000
constexpr unsigned int kResizeRecordsRva = 0x1224070;         // sub_141224070
constexpr unsigned int kResizeIntsRva = 0xEB4E30;             // sub_140EB4E30
constexpr unsigned int kRecordVtableRva = 0x237F040;          // off_14237F040
constexpr int kMissionObject = 0x68;
constexpr int kReleaseCount = 0x3D0;
constexpr int kNormalMissionData = 0x168;
constexpr int kNormalMissionCount = 0x170;

constexpr int kPlayableCount = 128;
constexpr int kRealModeCount = 144;
constexpr int kDifficultyCount = 4;
constexpr int kRecordBytes = 48;

constexpr std::size_t kMissionCount = sizeof(kGameMissionNumbers) / sizeof(kGameMissionNumbers[0]);
constexpr std::size_t kDebugMissionCount =
    sizeof(kDebugMissionNumbers) / sizeof(kDebugMissionNumbers[0]);
static_assert(kDebugMissionCount > 0 && kDebugMissionCount <= 64, "debug mission bitset");

constexpr bool CatalogIsSorted()
{
    for (std::size_t i = 1; i < kMissionCount; ++i)
    {
        if (kGameMissionNumbers[i] <= kGameMissionNumbers[i - 1])
            return false;
    }
    return kMissionCount > 0;
}

static_assert(CatalogIsSorted(), "mission catalog must be strictly increasing");

struct ScriptArray
{
    void* data;
    int count;
    int max;
};

struct PlayableRecord
{
    void* vtable;
    std::int32_t mission;
    std::uint8_t difficulty;
    std::uint8_t padDifficulty[3];
    std::int32_t playCount;
    std::int32_t clearCount;
    std::uint8_t rank;
    std::uint8_t padRank[3];
    std::int32_t totalScore;
    std::int32_t nearStop;
    std::int32_t perfectStop;
    std::int32_t perfectDouble;
    std::int32_t padTail;
};

static_assert(sizeof(PlayableRecord) == kRecordBytes, "playable record stride");
static_assert(offsetof(PlayableRecord, mission) == 8, "mission number");
static_assert(offsetof(PlayableRecord, difficulty) == 12, "difficulty byte");
static_assert(offsetof(PlayableRecord, playCount) == 16, "play count");
static_assert(offsetof(PlayableRecord, clearCount) == 20, "clear count");
static_assert(offsetof(PlayableRecord, rank) == 24, "rank byte");
static_assert(offsetof(PlayableRecord, totalScore) == 28, "total score");
static_assert(offsetof(PlayableRecord, nearStop) == 32, "near stop");
static_assert(offsetof(PlayableRecord, perfectStop) == 36, "perfect stop");
static_assert(offsetof(PlayableRecord, perfectDouble) == 40, "perfect double");

using GetPlayableMissionFn = char(__fastcall*)(void* self, int mission, unsigned char difficulty, void* out);
using CopyArrayFn = void*(__fastcall*)(void* self, void* out);
using GetPlayableRealModeFn = bool(__fastcall*)(void* self, int mission);
using CheckReleaseMissionFn = char(__fastcall*)(void* self, int mission, unsigned char hasProfessional);
using ResizeFn = void(__fastcall*)(ScriptArray* array, int count, int oldMax);
using ApplyMissionDataFn = void(__fastcall*)(void* self, void* incoming);
using SetMissionFn = void(__fastcall*)(void* self, int mission, void* row);
using SetNormalMissionFn = void(__fastcall*)(void* self, int mission);
using GetNormalMissionFn = void*(__fastcall*)(void* self, void* out);

GetPlayableMissionFn g_originalMission = nullptr;
CopyArrayFn g_originalMissionData = nullptr;
GetPlayableRealModeFn g_originalRealMode = nullptr;
CopyArrayFn g_originalRealModeList = nullptr;
CheckReleaseMissionFn g_originalCheckRelease = nullptr;
CopyArrayFn g_originalReleaseList = nullptr;
CopyArrayFn g_originalCopyRelease = nullptr;
ApplyMissionDataFn g_originalApply = nullptr;
SetMissionFn g_originalSetMission = nullptr;
SetNormalMissionFn g_setNormalMission = nullptr;
GetNormalMissionFn g_originalGetNormal = nullptr;
std::atomic<int> g_reportedApply{0};
std::atomic<unsigned long long> g_debugAdded{0};
std::atomic<int> g_reportedDebug{0};
std::atomic<int> g_reportedDebugFault{0};
int g_debugDepth = 0;

// Index into the normal-mode table after SetNormalMission appends one row.
// numberAt8 and line are checked again later: the array can move, and a
// replaced table must not be edited at a stale index.
struct DebugSlot
{
    int index;
    std::int32_t numberAt8;
    unsigned char line;
    unsigned char category;
    bool live;
    bool categorized;
};

DebugSlot g_debugSlots[kDebugMissionCount] = {};
ResizeFn g_resizeRecords = nullptr;
ResizeFn g_resizeInts = nullptr;
void* g_recordVtable = nullptr;
std::atomic<int> g_reportedEmpty{0};
std::atomic<int> g_reportedReleaseFill{0};
std::atomic<int> g_reportedReleaseSeen{0};
std::atomic<int> g_traceCheck{0};
std::atomic<int> g_tracePlayable{0};
std::atomic<int> g_traceMissionData{0};

const unsigned char kMissionPrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8D, 0xA9, 0xC8};
const unsigned char kMissionDataPrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x33,
    0xC0, 0x48, 0x8B, 0xFA};
const unsigned char kRealModePrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x8B, 0x81, 0x20, 0x01, 0x00, 0x00, 0x33, 0xDB, 0x4C, 0x63, 0xDA};
const unsigned char kRealModeListPrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x33,
    0xC0, 0x48, 0x8B, 0xDA};
const unsigned char kResizePrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x63, 0xDA};
const unsigned char kCheckReleasePrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x45, 0x33, 0xF6};
const unsigned char kReleaseListPrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48};
const unsigned char kCopyReleasePrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x33};
const unsigned char kApplyMissionPrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20, 0x48};
const unsigned char kSetMissionPrologue[] = {
    0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x89, 0x54, 0x24, 0x10, 0x57, 0x48};
const unsigned char kSetNormalMissionPrologue[] = {
    0x55, 0x48, 0x8D, 0x6C, 0x24, 0xA9, 0x48, 0x81, 0xEC, 0xC0, 0x00, 0x00, 0x00};
const unsigned char kGetNormalMissionPrologue[] = {
    0x48, 0x8B, 0xC2, 0x48, 0x8D, 0x91, 0x68, 0x01, 0x00, 0x00};

bool HasPrefix(const unsigned char* bytes, const unsigned char* prefix, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i)
    {
        if (bytes[i] != prefix[i])
            return false;
    }
    return true;
}

int ReadCount(const void* self, int offset)
{
    return *reinterpret_cast<const int*>(static_cast<const unsigned char*>(self) + offset);
}

void ProbeMissionObject(
    const void* self, void** field68, int* playable, int* table, int* release)
{
    *field68 = nullptr;
    *playable = -1;
    *table = -1;
    *release = -1;
    if (self == nullptr)
        return;
    const auto* bytes = static_cast<const unsigned char*>(self);
    __try
    {
        *field68 = *reinterpret_cast<void* const*>(bytes + kMissionObject);
        *playable = *reinterpret_cast<const int*>(bytes + kPlayableCount);
        *table = *reinterpret_cast<const int*>(bytes + kNormalMissionCount);
        *release = *reinterpret_cast<const int*>(bytes + kReleaseCount);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *playable = -2;
    }
}

void TraceCall(std::atomic<int>& seen, const char* name, const void* self)
{
    const int index = seen.fetch_add(1, std::memory_order_relaxed);
    if (index >= 4)
        return;
    void* field68 = nullptr;
    int playable = -1;
    int table = -1;
    int release = -1;
    ProbeMissionObject(self, &field68, &playable, &table, &release);
    LOG("mission trace %s #%d self=%p field68=%p playable=%d table=%d release=%d",
        name, index, self, field68, playable, table, release);
}

// Login writes both arrays or neither. A card that logged in with an empty
// real-mode list keeps that empty list.
bool ProfileHasNoRelease(const void* self)
{
    return ReadCount(self, kPlayableCount) == 0 && ReadCount(self, kRealModeCount) == 0;
}

bool CatalogContains(int mission)
{
    return std::binary_search(
        std::begin(kGameMissionNumbers), std::end(kGameMissionNumbers), mission);
}

void ReportReleased()
{
    if (g_reportedEmpty.exchange(1, std::memory_order_relaxed) == 0)
        LOG("Release map missed; releasing the built-in catalog (%zu)", kMissionCount);
}

void ReportReleaseList(int count)
{
    if (count <= 0)
    {
        if (g_reportedReleaseFill.exchange(1, std::memory_order_relaxed) == 0)
            LOG("Release list was empty; filled the built-in catalog (%zu)", kMissionCount);
        return;
    }
    if (g_reportedReleaseSeen.exchange(1, std::memory_order_relaxed) == 0)
        LOG("Release list already had %d rows", count);
}

// Same record the release-list copy writes: vtable, mission, difficulty, then
// the score ints. The caller destroys the buffer; scores stay zero.
bool FillCatalogRecords(ScriptArray* array)
{
    array->data = nullptr;
    array->count = 0;
    array->max = 0;
    if (g_resizeRecords == nullptr || g_recordVtable == nullptr)
        return false;
    const int total = static_cast<int>(kMissionCount) * kDifficultyCount;
    array->count = total;
    g_resizeRecords(array, total, 0);
    if (array->data == nullptr)
    {
        array->count = 0;
        array->max = 0;
        return false;
    }
    auto* records = static_cast<PlayableRecord*>(array->data);
    int index = 0;
    for (const int mission : kGameMissionNumbers)
    {
        for (int difficulty = 0; difficulty < kDifficultyCount; ++difficulty, ++index)
        {
            PlayableRecord record{};
            record.vtable = g_recordVtable;
            record.mission = mission;
            record.difficulty = static_cast<std::uint8_t>(difficulty);
            records[index] = record;
        }
    }
    return true;
}

char __fastcall CheckReleaseMissionDetour(void* self, int mission, unsigned char hasProfessional)
{
    TraceCall(g_traceCheck, "CheckReleaseMission", self);
    const char released = g_originalCheckRelease(self, mission, hasProfessional);
    if (released || self == nullptr || !CatalogContains(mission))
        return released;
    ReportReleased();
    return 1;
}

char __fastcall GetPlayableMissionDetour(void* self, int mission, unsigned char difficulty, void* out)
{
    TraceCall(g_tracePlayable, "GetPlayableMission", self);
    const char found = g_originalMission(self, mission, difficulty, out);
    if (found || self == nullptr || difficulty >= kDifficultyCount || !CatalogContains(mission))
        return found;
    // The miss path already wrote the mission number and difficulty into out.
    ReportReleased();
    return 1;
}

bool __fastcall GetPlayableRealModeDetour(void* self, int mission)
{
    const bool found = g_originalRealMode(self, mission);
    if (found || self == nullptr || !CatalogContains(mission))
        return found;
    ReportReleased();
    return true;
}

void* __fastcall GetPlayableMissionDataDetour(void* self, void* out)
{
    TraceCall(g_traceMissionData, "GetPlayableMissionData", self);
    if (self == nullptr || out == nullptr || !ProfileHasNoRelease(self) || g_resizeRecords == nullptr)
        return g_originalMissionData(self, out);

    // Match sub_140225870: count is stored before the allocator, which writes max.
    if (!FillCatalogRecords(static_cast<ScriptArray*>(out)))
    {
        LOG("ERROR: offline mission list was not allocated");
        return out;
    }
    ReportReleased();
    return out;
}

void* __fastcall GetPlayableRealModeListDetour(void* self, void* out)
{
    if (self == nullptr || out == nullptr || !ProfileHasNoRelease(self) || g_resizeInts == nullptr)
        return g_originalRealModeList(self, out);

    auto* array = static_cast<ScriptArray*>(out);
    const int total = static_cast<int>(kMissionCount);
    array->data = nullptr;
    array->count = total;
    array->max = 0;
    g_resizeInts(array, total, 0);
    if (array->data == nullptr)
    {
        array->count = 0;
        array->max = 0;
        LOG("ERROR: offline real-mode list was not allocated");
        return out;
    }
    auto* numbers = static_cast<int*>(array->data);
    for (std::size_t i = 0; i < kMissionCount; ++i)
        numbers[i] = kGameMissionNumbers[i];
    ReportReleased();
    return out;
}

void* __fastcall GetReleaseMissionListDetour(void* self, void* out)
{
    if (out == nullptr || g_originalReleaseList == nullptr)
        return out;
    const void* missionObject = nullptr;
    if (self != nullptr)
    {
        missionObject = *reinterpret_cast<void* const*>(
            static_cast<const unsigned char*>(self) + kMissionObject);
    }
    const int count = missionObject == nullptr ? 0 : ReadCount(missionObject, kReleaseCount);
    if (count > 0)
    {
        ReportReleaseList(count);
        return g_originalReleaseList(self, out);
    }
    if (!FillCatalogRecords(static_cast<ScriptArray*>(out)))
    {
        LOG("ERROR: release list was not allocated");
        return out;
    }
    ReportReleaseList(0);
    return out;
}

void* __fastcall CopyReleaseMissionListDetour(void* self, void* out)
{
    if (out == nullptr || g_originalCopyRelease == nullptr)
        return out;
    const int count = self == nullptr ? 0 : ReadCount(self, kReleaseCount);
    if (count > 0)
    {
        ReportReleaseList(count);
        return g_originalCopyRelease(self, out);
    }
    if (!FillCatalogRecords(static_cast<ScriptArray*>(out)))
    {
        LOG("ERROR: release list was not allocated");
        return out;
    }
    ReportReleaseList(0);
    return out;
}

void RemoveHook(void* target)
{
    MH_DisableHook(target);
    MH_RemoveHook(target);
}

bool Hook(void* target, void* detour, void** original)
{
    if (MH_CreateHook(target, detour, original) != MH_OK)
        return false;
    if (MH_EnableHook(target) != MH_OK)
    {
        MH_RemoveHook(target);
        *original = nullptr;
        return false;
    }
    return true;
}

bool InstallMissionHooks()
{
    HMODULE game = GetModuleHandleW(L"TG4AC-Win64-Shipping.exe");
    if (game == nullptr)
        game = GetModuleHandleW(nullptr);
    if (game == nullptr)
        return false;

    auto* base = reinterpret_cast<unsigned char*>(game);
    auto* mission = base + kGetPlayableMissionRva;
    auto* missionData = base + kGetPlayableMissionDataRva;
    auto* realMode = base + kGetPlayableRealModeRva;
    auto* realModeList = base + kGetPlayableRealModeListRva;
    auto* checkRelease = base + kCheckReleaseMissionRva;
    auto* releaseList = base + kGetReleaseMissionListRva;
    auto* copyRelease = base + kCopyReleaseMissionListRva;
    auto* resizeRecords = base + kResizeRecordsRva;
    auto* resizeInts = base + kResizeIntsRva;
    if (!HasPrefix(mission, kMissionPrologue, sizeof(kMissionPrologue)) ||
        !HasPrefix(missionData, kMissionDataPrologue, sizeof(kMissionDataPrologue)) ||
        !HasPrefix(realMode, kRealModePrologue, sizeof(kRealModePrologue)) ||
        !HasPrefix(realModeList, kRealModeListPrologue, sizeof(kRealModeListPrologue)) ||
        !HasPrefix(checkRelease, kCheckReleasePrologue, sizeof(kCheckReleasePrologue)) ||
        !HasPrefix(releaseList, kReleaseListPrologue, sizeof(kReleaseListPrologue)) ||
        !HasPrefix(copyRelease, kCopyReleasePrologue, sizeof(kCopyReleasePrologue)) ||
        !HasPrefix(resizeRecords, kResizePrologue, sizeof(kResizePrologue)) ||
        !HasPrefix(resizeInts, kResizePrologue, sizeof(kResizePrologue)))
    {
        return false;
    }

    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
        return false;

    g_resizeRecords = reinterpret_cast<ResizeFn>(resizeRecords);
    g_resizeInts = reinterpret_cast<ResizeFn>(resizeInts);
    g_recordVtable = base + kRecordVtableRva;

    void* enabled[7] = {};
    int enabledCount = 0;
    const bool hooked =
        Hook(checkRelease, reinterpret_cast<void*>(&CheckReleaseMissionDetour),
            reinterpret_cast<void**>(&g_originalCheckRelease)) &&
        Hook(mission, reinterpret_cast<void*>(&GetPlayableMissionDetour),
            reinterpret_cast<void**>(&g_originalMission)) &&
        Hook(missionData, reinterpret_cast<void*>(&GetPlayableMissionDataDetour),
            reinterpret_cast<void**>(&g_originalMissionData)) &&
        Hook(realMode, reinterpret_cast<void*>(&GetPlayableRealModeDetour),
            reinterpret_cast<void**>(&g_originalRealMode)) &&
        Hook(realModeList, reinterpret_cast<void*>(&GetPlayableRealModeListDetour),
            reinterpret_cast<void**>(&g_originalRealModeList)) &&
        Hook(releaseList, reinterpret_cast<void*>(&GetReleaseMissionListDetour),
            reinterpret_cast<void**>(&g_originalReleaseList)) &&
        Hook(copyRelease, reinterpret_cast<void*>(&CopyReleaseMissionListDetour),
            reinterpret_cast<void**>(&g_originalCopyRelease));
    if (hooked)
        return true;

    if (g_originalCheckRelease != nullptr)
        enabled[enabledCount++] = checkRelease;
    if (g_originalMission != nullptr)
        enabled[enabledCount++] = mission;
    if (g_originalMissionData != nullptr)
        enabled[enabledCount++] = missionData;
    if (g_originalRealMode != nullptr)
        enabled[enabledCount++] = realMode;
    if (g_originalRealModeList != nullptr)
        enabled[enabledCount++] = realModeList;
    if (g_originalReleaseList != nullptr)
        enabled[enabledCount++] = releaseList;
    if (g_originalCopyRelease != nullptr)
        enabled[enabledCount++] = copyRelease;
    for (int i = 0; i < enabledCount; ++i)
        RemoveHook(enabled[i]);
    g_originalCheckRelease = nullptr;
    g_originalMission = nullptr;
    g_originalMissionData = nullptr;
    g_originalRealMode = nullptr;
    g_originalRealModeList = nullptr;
    g_originalReleaseList = nullptr;
    g_originalCopyRelease = nullptr;
    g_resizeRecords = nullptr;
    g_resizeInts = nullptr;
    g_recordVtable = nullptr;
    return false;
}

using ExecFn = void(__fastcall*)(void* self, void* frame, void* result);

struct ExecTrace
{
    const char* name;
    unsigned int rva;
    unsigned char prefix[8];
    ExecFn original;
};

ExecTrace g_execTrace[] = {
    {"GetNormalMission", 0x2F17C0, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74}, nullptr},
    {"GetNormalMissionByCondition", 0x2F1860, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x57}, nullptr},
    {"GetAllMissionByCondition", 0x2EA6E0, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74}, nullptr},
    {"GetProfessionalMission", 0x2F2D70, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74}, nullptr},
    {"GetProfessionalMissionByCondition", 0x2F2E10, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x57}, nullptr},
    {"GetDailyMission", 0x2EBDC0, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74}, nullptr},
    {"GetMission", 0x2EFE10, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74}, nullptr},
    {"GetMissionClearLine", 0x2F0000, {0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B}, nullptr},
    {"GetMissionDataObject", 0x2F0050, {0x48, 0x8B, 0x42, 0x20, 0x45, 0x33, 0xC9, 0x48}, nullptr},
    {"isTrainLineTypeMission", 0x30B5F0, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74}, nullptr},
    {"GetMissionReleaseCondition", 0x2F0290, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C}, nullptr},
    {"SetMissionOpenState", 0x3040E0, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x57}, nullptr},
};

constexpr int kExecTraceCount = sizeof(g_execTrace) / sizeof(g_execTrace[0]);
std::atomic<int> g_execTraceSeen[kExecTraceCount] = {};

int DebugIndex(int mission)
{
    for (std::size_t i = 0; i < kDebugMissionCount; ++i)
    {
        if (kDebugMissionNumbers[i] == mission)
            return static_cast<int>(i);
    }
    return -1;
}

unsigned long long DebugMask(int index)
{
    return 1ULL << static_cast<unsigned int>(index);
}

bool ReadNormalTable(void* self, unsigned char** data, int* count)
{
    auto* bytes = static_cast<unsigned char*>(self);
    *data = *reinterpret_cast<unsigned char**>(bytes + kNormalMissionData);
    *count = *reinterpret_cast<int*>(bytes + kNormalMissionCount);
    if (*count < 0 || *count > 4096)
        return false;
    if (*count > 0 && *data == nullptr)
        return false;
    return true;
}

bool DebugSlotOwns(int rowIndex)
{
    for (std::size_t i = 0; i < kDebugMissionCount; ++i)
    {
        if (g_debugSlots[i].live && g_debugSlots[i].index == rowIndex)
            return true;
    }
    return false;
}

// The line list compares row+0xD with the category the blueprint asked for.
// Debug is not one of those categories. Copy the byte from another row on
// the same line (row+0x6D) and mark the row normal (row+0x94).
void ReclassifyDebugRow(unsigned char* data, int count, int rowIndex)
{
    unsigned char* row = data + static_cast<std::size_t>(rowIndex) * kNormalMissionStride;
    const unsigned char line = row[kLineTypeOffset];
    int donor = -1;
    for (int i = 0; i < count; ++i)
    {
        if (i == rowIndex || DebugSlotOwns(i))
            continue;
        unsigned char* other = data + static_cast<std::size_t>(i) * kNormalMissionStride;
        if (other[kLineTypeOffset] != line)
            continue;
        if (donor < 0)
            donor = i;
        if (other[kIsNormalModeOffset] != 0)
        {
            donor = i;
            break;
        }
    }
    if (donor >= 0)
    {
        const unsigned char* other =
            data + static_cast<std::size_t>(donor) * kNormalMissionStride;
        row[kCategoryOffset] = other[kCategoryOffset];
    }
    row[kIsNormalModeOffset] = 1;
    for (std::size_t i = 0; i < kDebugMissionCount; ++i)
    {
        if (!g_debugSlots[i].live || g_debugSlots[i].index != rowIndex)
            continue;
        g_debugSlots[i].category = row[kCategoryOffset];
        g_debugSlots[i].categorized = donor >= 0;
    }
}

void RememberDebugRow(int index, unsigned char* data, int rowIndex)
{
    unsigned char* row = data + static_cast<std::size_t>(rowIndex) * kNormalMissionStride;
    DebugSlot slot{};
    slot.index = rowIndex;
    slot.numberAt8 = *reinterpret_cast<std::int32_t*>(row + kMissionNumberOffset);
    slot.line = row[kLineTypeOffset];
    slot.category = row[kCategoryOffset];
    slot.live = true;
    slot.categorized = false;
    g_debugSlots[index] = slot;
    ReclassifyDebugRow(data, rowIndex + 1, rowIndex);
}

// SetNormalMission looks the id up in the map SetMission just filled and
// appends one 0xA0 row, station text included. A miss leaves the count alone
// so a later list query can try again.
void AppendDebugMission(void* self, int index)
{
    const unsigned long long mask = DebugMask(index);
    if ((g_debugAdded.fetch_or(mask, std::memory_order_acq_rel) & mask) != 0)
        return;
    if (g_setNormalMission == nullptr)
    {
        g_debugAdded.fetch_and(~mask, std::memory_order_acq_rel);
        return;
    }
    int before = 0;
    unsigned char* beforeData = nullptr;
    if (!ReadNormalTable(self, &beforeData, &before))
    {
        g_debugAdded.fetch_and(~mask, std::memory_order_acq_rel);
        return;
    }
    g_setNormalMission(self, kDebugMissionNumbers[index]);
    int after = 0;
    unsigned char* data = nullptr;
    // The native append grows the table by one. Any other change is not our row.
    if (!ReadNormalTable(self, &data, &after) || after != before + 1 || data == nullptr)
    {
        g_debugAdded.fetch_and(~mask, std::memory_order_acq_rel);
        return;
    }
    RememberDebugRow(index, data, before);
}

void ReclassifyDebugRows(void* self)
{
    unsigned char* data = nullptr;
    int count = 0;
    if (!ReadNormalTable(self, &data, &count) || data == nullptr || count <= 0)
        return;
    for (std::size_t i = 0; i < kDebugMissionCount; ++i)
    {
        const DebugSlot& slot = g_debugSlots[i];
        if (!slot.live || slot.index < 0 || slot.index >= count)
            continue;
        unsigned char* row = data + static_cast<std::size_t>(slot.index) * kNormalMissionStride;
        const std::int32_t number = *reinterpret_cast<std::int32_t*>(row + kMissionNumberOffset);
        if (number != slot.numberAt8 || row[kLineTypeOffset] != slot.line)
            continue;
        ReclassifyDebugRow(data, count, slot.index);
    }
}

void ReportDebugMissions(void* self)
{
    if (g_reportedDebug.load(std::memory_order_relaxed) != 0)
        return;
    unsigned char* data = nullptr;
    int count = 0;
    if (!ReadNormalTable(self, &data, &count) || (count > 0 && data == nullptr))
        count = -1;
    int added = 0;
    int sample = -1;
    for (std::size_t i = 0; i < kDebugMissionCount; ++i)
    {
        if ((g_debugAdded.load(std::memory_order_relaxed) & DebugMask(static_cast<int>(i))) != 0)
        {
            ++added;
            if (sample < 0 && g_debugSlots[i].live)
                sample = static_cast<int>(i);
        }
    }
    // Wait until the built-in rows exist so the category is copied from a
    // real neighbour, not from the still-empty list.
    if (added == 0 || count < 106)
        return;
    if (g_reportedDebug.exchange(1, std::memory_order_relaxed) != 0)
        return;
    if (sample < 0)
    {
        LOG("Normal mode added %d of %zu debug missions; table %d",
            added, kDebugMissionCount, count);
        return;
    }
    const DebugSlot& slot = g_debugSlots[sample];
    LOG("Normal mode added %d of %zu debug missions; table %d; +8=%d mission %d line %u category %u donor %d",
        added, kDebugMissionCount, count, slot.numberAt8, kDebugMissionNumbers[sample],
        slot.line, slot.category, slot.categorized ? 1 : 0);
}

void ReleaseUnstoredDebugBits()
{
    for (std::size_t i = 0; i < kDebugMissionCount; ++i)
    {
        if (!g_debugSlots[i].live)
            g_debugAdded.fetch_and(~DebugMask(static_cast<int>(i)), std::memory_order_acq_rel);
    }
}

// A non-empty ApplyMissionData replaces the table. The map is untouched, so
// the next list query can append the debug rows again.
void ClearDebugMissionRows()
{
    g_debugAdded.store(0, std::memory_order_relaxed);
    g_reportedDebug.store(0, std::memory_order_relaxed);
    g_reportedDebugFault.store(0, std::memory_order_relaxed);
    for (std::size_t i = 0; i < kDebugMissionCount; ++i)
        g_debugSlots[i].live = false;
}

void EnsureDebugListed(void* self)
{
    if (self == nullptr || g_setNormalMission == nullptr || g_debugDepth != 0)
        return;
    g_debugDepth = 1;
    __try
    {
        for (int i = 0; i < static_cast<int>(kDebugMissionCount); ++i)
            AppendDebugMission(self, i);
        ReclassifyDebugRows(self);
        ReportDebugMissions(self);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        if (g_reportedDebugFault.exchange(1, std::memory_order_relaxed) == 0)
            LOG("ERROR: debug mission list update failed");
        ReleaseUnstoredDebugBits();
    }
    g_debugDepth = 0;
}

void __fastcall SetMissionDetour(void* self, int mission, void* row)
{
    g_originalSetMission(self, mission, row);
    const int index = DebugIndex(mission);
    if (index < 0 || self == nullptr || g_debugDepth != 0)
        return;
    g_debugDepth = 1;
    __try
    {
        AppendDebugMission(self, index);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LOG("ERROR: debug mission %d was not added", mission);
        ReleaseUnstoredDebugBits();
    }
    g_debugDepth = 0;
}

template<int Index>
void __fastcall TraceExec(void* self, void* frame, void* result)
{
    static_assert(Index >= 0 && Index < kExecTraceCount, "trace index");
    // GetNormalMission and GetNormalMissionByCondition. Their context is the
    // mission object: the line-screen trace read the table count from it.
    // Reclassify before the original walk so the category filter sees the row.
    if constexpr (Index == 0 || Index == 1)
        EnsureDebugListed(self);
    TraceCall(g_execTraceSeen[Index], g_execTrace[Index].name, self);
    if (g_execTrace[Index].original != nullptr)
        g_execTrace[Index].original(self, frame, result);
}

// The startup mission replies replace the table at +360 with the incoming
// table. A failed reply has no rows, so the copy turns 106 built-in missions
// into 0 and the line screen closes. Keep the built-in table in that case.
void __fastcall ApplyMissionDataDetour(void* self, void* incoming)
{
    const int incomingCount = incoming == nullptr ? -1 : ReadCount(incoming, kNormalMissionCount);
    const int selfCount = self == nullptr ? -1 : ReadCount(self, kNormalMissionCount);
    if (incomingCount <= 0)
    {
        if (g_reportedApply.exchange(1, std::memory_order_relaxed) == 0)
        {
            LOG("ApplyMissionData kept %d built-in rows; incoming count %d",
                selfCount, incomingCount);
        }
        return;
    }
    g_originalApply(self, incoming);
    ClearDebugMissionRows();
    if (g_reportedApply.exchange(1, std::memory_order_relaxed) == 0)
        LOG("ApplyMissionData applied %d rows over %d", incomingCount, selfCount);
}

bool InstallApplyHook(unsigned char* base)
{
    auto* target = base + kApplyMissionDataRva;
    if (!HasPrefix(target, kApplyMissionPrologue, sizeof(kApplyMissionPrologue)))
    {
        LOG("ERROR: ApplyMissionData prologue mismatch");
        return false;
    }
    return Hook(target, reinterpret_cast<void*>(&ApplyMissionDataDetour),
        reinterpret_cast<void**>(&g_originalApply));
}

int InstallExecTraces(unsigned char* base)
{
    void* detours[kExecTraceCount] = {
        reinterpret_cast<void*>(&TraceExec<0>),
        reinterpret_cast<void*>(&TraceExec<1>),
        reinterpret_cast<void*>(&TraceExec<2>),
        reinterpret_cast<void*>(&TraceExec<3>),
        reinterpret_cast<void*>(&TraceExec<4>),
        reinterpret_cast<void*>(&TraceExec<5>),
        reinterpret_cast<void*>(&TraceExec<6>),
        reinterpret_cast<void*>(&TraceExec<7>),
        reinterpret_cast<void*>(&TraceExec<8>),
        reinterpret_cast<void*>(&TraceExec<9>),
        reinterpret_cast<void*>(&TraceExec<10>),
        reinterpret_cast<void*>(&TraceExec<11>),
    };
    int installed = 0;
    for (int i = 0; i < kExecTraceCount; ++i)
    {
        auto* target = base + g_execTrace[i].rva;
        if (!HasPrefix(target, g_execTrace[i].prefix, sizeof(g_execTrace[i].prefix)))
        {
            LOG("ERROR: mission trace prologue mismatch for %s", g_execTrace[i].name);
            continue;
        }
        if (!Hook(target, detours[i], reinterpret_cast<void**>(&g_execTrace[i].original)))
        {
            LOG("ERROR: mission trace hook failed for %s", g_execTrace[i].name);
            continue;
        }
        ++installed;
    }
    return installed;
}

bool InstallDebugMissionHooks(unsigned char* base)
{
    auto* setNormal = base + kSetNormalMissionRva;
    auto* setMission = base + kSetMissionRva;
    if (!HasPrefix(setNormal, kSetNormalMissionPrologue, sizeof(kSetNormalMissionPrologue)))
    {
        LOG("ERROR: SetNormalMission prologue mismatch");
        return false;
    }
    g_setNormalMission = reinterpret_cast<SetNormalMissionFn>(setNormal);
    if (!HasPrefix(setMission, kSetMissionPrologue, sizeof(kSetMissionPrologue)))
    {
        LOG("ERROR: SetMission prologue mismatch");
        return true;
    }
    if (!Hook(setMission, reinterpret_cast<void*>(&SetMissionDetour),
            reinterpret_cast<void**>(&g_originalSetMission)))
    {
        LOG("ERROR: SetMission hook failed");
        return true;
    }
    return true;
}

} // namespace

namespace features {

void Mission_Install(const wchar_t* moduleDir)
{
    static std::atomic<long> installed{0};
    if (moduleDir == nullptr || moduleDir[0] == L'\0')
        return;
    if (installed.exchange(1) != 0)
        return;

    if (!InstallMissionHooks())
    {
        LOG("ERROR: offline mission hooks were not installed");
        return;
    }
    LOG("Normal mode fills an empty release list from the built-in catalog");

    HMODULE game = GetModuleHandleW(L"TG4AC-Win64-Shipping.exe");
    if (game == nullptr)
        game = GetModuleHandleW(nullptr);
    if (game == nullptr)
    {
        LOG("ERROR: mission trace has no game module");
        return;
    }
    auto* image = reinterpret_cast<unsigned char*>(game);
    if (!InstallApplyHook(image))
        LOG("ERROR: ApplyMissionData hook was not installed");
    if (!InstallDebugMissionHooks(image))
        LOG("ERROR: debug missions stay out of the normal-mode list");
    else
        LOG("Debug missions are appended to the normal-mode list");
    const int traces = InstallExecTraces(image);
    LOG("Mission call trace installed (%d)", traces);
}

} // namespace features
