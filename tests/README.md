# EZOS E2E 回归套件

> **真机启动与 U 盘制作见 [`docs/USB_BOOT.md`](../docs/USB_BOOT.md)**：把 `os-image.bin`
> 整盘写进 U 盘（禁止拷成文件）、三平台步骤、BIOS 设置与"无屏只有串口"的排查清单。
> 配套安全写盘工具：`python3 tools/make_usb_boot.py`（默认只读检查，必须显式
> `--write --disk N` 并二次确认才真写，绝不自动选盘）。

在 **src 根目录**下运行（脚本自己按 `ROOT = 上级目录` 定位 os-image.bin / disk.img）。
前置：`ninja -f build.ninja` 已产出 `os-image.bin`、`disk.img`。
退出码 0 = 全部断言通过。

| 脚本 | 覆盖 | 端口 | 参考耗时 |
|---|---|---|---|
| `test_regress.py` | 广义回归：开机进 shell、ver/ps/ls/cat/calc、exec HELLO.ELF、nic、selftest、`mem 0xFFFFFFFF 4096`、空 calc、末尾仍可用；全程断言无 panic | QMP 4463 | ~1.5 min |
| `test_fs_matrix.py` | 8 种 FS（exFAT/FAT12/FAT16/FAT32/ext4/NTFS/F2FS/ReFS）× format/df/write/ls/cat/rm | QMP 4493 | ~5 min |
| `test_edge.py` | 10 个极端输入，每用例独立启一个 QEMU | QMP 4496 | ~7 min |
| `test_rmdir.py` | rmdir 三方语义（6 可写 FS × 13 用例：空/非空/不存在/`.`/`..`/根…） | QMP 4478 | ~4 min |
| `test_netapp.py` | ping + httpd（raw socket 对端，帧带 4B 长度前缀） | netdev 4477 + monitor 4479/4480 | ~2 min |
| `test_step7_sock.py` | 7.3：UDP/TCP echo 服务端，pcap 逐帧断言校验和、三次握手、四次挥手 | monitor 55555 + hostfwd 7000/7001 | ~1 min |
| `test_step7_tcp.py` | 7.4：主动 connect、乱序重组、重传（`-netdev socket` 直连对端） | netdev 4476 + monitor 55666 | ~1 min |
| `test_vi.py` | vi 功能回归：新建→插入→`:wq`→cat 回读→二次打开→退出后 shell 可用（防语法高亮渲染改动破坏编辑路径） | QMP 4482 | ~1 min |
| `test_jobs.py` | 后台任务：`cmd &` 立即回提示符、`[n] pid`、jobs 列表、退出后 `[n] done` 收割、ps 无 zombie、前台不受影响 | QMP 4485 | ~1 min |
| `test_power.py` | 真机点亮 H1a：COM1 串口镜像（自检/ACPI/banner）、ACPI S5 断电（QEMU 进程退出）、8042 重启后 shell 可用 | QMP 4486 + serial file | ~2 min |
| `test_ahci.py` | 真机点亮 H1b：ich9-ahci 控制器探测 + 端口上线（IDENTIFY 型号）、`setdrive 4` 后 exFAT format/write/ls/cat/rm 全走 AHCI DMA；全程走串口不依赖 OCR | QMP 4487 + serial 4488 | ~2 min |

`ezocr.py` 是前三个脚本共用的 VGA 文本截图 OCR（真字库像素匹配），不要单独删。

## 为什么这些脚本要进版本库

它们原本都在 `temp/`，而 `.gitignore` 忽略了 `temp/*`——9-21 那次清空把旧的
14 套脚本全部毁掉且无从恢复，项目一度只剩 2 套验证手段。这个内核没有别的测试
框架，E2E 就是唯一的正确性防线，所以与源码同等对待。

真正的一次性调试产物（`dbg_*.bin/ppm`、`probe_*.ppm`、pcap 抓包、`o.txt`、
`commit_msg*.txt`）仍留在 `temp/`，继续被忽略，不进仓库。

## 写断言时的两条铁律

1. **取"最近一条命令的输出"用 `split(">")[-2]`**：命令跑完 shell 会再打一个新
   提示符，`[-1]` 恒为空串（表现为 ls 假红、rm 假绿）。
2. **等待用"屏幕文本稳定"而非固定秒数**：不同 FS 驱动快慢差数倍，固定 sleep
   会让同一份脚本忽红忽绿。
3. **串口读线程必须区分 `socket.timeout`**：`create_connection(timeout=10)`
   建的 socket，guest 空闲超过 10s 时 `recv` 会抛 timeout。**把它当异常
   `break` 掉 = 读线程静默死亡**，之后所有断言都收不到输出，现象极像"系统
   挂死"（实际 guest 完全正常——用 screendump 能看到命令照常执行、输出照常
   上屏）。正确写法：`except socket.timeout: continue`。
4. 断言尽量等"必然出现的子串"；需要断言"某串不出现"时，先等一个必然出现的
   子串拿到整段输出，再在里面断言 absence——避免为了等不到而空转。
