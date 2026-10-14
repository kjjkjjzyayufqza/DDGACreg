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
#include <string>

// Controls the command line the game sees. Another module may hook
// GetCommandLineW and hand the game a command line with a fixed -ResX/-ResY.
// So instead of an inline hook (which may end up behind such a hook), this
// patches the game executable's own import table: the game's calls come to us
// first, we fetch the command line (through any other hook) and edit it:
//   - if renderW/renderH are set, remove existing resolution and window-mode
//     options and add -ResX/-ResY/-windowed/-ForceRes for that size
//   - append `extra`
// fitMonitor: ignore renderW/renderH and use the output monitor (config.ini FitMonitor).
bool InstallCommandLineHook(int renderW, int renderH, bool fitMonitor, const std::wstring& extra);
