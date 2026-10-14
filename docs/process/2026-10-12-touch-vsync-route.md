# Touch, frame pacing, and whole-line route — 2026-10-12

Target stays TG4AC 5.80.02, IDA instance `ida-47892`, image base `0x140000000`.
The game was not run in this session; the log of 2026-10-12 01:08 was read.

## Touch panel drops taps

That log has `Game window brought to the foreground` after nearly every tap
(about forty in a minute). `TakeForeground` logs only when the game was not
the foreground window, so each tap moved the foreground off the game. The
game's buttons fire on release; a deactivation between the press and the
release cancels the press, and the tap does nothing.

`WM_MOUSEACTIVATE` already answered `MA_NOACTIVATE`. The main output window
now also has `WS_EX_NOACTIVATE`, the documented way to keep a clicked
top-level window from becoming the foreground. Alt+Tab and the taskbar still
activate it, and its `WM_ACTIVATE` hands focus to the game as before.

After that change the per-tap foreground loss was gone, but touches stopped
after a few Alt+Tabs even though every refocus logged success. The cloaked
game window now never sees its deactivation: `WM_ACTIVATE` (`WA_INACTIVE`),
`WM_ACTIVATEAPP` (false), and `WM_KILLFOCUS` go to `DefWindowProcW` instead
of the game. Activation still reaches it, which refreshes modifier keys. The
touch hook also logs when the game has moved the cursor 10, 100, and 1000
times; a count that climbs every frame means the game switched to relative
(high-precision) mouse input.

## Camera judder (free camera)

Two vblank waits per frame:

- The cabinet sets `r.VSyncDWM` to 1 after each load screen
  (`sub_140203080`). The D3D11 viewport then waits on the DWM before its
  Present. `r.VSyncDWM` is an int ref cvar registered by `sub_1400C6580`
  (value `dword_142F1FAD0`, RVA `0x2F1FAD0`; object `qword_142F20148`,
  RVA `0x2F20148`). `stream.cpp` now forces it to 0.
- The compositor presents the output with `[Output] VSync`, then calls the
  game's own Present with the game's sync interval. The game frame is cloaked,
  so that second wait shows nothing. The game chain now presents with 0; the
  first non-zero interval is logged.

The free camera also dropped any mouse step over 300 pixels, which froze the
view for a frame on a fast flick at a low frame rate. The cursor is clipped to
the output and recentered every frame, so only a step larger than half the
clip is treated as a remapped cursor now.

## Whole-line route

The mission keeps only the current station and its neighbors. Besides
`LevelRelease` / `LevelReleaseStart`, the blueprint calls
`UGameplayStatics::UnloadStreamLevel` (`sub_1412DC5B0`, exec `sub_1417ABF80`)
and preloads neighbors with `UGameplayStatics::LoadStreamLevel`
(`sub_1412D0AC0`, exec `sub_14177B050`) and `bMakeVisibleAfterLoad` false.
The old hook only refused the manager release, expanded once per process, and
recognized Yamanote names only.

Now, once the world (from `UObject::GetWorld`, vtable slot 36) lists
`/Maps/InGame/` levels:

- every listed level is loaded and shown, and the YMTc packages are queued
  when the world is the Yamanote loop;
- releases are refused, an unload is redirected to `NAME_None` (the
  `FStreamLevelAction` finds no level and completes, so the blueprint does
  not stall), and a hidden preload is made visible;
- levels with `Lighting` in the name (weather scenarios, section lighting)
  are left to the mission, so only its weather shows;
- loading `OutGame` resets the state, so the next mission expands its own line.

Not proven live: whether every line's persistent world lists all of its
sections. The log shows `Route: whole line requested (N streaming names)` and
`Route: showed N streaming levels` when a mission starts.
