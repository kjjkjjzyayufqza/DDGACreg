#pragma once

#include <windows.h>

// True while the free camera is driving the view. Key messages it owns are
// not delivered to the game for that stretch, so a cheat bound to the same
// key does not run at the same time. While it is on, that set also includes
// Q, E, R, and the bracket keys. Those still reach the game when it is off.
bool Freecam_BlocksKeyMessage(UINT message, WPARAM virtualKey);
