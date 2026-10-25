# DDGACreg

[![Build](https://github.com/kjjkjjzyayufqza/DDGACreg/actions/workflows/build.yml/badge.svg)](https://github.com/kjjkjjzyayufqza/DDGACreg/actions/workflows/build.yml)
[![Buy Me a Coffee](https://img.shields.io/badge/Buy%20Me%20a%20Coffee-FFDD00?style=flat&logo=buymeacoffee&logoColor=black)](https://buymeacoffee.com/kjjkjj)

DDGACreg is an unofficial hobby hook for a local copy of TG4AC 5.80.02. It loads as `version.dll` next to the game. It is not affiliated with, endorsed by, or connected to Densha de Go!! arcade machines, their operators, or the game's developers.

<a href="https://buymeacoffee.com/kjjkjj"><img src="https://cdn.buymeacoffee.com/buttons/v2/default-yellow.png" alt="Buy Me a Coffee" height="40"></a>

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
| `build.bat` | Builds `version.dll` and, when a game folder is given, copies it there |
| `.github/workflows/build.yml` | Windows CI: build Release and publish `DDGACreg-windows.zip` on the GitHub Release |

## Download

Each push to `main` publishes [DDGACreg-windows.zip](https://github.com/kjjkjjzyayufqza/DDGACreg/releases/latest/download/DDGACreg-windows.zip) on the GitHub Release. The zip contains only:

| Path | Role |
| --- | --- |
| `version.dll` | Copy next to `TG4AC-Win64-Shipping.exe` |
| `config.ini` | Same folder as the DLL. `server\ddgac-content.exe` reads it by walking up from its own folder |
| `server\ddgac-content.exe` | Local content server. Start it before the game and leave the window open |
| `README.md` | This file |

The zip has no object files, debug symbols, or source tree.

1. Unzip the archive.
2. Copy `version.dll` and `config.ini` into `TG4AC\Binaries\Win64`. Do not replace the game executable.
3. From the unzipped folder, run `server\ddgac-content.exe`.
4. Start `TG4AC-Win64-Shipping.exe`.

Remove `version.dll` to uninstall.

## Run locally

Windows x64, Visual Studio 2022 with the Desktop development with C++ workload, CMake 3.24 or newer, and the Windows SDK (`fxc.exe` embeds the display shaders). `cmake` must be on `PATH`.

### 1. Build

From the repository root:

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

`build.bat release` builds only the Release DLL. `build.bat debug` builds the Debug DLL and its PDB.

Outputs:

- `build\dll\Release\version.dll`
- `build\server\Release\ddgac-content.exe`

GitHub Actions runs the same Release build on `windows-2022` and publishes those two programs, `config.ini`, and this README as `DDGACreg-windows.zip`.

### 2. Install the hook

Copy `version.dll` and `config.ini` into the folder that contains `TG4AC-Win64-Shipping.exe`:

```text
TG4AC\Binaries\Win64
```

Do not replace the game executable. Remove `version.dll` to uninstall.

To copy during the build, pass that folder, or set `DDGAC_GAME_DIR` to it:

```bat
build.bat release "C:\path\to\TG4AC\Binaries\Win64"
```

```bat
set DDGAC_GAME_DIR=C:\path\to\TG4AC\Binaries\Win64
build.bat release
```

A gitignored `build.local.bat` in the repository root may `set "DEST=..."` to the same folder. That file is the place for a machine path. Do not commit it.

`build.bat` also deletes `winmm.dll` and a `scripts` folder in the game directory (an old Ultimate ASI Loader). It leaves `dwmapi.dll` (UE4SS) in place. With no game folder set, it builds and does not copy.

### 3. Start the content server

Start this before the game, and leave the window open:

```bat
build\server\Release\ddgac-content.exe
```

The server walks upward from its own folder until it finds `config.ini`. It listens on `127.0.0.1:8750` (the port in `[Content] BaseUrl`) and stores `server\data\content.sqlite` when launched from a build tree. It refuses any host that is not loopback. The DLL does not open that database.

### 4. Launch the game

Start `TG4AC-Win64-Shipping.exe` (version 5.80.02). The hook opens a console titled `DDGACreg` and writes `DDGACreg.log` beside the DLL. Closing the console does not exit the game.

The DLL runs the cabinet's NESYS service inside the game and sends the game's HTTP and card requests to that server. On the mode select screen the NESiCA mode button stays grey while the game waits for a card: press P to tap it. The card is selected on the content server and the button lights up.

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
