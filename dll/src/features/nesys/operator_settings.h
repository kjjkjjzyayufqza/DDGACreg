#pragma once

namespace nesys {

// Overrides the game settings that keep NESYS off: WholeGameConfig DisableNESYS
// (DefaultGame.ini) and the HardSaveGame_C operator settings IsNesysUsed /
// IsUsedNESiCATime. Hooks the NesysManager constructor and the save-object
// calls, and rescans on its own thread.
bool StartOperatorSettings();

}
