@echo off
chcp 936 >nul
setlocal
cd /d "%~dp0"

rem ===== build-only 模式：只构建，不启动 QEMU，不 pause =====
set "BUILD_ONLY=0"
if /i "%~1"=="build-only" set "BUILD_ONLY=1"

rem ===== 自动探测 MyOS 根目录（遍历 C-H 盘找工具链） =====
set "MYOS_ROOT="
for %%D in (C D E F G H) do (
    if not defined MYOS_ROOT (
        if exist "%%D:\MyOS\tools\i686-elf-tools-windows\bin\i686-elf-gcc.exe" set "MYOS_ROOT=%%D:\MyOS"
    )
)
if not defined MYOS_ROOT (
    echo [错误] 未找到 MyOS 工具链，请检查 E:\MyOS\tools 是否存在。
    pause
    exit /b 1
)

rem ===== 生成带正确盘符的临时 ninja 文件 =====
set "MYOS_ROOT_SLASH=%MYOS_ROOT:\=/%"
powershell -NoProfile -Command "(Get-Content -Raw '%~dp0build.ninja') -replace 'E:/MyOS', '%MYOS_ROOT_SLASH%' | Set-Content -NoNewline '%~dp0build_auto.ninja'"

rem ===== 多线程并行构建（-j 取 CPU 逻辑核数） =====
set "NINJA_JOBS=%NUMBER_OF_PROCESSORS%"
if "%BUILD_ONLY%"=="1" (
    echo [build-only] ninja -j%NINJA_JOBS% 并行构建 os-image.bin（不启动 QEMU）...
    ninja -f "%~dp0build_auto.ninja" -j%NINJA_JOBS% os-image.bin
    if errorlevel 1 (
        echo [错误] 构建失败。
        exit /b 1
    )
    echo [build-only] 构建完成：os-image.bin
    exit /b 0
)
echo [多线程] ninja -j%NINJA_JOBS% 并行构建并启动 QEMU...
ninja -f "%~dp0build_auto.ninja" -j%NINJA_JOBS% run
pause