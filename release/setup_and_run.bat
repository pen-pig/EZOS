@echo off
chcp 65001 >nul
rem ============================================================
rem  EZOS - Windows 一键启动脚本（QEMU）
rem  要求: os-image.bin 与本脚本同目录
rem  若未安装 QEMU 会自动尝试 winget 安装（包 ID: SoftwareFreedomConservancy.QEMU）
rem  数据盘 disk.img 缺失时用同目录 gen_diskimg.py 重新生成
rem ============================================================
setlocal
cd /d "%~dp0"

if not exist "os-image.bin" (
    echo [ERROR] os-image.bin not found next to this script.
    echo         Please download the full EZOS release zip.
    pause
    exit /b 1
)

rem ---- locate qemu-system-x86_64 ----
where qemu-system-x86_64 >nul 2>nul
if not errorlevel 1 goto :have_qemu

echo [INFO] qemu-system-x86_64 not found on PATH.
echo [INFO] Trying winget to install QEMU...
winget install --id SoftwareFreedomConservancy.QEMU -e --accept-source-agreements --accept-package-agreements
if errorlevel 1 (
    echo [ERROR] winget install failed / package not found.
    echo         Please install QEMU manually from:
    echo         https://www.qemu.org/download/#windows
    echo         and re-run this script.
    pause
    exit /b 1
)

rem refresh PATH for this session (delayed-expansion safe)
set "PATH=%LOCALAPPDATA%\Programs\QEMU;%PATH%"

:have_qemu
where qemu-system-x86_64 >nul 2>nul
if errorlevel 1 (
    echo [ERROR] Still cannot find qemu-system-x86_64. Install QEMU and retry.
    pause
    exit /b 1
)

rem ---- ensure data disk ----
if not exist "disk.img" (
    echo [INFO] disk.img missing, regenerating with gen_diskimg.py...
    python "%~dp0gen_diskimg.py" "disk.img"
    if errorlevel 1 (
        echo [ERROR] failed to generate disk.img. Check Python installation.
        pause
        exit /b 1
    )
)

echo [INFO] Booting EZOS in QEMU...
qemu-system-x86_64 -vga std -m 128 -drive format=raw,file=os-image.bin -drive format=raw,file=disk.img
endlocal
