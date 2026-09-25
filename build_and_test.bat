@echo off
setlocal
cd /d "%~dp0"

rem ===== toolchain resolution: prefer PATH, auto-detect MyOS tools dir as fallback =====
rem NOTE: this file must stay pure ASCII. cmd.exe decodes .bat with the local
rem codepage (GBK on zh-CN Windows), so any UTF-8 Chinese text here turns into
rem mojibake and may even be executed as a command.
set "MYOS_ROOT="
for %%D in (C D E F G H) do (
    if not defined MYOS_ROOT (
        if exist "%%D:\MyOS\tools\i686-elf-tools-windows\bin\i686-elf-gcc.exe" set "MYOS_ROOT=%%D:\MyOS"
    )
)
if defined MYOS_ROOT goto have_root
echo [ERROR] MyOS toolchain not found. Check that MyOS\tools exists on C-H drives.
pause
exit /b 1

:have_root
set "TOOLS_DIR=%MYOS_ROOT%\tools"
set "BIN_DIR=%TOOLS_DIR%\i686-elf-tools-windows\bin"
set "NASM_DIR=%TOOLS_DIR%\NASM"
rem QEMU dir: no version hardcoded, take first qemu-* under tools
set "QEMU_DIR="
for /d %%Q in ("%TOOLS_DIR%\qemu-*") do if not defined QEMU_DIR set "QEMU_DIR=%%Q"
set "PATH=%BIN_DIR%;%NASM_DIR%;%QEMU_DIR%;%PATH%"

rem ===== verify toolchain =====
where i686-elf-gcc >nul 2>nul
if errorlevel 1 (
    echo [ERROR] i686-elf-gcc not found. Add i686-elf toolchain to PATH or install under MyOS\tools.
    pause
    exit /b 1
)
where nasm >nul 2>nul
if errorlevel 1 (
    echo [ERROR] nasm not found. Add NASM to PATH or install under MyOS\tools.
    pause
    exit /b 1
)
where qemu-system-x86_64 >nul 2>nul
if errorlevel 1 (
    echo [ERROR] qemu-system-x86_64 not found. Add QEMU to PATH or install under MyOS\tools.
    pause
    exit /b 1
)

set "CFLAGS=-ffreestanding -O2 -Wall -Wextra -Ikernel"
set "LDFLAGS=-m elf_i386 -T linker.ld --oformat binary -e _start"

set "AUTO_RUN=1"
if /i "%1"=="build-only" set "AUTO_RUN=0"
if /i "%1"=="no-run" set "AUTO_RUN=0"

if /i "%1"=="clean" goto clean

echo ============================================
echo Building My OS...
echo ============================================

echo [1/6] assembling boot sector...
nasm -f bin boot\boot.asm -o boot\boot.bin
if errorlevel 1 goto error

echo [2/6] assembling kernel entry + task switch...
nasm -f elf32 boot\kernel_entry.asm -o boot\kernel_entry.o
if errorlevel 1 goto error
rem task_switch.asm provides switch_to and task_irq_trampoline (preemptive
rem scheduler). It is NOT auto-discovered by the kernel\*.c loop below, so it
rem must be listed explicitly here and in OBJS - forgetting it shows up as
rem "undefined reference to switch_to / task_irq_trampoline" at link time.
nasm -f elf32 boot\task_switch.asm -o boot\task_switch.o
if errorlevel 1 goto error

rem ===== prefer ninja: build.ninja is the source of truth for LINK ORDER =====
rem Compiling kernel\*.c one by one produces byte-identical .o files either
rem way, but this kernel is LINK-ORDER SENSITIVE: linking in alphabetical
rem order (what an auto-discovery loop gives) boots fine through the self-test
rem and then HANGS before reaching the shell. Only the explicit order in
rem build.ninja is known good. So delegate to ninja whenever it is present
rem instead of maintaining a second, drifting link list here.
if not exist "%~dp0ninja.exe" goto auto_discover
echo [3/6] building with ninja (build.ninja defines the correct link order)...
ninja -f build.ninja os-image.bin
if errorlevel 1 goto error
echo.
echo Build OK: os-image.bin generated (ninja)
if "%AUTO_RUN%"=="1" goto run
echo build-only mode: QEMU not launched.
exit /b 0

:auto_discover
echo [3/6] compiling kernel C files (auto-discovered, FALLBACK)...
echo [WARN] ninja.exe not found - falling back to alphabetical link order.
echo [WARN] This is known to produce a kernel that hangs after the boot
echo [WARN] self-test. Use build.ninja (or drop ninja.exe here) instead.
rem Sources are auto-discovered: adding kernel\*.c needs no change here.
rem Older revisions hardcoded a compile list AND a separate link list, which
rem easily drifted apart and produced link-time undefined references that are
rem hard to tell apart from real code errors.
setlocal enabledelayedexpansion
set "OBJS=boot\kernel_entry.o boot\task_switch.o"
for %%F in (kernel\*.c) do (
    echo   CC %%~nxF
    i686-elf-gcc %CFLAGS% -c "%%F" -o "kernel\%%~nF.o"
    if errorlevel 1 goto error
    set "OBJS=!OBJS! kernel\%%~nF.o"
)

echo [4/6] linking kernel...
i686-elf-ld %LDFLAGS% -o kernel_raw.bin !OBJS!
if errorlevel 1 goto error
endlocal & set "OBJS="

echo [5/6] padding kernel to 384KB...
i686-elf-objcopy -I binary -O binary --pad-to 507904 kernel_raw.bin kernel.bin
if errorlevel 1 goto error

echo [6/6] generating system image...
copy /b boot\boot.bin + kernel.bin os-image.bin >nul
if errorlevel 1 goto error

echo.
echo Build OK: os-image.bin generated

if "%AUTO_RUN%"=="1" goto run

echo build-only mode: QEMU not launched.
exit /b 0

:run
echo rebuilding disk.img (exFAT layout)...
python "%~dp0temp\gen_diskimg.py" disk.img
if errorlevel 1 goto error
echo launching QEMU...
qemu-system-x86_64 -icount shift=auto -vga std -drive format=raw,file=os-image.bin -drive format=raw,file=disk.img
pause
goto end

:clean
echo cleaning build artifacts...
del /q boot\boot.bin boot\kernel_entry.o boot\task_switch.o kernel\*.o kernel_raw.bin kernel.bin os-image.bin 2>nul
echo clean done.
pause
exit /b 0

:error
echo.
echo build failed. check errors above.
pause
exit /b 1

:end
endlocal
