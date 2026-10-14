#include "features/module_api.h"

#include <MinHook.h>

#include "log.h"

#include <windows.h>

#include <atomic>
#include <cwchar>

// Local cabinet coin for TG4AC 5.80.02.
//
// UKeyboardInput::CoinOperation only latches a press or release bit.
// UKeyboardInput::Update samples those bits, runs the coin-switch state
// machine, and on the release edge (state 3) increments Coin and broadcasts
// CoinNumChangeDelegate. See docs/process/2026-10-11-coin.md.
//
// Image base in the IDA database is 0x140000000. Runtime addresses are
// module base + RVA because the EXE is relocated.

namespace {

constexpr unsigned int kKeyboardUpdateRva = 0x209170; // sub_140209170
constexpr unsigned int kCoinOperationRva = 0x1E6950;  // sub_1401E6950
constexpr std::size_t kCoinSwitchOffset = 0x50;

using KeyboardUpdateFn = void(__fastcall*)(void* self);
using CoinOperationFn = void(__fastcall*)(void* self, char pressRelease);

KeyboardUpdateFn g_originalUpdate = nullptr;
CoinOperationFn g_coinOperation = nullptr;
std::atomic<int> g_pendingCredits{0};
int g_virtualKey = VK_F3;

const unsigned char kUpdatePrologue[] = {0x48, 0x89, 0x5C, 0x24, 0x10};
const unsigned char kCoinPrologue[] = {0x84, 0xD2, 0x74, 0x05, 0x83, 0x49, 0x28, 0x01, 0xC3};

bool HasPrefix(const unsigned char* bytes, const unsigned char* prefix, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i)
    {
        if (bytes[i] != prefix[i])
            return false;
    }
    return true;
}

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

// Returns 0 when the name is not a virtual key we accept.
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

int ReadCoinVirtualKey(const wchar_t* moduleDir)
{
    wchar_t iniPath[MAX_PATH];
    const std::size_t dirLen = wcslen(moduleDir);
    // "config.ini" plus a NUL. DllMain passes the directory with a trailing separator.
    if (dirLen + 11 > MAX_PATH)
        return VK_F3;

    wcscpy_s(iniPath, moduleDir);
    wcscat_s(iniPath, L"config.ini");

    wchar_t keyName[32];
    GetPrivateProfileStringW(L"Input", L"Coin", L"F3", keyName, 32, iniPath);
    const int parsed = ParseVirtualKey(keyName);
    return parsed != 0 ? parsed : VK_F3;
}

void __fastcall KeyboardUpdateDetour(void* self)
{
    // One queued rising edge becomes one press+release pair, and only while
    // the coin switch is idle. Update itself turns that pair into state 3
    // and one credit, then leaves the switch at 3 until the next tick.
    if (self != nullptr && g_coinOperation != nullptr)
    {
        const auto coinSwitch = static_cast<const unsigned char*>(self)[kCoinSwitchOffset];
        if (coinSwitch == 0)
        {
            int pending = g_pendingCredits.load(std::memory_order_acquire);
            while (pending > 0)
            {
                if (g_pendingCredits.compare_exchange_weak(
                        pending,
                        pending - 1,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire))
                {
                    g_coinOperation(self, 1);
                    g_coinOperation(self, 0);
                    break;
                }
            }
        }
    }

    if (g_originalUpdate != nullptr)
        g_originalUpdate(self);
}

DWORD WINAPI PollCoinKey(LPVOID)
{
    bool wasDown = (GetAsyncKeyState(g_virtualKey) & 0x8000) != 0;
    for (;;)
    {
        const bool down = (GetAsyncKeyState(g_virtualKey) & 0x8000) != 0;
        if (down && !wasDown)
            g_pendingCredits.fetch_add(1, std::memory_order_release);
        wasDown = down;
        Sleep(5);
    }
}

bool InstallCoinHook()
{
    HMODULE game = GetModuleHandleW(L"TG4AC-Win64-Shipping.exe");
    if (game == nullptr)
        game = GetModuleHandleW(nullptr);
    if (game == nullptr)
        return false;

    auto* base = reinterpret_cast<unsigned char*>(game);
    auto* update = base + kKeyboardUpdateRva;
    auto* coinOperation = base + kCoinOperationRva;
    if (!HasPrefix(update, kUpdatePrologue, sizeof(kUpdatePrologue)) ||
        !HasPrefix(coinOperation, kCoinPrologue, sizeof(kCoinPrologue)))
    {
        return false;
    }

    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
        return false;

    g_coinOperation = reinterpret_cast<CoinOperationFn>(coinOperation);
    if (MH_CreateHook(update, &KeyboardUpdateDetour, reinterpret_cast<void**>(&g_originalUpdate)) != MH_OK)
        return false;
    // Enable this hook only. Other features own their own targets.
    if (MH_EnableHook(update) != MH_OK)
        return false;
    return true;
}

} // namespace

namespace features {

void Coin_Install(const wchar_t* moduleDir)
{
    static std::atomic<long> installed{0};
    if (moduleDir == nullptr || moduleDir[0] == L'\0')
        return;
    if (installed.exchange(1) != 0)
        return;

    g_virtualKey = ReadCoinVirtualKey(moduleDir);
    if (!InstallCoinHook())
    {
        LOG("ERROR: coin hook was not installed (VK 0x%02X)", g_virtualKey);
        return;
    }
    LOG("Coin key VK 0x%02X inserts one credit", g_virtualKey);

    const HANDLE thread = CreateThread(nullptr, 0, &PollCoinKey, nullptr, 0, nullptr);
    if (thread != nullptr)
        CloseHandle(thread);
}

} // namespace features
