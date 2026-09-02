/*
 * fs.h - 文件系统统一分发层
 *
 * 职责：
 *   - MBR 分区扫描（含 superfloppy 整盘卷）
 *   - exFAT / FAT12 / FAT16 / FAT32 自动识别与挂载
 *   - 对内核其余部分提供与具体文件系统无关的 fs_* API
 *   - fs_create_file 为 create-or-replace 语义（覆盖写）
 */
#ifndef FS_H
#define FS_H

#include "types.h"

/* 当前挂载的文件系统类型 */
#define FS_NONE  0
#define FS_EXFAT 1
#define FS_FAT12 12
#define FS_FAT16 16
#define FS_FAT32 32

/* 目录项输出结构（与 exfat_dir_entry_t / fat_dir_entry_t 布局一致） */
typedef struct {
    char     name[256];
    uint32_t size;
    uint8_t  is_dir;
} fs_dir_entry_t;

/* 通用卷信息（两种后端的公共字段子集） */
typedef struct {
    uint8_t  type;                /* FS_* */
    uint8_t  drive;               /* ATA 驱动器号 0-3 */
    uint32_t part_start;          /* 卷起始绝对 LBA */
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint32_t cluster_count;       /* 数据簇总数 */
    uint32_t volume_sectors;      /* 卷总扇区数 */
} fs_info_t;

/* 挂载：0=成功（已挂载任意文件系统）；-1=无介质；-2=有介质但无可识别文件系统 */
int fs_init(void);

/* 是否已挂载 */
int fs_ready(void);

/* 类型名："exFAT" / "FAT12" / "FAT16" / "FAT32" / "none" */
const char *fs_type_name(void);

/* 挂载后卷信息（未挂载时 type==FS_NONE） */
const fs_info_t *fs_get_info(void);

/* 切换首选数据盘（0-3；下次 fs_init 重新扫描挂载） */
void fs_set_drive(uint8_t drive);

/* 将首选数据盘格式化为 exFAT（拒绝格式化 0 号引导盘） */
int fs_format(void);

/* ---- 以下 API 语义与原 exfat_* 完全一致 ---- */
int      fs_read_file(const char *name, uint8_t *buffer, uint32_t max_size);
uint32_t fs_get_file_size(const char *name);
int      fs_create_file(const char *name, const uint8_t *data, uint32_t size);
int      fs_delete_file(const char *name);
int      fs_list_root(void);
int      fs_read_dir(fs_dir_entry_t *entries, int max_entries);
int      fs_change_dir(const char *name);
int      fs_mkdir(const char *name);
const char *fs_cwd_path(void);
uint32_t fs_count_used_clusters(void);
uint32_t fs_get_file_clusters(const char *name);

#endif
