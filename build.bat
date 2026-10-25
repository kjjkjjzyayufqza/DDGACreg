@echo off
setlocal EnableExtensions
cd /d "%~dp0"

rem Delayed expansion stays off. A game path may contain "!!", which cmd would
rem erase if delayed expansion were enabled. Parentheses in that path are also
rem left outside of parenthesized IF blocks so they are not parsed as block ends.
rem
rem The install folder is never stored in this script. Optional sources, in order:
rem   build.bat release "<TG4AC\Binaries\Win64>"
rem   DDGAC_GAME_DIR
rem   build.local.bat in this directory (gitignored; may set DEST)

if /I "%~1"=="release" goto :config_release
if /I "%~1"=="debug" goto :config_debug

echo Usage: build.bat release ^| debug [game_dir]
echo   release    Build Release version.dll.
echo   debug      Build Debug version.dll and version.pdb.
echo   game_dir   Optional folder that contains TG4AC-Win64-Shipping.exe.
echo              When omitted, DDGAC_GAME_DIR or build.local.bat is used.
echo              With none of those, the DLL stays under build\ and is not copied.
exit /b 1

:config_release
set "CONFIG=Release"
goto :build

:config_debug
set "CONFIG=Debug"
goto :build

:build
if exist "%~dp0build.local.bat" call "%~dp0build.local.bat"
if not "%~2"=="" set "DEST=%~2"
if not defined DEST set "DEST=%DDGAC_GAME_DIR%"

where cmake >nul 2>&1
if errorlevel 1 goto :missing_cmake

echo Building %CONFIG% version.dll
cmake -S "%CD%" -B "%CD%\build" -A x64
if errorlevel 1 exit /b 1
cmake --build "%CD%\build" --config %CONFIG% --target ddgac
if errorlevel 1 exit /b 1

if not defined DEST goto :built_only

if not exist "%DEST%\TG4AC-Win64-Shipping.exe" goto :missing_game

rem Drop the old Ultimate ASI Loader and its scripts folder. UE4SS (dwmapi.dll)
rem is a different proxy and is left in place.
if exist "%DEST%\scripts" rd /s /q "%DEST%\scripts"
if exist "%DEST%\winmm.dll" del /f /q "%DEST%\winmm.dll"

copy /Y "%CD%\build\dll\%CONFIG%\version.dll" "%DEST%\version.dll" >nul
if errorlevel 1 goto :copy_failed
if /I not "%CONFIG%"=="Debug" goto :copy_ini
copy /Y "%CD%\build\dll\%CONFIG%\version.pdb" "%DEST%\version.pdb" >nul
if errorlevel 1 goto :copy_failed

:copy_ini
if not exist "%DEST%\config.ini" copy /Y "%CD%\config.ini" "%DEST%\config.ini" >nul

echo Copied %CONFIG% version.dll to:
echo   %DEST%
exit /b 0

:built_only
echo Built %CONFIG% version.dll:
echo   %CD%\build\dll\%CONFIG%\version.dll
echo Pass the game Binaries\Win64 folder, or set DDGAC_GAME_DIR, to copy it.
exit /b 0

:missing_cmake
echo cmake.exe was not found on PATH. Install CMake and Visual Studio 2022 x64.
exit /b 1

:missing_game
echo Game folder not found:
echo   %DEST%
exit /b 1

:copy_failed
echo Failed to copy version.dll. Close the game if it is running.
exit /b 1
