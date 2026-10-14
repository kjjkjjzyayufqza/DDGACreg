#pragma once

// Hide or restore the in-game HUD. Hud_Install reads [Hud] Enabled; when that
// is 0, Hud_SetHidden does nothing. Calls made before Hud_Install finishes
// are ignored. If the world or the widgets do not exist yet, the request is
// kept and applied on a later frame. A second true does not replace the
// visibility saved on the first hide. Free camera passes true on activate
// and false on deactivate.
void Hud_SetHidden(bool hidden);
