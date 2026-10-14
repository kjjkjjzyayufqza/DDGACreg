; Copyright (c) 2026 Spencer Miller
;
; Permission is hereby granted, free of charge, to any person obtaining a copy
; of this software and associated documentation files (the "Software"), to deal
; in the Software without restriction, including without limitation the rights
; to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
; copies of the Software, and to permit persons to whom the Software is
; furnished to do so, subject to the following conditions:
;
; The above copyright notice and this permission notice shall be included in all
; copies or substantial portions of the Software.
;
; THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
; IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
; FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
; AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
; LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
; OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
; SOFTWARE.
;
; Adapted for DDGACreg from https://github.com/percentcer/DDGUltrawide
; Export thunks: each jumps to the matching function in the real System32 version.dll.
; g_origFuncs is filled in by proxy.cpp before the game can call any of these.

EXTERN g_origFuncs:QWORD

THUNK MACRO name, idx
name PROC
    jmp qword ptr [g_origFuncs + idx*8]
name ENDP
ENDM

.code
THUNK Thunk_GetFileVersionInfoA, 0
THUNK Thunk_GetFileVersionInfoByHandle, 1
THUNK Thunk_GetFileVersionInfoExA, 2
THUNK Thunk_GetFileVersionInfoExW, 3
THUNK Thunk_GetFileVersionInfoSizeA, 4
THUNK Thunk_GetFileVersionInfoSizeExA, 5
THUNK Thunk_GetFileVersionInfoSizeExW, 6
THUNK Thunk_GetFileVersionInfoSizeW, 7
THUNK Thunk_GetFileVersionInfoW, 8
THUNK Thunk_VerFindFileA, 9
THUNK Thunk_VerFindFileW, 10
THUNK Thunk_VerInstallFileA, 11
THUNK Thunk_VerInstallFileW, 12
THUNK Thunk_VerLanguageNameA, 13
THUNK Thunk_VerLanguageNameW, 14
THUNK Thunk_VerQueryValueA, 15
THUNK Thunk_VerQueryValueW, 16

END
