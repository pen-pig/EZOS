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
rem MUST be a byte-level copy. PowerShell's Get-Content decodes UTF-8 as GBK, and an
rem orphan GBK lead byte (e.g. 0x82 left over from U+3002) swallows the following
rem 0x0A, silently merging ninja lines -> "unexpected indent" on the next build.
set "MYOS_ROOT_SLASH=%MYOS_ROOT:\=/%"
set "PYEXE="
for /f "delims=" %%P in ('where python 2^>nul') do if not defined PYEXE set "PYEXE=%%P"
if not defined PYEXE if exist "D:\Python315\python.exe" set "PYEXE=D:\Python315\python.exe"
if not defined PYEXE set "PYEXE=python"
"%PYEXE%" -c "import re; src=r'%~dp0build.ninja'; dst=r'%~dp0build_auto.ninja'; root=r'%MYOS_ROOT_SLASH%'; d=open(src,'rb').read(); open(dst,'wb').write(re.sub(rb'[A-Za-z]:/MyOS', root.encode(), d))"
if errorlevel 1 (
    echo [ERROR] failed to generate build_auto.ninja - python is required
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
