/*
 * ext4.h - ext2/ext3/ext4 驱动（读 + 写）
 *
 * refs:
 *   - GRUB grub-core/fs/ext2.c (GPLv3+) - 只读 extent/htree 遍历参考
 *   - Linux fs/ext4/ext4.h, e2fsprogs lib/ext2fs/ext2_fs.h - 磁盘结构定义
 *   - e2fsprogs misc/mke2fs.c - 格式化参数参考
 */
#ifndef EXT4_H
#define EXT4_H

#include "types.h"
#include "fs.h"

typedef struct {
    uint32_t part_start;            /* 卷起始绝对 LBA */
    uint16_t bytes_per_sector;      /* 恒 512 */
    uint8_t  sectors_per_cluster;   /* 块大小 / 512 */
    uint32_t cluster_count;         /* 文件系统总块数 */
    uint32_t volume_sectors;        /* 卷总扇区数 */
    uint32_t used_clusters;         /* 已用块数（df） */
} ext4_info_t;

/* 在 drive 号盘 part_start 扇区处尝试挂载 ext2/3/4；0=成功 */
int ext4_mount(uint8_t drive, uint32_t part_start);

const ext4_info_t *ext4_get_info(void);

/* 路径均为从根开始的绝对路径（"/docs/readme.txt"），由 fs.c 拼接 cwd */
int      ext4_read_file(const char *path, uint8_t *buffer, uint32_t max_size);
uint32_t ext4_get_file_size(const char *path);
int      ext4_read_dir(const char *path, fs_dir_entry_t *entries, int max_entries);
int      ext4_is_dir(const char *path);       /* 1=目录 0=文件 -1=不存在 */
uint32_t ext4_get_file_clusters(const char *path);

/* 写入 API（create-or-replace 语义，与 exfat_/fat_ 一致） */
int      ext4_create_file(const char *name, const uint8_t *data, uint32_t size);
int      ext4_delete_file(const char *name);
int      ext4_mkdir(const char *name);

/* 格式化：在 drive 上创建 ext4 卷（1024B 块，单块组）并挂载；0=成功 */
int      ext4_format(uint8_t drive);

#endif
