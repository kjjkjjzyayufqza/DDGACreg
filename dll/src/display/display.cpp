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

// DDGUltrawide's DllMain, folded into Display_Install. The real DllMain lives in
// dllmain.cpp and calls this before the other feature installs. Order matches
// upstream: log, real version.dll, config, MinHook, game-window and command-line
// hooks, then compositor and touch on a worker thread.

#include "display/display.h"

#include "cmdline.h"
#include "compositor.h"
#include "config.h"
#include "gamewindow.h"
#include "log.h"
#include "proxy.h"
#include "touch.h"
#include "version.h"

#include <windows.h>
#include <MinHook.h>
#include <string>

namespace
{
    DWORD WINAPI InitThread(LPVOID)
    {
        ResolveOutput();
        LOG("Startup: installing the compositor");
        InstallCompositor();
        InstallTouchHooks();
        return 0;
    }
}

void Display_Install(const wchar_t* moduleDir)
{
    const std::wstring dir = moduleDir ? moduleDir : L"";

    logx::Init(dir + L"DDGACreg.log");
    LOG("DDGUltrawide " DDGU_VERSION_STRING " loaded into %p", GetModuleHandleW(nullptr));

    // Must happen before the game calls any version.dll function.
    if (!LoadOriginalVersionDll())
    {
        LOG("FATAL: could not load the real version.dll");
        return;
    }

    LoadConfig(dir + L"config.ini");

    const MH_STATUS mh = MH_Initialize();
    if (mh != MH_OK && mh != MH_ERROR_ALREADY_INITIALIZED)
    {
        LOG("MH_Initialize failed; running without hooks");
        return;
    }

    // These have to be in place before the game's startup code reads its command
    // line or creates its window, so they're installed right here.
    // Preliminary size so the window lock is installed before the game asks for
    // its command line. EditCommandLine resolves the real monitor after that.
    ResolveRenderSize();
    if (g_cfg.renderW > 0 && g_cfg.renderH > 0)
        InstallGameWindowHook();
    InstallCommandLineHook(g_cfg.renderW, g_cfg.renderH, g_cfg.renderFitMonitor, g_cfg.extraCommandLine);

    // The remaining hooks go on a separate thread to keep work under the loader lock small.
    const HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    if (t)
        CloseHandle(t);
}
