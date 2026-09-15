# EZOS 深度源码分析与功能建议报告

> 分析日期：2026-09-13 · 代码基线：D:\MyOS\src（约 21,300 行 C + 2 个 NASM 文件）
> 项目定位（据项目说明）：手搓系统、可能用于生产级环境、代码需严谨

---

## 一、架构执行模型（一句话）

**单任务、无内存管理、中断驱动的轮询式 32 位保护模式内核**——前台一个大循环（shell ↔ GUI），IRQ0/1/12 三个中断只做数据入队，所有 I/O 靠主动轮询完成。

### 1.1 启动链路（已验证实现）

```
boot.asm(MBR 512B)
  ├─ INT 13h AH=42h 扩展读：768 扇区(384KB)内核 → 0x10000，每批 64 扇区、3 次重试
  ├─ VBE 探测 0x11A→0x117→0x114→0x111（16bpp+LFB 硬校验），结果存 0x5000
  ├─ A20 三重回退（BIOS int15h → 0x92 → KBC）
  └─ GDT(3 描述符) → 保护模式 → kernel_entry.asm
       ├─ esp=0x90000，清零 .bss 与 .bss.hi(1MB-2MB 高内存缓冲区)
       └─ kernel_main()
```

内存布局（linker.ld 编译期断言保证）：

| 区域 | 范围 | 限制 |
|---|---|---|
| 内核镜像 | 0x10000 起 | ≤384KB（ASSERT） |
| BSS | 0x10000+镜像 | ≤0x90000 栈区（ASSERT） |
| 高内存 BSS | 1MB–2MB | ≤2MB，放只读 FS 大缓冲（NOLOAD） |
| 栈 | 0x90000 向下 | 无栈溢出检测 |
| GUI 后缓冲 | 16MB 固定地址 | 上限 1600×1200 |

### 1.2 中断与 I/O 模型

- IDT 256 门，**实际只挂 3 个**：IRQ0(PIT 1000Hz)/IRQ1(键盘)/IRQ12(鼠标)
- **CPU 异常处理门完全空缺**——`isr_install()` 是空函数（源码注释自认 "isr_install is a stub"），任何 #GP/#PF/#DF 直接三重故障复位
- PIC 重映射 0x20-0x2F，MSR 0x1B 关 LAPIC（QEMU 兼容处理）
- 键盘/鼠标驱动质量高：环形缓冲、同步位校验、ACK/BAT 字节防御（注释详述了 0xFA 包流错位教训）、溢出包丢弃——这是全项目驱动里最严谨的部分
- **ATA PIO 无 cli/sti 临界区**：读盘靠 400 万次轮询超时；IRQ0 在读盘期间仍在触发，读盘代码与中断共享的数据理论上存在竞争（当前单任务模型下危害有限）

### 1.3 已实现的稳健性细节（值得肯定）

- `linker.ld` 三条 ASSERT 编译期锁死内存边界
- `fs.c` 用 `typedef char[cond?1:-1]` 做目录项布局静态一致性检查
- 键盘缓冲/滚轮缓冲满时丢新数据（不溢出）
- `my_atoi` 溢出钳制、shell `udiv64_32` 手写 64/32 除法避开 libgcc
- fs.c：0 号引导盘不参与自动挂载回退、拒绝格式化（防抹启动镜像）
- 引用源清晰标注（RetrOS-32 MIT、GRUB/Linux、f2fs-tools、sinwindows）

---

## 二、文件系统层（项目重心，约 9,900 行）

### 2.1 分发层设计（fs.c，583 行）

挂载顺序：exFAT → FAT12/16/32（MBR 扫描+superfloppy 回退）→ ext4 → NTFS → F2FS → EROFS → ReFS；首选从盘，失败轮 1-3 号盘。

两个亮点：
1. **create-or-replace 统一语义**：`fs_create_file` 先删后建，修复了记事本/vi 二次保存失败的历史 bug——对可写 FS 是务实选择
2. **路径规范化器 `fs_ro_abs`**：分段处理 `.`/`..`，上限 64 段，为绝对路径型驱动（ext4/NTFS/F2FS/EROFS/ReFS）统一拼 cwd

### 2.2 驱动矩阵

| 驱动 | 行数 | 读 | 写 | 格式化 | 备注 |
|---|---|---|---|---|---|
| exFAT | 1,226 | ✓ | ✓ | ✓ | FAT 长链 |
| FAT12/16/32 | 1,394 | ✓ | ✓(LFN) | ✓ | mkfs 含 LFN 测试条目 |
| ext4 | 1,532 | ✓ | ✓ | ✓ | |
| NTFS | 1,966 | ✓ | ✓ | ✓ | |
| F2FS | 1,948 | ✓ | ✓ | ✓ | f2fs-tools 布局 |
| EROFS | 1,056 | ✓ | ✗ | ✗ | 真只读 |
| ReFS | 620 | ✓ | ✓ | ✓ | |

### 2.3 生产级视角的风险点（重点）

**P0 — 掉电/崩溃一致性**
- 全部可写 FS 的元数据更新无日志、无两阶段提交。FAT/exFAT 断电易出孤儿簇/交叉链；ext4 声称写支持却无 journal 回放，反而比只读更危险
- **建议**：短期加"写前校验+幂等重放"标记；中期给 exFAT/FAT 加 dirty bit + fsck 命令；ext4 写路径要么补 journal 回放，要么降级为只读

**P0 — 无 CPU 异常处理**
- 缺 ISRS 的后果：坏介质导致 FS 驱动解引用野指针时不是蓝屏而是静默重启，生产环境无法留尸检现场
- **建议**：补 32 个 CPU 异常门（#GP/#PF/#DF 优先），打寄存器+CR2 快照到 VGA/串口后停机——工作量约 200 行汇编+C，是全项目性价比最高的改造

**P1 — 静态缓冲与文件大小上限**
- vi 编辑器 4KB 固定缓冲、256 字节文件名、64 段路径、GW_MAX_WINDOWS=8
- **建议**：引入简单 slab/free-list 堆（哪怕是 1MB 静态池），fs_read_file API 增加溢出返回码

**P1 — 无 RTC 时区/DST、CMOS 读时未等 update-in-progress**
- `rtc_read` 读 6 个寄存器间可能跨秒翻转，日期偶发错 1 天
- **建议**：读前轮询 regA UIP 位，或双读一致法

**P2 — 编码混乱**：kernel.c/tty.c 等为 GBK 乱码注释，gfxwin.c/fs.c 为正常 UTF-8。建议统一 UTF-8（gcc 加 `-finput-charset=GBK` 过渡或 iconv 批量转换）

---

## 三、交互层现状

- **shell**：约 80 个命令，覆盖文件操作全套（ls/cd/cp/mv/rm/mkdir/cat/head/tail/grep/wc/hexdump）、系统信息（cpuid/meminfo/rdtsc/uptime）、磁盘实验（readdisk/format/setdrive）、7 款游戏、vi、Tab 补全+历史（HISTORY_SIZE=8）
- **GUI**：双主题（Win10 + Ubuntu GNOME Dock），任务栏/开始菜单/窗口拖动/最小化/最大化/Alt+Tab，双后端自适应（VBE 16bpp 1280×1024 → VGA 0x13 320×200）
- **测试**：自研 **VGA 文本模式 OCR + QMP 协议** 的端到端框架（5 个 Python 文件 1,440 行）——不依赖 QEMU 内部设施，从像素反查字符，跨平台可复现；ci/build.sh 从 boot.asm 自动推导 pad 尺寸避免 main/dev 不一致

---

## 四、功能建议（按投入产出比排序）

### 第一梯队：低投入 · 高回报（每项 ≤400 行）

**1. 补 CPU 异常处理 + panic 屏幕（P0，前面已述）**
BSOD 风格蓝屏：异常号/错误码/寄存器/最近 shell 命令。生产级环境的底线能力。

**2. 内存管理器 v1：静态池 free-list**
- 1MB 池 + 16 字节粒度 + 头部魔数检测越界
- 先给 vi 缓冲、games 状态、FS 长文件名解锁大小限制
- 为未来 everything 铺路（信号量、管道都需要动态内存）

**3. `hdparm`/SMART 类命令：`smart` 读取 ATA SMART 健康值**
生产盘的死亡预警。现成的 ATA PIO 通道上加 0xB0 命令即可，约 150 行。

**4. `dmesg` 命令：klog 缓冲化**
现在 klog 直写 VGA，退出 GUI 后历史全丢。改成 32KB 环形缓冲 + dmesg 命令回看，故障排查刚需，约 100 行。

**5. `md5sum`/`crc32` 命令**
文件校验，配合 readdisk 做坏盘证据链，约 80 行。

### 第二梯队：中投入 · 填补架构空白（400-1,500 行）

**6. 协作式多任务 v1（round-robin）**
现有主循环已经是"每帧轮询键盘鼠标"，天然适合改造：
- 每任务独立 4KB 栈 + task struct（保存 esp/pcx）
- PIT IRQ0 尾部做 yield 切换（IRQ0 处理器已有，加 30 行上下文切换）
- 先跑两个 demo 任务：时钟窗口 + 打字练习程序
- **注意**：先做 2（堆），任务栈需要动态分配

**7. 键盘增强：全套 5 个命令映射 + CapsLock/NumLock LED**
现只映射 Shift/Alt。补 Ctrl（Ctrl+C 中断当前程序、Ctrl+L 清屏）、CapsLock 状态机，约 120 行。

**8. exFAT/FAT fsck（扫描孤儿簇 + 修复 dirty bit）**
配合 2.3 节 P0 掉电一致性，先只做检测报告不做修复，报告孤儿链/环/坏簇号，约 300 行。

**9. `history` 深度化 + `alias` 系统**
HISTORY_SIZE 8→64 存入 SCORES.DAT 同款持久化机制，`!n` 重复执行，Ctrl+R 增量搜索。

**10. GUI 双缓冲裁剪**
当前 GUI 全屏重绘，320×200 下尚可，1280×1024 全屏 blit 耗时明显（16MB 后缓冲已就位）。实现脏矩形链表，每帧只 blit 变化区域，游戏帧率和拖动流畅度都会显著提升。

### 第三梯队：战略级（1,500+ 行，生产级必选）

**11. 用户态与系统调用（Ring3）**
现有"无 TSS、无用户模式"（kernel.c 日志自述）。路径：
- TSS + 一个 call-gate 或 SYSENTER/SYSCALL&SYSRET（Pentium+ 都支持，CPUID 已探测）
- 先只开放 3 个系统调用：read/write/exit 的最小 read-eval-print 程序加载器
- 这是"手搓 OS"走向"能跑用户程序 OS"的分水岭

**12. ELF 加载器 + 用户程序工具链**
- EZOS 自带 ld（binutils），内核能跑后交叉编译 hello.elf 放进数据盘
- shell 加 `exec hello.elf` 命令
- 配合 11 的分界线方案：所有应用（vi/games/计算器）可渐进迁出内核，镜像 384KB 上限问题同时缓解

**13. 串口控制台（COM1 115200）**
QEMU `-serial` 即得第二个控制台。dmesg 输出镜像到串口后：
- 无 OCR 的平台也能跑端到端测试（Python 脚本直接读串口，比 OCR 快 10 倍）
- 真机调试（USB 转 TTL）不再依赖 VGA 字符 OCR
- 约 200 行 + 测试框架改串口读取

**14. 增量构建产物缓存**：ninja 已支持，把 temp/ 中间产物纳入依赖图，实现改动一个 .c 只重编译该文件（当前 build_and_test.bat 每次全量）。**修正**：细看 build.ninja 已含 `depfile = $out.d` 与 `deps = gcc`，实际已增量——此项转为"补一个 `ninja -t clean` 后全量验证的 CI 夜测"。

---

## 五、总结判断

**优势**：FS 层广度罕见（8 驱动可写 6 个）、驱动细节严谨（键盘/鼠标防御性编程、ATA 浮动总线检测）、自研 OCR 测试闭环有工程想象力、构建链成熟（ninja+CI+自动 pad 推导）。

**短板排序**：① CPU 异常处理空缺 ② 无内存管理 ③ FS 写路径无崩溃一致性 ④ 单任务无用户态。前两项是"生产级"声明的直接否定项，但都是 1-2 天工作量能补的洞。

**建议路线**：先做 1（异常+panic）→ 2（堆）→ 4（dmesg）→ 6（多任务）→ 11（Ring3）→ 12（ELF），这是从 demo 到系统的主线；FS 一致性（8/10）与串口（13）穿插进行。
