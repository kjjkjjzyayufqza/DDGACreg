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
#include "log.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace
{
    FILE* g_file = nullptr;
    HANDLE g_console = INVALID_HANDLE_VALUE;
    std::mutex g_mutex;

    BOOL WINAPI OnConsoleCtrl(DWORD)
    {
        // Swallow close, Ctrl+C, and break so the game process stays up.
        return TRUE;
    }

    void ApplyConsoleFont(HANDLE console)
    {
        CONSOLE_FONT_INFOEX font{};
        font.cbSize = sizeof(font);
        font.dwFontSize.Y = 16;
        font.FontFamily = FF_MODERN;
        font.FontWeight = FW_NORMAL;
        wcscpy_s(font.FaceName, L"Consolas");
        SetCurrentConsoleFontEx(console, FALSE, &font);
    }

    void PlaceConsoleWindow()
    {
        HWND window = GetConsoleWindow();
        if (!window)
            return;
        const int width = 760;
        const int height = 440;
        int x = GetSystemMetrics(SM_CXSCREEN) - width - 32;
        int y = 40;
        if (x < 0)
            x = 0;
        if (y < 0)
            y = 0;
        SetWindowPos(window, HWND_TOP, x, y, width, height, SWP_SHOWWINDOW);
    }

    void OpenConsole()
    {
        AllocConsole();
        SetConsoleTitleW(L"DDGACreg");
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
        SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);

        g_console = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (g_console == INVALID_HANDLE_VALUE)
            g_console = GetStdHandle(STD_OUTPUT_HANDLE);
        if (g_console == INVALID_HANDLE_VALUE || g_console == nullptr)
            return;

        DWORD mode = 0;
        if (GetConsoleMode(g_console, &mode))
            SetConsoleMode(g_console, mode | ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT);

        COORD buffer{120, 2000};
        SetConsoleScreenBufferSize(g_console, buffer);
        SMALL_RECT view{0, 0, 119, 32};
        SetConsoleWindowInfo(g_console, TRUE, &view);
        ApplyConsoleFont(g_console);
        PlaceConsoleWindow();
    }

    void Paint(const char* text, WORD color)
    {
        if (g_console == INVALID_HANDLE_VALUE || g_console == nullptr || text == nullptr || text[0] == '\0')
            return;
        SetConsoleTextAttribute(g_console, color);
        DWORD written = 0;
        WriteConsoleA(g_console, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
    }

    const char* Classify(const char* body)
    {
        if (std::strncmp(body, "ERROR:", 6) == 0 || std::strncmp(body, "FATAL", 5) == 0)
            return "ERROR";
        if (std::strncmp(body, "WARN:", 5) == 0)
            return "WARN";
        if (std::strstr(body, "FAILED") || std::strstr(body, "FATAL") || std::strstr(body, "Fault")
            || std::strstr(body, "Could not"))
            return "ERROR";
        if (std::strstr(body, "Bad ") || std::strstr(body, "not found") || std::strstr(body, "Ignoring")
            || std::strstr(body, "disabled") || std::strstr(body, "unavailable") || std::strstr(body, "Note:"))
            return "WARN";
        return "INFO";
    }

    WORD LevelColor(const char* level)
    {
        if (std::strcmp(level, "ERROR") == 0)
            return FOREGROUND_RED | FOREGROUND_INTENSITY;
        if (std::strcmp(level, "WARN") == 0)
            return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        return FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
    }

    void Emit(const SYSTEMTIME& time, const char* level, const char* body)
    {
        char stamp[24];
        std::snprintf(stamp, sizeof(stamp), "%02u:%02u:%02u.%03u",
            static_cast<unsigned>(time.wHour), static_cast<unsigned>(time.wMinute),
            static_cast<unsigned>(time.wSecond), static_cast<unsigned>(time.wMilliseconds));

        char levelField[8];
        std::snprintf(levelField, sizeof(levelField), "%-5s", level);

        std::string line;
        line.reserve(std::strlen(body) + 32);
        line += stamp;
        line += "  ";
        line += levelField;
        line += "  ";
        line += body;
        line += "\n";

        if (g_file)
        {
            std::fputs(line.c_str(), g_file);
            std::fflush(g_file);
        }

        const WORD dim = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
        Paint(stamp, FOREGROUND_INTENSITY);
        Paint("  ", dim);
        Paint(levelField, LevelColor(level));
        Paint("  ", dim);
        Paint(body, dim);
        Paint("\n", dim);

        OutputDebugStringA(line.c_str());
    }
}

void logx::Init(const std::wstring& path)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_file)
        g_file = _wfopen(path.c_str(), L"w");
    if (g_console == INVALID_HANDLE_VALUE)
        OpenConsole();

    const WORD accent = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
    const WORD dim = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    Paint("DDGACreg\n", accent);
    Paint("runtime log\n", dim);
    if (!path.empty())
    {
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (bytes > 1)
        {
            std::string file(static_cast<size_t>(bytes), '\0');
            WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, file.data(), bytes, nullptr, nullptr);
            if (!file.empty() && file.back() == '\0')
                file.pop_back();
            Paint("file  ", dim);
            Paint(file.c_str(), dim);
            Paint("\n\n", dim);
        }
    }
}

void logx::Write(const char* fmt, ...)
{
    if (fmt == nullptr)
        return;

    char body[4096];
    va_list args;
    va_start(args, fmt);
    const int wrote = std::vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);
    if (wrote < 0)
        return;
    body[sizeof(body) - 1] = '\0';

    SYSTEMTIME time{};
    GetLocalTime(&time);
    const char* level = Classify(body);

    std::lock_guard<std::mutex> lock(g_mutex);
    Emit(time, level, body);
}
