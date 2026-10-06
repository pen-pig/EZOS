# 在新环境编译与运行（路径可搬迁）

仓库**不依赖任何硬编码盘符**：整目录拷到任意盘/任意机器都能编译运行。
所有外部工具（gcc/ld/nasm/cargo/zig/qemu/OVMF）都由项目根的
[`ezos_env.py`](../ezos_env.py) 统一解析。

## 最短路径

```bat
:: 1) 工具链放到 <任意位置>/MyOS/tools/（或设 EZOS_TOOLS 指向它）
::    必需：i686-elf-tools-windows\bin\i686-elf-{gcc,ld,objcopy}.exe
::          NASM\nasm.exe
::          zig\zig-x86_64-windows-0.15.1\zig.exe
::          qemu-portable-20241220\qemu-system-x86_64.exe（跑测试/运行时）
::    可选：qemu-system-i386.exe + share\edk2-i386-code.fd（UEFI 路径测试）
::    Rust：cargo 默认不在 PATH，装好后设 EZOS_CARGO 指向它

:: 2) 编译
build_and_test_ninja.bat build-only

:: 3) 跑（会真的开 QEMU）
build_and_test_ninja.bat
```

## 路径解析规则（`ezos_env.py`）

优先级从高到低：

1. 环境变量：`EZOS_TOOLS`（工具根）、`EZOS_CC` / `EZOS_LD` / `EZOS_NASM` /
   `EZOS_ZIG` / `EZOS_QEMU` / `EZOS_QEMU32` / `EZOS_OVMF` / `EZOS_CARGO`
2. **按项目自身位置推导**：`<项目根>/../tools` —— 仓库放在 `X:\MyOS\src` 时
   自动使用 `X:\MyOS\tools`，D/E 多副本零配置
3. 扫 C–H 盘找 `MyOS/tools`（老布局兜底）
4. 回退 `D:/MyOS/tools`

缺任何一件都会**在构建前**报出缺什么、以及三种修法，而不是让 ninja 在几百行
之后抛一句 `command not found`。

自查当前环境：

```bat
python ezos_env.py
```

输出形如：

```
项目根  : D:\MyOS\src
工具根  : D:\MyOS\tools
  cc       OK  D:\MyOS\tools\i686-elf-tools-windows\bin\i686-elf-gcc.exe
  ...
cargo    : C:\Users\<you>\.cargo\bin\cargo.exe
```

## 没有 Rust / 没有 Zig 也能编

Rust(`libezos_rs.a`) 与 Zig(`ezos_zig.o`) 是链接必需项，但可以退回纯 C 实现：

```bat
set EZOS_SKIP_RUSTZIG=1
build_and_test_ninja.bat build-only
```

此时 `gen_ninja.py` 不写 ZIG/CARGO 行，`EZ_EXFAT_IMPL` 应配合设为 0
（`kernel/rust_bridge.h`，默认是 1=Rust）。

## 回归测试

```bat
python tools\run_tests.py core      :: 每次改动都跑的那一组
python tools\run_tests.py changed   :: 按 git 改动自动选层
python tools\run_tests.py full      :: 全部（很慢）
```

串行执行是硬要求：测试共享 `os-image.bin` 与 QEMU 端口，并行会互抢
（症状是莫名其妙的 `ConnectionRefused`）。

## 已知不可复现项

`kernel/sysvol_data.c` 里的 `/system` 卷带 `built=YYYY-MM-DD` 构建日期戳，
因此**跨天**构建出的 `kernel_raw.bin` 有 4 字节差异。代码编译本身逐字节可复现
（`boot/boot.bin` 在 D/E 两盘 SHA256 相同）。
