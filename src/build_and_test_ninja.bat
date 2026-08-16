@echo off
chcp 936 >nul
setlocal
cd /d "%~dp0"

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

ninja -f "%~dp0build_auto.ninja" run
pause