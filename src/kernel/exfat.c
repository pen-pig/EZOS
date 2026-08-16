#include "exfat.h"
#include "ata.h"
#include "tty.h"
#include "port.h"

static exfat_info_t exfat_info;
static int exfat_ready = 0;
static uint32_t exfat_partition_start = 0;

static uint8_t exfat_drive = 1;   // 默认从盘，可修改

static uint32_t current_dir_cluster = 0;   // 当前工作目录簇，0 表示尚未初始化
static char cwd_path[256] = "/";           // 当前工作目录路径字符串

static int my_strcmp(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a - *b;
}
void exfat_set_drive(uint8_t drive) {
    exfat_drive = drive;
}
static int exfat_read_sector(uint32_t sector, uint8_t *buffer) {
    return ata_read_sector(exfat_drive, sector, buffer);
}

static int exfat_write_sector(uint32_t sector, const uint8_t *buffer) {
    return ata_write_sector(exfat_drive, sector, buffer);
}

static int exfat_read_cluster(uint32_t cluster, uint8_t *buffer) {
    uint32_t first_sector = exfat_info.cluster_heap_offset +
                            (cluster - 2) * exfat_info.sectors_per_cluster;
    for (int i = 0; i < exfat_info.sectors_per_cluster; i++) {
        if (exfat_read_sector(first_sector + i, buffer + i * exfat_info.bytes_per_sector) != 0)
            return -1;
    }
    return 0;
}

static int exfat_write_cluster(uint32_t cluster, const uint8_t *buffer) {
    uint32_t first_sector = exfat_info.cluster_heap_offset +
                            (cluster - 2) * exfat_info.sectors_per_cluster;
    for (int i = 0; i < exfat_info.sectors_per_cluster; i++) {
        if (exfat_write_sector(first_sector + i, buffer + i * exfat_info.bytes_per_sector) != 0)
            return -1;
    }
    return 0;
}

static uint32_t exfat_read_fat_entry(uint32_t cluster) {
    uint32_t fat_sector = exfat_info.fat_offset + (cluster / 128);
    uint32_t fat_offset = (cluster % 128) * 4;
    uint8_t sector[512];
    if (exfat_read_sector(fat_sector, sector) != 0) return 0xFFFFFFFF;
    return *((uint32_t*)(sector + fat_offset));
}

static int exfat_write_fat_entry(uint32_t cluster, uint32_t value) {
    uint32_t fat_sector = exfat_info.fat_offset + (cluster / 128);
    uint32_t fat_offset = (cluster % 128) * 4;
    uint8_t sector[512];
    if (exfat_read_sector(fat_sector, sector) != 0) return -1;
    *((uint32_t*)(sector + fat_offset)) = value;
    if (exfat_write_sector(fat_sector, sector) != 0) return -1;
    return 0;
}

// 扫描 FAT 找第一个空闲簇（FAT entry == 0），天然支持删除后的回收
static uint32_t exfat_find_free_cluster(void) {
    for (uint32_t c = 2; c < exfat_info.cluster_count + 2; c++) {
        if (exfat_read_fat_entry(c) == 0) {
            return c;
        }
    }
    return 0;  // 无空闲簇
}

/* ============ 目录支持（子目录） ============ */

// 返回当前工作目录簇（首次调用时初始化为根目录簇）
uint32_t exfat_cwd_cluster(void) {
    if (!exfat_ready || current_dir_cluster == 0) {
        current_dir_cluster = exfat_info.root_dir_cluster;
        cwd_path[0] = '/';
        cwd_path[1] = '\0';
    }
    return current_dir_cluster;
}

// 返回当前工作目录路径字符串
const char *exfat_cwd_path(void) {
    return cwd_path;
}

// 在指定目录簇中查找名字匹配的目录项，输出完整 32 字节，返回偏移；未找到返回 -1
static int exfat_find_entry(uint32_t dir_cluster, const char *name, uint8_t *out_entry) {
    static uint8_t dir_data[512 * 16];
    uint8_t *buffer = dir_data;
    if (exfat_read_cluster(dir_cluster, buffer) != 0) return -1;

    uint32_t cluster_size = exfat_info.bytes_per_sector * exfat_info.sectors_per_cluster;
    for (uint32_t off = 0; off < cluster_size; off += 32) {
        uint8_t *entry = buffer + off;
        if (entry[0] == 0x00) break;
        if (entry[0] == 0x85) {
            char entry_name[256];
            int name_len = 0;
            for (int i = 0; i < entry[2]; i++) {
                uint16_t ch = *((uint16_t*)(entry + 4 + i * 2));
                if (ch < 128) entry_name[name_len++] = (char)ch;
            }
            entry_name[name_len] = '\0';
            if (my_strcmp(entry_name, name) == 0) {
                if (out_entry) {
                    for (int i = 0; i < 32; i++) out_entry[i] = entry[i];
                }
                return (int)off;
            }
        }
    }
    return -1;
}

// 切换当前工作目录
int exfat_change_dir(const char *name) {
    if (!exfat_ready) return -1;
    if (my_strcmp(name, ".") == 0) return 0;
    if (my_strcmp(name, "..") == 0) {
        // 扁平目录项未存父指针，简化：上级即根目录
        current_dir_cluster = exfat_info.root_dir_cluster;
        cwd_path[0] = '/';
        cwd_path[1] = '\0';
        return 0;
    }
    uint8_t entry[32];
    if (exfat_find_entry(exfat_cwd_cluster(), name, entry) < 0) return -1;
    if ((entry[1] & EXFAT_ATTR_DIRECTORY) == 0) return -1;   // 不是目录
    current_dir_cluster = *((uint32_t*)(entry + 20));

    // 更新路径字符串
    int plen = 0;
    while (cwd_path[plen]) plen++;
    if (plen > 1) cwd_path[plen++] = '/';
    int nlen = 0;
    while (name[nlen]) nlen++;
    for (int i = 0; i < nlen; i++) cwd_path[plen++] = name[i];
    cwd_path[plen] = '\0';
    return 0;
}

// 创建子目录（单簇）
int exfat_mkdir(const char *name) {
    if (!exfat_ready) return -1;

    uint32_t new_cluster = exfat_find_free_cluster();
    if (new_cluster == 0) return -1;
    exfat_write_fat_entry(new_cluster, 0xFFFFFFFF);   // 目录仅单簇，标记链尾

    uint32_t cluster_size = exfat_info.bytes_per_sector * exfat_info.sectors_per_cluster;
    static uint8_t sub_dir[512 * 16];
    for (uint32_t i = 0; i < cluster_size; i++) sub_dir[i] = 0;
    if (exfat_write_cluster(new_cluster, sub_dir) != 0) return -1;

    uint32_t parent_cluster = exfat_cwd_cluster();
    static uint8_t dir_data[512 * 16];
    uint8_t *dbuf = dir_data;
    if (exfat_read_cluster(parent_cluster, dbuf) != 0) return -1;

    int free_off = -1;
    for (uint32_t off = 0; off < cluster_size; off += 32) {
        if (dbuf[off] == 0x00 || dbuf[off] == 0x05) {
            free_off = (int)off;
            break;
        }
    }
    if (free_off == -1) return -1;

    uint8_t dir_entry[32];
    for (int i = 0; i < 32; i++) dir_entry[i] = 0;
    dir_entry[0] = 0x85;
    dir_entry[1] = EXFAT_ATTR_DIRECTORY;
    int name_len = 0;
    while (name[name_len] != '\0') name_len++;
    dir_entry[2] = (uint8_t)name_len;
    for (int i = 0; i < name_len; i++) {
        *((uint16_t*)(dir_entry + 4 + i * 2)) = (uint16_t)name[i];
    }
    *((uint32_t*)(dir_entry + 20)) = new_cluster;
    *((uint32_t*)(dir_entry + 24)) = 0;   // 目录大小视为 0

    for (int i = 0; i < 32; i++) {
        dbuf[free_off + i] = dir_entry[i];
    }
    if (exfat_write_cluster(parent_cluster, dbuf) != 0) return -1;

    return 0;
}

int exfat_format(void) {
    exfat_info.bytes_per_sector = 512;
    exfat_info.sectors_per_cluster = 1;
    exfat_info.fat_offset = 1;
    exfat_info.fat_length = 1;
    exfat_info.cluster_heap_offset = 2;
    exfat_info.cluster_count = 100;
    exfat_info.root_dir_cluster = 2;
    exfat_info.volume_length = 1024;
    exfat_partition_start = 0;

    // 1. 写入 VBR
    uint8_t vbr[512];
    for (int i = 0; i < 512; i++) vbr[i] = 0;
    vbr[0] = 0xEB; vbr[1] = 0x76; vbr[2] = 0x90;
    vbr[3] = 'E'; vbr[4] = 'X'; vbr[5] = 'F'; vbr[6] = 'A'; vbr[7] = 'T'; vbr[8] = ' '; vbr[9] = ' '; vbr[10] = ' ';
    *((uint16_t*)(vbr + 0x0B)) = exfat_info.bytes_per_sector;
    vbr[0x0D] = exfat_info.sectors_per_cluster;
    *((uint32_t*)(vbr + 0x18)) = exfat_info.fat_offset;
    *((uint32_t*)(vbr + 0x1C)) = exfat_info.fat_length;
    *((uint32_t*)(vbr + 0x20)) = exfat_info.cluster_heap_offset;
    *((uint32_t*)(vbr + 0x24)) = exfat_info.cluster_count;
    *((uint32_t*)(vbr + 0x2C)) = exfat_info.root_dir_cluster;
    *((uint64_t*)(vbr + 0x40)) = exfat_info.volume_length;
    vbr[510] = 0x55;
    vbr[511] = 0xAA;
    if (exfat_write_sector(0, vbr) != 0) return -1;

    // 2. 写入 FAT 表（全部初始化为空闲，仅根目录簇2标记为链结束）
    uint8_t fat_sector[512];
    for (int i = 0; i < 512; i++) fat_sector[i] = 0;
    *((uint32_t*)(fat_sector + 2 * 4)) = 0xFFFFFFFF;  // 根目录簇结束
    if (exfat_write_sector(exfat_info.fat_offset, fat_sector) != 0) return -1;

    // 3. 初始化根目录
    uint8_t root_cluster[512];
    for (int i = 0; i < 512; i++) root_cluster[i] = 0;
    if (exfat_write_cluster(exfat_info.root_dir_cluster, root_cluster) != 0) return -1;

    exfat_ready = 1;
    return 0;
}

int exfat_init(void) {
    uint8_t mbr[512];

    if (exfat_read_sector(0, mbr) != 0) {
        return -1;
    }

    int found_vbr = 0;
    if (mbr[510] == 0x55 && mbr[511] == 0xAA) {
        for (int i = 0; i < 4; i++) {
            uint8_t *entry = mbr + 446 + i * 16;
            if (entry[4] != 0) {
                uint32_t part_start = *((uint32_t*)(entry + 8));
                uint8_t vbr[512];
                if (exfat_read_sector(part_start, vbr) == 0 &&
                    vbr[0] == 0xEB && vbr[1] == 0x76 && vbr[2] == 0x90) {
                    exfat_partition_start = part_start;
                    found_vbr = 1;
                    break;
                }
            }
        }
    }

    if (!found_vbr) {
        uint8_t vbr[512];
        if (exfat_read_sector(0, vbr) == 0 &&
            vbr[0] == 0xEB && vbr[1] == 0x76 && vbr[2] == 0x90) {
            exfat_partition_start = 0;
            found_vbr = 1;
        }
    }

    if (!found_vbr) {
        return -2;  // 需要格式化
    }

    uint8_t vbr[512];
    if (exfat_read_sector(exfat_partition_start, vbr) != 0) return -1;
    if (vbr[0] != 0xEB || vbr[1] != 0x76 || vbr[2] != 0x90) return -1;

    exfat_info.bytes_per_sector = *((uint16_t*)(vbr + 0x0B));
    exfat_info.sectors_per_cluster = vbr[0x0D];
    exfat_info.fat_offset = *((uint32_t*)(vbr + 0x18));
    exfat_info.fat_length = *((uint32_t*)(vbr + 0x1C));
    exfat_info.cluster_heap_offset = *((uint32_t*)(vbr + 0x20));
    exfat_info.cluster_count = *((uint32_t*)(vbr + 0x24));
    exfat_info.root_dir_cluster = *((uint32_t*)(vbr + 0x2C));
    exfat_info.volume_length = *((uint64_t*)(vbr + 0x40));
    exfat_info.partition_offset = exfat_partition_start;

    if (exfat_info.bytes_per_sector != 512) return -1;
    if (exfat_info.sectors_per_cluster == 0) return -1;

    exfat_ready = 1;
    return 0;
}

int exfat_list_root(void) {
    if (!exfat_ready) return -1;

    static uint8_t root_cluster_data[512 * 16];
    uint8_t *buffer = root_cluster_data;

    uint32_t cluster = exfat_cwd_cluster();
    if (exfat_read_cluster(cluster, buffer) != 0) return -1;

    uint32_t offset = 0;
    uint32_t cluster_size = exfat_info.bytes_per_sector * exfat_info.sectors_per_cluster;
    while (offset < cluster_size) {
        uint8_t *entry = buffer + offset;
        if (entry[0] == 0x00) break;
        if (entry[0] == 0x85) {
            uint8_t is_dir = (entry[1] & 0x10) ? 1 : 0;
            char name[256];
            int name_len = 0;
			for (int i = 0; i < entry[2]; i++) {
			    uint16_t ch = *((uint16_t*)(entry + 4 + i * 2));
			    if (ch >= 32 && ch <= 126) {
			        name[name_len++] = (char)ch;
			    } else {
			        name[name_len++] = '.';   // 用 '.' 替代无法显示的字符
			    }
			}
            name[name_len] = '\0';
            terminal_writestring(name);
            if (is_dir) terminal_writestring("/");
            terminal_putchar('\n');
        }
        offset += 32;
    }
    return 0;
}

uint32_t exfat_get_file_size(const char *name) {
    if (!exfat_ready) return 0;

    static uint8_t root_cluster_data[512 * 16];
    uint8_t *buffer = root_cluster_data;
    uint32_t cluster = exfat_cwd_cluster();
    if (exfat_read_cluster(cluster, buffer) != 0) return 0;

    uint32_t offset = 0;
    uint32_t cluster_size = exfat_info.bytes_per_sector * exfat_info.sectors_per_cluster;
    while (offset < cluster_size) {
        uint8_t *entry = buffer + offset;
        if (entry[0] == 0x00) break;
        if (entry[0] == 0x85) {
            char entry_name[256];
            int name_len = 0;
            for (int i = 0; i < entry[2]; i++) {
                uint16_t ch = *((uint16_t*)(entry + 4 + i * 2));
                if (ch < 128) entry_name[name_len++] = (char)ch;
            }
            entry_name[name_len] = '\0';
            if (my_strcmp(entry_name, name) == 0) {
                return *((uint32_t*)(entry + 24));
            }
        }
        offset += 32;
    }
    return 0;
}

int exfat_read_file(const char *name, uint8_t *buffer, uint32_t max_size) {
    if (!exfat_ready) return -1;

    static uint8_t root_cluster_data[512 * 16];
    uint8_t *root_buffer = root_cluster_data;
    uint32_t cluster = exfat_cwd_cluster();
    if (exfat_read_cluster(cluster, root_buffer) != 0) return -1;

    uint32_t offset = 0;
    uint32_t cluster_size = exfat_info.bytes_per_sector * exfat_info.sectors_per_cluster;
    uint32_t file_start_cluster = 0;
    uint32_t file_size = 0;

    while (offset < cluster_size) {
        uint8_t *entry = root_buffer + offset;
        if (entry[0] == 0x00) break;
        if (entry[0] == 0x85) {
            char entry_name[256];
            int name_len = 0;
            for (int i = 0; i < entry[2]; i++) {
                uint16_t ch = *((uint16_t*)(entry + 4 + i * 2));
                if (ch < 128) entry_name[name_len++] = (char)ch;
            }
            entry_name[name_len] = '\0';
            if (my_strcmp(entry_name, name) == 0) {
                file_start_cluster = *((uint32_t*)(entry + 20));
                file_size = *((uint32_t*)(entry + 24));
                break;
            }
        }
        offset += 32;
    }
    if (file_start_cluster == 0) return -1;

    uint32_t bytes_read = 0;
    uint32_t current_cluster = file_start_cluster;
    while (bytes_read < file_size && bytes_read < max_size) {
        static uint8_t cluster_data[512 * 16];
        if (exfat_read_cluster(current_cluster, cluster_data) != 0) return -1;
        uint32_t to_copy = file_size - bytes_read;
        if (to_copy > cluster_size) to_copy = cluster_size;
        if (to_copy > max_size - bytes_read) to_copy = max_size - bytes_read;
        for (uint32_t i = 0; i < to_copy; i++) {
            buffer[bytes_read + i] = cluster_data[i];
        }
        bytes_read += to_copy;
        if (bytes_read >= file_size || bytes_read >= max_size) break;
        current_cluster = exfat_read_fat_entry(current_cluster);
        if (current_cluster >= 0xFFFFFFF8) break;  // 链结束或坏簇
    }
    return (int)bytes_read;
}

int exfat_create_file(const char *name, const uint8_t *data, uint32_t size) {
    if (!exfat_ready) return -1;

    static uint8_t root_cluster_data[512 * 16];
    uint8_t *root_buffer = root_cluster_data;
    uint32_t root_cluster = exfat_cwd_cluster();
    if (exfat_read_cluster(root_cluster, root_buffer) != 0) return -1;

    int free_entry_offset = -1;
    uint32_t cluster_size = exfat_info.bytes_per_sector * exfat_info.sectors_per_cluster;
    for (uint32_t off = 0; off < cluster_size; off += 32) {
        uint8_t *entry = root_buffer + off;
        if (entry[0] == 0x00 || entry[0] == 0x05) {
            free_entry_offset = (int)off;
            break;
        }
    }
    if (free_entry_offset == -1) return -1;

    uint32_t data_clusters = (size + cluster_size - 1) / cluster_size;
    if (data_clusters == 0) data_clusters = 1;

    // 逐个分配空闲簇并串成 FAT 链（碎片化安全，不依赖连续簇）
    uint32_t first_cluster = 0;
    uint32_t prev_cluster = 0;
    for (uint32_t i = 0; i < data_clusters; i++) {
        uint32_t cl = exfat_find_free_cluster();
        if (cl == 0) {
            // 分配失败，回滚已占用的簇
            uint32_t cur = first_cluster;
            while (cur >= 2 && cur < exfat_info.cluster_count + 2) {
                uint32_t next = exfat_read_fat_entry(cur);
                exfat_write_fat_entry(cur, 0);
                if (next >= 0xFFFFFFF8) break;
                cur = next;
            }
            return -1;
        }
        exfat_write_fat_entry(cl, 0xFFFFFFFF);  // 先标记为链尾
        if (first_cluster == 0) first_cluster = cl;
        else exfat_write_fat_entry(prev_cluster, cl);
        prev_cluster = cl;
    }
    uint32_t start_cluster = first_cluster;

    uint32_t bytes_written = 0;
    uint32_t cur_cluster = start_cluster;
    for (uint32_t i = 0; i < data_clusters; i++) {
        uint8_t cluster_buf[512 * 16];
        for (uint32_t j = 0; j < cluster_size; j++) cluster_buf[j] = 0;
        uint32_t to_copy = size - bytes_written;
        if (to_copy > cluster_size) to_copy = cluster_size;
        for (uint32_t j = 0; j < to_copy; j++) {
            cluster_buf[j] = data[bytes_written + j];
        }
        if (exfat_write_cluster(cur_cluster, cluster_buf) != 0) return -1;
        bytes_written += to_copy;
        if (i < data_clusters - 1) {
            cur_cluster = exfat_read_fat_entry(cur_cluster);
        }
    }

    uint8_t dir_entry[32];
    for (int i = 0; i < 32; i++) dir_entry[i] = 0;
    dir_entry[0] = 0x85;
    dir_entry[1] = 0x00;
    int name_len = 0;
    while (name[name_len] != '\0') name_len++;
    dir_entry[2] = (uint8_t)name_len;
    for (int i = 0; i < name_len; i++) {
        *((uint16_t*)(dir_entry + 4 + i * 2)) = (uint16_t)name[i];
    }
    *((uint32_t*)(dir_entry + 20)) = start_cluster;
    *((uint32_t*)(dir_entry + 24)) = size;

    for (int i = 0; i < 32; i++) {
        root_buffer[free_entry_offset + i] = dir_entry[i];
    }
    if ((uint32_t)(free_entry_offset + 32) < cluster_size) {
        root_buffer[free_entry_offset + 32] = 0x00;
    }
    if (exfat_write_cluster(root_cluster, root_buffer) != 0) return -1;

    return 0;
}

int exfat_delete_file(const char *name) {
    if (!exfat_ready) return -1;

    static uint8_t root_cluster_data[512 * 16];
    uint8_t *root_buffer = root_cluster_data;
    uint32_t root_cluster = exfat_cwd_cluster();
    if (exfat_read_cluster(root_cluster, root_buffer) != 0) return -1;

    uint32_t cluster_size = exfat_info.bytes_per_sector * exfat_info.sectors_per_cluster;
    for (uint32_t off = 0; off < cluster_size; off += 32) {
        uint8_t *entry = root_buffer + off;
        if (entry[0] == 0x00) break;
        if (entry[0] == 0x85) {
            char entry_name[256];
            int name_len = 0;
            for (int i = 0; i < entry[2]; i++) {
                uint16_t ch = *((uint16_t*)(entry + 4 + i * 2));
                if (ch < 128) entry_name[name_len++] = (char)ch;
            }
            entry_name[name_len] = '\0';
            if (my_strcmp(entry_name, name) == 0) {
                // 释放该文件占用的 FAT 链
                uint32_t cl = *((uint32_t*)(entry + 20));
                while (cl >= 2 && cl < exfat_info.cluster_count + 2) {
                    uint32_t next = exfat_read_fat_entry(cl);
                    exfat_write_fat_entry(cl, 0);   // 标记为空闲
                    if (next >= 0xFFFFFFF8) break;
                    cl = next;
                }
                entry[0] = 0x05;
                if (exfat_write_cluster(root_cluster, root_buffer) != 0) return -1;
                return 0;
            }
        }
    }
    return -1;
}
