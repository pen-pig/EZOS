/*
 * f2fs.h - F2FS（Flash-Friendly File System）驱动（读 + 写）
 *
 * refs:
 *   - GRUB grub-core/fs/f2fs.c (GPLv3+, Jaegeuk Kim)
 *     CP/NAT 解析、节点映射、inline data/dentry 遍历逻辑
 *   - Linux fs/f2fs/ + include/uapi/linux/f2fs.h - 磁盘结构定义
 *   - f2fs-tools mkfs/f2fs_format.c - 格式化布局参考
 */
#ifndef F2FS_H
#define F2FS_H

#include "types.h"
#include "fs.h"

typedef struct {
    uint32_t part_start;            /* 卷起始绝对 LBA */
    uint16_t bytes_per_sector;      /* 恒 512 */
    uint8_t  sectors_per_cluster;   /* 4KB 块 = 8 扇区 */
    uint32_t cluster_count;         /* main 区总块数 */
    uint32_t volume_sectors;        /* 卷总扇区数 */
    uint32_t used_clusters;         /* CP valid_block_count（df） */
} f2fs_info_t;

/* 在 drive 号盘 part_start 扇区处尝试挂载 F2FS；0=成功 */
int f2fs_mount(uint8_t drive, uint32_t part_start);

const f2fs_info_t *f2fs_get_info(void);

/* 路径均为从根开始的绝对路径（"/docs/readme.txt"），由 fs.c 拼接 cwd */
int      f2fs_read_file(const char *path, uint8_t *buffer, uint32_t max_size);
uint32_t f2fs_get_file_size(const char *path);
int      f2fs_read_dir(const char *path, fs_dir_entry_t *entries, int max_entries);
int      f2fs_is_dir(const char *path);       /* 1=目录 0=文件 -1=不存在 */
uint32_t f2fs_get_file_clusters(const char *path);

/* 写入 API（create-or-replace 语义，与 exfat_/fat_ 一致；
 * 仅支持本驱动 format 出的 inline dentry 单 NAT 块小卷模型） */
int      f2fs_create_file(const char *name, const uint8_t *data, uint32_t size);
int      f2fs_delete_file(const char *name);
int      f2fs_mkdir(const char *name);

/* 格式化：在 drive 上创建 F2FS 卷（2MB，4KB 块，inline dentry 模型）
 * 并挂载；0=成功 */
int      f2fs_format(uint8_t drive);

#endif
