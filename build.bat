@echo off
setlocal EnableExtensions
cd /d "%~dp0"

rem Delayed expansion stays off. The game path contains "!!", which cmd would
rem erase if delayed expansion were enabled. Parentheses in that path are also
rem left outside of parenthesized IF blocks so they are not parsed as block ends.

if /I "%~1"=="release" goto :config_release
if /I "%~1"=="debug" goto :config_debug

echo Usage: build.bat release ^| debug
echo   release   Build Release version.dll and copy it next to the game EXE.
echo   debug     Build Debug version.dll and version.pdb into the same folder.
exit /b 1

:config_release
set "CONFIG=Release"
goto :build

:config_debug
set "CONFIG=Debug"
goto :build

:build
set "DEST=E:\Densha de Go!!\Densha de Go!! (5.80.02) (2019) (Japan) [Taito Type X4] [TP]\TG4AC\Binaries\Win64"
set "CMAKE=C:\Program Files\CMake\bin\cmake.exe"
if not exist "%CMAKE%" goto :missing_cmake

echo Building %CONFIG% version.dll
"%CMAKE%" -S "%CD%" -B "%CD%\build" -A x64
if errorlevel 1 exit /b 1
"%CMAKE%" --build "%CD%\build" --config %CONFIG% --target ddgac
if errorlevel 1 exit /b 1

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

:missing_cmake
echo cmake.exe was not found. Install CMake and Visual Studio 2022 x64.
exit /b 1

:missing_game
echo Game folder not found:
echo   %DEST%
exit /b 1

:copy_failed
echo Failed to copy version.dll. Close the game if it is running.
exit /b 1
