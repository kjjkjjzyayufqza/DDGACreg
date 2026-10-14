#include "features/module_api.h"
#include "features/freecam/freecam.h"
#include "features/hud/hud.h"
#include "display/touch.h"

#include <MinHook.h>

#include "log.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cwchar>

namespace {

// APlayerCameraManager::DoUpdateCamera. Image base 0x140000000, so the RVA is
// 0x14A4D80. rcx is the manager and xmm1 is DeltaTime. The function copies the
// finished FMinimalViewInfo into CameraCache.POV at this+0x410 before returning.
constexpr std::uintptr_t kDoUpdateCameraRva = 0x14A4D80;

// FCameraCacheEntry.POV is at +0x10. FMinimalViewInfo.Location is the first
// three floats, Rotation (pitch, yaw, roll) starts at +0x0C, and FOV is at
// +0x18. While the free camera is on, FOV is written with the pose.
constexpr unsigned kCameraCachePov = 0x410;
constexpr unsigned kLastFramePov = 0x950;
constexpr unsigned kViewTargetPov = 0xE90;
constexpr unsigned kRotationOffset = 0x0C;
constexpr unsigned kFovOffset = 0x18;
// PendingViewTarget.POV is +0x13E0. DoUpdateCamera blends from it, then
// commits the three slots above. UpdateCamera's only read after that call
// is CameraCache, for the client camera RPC. Leave +0x13E0 alone.

constexpr float kDegreesToRadians = 0.017453292519943295f;
constexpr float kPitchLimit = 89.0f;
// Ini MoveSpeed is the base. The live speed starts at 10x that base.
constexpr float kInitialSpeedScale = 10.0f;
constexpr float kMinLiveSpeed = 1.0f;
constexpr float kMaxLiveSpeed = 100000.0f;
constexpr float kFovDegreesPerSecond = 30.0f;
constexpr float kMinFov = 20.0f;
constexpr float kMaxFov = 150.0f;
// UE 4.15 four-player splitscreen is this cabinet frame: call 0 left
// (top-left), call 1 center (top-right), call 2 right (bottom-left), call 3
// touch (bottom-right). DoUpdateCamera runs once per local player, in that
// order. Seed from the center call. Do not add a constant yaw.
constexpr int kCenterCallIndex = 1;
// Split-screen updates land in one burst. A later burst is the next frame.
constexpr double kFrameGapSeconds = 0.005;

struct NamedVirtualKey {
    const wchar_t* name;
    int vk;
};

constexpr NamedVirtualKey kNamedVirtualKeys[] = {
    {L"SPACE", VK_SPACE},
    {L"SHIFT", VK_SHIFT},
    {L"LSHIFT", VK_LSHIFT},
    {L"RSHIFT", VK_RSHIFT},
    {L"CTRL", VK_CONTROL},
    {L"CONTROL", VK_CONTROL},
    {L"NONE", 0},
    {L"OFF", 0},
    {L"LCTRL", VK_LCONTROL},
    {L"RCTRL", VK_RCONTROL},
    {L"ALT", VK_MENU},
    {L"UP", VK_UP},
    {L"DOWN", VK_DOWN},
    {L"LEFT", VK_LEFT},
    {L"RIGHT", VK_RIGHT},
    {L"TAB", VK_TAB},
    {L"ESC", VK_ESCAPE},
    {L"ESCAPE", VK_ESCAPE},
    {L"ENTER", VK_RETURN},
    {L"RETURN", VK_RETURN},
    {L"BACKSPACE", VK_BACK},
    {L"BACK", VK_BACK},
};

using DoUpdateCameraFn = std::uintptr_t(__fastcall*)(void* cameraManager, float deltaSeconds);

DoUpdateCameraFn g_original = nullptr;

int g_toggleVk = VK_F2;
int g_fasterVk = VK_SHIFT;
int g_slowerVk = 0;
int g_upVk = VK_SPACE;
int g_downVk = VK_CONTROL;
float g_moveSpeed = 8.0f;
float g_fastMultiplier = 4.0f;
float g_slowMultiplier = 0.25f;
float g_mouseSensitivity = 0.15f;

bool g_active = false;
bool g_toggleWasDown = false;
bool g_escapeWasDown = false;
float g_x = 0.0f;
float g_y = 0.0f;
float g_z = 0.0f;
float g_pitch = 0.0f;
float g_yaw = 0.0f;
float g_roll = 0.0f;
float g_fov = 90.0f;
// Center CameraCache.POV FOV captured when the free camera turns on.
float g_seedFov = 90.0f;
float g_liveSpeed = 80.0f;

bool g_pending = false;
bool g_driveInput = false;
int g_callsInFrame = 0;
void* g_firstManager = nullptr;
bool g_haveFirstPose = false;
float g_firstPitch = 0.0f;
float g_firstYaw = 0.0f;
float g_firstRoll = 0.0f;
int g_fovNarrowVk = VK_OEM_4;
int g_fovWidenVk = VK_OEM_6;

bool g_cursorHeld = false;
bool g_skipMouse = false;
int g_cursorHides = 0;
POINT g_center = {};
// Half the clipped client size. A sample can never move farther than this.
POINT g_clipHalf = {};

void CopyIniToken(const wchar_t* text, wchar_t* out, std::size_t outChars)
{
    if (outChars == 0)
        return;
    out[0] = L'\0';
    if (text == nullptr)
        return;

    while (*text == L' ' || *text == L'\t')
        ++text;

    std::size_t length = 0;
    while (*text != L'\0' && *text != L';' && length + 1 < outChars)
        out[length++] = *text++;

    while (length > 0 && (out[length - 1] == L' ' || out[length - 1] == L'\t'))
        --length;
    out[length] = L'\0';
}

int ParseFunctionKey(const wchar_t* token)
{
    if (token[0] != L'F' && token[0] != L'f')
        return 0;
    if (token[1] < L'0' || token[1] > L'9')
        return 0;

    int number = 0;
    const wchar_t* cursor = token + 1;
    while (*cursor >= L'0' && *cursor <= L'9') {
        number = number * 10 + (*cursor - L'0');
        ++cursor;
    }
    if (*cursor != L'\0' || number < 1 || number > 24)
        return 0;
    return VK_F1 + (number - 1);
}

int ParseVirtualKey(const wchar_t* text, int fallback)
{
    wchar_t token[32];
    CopyIniToken(text, token, 32);
    if (token[0] == L'\0')
        return fallback;

    const int functionKey = ParseFunctionKey(token);
    if (functionKey != 0)
        return functionKey;

    if (token[1] == L'\0') {
        wchar_t letter = token[0];
        if (letter >= L'a' && letter <= L'z')
            letter = static_cast<wchar_t>(letter - L'a' + L'A');
        if ((letter >= L'A' && letter <= L'Z') || (letter >= L'0' && letter <= L'9'))
            return letter;
    }

    for (const NamedVirtualKey& named : kNamedVirtualKeys) {
        if (_wcsicmp(token, named.name) == 0)
            return named.vk;
    }
    return fallback;
}

float ParseIniFloat(const wchar_t* text, float fallback)
{
    if (text == nullptr || text[0] == L'\0')
        return fallback;

    wchar_t* end = nullptr;
    const float value = wcstof(text, &end);
    if (end == text)
        return fallback;
    return value;
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

bool IsKeyDown(int virtualKey)
{
    if (virtualKey <= 0 || virtualKey >= 256)
        return false;
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

struct WindowSearch {
    DWORD processId;
    HWND window;
    LONG area;
};

BOOL CALLBACK FindLargestWindow(HWND hwnd, LPARAM param)
{
    WindowSearch* search = reinterpret_cast<WindowSearch*>(param);
    DWORD windowProcess = 0;
    GetWindowThreadProcessId(hwnd, &windowProcess);
    if (windowProcess != search->processId || !IsWindowVisible(hwnd))
        return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != nullptr)
        return TRUE;

    RECT client = {};
    if (!GetClientRect(hwnd, &client))
        return TRUE;
    const LONG width = client.right - client.left;
    const LONG height = client.bottom - client.top;
    if (width <= 0 || height <= 0)
        return TRUE;
    const LONG area = width * height;
    if (area > search->area) {
        search->area = area;
        search->window = hwnd;
    }
    return TRUE;
}

HWND FindGameWindow()
{
    WindowSearch search = {};
    search.processId = GetCurrentProcessId();
    EnumWindows(FindLargestWindow, reinterpret_cast<LPARAM>(&search));
    return search.window;
}

void HideCursor()
{
    g_cursorHides = 0;
    for (int i = 0; i < 32; ++i) {
        const int count = ShowCursor(FALSE);
        ++g_cursorHides;
        if (count < 0)
            break;
    }
}

void RestoreCursor()
{
    for (int i = 0; i < g_cursorHides; ++i)
        ShowCursor(TRUE);
    g_cursorHides = 0;
}

void ReleaseCursor()
{
    if (!g_cursorHeld)
        return;
    ClipCursor(nullptr);
    RestoreCursor();
    g_cursorHeld = false;
    g_skipMouse = false;
}

bool RefreshCursorClip()
{
    HWND window = Touch_MainOutputWindow();
    if (window == nullptr)
        window = FindGameWindow();
    if (window == nullptr)
        return false;

    RECT client = {};
    if (!GetClientRect(window, &client))
        return false;

    POINT topLeft = {client.left, client.top};
    POINT bottomRight = {client.right, client.bottom};
    if (!ClientToScreen(window, &topLeft) || !ClientToScreen(window, &bottomRight))
        return false;
    if (bottomRight.x - topLeft.x < 2 || bottomRight.y - topLeft.y < 2)
        return false;

    const RECT clip = {topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
    ClipCursor(&clip);
    g_clipHalf.x = (bottomRight.x - topLeft.x) / 2;
    g_clipHalf.y = (bottomRight.y - topLeft.y) / 2;
    g_center.x = topLeft.x + g_clipHalf.x;
    g_center.y = topLeft.y + g_clipHalf.y;
    return true;
}

void CaptureCursor()
{
    ReleaseCursor();
    Touch_SetCursorPassthrough(true);
    if (!RefreshCursorClip())
        return;
    HideCursor();
    SetCursor(nullptr);
    SetCursorPos(g_center.x, g_center.y);
    g_cursorHeld = true;
    g_skipMouse = true;
}

void ReadPose(const void* pov, float& x, float& y, float& z, float& pitch, float& yaw, float& roll)
{
    const auto* floats = static_cast<const float*>(pov);
    x = floats[0];
    y = floats[1];
    z = floats[2];
    pitch = floats[3];
    yaw = floats[4];
    roll = floats[5];
}

float ReadFov(const void* pov)
{
    const auto* fov = reinterpret_cast<const float*>(static_cast<const unsigned char*>(pov) + kFovOffset);
    return *fov;
}

void WritePose(void* pov)
{
    auto* location = static_cast<float*>(pov);
    auto* rotation = reinterpret_cast<float*>(static_cast<unsigned char*>(pov) + kRotationOffset);
    auto* fov = reinterpret_cast<float*>(static_cast<unsigned char*>(pov) + kFovOffset);
    location[0] = g_x;
    location[1] = g_y;
    location[2] = g_z;
    rotation[0] = g_pitch;
    rotation[1] = g_yaw;
    rotation[2] = g_roll;
    *fov = g_fov;
}

float ClampRange(float value, float minValue, float maxValue)
{
    if (value < minValue)
        return minValue;
    if (value > maxValue)
        return maxValue;
    return value;
}

float WrapDegrees(float degrees)
{
    while (degrees > 180.0f)
        degrees -= 360.0f;
    while (degrees < -180.0f)
        degrees += 360.0f;
    return degrees;
}

void SeedFromCache(void* cameraManager)
{
    auto* cachePov = static_cast<unsigned char*>(cameraManager) + kCameraCachePov;
    ReadPose(cachePov, g_x, g_y, g_z, g_pitch, g_yaw, g_roll);
    // Keep the pitch the game wrote. Clamping here pulls a cab camera that
    // looks down past 89 degrees up to the limit. Clamp only after a mouse move.
    g_yaw = WrapDegrees(g_yaw);
    g_roll = WrapDegrees(g_roll);
    g_fov = ClampRange(ReadFov(cachePov), kMinFov, kMaxFov);
    g_seedFov = g_fov;
    g_liveSpeed = ClampRange(g_moveSpeed * kInitialSpeedScale, kMinLiveSpeed, kMaxLiveSpeed);
}

void WriteActiveView(void* cameraManager)
{
    auto* base = static_cast<unsigned char*>(cameraManager);
    const unsigned offsets[] = {kCameraCachePov, kLastFramePov, kViewTargetPov};
    for (unsigned offset : offsets)
        WritePose(base + offset);
}

bool TakeMouseSample(int& deltaX, int& deltaY)
{
    // DoUpdateCamera runs once per split-screen manager. Sample the cursor
    // once per frame or the same pixels are added several times.
    static LARGE_INTEGER frequency = {};
    static LARGE_INTEGER last = {};
    if (frequency.QuadPart == 0)
        QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    if (last.QuadPart != 0 && frequency.QuadPart != 0)
    {
        const double seconds = static_cast<double>(now.QuadPart - last.QuadPart) / static_cast<double>(frequency.QuadPart);
        if (seconds < 0.002)
            return false;
    }
    last.QuadPart = now.QuadPart;

    POINT cursor = {};
    if (!GetCursorPos(&cursor))
        return false;
    deltaX = cursor.x - g_center.x;
    deltaY = cursor.y - g_center.y;
    SetCursorPos(g_center.x, g_center.y);
    if (g_skipMouse)
    {
        g_skipMouse = false;
        return false;
    }
    // The cursor is clipped to the window and recentered every frame, so a
    // real sample stays inside half the clip. A larger step is a remapped
    // cursor, not a turn. A fixed pixel cap dropped fast flicks at low frame
    // rates, which froze the view for that frame.
    if (deltaX > g_clipHalf.x || deltaX < -g_clipHalf.x || deltaY > g_clipHalf.y || deltaY < -g_clipHalf.y)
        return false;
    return true;
}

void ApplyMouseLook()
{
    if (!g_cursorHeld)
        CaptureCursor();
    if (!g_cursorHeld)
        return;

    const bool clipped = RefreshCursorClip();
    SetCursor(nullptr);
    if (!clipped)
        return;

    int deltaX = 0;
    int deltaY = 0;
    if (!TakeMouseSample(deltaX, deltaY))
        return;

    // A centered cursor is not a look. Clamping a zero sample would change a
    // seeded pitch the game left steeper than 89 degrees.
    if (deltaX == 0 && deltaY == 0)
        return;

    g_yaw += static_cast<float>(deltaX) * g_mouseSensitivity;
    g_pitch -= static_cast<float>(deltaY) * g_mouseSensitivity;
    g_pitch = WrapDegrees(g_pitch);
    if (g_pitch > kPitchLimit)
        g_pitch = kPitchLimit;
    if (g_pitch < -kPitchLimit)
        g_pitch = -kPitchLimit;
    g_yaw = WrapDegrees(g_yaw);
}

void ApplyLiveSpeed(float deltaSeconds)
{
    // R wins over the Q/E ramp for this step and restores the 10x base.
    // The same hold restores the FOV seeded at activation.
    if (IsKeyDown('R'))
    {
        g_liveSpeed = ClampRange(g_moveSpeed * kInitialSpeedScale, kMinLiveSpeed, kMaxLiveSpeed);
        g_fov = g_seedFov;
        return;
    }
    if (deltaSeconds <= 0.0f || deltaSeconds >= 0.5f)
        return;
    // About 0.5x per second while Q is held, about 2x per second while E is held.
    if (IsKeyDown('Q'))
        g_liveSpeed *= powf(0.5f, deltaSeconds);
    if (IsKeyDown('E'))
        g_liveSpeed *= powf(2.0f, deltaSeconds);
    g_liveSpeed = ClampRange(g_liveSpeed, kMinLiveSpeed, kMaxLiveSpeed);
}

int VirtualKeyFor(wchar_t character, int fallback)
{
    const SHORT scanned = VkKeyScanW(character);
    if (scanned == -1)
        return fallback;
    const int vk = scanned & 0xFF;
    if (vk <= 0 || vk >= 256)
        return fallback;
    return vk;
}

void RefreshFovKeys()
{
    // US OEM_5 is backslash, not ']'. Ask the active layout which key
    // produces each bracket, and fall back to OEM_4 / OEM_6.
    g_fovNarrowVk = VirtualKeyFor(L'[', VK_OEM_4);
    g_fovWidenVk = VirtualKeyFor(L']', VK_OEM_6);
}

void ApplyFovKeys(float deltaSeconds)
{
    // ApplyLiveSpeed already wrote g_seedFov. Skip the brackets so a held
    // R keeps that FOV even when [ or ] is also down.
    if (IsKeyDown('R'))
        return;
    if (deltaSeconds <= 0.0f || deltaSeconds >= 0.5f)
        return;
    RefreshFovKeys();
    const float step = kFovDegreesPerSecond * deltaSeconds;
    if (IsKeyDown(g_fovNarrowVk))
        g_fov = ClampRange(g_fov - step, kMinFov, kMaxFov);
    if (IsKeyDown(g_fovWidenVk))
        g_fov = ClampRange(g_fov + step, kMinFov, kMaxFov);
}

void ApplyMovement(float deltaSeconds)
{
    // Live speed is unreal units per second. Shift scales this step while held.
    // Ctrl is down, so it must not also scale the step.
    float speed = g_liveSpeed;
    if (IsKeyDown(g_fasterVk))
        speed *= g_fastMultiplier;
    if (g_slowerVk != 0 && g_slowerVk != g_downVk && g_slowerVk != g_upVk && IsKeyDown(g_slowerVk))
        speed *= g_slowMultiplier;

    float step = 0.0f;
    if (deltaSeconds > 0.0f && deltaSeconds < 0.5f)
        step = speed * deltaSeconds;

    const float yawRadians = g_yaw * kDegreesToRadians;
    const float cosYaw = cosf(yawRadians);
    const float sinYaw = sinf(yawRadians);
    // UE yaw 0 faces +X. Right is yaw + 90 degrees.
    const float forwardX = cosYaw;
    const float forwardY = sinYaw;
    const float rightX = -sinYaw;
    const float rightY = cosYaw;

    if (IsKeyDown('W')) {
        g_x += forwardX * step;
        g_y += forwardY * step;
    }
    if (IsKeyDown('S')) {
        g_x -= forwardX * step;
        g_y -= forwardY * step;
    }
    if (IsKeyDown('D')) {
        g_x += rightX * step;
        g_y += rightY * step;
    }
    if (IsKeyDown('A')) {
        g_x -= rightX * step;
        g_y -= rightY * step;
    }
    if (IsKeyDown(g_upVk))
        g_z += step;
    if (IsKeyDown(g_downVk))
        g_z -= step;
}

struct FrameSlot {
    bool newFrame;
    int index;
};

FrameSlot NextFrameSlot()
{
    static LARGE_INTEGER frequency = {};
    static LARGE_INTEGER last = {};
    static int index = 0;
    if (frequency.QuadPart == 0)
        QueryPerformanceFrequency(&frequency);

    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    bool newFrame = true;
    if (last.QuadPart != 0 && frequency.QuadPart != 0)
    {
        const double seconds = static_cast<double>(now.QuadPart - last.QuadPart)
            / static_cast<double>(frequency.QuadPart);
        newFrame = seconds >= kFrameGapSeconds;
    }
    last.QuadPart = now.QuadPart;
    if (newFrame)
        index = 0;
    else
        ++index;
    return {newFrame, index};
}

void Deactivate()
{
    ReleaseCursor();
    Touch_SetCursorPassthrough(false);
    Touch_SetMouseForwarding(true);
    g_active = false;
    g_pending = false;
    g_driveInput = false;
    g_firstManager = nullptr;
    g_haveFirstPose = false;
    Hud_SetHidden(false);
}

void Activate(void* cameraManager)
{
    SeedFromCache(cameraManager);
    g_active = true;
    Touch_ReleaseForwardedButtons();
    Touch_SetMouseForwarding(false);
    CaptureCursor();
    if (g_haveFirstPose)
    {
        LOG("Free camera on at %.0f, %.0f, %.0f  pitch %.1f yaw %.1f roll %.1f fov %.1f; earlier call pitch %.1f yaw %.1f roll %.1f",
            g_x, g_y, g_z, g_pitch, g_yaw, g_roll, g_fov, g_firstPitch, g_firstYaw, g_firstRoll);
    }
    else
    {
        LOG("Free camera on at %.0f, %.0f, %.0f  pitch %.1f yaw %.1f roll %.1f fov %.1f",
            g_x, g_y, g_z, g_pitch, g_yaw, g_roll, g_fov);
    }
    g_haveFirstPose = false;
    g_driveInput = true;
    Hud_SetHidden(true);
}

void DriveFrame(float deltaSeconds)
{
    ApplyMouseLook();
    ApplyLiveSpeed(deltaSeconds);
    ApplyFovKeys(deltaSeconds);
    ApplyMovement(deltaSeconds);
}

std::uintptr_t __fastcall DoUpdateCameraDetour(void* cameraManager, float deltaSeconds)
{
    const std::uintptr_t result = g_original != nullptr
        ? g_original(cameraManager, deltaSeconds)
        : 0;
    if (cameraManager == nullptr)
        return result;

    const FrameSlot slot = NextFrameSlot();
    // A frame that only updated one manager has no center slot. Seed from
    // that camera once the next frame shows the burst is over.
    if (slot.newFrame && g_pending && !g_active && g_callsInFrame == 1)
    {
        Activate(cameraManager);
        g_pending = false;
    }
    if (slot.newFrame)
        g_callsInFrame = 0;
    ++g_callsInFrame;

    const bool toggleDown = IsKeyDown(g_toggleVk);
    const bool escapeDown = IsKeyDown(VK_ESCAPE);
    const bool toggleEdge = toggleDown && !g_toggleWasDown;
    const bool escapeEdge = escapeDown && !g_escapeWasDown;
    g_toggleWasDown = toggleDown;
    g_escapeWasDown = escapeDown;

    if (g_active)
    {
        if (toggleEdge || escapeEdge)
        {
            Deactivate();
            return result;
        }
    }
    else if (toggleEdge)
    {
        g_pending = true;
    }

    if (!g_active)
    {
        if (!g_pending)
            return result;
        if (slot.index == 0)
        {
            auto* cachePov = static_cast<unsigned char*>(cameraManager) + kCameraCachePov;
            float ignoredX = 0.0f;
            float ignoredY = 0.0f;
            float ignoredZ = 0.0f;
            ReadPose(cachePov, ignoredX, ignoredY, ignoredZ, g_firstPitch, g_firstYaw, g_firstRoll);
            g_firstManager = cameraManager;
            g_haveFirstPose = true;
            return result;
        }
        if (slot.index != kCenterCallIndex)
            return result;
        Activate(cameraManager);
        g_pending = false;
    }

    // One integration per frame. The four split-screen calls must not stack.
    if (g_driveInput || slot.index == 0)
    {
        g_driveInput = false;
        DriveFrame(deltaSeconds);
    }
    if (g_firstManager != nullptr && g_firstManager != cameraManager)
        WriteActiveView(g_firstManager);
    g_firstManager = nullptr;
    WriteActiveView(cameraManager);
    return result;
}

void ReadConfig(const wchar_t* iniPath)
{
    g_toggleVk = VK_F2;
    g_fasterVk = VK_SHIFT;
    g_slowerVk = 0;
    g_upVk = VK_SPACE;
    g_downVk = VK_CONTROL;
    g_moveSpeed = 8.0f; // base; live speed starts at 10x (8 behaves as 80)
    g_fastMultiplier = 4.0f;
    g_slowMultiplier = 0.25f;
    g_mouseSensitivity = 0.15f;

    wchar_t text[64];
    GetPrivateProfileStringW(L"FreeCamera", L"Toggle", L"F2", text, 64, iniPath);
    g_toggleVk = ParseVirtualKey(text, VK_F2);
    GetPrivateProfileStringW(L"FreeCamera", L"Faster", L"SHIFT", text, 64, iniPath);
    g_fasterVk = ParseVirtualKey(text, VK_SHIFT);
    GetPrivateProfileStringW(L"FreeCamera", L"Slower", L"NONE", text, 64, iniPath);
    g_slowerVk = ParseVirtualKey(text, 0);
    GetPrivateProfileStringW(L"FreeCamera", L"Up", L"SPACE", text, 64, iniPath);
    g_upVk = ParseVirtualKey(text, VK_SPACE);
    GetPrivateProfileStringW(L"FreeCamera", L"Down", L"CTRL", text, 64, iniPath);
    g_downVk = ParseVirtualKey(text, VK_CONTROL);

    g_moveSpeed = static_cast<float>(GetPrivateProfileIntW(L"FreeCamera", L"MoveSpeed", 8, iniPath));

    GetPrivateProfileStringW(L"FreeCamera", L"FastMultiplier", L"4", text, 64, iniPath);
    g_fastMultiplier = ParseIniFloat(text, 4.0f);
    GetPrivateProfileStringW(L"FreeCamera", L"SlowMultiplier", L"0.25", text, 64, iniPath);
    g_slowMultiplier = ParseIniFloat(text, 0.25f);
    GetPrivateProfileStringW(L"FreeCamera", L"MouseSensitivity", L"0.15", text, 64, iniPath);
    g_mouseSensitivity = ParseIniFloat(text, 0.15f);
}

} // namespace

bool MatchesBoundKey(int bound, int key);

bool Freecam_BlocksKeyMessage(UINT message, WPARAM virtualKey)
{
    if (!g_active)
        return false;
    switch (message)
    {
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        break;
    default:
        return false;
    }

    const int key = static_cast<int>(virtualKey);
    if (key == 'W' || key == 'A' || key == 'S' || key == 'D' || key == VK_ESCAPE)
        return true;
    // Q and R are in-game cheat keys. The brackets are cabinet LeftBracket
    // and RightBracket. Swallow them only while this function is reached,
    // which is while the free camera is on.
    RefreshFovKeys();
    if (key == 'Q' || key == 'E' || key == 'R' || key == g_fovNarrowVk || key == g_fovWidenVk)
        return true;
    if (MatchesBoundKey(g_upVk, key) || MatchesBoundKey(g_downVk, key) ||
        MatchesBoundKey(g_fasterVk, key) || MatchesBoundKey(g_slowerVk, key))
        return true;
    return false;
}

bool MatchesBoundKey(int bound, int key)
{
    if (bound == 0 || key == 0)
        return false;
    if (bound == key)
        return true;
    const bool boundControl = bound == VK_CONTROL || bound == VK_LCONTROL || bound == VK_RCONTROL;
    const bool keyControl = key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL;
    if (boundControl && keyControl)
        return true;
    const bool boundShift = bound == VK_SHIFT || bound == VK_LSHIFT || bound == VK_RSHIFT;
    const bool keyShift = key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT;
    return boundShift && keyShift;
}

namespace features {

void Freecam_Install(const wchar_t* moduleDir)
{
    wchar_t iniPath[MAX_PATH];
    if (!BuildIniPath(moduleDir, iniPath, MAX_PATH))
        return;

    const int enabled = GetPrivateProfileIntW(L"FreeCamera", L"Enabled", 1, iniPath);
    if (enabled == 0)
    {
        LOG("WARN: free camera is off in config.ini");
        return;
    }

    ReadConfig(iniPath);

    const MH_STATUS initialised = MH_Initialize();
    if (initialised != MH_OK && initialised != MH_ERROR_ALREADY_INITIALIZED)
        return;

    const auto base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (base == nullptr)
        return;

    void* const target = base + kDoUpdateCameraRva;
    if (MH_CreateHook(target, &DoUpdateCameraDetour, reinterpret_cast<void**>(&g_original)) != MH_OK)
    {
        LOG("ERROR: free camera hook was not created");
        return;
    }
    MH_EnableHook(target);
    LOG("Free camera installed. Toggle VK 0x%02X", g_toggleVk);
}

} // namespace features
