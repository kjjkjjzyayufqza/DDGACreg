#include "features/module_api.h"

#include <MinHook.h>

#include "log.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>

// Keyboard NESiCA reader for TG4AC 5.80.02 (image base 0x140000000 in IDA;
// hooks use the relocated module base plus RVA).
//
// MyGameInstance picks the input device per call: MasterController,
// HornButtonAtKeyboard, StartReadNESiCA, NESiCAReadState and GetNESiCAID use
// MarshallingIO (the cabinet USB-I/O) only while MarshallingIO.IsCan
// (sub_140227350) is true and UseKeyboardDeviceWithCardReader is off, and
// KeyboardInput otherwise. Without the USB-I/O, UMarshallingIO::Update
// (sub_1402467A0) sets the device-error byte +180 every frame and IsCan stays
// false, so driving and NESiCA both go through KeyboardInput. Forcing IsCan
// (or clearing +180) made the blueprints read the empty cabinet image: the
// horn bit is active-low, so the horn sounded nonstop and the master
// controller and keys stopped answering. The cabinet reader path could not
// read a card anyway: MarshallingIO.StartReadNESiCA (sub_140244E80) also needs
// reader +24, which only a loaded ArrowheadUSBIO.dll sets.
//
// KeyboardInput is the developer reader:
//   StartReadNESiCA (loc_140206DD0) sets +48, StopReadNESiCA (sub_140206F30)
//   clears +48/+49, GetNESiCAReadState (sub_1401EFAF0) returns
//   ENESiCAReadState NoRead (4) while +48 is clear, Reading (1) while no card
//   is present and Success (2) once +49 is set, and GetNESiCAID
//   (sub_1401EF9D0) returns the FString at +296. SetMimicNESiCAID
//   (sub_140205550) copies an id there and sets +49.
// The level blueprints poll the read state every frame while they wait for a
// card. The hook below calls SetMimicNESiCAID when the card key taps, so the
// same poll returns Success and the game reads the id and selects the card.
//
// The key (default P) is a tap: a press while a read is waiting completes it,
// and a press up to kTapWindowMs before the poll sees it is kept for it. P is
// also in-game cheat binding 60. GetAsyncKeyState still sees it if the game
// handles the key, so this feature does not swallow P.

namespace {

constexpr unsigned int kKeyboardReadStateRva = 0x1EFAF0; // sub_1401EFAF0 KeyboardInput.GetNESiCAReadState
constexpr unsigned int kKeyboardSetIdRva = 0x205550;     // sub_140205550 KeyboardInput.SetMimicNESiCAID

constexpr std::size_t kKeyboardReading = 48; // +48 set by StartReadNESiCA
constexpr std::size_t kKeyboardHasCard = 49; // +49 set by SetMimicNESiCAID

constexpr unsigned long long kTapWindowMs = 3000;
constexpr DWORD kKeyPollMs = 5;

constexpr char kDefaultCardId[] = "7070000000000001";

const unsigned char kKeyboardReadStatePrologue[] = {0x80, 0x79, 0x30, 0x00, 0x75, 0x03, 0xB0, 0x04, 0xC3};
const unsigned char kKeyboardSetIdPrologue[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9};

// Engine FString: data, count including the NUL, capacity.
struct EngineString
{
    const wchar_t* data;
    int count;
    int capacity;
};

using KeyboardReadStateFn = std::uint8_t(__fastcall*)(unsigned char* self);
using KeyboardSetIdFn = void*(__fastcall*)(unsigned char* self, const EngineString* id);

KeyboardReadStateFn g_originalKeyboardReadState = nullptr;
KeyboardSetIdFn g_keyboardSetId = nullptr;
bool g_keyboardWaitLogged = false;
char g_cardId[17] = "7070000000000001";
wchar_t g_cardIdWide[17] = L"7070000000000001";
int g_virtualKey = 'P';
bool g_enabled = true;

std::atomic<unsigned long long> g_tapTick{0};

void TrimCopy(wchar_t* dest, std::size_t destCount, const wchar_t* src)
{
    while (*src == L' ' || *src == L'\t')
        ++src;

    std::size_t length = 0;
    while (src[length] != L'\0' && length + 1 < destCount)
    {
        dest[length] = src[length];
        ++length;
    }
    dest[length] = L'\0';

    while (length > 0 && (dest[length - 1] == L' ' || dest[length - 1] == L'\t'))
    {
        --length;
        dest[length] = L'\0';
    }
}

void ToUpperAscii(wchar_t* text)
{
    for (; *text != L'\0'; ++text)
    {
        if (*text >= L'a' && *text <= L'z')
            *text = static_cast<wchar_t>(*text - L'a' + L'A');
    }
}

int HexDigit(wchar_t ch)
{
    if (ch >= L'0' && ch <= L'9')
        return ch - L'0';
    if (ch >= L'A' && ch <= L'F')
        return ch - L'A' + 10;
    return -1;
}

int ParseVirtualKey(const wchar_t* text)
{
    wchar_t name[32];
    TrimCopy(name, 32, text);
    ToUpperAscii(name);
    if (name[0] == L'\0')
        return 0;

    if (name[0] == L'0' && name[1] == L'X')
    {
        const int high = HexDigit(name[2]);
        if (high < 0)
            return 0;
        if (name[3] == L'\0')
            return high;
        const int low = HexDigit(name[3]);
        if (low < 0 || name[4] != L'\0')
            return 0;
        return (high << 4) | low;
    }

    if (wcscmp(name, L"SPACE") == 0)
        return VK_SPACE;
    if (wcscmp(name, L"SHIFT") == 0)
        return VK_SHIFT;
    if (wcscmp(name, L"CTRL") == 0)
        return VK_CONTROL;
    if (wcscmp(name, L"UP") == 0)
        return VK_UP;
    if (wcscmp(name, L"DOWN") == 0)
        return VK_DOWN;
    if (wcscmp(name, L"LEFT") == 0)
        return VK_LEFT;
    if (wcscmp(name, L"RIGHT") == 0)
        return VK_RIGHT;

    if (name[0] == L'F')
    {
        int number = 0;
        const wchar_t* digit = name + 1;
        if (*digit == L'\0')
            return 0;
        while (*digit >= L'0' && *digit <= L'9')
        {
            number = number * 10 + (*digit - L'0');
            ++digit;
            if (number > 12)
                return 0;
        }
        if (*digit != L'\0' || number < 1)
            return 0;
        return VK_F1 + (number - 1);
    }

    if (name[1] == L'\0' && name[0] >= L'A' && name[0] <= L'Z')
        return name[0];
    if (name[1] == L'\0' && name[0] >= L'0' && name[0] <= L'9')
        return name[0];
    return 0;
}

bool AcceptCardId(const char* text)
{
    if (text == nullptr)
        return false;
    if (std::strlen(text) != 16)
        return false;
    if (std::strncmp(text, "707", 3) != 0 && std::strncmp(text, "709", 3) != 0)
        return false;
    for (int i = 0; i < 16; ++i)
    {
        const char ch = text[i];
        const bool digit = ch >= '0' && ch <= '9';
        const bool upper = ch >= 'A' && ch <= 'Z';
        if (!digit && !upper)
            return false;
    }
    return true;
}

void NarrowCopy(char* dest, std::size_t destCount, const wchar_t* src)
{
    std::size_t length = 0;
    while (src[length] != L'\0' && length + 1 < destCount)
    {
        const wchar_t ch = src[length];
        dest[length] = (ch >= 32 && ch < 127) ? static_cast<char>(ch) : '?';
        ++length;
    }
    dest[length] = '\0';
}

void ReadCardConfig(const wchar_t* moduleDir)
{
    wchar_t iniPath[MAX_PATH];
    const std::size_t dirLen = wcslen(moduleDir);
    if (dirLen + 11 > MAX_PATH)
        return;

    wcscpy_s(iniPath, moduleDir);
    wcscat_s(iniPath, L"config.ini");

    g_enabled = GetPrivateProfileIntW(L"Card", L"Enabled", 1, iniPath) != 0;

    wchar_t keyName[32];
    GetPrivateProfileStringW(L"Card", L"Key", L"P", keyName, 32, iniPath);
    const int parsed = ParseVirtualKey(keyName);
    g_virtualKey = parsed != 0 ? parsed : 'P';

    wchar_t idText[32];
    GetPrivateProfileStringW(L"Card", L"Id", L"7070000000000001", idText, 32, iniPath);
    char narrow[32];
    NarrowCopy(narrow, 32, idText);
    if (AcceptCardId(narrow))
        std::memcpy(g_cardId, narrow, 17);
    else
        std::memcpy(g_cardId, kDefaultCardId, sizeof(kDefaultCardId));
    for (std::size_t i = 0; i < 17; ++i)
        g_cardIdWide[i] = static_cast<wchar_t>(g_cardId[i]);
}

bool TakeFreshTap()
{
    const unsigned long long tapped = g_tapTick.exchange(0);
    return tapped != 0 && GetTickCount64() - tapped <= kTapWindowMs;
}

// KeyboardInput.GetNESiCAReadState, polled while the game waits for a card.
std::uint8_t __fastcall KeyboardReadStateDetour(unsigned char* self)
{
    const bool waiting = self != nullptr && self[kKeyboardReading] != 0 && self[kKeyboardHasCard] == 0;
    if (waiting && !g_keyboardWaitLogged)
    {
        g_keyboardWaitLogged = true;
        LOG("Card: the game is reading NESiCA, press the card key to tap %s", g_cardId);
    }
    if (waiting && TakeFreshTap())
    {
        const EngineString id{g_cardIdWide, 17, 17};
        g_keyboardSetId(self, &id);
        g_keyboardWaitLogged = false;
        LOG("Card: tapped %s", g_cardId);
    }
    return g_originalKeyboardReadState(self);
}

DWORD WINAPI PollCardKey(LPVOID)
{
    bool wasDown = (GetAsyncKeyState(g_virtualKey) & 0x8000) != 0;
    for (;;)
    {
        const bool down = (GetAsyncKeyState(g_virtualKey) & 0x8000) != 0;
        if (down && !wasDown)
            g_tapTick.store(GetTickCount64());
        wasDown = down;
        Sleep(kKeyPollMs);
    }
}

bool InstallCardHooks()
{
    auto* image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (image == nullptr)
        return false;

    auto* keyboardReadState = image + kKeyboardReadStateRva;
    auto* keyboardSetId = image + kKeyboardSetIdRva;
    if (std::memcmp(keyboardReadState, kKeyboardReadStatePrologue, sizeof(kKeyboardReadStatePrologue)) != 0
        || std::memcmp(keyboardSetId, kKeyboardSetIdPrologue, sizeof(kKeyboardSetIdPrologue)) != 0)
    {
        LOG("ERROR: card reader prologue mismatch (keyboard read state RVA 0x%X, keyboard id RVA 0x%X)",
            kKeyboardReadStateRva, kKeyboardSetIdRva);
        return false;
    }
    g_keyboardSetId = reinterpret_cast<KeyboardSetIdFn>(keyboardSetId);

    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
        return false;
    if (MH_CreateHook(keyboardReadState, reinterpret_cast<void*>(&KeyboardReadStateDetour),
                      reinterpret_cast<void**>(&g_originalKeyboardReadState)) != MH_OK
        || MH_EnableHook(keyboardReadState) != MH_OK)
    {
        LOG("ERROR: keyboard read-state hook was not installed");
        return false;
    }
    return true;
}

} // namespace

namespace features {

void Card_Install(const wchar_t* moduleDir)
{
    static std::atomic<long> installed{0};
    if (moduleDir == nullptr || moduleDir[0] == L'\0')
        return;
    if (installed.exchange(1) != 0)
        return;

    ReadCardConfig(moduleDir);
    if (!g_enabled)
    {
        LOG("Card tap disabled");
        return;
    }
    if (!InstallCardHooks())
    {
        LOG("ERROR: card reader was not installed (VK 0x%02X id %s)", g_virtualKey, g_cardId);
        return;
    }
    LOG("Card key VK 0x%02X taps id %s when the game reads NESiCA (keyboard device)", g_virtualKey, g_cardId);

    const HANDLE thread = CreateThread(nullptr, 0, &PollCardKey, nullptr, 0, nullptr);
    if (thread != nullptr)
        CloseHandle(thread);
    else
        LOG("ERROR: card key thread was not created (%lu)", GetLastError());
}

} // namespace features
