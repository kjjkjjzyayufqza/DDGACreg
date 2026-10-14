// Copyright (c) 2026 Spencer Miller
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// Adapted for DDGACreg from https://github.com/percentcer/DDGUltrawide
#pragma once
#include <windows.h>

// Mouse/touch forwarding from the output window to the game window.
// Clicks on the configured regions of the output are mapped to the matching
// point in the game's own frame, and GetCursorPos / WindowFromPoint are hooked
// so the game sees the cursor at that point.
bool InstallTouchHooks();

// window: kMainWindow or kPanelWindow (config.h)
void TouchSetOutputWindow(int window, HWND out);
void TouchSetGameWindow(HWND game);

// Called from the output window's WndProc. Returns true if the message was handled.
bool TouchHandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT& result);

// Drop a press that was forwarded to the game. Layout and window-mode changes
// can lose the matching button-up; the game then ignores later clicks.
void Touch_ReleaseForwardedButtons();

// The visible output window, or null before it exists.
HWND Touch_MainOutputWindow();

// While passthrough is set, GetCursorPos and SetCursorPos are not remapped.
// The free camera recenters the real cursor; remapping that reads as a huge
// turn every frame.
void Touch_SetCursorPassthrough(bool passthrough);

// While false, mouse messages on the output window are not forwarded.
void Touch_SetMouseForwarding(bool enabled);
