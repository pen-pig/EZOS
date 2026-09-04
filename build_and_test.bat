@echo off
setlocal
cd /d "%~dp0"

rem ===== toolchain resolution: prefer PATH, auto-detect MyOS tools dir as fallback =====
rem no hardcoded tool paths; all tools resolved via PATH
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

echo [2/6] assembling kernel entry...
nasm -f elf32 boot\kernel_entry.asm -o boot\kernel_entry.o
if errorlevel 1 goto error

echo [3/6] compiling kernel C files...
i686-elf-gcc %CFLAGS% -c kernel\kernel.c -o kernel\kernel.o
if errorlevel 1 goto error
i686-elf-gcc %CFLAGS% -c kernel\tty.c -o kernel\tty.o
if errorlevel 1 goto error
i686-elf-gcc %CFLAGS% -c kernel\idt.c -o kernel\idt.o
if errorlevel 1 goto error
i686-elf-gcc %CFLAGS% -c kernel\isr.c -o kernel\isr.o
if errorlevel 1 goto error
i686-elf-gcc %CFLAGS% -c kernel\keyboard.c -o kernel\keyboard.o
if errorlevel 1 goto error
i686-elf-gcc %CFLAGS% -c kernel\ata.c -o kernel\ata.o
if errorlevel 1 goto error
i686-elf-gcc %CFLAGS% -c kernel\shell.c -o kernel\shell.o
if errorlevel 1 goto error
i686-elf-gcc %CFLAGS% -c kernel\shell_extra.c -o kernel\shell_extra.o
if errorlevel 1 goto error
i686-elf-gcc %CFLAGS% -c kernel\exfat.c -o kernel\exfat.o
if errorlevel 1 goto error
i686-elf-gcc %CFLAGS% -c kernel\fat.c -o kernel\fat.o
if errorlevel 1 goto error
i686-elf-gcc %CFLAGS% -c kernel\fs.c -o kernel\fs.o
if errorlevel 1 goto error
i686-elf-gcc %CFLAGS% -c kernel\ext4.c -o kernel\ext4.o
if errorlevel 1 goto error
i686-elf-gcc %CFLAGS% -c kernel\ntfs.c -o kernel\ntfs.o
if errorlevel 1 goto error
i686-elf-gcc %CFLAGS% -c kernel\f2fs.c -o kernel\f2fs.o
if errorlevel 1 goto error
i686-elf-gcc %CFLAGS% -c kernel\erofs.c -o kernel\erofs.o
if errorlevel 1 goto error
i686-elf-gcc %CFLAGS% -c kernel\div64.c -o kernel\div64.o
if errorlevel 1 goto error
i686-elf-gcc %CFLAGS% -c kernel\gfx.c -o kernel\gfx.o
if errorlevel 1 goto error
i686-elf-gcc %CFLAGS% -c kernel\gui.c -o kernel\gui.o
if errorlevel 1 goto error
i686-elf-gcc %CFLAGS% -c kernel\mouse.c -o kernel\mouse.o
if errorlevel 1 goto error
i686-elf-gcc %CFLAGS% -c kernel\gfxwin.c -o kernel\gfxwin.o
if errorlevel 1 goto error
i686-elf-gcc %CFLAGS% -c kernel\desktop.c -o kernel\desktop.o
if errorlevel 1 goto error
i686-elf-gcc %CFLAGS% -c kernel\games.c -o kernel\games.o
if errorlevel 1 goto error
i686-elf-gcc %CFLAGS% -c kernel\hiscore.c -o kernel\hiscore.o
if errorlevel 1 goto error

echo [4/6] linking kernel...
i686-elf-ld %LDFLAGS% -o kernel_raw.bin boot\kernel_entry.o kernel\kernel.o kernel\tty.o kernel\idt.o kernel\isr.o kernel\keyboard.o kernel\ata.o kernel\shell.o kernel\shell_extra.o kernel\exfat.o kernel\fat.o kernel\fs.o kernel\ext4.o kernel\ntfs.o kernel\f2fs.o kernel\erofs.o kernel\div64.o kernel\gfx.o kernel\gui.o kernel\mouse.o kernel\gfxwin.o kernel\desktop.o kernel\games.o kernel\hiscore.o
if errorlevel 1 goto error

echo [5/6] padding kernel to 384KB...
i686-elf-objcopy -I binary -O binary --pad-to 393216 kernel_raw.bin kernel.bin
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
qemu-system-x86_64 -vga std -drive format=raw,file=os-image.bin -drive format=raw,file=disk.img
pause
goto end

:clean
echo cleaning build artifacts...
del /q boot\boot.bin boot\kernel_entry.o kernel\kernel.o kernel\tty.o kernel\idt.o kernel\isr.o kernel\keyboard.o kernel\ata.o kernel\shell.o kernel\exfat.o kernel\gui.o kernel\gfx.o kernel\gfxwin.o kernel\desktop.o kernel\games.o kernel_raw.bin kernel.bin os-image.bin 2>nul
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
