# EZOS

用 C / C++ / 汇编语言手搓的类 Windows 操作系统（自研内核 + 引导）。

## 当前进度

- 版本：v0.3-gui
- 引导：NASM 编写 bootloader + kernel_entry
- 内核：IDT/ISR、键盘、鼠标、TTY 文本终端、ATA 磁盘、exFAT 文件系统
- 图形：VGA 0x13 图形模式（gfx），内置 8x8 字体，GUI 菜单
- 构建：ninja + i686-elf-gcc + NASM + QEMU

## 目录结构

```
src/
├── boot/          # 引导与内核入口（NASM）
├── kernel/        # 内核源码（C）
│   ├── gfx.c      # 图形模式
│   ├── gui.c      # GUI 菜单
│   ├── mouse.c    # 鼠标驱动
│   ├── keyboard.c # 键盘驱动
│   ├── tty.c      # 文本终端
│   ├── ata.c      # ATA 磁盘
│   ├── exfat.c    # exFAT 文件系统
│   └── shell.c    # 命令行 shell
├── build.ninja    # ninja 构建脚本
└── linker.ld      # 链接脚本
```

## 构建

```bat
build_and_test.bat
```
