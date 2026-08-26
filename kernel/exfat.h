#ifndef EXFAT_H
#define EXFAT_H

#include "types.h"

typedef struct {
    uint8_t  jump[3];
    char     oem_name[8];
    uint8_t  zero[53];
    uint64_t partition_offset;
    uint64_t volume_length;
    uint32_t fat_offset;
    uint32_t fat_length;
    uint32_t cluster_heap_offset;
    uint32_t cluster_count;
    uint32_t root_dir_cluster;
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
} exfat_info_t;

int exfat_init(void);
int exfat_format(void);
const exfat_info_t *exfat_get_info(void);
int exfat_list_root(void);
int exfat_read_file(const char *name, uint8_t *buffer, uint32_t max_size);
uint32_t exfat_get_file_size(const char *name);
uint32_t exfat_count_used_clusters(void);
uint32_t exfat_get_file_clusters(const char *name);
int exfat_create_file(const char *name, const uint8_t *data, uint32_t size);
int exfat_delete_file(const char *name);
void exfat_set_drive(uint8_t drive);

/* 目录属性位（0x85 目录项 entry[1]） */
#define EXFAT_ATTR_READONLY 0x01
#define EXFAT_ATTR_DIRECTORY 0x10

/* 目录支持 */
uint32_t exfat_cwd_cluster(void);
const char *exfat_cwd_path(void);
int exfat_change_dir(const char *name);
int exfat_mkdir(const char *name);

/* 目录项结构（供 desktop 文件管理器使用） */
typedef struct {
    char name[256];
    uint32_t size;
    uint8_t is_dir;
} exfat_dir_entry_t;

int exfat_read_dir(exfat_dir_entry_t *entries, int max_entries);

#endif
