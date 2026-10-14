#include "stream.h"
#include "features/hook_chain.h"

#include "log.h"

#include <MinHook.h>

#include <windows.h>

#include <cstddef>
#include <cstdint>

// TG4AC 5.80.02 keeps the cabinet frame short by slicing load and release work.
// The sliced knobs are real console variables in TG4AC-Win64-Shipping.exe
// (image base 0x140000000). IConsoleVariable::Set is what the ini loader and
// the cabinet use, so a later Set of the arcade value is written back here.
//
// r.VSyncDWM (sub_1400C6580 registers it) is forced to 0. The cabinet writes 1
// after each load screen, and the D3D11 viewport then waits on the DWM before
// its Present. The compositor's output window already waits for vblank, so the
// two waits cost every other refresh.
//
// Not touched: gc.TimeBetweenPurgingPendingKillObjects (a purge interval, not a
// per-frame cap), r.Streaming.PoolSize, dgo.Streaming.PoolSizeKHTr, mip drop
// cvars, sg.* scalability, mesh LOD, and which levels load.

namespace {

constexpr std::uint32_t kSetByCode = 0x08000000u;

// Milliseconds. The cabinet restores these to 5 and, while a load screen is up,
// writes 100 and 300. Zero would stop actor registration on the first check
// (elapsed > limit). A long limit does not trip that check.
constexpr float kRaisedMilliseconds = 10000.0f;

// s.LevelStreamingComponentsUnregistrationGranularity. The unregister loop
// stops once the running component count exceeds this. Zero is a one-step cap.
// Registration granularity is the opposite: zero means no batch cap.
constexpr int kUnlimitedUnregBatch = 2147483647;

constexpr std::uintptr_t kFloatSetRva = 0x472480;  // sub_140472480
constexpr std::uintptr_t kIntSetRva = 0x4723F0;    // sub_1404723F0
constexpr std::uintptr_t kOwnedSetRva = 0x472200;  // sub_140472200
constexpr std::uintptr_t kFramesInitRva = 0x14E150; // sub_14014E150
constexpr std::uintptr_t kStaticInitRva = 0x14E850; // sub_14014E850

enum class FloatPolicy
{
    AtLeast,      // a larger limit spends more time per frame
    NonPositive,  // 0 disables the time slice (unregister)
};

enum class IntPolicy
{
    AtLeast,  // a larger count is a looser cap
    ForceZero // the value must stay 0 (no batch limit, no DWM wait)
};

struct FloatKnob
{
    const char* name;
    const wchar_t* forcedText;
    std::uintptr_t valueRva;
    std::uintptr_t cvarRva;
    float forced;
    FloatPolicy policy;
    bool loggedRewrite;
};

struct IntKnob
{
    const char* name;
    const wchar_t* forcedText;
    std::uintptr_t valueRva;
    std::uintptr_t cvarRva;
    int forced;
    IntPolicy policy;
    bool loggedRewrite;
};

struct OwnedKnob
{
    const char* name;
    const wchar_t* forcedText;
    std::uintptr_t cvarRva;
    std::uintptr_t dataRva;
    int forced;
    int imageDefault;
    bool applied;
    bool loggedRewrite;
};

using SetFn = std::int64_t (*)(void* self, const wchar_t* value, std::uint32_t flags);
using InitFn = int (*)();

unsigned char* g_base = nullptr;
SetFn g_floatSet = nullptr;
SetFn g_intSet = nullptr;
SetFn g_ownedSet = nullptr;
InitFn g_framesInit = nullptr;
InitFn g_staticInit = nullptr;

// valueRva is the float the loader reads. cvarRva is the IConsoleVariable*
// filled by the static initializer (0 until then).
FloatKnob g_floats[] = {
    { "s.AsyncLoadingTimeLimit", L"10000", 0x2D739C4, 0x2FDED38, kRaisedMilliseconds, FloatPolicy::AtLeast, false },
    { "s.PriorityAsyncLoadingExtraTime", L"10000", 0x2D739CC, 0x2FDED58, kRaisedMilliseconds, FloatPolicy::AtLeast, false },
    { "s.LevelStreamingActorsUpdateTimeLimit", L"10000", 0x2D739D0, 0x2FDED68, kRaisedMilliseconds, FloatPolicy::AtLeast, false },
    { "s.UnregisterComponentsTimeLimit", L"0", 0x2D739DC, 0x2FDED78, 0.0f, FloatPolicy::NonPositive, false },
};

IntKnob g_ints[] = {
    { "s.AsyncLoadingUseFullTimeLimit", L"1", 0x2D739C8, 0x2FDED48, 1, IntPolicy::AtLeast, false },
    { "s.LevelStreamingComponentsRegistrationGranularity", L"0", 0x2D739D4, 0x2FDED88, 0, IntPolicy::ForceZero, false },
    { "s.LevelStreamingComponentsUnregistrationGranularity", L"2147483647", 0x2D739D8, 0x2FDED98, kUnlimitedUnregBatch, IntPolicy::AtLeast, false },
    { "r.VSyncDWM", L"0", 0x2F1FAD0, 0x2F20148, 0, IntPolicy::ForceZero, false },
};

// dataRva is the int* returned by the cvar's GetData (object + 104).
OwnedKnob g_frames = {
    "r.Streaming.FramesForFullUpdate", L"0", 0x2FFAC80, 0x2FFAC88, 0, 5, false, false
};
OwnedKnob g_staticComponents = {
    "r.Streaming.NumStaticComponentsProcessedPerFrame", L"0", 0x2FFB120, 0x2FFB128, 0, 50, false, false
};

bool BuildIniPath(const wchar_t* moduleDir, wchar_t* out, std::size_t outChars)
{
    if (moduleDir == nullptr || moduleDir[0] == L'\0' || outChars < 12)
        return false;
    if (wcscpy_s(out, outChars, moduleDir) != 0)
        return false;
    const std::size_t length = wcslen(out);
    if (length > 0 && out[length - 1] != L'\\' && out[length - 1] != L'/') {
        if (wcscat_s(out, outChars, L"\\") != 0)
            return false;
    }
    return wcscat_s(out, outChars, L"config.ini") == 0;
}

bool ParseNumber(const wchar_t* text, double& out)
{
    if (text == nullptr)
        return false;
    while (*text == L' ' || *text == L'\t')
        ++text;
    double sign = 1.0;
    if (*text == L'+')
        ++text;
    else if (*text == L'-') {
        sign = -1.0;
        ++text;
    }
    bool any = false;
    double value = 0.0;
    while (*text >= L'0' && *text <= L'9') {
        any = true;
        value = value * 10.0 + static_cast<double>(*text - L'0');
        ++text;
    }
    if (*text == L'.') {
        ++text;
        double place = 0.1;
        while (*text >= L'0' && *text <= L'9') {
            any = true;
            value += static_cast<double>(*text - L'0') * place;
            place *= 0.1;
            ++text;
        }
    }
    if (!any)
        return false;
    out = sign * value;
    return true;
}

bool FloatIsTighter(const FloatKnob& knob, double incoming)
{
    if (knob.policy == FloatPolicy::NonPositive)
        return incoming > 0.0;
    return incoming + 0.001 < static_cast<double>(knob.forced);
}

bool IntIsTighter(const IntKnob& knob, int incoming)
{
    if (knob.policy == IntPolicy::ForceZero)
        return incoming != 0;
    return incoming < knob.forced;
}

void NoteFloatRewrite(FloatKnob& knob, double incoming)
{
    if (knob.loggedRewrite)
        return;
    knob.loggedRewrite = true;
    LOG("Stream %s later set %g kept %g", knob.name, incoming, static_cast<double>(knob.forced));
}

void NoteIntRewrite(IntKnob& knob, int incoming)
{
    if (knob.loggedRewrite)
        return;
    knob.loggedRewrite = true;
    LOG("Stream %s later set %d kept %d", knob.name, incoming, knob.forced);
}

void NoteOwnedRewrite(OwnedKnob& knob, int incoming)
{
    if (knob.loggedRewrite)
        return;
    knob.loggedRewrite = true;
    LOG("Stream %s later set %d kept %d", knob.name, incoming, knob.forced);
}

FloatKnob* FindFloat(const void* global)
{
    if (g_base == nullptr || global == nullptr)
        return nullptr;
    for (FloatKnob& knob : g_floats) {
        if (g_base + knob.valueRva == global)
            return &knob;
    }
    return nullptr;
}

IntKnob* FindInt(const void* global)
{
    if (g_base == nullptr || global == nullptr)
        return nullptr;
    for (IntKnob& knob : g_ints) {
        if (g_base + knob.valueRva == global)
            return &knob;
    }
    return nullptr;
}

bool IsOwned(void* self, const OwnedKnob& knob)
{
    if (g_base == nullptr || self == nullptr)
        return false;
    void* cvar = *reinterpret_cast<void**>(g_base + knob.cvarRva);
    if (cvar != nullptr && cvar == self)
        return true;
    int* data = *reinterpret_cast<int**>(g_base + knob.dataRva);
    return data != nullptr && data == reinterpret_cast<int*>(static_cast<unsigned char*>(self) + 104);
}

std::int64_t FloatSetDetour(void* self, const wchar_t* value, std::uint32_t flags)
{
    if (g_floatSet == nullptr)
        return 0;
    void* global = nullptr;
    if (self != nullptr)
        global = *reinterpret_cast<void**>(static_cast<unsigned char*>(self) + 104);
    FloatKnob* knob = FindFloat(global);
    double incoming = 0.0;
    if (knob == nullptr || !ParseNumber(value, incoming) || !FloatIsTighter(*knob, incoming))
        return g_floatSet(self, value, flags);
    NoteFloatRewrite(*knob, incoming);
    return g_floatSet(self, knob->forcedText, flags);
}

std::int64_t IntSetDetour(void* self, const wchar_t* value, std::uint32_t flags)
{
    if (g_intSet == nullptr)
        return 0;
    void* global = nullptr;
    if (self != nullptr)
        global = *reinterpret_cast<void**>(static_cast<unsigned char*>(self) + 104);
    IntKnob* knob = FindInt(global);
    double incoming = 0.0;
    if (knob == nullptr || !ParseNumber(value, incoming))
        return g_intSet(self, value, flags);
    const int asInt = static_cast<int>(incoming);
    if (!IntIsTighter(*knob, asInt))
        return g_intSet(self, value, flags);
    NoteIntRewrite(*knob, asInt);
    return g_intSet(self, knob->forcedText, flags);
}

std::int64_t OwnedSetDetour(void* self, const wchar_t* value, std::uint32_t flags)
{
    if (g_ownedSet == nullptr)
        return 0;
    OwnedKnob* knob = nullptr;
    if (IsOwned(self, g_frames))
        knob = &g_frames;
    else if (IsOwned(self, g_staticComponents))
        knob = &g_staticComponents;
    double incoming = 0.0;
    if (knob == nullptr || !ParseNumber(value, incoming) || incoming <= 0.0)
        return g_ownedSet(self, value, flags);
    NoteOwnedRewrite(*knob, static_cast<int>(incoming));
    return g_ownedSet(self, knob->forcedText, flags);
}

void ApplyOwned(OwnedKnob& knob)
{
    if (knob.applied || g_base == nullptr || g_ownedSet == nullptr)
        return;
    void* cvar = *reinterpret_cast<void**>(g_base + knob.cvarRva);
    if (cvar == nullptr)
        return;
    int original = knob.imageDefault;
    int* data = *reinterpret_cast<int**>(g_base + knob.dataRva);
    if (data != nullptr)
        original = *data;
    knob.applied = true;
    LOG("Stream %s %d -> %d", knob.name, original, knob.forced);
    g_ownedSet(cvar, knob.forcedText, kSetByCode);
}

int FramesInitDetour()
{
    const int result = g_framesInit ? g_framesInit() : 0;
    ApplyOwned(g_frames);
    return result;
}

int StaticInitDetour()
{
    const int result = g_staticInit ? g_staticInit() : 0;
    ApplyOwned(g_staticComponents);
    return result;
}

const wchar_t* RewriteIntRef(void* self, const wchar_t* value)
{
    void* global = nullptr;
    if (self != nullptr)
        global = *reinterpret_cast<void**>(static_cast<unsigned char*>(self) + 104);
    IntKnob* knob = FindInt(global);
    double incoming = 0.0;
    if (knob == nullptr || !ParseNumber(value, incoming))
        return nullptr;
    const int asInt = static_cast<int>(incoming);
    if (!IntIsTighter(*knob, asInt))
        return nullptr;
    NoteIntRewrite(*knob, asInt);
    return knob->forcedText;
}

const wchar_t* RewriteOwned(void* self, const wchar_t* value)
{
    OwnedKnob* knob = nullptr;
    if (IsOwned(self, g_frames))
        knob = &g_frames;
    else if (IsOwned(self, g_staticComponents))
        knob = &g_staticComponents;
    double incoming = 0.0;
    if (knob == nullptr || !ParseNumber(value, incoming) || incoming <= 0.0)
        return nullptr;
    NoteOwnedRewrite(*knob, static_cast<int>(incoming));
    return knob->forcedText;
}

bool HookOne(void* target, void* detour, void** original, const char* name)
{
    const MH_STATUS created = MH_CreateHook(target, detour, original);
    if (created == MH_ERROR_ALREADY_CREATED)
        return false;
    if (created != MH_OK) {
        LOG("ERROR: stream hook %s was not created", name);
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        LOG("ERROR: stream hook %s was not enabled", name);
        return false;
    }
    return true;
}

void RaiseFloat(FloatKnob& knob)
{
    float* slot = reinterpret_cast<float*>(g_base + knob.valueRva);
    const float original = *slot;
    LOG("Stream %s %g -> %g", knob.name, static_cast<double>(original), static_cast<double>(knob.forced));
    *slot = knob.forced;
    if (g_floatSet == nullptr)
        return;
    void* cvar = *reinterpret_cast<void**>(g_base + knob.cvarRva);
    if (cvar != nullptr)
        g_floatSet(cvar, knob.forcedText, kSetByCode);
}

void RaiseInt(IntKnob& knob)
{
    int* slot = reinterpret_cast<int*>(g_base + knob.valueRva);
    const int original = *slot;
    LOG("Stream %s %d -> %d", knob.name, original, knob.forced);
    *slot = knob.forced;
    if (g_intSet == nullptr)
        return;
    void* cvar = *reinterpret_cast<void**>(g_base + knob.cvarRva);
    if (cvar != nullptr)
        g_intSet(cvar, knob.forcedText, kSetByCode);
}

} // namespace

namespace features {

void Stream_Install(const wchar_t* moduleDir)
{
    wchar_t iniPath[MAX_PATH];
    if (!BuildIniPath(moduleDir, iniPath, MAX_PATH))
        return;

    // Missing key defaults to on, matching the value shipped in config.ini.
    // Enabled=0 is the only skip.
    if (GetPrivateProfileIntW(L"Stream", L"Enabled", 1, iniPath) == 0) {
        LOG("Stream budgets left at cabinet values");
        return;
    }

    const MH_STATUS initialised = MH_Initialize();
    if (initialised != MH_OK && initialised != MH_ERROR_ALREADY_INITIALIZED)
        return;

    g_base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (g_base == nullptr)
        return;

    HookOne(g_base + kFloatSetRva, reinterpret_cast<void*>(&FloatSetDetour),
            reinterpret_cast<void**>(&g_floatSet), "float set");
    if (!HookOne(g_base + kIntSetRva, reinterpret_cast<void*>(&IntSetDetour),
            reinterpret_cast<void**>(&g_intSet), "int set")) {
        // Quality already owns this Set. Calling the entry re-enters that
        // detour, which applies RewriteIntRef.
        g_intSet = reinterpret_cast<SetFn>(g_base + kIntSetRva);
        hookchain::SetIntRefRewrite(&RewriteIntRef);
        LOG("Stream int set shares the quality hook");
    }
    if (!HookOne(g_base + kOwnedSetRva, reinterpret_cast<void*>(&OwnedSetDetour),
            reinterpret_cast<void**>(&g_ownedSet), "owned int set")) {
        g_ownedSet = reinterpret_cast<SetFn>(g_base + kOwnedSetRva);
        hookchain::SetOwnedIntRewrite(&RewriteOwned);
        LOG("Stream owned int set shares the quality hook");
    }

    for (FloatKnob& knob : g_floats)
        RaiseFloat(knob);
    for (IntKnob& knob : g_ints)
        RaiseInt(knob);

    // Owned values live inside the cvar. Hook registration so the first
    // constructor value is replaced before a frame runs, and apply now when
    // this DLL is attached after those constructors.
    if (g_ownedSet != nullptr) {
        HookOne(g_base + kFramesInitRva, reinterpret_cast<void*>(&FramesInitDetour),
                reinterpret_cast<void**>(&g_framesInit), "frames init");
        HookOne(g_base + kStaticInitRva, reinterpret_cast<void*>(&StaticInitDetour),
                reinterpret_cast<void**>(&g_staticInit), "static components init");
        ApplyOwned(g_frames);
        ApplyOwned(g_staticComponents);
    }

    LOG("Stream set hooks RVA 0x%X 0x%X 0x%X",
        static_cast<unsigned>(kFloatSetRva),
        static_cast<unsigned>(kIntSetRva),
        static_cast<unsigned>(kOwnedSetRva));
}

} // namespace features
