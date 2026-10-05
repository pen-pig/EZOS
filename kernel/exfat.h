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
/* 校验和/文件名哈希的 **C 参考实现**：`rstest` 的对拍基线，也是
 * EZ_EXFAT_IMPL=0 时的回退目标。生产路径走 rust_bridge.h 选中的实现
 * （默认 Rust），不要把这两套混着调。 */
uint32_t exfat_checksum_c(const uint8_t *data, int len);
uint16_t exfat_name_hash_c(const uint16_t *name, int name_len);

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
void exfat_reset_cwd(void);
int exfat_change_dir(const char *name);
int exfat_mkdir(const char *name);
int exfat_rmdir(const char *name);   /* 仅删目录；目标是文件或非空则失败 */

/* 目录项结构（供 desktop 文件管理器使用） */
typedef struct {
    char name[256];
    uint32_t size;
    uint8_t is_dir;
} exfat_dir_entry_t;

int exfat_read_dir(exfat_dir_entry_t *entries, int max_entries);
/* 列指定路径的目录（支持 "SD"、"SD/sub"、"/SD"）；不存在或不是目录 -> -1 */
int exfat_read_dir_path(const char *path, exfat_dir_entry_t *entries, int max_entries);

#endif
