# Normal mode mission list — 2026-10-30

Target is TG4AC 5.80.02, image base `0x140000000`. Confirmed in game on
2026-10-30: after a card swipe, いつでもGOモード can open Yamanote and the
stations stay on screen. Offline with no NESYS was already fine. NESiCA mode
was already fine.

## Symptom

The line buttons are drawn. Choosing Yamanote opens an empty list and the
screen returns. `DDGACreg.log` for the 00:28 run is the evidence.

Startup `GetMission` on mission object `0000024D60AB3A00` saw 106 rows.
`UHttp_SetupInformation` then sent `mission` for `train_line` 1 through 8.
Each reply was result 0 and 0 bytes. The next `GetMission` on the same object
saw 0 rows. At 00:28:50, entering the line called `GetNormalMission` and
`GetNormalMissionByCondition` on that object, both with `table=0`.

## What the line screen does not call

These hooks loaded and never ran while the line was opened:

- `CheckReleaseMission` (`sub_1401B5B30`) and `GetPlayableMission`
  (`sub_1401C34B0`), which search only the release map at player+0xC8.
  Login never fills that map.
- `GetReleaseMissionList`. The player native `sub_140225F20` (exec
  `0x1402F3A40`) loads player+0x68. `sub_140226000` (also exec `0x1402F3A90`)
  copies mission+0x3C8, count at +0x3D0, stride 0x30. Filling that copy does
  not feed this screen.

`GetPlayableMissionData` ran on a different object and reported 1472 playable
rows (368 missions times 4 difficulties). That is the NESiCA list.

## The table the line screen reads

`GetNormalMission` (exec `0x1402F17C0`, inner `sub_1401C2690`) copies the
TArray at mission+0x168. The count is the int at +0x170. Element stride is
0xA0. `GetNormalMissionByCondition` (`sub_1401C26B0`) walks the same array and
appends rows that pass its filters. `GetAllMissionByCondition`
(`sub_1401BEB00`) uses it too. `isTrainLineTypeMission` (`sub_1401DA7F0`)
walks +0x168 and +0x178, stride 0xA0, and compares the byte at row+0x6D with
the line type.

The 106 rows are the missions built into the game. They already have the
station text. An empty replacement has nothing to show, so the widget closes.

## Why a declined mission reply clears them

`ApplyMissionData` (`sub_1401C6C60`, blueprint exec `0x1402FD1E0`) is
`void(this, incoming)`. Its tail copies four arrays from `incoming` onto
`this`:

| Dest | Source | Copy |
| --- | --- | --- |
| +0x168 | +0x168 | `sub_1401AEDE0` |
| +0x188 | +0x188 | `sub_1401AEDE0` |
| +0x198 | +0x198 | `sub_1401AF890` |
| +0x178 | +0x178 | `sub_1401AEDE0` |

A result line other than 1 still reaches this function with an empty incoming
table. The copy replaces the 106 built-in rows with 0. A result of 1 whose
body has only `mission_value` and empty strings does the same and also wipes
the text in each row (`sub_1401B1DD0` / `sub_1401B01C0`). Offline play never
sends `mission`, so the built-in rows stay.

The content server still declines `mission`. It does not invent station text.

## Fix

`version.dll` hooks `sub_1401C6C60`. When the incoming count at +0x170 is 0
or the pointer is null, the hook returns and leaves the built-in tables.
A non-empty incoming table is still applied. The log line is
`ApplyMissionData kept N built-in rows; incoming count M`.

The older catalog-on-map-miss hooks and the empty-profile list synthesis stay.
They are not what puts stations on the Yamanote screen.
