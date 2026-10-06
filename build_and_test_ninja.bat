@echo off
setlocal
cd /d "%~dp0"

rem ===== build-only mode: build only, no QEMU, no pause =====
set "BUILD_ONLY=0"
if /i "%~1"=="build-only" set "BUILD_ONLY=1"

rem ===== locate python =====
set "PYEXE="
for /f "delims=" %%P in ('where python 2^>nul') do if not defined PYEXE set "PYEXE=%%P"
if not defined PYEXE if exist "%LOCALAPPDATA%\Programs\Python\Python313\python.exe" set "PYEXE=%LOCALAPPDATA%\Programs\Python\Python313\python.exe"
if not defined PYEXE if exist "D:\Python315\python.exe" set "PYEXE=D:\Python315\python.exe"
if not defined PYEXE set "PYEXE=python"

rem ===== generate build_auto.ninja =====
rem All toolchain paths are resolved by tools\gen_ninja.py via ezos_env.py, which
rem derives them from the project location. Nothing here is hardcoded to a drive
rem letter, so a copy of the repo on another disk builds without editing files.
rem The rewrite MUST stay byte-level: PowerShell Get-Content decodes UTF-8 as GBK
rem and an orphan lead byte (e.g. 0x82 left from U+3002) swallows the following
rem 0x0A, merging two ninja lines into "unexpected indent".
"%PYEXE%" "%~dp0tools\gen_ninja.py" --src "%~dp0build.ninja" --out "%~dp0build_auto.ninja"
if errorlevel 1 (
    echo [ERROR] failed to generate build_auto.ninja
    echo         see the message above for the missing tool or path
    pause
    exit /b 1
)

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

rem ===== parallel build -j = CPU logical cores =====
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
