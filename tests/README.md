# EZOS E2E 回归套件

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
