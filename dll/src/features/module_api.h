#pragma once

// Stable entry points for feature modules.
// dllmain.cpp is the only caller. Feature agents implement these and do not
// edit this header or each other's directories.

namespace features {

// [Input] Default coin key is F3. Reads config.ini from moduleDir.
void Coin_Install(const wchar_t* moduleDir);

// [Card] Keyboard NESiCA reader. The tap key (default P) completes the read the
// game starts on its keyboard device.
void Card_Install(const wchar_t* moduleDir);

// [LayoutMode] 32:9, 16:9, and single-screen focus (Ctrl+1..4, Ctrl+0 restores).
void Layout_Install(const wchar_t* moduleDir);

// [FreeCamera] WASD, mouse look, Shift, Ctrl, Space, descend.
void Freecam_Install(const wchar_t* moduleDir);

// [Nesys] Local NESYS service on the nesys_games pipe. Bridges the game's
// HTTP and card requests to the content server.
void Nesys_Install(const wchar_t* moduleDir);

// [Hud] Hide every in-game HUD widget. Free camera calls Hud_SetHidden.
void Hud_Install(const wchar_t* moduleDir);

// [Quality] Force the highest scalability and LOD settings.
void Quality_Install(const wchar_t* moduleDir);

// [Route] Keep the whole Yamanote line loaded instead of one mission section.
void Route_Install(const wchar_t* moduleDir);

// [Stream] Lift arcade loading and streaming budgets.
void Stream_Install(const wchar_t* moduleDir);

}
