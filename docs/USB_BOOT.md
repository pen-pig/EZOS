# 真机点亮 H1c：把 os-image.bin 做成可启动 U 盘

> 适用对象：一个要在**真实 PC** 上点亮 EZOS 内核的人。
> 场景：**没有屏幕、只有一根 USB-TTL/串口线**也能判断卡在哪一步。
> 内核当前是**纯 Legacy BIOS** 引导（H2 才做 UEFI），所以这份文档只讲 Legacy 路线。

---

## 0. 一句话结论

把 `os-image.bin` **整盘原样写进 U 盘**（像写软盘镜像那样从 LBA0 开始），
**不要**把它当文件拷进一个 FAT32/U 盘分区。写完的 U 盘在 Windows 里看起来像
"未格式化"，这是正常的，千万别去"格式化拯救它"。

---

## 1. 镜像到底是怎么被引导的（先看懂，才不会写错）

`os-image.bin` 是两段拼起来的裸镜像：

```
os-image.bin = boot/boot.bin (512 字节，引导扇区)
             + kernel.bin     (507904 字节，已 pad 到 KERNEL_SECTORS=992 扇区)
```

- 总大小 508416 字节 = 993 扇区（512 × 993）。其中第 0 扇区是引导代码，
  第 1~992 扇区是内核。
- 引导扇区以 `0x55 0xAA` 结尾（BIOS 据此判定"这是可引导设备"）。

引导流程（`boot/boot.asm`）：

1. BIOS 把 LBA0 的一个扇区（512B）加载到 `0x7C00` 并执行。
2. 引导代码**只用 BIOS `INT 13h AH=42h`（扩展读）**，按 DAP 把后续
   **992 个扇区**读到 `0x10000`，每批 64 扇区、每批重试 3 次。
3. 依次做：VBE 探测（找 16bpp LFB 模式）→ A20 开启（三重回退）→
   建 GDT → 切入 32 位保护模式 → `jmp 0x10000` 进内核。

### 为什么"不能把 os-image.bin 拷进一个 FAT32 分区当文件"

引导阶段**完全不认识任何文件系统**。BIOS 只负责从 LBA0 找引导扇区并跳进去；
剩下的 992 扇区内核，是引导代码用 `INT 13h` 按**绝对扇区号（LBA）**整块读上来的，
不是从某个分区里的文件读出来的。

- 如果你把 `os-image.bin` 当普通文件丢进一个格式化的 U 盘，BIOS 看到的 LBA0
  是**分区表 / 文件系统引导记录**，不是我们的引导代码 → 要么报"无可引导设备"，
  要么去执行文件系统里不相干的东西，内核根本不会被加载。
- 必须让 `os-image.bin` 的第 0 字节落在 U 盘的 LBA0，也就是**整盘写入**。

### 为什么"写完像未格式化，别去格式化"

镜像里**没有 MBR 分区表**（`boot.bin` 本身就是引导扇区，不是分区引导记录）。
所以 dd 到 U 盘后，U 盘的第一个扇区就是引导扇区，整盘在操作系统眼里是
"没有分区表的超级软盘"。Windows 弹出"需要格式化"是预期行为——**点取消**，
格式化会覆盖引导扇区，U 盘就再也起不来了。

### 为什么"必须 Legacy / CSM，不支持 UEFI"

这是纯 Legacy BIOS 内核：引导靠 `INT 13h`、实模式切换、VBE，没有 UEFI
应用（没有 PE 头、没有 `efi/main.c`）。在纯 UEFI（关闭 CSM）模式下，
固件不会去执行 0x7C00 那段 16 位代码，机器会直接报"无可引导设备"。
**UEFI 支持是 H2 的事，本项目当前版本不要指望它能从 UEFI 启动。**

### U 盘以什么方式被枚举

BIOS 把 U 盘当成 **USB-HDD** 或 **USB-ZIP** 来枚举，取决于主板和 U 盘。
- 首选 **USB-HDD（硬盘仿真）**：我们的镜像无分区表，按超级软盘方式也能被
  大多数 BIOS 的 USB-HDD 仿真认出来启动。
- 部分老机器/部分 U 盘，需要在 BIOS 启动菜单里选 **USB-ZIP** 或
  **Removable / FDD 模式**；反过来，有些机器选了 USB-ZIP 反而起不来。
  **如果 USB-HDD 起不来，第一件事就是换一个启动项（ZIP/FDD/HDD）试试**，
  而不是怀疑镜像。

---

## 2. 三平台制作步骤

> 通用铁律（适用于所有平台）：
> 1. 先确认你要写的盘**确实是那块 U 盘**，方法是看容量（U 盘容量和别的盘对得上吗）
>    和型号（U 盘外壳/标签上的字）。
> 2. **写错盘 = 那块盘上的数据全部清空且难以恢复。** 操作前先拔掉不相关的
>    移动硬盘，避免手滑选错。
> 3. 写完别在 Windows 里点"格式化"。直接弹出 U 盘去真机试。

### 2.1 Windows

Windows 没有自带 `dd`，有三条可行路线，**任选其一**。最稳妥的是前两条
（图形化，确认盘符时更不容易错）。

#### 路线 A：Win32 Disk Imager（图形化，最不容易写错盘）

1. 去官网下载并安装 Win32 Disk Imager（开源）。
2. 插上目标 U 盘。
3. 打开程序：
   - **Image File**：浏览选到 `os-image.bin`。
   - **Device**：下拉框选**目标 U 盘的盘符**（例如 `F:`）。
     **风险提示**：下拉框只显示盘符，务必对照"计算机"里看到的 U 盘盘符，
     不要凭记忆选。如果你有两块盘盘符相近，先拔掉无关盘再开程序。
   - 如果镜像不在列表里，把右下角文件类型改成 `*.*` 再选。
4. 点 **Write**。程序会弹窗再次确认"将写入 X:，数据会被覆盖"，确认无误点 Yes。
5. 进度条走完显示 `Write Successful`，点 **Exit**，在系统托盘里"安全弹出"U 盘。

```
# 没有命令行可复制——这是 GUI 操作。
# 唯一要你手动确认的就是 Device 下拉框里的盘符，选错盘就毁数据。
```

#### 路线 B：Rufus（必须用"DD 镜像模式"，不是 ISO 模式）

> 关键坑：Rufus 默认是 **ISO 镜像模式**，会把 os-image.bin 当 ISO 解构后
> 按自己的引导方式写，**那会丢失我们的引导扇区，起不来**。必须选 **DD 镜像模式**。

1. 插上目标 U 盘，打开 Rufus。
2. **设备**下拉框选目标 U 盘（同样按盘符/容量确认）。
3. 点 **选择**，挑 `os-image.bin`。
4. 选完之后 Rufus 会弹一个对话框：
   _"How do you want to write this image?"_（你要以哪种模式写入？）
   - 选 **`DD Image`**（DD 镜像模式），**不要选 `ISO Image`**。
5. 点**开始**，再确认一次覆盖警告。
6. 完成后直接拔出 U 盘（Rufus 写完已安全卸载）。

```
# Rufus 是 GUI，同样没有命令行。致命点只有一句：弹窗里必须选 DD Image。
```

#### 路线 C：dd for Windows（命令行，适合脚本化/无 GUI 环境）

1. 下载 `dd for Windows`（chrysocome 的 `dd.exe`），放到 PATH 或当前目录。
2. **确认物理磁盘号**（见下）。
3. 以**管理员**身份打开 cmd / PowerShell 执行：

```cmd
REM 先确认磁盘号（见 2.1 的"磁盘号确认方法"）。下面假设目标盘是 PhysicalDrive2。
REM of 后面必须是 \\.\PhysicalDriveN，N 是物理磁盘号，不是盘符！
dd if=os-image.bin of=\\.\PhysicalDrive2 bs=1M --progress
```

> 风险提示：`\\.\PhysicalDrive2` 是整个物理盘。N 写错，那块盘（可能是你的系统盘！）
> 直接被覆盖。执行前务必用下面的方法核对 N。

#### Windows 磁盘号确认方法（路线 C 必看，路线 A/B 也建议核对）

用 PowerShell 看每块磁盘的**编号、型号、容量**：

```powershell
Get-Disk | Select-Object Number, FriendlyName, Size, TotalSize, BusType | Format-Table -AutoSize
```

只看 U 盘的 BusType（`USB`）和容量（比如 15,932,346,624 字节 ≈ 16GB）来认盘。
如果想进一步确认某块盘的盘符，避免认错：

```powershell
Get-Disk -Number 2 | Get-Partition | Get-Volume | Select-Object DriveLetter, FriendlyName, Size
```

（把 `2` 换成你怀疑的盘号，看 DriveLetter 是不是你那个 U 盘的盘符。）

更底层、能看到序列号的方法（cmd 即可）：

```cmd
wmic diskdrive list brief
```

> 一句话原则：在 Windows 上，**容量对得上 + 总线类型是 USB** 才是你的 U 盘；
> 系统盘通常是 `Number 0` 且 BusType 是 `SATA`/`NVMe`，**绝对不要碰 Number 0**。

---

### 2.2 Linux

```bash
# 1) 插上 U 盘，确认设备名。看容量和型号，别凭 sda/sdb 猜。
lsblk -b -d -o NAME,SIZE,MODEL,TRAN | grep -i usb
# 或者是想看全部盘再肉眼挑：
lsblk -b -d -o NAME,SIZE,MODEL,TRAN

# 2) 确认 U 盘当前没被挂载（写裸设备前必须卸载，否则数据损坏/写入不一致）
lsblk /dev/sdX          # 看下面有没有挂载点
sudo umount /dev/sdX*   # 把该盘所有分区都卸掉（U 盘通常是 sdX1 等）

# 3) 把镜像整盘写入。of 必须是整块设备 /dev/sdX，不能是某个分区 /dev/sdX1。
#    写错盘同样毁数据——再核对一次 SIZE 对不对。
sudo dd if=os-image.bin of=/dev/sdX bs=1M status=progress conv=fsync

# 4) 等命令返回、缓冲落盘（conv=fsync 已保证），再拔出：
sync
```

> 风险提示：`/dev/sdX` 写错就是整盘清零。Linux 上最常见的事故是系统盘恰好是
> `sda` 而 U 盘是 `sdb`，但你手滑写成 `sda`。**写之前用 `lsblk` 的容量确认两遍。**

---

### 2.3 macOS

```bash
# 1) 插上 U 盘，列出所有磁盘，按容量认 U 盘（通常是 /dev/diskN，N 是数字）
diskutil list

# 2) 卸载（不是弹出）U 盘的所有卷，让原始设备可写。
#    假设 U 盘是 /dev/disk4
diskutil unmountDisk /dev/disk4

# 3) 整盘写入。of 用 /dev/rdiskN（原始设备，比 /dev/diskN 快很多）。
#    rdisk 写错同样毁数据，先按容量确认 N。
sudo dd if=os-image.bin of=/dev/rdisk4 bs=1M
#    想看进度：macOS 的 dd 不支持 status=progress，可另开一个终端按 Ctrl+T 看实时进度。

# 4) 写完确保缓冲落盘再拔：
sync
diskutil eject /dev/disk4
```

> 风险提示：macOS 上 `/dev/disk0` 通常是内置硬盘，**永远不要写 disk0**。
> 只用 `diskutil list` 里容量对得上的那块外接盘。

---

## 3. 真机 BIOS 设置清单

进 BIOS（开机狂按 Del / F2 / F12 视主板而定），逐项确认：

| 设置 | 应设为 | 说明 |
|---|---|---|
| **CSM / Legacy Boot / 兼容性支持模块** | **Enabled（开启）** | 纯 Legacy BIOS 内核，必须开 CSM 才能跑 16 位引导代码。 |
| **Secure Boot（安全启动）** | **Disabled（关闭）** | Secure Boot 只认签名 EFI，会直接拦掉我们的裸镜像。 |
| **Boot Mode / 启动模式** | **Legacy / UEFI+Legacy（CSM）**，不要纯 UEFI | 纯 UEFI 起不来（见第 1 节）。 |
| **USB 启动项优先级** | 把 **USB-HDD / USB 设备** 挪到硬盘前面 | 否则主板会先去启动系统盘，跳过 U 盘。 |
| **USB 仿真类型（如有）** | 先试 **USB-HDD**；起不来再换 **USB-ZIP / FDD** | 见第 1 节最后的枚举说明。 |
| **SATA 模式（AHCI / IDE / RAID）** | 见下 | 决定你用 `setdrive` 选几号盘（见第 5 节排查表）。 |

**关于 SATA 模式（AHCI vs IDE）——与后面 `setdrive` 的关系：**
- **AHCI 模式**：SATA 控制器走 AHCI，内核扫描 PCI class `0x0106` 控制器，
  端口号 0~N，`setdrive 4-11` 选 AHCI 盘。这是 H1b 验证过的路径。
- **IDE / 兼容模式（有时叫 RAID 或 ATA）**：控制器退化为 IDE（ATA PIO），
  内核走 `setdrive 0-3` 选 IDE 盘（主/从，主盘控制器 0/1，从盘 2/3）。
- 没把握就**先用 AHCI 模式**写盘试；进 shell 后用 `setdrive` + `ls` 看哪个盘能挂上。

---

## 4. 启动后"预期看到什么"（对照用）

接上 USB-TTL 线到主板 COM1（见第 5 节"串口一条都没有"里的接线），
115200 8N1，上电后串口应**依次**出现（屏幕也会同步显示，但无屏时看串口即可）：

```
SERIAL: COM1 115200 8N1 loopback OK          ; 串口自检通过，之后所有信息都镜像到这里
VBE: boot probe OK, LFB 0x........ (16bpp RGB565)   ; 或 "VBE: no LFB mode probed, will fallback to VGA 0x13 320x200x256"
SELFTEST: kmalloc  kernel heap        ... OK        ; 第 1 项
SELFTEST: paging   identity + map/unmap ... OK
SELFTEST: pmm      page frame allocator  ... OK
SELFTEST: elf      ELF32 loader        ... OK
SELFTEST: fpu      x87 arithmetic      ... OK
SELFTEST: calc     expression engine   ... OK
SELFTEST: exec     ring3 ELF run       ... OK
SELFTEST: fd       file descriptors    ... OK
SELFTEST: serial   COM1 diagnostic     ... OK        ; 以上 8 项 = 核心子系统
SELFTEST: pit      system timer tick   ... OK        ; 以下 13 项 = 硬件/软件探针
SELFTEST: rtc      CMOS wall clock     ... OK
SELFTEST: ata      block device MBR    ... OK
SELFTEST: pci      bus enumeration     ... OK
SELFTEST: nic      rtl8139 MAC         ... OK
SELFTEST: acpi     FADT/_S5 tables     ... OK
   （找不到 FADT 时打印：ACPI: FADT not found - legacy QEMU power）
SELFTEST: kbd      8042 controller     ... OK
SELFTEST: vga      text mem page1      ... OK
SELFTEST: tsc      cycle counter       ... OK
SELFTEST: div64    64-bit division     ... OK
SELFTEST: pipe     kernel pipe pair    ... OK
SELFTEST: string   str/fmt helpers     ... OK
AHCI: port 0 online (model: ...)                ; 若 SATA 在 AHCI 模式且有盘
AHCI: port 1 absent (SSTS.DET != 3)             ; 空端口
...
EZOS>                                            ; 进 shell，可以敲命令了
```

> 自检共 **21 项**：8 个核心子系统（kmalloc/paging/pmm/elf/fpu/calc/exec/fd）
> \+ 13 个硬件/软件探针（serial/pit/rtc/ata/pci/nic/acpi/kbd/vga/tsc/div64/pipe/string）。
> 任何一项失败只会打印一行 `SELFTEST: ... FAIL`/异常，不会拦住启动；
> 但**那一行就是排查线索**——对照上面这串，看卡在第几项之后。

---

## 5. 排查清单（现象 → 最可能原因 → 怎么验证）

> 用法：从上到下，按你"实际看到的现象"找对应行，照"怎么验证"逐一排查。
> 没有屏幕时，**第 6~8 行（串口相关）是主战场**。

| # | 现象 | 最可能原因 | 怎么验证 / 怎么办 |
|---|---|---|---|
| 1 | **完全无显示、串口也一条都没有**（机器像没通电） | ① U 盘没插好 / 没被选为启动项；② 主板根本没从 U 盘启动；③ 电源/接线问题 | 进 BIOS 启动菜单（F12/Boot Menu）手动选 USB 设备；换 USB 口（优先后置主板直连口，别用前置面板或 hub）；确认 U 盘灯有活动。 |
| 2 | **有 BIOS 画面，但报 "No bootable device / 无可引导设备"** | ① 用了 ISO 模式（Rufus）或把镜像当文件拷进分区，LBA0 不是引导扇区；② 选了纯 UEFI 模式 | 重做 U 盘，确认是 **整盘 DD 写入**（Rufus 选 DD Image）；BIOS 开 CSM、关 Secure Boot、启动模式改 Legacy。 |
| 3 | **卡在读盘**（屏幕停在 `Loading kernel...` 或 `Disk read error!`） | ① 镜像没写完整（U 盘被提前拔出 / `conv=fsync` 没等）；② U 盘主控对 `INT 13h` 扩展读支持差；③ 写错盘导致镜像残缺 | 重新整盘写入并等 `sync`；换一个品牌/U 盘的盘（某些杂牌盘 INT13 扩展读有问题）；用 `tools/make_usb_boot.py` dry-run 确认首扇区 `0x55AA` 和大小。注：`Disk read error!` 是引导代码自身打印的，说明 BIOS 已加载引导扇区、但读内核扇区失败。 |
| 4 | **卡在 A20**（进不了保护模式，无任何后续输出） | 老机器 A20 门没开，三重回退仍失败（极少见，多见于很老的 486/早期 Pentium） | 串口若连着，看是否停在 VBE/SELFTEST 之前；换更老或更新的机器；这基本是硬件兼容问题，软件侧已做三重回退。 |
| 5 | **卡在 VBE**（屏幕花屏/黑屏，但串口可能还在走） | VBE 探测卡死或 LFB 地址异常（个别集成显卡 BIOS 有 bug） | 串口若打出 `VBE: no LFB mode probed, will fallback to VGA 0x13 320x200x256` 是正常的，会自动回退；若完全卡死无串口，是显卡 VBE BIOS 问题，换显示器接口/换机器。 |
| 6 | **串口一条都没有**（连 `SERIAL:` 都没有） | ① 串口线接法错（TX/RX 交叉、GND 没接）；② 波特率/参数不对（必须是 115200 8N1）；③ 主板 COM1 被禁用或地址不是 0x3F8；④ 用的不是 COM1 | **接线**：USB-TTL 模块的 **TX→主板 COM1 RX（接主板 9 针座的 2 脚）、RX→主板 COM1 TX（3 脚）、GND→GND（5 脚）**；注意是交叉（模块的 TX 接对方的 RX）。**参数**：终端软件 115200 8N1 无流控（无 XON/XOFF、无 RTS/CTS）。**BIOS**：进 BIOS 确认 `Serial Port / COM1` 是 Enabled，模式 Standard（不是 SOL/Disabled），地址 0x3F8/IRQ4。内核只接 COM1（0x3F8），别接 COM2。 |
| 7 | **串口有输出，但屏幕全黑** | 只是没接显示器或显示器输入源选错；内核本身正常 | 这是**最理想**的排障状态——所有信息都在串口。确认显示器信号源（VGA/HDMI 切对）；若坚持要屏，按第 5 节看 K 项。 |
| 8 | **只有 `SERIAL: COM1 115200 8N1 loopback OK` 一行，之后就没了** | 串口自检通过了，但内核早期（VBE/A20/保护模式切换）在某处静默挂死 | 对照第 4 节预期输出，缺的就是卡住的那一步：没 `VBE:` 行 → 卡 VBE（见第 5 行）；连 SELFTEST 都没有 → 卡在 A20/保护模式（见第 4 行）。若串口在 SERIAL 后彻底停，多半是显示/保护模式切换时的硬件异常。 |
| 9 | **进得了 shell（EZOS> 出来了），但 AHCI/SATA 盘没识别** | ① SATA 模式是 IDE，却用 `setdrive 4-11`；② 没盘 / 端口空；③ 控制器不是 class 0x0106 | 在 shell 里：先 `pci` 看有没有 `0106` 类设备；`AHCI: port N online` 有没有打出来；按 BIOS 里 SATA 模式选盘：`AHCI` 模式用 `setdrive 4`（或 5/6…），`IDE` 模式用 `setdrive 0-3`；换端口号再 `ls` 试。空端口会打 `absent (SSTS.DET != 3)`，那是物理没接盘。 |
| 10 | **键盘/鼠标无响应** | 真机上 **USB HID 键盘鼠标要到 H2 才支持**；当前内核只认 **PS/2** 键鼠（走 8042 控制器） | **用 PS/2 接口的有线键鼠**（圆口，不是 USB）。USB 键盘在 H1c 阶段不会被识别，表现为"完全没反应"——这不是镜像写错，是功能未到。若主板只有 USB 口，开 BIOS 的 **"USB Legacy / PS/2 Emulation"** 选项，让固件把 USB 键盘模拟成 PS/2（部分主板有效，老主板可能不稳定）。 |
| 11 | **进 shell 但 `ls` 看不到数据盘/文件系统** | 没 `setdrive` 选盘，或所选盘没有内核支持的 FS | shell 里 `setdrive <n>` 切换盘号（AHCI 用 4-11，IDE 用 0-3），再 `ls`；内核支持 exFAT/FAT/ext4/NTFS/F2FS/EROFS/ReFS，盘得是这些格式之一。引导盘（0 号）不会自动挂载，防误格。 |
| 12 | **串口只到 `SERIAL:` 就 `SERIAL: COM1 not present - debug output disabled`** | 内核没找到 COM1（自检判定串口不存在），后续所有 klog 都不会镜像到串口 | 这是串口自检**失败**的标志（和"一行 OK"相反）。说明 COM1 没枚举到/地址不对。进 BIOS 确认 COM1 Enabled 且地址 0x3F8；或主板根本没 COM1（需用 PCI 串口卡——当前内核只认板载 0x3F8）。 |

> 口诀：**没串口输出先查线（TX/RX 交叉 + GND + 115200 8N1）；有串口但卡住就对照第 4 节看缺哪一行；进 shell 没盘查 `setdrive` 和 SATA 模式；键鼠不动换 PS/2。**

---

## 6. 附：一键检查镜像 + 安全写盘工具

仓库自带 `tools/make_usb_boot.py`，它**默认只做只读检查**，列出本机磁盘、
校验镜像（大小是 512 的倍数、首扇区以 `0x55AA` 结尾）、打印将要写入的字节数和
目标盘容量；**只有显式 `--write --disk N` 并二次输入盘号确认后才会真写**，
绝不自动选盘。详见该脚本内的 `--help`。

```bash
# 只读自检（不写任何东西）：列出磁盘 + 校验镜像
python3 tools/make_usb_boot.py

# 真的写盘（务必先核对 dry-run 打印的磁盘号和容量）
python3 tools/make_usb_boot.py --write --disk 2
# 脚本会再次要求你输入 "2" 确认，输错就中止，绝不误写。
```

> 想用脚本写盘，Windows 上请以**管理员**运行（写物理盘需要权限）；
> Linux/macOS 用 `sudo`。写错盘的后果脚本无法替你承担——它只负责"逼你确认两遍"。
