#pragma once

namespace nesys {

// Logs the NESYS_INIT result and every change of the NesysManager connection
// flags, checked after each NesysManager_Update on the game thread.
bool StartStateWatch();

// Marks the current play as a NESiCA play (NesysManager +684, read by
// NesysManager_IsUsedNESiCA). False when the manager does not exist yet.
bool MarkNesicaUsed();

}
