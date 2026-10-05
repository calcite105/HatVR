@echo off
setlocal

REM Run from an x64 Native Tools Command Prompt for Visual Studio.
pushd "%~dp0" || exit /b 1

cmake -S . -B build -G "Visual Studio 17 2022" -A x64
if errorlevel 1 goto :fail

cmake --build build --config Release --clean-first
if errorlevel 1 goto :fail

echo.
echo Release build completed successfully.
popd
exit /b 0

:fail
echo.
echo RELEASE BUILD FAILED.
popd
exit /b 1
