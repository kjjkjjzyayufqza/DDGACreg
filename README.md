# DDGACreg

DDGACreg is a hook for Densha de Go!! AC (TG4AC 5.80.02). It loads as `version.dll` next to the game.

## Credit

The ultrawide display port is from [DDGUltrawide](https://github.com/percentcer/DDGUltrawide) by Spencer Miller (MIT, Copyright (c) 2026 Spencer Miller). MinHook is by Tsuda Kageyu (BSD, Copyright (c) 2009-2017). See `THIRD_PARTY_NOTICES.md`. This project also adds 16:9 and single-screen layouts, a coin key, a free camera, an in-process NESYS service, a keyboard NESiCA reader, HUD hiding, cinematic quality, full-line streaming, higher load budgets, and a local card, player, and leaderboard server.

## Where things are

| Path | Role |
| --- | --- |
| `dll/` | Game hook (`version.dll`) |
| `dll/src/display/` | DDGUltrawide display port (window, compositor, cabinet, touch) |
| `dll/src/features/` | Coin, card, layout, free camera, NESYS, HUD, quality, route, stream |
| `server/` | Localhost content program (`ddgac-content.exe`) and its SQLite database |
| `config.ini` | Settings copied next to the game. The content server reads the same file |
| `third_party/minhook/` | Vendored MinHook |
| `build.bat` | Builds `version.dll` and copies it next to the game |

## Install

Copy the Release `version.dll` and `config.ini` into `TG4AC\Binaries\Win64`, next to `TG4AC-Win64-Shipping.exe` (version 5.80.02). Do not replace the game executable. Remove `version.dll` to uninstall.

Start `ddgac-content.exe` before the game for cards, players, and leaderboards. A build from this tree listens on `127.0.0.1:8750` and stores `server/data/content.sqlite`. The DLL does not open that database.

The DLL runs the cabinet's NESYS service inside the game and sends the game's HTTP and card requests to that server. On the mode select screen the NESiCA mode button stays grey while the game waits for a card: press P to tap it. The card is selected on the content server and the button lights up.

The hook opens a console titled `DDGACreg` and writes `DDGACreg.log` beside the DLL. Closing the console does not exit the game.

## config.ini

- `[Input] Coin` — coin key, default `F3`. Other keys use virtual-key names such as `A` and `SPACE`.
- `[Output] WindowMode` — `borderless` or `windowed`. F11 toggles them.
- `[Game]` — `FitMonitor=1` sizes from the monitor. `CenterWidth` and `CenterHeight` are the center screen in pixels; the frame is twice that size. `ScreenScale` multiplies the monitor size when both center sizes are 0.
- `[LayoutMode] Preset` — `32:9`, `16:9`, or `single`. In `16:9` the center panel is the largest and the touch panel sits under it. Ctrl+1 left, Ctrl+2 center, Ctrl+3 right, Ctrl+4 touch. Ctrl+0 restores the multi-screen layout.
- `[FreeCamera]` — F2 toggles. WASD moves and the mouse looks. Live speed starts at 10x `MoveSpeed`. Hold Q to halve it each second, E to double it, R to snap back to that 10x base and to the FOV from when the camera turned on. Shift multiplies the live speed while held. Space rises and Ctrl descends. Hold `[` to narrow the FOV and `]` to widen it. F1, G, Enter, and the arrow keys stay with the game.
- `[Content] BaseUrl` — `http://127.0.0.1:8750`. Only the port is used. `UnlockAll=1` (read by `ddgac-content.exe`) releases every mission, difficulty, and real mode at NESiCA login and keeps the card's meister points, the shop currency, at 99999.
- `[Nesys] Enabled` — the in-game NESYS service that carries HTTP and card requests to `BaseUrl`. `Trace=1` writes every pipe message both ways to `DDGACreg.log`.
- `[Hud] Enabled` — while the free camera is on, hides every driving HUD widget (left, center, right, touch, fade, and now-loading). `Enabled=0` leaves the HUD up.
- `[Quality] Enabled` — forces cinematic scalability, then lifts the arcade cuts that level still leaves in place (view distance, motion blur, HLOD, shadow distance). `Enabled=0` installs nothing.
- `[Route] Enabled` — once a mission world is up, loads and shows every streaming level of the line, and queues every YMTc section on the Yamanote loop. Releases, unloads, and hidden preloads are refused until OutGame loads again. Weather and section lighting stay with the mission.
- `[Stream] Enabled` — raises the cabinet's per-frame load and streaming caps, and turns off the game's extra DWM vsync wait. `Enabled=0` leaves those caps alone.
- `[Card]` — `Key` (default `P`) taps the card while the game is reading; a press up to 3 seconds earlier also counts. `Id` is the 16-character card and must start with `707` or `709`.
- `[Layout]`, `[Source]`, `[Touch]`, `[Hotkeys]`, and `[TouchPanelWindow]` are DDGUltrawide settings. Comments in `config.ini` describe them.

## Build

Visual Studio 2022 x64, CMake 3.24+, Windows SDK.

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

Outputs:

- `build/dll/Release/version.dll`
- `build/server/Release/ddgac-content.exe`

`build.bat release` builds the Release DLL and copies `version.dll` next to the game executable. It does not build or copy the content server. `build.bat debug` does the same for a Debug DLL and its PDB.
