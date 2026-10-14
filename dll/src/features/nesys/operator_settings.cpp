#include "features/nesys/operator_settings.h"

#include <MinHook.h>

#include "log.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

// TG4AC 5.80.02 (image base 0x140000000). Settings that keep NESYS and the
// NESiCA reader off on a keyboard setup:
//
//   WholeGameConfig (Config/DefaultGame.ini, [/Script/TG4AC.WholeGameConfig])
//     DisableNESYS=True: the NesysManager constructor (sub_14020BE90) reads the
//       class default at +0x58 and stores state 2, so NesysManager_Update never
//       runs NESYS_INIT. AH_NESYS_DISABLE in the environment does the same.
//     UseKeyboardDeviceWithCardReader stays as the ini sets it (False). True
//       makes the keyboard setup check the cabinet USB-I/O, which is absent,
//       and the game stops on the I/O device error screen. NESiCA then goes
//       through KeyboardInput (card.cpp).
//   HardSaveGame_C (operator test-menu save, MyGameInstance_C.HardSave)
//     IsNesysUsed: MyGameInstance_C.IsUsedNesys reads it.
//     IsUsedNESiCATime: limits NESiCA to the NESiCATime_<weekday> windows.
//
// The constructor result is corrected in a hook. The bools are written on the
// class default objects and instances: as CreateSaveGameObject (sub_1412C2440)
// or LoadGameFromSlot (sub_1412D0600) returns a save, and on a kPollMs rescan
// of GUObjectArray, so a later write by the blueprints does not stick.
//
// The bools are found through reflection, UE 4.15 layout read from this exe:
//   UObject: class +0x10, name +0x18
//   UField::Next +0x28; UStruct::SuperStruct +0x30, Children +0x38
//   UProperty (ctor sub_1405DD5B0): ElementSize +0x34, Offset_Internal +0x50
//   UBoolProperty: ByteOffset +0x79, ByteMask +0x7A, FieldMask +0x7B
//   GUObjectArray (RVA 0x2F13580): items +0x10, count +0x1C, 24-byte items
//   FName lookup sub_14052EC80 (find mode 0 returns index 0 when absent)

namespace {

constexpr std::uintptr_t kGuObjectArrayRva = 0x2F13580;
constexpr std::uintptr_t kMakeNameRva = 0x52EC80;
constexpr std::uintptr_t kCreateSaveRva = 0x12C2440;     // sub_1412C2440
constexpr std::uintptr_t kLoadSaveRva = 0x12D0600;       // sub_1412D0600
constexpr std::uintptr_t kNesysManagerCtorRva = 0x20BE90; // sub_14020BE90

const unsigned char kCreateSavePrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08};
const unsigned char kLoadSavePrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57};
const unsigned char kNesysManagerCtorPrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C,
                                                   0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57};

constexpr std::size_t kGuItems = 0x10;
constexpr std::size_t kGuCount = 0x1C;
constexpr std::size_t kGuItemBytes = 24;
constexpr int kMaxObjects = 5000000;

constexpr std::size_t kObjectClass = 0x10;
constexpr std::size_t kObjectName = 0x18;
constexpr std::size_t kFieldNext = 0x28;
constexpr std::size_t kStructSuper = 0x30;
constexpr std::size_t kStructChildren = 0x38;
constexpr std::size_t kPropertyElementSize = 0x34;
constexpr std::size_t kPropertyOffset = 0x50;
constexpr std::size_t kBoolByteOffset = 0x79;
constexpr std::size_t kBoolByteMask = 0x7A;
constexpr std::size_t kBoolFieldMask = 0x7B;

// NesysManager +0: 0 runs NESYS_INIT on the next update, 2 is disabled.
constexpr std::int32_t kNesysStateInit = 0;
constexpr std::int32_t kNesysStateDisabled = 2;

constexpr int kFindName = 0;
constexpr int kMaxChildren = 4096;
constexpr int kMaxSuperDepth = 16;
constexpr int kMaxPatchLogs = 16;
constexpr DWORD kPollMs = 200;

struct FName {
    std::uint32_t index;
    std::uint32_t number;
};

using MakeNameFn = FName*(__fastcall*)(FName* out, const wchar_t* text, int findType);
using CreateSaveFn = void*(__fastcall*)(void* saveClass);
using LoadSaveFn = void*(__fastcall*)(void* slotName, int userIndex);
using NesysManagerCtorFn = void*(__fastcall*)(void* self);

struct BoolField {
    std::int32_t offset;
    std::uint8_t byteOffset;
    std::uint8_t byteMask;
    std::uint8_t fieldMask;
};

struct Setting {
    const wchar_t* className;
    const wchar_t* fieldName;
    bool wanted;
    std::uint32_t classIndex;
    std::uint32_t fieldIndex;
    const void* resolvedClass;
    BoolField field;
    bool valid;
};

Setting g_settings[] = {
    {L"WholeGameConfig", L"DisableNESYS", false, 0, 0, nullptr, {}, false},
    {L"HardSaveGame_C", L"IsNesysUsed", true, 0, 0, nullptr, {}, false},
    {L"HardSaveGame_C", L"IsUsedNESiCATime", false, 0, 0, nullptr, {}, false},
};

unsigned char* g_image = nullptr;
MakeNameFn g_makeName = nullptr;
CreateSaveFn g_originalCreate = nullptr;
LoadSaveFn g_originalLoad = nullptr;
NesysManagerCtorFn g_originalNesysCtor = nullptr;

// Guards the settings table and the log counter. The hooks run on the game
// thread, the scan on its own.
SRWLOCK g_lock = SRWLOCK_INIT;
std::uint32_t g_boolPropertyName = 0;
int g_patchLogs = 0;

template <typename T>
T Read(const void* base, std::size_t offset)
{
    T value{};
    std::memcpy(&value, static_cast<const unsigned char*>(base) + offset, sizeof(T));
    return value;
}

std::uint32_t FindName(const wchar_t* text)
{
    FName name{};
    g_makeName(&name, text, kFindName);
    return name.index;
}

std::uint32_t NameOf(const void* object)
{
    return Read<std::uint32_t>(object, kObjectName);
}

// Class names appear once their packages load; retried on every pass.
void ResolveNames()
{
    if (g_boolPropertyName == 0)
        g_boolPropertyName = FindName(L"BoolProperty");
    for (Setting& setting : g_settings) {
        if (setting.classIndex == 0)
            setting.classIndex = FindName(setting.className);
        if (setting.fieldIndex == 0)
            setting.fieldIndex = FindName(setting.fieldName);
    }
}

// Walks the class and its supers for a BoolProperty with this name.
bool FindBool(const void* type, std::uint32_t name, BoolField& out)
{
    for (int depth = 0; type != nullptr && depth < kMaxSuperDepth; ++depth) {
        const void* field = Read<const void*>(type, kStructChildren);
        for (int step = 0; field != nullptr && step < kMaxChildren; ++step) {
            const void* fieldClass = Read<const void*>(field, kObjectClass);
            if (NameOf(field) == name && fieldClass != nullptr && NameOf(fieldClass) == g_boolPropertyName) {
                out.offset = Read<std::int32_t>(field, kPropertyOffset);
                const std::int32_t size = Read<std::int32_t>(field, kPropertyElementSize);
                out.byteOffset = Read<std::uint8_t>(field, kBoolByteOffset);
                out.byteMask = Read<std::uint8_t>(field, kBoolByteMask);
                out.fieldMask = Read<std::uint8_t>(field, kBoolFieldMask);
                const bool valid = out.offset > 0 && size >= 1 && size <= 8 && out.byteOffset < size
                    && out.byteMask != 0 && (out.byteMask & out.fieldMask) == out.byteMask;
                if (!valid)
                    LOG("ERROR: bool layout off: offset %d size %d byte %u mask %02X/%02X", out.offset, size,
                        out.byteOffset, out.byteMask, out.fieldMask);
                return valid;
            }
            field = Read<const void*>(field, kFieldNext);
        }
        type = Read<const void*>(type, kStructSuper);
    }
    return false;
}

bool ResolveField(Setting& setting, const void* type)
{
    if (type == setting.resolvedClass)
        return setting.valid;
    setting.resolvedClass = type;
    setting.valid = setting.fieldIndex != 0 && g_boolPropertyName != 0
        && FindBool(type, setting.fieldIndex, setting.field);
    if (setting.valid)
        LOG("NESYS: %ls.%ls at +0x%X", setting.className, setting.fieldName, setting.field.offset);
    else
        LOG("ERROR: %ls has no BoolProperty %ls", setting.className, setting.fieldName);
    return setting.valid;
}

void PatchObject(void* object, const char* source)
{
    if (object == nullptr)
        return;
    const void* type = Read<const void*>(object, kObjectClass);
    if (type == nullptr)
        return;
    const std::uint32_t typeName = NameOf(type);
    for (Setting& setting : g_settings) {
        if (setting.classIndex == 0 || typeName != setting.classIndex || !ResolveField(setting, type))
            continue;
        auto* slot = static_cast<unsigned char*>(object) + setting.field.offset + setting.field.byteOffset;
        const unsigned char current = *slot;
        const auto wanted =
            static_cast<unsigned char>((current & ~setting.field.fieldMask) | (setting.wanted ? setting.field.byteMask : 0));
        if (current == wanted)
            continue;
        *slot = wanted;
        if (g_patchLogs < kMaxPatchLogs) {
            ++g_patchLogs;
            LOG("NESYS: %s %ls %p: %ls %d -> %d", source, setting.className, object, setting.fieldName,
                (current & setting.field.fieldMask) != 0, setting.wanted);
        }
    }
}

// SEH frames only: a garbage-collected object read mid-scan faults here and
// the next pass retries.
void TryPatchObject(void* object, const char* source)
{
    __try {
        PatchObject(object, source);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void TryScanObjects()
{
    __try {
        const unsigned char* array = g_image + kGuObjectArrayRva;
        const auto* items = Read<const unsigned char*>(array, kGuItems);
        const int count = Read<int>(array, kGuCount);
        if (items == nullptr || count <= 0 || count > kMaxObjects)
            return;
        for (int i = 0; i < count; ++i) {
            void* object = Read<void*>(items + static_cast<std::size_t>(i) * kGuItemBytes, 0);
            if (object != nullptr)
                PatchObject(object, "scan");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void PatchLocked(void* object, const char* source)
{
    AcquireSRWLockExclusive(&g_lock);
    ResolveNames();
    TryPatchObject(object, source);
    ReleaseSRWLockExclusive(&g_lock);
}

void* __fastcall CreateSaveDetour(void* saveClass)
{
    void* object = g_originalCreate(saveClass);
    PatchLocked(object, "CreateSaveGameObject");
    return object;
}

void* __fastcall LoadSaveDetour(void* slotName, int userIndex)
{
    void* object = g_originalLoad(slotName, userIndex);
    PatchLocked(object, "LoadGameFromSlot");
    return object;
}

void* __fastcall NesysManagerCtorDetour(void* self)
{
    void* result = g_originalNesysCtor(self);
    auto* state = static_cast<std::int32_t*>(self);
    if (*state == kNesysStateDisabled) {
        *state = kNesysStateInit;
        LOG("NESYS: DisableNESYS / AH_NESYS_DISABLE overridden; NESYS_INIT runs on the next update");
    }
    return result;
}

DWORD WINAPI KeeperThread(LPVOID)
{
    for (;;) {
        Sleep(kPollMs);
        AcquireSRWLockExclusive(&g_lock);
        ResolveNames();
        TryScanObjects();
        ReleaseSRWLockExclusive(&g_lock);
    }
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

bool StartOperatorSettings()
{
    g_image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (g_image == nullptr)
        return false;
    g_makeName = reinterpret_cast<MakeNameFn>(g_image + kMakeNameRva);

    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
        return false;
    if (!Hook(kNesysManagerCtorRva, kNesysManagerCtorPrologue, sizeof(kNesysManagerCtorPrologue),
              reinterpret_cast<void*>(&NesysManagerCtorDetour), reinterpret_cast<void**>(&g_originalNesysCtor),
              "NesysManager constructor")
        || !Hook(kCreateSaveRva, kCreateSavePrologue, sizeof(kCreateSavePrologue),
                 reinterpret_cast<void*>(&CreateSaveDetour), reinterpret_cast<void**>(&g_originalCreate),
                 "CreateSaveGameObject")
        || !Hook(kLoadSaveRva, kLoadSavePrologue, sizeof(kLoadSavePrologue),
                 reinterpret_cast<void*>(&LoadSaveDetour), reinterpret_cast<void**>(&g_originalLoad),
                 "LoadGameFromSlot"))
        return false;

    const HANDLE thread = CreateThread(nullptr, 0, &KeeperThread, nullptr, 0, nullptr);
    if (thread == nullptr) {
        LOG("ERROR: operator settings thread was not created (%lu)", GetLastError());
        return false;
    }
    CloseHandle(thread);
    return true;
}

} // namespace nesys
