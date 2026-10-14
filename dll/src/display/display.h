#pragma once

// Implemented by the display port (DDGUltrawide). Starts the version.dll
// proxy hooks that must run inside DllMain, then the rest on a worker thread.
void Display_Install(const wchar_t* moduleDir);
