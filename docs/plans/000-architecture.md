# Architecture

DDGACreg is a local mod for Densha de Go!! AC (TG4AC 5.80.02). The repository
root is the project. There is no nested solution folder.

Two programs share this repo and nothing else:

| Tree | Process | Job |
| --- | --- | --- |
| `dll/` | `version.dll` loaded by `TG4AC-Win64-Shipping.exe` | MinHook hooks inside the game |
| `server/` | `ddgac-content.exe` | SQLite content backend on localhost |

The DLL is not an ASI plugin and is not loaded by Ultimate ASI Loader. It is a
`version.dll` proxy, the same non-ASI pattern as
[DDGUltrawide](https://github.com/percentcer/DDGUltrawide) and the proxy-DLL
mods that sit next to a game executable. MinHook lives in `third_party/minhook`.

Game binaries used for research live in a local `TG4AC\Binaries\Win64` folder
next to `TG4AC-Win64-Shipping.exe`. Do not commit that path.

## Load path

Windows loads `version.dll` from the game directory before `System32`.
`dll/src/dllmain.cpp` forwards the real `version.dll` exports, then installs
hooks. Display code is a credited port of DDGUltrawide under
`dll/src/display/`. Feature modules live under `dll/src/features/` and must not
edit each other's files.

## File ownership

| Owner | Paths |
| --- | --- |
| Display port | `dll/src/display/**`, `dll/src/shaders/**` |
| Coin and key config | `dll/src/features/coin/**` |
| Layout presets and screen focus | `dll/src/features/layout/**` |
| Free camera | `dll/src/features/freecam/**` |
| Content client | `dll/src/features/content/**` |
| Content server | `server/**` |
| Orchestration | `dll/src/dllmain.cpp`, root `CMakeLists.txt`, `config.ini`, `README.md` |

Feature modules export the headers below. `dllmain.cpp` is the only file that
calls all of them.

## config.ini

One INI ships next to the DLL. Sections:

- `[Input]` coin and other keys. Default coin key is `F3`.
- `[LayoutMode]` `32:9`, `16:9`, or `single`, plus the focused screen.
- `[FreeCamera]` enable toggle and movement keys.
- `[Content]` base URL of the local server, default `http://127.0.0.1:8750`.
- Display sections (`[Output]`, `[Layout]`, `[Source]`, `[Touch]`, `[Game]`,
  `[Hotkeys]`, `[TouchPanelWindow]`) keep DDGUltrawide's names so its comments
  still apply. `[LayoutMode]` overrides how those rects are used.

## Content protocol

The server is the game's "content" stand-in. The DLL redirects the game's
HTTP content host to `[Content] BaseUrl`. Bodies are stored in SQLite tables
`cards`, `players`, `player_data`, and `leaderboards`. See
`docs/plans/001-content-api.md` once the server agent fills the route list
from the IDA string survey.

## Research log

Every IDA session and design change is appended under `docs/process/`. Plans
stay in `docs/plans/`. Do not put research notes only in chat.
