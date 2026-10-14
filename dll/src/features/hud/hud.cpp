#include "features/module_api.h"
#include "features/hud/hud.h"
#include "features/hook_chain.h"

#include <MinHook.h>

#include "log.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <cwchar>
#include <unordered_set>
#include <vector>

// Full driving-HUD hide for TG4AC 5.80.02 (UE 4.15).
//
// AllHudVisiblityDelegate only switches HudVisibleType. AllHidden leaves
// FadeTree, the per-screen now-loading widget, and the touch-panel root
// (tpHUD_root) up. This feature calls UWidget::SetVisibility with
// ESlateVisibility::Hidden (2), the same value those widgets already use
// when they hide something, on each screen widget and on every widget in
// its tree, plus live fade / now-loading / touch-root instances.
//
// Image base in the IDA database is 0x140000000. Runtime addresses are
// module base + RVA.

namespace {

constexpr std::uintptr_t kUpdateCameraRva = 0x14BD5B0;   // APlayerCameraManager::UpdateCamera
constexpr std::uintptr_t kSetVisibilityRva = 0xD8B520;   // UWidget::SetVisibility
constexpr std::uintptr_t kObjectArrayRva = 0x2F13580;    // FUObjectArray GUObjectArray
constexpr std::uintptr_t kNameCtorRva = 0x52EC80;        // FName::FName(const wchar_t*, EFindName)
constexpr std::uintptr_t kNameToStringRva = 0x533220;    // FName::ToString(FString&) const

constexpr unsigned kObjectFlags = 0x8;
constexpr unsigned kInternalIndex = 0xC;
constexpr unsigned kClass = 0x10;
constexpr unsigned kName = 0x18;
constexpr unsigned kOuter = 0x20;
constexpr unsigned kSuperStruct = 0x30;
constexpr unsigned kVisibility = 0x91;
constexpr unsigned kWidgetTree = 0x190;
constexpr unsigned kRootWidget = 0x28;
constexpr unsigned kSlots = 0x118;
constexpr unsigned kSlotParent = 0x28;
constexpr unsigned kSlotContent = 0x30;

// FUObjectArray in this exe is the flat table, not the chunked one.
// AllocateSerialNumber (RVA 0x600B90) bounds the index with the dword at
// +0x1C and addresses an item as Objects + 24 * index. +0x18 is MaxElements.
// A chunk count does not exist; +0x20 is the critical section.
constexpr unsigned kObjects = 0x10;
constexpr unsigned kMaxElements = 0x18;
constexpr unsigned kNumElements = 0x1C;
constexpr int kItemStride = 24;
constexpr unsigned kItemSerial = 0x10;

constexpr std::uint32_t kClassDefaultObject = 0x00000010;
constexpr std::uint32_t kArchetypeObject = 0x00000020;
constexpr std::uint32_t kBeginDestroyed = 0x00008000;
constexpr std::uint32_t kFinishDestroyed = 0x00010000;

constexpr unsigned char kHidden = 2; // ESlateVisibility::Hidden

constexpr unsigned char kUpdatePrologue[] = {
    0x40, 0x53, 0x48, 0x83, 0xEC, 0x60, 0x48, 0x8B, 0xD9, 0x0F, 0x29, 0x74, 0x24, 0x50
};
constexpr unsigned char kVisibilityPrologue[] = {
    0x40, 0x53, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00, 0x0F, 0xB6, 0xDA, 0x88, 0x91, 0x91, 0x00, 0x00, 0x00
};
constexpr unsigned char kNameCtorPrologue[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xD9};
constexpr unsigned char kNameToStringPrologue[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x33, 0xC0};

struct FName {
    std::int32_t index;
    std::int32_t number;
};

struct FStringOut {
    wchar_t* data;
    int count;
    int max;
};

struct SavedWidget {
    void* widget;
    std::int32_t index;
    std::int32_t serial;
    unsigned char visibility;
};

struct TargetClass {
    const wchar_t* name;
    std::int32_t index;
};

using UpdateCameraFn = std::uintptr_t(__fastcall*)(void* cameraManager, float deltaSeconds);
using SetVisibilityFn = void(__fastcall*)(void* widget, unsigned char visibility);
using NameCtorFn = void(__fastcall*)(FName* self, const wchar_t* text, int findType);
using NameToStringFn = void(__fastcall*)(const FName* self, FStringOut* out);

UpdateCameraFn g_originalUpdate = nullptr;
SetVisibilityFn g_originalVisibility = nullptr;
NameCtorFn g_nameCtor = nullptr;
NameToStringFn g_nameToString = nullptr;

std::uintptr_t g_module = 0;
std::uintptr_t g_moduleEnd = 0;
void* g_objects = nullptr;
int g_stride = 0;
unsigned char* g_items = nullptr;
int g_objectCount = 0;
unsigned long long g_nextHideScan = 0;

bool g_ready = false;
bool g_hide = false;
bool g_inApply = false;
bool g_applyAgain = false;
bool g_namesBroken = false;
bool g_userWidgetVerified = false;
bool g_objectsBroken = false;
bool g_loggedHide = false;
bool g_loggedObjects = false;

std::int32_t g_userWidget = 0;
std::int32_t g_panelWidget = 0;
std::int32_t g_widgetBlueprintClass = 0;
std::int32_t g_blueprintClass = 0;
std::int32_t g_classClass = 0;

TargetClass g_targets[] = {
    {L"CenterScreenHUD_C", 0},
    {L"LeftScreenHUD_C", 0},
    {L"RightScreenHUD_C", 0},
    {L"TouchPanelHUD_C", 0},
    {L"Fade_C", 0},
    {L"NowLoading_Center_C", 0},
    {L"loading_Left_C", 0},
    {L"loading_Right_C", 0},
    {L"InGameNowLoadingBottom_C", 0},
    {L"NowLoadingBottom_C", 0},
    {L"NowLoadingWord_C", 0},
    {L"tpHUD_root_C", 0},
};

std::vector<SavedWidget> g_saved;
std::unordered_set<void*> g_savedSet;
std::unordered_set<void*> g_visited;

bool HasPrefix(const unsigned char* bytes, const unsigned char* prefix, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i) {
        if (bytes[i] != prefix[i])
            return false;
    }
    return true;
}

bool InModule(std::uintptr_t address)
{
    return address >= g_module && address < g_moduleEnd;
}

bool Aligned(const void* pointer)
{
    const auto value = reinterpret_cast<std::uintptr_t>(pointer);
    return value > 0x10000 && (value & 7u) == 0;
}

bool Readable(const void* pointer, std::size_t bytes)
{
    if (pointer == nullptr || bytes == 0)
        return false;
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(pointer, &info, sizeof(info)) == 0)
        return false;
    if (info.State != MEM_COMMIT)
        return false;
    if ((info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
        return false;
    const auto start = reinterpret_cast<std::uintptr_t>(pointer);
    const auto regionEnd = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    return start + bytes <= regionEnd;
}

bool LooksLikeObject(void* object)
{
    if (!Aligned(object))
        return false;
    const auto vtable = *reinterpret_cast<std::uintptr_t*>(object);
    return InModule(vtable);
}

void* ClassOf(void* object)
{
    if (!LooksLikeObject(object))
        return nullptr;
    void* type = *reinterpret_cast<void**>(static_cast<unsigned char*>(object) + kClass);
    if (!LooksLikeObject(type))
        return nullptr;
    return type;
}

FName NameOf(void* object)
{
    FName name{0, 0};
    if (!LooksLikeObject(object))
        return name;
    std::memcpy(&name, static_cast<unsigned char*>(object) + kName, sizeof(name));
    return name;
}

void* OuterOf(void* object)
{
    if (!LooksLikeObject(object))
        return nullptr;
    return *reinterpret_cast<void**>(static_cast<unsigned char*>(object) + kOuter);
}

std::uint32_t FlagsOf(void* object)
{
    if (!LooksLikeObject(object))
        return kFinishDestroyed;
    return *reinterpret_cast<std::uint32_t*>(static_cast<unsigned char*>(object) + kObjectFlags);
}

std::int32_t IndexOf(void* object)
{
    if (!LooksLikeObject(object))
        return -1;
    return *reinterpret_cast<std::int32_t*>(static_cast<unsigned char*>(object) + kInternalIndex);
}

unsigned char VisibilityOf(void* widget)
{
    if (!LooksLikeObject(widget))
        return 0xFF;
    return static_cast<unsigned char*>(widget)[kVisibility];
}

bool SpanReadable(const void* pointer, std::size_t bytes)
{
    if (pointer == nullptr || bytes == 0)
        return false;
    auto cursor = reinterpret_cast<std::uintptr_t>(pointer);
    const auto end = cursor + bytes;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) == 0)
            return false;
        if (info.State != MEM_COMMIT)
            return false;
        if ((info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
            return false;
        const auto regionEnd = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (regionEnd <= cursor)
            return false;
        cursor = regionEnd;
    }
    return true;
}

bool ObjectCounts(int* outCount, unsigned char** outItems)
{
    if (!Readable(g_objects, kNumElements + sizeof(std::int32_t)))
        return false;
    const auto base = static_cast<unsigned char*>(g_objects);
    const int count = *reinterpret_cast<std::int32_t*>(base + kNumElements);
    const int capacity = *reinterpret_cast<std::int32_t*>(base + kMaxElements);
    auto* items = *reinterpret_cast<unsigned char**>(base + kObjects);
    if (count <= 0 || count > 5000000 || capacity < count || items == nullptr)
        return false;
    if (!SpanReadable(items, static_cast<std::size_t>(count) * static_cast<std::size_t>(kItemStride)))
        return false;
    *outCount = count;
    *outItems = items;
    return true;
}

bool RefreshView()
{
    g_items = nullptr;
    g_objectCount = 0;
    int count = 0;
    unsigned char* items = nullptr;
    if (g_stride == 0 || !ObjectCounts(&count, &items))
        return false;
    g_items = items;
    g_objectCount = count;
    return true;
}

void* ObjectAt(std::int32_t index, std::int32_t* serial)
{
    if (serial != nullptr)
        *serial = 0;
    if (index < 0)
        return nullptr;
    if (g_items == nullptr || index >= g_objectCount) {
        if (!RefreshView() || index >= g_objectCount)
            return nullptr;
    }
    auto* item = g_items + static_cast<std::size_t>(index) * static_cast<std::size_t>(g_stride);
    if (serial != nullptr && g_stride >= static_cast<int>(kItemSerial + sizeof(std::int32_t)))
        *serial = *reinterpret_cast<std::int32_t*>(item + kItemSerial);
    return *reinterpret_cast<void**>(item);
}

int ScoreFlat()
{
    int count = 0;
    unsigned char* items = nullptr;
    if (!ObjectCounts(&count, &items))
        return 0;
    const int limit = count < 64 ? count : 64;
    int score = 0;
    for (int i = 0; i < limit; ++i) {
        void* object = *reinterpret_cast<void**>(items + static_cast<std::size_t>(i) * kItemStride);
        if (!LooksLikeObject(object))
            continue;
        const auto index = *reinterpret_cast<std::int32_t*>(static_cast<unsigned char*>(object) + kInternalIndex);
        if (index == i)
            ++score;
    }
    return score;
}

bool EnsureObjects()
{
    if (g_objectsBroken)
        return false;
    if (g_stride != 0)
        return true;
    int count = 0;
    unsigned char* items = nullptr;
    if (!ObjectCounts(&count, &items))
        return false;
    if (ScoreFlat() >= 8)
        g_stride = kItemStride;
    else if (count > 10000) {
        if (!g_loggedObjects) {
            LOG("ERROR: HUD object array layout was not recognized (count %d)", count);
            g_loggedObjects = true;
        }
        g_objectsBroken = true;
        return false;
    } else {
        return false;
    }
    LOG("HUD object array flat stride %d count %d", g_stride, count);
    return true;
}

bool ResolveName(const wchar_t* text, std::int32_t* outIndex)
{
    if (g_nameCtor == nullptr || text == nullptr || outIndex == nullptr)
        return false;
    FName name{0, 0};
    g_nameCtor(&name, text, 0);
    if (name.index <= 0 || name.number != 0)
        return false;
    *outIndex = name.index;
    return true;
}

int NamesMatchEngine()
{
    std::int32_t index = 0;
    if (!ResolveName(L"UserWidget", &index))
        return 0;
    if (g_nameToString == nullptr)
        return 0;
    const FName name{index, 0};
    FStringOut out{nullptr, 0, 0};
    g_nameToString(&name, &out);
    if (out.data == nullptr)
        return 0;
    // One engine allocation. A mismatch must not retry, or this leaks every frame.
    if (wcscmp(out.data, L"UserWidget") != 0)
        return -1;
    return 1;
}

int TryPrepareNames()
{
    __try {
        return NamesMatchEngine();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool EnsureNames()
{
    if (g_namesBroken)
        return false;
    if (!g_userWidgetVerified) {
        const int verified = TryPrepareNames();
        if (verified < 0) {
            g_namesBroken = true;
            LOG("ERROR: HUD name lookup failed");
            return false;
        }
        if (verified == 0)
            return false;
        g_userWidgetVerified = true;
    }

    if (g_userWidget == 0 && !ResolveName(L"UserWidget", &g_userWidget))
        return false;
    if (g_panelWidget == 0 && !ResolveName(L"PanelWidget", &g_panelWidget))
        return false;
    if (g_widgetBlueprintClass == 0 && !ResolveName(L"WidgetBlueprintGeneratedClass", &g_widgetBlueprintClass))
        return false;
    if (g_blueprintClass == 0 && !ResolveName(L"BlueprintGeneratedClass", &g_blueprintClass))
        return false;
    if (g_classClass == 0 && !ResolveName(L"Class", &g_classClass))
        return false;

    // HUD classes are absent until their packages load. Missing names stay 0
    // and are tried again on a later frame.
    for (TargetClass& target : g_targets) {
        if (target.index == 0)
            ResolveName(target.name, &target.index);
    }
    return true;
}

bool IsTargetIndex(std::int32_t index)
{
    if (index <= 0)
        return false;
    for (const TargetClass& target : g_targets) {
        if (target.index == index)
            return true;
    }
    return false;
}

bool Extends(void* type, std::int32_t wanted)
{
    if (wanted == 0)
        return false;
    for (int depth = 0; depth < 32 && type != nullptr; ++depth) {
        if (!LooksLikeObject(type))
            return false;
        const FName name = NameOf(type);
        if (name.index == wanted && name.number == 0)
            return true;
        void* super = *reinterpret_cast<void**>(static_cast<unsigned char*>(type) + kSuperStruct);
        if (super == type)
            return false;
        type = super;
    }
    return false;
}

bool OwnedByAsset(void* object)
{
    void* outer = OuterOf(object);
    for (int depth = 0; depth < 16 && outer != nullptr; ++depth) {
        void* type = ClassOf(outer);
        const FName name = NameOf(type);
        if (name.number == 0 &&
            (name.index == g_widgetBlueprintClass || name.index == g_blueprintClass || name.index == g_classClass))
            return true;
        outer = OuterOf(outer);
    }
    return false;
}

bool LiveInstance(void* object)
{
    if (!LooksLikeObject(object))
        return false;
    const std::uint32_t flags = FlagsOf(object);
    if ((flags & (kClassDefaultObject | kArchetypeObject | kBeginDestroyed | kFinishDestroyed)) != 0)
        return false;
    return !OwnedByAsset(object);
}

bool IsDirectTarget(void* object)
{
    void* type = ClassOf(object);
    if (type == nullptr)
        return false;
    const FName name = NameOf(type);
    const bool named = name.number == 0 && IsTargetIndex(name.index);
    // Every live user widget, not only the named screen roots. A screen HUD
    // that is not in the name list would otherwise stay on screen.
    if (!named && !Extends(type, g_userWidget))
        return false;
    return LiveInstance(object);
}

bool StillSaved(const SavedWidget& saved)
{
    std::int32_t serial = 0;
    void* object = ObjectAt(saved.index, &serial);
    if (object != saved.widget)
        return false;
    if (g_stride >= static_cast<int>(kItemSerial + sizeof(std::int32_t)) && serial != saved.serial)
        return false;
    return LiveInstance(object);
}

bool Remember(void* widget)
{
    if (widget == nullptr)
        return false;
    if (g_savedSet.find(widget) != g_savedSet.end())
        return true;
    const unsigned char visibility = VisibilityOf(widget);
    if (visibility > 4)
        return false;
    const std::int32_t index = IndexOf(widget);
    std::int32_t serial = 0;
    if (ObjectAt(index, &serial) != widget)
        return false;
    g_saved.push_back({widget, index, serial, visibility});
    g_savedSet.insert(widget);
    return true;
}

void HideWidget(void* widget)
{
    if (!LiveInstance(widget) || g_originalVisibility == nullptr)
        return;
    const bool known = g_savedSet.find(widget) != g_savedSet.end();
    if (!Remember(widget))
        return;
    // The first hide always calls through, so Slate matches the byte even
    // when the widget was already marked Hidden. Later calls only fire when
    // something put it back on screen.
    if (!known || VisibilityOf(widget) != kHidden)
        g_originalVisibility(widget, kHidden);
}

void HideTree(void* root)
{
    void* stack[2048];
    int count = 0;
    if (root == nullptr)
        return;
    stack[count++] = root;
    while (count > 0 && g_hide) {
        void* widget = stack[--count];
        if (!g_visited.insert(widget).second)
            continue;
        if (!LiveInstance(widget))
            continue;
        HideWidget(widget);

        void* type = ClassOf(widget);
        if (Extends(type, g_userWidget)) {
            if (Readable(widget, kWidgetTree + sizeof(void*))) {
                void* tree = *reinterpret_cast<void**>(static_cast<unsigned char*>(widget) + kWidgetTree);
                if (Readable(tree, kRootWidget + sizeof(void*)) && OuterOf(tree) == widget) {
                    void* child = *reinterpret_cast<void**>(static_cast<unsigned char*>(tree) + kRootWidget);
                    if (child != nullptr && count < 2048)
                        stack[count++] = child;
                }
            }
        }

        if (!Extends(type, g_panelWidget) || !Readable(widget, kSlots + 16))
            continue;
        auto* data = *reinterpret_cast<void***>(static_cast<unsigned char*>(widget) + kSlots);
        const int slots = *reinterpret_cast<std::int32_t*>(static_cast<unsigned char*>(widget) + kSlots + 8);
        if (data == nullptr || slots <= 0 || slots > 256)
            continue;
        if (!Readable(data, static_cast<std::size_t>(slots) * sizeof(void*)))
            continue;
        for (int i = 0; i < slots && count < 2048; ++i) {
            void* slot = data[i];
            if (!Readable(slot, kSlotContent + sizeof(void*)))
                continue;
            void* parent = *reinterpret_cast<void**>(static_cast<unsigned char*>(slot) + kSlotParent);
            if (parent != widget) {
                if (i == 0)
                    break;
                continue;
            }
            void* content = *reinterpret_cast<void**>(static_cast<unsigned char*>(slot) + kSlotContent);
            if (content != nullptr)
                stack[count++] = content;
        }
    }
}

void HideAll()
{
    if (!EnsureObjects() || !RefreshView() || !EnsureNames() || g_originalVisibility == nullptr)
        return;
    const unsigned long long now = GetTickCount64();
    if (now < g_nextHideScan && !g_saved.empty())
        return;
    g_nextHideScan = now + 500;

    g_visited.clear();
    for (int index = 0; index < g_objectCount && g_hide; ++index) {
        void* object = ObjectAt(index, nullptr);
        if (object == nullptr || !IsDirectTarget(object))
            continue;
        HideTree(object);
    }

    if (!g_loggedHide && !g_saved.empty()) {
        LOG("HUD hidden (%zu widgets)", g_saved.size());
        g_loggedHide = true;
    } else if (!g_loggedHide && g_saved.empty() && !g_loggedObjects) {
        LOG("HUD hide found no widgets (objects %d)", g_objectCount);
        g_loggedObjects = true;
    }
}

void Restore()
{
    if (g_hide)
        return;
    if (g_originalVisibility == nullptr) {
        g_saved.clear();
        g_savedSet.clear();
        return;
    }
    if (!RefreshView())
        return;
    const bool had = !g_saved.empty();
    for (const SavedWidget& saved : g_saved) {
        if (g_hide)
            return;
        if (StillSaved(saved))
            g_originalVisibility(saved.widget, saved.visibility);
    }
    if (g_hide)
        return;
    g_saved.clear();
    g_savedSet.clear();
    g_visited.clear();
    if (had && g_loggedHide) {
        LOG("HUD restored");
        g_loggedHide = false;
    }
}

void ApplyBody()
{
    if (g_hide)
        HideAll();
    if (!g_hide)
        Restore();
}

int ApplyBodyGuarded()
{
    __try {
        ApplyBody();
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void Apply()
{
    if (!g_ready || g_inApply) {
        g_applyAgain = true;
        return;
    }
    for (int pass = 0; pass < 3; ++pass) {
        g_applyAgain = false;
        g_inApply = true;
        if (ApplyBodyGuarded() == 0) {
            g_objectsBroken = true;
            g_saved.clear();
            g_savedSet.clear();
            g_visited.clear();
            LOG("ERROR: HUD hide faulted");
            g_inApply = false;
            return;
        }
        g_inApply = false;
        if (!g_applyAgain)
            break;
    }
}

std::uintptr_t __fastcall UpdateCameraDetour(void* cameraManager, float deltaSeconds)
{
    const std::uintptr_t result = g_originalUpdate != nullptr
        ? g_originalUpdate(cameraManager, deltaSeconds)
        : 0;
    Apply();
    hookchain::RunCameraTick();
    return result;
}

void __fastcall SetVisibilityDetour(void* widget, unsigned char visibility)
{
    if (g_hide && widget != nullptr &&
        (g_savedSet.find(widget) != g_savedSet.end() || IsDirectTarget(widget))) {
        const bool known = g_savedSet.find(widget) != g_savedSet.end();
        if (Remember(widget)) {
            if (known && VisibilityOf(widget) == kHidden)
                return;
            visibility = kHidden;
        }
    }
    if (g_originalVisibility != nullptr)
        g_originalVisibility(widget, visibility);
}

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

bool InstallHooks()
{
    HMODULE game = GetModuleHandleW(L"TG4AC-Win64-Shipping.exe");
    if (game == nullptr)
        game = GetModuleHandleW(nullptr);
    if (game == nullptr)
        return false;

    auto* base = reinterpret_cast<unsigned char*>(game);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;

    g_module = reinterpret_cast<std::uintptr_t>(base);
    g_moduleEnd = g_module + nt->OptionalHeader.SizeOfImage;
    g_objects = base + kObjectArrayRva;

    auto* update = base + kUpdateCameraRva;
    auto* visibility = base + kSetVisibilityRva;
    auto* nameCtor = base + kNameCtorRva;
    auto* nameToString = base + kNameToStringRva;
    if (!HasPrefix(update, kUpdatePrologue, sizeof(kUpdatePrologue)) ||
        !HasPrefix(visibility, kVisibilityPrologue, sizeof(kVisibilityPrologue)) ||
        !HasPrefix(nameCtor, kNameCtorPrologue, sizeof(kNameCtorPrologue)) ||
        !HasPrefix(nameToString, kNameToStringPrologue, sizeof(kNameToStringPrologue)))
        return false;

    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
        return false;

    g_nameCtor = reinterpret_cast<NameCtorFn>(nameCtor);
    g_nameToString = reinterpret_cast<NameToStringFn>(nameToString);
    if (MH_CreateHook(update, &UpdateCameraDetour, reinterpret_cast<void**>(&g_originalUpdate)) != MH_OK)
        return false;
    if (MH_CreateHook(visibility, &SetVisibilityDetour, reinterpret_cast<void**>(&g_originalVisibility)) != MH_OK)
        return false;
    if (MH_EnableHook(update) != MH_OK || MH_EnableHook(visibility) != MH_OK)
        return false;
    return true;
}

} // namespace

namespace features {

void Hud_Install(const wchar_t* moduleDir)
{
    wchar_t iniPath[MAX_PATH];
    if (!BuildIniPath(moduleDir, iniPath, MAX_PATH))
        return;
    if (GetPrivateProfileIntW(L"Hud", L"Enabled", 1, iniPath) == 0) {
        LOG("HUD hide is off");
        return;
    }
    if (!InstallHooks()) {
        LOG("ERROR: HUD hide was not installed");
        return;
    }
    g_saved.reserve(512);
    g_ready = true;
    LOG("HUD hide installed");
}

} // namespace features

void Hud_SetHidden(bool hidden)
{
    if (!g_ready)
        return;
    g_hide = hidden;
    Apply();
}
