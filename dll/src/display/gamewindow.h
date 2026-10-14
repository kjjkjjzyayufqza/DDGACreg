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

// Lets the game's window grow larger than the screen: Windows normally limits a
// window to about the desktop's size, which would stop the game's 3840x2160
// window on smaller monitors. Hooks CreateWindowExW so the game's windows are
// adjusted from the moment they're created.
bool InstallGameWindowHook();

// Moves the game's window so the part of its frame holding the touch screens
// is on the desktop (and keeps it there): the game reads the cursor's desktop
// position while driving, and clamps it to the desktop, so a touch screen
// hanging off it wouldn't take touches below the desktop's edge. The window is
// cloaked, so that placement does not show. The output window is not owned by
// it: cloaking an owner also cloaks every owned window.
void PlaceGameWindow(HWND game);

// The output window to keep directly above the game window, without making it
// topmost. Pass null to stop.
void SetOutputCoverWindow(HWND cover);

// Shows the game window again. Used when the output window cannot be shown.
void UncloakGameWindow(HWND game);

// Makes the game's window the foreground window (on its own thread, soon), so
// keys go to the game: Alt+F4 otherwise closes whatever had the focus before.
void FocusGameWindow(HWND game);
