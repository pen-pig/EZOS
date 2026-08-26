@echo off
setlocal
cd /d "%~dp0"

rem ===== build-only mode: build only, no QEMU, no pause =====
set "BUILD_ONLY=0"
if /i "%~1"=="build-only" set "BUILD_ONLY=1"

rem ===== auto-detect MyOS root (scan C-H drives for toolchain) =====
set "MYOS_ROOT="
for %%D in (C D E F G H) do (
    if not defined MYOS_ROOT (
        if exist "%%D:\MyOS\tools\i686-elf-tools-windows\bin\i686-elf-gcc.exe" set "MYOS_ROOT=%%D:\MyOS"
    )
)
if not defined MYOS_ROOT (
    echo [ERROR] MyOS toolchain not found. Check that MyOS\tools exists on C-H drives.
    pause
    exit /b 1
)

rem ===== generate temp ninja file with correct root path =====
rem dynamically replace any drive-letter path (D:/MyOS or E:/MyOS), never hardcoded
set "MYOS_ROOT_SLASH=%MYOS_ROOT:\=/%"
powershell -NoProfile -Command "(Get-Content -Raw '%~dp0build.ninja') -replace '[A-Za-z]:/MyOS', '%MYOS_ROOT_SLASH%' | Set-Content -NoNewline '%~dp0build_auto.ninja'"

rem ===== ninja resolution: prefer ninja.exe next to this script, fallback to PATH =====
set "NINJA=%~dp0ninja.exe"
if not exist "%NINJA%" (
    where ninja >nul 2>nul
    if errorlevel 1 (
        echo [ERROR] ninja not found. Add ninja to PATH or place it next to this script.
        pause
        exit /b 1
    )
    set "NINJA=ninja"
)

rem ===== parallel build (-j = CPU logical cores) =====
set "NINJA_JOBS=%NUMBER_OF_PROCESSORS%"
if "%BUILD_ONLY%"=="1" (
    echo [build-only] ninja -j%NINJA_JOBS% building os-image.bin, no QEMU...
    "%NINJA%" -f "%~dp0build_auto.ninja" -j%NINJA_JOBS% os-image.bin
    if errorlevel 1 (
        echo [ERROR] build failed.
        exit /b 1
    )
    echo [build-only] build done: os-image.bin
    exit /b 0
)
echo [parallel] ninja -j%NINJA_JOBS% building and launching QEMU...
"%NINJA%" -f "%~dp0build_auto.ninja" -j%NINJA_JOBS% run
pause
