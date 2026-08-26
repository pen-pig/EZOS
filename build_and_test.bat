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
    echo [错误] 未找到 MyOS 工具链，请检查 C-H 盘中 MyOS\tools 是否存在。
    pause
    exit /b 1
)

set "BIN_DIR=%MYOS_ROOT%\tools\i686-elf-tools-windows\bin"
set "CC=%BIN_DIR%\i686-elf-gcc.exe"
set "LD=%BIN_DIR%\i686-elf-ld.exe"
set "ASM=%MYOS_ROOT%\tools\NASM\nasm.exe"
set "QEMU=%MYOS_ROOT%\tools\qemu-portable-20241220\qemu-system-x86_64.exe"
set "OBJCOPY=%BIN_DIR%\i686-elf-objcopy.exe"

set "CFLAGS=-ffreestanding -O2 -Wall -Wextra -Ikernel"
set "LDFLAGS=-m elf_i386 -T linker.ld --oformat binary -e _start"

set "AUTO_RUN=1"
if /i "%1"=="build-only" set "AUTO_RUN=0"
if /i "%1"=="no-run" set "AUTO_RUN=0"

if /i "%1"=="clean" goto clean

echo ============================================
echo 正在构建 My OS...
echo ============================================

echo [1/6] 汇编引导扇区...
"%ASM%" -f bin boot\boot.asm -o boot\boot.bin
if errorlevel 1 goto error

echo [2/6] 汇编内核入口...
"%ASM%" -f elf32 boot\kernel_entry.asm -o boot\kernel_entry.o
if errorlevel 1 goto error

echo [3/6] 编译内核 C 文件...
"%CC%" %CFLAGS% -c kernel\kernel.c -o kernel\kernel.o
if errorlevel 1 goto error
"%CC%" %CFLAGS% -c kernel\tty.c -o kernel\tty.o
if errorlevel 1 goto error
"%CC%" %CFLAGS% -c kernel\idt.c -o kernel\idt.o
if errorlevel 1 goto error
"%CC%" %CFLAGS% -c kernel\isr.c -o kernel\isr.o
if errorlevel 1 goto error
"%CC%" %CFLAGS% -c kernel\keyboard.c -o kernel\keyboard.o
if errorlevel 1 goto error
"%CC%" %CFLAGS% -c kernel\ata.c -o kernel\ata.o
if errorlevel 1 goto error
"%CC%" %CFLAGS% -c kernel\shell.c -o kernel\shell.o
if errorlevel 1 goto error
"%CC%" %CFLAGS% -c kernel\shell_extra.c -o kernel\shell_extra.o
if errorlevel 1 goto error
"%CC%" %CFLAGS% -c kernel\exfat.c -o kernel\exfat.o
if errorlevel 1 goto error
"%CC%" %CFLAGS% -c kernel\gfx.c -o kernel\gfx.o
if errorlevel 1 goto error
"%CC%" %CFLAGS% -c kernel\gui.c -o kernel\gui.o
if errorlevel 1 goto error
"%CC%" %CFLAGS% -c kernel\mouse.c -o kernel\mouse.o
if errorlevel 1 goto error
"%CC%" %CFLAGS% -c kernel\gfxwin.c -o kernel\gfxwin.o
if errorlevel 1 goto error
"%CC%" %CFLAGS% -c kernel\desktop.c -o kernel\desktop.o
if errorlevel 1 goto error
"%CC%" %CFLAGS% -c kernel\games.c -o kernel\games.o
if errorlevel 1 goto error

echo [4/6] 链接内核...
"%LD%" %LDFLAGS% -o kernel_raw.bin boot\kernel_entry.o kernel\kernel.o kernel\tty.o kernel\idt.o kernel\isr.o kernel\keyboard.o kernel\ata.o kernel\shell.o kernel\shell_extra.o kernel\exfat.o kernel\gfx.o kernel\gui.o kernel\mouse.o kernel\gfxwin.o kernel\desktop.o kernel\games.o
if errorlevel 1 goto error

echo [5/6] 填充内核到 80KB...
"%OBJCOPY%" -I binary -O binary --pad-to 262144 kernel_raw.bin kernel.bin
if errorlevel 1 goto error

echo [6/6] 生成系统镜像...
copy /b boot\boot.bin + kernel.bin os-image.bin >nul
if errorlevel 1 goto error

echo.
echo 构建成功！已生成 os-image.bin

if "%AUTO_RUN%"=="1" goto run

echo 本次构建未启动 QEMU（使用了 build-only 模式）。
exit /b 0

:run
echo 正在重建 disk.img（exFAT 布局）...
python "%~dp0temp\gen_diskimg.py" disk.img
if errorlevel 1 goto error
echo 正在启动 QEMU...
"%QEMU%" -vga std -drive format=raw,file=os-image.bin -drive format=raw,file=disk.img
pause
goto end

:clean
echo 正在清理构建产物...
del /q boot\boot.bin boot\kernel_entry.o kernel\kernel.o kernel\tty.o kernel\idt.o kernel\isr.o kernel\keyboard.o kernel\ata.o kernel\shell.o kernel\exfat.o kernel\gui.o kernel\gfx.o kernel\gfxwin.o kernel\desktop.o kernel\games.o kernel_raw.bin kernel.bin os-image.bin 2>nul
echo 清理完成！
pause
exit /b 0

:error
echo.
echo 构建失败，请检查错误信息！
pause
exit /b 1

:end
endlocal