#ifndef FAT_H
#define FAT_H

#include "types.h"

/* FAT12/16/32 卷信息（挂载后只读快照，供 fs.c 统一导出） */
typedef struct {
    uint16_t bytes_per_sector;      /* 512/1024/2048/4096 */
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entry_count;      /* FAT12/16 有效；FAT32 为 0 */
    uint32_t total_sectors;         /* 卷总扇区数 */
    uint32_t fat_sectors;           /* 每 FAT 扇区数 */
    uint32_t root_cluster;          /* FAT32 根目录首簇 */
    uint32_t first_data_sector;     /* 分区相对：数据区首扇区 */
    uint32_t cluster_count;         /* 数据簇数 */
    uint32_t root_dir_sector;       /* FAT12/16：根目录首扇区 */
    uint32_t root_dir_sectors;
    uint32_t part_start;            /* 卷起始绝对 LBA（fs.c 导出用） */
} fat_info_t;

/* 目录项输出结构（与 exfat_dir_entry_t / fs_dir_entry_t 布局一致） */
typedef struct {
    char     name[256];
    uint32_t size;
    uint8_t  is_dir;
} fat_dir_entry_t;

/* 探测引导扇区是否为 FAT（返回 12/16/32，非 FAT 返回 0） */
int fat_probe(const uint8_t *boot_sector);

/* 在 drive 号盘 part_start 扇区处挂载 FAT 卷；成功返回 12/16/32，失败返回 0 */
int fat_mount(uint8_t drive, uint32_t part_start);

int fat_ready(void);
const fat_info_t *fat_get_info(void);

/* 与 exFAT 相同语义的文件/目录 API（fs.c 分发调用） */
int      fat_read_file(const char *name, uint8_t *buffer, uint32_t max_size);
uint32_t fat_get_file_size(const char *name);
int      fat_create_file(const char *name, const uint8_t *data, uint32_t size);
int      fat_delete_file(const char *name);
int      fat_list_root(void);
int      fat_read_dir(fat_dir_entry_t *entries, int max_entries);
int      fat_change_dir(const char *name);
int      fat_mkdir(const char *name);
uint32_t fat_cwd_cluster(void);
const char *fat_cwd_path(void);
uint32_t fat_count_used_clusters(void);
uint32_t fat_get_file_clusters(const char *name);

#endif
