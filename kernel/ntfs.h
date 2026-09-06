/*
 * ntfs.h - NTFS 驱动（读 + 写）
 *
 * refs:
 *   - GRUB grub-core/fs/ntfs.c + include/grub/ntfs.h (GPLv3+)
 *     fixup / run list / INDEX_ROOT / INDEX_ALLOCATION 解析逻辑
 *   - NTFS.com 与 flatcap linux-ntfs 文档（磁盘结构字段偏移）
 *     https://flatcap.github.io/linux-ntfs/ntfs/
 *   - ntfs-3g ntfsprogs/mkntfs.c - 格式化布局参考
 */
#ifndef NTFS_H
#define NTFS_H

#include "types.h"
#include "fs.h"

typedef struct {
    uint32_t part_start;            /* 卷起始绝对 LBA */
    uint16_t bytes_per_sector;      /* 恒 512 */
    uint8_t  sectors_per_cluster;
    uint32_t cluster_count;         /* 卷总簇数 */
    uint32_t volume_sectors;        /* 卷总扇区数 */
    uint32_t used_clusters;         /* $Bitmap 置位簇数（df） */
} ntfs_info_t;

/* 在 drive 号盘 part_start 扇区处尝试挂载 NTFS；0=成功 */
int ntfs_mount(uint8_t drive, uint32_t part_start);

const ntfs_info_t *ntfs_get_info(void);

/* 路径均为从根开始的绝对路径（"/docs/readme.txt"），由 fs.c 拼接 cwd */
int      ntfs_read_file(const char *path, uint8_t *buffer, uint32_t max_size);
uint32_t ntfs_get_file_size(const char *path);
int      ntfs_read_dir(const char *path, fs_dir_entry_t *entries, int max_entries);
int      ntfs_is_dir(const char *path);       /* 1=目录 0=文件 -1=不存在 */
uint32_t ntfs_get_file_clusters(const char *path);

/* 写入 API（create-or-replace 语义，与 exfat_/fat_ 一致） */
int      ntfs_create_file(const char *name, const uint8_t *data, uint32_t size);
int      ntfs_delete_file(const char *name);
int      ntfs_mkdir(const char *name);

/* 格式化：在 drive 上创建 NTFS 卷（4KB 簇，驻留 INDEX_ROOT 小目录模型）
 * 并挂载；0=成功 */
int      ntfs_format(uint8_t drive);

#endif
