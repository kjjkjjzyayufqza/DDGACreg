#include "layout_api.h"
#include "features/module_api.h"
#include "display/touch.h"

#include "log.h"

#include <windows.h>

#include <atomic>
#include <cwchar>

namespace {

constexpr int kPanelWidth = 1920;
constexpr int kForwardPanelHeight = 1080;
constexpr int kTouchPanelHeight = 822;
constexpr float kHalf = 0.5f;
constexpr float kTouchSourceHeight = 822.0f / 2160.0f;

constexpr float kForwardAspect = 1920.0f / 1080.0f;
constexpr float kTouchAspect = 1920.0f / 822.0f;

std::atomic<int> g_preset{LayoutPreset_Cabinet32x9};
std::atomic<int> g_focus{LayoutFocus_Center};
std::atomic<int> g_returnPreset{LayoutPreset_Widescreen16x9};
std::atomic<bool> g_pollStarted{false};

bool IsKeyDown(int virtualKey)
{
    if (virtualKey <= 0)
        return false;
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

void CopyIniToken(const wchar_t* text, wchar_t* out, size_t outChars)
{
    if (outChars == 0)
        return;
    out[0] = L'\0';
    if (text == nullptr)
        return;

    while (*text == L' ' || *text == L'\t')
        ++text;

    size_t length = 0;
    while (*text != L'\0' && *text != L';' && length + 1 < outChars)
        out[length++] = *text++;

    while (length > 0 && (out[length - 1] == L' ' || out[length - 1] == L'\t'))
        --length;
    out[length] = L'\0';
}

int ParsePreset(const wchar_t* text)
{
    wchar_t token[32];
    CopyIniToken(text, token, 32);
    if (_wcsicmp(token, L"16:9") == 0)
        return LayoutPreset_Widescreen16x9;
    if (_wcsicmp(token, L"single") == 0)
        return LayoutPreset_Single;
    return LayoutPreset_Cabinet32x9;
}

int NormalizePreset(int preset)
{
    if (preset == LayoutPreset_Widescreen16x9 || preset == LayoutPreset_Single)
        return preset;
    return LayoutPreset_Cabinet32x9;
}

int NormalizeFocus(int focus)
{
    if (focus >= LayoutFocus_Left && focus <= LayoutFocus_Touch)
        return focus;
    return LayoutFocus_Center;
}

LayoutRect CabinetSource(int focus)
{
    switch (focus) {
    case LayoutFocus_Center:
        return LayoutRect{kHalf, kHalf, kHalf, 0.0f};
    case LayoutFocus_Right:
        return LayoutRect{kHalf, kHalf, 0.0f, kHalf};
    case LayoutFocus_Touch:
        return LayoutRect{kHalf, kTouchSourceHeight, kHalf, kHalf};
    case LayoutFocus_Left:
    default:
        return LayoutRect{kHalf, kHalf, 0.0f, 0.0f};
    }
}

void FillCabinetSources(LayoutRect source[4])
{
    source[LayoutFocus_Left] = CabinetSource(LayoutFocus_Left);
    source[LayoutFocus_Center] = CabinetSource(LayoutFocus_Center);
    source[LayoutFocus_Right] = CabinetSource(LayoutFocus_Right);
    source[LayoutFocus_Touch] = CabinetSource(LayoutFocus_Touch);
}

int PanelContentHeight(int focus)
{
    if (focus == LayoutFocus_Touch)
        return kTouchPanelHeight;
    return kForwardPanelHeight;
}

// Fit contentWidth x contentHeight inside the output without stretching.
// Wider content gets bars above and below; taller content gets side bars.
LayoutRect FitPanel(int outputWidth, int outputHeight, int contentWidth, int contentHeight)
{
    const LayoutRect fullWindow{1.0f, 1.0f, 0.0f, 0.0f};
    if (outputWidth <= 0 || outputHeight <= 0 || contentWidth <= 0 || contentHeight <= 0)
        return fullWindow;

    const long long contentSpan = static_cast<long long>(contentWidth) * outputHeight;
    const long long outputSpan = static_cast<long long>(outputWidth) * contentHeight;
    if (contentSpan == outputSpan)
        return fullWindow;

    if (contentSpan > outputSpan) {
        const double sizeY = (static_cast<double>(contentHeight) * outputWidth) /
            (static_cast<double>(contentWidth) * outputHeight);
        const float fittedY = sizeY <= 0.0 ? 0.0f : (sizeY >= 1.0 ? 1.0f : static_cast<float>(sizeY));
        return LayoutRect{1.0f, fittedY, 0.0f, (1.0f - fittedY) * 0.5f};
    }

    const double sizeX = (static_cast<double>(contentWidth) * outputHeight) /
        (static_cast<double>(contentHeight) * outputWidth);
    const float fittedX = sizeX <= 0.0 ? 0.0f : (sizeX >= 1.0 ? 1.0f : static_cast<float>(sizeX));
    return LayoutRect{fittedX, 1.0f, (1.0f - fittedX) * 0.5f, 0.0f};
}

// Fit a width/height aspect inside a cell of the output, centered. The whole
// picture stays visible; leftover cell space is left empty.
LayoutRect FitInCell(int outputWidth, int outputHeight, float cellX, float cellY,
                     float cellW, float cellH, float contentAspect)
{
    if (outputWidth <= 0 || outputHeight <= 0 || cellW <= 0.0f || cellH <= 0.0f || contentAspect <= 0.0f)
        return LayoutRect{cellW, cellH, cellX, cellY};

    const double cellAspect = (static_cast<double>(cellW) * outputWidth) /
        (static_cast<double>(cellH) * outputHeight);
    double sizeX = cellW;
    double sizeY = cellH;
    double originX = cellX;
    double originY = cellY;
    if (contentAspect > cellAspect)
    {
        sizeY = cellH * (cellAspect / contentAspect);
        originY = cellY + (cellH - sizeY) * 0.5;
    }
    else
    {
        sizeX = cellW * (contentAspect / cellAspect);
        originX = cellX + (cellW - sizeX) * 0.5;
    }
    return LayoutRect{
        static_cast<float>(sizeX),
        static_cast<float>(sizeY),
        static_cast<float>(originX),
        static_cast<float>(originY),
    };
}

// The center screen is the largest. On a 16:9 monitor its width fraction is
// also its height fraction, so it fills most of the window. Side screens share
// the rest of that row. The touch panel sits under the center and is no wider
// than it, so the bottom strip cannot outgrow the main screen.
void FillWidescreen(LayoutRect dest[4], int outputWidth, int outputHeight)
{
    constexpr float kCenterW = 0.66f;
    const float sideW = (1.0f - kCenterW) * 0.5f;
    float rowH = kCenterW;
    if (outputWidth > 0 && outputHeight > 0)
    {
        const double fitted = (static_cast<double>(kCenterW) * outputWidth) /
            (static_cast<double>(outputHeight) * kForwardAspect);
        rowH = static_cast<float>(fitted);
        if (rowH > 0.80f)
            rowH = 0.80f;
        if (rowH < 0.50f)
            rowH = 0.50f;
    }
    const float touchH = 1.0f - rowH;
    dest[LayoutFocus_Left] = FitInCell(outputWidth, outputHeight, 0.0f, 0.0f, sideW, rowH, kForwardAspect);
    dest[LayoutFocus_Center] = FitInCell(outputWidth, outputHeight, sideW, 0.0f, kCenterW, rowH, kForwardAspect);
    dest[LayoutFocus_Right] = FitInCell(outputWidth, outputHeight, sideW + kCenterW, 0.0f, sideW, rowH, kForwardAspect);
    dest[LayoutFocus_Touch] = FitInCell(outputWidth, outputHeight, sideW, rowH, kCenterW, touchH, kTouchAspect);
}

// moduleDir from DllMain has no trailing slash. Join it to config.ini.
bool BuildIniPath(const wchar_t* moduleDir, wchar_t* out, size_t outChars)
{
    if (moduleDir == nullptr || moduleDir[0] == L'\0')
        return false;
    if (wcscpy_s(out, outChars, moduleDir) != 0)
        return false;

    const size_t length = wcslen(out);
    if (length > 0 && out[length - 1] != L'\\' && out[length - 1] != L'/') {
        if (wcscat_s(out, outChars, L"\\") != 0)
            return false;
    }
    return wcscat_s(out, outChars, L"config.ini") == 0;
}

void LoadConfig(const wchar_t* moduleDir)
{
    wchar_t iniPath[1024];
    if (!BuildIniPath(moduleDir, iniPath, 1024))
        return;

    wchar_t presetText[64];
    GetPrivateProfileStringW(L"LayoutMode", L"Preset", L"16:9", presetText, 64, iniPath);

    const int preset = ParsePreset(presetText);
    g_preset.store(preset, std::memory_order_relaxed);
    if (preset != LayoutPreset_Single)
        g_returnPreset.store(preset, std::memory_order_relaxed);
}

bool DigitDown(int numberKey, int numpadKey)
{
    return IsKeyDown(numberKey) || IsKeyDown(numpadKey);
}

// Ctrl+1..4 show one screen. Ctrl+0 restores the multi-screen preset.
// Alt is excluded so Alt+F4 is never involved, and arrow keys stay with the game.
void ShowSingleScreen(int focus)
{
    Touch_ReleaseForwardedButtons();
    const int current = g_preset.load(std::memory_order_relaxed);
    if (current != LayoutPreset_Single)
        g_returnPreset.store(current, std::memory_order_relaxed);
    g_focus.store(focus, std::memory_order_relaxed);
    g_preset.store(LayoutPreset_Single, std::memory_order_relaxed);
}

void RestoreMultiScreen()
{
    Touch_ReleaseForwardedButtons();
    int back = g_returnPreset.load(std::memory_order_relaxed);
    if (back != LayoutPreset_Widescreen16x9 && back != LayoutPreset_Cabinet32x9)
        back = LayoutPreset_Widescreen16x9;
    g_preset.store(back, std::memory_order_relaxed);
}

DWORD WINAPI PollThread(LPVOID)
{
    bool singleWasDown[4] = {};
    bool restoreWasDown = false;
    const int numberKey[4] = {'1', '2', '3', '4'};
    const int numpadKey[4] = {VK_NUMPAD1, VK_NUMPAD2, VK_NUMPAD3, VK_NUMPAD4};
    const int focusOf[4] = {
        LayoutFocus_Left,
        LayoutFocus_Center,
        LayoutFocus_Right,
        LayoutFocus_Touch,
    };

    for (;;) {
        const bool ctrl = IsKeyDown(VK_CONTROL);
        const bool alt = IsKeyDown(VK_MENU);
        const bool chord = ctrl && !alt;

        for (int index = 0; index < 4; ++index) {
            const bool down = chord && DigitDown(numberKey[index], numpadKey[index]);
            if (down && !singleWasDown[index])
                ShowSingleScreen(focusOf[index]);
            singleWasDown[index] = down;
        }

        const bool restoreDown = chord && DigitDown('0', VK_NUMPAD0);
        if (restoreDown && !restoreWasDown
            && g_preset.load(std::memory_order_relaxed) == LayoutPreset_Single)
            RestoreMultiScreen();
        restoreWasDown = restoreDown;

        Sleep(15);
    }
}

}  // namespace

LayoutView Layout_Query(int outputWidth, int outputHeight)
{
    LayoutView view{};
    view.preset = NormalizePreset(g_preset.load(std::memory_order_relaxed));
    view.focus = NormalizeFocus(g_focus.load(std::memory_order_relaxed));
    FillCabinetSources(view.source);

    if (view.preset == LayoutPreset_Widescreen16x9) {
        view.overridePlacements = true;
        view.activeCount = 4;
        FillWidescreen(view.dest, outputWidth, outputHeight);
        return view;
    }

    if (view.preset == LayoutPreset_Single) {
        view.overridePlacements = true;
        view.activeCount = 1;
        view.source[0] = CabinetSource(view.focus);
        view.dest[0] = FitPanel(outputWidth, outputHeight, kPanelWidth, PanelContentHeight(view.focus));
        for (int index = 1; index < 4; ++index) {
            view.source[index] = LayoutRect{};
            view.dest[index] = LayoutRect{};
        }
        return view;
    }

    view.overridePlacements = false;
    view.activeCount = 4;
    return view;
}

namespace features {

void Layout_Install(const wchar_t* moduleDir)
{
    bool expected = false;
    if (!g_pollStarted.compare_exchange_strong(expected, true))
        return;

    LoadConfig(moduleDir);
    const int preset = g_preset.load(std::memory_order_relaxed);
    const char* presetName = "32:9";
    if (preset == LayoutPreset_Widescreen16x9)
        presetName = "16:9";
    else if (preset == LayoutPreset_Single)
        presetName = "single";
    LOG("Layout preset %s. Ctrl+1 left, Ctrl+2 center, Ctrl+3 right, Ctrl+4 touch, Ctrl+0 restores",
        presetName);

    const HANDLE thread = CreateThread(nullptr, 0, &PollThread, nullptr, 0, nullptr);
    if (thread != nullptr)
        CloseHandle(thread);
}

}  // namespace features
