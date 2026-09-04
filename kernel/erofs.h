/*
 * erofs.h - EROFS（Enhanced ROM File System）只读驱动
 *
 * refs:
 *   - Linux fs/erofs/erofs_fs.h（GPL-2.0-only OR Apache-2.0）
 *     磁盘结构：super_block / inode compact+extended / dirent / z_erofs 索引
 *   - Linux fs/erofs/{inode.c,data.c,zmap.c,namei.c,super.c} - 布局映射规则
 *   - U-Boot fs/erofs 目录 (GPL-2.0+) - zmap 只读映射参考实现
 *   - Mark Adler puff.c (zlib license) - DEFLATE inflate 参考实现
 *   - LZ4 Block Format (https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md)
 *   - erofs-utils mkfs.erofs - 镜像生成布局
 */
#ifndef EROFS_H
#define EROFS_H

#include "types.h"
#include "fs.h"

typedef struct {
    uint32_t part_start;            /* 卷起始绝对 LBA */
    uint16_t bytes_per_sector;      /* 恒 512 */
    uint8_t  sectors_per_cluster;   /* 块大小 / 512 */
    uint32_t cluster_count;         /* 已用块数（blocks，statfs 语义） */
    uint32_t volume_sectors;        /* 卷总扇区数（MBR 分区长度；未知为 0） */
    uint32_t used_clusters;         /* 只读 FS：等于 blocks */
} erofs_info_t;

/* 在 drive 号盘 part_start 扇区处尝试挂载 EROFS；0=成功 */
int erofs_mount(uint8_t drive, uint32_t part_start);

const erofs_info_t *erofs_get_info(void);

/* 路径均为从根开始的绝对路径（"/docs/readme.txt"），由 fs.c 拼接 cwd */
int      erofs_read_file(const char *path, uint8_t *buffer, uint32_t max_size);
uint32_t erofs_get_file_size(const char *path);
int      erofs_read_dir(const char *path, fs_dir_entry_t *entries, int max_entries);
int      erofs_is_dir(const char *path);       /* 1=目录 0=文件 -1=不存在 */
uint32_t erofs_get_file_clusters(const char *path);

#endif
