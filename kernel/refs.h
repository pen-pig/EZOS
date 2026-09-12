/*
 * refs.h - ReFS（Resilient File System）驱动（读 + 写，简化布局）
 *
 * 实现说明（ReFS 无官方公开规范）：
 *   - 卷头（LBA 0，512B）采用真实 ReFS 结构：0x03 "ReFS\0\0\0\0" 签名 +
 *     0x10 "FSRS" 文件系统识别结构（含 MSDN 校验和算法），Windows 工具
 *     可正确识别本驱动格式化卷的文件系统类型；
 *   - 真实 ReFS 元数据为 16KB 页 B+ 树 + 完整性流（libfsrefs 逆向文档），
 *     本驱动采用自有简化布局（0x40 处 "EZOSREFS" 自标识）：位图分配 +
 *     定长 512B 目录项数组 + 连续簇文件数据，语义与 f2fs 简化模型一致；
 *   - 非 "EZOSREFS" 标识的 ReFS 盘（真实 Windows 格式化）不挂载。
 *
 * refs:
 *   - libyal/libfsrefs documentation/ReFS.asciidoc - 卷头/FSRS/版本表
 *   - MSDN FILE_SYSTEM_RECOGNITION_STRUCTURE - FSRS 校验和算法
 *   - MSDN "Computing a File System Recognition Checksum"
 *   - Wikipedia "Resilient File System"
 */
#ifndef REFS_H
#define REFS_H

#include "types.h"
#include "fs.h"

typedef struct {
    uint32_t part_start;            /* 卷起始绝对 LBA */
    uint16_t bytes_per_sector;      /* 恒 512 */
    uint8_t  sectors_per_cluster;   /* 4KB 簇 = 8 扇区 */
    uint32_t cluster_count;         /* 数据区总簇数 */
    uint32_t volume_sectors;        /* 卷总扇区数 */
    uint32_t used_clusters;         /* 目录项 n_clusters 之和（df） */
} refs_info_t;

/* 在 drive 号盘 part_start 扇区处尝试挂载 ReFS；0=成功 */
int refs_mount(uint8_t drive, uint32_t part_start);

const refs_info_t *refs_get_info(void);

/* 路径均为从根开始的绝对路径（"/docs/readme.txt"），由 fs.c 拼接 cwd */
int      refs_read_file(const char *path, uint8_t *buffer, uint32_t max_size);
uint32_t refs_get_file_size(const char *path);
int      refs_read_dir(const char *path, fs_dir_entry_t *entries, int max_entries);
int      refs_is_dir(const char *path);       /* 1=目录 0=文件 -1=不存在 */
uint32_t refs_get_file_clusters(const char *path);

/* 写入 API（create-or-replace 语义，与 exfat_/fat_ 一致；
 * 仅支持本驱动 format 出的简化布局小卷模型） */
int      refs_create_file(const char *name, const uint8_t *data, uint32_t size);
int      refs_delete_file(const char *name);
int      refs_mkdir(const char *name);

/* 格式化：在 drive 上创建 ReFS 卷（2MB，4KB 簇，位图 + 128 目录项模型）
 * 并挂载；0=成功 */
int      refs_format(uint8_t drive);

#endif
