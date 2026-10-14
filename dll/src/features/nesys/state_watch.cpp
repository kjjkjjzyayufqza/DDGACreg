#include "features/nesys/state_watch.h"

#include <MinHook.h>

#include "log.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

// TG4AC 5.80.02 (image base 0x140000000).
//
// MyGameInstance.UpdateGameInstance calls NesysManager_Update every frame
// (exec thunk sub_1402FCD20 -> sub_140228C40 -> sub_140246240). The first call
// runs NESYS_INIT (sub_140001000), which opens \\.\pipe\nesys_games.
//
// NesysManager singleton (qword_142DD16C0):
//   +0   state: 0 starts NESYS on the next update, 2 disabled (operator_settings.cpp)
//   +4   NESYS_INIT succeeded
//   +5   network up (event 6 sets it, event 21 = DISCONNECT_REPLY clears it)
//   +6   waiting for CERT_INIT (event 1 clears it)
//   +9   last ping succeeded (sub_140239800)
//   +680 current player id, +684 NESiCA used in this play
// NESYS client (Code, qword_1432EF240) +8300: connected (CERT_INIT sets it).
// NesysManager_IsAbleSend (sub_140228610) is +4 && Code+8300 && +5 && !+6 && +9.
// MyGameInstance.StartReadNESiCA and NESiCAReadState only reach the card
// device while IsAbleSend holds, so these flags explain a reader that never
// starts.
//
// +684 is written only by NesysManager_SetUsedNESiCA (sub_140228B90), which no
// shipped blueprint calls (only TestData/DebugResultBaseScreen does), and
// cleared by NesysManager_ResetParameter (sub_14023AFD0). The main menu only
// enables the NESiCA mode button (typeSelectWidget.ableNESiCA) and logs the
// player in while NesysManager_IsUsedNESiCA is true, so the NESYS service
// marks it when a card select or registration succeeds (MarkNesicaUsed).

namespace {

constexpr std::uintptr_t kNesysUpdateRva = 0x228C40; // sub_140228C40
constexpr std::uintptr_t kNesysInitRva = 0x1000;     // sub_140001000 NESYS_INIT
constexpr std::uintptr_t kManagerRva = 0x2DD16C0;    // qword_142DD16C0
constexpr std::uintptr_t kClientRva = 0x32EF240;     // Code, qword_1432EF240

constexpr std::size_t kInitOk = 4;
constexpr std::size_t kNetworkUp = 5;
constexpr std::size_t kPending = 6;
constexpr std::size_t kPingOk = 9;
constexpr std::size_t kPlayerId = 680;
constexpr std::size_t kNesicaUsed = 684;
constexpr std::size_t kClientConnected = 8300;

const unsigned char kNesysUpdatePrologue[] = {0x48, 0x83, 0xEC, 0x38, 0x48, 0x8B, 0x05};
const unsigned char kNesysInitPrologue[] = {0x40, 0x57, 0x48, 0x83, 0xEC, 0x30};

using NesysUpdateFn = std::int64_t(__fastcall*)();
using NesysInitFn = std::int64_t(__fastcall*)(void* callback);

unsigned char* g_image = nullptr;
NesysUpdateFn g_originalNesysUpdate = nullptr;
NesysInitFn g_originalNesysInit = nullptr;

struct ManagerState
{
    int state = -1;
    bool initOk = false;
    bool networkUp = false;
    bool pending = false;
    bool connected = false;
    bool pingOk = false;
    bool nesicaUsed = false;
    std::uint32_t playerId = 0;

    bool operator==(const ManagerState& other) const
    {
        return state == other.state && initOk == other.initOk && networkUp == other.networkUp
            && pending == other.pending && connected == other.connected && pingOk == other.pingOk
            && nesicaUsed == other.nesicaUsed && playerId == other.playerId;
    }
};

// Game thread only.
ManagerState g_lastState;

unsigned char* Manager(unsigned char* image)
{
    return *reinterpret_cast<unsigned char**>(image + kManagerRva);
}

ManagerState ReadState(const unsigned char* manager)
{
    const auto* client = *reinterpret_cast<unsigned char* const*>(g_image + kClientRva);
    ManagerState state;
    std::memcpy(&state.state, manager, sizeof(state.state));
    state.initOk = manager[kInitOk] != 0;
    state.networkUp = manager[kNetworkUp] != 0;
    state.pending = manager[kPending] != 0;
    state.pingOk = manager[kPingOk] != 0;
    state.nesicaUsed = manager[kNesicaUsed] != 0;
    std::memcpy(&state.playerId, manager + kPlayerId, sizeof(state.playerId));
    if (client != nullptr) {
        std::uint32_t connected = 0;
        std::memcpy(&connected, client + kClientConnected, sizeof(connected));
        state.connected = connected != 0;
    }
    return state;
}

std::int64_t __fastcall NesysUpdateDetour()
{
    const std::int64_t result = g_originalNesysUpdate();
    const unsigned char* manager = Manager(g_image);
    if (manager == nullptr)
        return result;
    const ManagerState state = ReadState(manager);
    if (!(state == g_lastState)) {
        g_lastState = state;
        const bool ableSend = state.initOk && state.connected && state.networkUp && !state.pending && state.pingOk;
        LOG("NESYS: state %d, init %d, network %d, pending %d, connected %d, ping %d -> able to send %d; "
            "NESiCA used %d, player %u",
            state.state, state.initOk, state.networkUp, state.pending, state.connected, state.pingOk, ableSend,
            state.nesicaUsed, state.playerId);
    }
    return result;
}

std::int64_t __fastcall NesysInitDetour(void* callback)
{
    const std::int64_t result = g_originalNesysInit(callback);
    if (result == 1)
        LOG("NESYS: NESYS_INIT connected to the pipe");
    else
        LOG("ERROR: NESYS_INIT returned %lld", static_cast<long long>(result));
    return result;
}

bool Hook(std::uintptr_t rva, const unsigned char* prologue, std::size_t bytes, void* detour, void** original,
          const char* name)
{
    unsigned char* target = g_image + rva;
    if (std::memcmp(target, prologue, bytes) != 0) {
        LOG("ERROR: %s prologue mismatch at RVA 0x%llX", name, static_cast<unsigned long long>(rva));
        return false;
    }
    if (MH_CreateHook(target, detour, original) != MH_OK || MH_EnableHook(target) != MH_OK) {
        LOG("ERROR: %s hook was not installed", name);
        return false;
    }
    return true;
}

} // namespace

namespace nesys {

bool StartStateWatch()
{
    g_image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (g_image == nullptr)
        return false;

    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        return false;
    return Hook(kNesysUpdateRva, kNesysUpdatePrologue, sizeof(kNesysUpdatePrologue),
                reinterpret_cast<void*>(&NesysUpdateDetour), reinterpret_cast<void**>(&g_originalNesysUpdate),
                "NesysManager_Update")
        && Hook(kNesysInitRva, kNesysInitPrologue, sizeof(kNesysInitPrologue),
                reinterpret_cast<void*>(&NesysInitDetour), reinterpret_cast<void**>(&g_originalNesysInit),
                "NESYS_INIT");
}

bool MarkNesicaUsed()
{
    unsigned char* manager = Manager(reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr)));
    if (manager == nullptr)
        return false;
    *reinterpret_cast<volatile unsigned char*>(manager + kNesicaUsed) = 1;
    return true;
}

} // namespace nesys
