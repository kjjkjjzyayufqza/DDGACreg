#include "display/display.h"
#include "features/module_api.h"

#include <windows.h>

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;

    DisableThreadLibraryCalls(module);

    wchar_t path[MAX_PATH];
    const DWORD len = GetModuleFileNameW(module, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        return TRUE;

    wchar_t* slash = path + len;
    while (slash > path && slash[-1] != L'\\' && slash[-1] != L'/')
        --slash;
    *slash = L'\0';

    // Display installs the version.dll forwarder before any other hook.
    // Feature installs only register their own MinHook callbacks.
    Display_Install(path);
    features::Coin_Install(path);
    features::Card_Install(path);
    features::Layout_Install(path);
    features::Freecam_Install(path);
    features::Nesys_Install(path);
    features::Hud_Install(path);
    features::Quality_Install(path);
    features::Route_Install(path);
    features::Stream_Install(path);
    return TRUE;
}
