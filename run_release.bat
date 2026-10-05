@echo off
setlocal

REM Build HatVR, deploy it to A Hat in Time, then launch through Steam.
REM Pass the game's Win64 directory as the first argument, or set HATINTIME_DIR.
REM Example: run_release.bat "D:\SteamLibrary\steamapps\common\HatinTime\Binaries\Win64"

set "GAME_DIR=%~1"
if not defined GAME_DIR set "GAME_DIR=%HATINTIME_DIR%"

if not defined GAME_DIR (
    echo ERROR: No A Hat in Time Win64 directory was provided.
    echo.
    echo Usage:
    echo   run_release.bat "path\to\HatinTime\Binaries\Win64"
    echo.
    echo Or set HATINTIME_DIR to that directory first.
    exit /b 1
)

if not exist "%GAME_DIR%" (
    echo ERROR: Game directory does not exist:
    echo   "%GAME_DIR%"
    exit /b 1
)

set "DLL1_SRC=%~dp0build\Release\d3d9.dll"
set "DLL1_DST=%GAME_DIR%\d3d9.dll"
set "DLL2_SRC=%~dp0build\Release\HatVR.dll"
set "DLL2_DST=%GAME_DIR%\HatVR.dll"

pushd "%~dp0" || exit /b 1

cmake -S . -B build -G "Visual Studio 17 2022" -A x64
if errorlevel 1 goto :fail

cmake --build build --config Release --clean-first
if errorlevel 1 goto :fail

if not exist "%DLL1_SRC%" (
    echo ERROR: Build reported success but "%DLL1_SRC%" does not exist.
    goto :fail
)

if not exist "%DLL2_SRC%" (
    echo ERROR: Build reported success but "%DLL2_SRC%" does not exist.
    goto :fail
)

copy /Y "%DLL1_SRC%" "%DLL1_DST%"
if errorlevel 1 goto :copyfail

copy /Y "%DLL2_SRC%" "%DLL2_DST%"
if errorlevel 1 goto :copyfail

start "" "steam://rungameid/253230"

popd
exit /b 0

:copyfail
echo ERROR: Could not copy the DLLs to the game directory.
goto :fail

:fail
echo RELEASE BUILD/DEPLOY FAILED - game was not launched.
popd
exit /b 1
