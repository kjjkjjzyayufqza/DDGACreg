#pragma once

namespace features {

// [Stream] Raise arcade per-frame loading and streaming budgets.
// Enabled=0 leaves the cabinet values in place.
void Stream_Install(const wchar_t* moduleDir);

}
