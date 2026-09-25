# UEFI 最小启动链路验证（H2-U2a）

目标：验证本机工具链能产出可在 OVMF 下启动的 32 位 EFI 应用（BOOTIA32.EFI），
并成功打印标记 `EZEFI: hello`。不改内核、不改 boot.asm。

## 产出

- `uefi/main.c`、`uefi/efi.h`：最小 EFI 应用源码。
- `uefi/build.sh`：一条命令重建 `BOOTIA32.EFI`。
- `uefi/esp/EFI/BOOT/BOOTIA32.EFI`：构建产物（可被 OVMF 当作可移动介质自动加载）。
- `uefi/serial.log`：QEMU 串口抓取日志，含标记行。

## 一键重建

```bash
bash uefi/build.sh
```

## 工具链（本机已确认存在，未下载任何东西）

- 编译器：`/d/MyOS/tools/i686-elf-tools-windows/bin/i686-elf-gcc`
  （i686-elf 交叉编译器，**不在 PATH**，需绝对路径调用）。产出 ELF32 目标文件。
- 链接器：`/c/Program Files (x86)/Dev-Cpp/MinGW64/bin/ld.exe`
  （配 `-mi386pe -subsystem 10` 把 ELF32 目标文件链接成 PE32 EFI 应用）。
- 后处理：`.../MinGW64/bin/objcopy.exe`（`--remove-section .comment`）。
- OVMF：`/d/MyOS/tools/qemu-portable-20241220/share/edk2-i386-code.fd`
- QEMU：`/d/MyOS/tools/qemu-portable-20241220/qemu-system-i386.exe`

注意：i686-elf-gcc / ld / qemu **都是原生 Windows 程序**，只认 Windows 路径。
`build.sh` 内部用 `cygpath -w` 把仓库路径转成 `D:\...` 形态再传给它们。

## 启动验证（QEMU 虚拟 FAT 当 ESP，无需 mkfs/mtools）

```bash
QEMU=/d/MyOS/tools/qemu-portable-20241220/qemu-system-i386.exe
FD=$(cygpath -w /d/MyOS/tools/qemu-portable-20241220/share/edk2-i386-code.fd)
ESP=$(cygpath -w /d/MyOS/src/uefi/esp)
LOG=$(cygpath -w /d/MyOS/src/uefi/serial.log)
"$QEMU" -machine pc -m 256 \
  -drive if=pflash,format=raw,readonly=on,file="$FD" \
  -drive if=none,format=raw,file=fat:rw:"$ESP",id=esp \
  -usb -device usb-storage,drive=esp \
  -serial file:"$LOG" -net none -nographic
```

关键：FAT 必须挂成**可移动介质**（USB 大容量存储），否则 OVMF 不会从
`\EFI\BOOT\BOOTIA32.EFI` 自动加载，只会掉进 EFI Shell。

## 踩过的坑

1. **路径形态**：上述 Windows 工具不认 `/d/...`（MSYS）路径，必须 `cygpath -w`。
2. **字符集**：i686-elf-gcc 的 `wchar_t` 是 4 字节，禁用 `L"..."` 字面量。
   屏幕用的 UTF-16 字符串一律用 `uint16_t` 数组手工构造（`'E','Z',...`）。
3. **`.comment: section below image base`**：i686-elf-gcc 产出的 `.comment` 段
   VMA 低于 PE image base，导致 OVMF 镜像加载器直接拒绝（报 `Load Error`）。
   解决：编译加 `-fno-ident`，并 objcopy `--remove-section .comment` 兜底。
4. **重定位**：用 `-fno-pic` 避免 GOT；数据引用走 PE 基址重定位（`.reloc`），
   OVMF 可正常重定位加载。当前最小程序无外部符号，链接干净。
5. **串口与 ConOut 重复**：标记在串口出现两次——一次是我们直接 `out 0x3F8`，
   一次是 OVMF 把 ConOut 也镜像到同一串口。两路都生效，属预期。

## U2b：GOP 帧缓冲（写入内核约定地址 0x5000）

`main.c` 在 `EfiMain` 里用 `BootServices->LocateProtocol` 取
`EFI_GRAPHICS_OUTPUT_PROTOCOL`（GUID `9042a9de-23dc-4a38-96fb-7aded080516a`），
从 `Mode->Info` / `Mode->FrameBufferBase` 取分辨率、像素格式、帧缓冲物理地址、扫描行跨度。

**内核约定布局（kernel/gfx.c:139-142 核对）**：物理地址 0x5000 处
- `0x5000` uint32  LFB 物理地址（0=无 LFB，回退 VGA 0x13）
- `0x5004` uint16  XRES
- `0x5006` uint16  YRES
- `0x5008` uint8   BPP（每像素字节数；当前 GOP 32bpp 写 32。注意 gfx.c 现仅接受 16=RGB565，
  故 U3 改 gfx.c 前仍会走 VGA 0x13 回退——属预期，非本步 bug）
- `0x5009` uint8   GOP PixelFormat 枚举值（0=RGBA 1=BGR 2=BitMask 3=BltOnly）
- `0x500A` uint16  保留
- `0x500C` uint32  PixelsPerScanLine（stride，单位像素）

UEFI IA32 启动阶段为 1:1 映射，按物理地址直接写入即可。PixelFormat>=3 视为不可用，不写。

串口输出两行（外加保留的 `EZEFI: hello`）：
- `EZEFI:gop <w>x<h> fmt=<N> fb=0x<HEX> stride=<N>`
- `EZEFI:gop write@0x5000 ok`

实测（OVMF i386，默认 VGA）：`EZEFI:gop 1280x800 fmt=1 fb=0x80000000 stride=1280`
（fmt=1=BGRA 32bpp，fb 落在 32 位空间）。

### U2b 新增踩坑
6. **EFI_BOOT_SERVICES 布局**：`LocateProtocol` 是第 38 个成员（Hdr 之后偏移 `0xac`/172 字节），
   不是第 37 个。介于 `HandleProtocol` 与 `RegisterProtocolNotify` 之间还有一个 UEFI 2.0 兼容的
   `Reserved` 槽（void*）——漏掉它偏移错位，会把别的 BS 函数当成 LocateProtocol，
   表现为 `EFI_NOT_FOUND` 或返回 0 但接口指针为 NULL。已用探针在多个偏移实测确认 0xac 正确。
7. 取不到 GOP（PixelFormat>=3 或 LocateProtocol 失败）时如实打 `FAIL`，不伪造数据。

## 结论

32 位路径打通：BOOTIA32.EFI 在 OVMF 下自动加载、运行、打印标记并向 0x5000 写入 GOP 帧缓冲参数。
后续 U2c 再加 ExitBootServices、加载并跳转内核（U3 改 gfx.c 接受 32bpp）。
