// Copyright (c) 2026 Spencer Miller
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// Adapted for DDGACreg from https://github.com/percentcer/DDGUltrawide
#include "gamewindow.h"
#include "config.h"
#include "features/freecam/freecam.h"
#include "log.h"

#include <windows.h>
#include <MinHook.h>
#include <cwchar>
#include <mutex>
#include <unordered_map>

namespace
{
    using CreateWindowExWFn = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int,
                                            HWND, HMENU, HINSTANCE, LPVOID);
    CreateWindowExWFn g_origCreateWindowExW = nullptr;

    std::mutex g_mutex;
    std::unordered_map<HWND, WNDPROC> g_origProcs;
    std::unordered_map<HWND, bool> g_locked;    // top-level: its client area is kept at the game's render size
    bool g_placed = false;                      // PlaceGameWindow: keep its position too
    const UINT kFocusMessage = RegisterWindowMessageW(L"DDGUltrawide.FocusGameWindow");

    // DWMWA_CLOAK. The raw frame stays a real window (the game clamps the cursor
    // to the desktop, so the touch quadrant has to keep a desktop position) but
    // must not be composed. On a monitor smaller than 3840x2160 that quadrant
    // lands on the visible desktop, and a flip-model present draws it on top of
    // the output window.
    constexpr DWORD kDwmCloak = 13;

    using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    DwmSetWindowAttributeFn g_dwmSetAttribute = nullptr;
    bool g_cloakLogged = false;
    bool g_deactivationLogged = false;
    HWND g_cover = nullptr;

    bool IsDeactivation(UINT msg, WPARAM wp)
    {
        return (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE)
            || (msg == WM_ACTIVATEAPP && wp == FALSE)
            || msg == WM_KILLFOCUS;
    }

    void CloakGameWindow(HWND hwnd)
    {
        if (!hwnd)
            return;
        if (!g_dwmSetAttribute)
        {
            wchar_t path[MAX_PATH] = {};
            const UINT n = GetSystemDirectoryW(path, MAX_PATH);
            if (n > 0 && n + 12 < MAX_PATH)
            {
                wcscat_s(path, L"\\dwmapi.dll");
                if (const HMODULE module = LoadLibraryW(path))
                    g_dwmSetAttribute = reinterpret_cast<DwmSetWindowAttributeFn>(
                        GetProcAddress(module, "DwmSetWindowAttribute"));
            }
        }
        if (!g_dwmSetAttribute)
        {
            if (!g_cloakLogged)
            {
                g_cloakLogged = true;
                LOG("WARN: could not cloak the game window (dwmapi unavailable)");
            }
            return;
        }

        const BOOL cloak = TRUE;
        const HRESULT hr = g_dwmSetAttribute(hwnd, kDwmCloak, &cloak, sizeof(cloak));
        if (!g_cloakLogged)
        {
            g_cloakLogged = true;
            if (SUCCEEDED(hr))
                LOG("Game window cloaked; the raw frame stays off the desktop");
            else
                LOG("WARN: cloaking the game window failed (%08lx)", static_cast<unsigned long>(hr));
        }
    }

    // Brings the window to the foreground (on its own thread). Windows only
    // lets a process take the foreground in some situations, so this briefly
    // shares input with the thread that has it.
    void TakeForeground(HWND hwnd)
    {
        HWND fg = GetForegroundWindow();
        if (fg == hwnd) return;
        const DWORD me = GetCurrentThreadId();
        const DWORD them = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
        const bool attached = them && them != me && AttachThreadInput(me, them, TRUE);
        ShowWindow(hwnd, SW_SHOW);
        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
        SetActiveWindow(hwnd);
        SetFocus(hwnd);
        if (attached) AttachThreadInput(me, them, FALSE);
        LOG(GetForegroundWindow() == hwnd ? "Game window brought to the foreground"
                                          : "Could not bring the game window to the foreground");
    }

    // Where the game window's client area goes (desktop pixels): with the
    // touch panel's part of the frame at the top left of the output that
    // shows it (the panel's own window, or the main one)
    POINT ClientOrigin()
    {
        float x0 = 0.5f, y0 = 0.5f;                                      // (the touch panel's quadrant)
        int width = g_cfg.outW;
        int height = g_cfg.outH;
        int dx = g_cfg.outX;
        int dy = g_cfg.outY;
        // Follow the live output. F11 and dragging the window move it; a stale
        // origin leaves the touch quadrant off the desktop, and the game then
        // clamps the cursor away from the click.
        if (g_cover && IsWindow(g_cover))
        {
            POINT client = { 0, 0 };
            RECT cr = {};
            if (ClientToScreen(g_cover, &client))
            {
                dx = client.x;
                dy = client.y;
            }
            if (GetClientRect(g_cover, &cr) && cr.right > 0 && cr.bottom > 0)
            {
                width = cr.right;
                height = cr.bottom;
            }
        }
        for (const Placement& p : Placements(kMainWindow, width, height))
            if (p.screen == kTouchPanelScreen) { x0 = p.source.originX; y0 = p.source.originY; }
        const bool own = g_cfg.panelWindow && g_cfg.panelW > 0 && g_cfg.panelH > 0;
        if (own)
        {
            dx = g_cfg.panelX;
            dy = g_cfg.panelY;
        }
        return { dx - static_cast<LONG>(x0 * g_cfg.renderW), dy - static_cast<LONG>(y0 * g_cfg.renderH) };
    }

    LRESULT CALLBACK SubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        WNDPROC orig = nullptr;
        bool locked = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_origProcs.find(hwnd);
            if (it != g_origProcs.end()) orig = it->second;
            auto lk = g_locked.find(hwnd);
            if (lk != g_locked.end()) locked = lk->second;
            if (msg == WM_NCDESTROY && it != g_origProcs.end()) g_origProcs.erase(it);
            if (msg == WM_NCDESTROY && lk != g_locked.end()) g_locked.erase(lk);
        }
        if (!orig) return DefWindowProcW(hwnd, msg, wp, lp);
        // Alt+F4 must close the game even while this window is cloaked and
        // standing behind the output. The layout keys do not use F4.
        if (locked && msg == WM_SYSKEYDOWN && wp == VK_F4 && (lp & (1 << 29)))
        {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        // Free camera reads these with GetAsyncKeyState. Swallowing the
        // message keeps the game's own binding for that key from running
        // until the free camera is turned off.
        if (locked && Freecam_BlocksKeyMessage(msg, wp))
            return 0;
        // Keep it out of Alt+Tab and the taskbar, whatever styles the game
        // sets on it later (see Hook_CreateWindowExW)
        if (msg == WM_STYLECHANGING && wp == static_cast<WPARAM>(GWL_EXSTYLE) && lp && locked)
        {
            auto* st = reinterpret_cast<STYLESTRUCT*>(lp);
            st->styleNew = (st->styleNew | WS_EX_TOOLWINDOW) & ~WS_EX_APPWINDOW;
        }
        // The cloaked frame stands behind the output windows, so the game is
        // never told it lost the foreground (Alt+Tab, another app, a click
        // that activates something else). Its deactivation handling reset the
        // pointer state, and touches stopped reaching the panel after a few
        // Alt+Tabs. Activation still reaches it and refreshes modifier keys.
        if (locked && IsDeactivation(msg, wp))
        {
            if (!g_deactivationLogged)
            {
                g_deactivationLogged = true;
                LOG("Game window deactivation kept from the game (Alt+Tab and focus changes)");
            }
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        if (msg == kFocusMessage && kFocusMessage)
        {
            TakeForeground(hwnd);
            return 0;
        }
        if (msg == WM_SHOWWINDOW && wp && locked)
            CloakGameWindow(hwnd);

        // Keep its client area at the game's render size: the game renders at
        // its window's size, and window managers (such as PowerToys
        // FancyZones) can resize it, which squashes or shifts the game's
        // screens in its frame. The window's outer size follows from its
        // current style (the game changes it). Moving and minimizing are left alone.
        if (msg == WM_WINDOWPOSCHANGING && lp && locked && g_cfg.renderW > 0 && g_cfg.renderH > 0 && !IsIconic(hwnd))
        {
            auto* wpos = reinterpret_cast<WINDOWPOS*>(lp);
            RECT want = { 0, 0, g_cfg.renderW, g_cfg.renderH };
            AdjustWindowRectEx(&want, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE,
                               static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)));
            const int cx = want.right - want.left, cy = want.bottom - want.top;
            if (g_placed && !(wpos->flags & SWP_NOMOVE))
            {
                const POINT o = ClientOrigin();
                wpos->x = o.x + want.left;
                wpos->y = o.y + want.top;
            }
            if (!(wpos->flags & SWP_NOSIZE) && (wpos->cx != cx || wpos->cy != cy))
            {
                static int logged = 0;
                if (logged++ < 5)
                    LOG("Game window resize to %dx%d changed to %dx%d (a %dx%d client area)",
                        wpos->cx, wpos->cy, cx, cy, g_cfg.renderW, g_cfg.renderH);
                wpos->cx = cx;
                wpos->cy = cy;
            }
        }

        const LRESULT r = CallWindowProcW(orig, hwnd, msg, wp, lp);
        if (msg == WM_GETMINMAXINFO && lp)
        {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            if (mm->ptMaxTrackSize.x < 16384) mm->ptMaxTrackSize.x = 16384;
            if (mm->ptMaxTrackSize.y < 16384) mm->ptMaxTrackSize.y = 16384;
        }
        // Keep the output directly above this window. HWND_TOPMOST is not used:
        // other applications can still cover the output. A cloaked window still
        // occupies z-order, so the output has to sit above it or that region
        // shows the desktop. Async so this window procedure cannot deadlock
        // against the output window's thread.
        if (msg == WM_WINDOWPOSCHANGED && locked && g_cover && IsWindow(g_cover)
            && GetWindow(hwnd, GW_HWNDPREV) != g_cover)
            SetWindowPos(g_cover, hwnd, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        return r;
    }

    HWND WINAPI Hook_CreateWindowExW(DWORD exStyle, LPCWSTR cls, LPCWSTR name, DWORD style, int x, int y,
                                     int w, int h, HWND parent, HMENU menu, HINSTANCE inst, LPVOID param)
    {
        // The game's main window stays out of Alt+Tab and the taskbar: our
        // output window stands in for it there (so its thumbnail shows the
        // output, not the game's raw frame). Set before it's ever shown.
        const bool unreal = cls && HIWORD(reinterpret_cast<ULONG_PTR>(cls)) != 0 && wcscmp(cls, L"UnrealWindow") == 0;
        if (unreal && !parent && !(style & WS_CHILD))
            exStyle = (exStyle | WS_EX_TOOLWINDOW) & ~WS_EX_APPWINDOW;

        HWND hwnd = g_origCreateWindowExW(exStyle, cls, name, style, x, y, w, h, parent, menu, inst, param);
        if (!hwnd) return hwnd;

        wchar_t className[64] = {};
        GetClassNameW(hwnd, className, 64);
        if (wcscmp(className, L"UnrealWindow") != 0) return hwnd;   // only the engine's own windows

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto prev = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&SubclassProc)));
            if (!prev) return hwnd;
            g_origProcs[hwnd] = prev;
            g_locked[hwnd] = !parent;
        }
        LOG("Game window %p created (%dx%d); size limit lifted", hwnd, w, h);

        // (Outside the lock: changing its style calls back into SubclassProc)
        if (!parent && !(style & WS_CHILD))
        {
            const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
            const LONG_PTR want = (ex | WS_EX_TOOLWINDOW) & ~WS_EX_APPWINDOW;
            if (ex != want) SetWindowLongPtrW(hwnd, GWL_EXSTYLE, want);
            CloakGameWindow(hwnd);
            LOG("  kept out of Alt+Tab and the taskbar (extended style %08llx -> %08llx)",
                static_cast<unsigned long long>(ex), static_cast<unsigned long long>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)));
        }
        return hwnd;
    }
}

void SetOutputCoverWindow(HWND cover)
{
    g_cover = cover;
}

void UncloakGameWindow(HWND hwnd)
{
    if (!hwnd || !g_dwmSetAttribute)
        return;
    const BOOL cloak = FALSE;
    g_dwmSetAttribute(hwnd, kDwmCloak, &cloak, sizeof(cloak));
    LOG("Game window uncloaked");
}

void PlaceGameWindow(HWND game)
{
    if (!game || g_cfg.renderW <= 0 || g_cfg.renderH <= 0) return;
    g_placed = true;
    const POINT o = ClientOrigin();
    RECT want = { 0, 0, g_cfg.renderW, g_cfg.renderH };
    AdjustWindowRectEx(&want, static_cast<DWORD>(GetWindowLongPtrW(game, GWL_STYLE)), FALSE,
                       static_cast<DWORD>(GetWindowLongPtrW(game, GWL_EXSTYLE)));
    const int x = o.x + want.left;
    const int y = o.y + want.top;
    static bool haveLast = false;
    static int lastX = 0;
    static int lastY = 0;
    if (haveLast && x == lastX && y == lastY)
        return;
    haveLast = true;
    lastX = x;
    lastY = y;
    // Asynchronously: this runs on our output windows' thread, and the game's
    // thread (which owns its window) may be waiting on ours, presenting to our
    // windows; waiting on it back would deadlock
    SetWindowPos(game, nullptr, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_ASYNCWINDOWPOS );
    CloakGameWindow(game);
    static int logged = 0;
    if (logged < 4)
    {
        ++logged;
        LOG("Game window placed with its client area at %ld,%ld (the touch screens on the desktop)", o.x, o.y);
    }
}

void FocusGameWindow(HWND game)
{
    if (game && kFocusMessage) PostMessageW(game, kFocusMessage, 0, 0);
}

bool InstallGameWindowHook()
{
    const bool ok =
        MH_CreateHookApi(L"user32", "CreateWindowExW", reinterpret_cast<void*>(&Hook_CreateWindowExW),
                         reinterpret_cast<void**>(&g_origCreateWindowExW)) == MH_OK &&
        MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
    LOG(ok ? "Game window hook installed" : "Game window hook FAILED");
    return ok;
}
