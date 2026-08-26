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
static uint32_t dir_stack[32];             // 目录栈（父目录簇），dir_stack[0] 恒为根
static int dir_depth = 0;                  // 当前目录深度（根=0）

static int my_strcmp(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a - *b;
}

// 合并结构字段偏移（文件名 UTF-16 从 out[4] 起，最多 255 字符，占 510 字节）
#define EXFAT_MERGED_CLUSTER_OFF 256
#define EXFAT_MERGED_SIZE_OFF    260
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
    uint32_t first_sector = exfat_partition_start + exfat_info.cluster_heap_offset +
                            (cluster - 2) * exfat_info.sectors_per_cluster;
    for (int i = 0; i < exfat_info.sectors_per_cluster; i++) {
        if (exfat_read_sector(first_sector + i, buffer + i * exfat_info.bytes_per_sector) != 0)
            return -1;
    }
    return 0;
}

static int exfat_write_cluster(uint32_t cluster, const uint8_t *buffer) {
    uint32_t first_sector = exfat_partition_start + exfat_info.cluster_heap_offset +
                            (cluster - 2) * exfat_info.sectors_per_cluster;
    for (int i = 0; i < exfat_info.sectors_per_cluster; i++) {
        if (exfat_write_sector(first_sector + i, buffer + i * exfat_info.bytes_per_sector) != 0)
            return -1;
    }
    return 0;
}

static uint32_t exfat_read_fat_entry(uint32_t cluster) {
    uint32_t fat_sector = exfat_partition_start + exfat_info.fat_offset + (cluster / 128);
    uint32_t fat_offset = (cluster % 128) * 4;
    uint8_t sector[512];
    if (exfat_read_sector(fat_sector, sector) != 0) return 0xFFFFFFFF;
    return *((uint32_t*)(sector + fat_offset));
}

static int exfat_write_fat_entry(uint32_t cluster, uint32_t value) {
    uint32_t fat_sector = exfat_partition_start + exfat_info.fat_offset + (cluster / 128);
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

// 同步 Allocation Bitmap（簇3）：cluster 对应 bit 置 used(1)/空闲(0)
// 位映射：簇2 → bit0，簇3 → bit1 ...（簇号减 2）
static int exfat_bitmap_set(uint32_t cluster, int used) {
    if (cluster < 2) return -1;
    uint8_t bmp[512 * 16];
    uint32_t bmp_cluster = 3;   // Allocation Bitmap 固定在簇3
    if (exfat_read_cluster(bmp_cluster, bmp) != 0) return -1;
    uint32_t bit = cluster - 2;
    uint32_t byte_idx = bit / 8;
    uint8_t mask = (uint8_t)(1 << (bit % 8));
    if (byte_idx >= exfat_info.bytes_per_sector * exfat_info.sectors_per_cluster) return -1;
    if (used) bmp[byte_idx] |= mask;
    else bmp[byte_idx] &= (uint8_t)~mask;
    return exfat_write_cluster(bmp_cluster, bmp);
}

/* ============ 标准 exFAT 目录项（entry set）支持 ============ */

// 计算 entry set 校验和（跳过 0x85 条目的 SetChecksum 字段 offset 2-3）
static uint16_t exfat_set_checksum(const uint8_t *entries, int total_bytes) {
    uint16_t sum = 0;
    for (int i = 0; i < total_bytes; i++) {
        if (i == 2 || i == 3) continue;
        sum = (uint16_t)(((sum << 15) | (sum >> 1)) + entries[i]);
    }
    return sum;
}

// 计算文件名哈希（UTF-16 字符）
static uint16_t exfat_name_hash(const uint16_t *name, int name_len) {
    uint16_t hash = 0;
    for (int i = 0; i < name_len; i++) {
        hash = (uint16_t)(((hash << 15) | (hash >> 1)) + name[i]);
    }
    return hash;
}

// 写 FAT 风格时间戳（固定 2026-01-01 00:00:00）
static void exfat_write_timestamp(uint8_t *dst) {
    *((uint16_t*)(dst)) = 0x5C21;    // 日期: (2026-1980)<<9 | 1<<5 | 1
    *((uint16_t*)(dst + 2)) = 0x0000; // 时间: 00:00:00
}

// 计算 entry set 需要的条目数（0x85 + 0xC0 + ceil(name_len/15) 个 0xC1）
static int exfat_entry_set_count(int name_len) {
    return 2 + (name_len + 14) / 15;
}

// 在目录缓冲区中找连续空闲条目区（空闲=0x00 或已删除=最高位为0）
static int exfat_find_free_set(uint8_t *dir_buf, uint32_t cluster_size, int need_entries) {
    int run = 0;
    for (uint32_t off = 0; off < cluster_size; off += 32) {
        uint8_t *e = dir_buf + off;
        if (e[0] == 0x00 || (e[0] & 0x80) == 0) {
            run++;
            if (run == need_entries) {
                return (int)(off - (uint32_t)(need_entries - 1) * 32);
            }
        } else {
            run = 0;
        }
    }
    return -1;
}

// 读取目录链全部簇到 buffer，返回目录链占用的簇数（沿 FAT 链遍历）
static uint32_t exfat_read_dir_chain(uint32_t dir_cluster, uint8_t *buffer, uint32_t buf_clusters) {
    uint32_t cluster_size = exfat_info.bytes_per_sector * exfat_info.sectors_per_cluster;
    uint32_t n = 0, cur = dir_cluster;
    while (cur >= 2 && cur < exfat_info.cluster_count + 2 && n < buf_clusters) {
        if (exfat_read_cluster(cur, buffer + n * cluster_size) != 0) return n;
        n++;
        cur = exfat_read_fat_entry(cur);
        if (cur >= 0xFFFFFFF8) break;
    }
    return n;
}

// 为目录链追加一个新簇并清空，返回新簇号；失败返回 0
static uint32_t exfat_extend_dir(uint32_t dir_cluster) {
    uint32_t new_cl = exfat_find_free_cluster();
    if (new_cl == 0) return 0;
    if (exfat_write_fat_entry(new_cl, 0xFFFFFFFF) != 0) return 0;
    // 找到链尾并接上新簇
    uint32_t cur = dir_cluster;
    uint32_t next = exfat_read_fat_entry(cur);
    while (next >= 2 && next < exfat_info.cluster_count + 2) {
        cur = next;
        next = exfat_read_fat_entry(cur);
    }
    if (exfat_write_fat_entry(cur, new_cl) != 0) return 0;
    // 清空新簇
    uint32_t cluster_size = exfat_info.bytes_per_sector * exfat_info.sectors_per_cluster;
    static uint8_t zero[512 * 16];
    for (uint32_t i = 0; i < cluster_size; i++) zero[i] = 0;
    if (exfat_write_cluster(new_cl, zero) != 0) return 0;
    exfat_bitmap_set(new_cl, 1);
    return new_cl;
}

// 将文件/目录的 entry set 写入 dir_buf 的 free_off 处
static void exfat_write_entry_set(uint8_t *dir_buf, int free_off,
                                  const char *name, uint32_t first_cluster,
                                  uint32_t size, uint8_t is_dir) {
    int name_len = 0;
    while (name[name_len]) name_len++;
    int name_entries = (name_len + 14) / 15;
    int secondary_count = 1 + name_entries;

    // 0x85 File entry
    uint8_t *e85 = dir_buf + free_off;
    for (int i = 0; i < 32; i++) e85[i] = 0;
    e85[0] = 0x85;
    e85[1] = (uint8_t)secondary_count;
    *((uint16_t*)(e85 + 4)) = is_dir ? 0x10 : 0x20;  // FileAttributes
    exfat_write_timestamp(e85 + 8);    // CreateTimestamp
    exfat_write_timestamp(e85 + 12);   // LastModifiedTimestamp
    exfat_write_timestamp(e85 + 16);   // LastAccessedTimestamp

    // 0xC0 Stream extension
    uint8_t *ec0 = dir_buf + free_off + 32;
    for (int i = 0; i < 32; i++) ec0[i] = 0;
    ec0[0] = 0xC0;
    ec0[1] = 0x02;   // GeneralSecondaryFlags: NoFatChain（连续存储）
    ec0[3] = (uint8_t)name_len;   // NameLength
    uint16_t name_utf16[256];
    for (int i = 0; i < name_len; i++) name_utf16[i] = (uint16_t)(uint8_t)name[i];
    *((uint16_t*)(ec0 + 4)) = exfat_name_hash(name_utf16, name_len);  // NameHash
    *((uint64_t*)(ec0 + 8)) = size;    // ValidDataLength
    *((uint32_t*)(ec0 + 0x14)) = first_cluster;  // FirstCluster
    *((uint64_t*)(ec0 + 0x18)) = size;  // DataLength

    // 0xC1 File name entries
    for (int n = 0; n < name_entries; n++) {
        uint8_t *ec1 = dir_buf + free_off + 64 + n * 32;
        for (int i = 0; i < 32; i++) ec1[i] = 0;
        ec1[0] = 0xC1;
        for (int i = 0; i < 15; i++) {
            int idx = n * 15 + i;
            if (idx < name_len) {
                *((uint16_t*)(ec1 + 2 + i * 2)) = (uint16_t)(uint8_t)name[idx];
            } else {
                *((uint16_t*)(ec1 + 2 + i * 2)) = 0x0000;
            }
        }
    }

    // 计算并写回 SetChecksum
    int total_bytes = (2 + name_entries) * 32;
    uint16_t chk = exfat_set_checksum(dir_buf + free_off, total_bytes);
    *((uint16_t*)(e85 + 2)) = chk;
}

// 解析 entry set，将 0x85 条目合并为兼容结构输出：
//   out[0]=0x85, out[1]=属性低字节, out[2]=name_len, out[4..]=UTF16名,
//   out[256..259]=first_cluster, out[260..263]=size
static int exfat_parse_entry_set(const uint8_t *entry, uint8_t *out) {
    if (entry[0] != 0x85) return -1;
    int sec_count = entry[1];
    if (sec_count < 2) return -1;
    const uint8_t *ec0 = entry + 32;
    int name_len = ec0[3];
    uint32_t first_cluster = *((uint32_t*)(ec0 + 0x14));
    uint64_t data_len = *((uint64_t*)(ec0 + 0x18));

    for (int i = 0; i < 32; i++) out[i] = 0;
    out[0] = 0x85;
    out[1] = entry[4];   // 属性低字节（含目录位 0x10）
    out[2] = (uint8_t)name_len;
    int nlen = 0;
    int name_entries = sec_count - 1;
    for (int n = 0; n < name_entries && nlen < 255; n++) {
        const uint8_t *ec1 = entry + 64 + n * 32;
        for (int i = 0; i < 15 && nlen < name_len; i++) {
            uint16_t ch = *((uint16_t*)(ec1 + 2 + i * 2));
            *((uint16_t*)(out + 4 + nlen * 2)) = ch;
            nlen++;
        }
    }
    *((uint32_t*)(out + EXFAT_MERGED_CLUSTER_OFF)) = first_cluster;
    *((uint32_t*)(out + EXFAT_MERGED_SIZE_OFF)) = (uint32_t)data_len;
    return 0;
}

// 在指定目录链中查找名字匹配的目录项，输出合并结构，返回偏移；未找到返回 -1
static int exfat_find_entry(uint32_t dir_cluster, const char *name, uint8_t *out_entry) {
    static uint8_t dir_data[512 * 16];
    uint8_t *buffer = dir_data;
    uint32_t cluster_size = exfat_info.bytes_per_sector * exfat_info.sectors_per_cluster;

    uint32_t cur = dir_cluster;
    while (cur >= 2 && cur < exfat_info.cluster_count + 2) {
        if (exfat_read_cluster(cur, buffer) != 0) return -1;
        for (uint32_t off = 0; off < cluster_size; ) {
            uint8_t *entry = buffer + off;
            if (entry[0] == 0x00) break;
            if (entry[0] == 0x85) {
                int sec_count = entry[1];
                uint8_t merged[512];
                if (exfat_parse_entry_set(entry, merged) == 0) {
                    char entry_name[256];
                    int name_len = merged[2];
                    int nlen = 0;
                    for (int i = 0; i < name_len && i < 255; i++) {
                        uint16_t ch = *((uint16_t*)(merged + 4 + i * 2));
                        if (ch < 128) entry_name[nlen++] = (char)ch;
                        else entry_name[nlen++] = '.';
                    }
                    entry_name[nlen] = '\0';
                    if (my_strcmp(entry_name, name) == 0) {
                        if (out_entry) {
                            for (int i = 0; i < 512; i++) out_entry[i] = merged[i];
                        }
                        return (int)off;
                    }
                }
                off += (1 + sec_count) * 32;
            } else {
                off += 32;
            }
        }
        cur = exfat_read_fat_entry(cur);
        if (cur >= 0xFFFFFFF8) break;
    }
    return -1;
}

/* ============ 目录支持（子目录） ============ */

// 返回当前工作目录簇（首次调用时初始化为根目录簇）
uint32_t exfat_cwd_cluster(void) {
    if (!exfat_ready || current_dir_cluster == 0) {
        current_dir_cluster = exfat_info.root_dir_cluster;
        cwd_path[0] = '/';
        cwd_path[1] = '\0';
        dir_stack[0] = exfat_info.root_dir_cluster;
        dir_depth = 0;
    }
    return current_dir_cluster;
}

// 返回当前工作目录路径字符串
const char *exfat_cwd_path(void) {
    return cwd_path;
}

// 切换当前工作目录。支持：
//   cd ..           返回父目录（目录栈回溯）
//   cd /            回到根目录
//   cd /a/b/c       绝对路径
//   cd a/b/c        相对路径（逐级解析）
int exfat_change_dir(const char *name) {
    if (!exfat_ready) return -1;
    exfat_cwd_cluster();

    if (name[0] == '\0' || my_strcmp(name, ".") == 0) return 0;

    // 绝对路径：从根开始
    uint32_t cur_cluster = exfat_info.root_dir_cluster;
    int depth = 0;
    char pathbuf[256];
    int pb = 0;
    pathbuf[pb++] = '/';

    if (name[0] == '/') {
        // 重建栈：根
        if (my_strcmp(name, "/") == 0) {
            dir_stack[0] = exfat_info.root_dir_cluster;
            dir_depth = 0;
            current_dir_cluster = exfat_info.root_dir_cluster;
            cwd_path[0] = '/';
            cwd_path[1] = '\0';
            return 0;
        }
        const char *p = name + 1;
        char comp[128];
        while (*p) {
            int cl = 0;
            while (*p && *p != '/') comp[cl++] = *p++;
            comp[cl] = '\0';
            if (*p == '/') p++;
            if (cl == 0) continue;
            if (my_strcmp(comp, "..") == 0) {
                // 绝对路径中 .. 表示上一级
                if (depth > 0) {
                    depth--;
                    // 路径回溯（简单截断）
                    while (pb > 1 && pathbuf[pb-1] != '/') pb--;
                    if (pb > 1) pb--;
                    cur_cluster = (depth == 0) ? exfat_info.root_dir_cluster : dir_stack[depth];
                }
                continue;
            }
            if (my_strcmp(comp, ".") == 0) continue;
            // 查找子目录
            uint8_t entry[512];
            if (exfat_find_entry(cur_cluster, comp, entry) < 0) return -1;
            if ((entry[1] & EXFAT_ATTR_DIRECTORY) == 0) return -1;
            cur_cluster = *((uint32_t*)(entry + EXFAT_MERGED_CLUSTER_OFF));
            depth++;
            dir_stack[depth] = cur_cluster;
            if (pb > 1) pathbuf[pb++] = '/';
            for (int i = 0; i < cl; i++) pathbuf[pb++] = comp[i];
        }
        dir_stack[0] = exfat_info.root_dir_cluster;
        dir_depth = depth;
        current_dir_cluster = cur_cluster;
        pathbuf[pb] = '\0';
        for (int i = 0; i <= pb; i++) cwd_path[i] = pathbuf[i];
        return 0;
    }

    // 相对路径（可能含 /）
    const char *p = name;
    uint32_t saved_cwd = current_dir_cluster;
    cur_cluster = current_dir_cluster;
    depth = dir_depth;
    int saved_depth = dir_depth;
    uint32_t saved_stack[32];
    for (int i = 0; i <= dir_depth; i++) saved_stack[i] = dir_stack[i];
    int path_pb = 0;
    char pathbuf2[256];
    for (int i = 0; cwd_path[i]; i++) pathbuf2[path_pb++] = cwd_path[i];
    pathbuf2[path_pb] = '\0';

    while (*p) {
        char comp[128];
        int cl = 0;
        while (*p && *p != '/') comp[cl++] = *p++;
        comp[cl] = '\0';
        if (*p == '/') p++;
        if (cl == 0) continue;
        if (my_strcmp(comp, "..") == 0) {
            if (depth > 0) {
                depth--;
                while (path_pb > 1 && pathbuf2[path_pb-1] != '/') path_pb--;
                if (path_pb > 1) path_pb--;
                pathbuf2[path_pb] = '\0';
                cur_cluster = (depth == 0) ? exfat_info.root_dir_cluster : dir_stack[depth];
            }
            continue;
        }
        if (my_strcmp(comp, ".") == 0) continue;
        uint8_t entry[512];
        if (exfat_find_entry(cur_cluster, comp, entry) < 0) {
            // 回滚目录栈
            dir_depth = saved_depth;
            for (int i = 0; i <= saved_depth; i++) dir_stack[i] = saved_stack[i];
            current_dir_cluster = saved_cwd;
            return -1;
        }
        if ((entry[1] & EXFAT_ATTR_DIRECTORY) == 0) {
            dir_depth = saved_depth;
            for (int i = 0; i <= saved_depth; i++) dir_stack[i] = saved_stack[i];
            current_dir_cluster = saved_cwd;
            return -1;
        }
        cur_cluster = *((uint32_t*)(entry + EXFAT_MERGED_CLUSTER_OFF));
        depth++;
        dir_stack[depth] = cur_cluster;
        if (path_pb > 1) pathbuf2[path_pb++] = '/';
        for (int i = 0; i < cl; i++) pathbuf2[path_pb++] = comp[i];
        pathbuf2[path_pb] = '\0';
    }
    dir_stack[0] = exfat_info.root_dir_cluster;
    dir_depth = depth;
    current_dir_cluster = cur_cluster;
    for (int i = 0; i <= path_pb; i++) cwd_path[i] = pathbuf2[i];
    return 0;
}

// 创建子目录（支持多级路径 /a/b/c，最后一段为目录名，前面各级必须已存在）
int exfat_mkdir(const char *name) {
    if (!exfat_ready) return -1;

    int ok = 0;
    // 保存当前目录状态（mkdir 不应改变调用方 cwd）
    char saved_path[256];
    uint32_t saved_cwd = exfat_cwd_cluster();
    int saved_depth = dir_depth;
    uint32_t saved_stack[32];
    for (int i = 0; i <= dir_depth && i < 32; i++) saved_stack[i] = dir_stack[i];
    {
        int k = 0;
        while (cwd_path[k] && k < 255) { saved_path[k] = cwd_path[k]; k++; }
        saved_path[k] = '\0';
    }

    // 解析路径：最后一段是待创建目录名，前面各级必须已存在
    char path_buf[128];
    int nlen = 0;
    while (name[nlen] && nlen < 127) nlen++;
    for (int i = 0; i < nlen; i++) path_buf[i] = name[i];
    path_buf[nlen] = '\0';

    char dir_name[128];
    int dn = 0;
    int last_sep = -1;
    for (int i = nlen - 1; i >= 0; i--) {
        if (path_buf[i] == '/') { last_sep = i; break; }
    }
    if (last_sep >= 0) {
        // 有路径前缀：先 cd 过去
        char prefix[128];
        int pl = last_sep;
        if (pl == 0) pl = 1;   // 根目录 "/xxx" → "/"
        for (int i = 0; i < pl; i++) prefix[i] = path_buf[i];
        prefix[pl] = '\0';
        if (exfat_change_dir(prefix) != 0) goto restore;
        dn = nlen - last_sep - 1;
        for (int i = 0; i < dn; i++) dir_name[i] = path_buf[last_sep + 1 + i];
        dir_name[dn] = '\0';
    } else {
        for (int i = 0; i < nlen; i++) dir_name[i] = path_buf[i];
        dir_name[nlen] = '\0';
        dn = nlen;
    }
    if (dn == 0) goto restore;

    // 重名检查
    uint8_t exist_entry[512];
    if (exfat_find_entry(exfat_cwd_cluster(), dir_name, exist_entry) >= 0) goto restore;

    uint32_t new_cluster = exfat_find_free_cluster();
    if (new_cluster == 0) goto restore;
    exfat_write_fat_entry(new_cluster, 0xFFFFFFFF);   // 目录仅单簇，标记链尾
    exfat_bitmap_set(new_cluster, 1);

    uint32_t cluster_size = exfat_info.bytes_per_sector * exfat_info.sectors_per_cluster;
    static uint8_t sub_dir[512 * 16];
    for (uint32_t i = 0; i < cluster_size; i++) sub_dir[i] = 0;
    if (exfat_write_cluster(new_cluster, sub_dir) != 0) goto restore;

    uint32_t parent_cluster = exfat_cwd_cluster();
    uint8_t dir_data[512 * 16];
    uint8_t *dbuf = dir_data;
    uint32_t dir_clusters = exfat_read_dir_chain(parent_cluster, dbuf, 16);
    if (dir_clusters == 0) goto restore;

    int name_len = 0;
    while (dir_name[name_len]) name_len++;
    int need = exfat_entry_set_count(name_len);
    int free_off = exfat_find_free_set(dbuf, dir_clusters * cluster_size, need);
    if (free_off == -1) {
        // 目录已满，尝试扩展
        uint32_t new_dir_cl = exfat_extend_dir(parent_cluster);
        if (new_dir_cl == 0) goto restore;
        dir_clusters = exfat_read_dir_chain(parent_cluster, dbuf, 16);
        free_off = exfat_find_free_set(dbuf, dir_clusters * cluster_size, need);
        if (free_off == -1) goto restore;
    }

    exfat_write_entry_set(dbuf, free_off, dir_name, new_cluster, 0, 1);
    if (exfat_write_cluster(parent_cluster, dbuf) != 0) goto restore;
    if (dir_clusters > 1) {
        // 写回目录链其余簇
        uint32_t cur = exfat_read_fat_entry(parent_cluster);
        uint32_t idx = 1;
        while (cur >= 2 && cur < exfat_info.cluster_count + 2 && idx < dir_clusters) {
            if (exfat_write_cluster(cur, dbuf + idx * cluster_size) != 0) goto restore;
            idx++;
            cur = exfat_read_fat_entry(cur);
        }
    }
    ok = 1;

restore:
    // 恢复调用方目录状态
    current_dir_cluster = saved_cwd;
    dir_depth = saved_depth;
    for (int i = 0; i <= saved_depth && i < 32; i++) dir_stack[i] = saved_stack[i];
    {
        int k = 0;
        while (saved_path[k] && k < 255) { cwd_path[k] = saved_path[k]; k++; }
        cwd_path[k] = '\0';
    }
    return ok ? 0 : -1;
}

// exFAT 校验和算法：chk = ((chk << 31) | (chk >> 1)) + byte（uint32 自然溢出）
static uint32_t exfat_checksum(const uint8_t *data, int len) {
    uint32_t chk = 0;
    for (int i = 0; i < len; i++) {
        chk = ((chk << 31) | (chk >> 1)) + data[i];
    }
    return chk;
}

int exfat_format(void) {
    exfat_info.bytes_per_sector = 512;
    exfat_info.sectors_per_cluster = 1;
    exfat_info.fat_offset = 25;         // 备份启动区占 13-24，FAT 必须从 25 开始
    exfat_info.fat_length = 1;
    exfat_info.cluster_heap_offset = 26;  // >= FatOffset + FatLength
    exfat_info.cluster_count = 100;
    exfat_info.root_dir_cluster = 2;
    exfat_info.volume_length = 32767;   // 分区长度 = 16MB/512 - 1（MBR 占 1 扇区）
    exfat_partition_start = 1;

    // 0. 标准 MBR（分区表：1 个 exFAT 分区，从扇区 1 开始）
    uint8_t mbr[512];
    for (int i = 0; i < 512; i++) mbr[i] = 0;
    mbr[446] = 0x00;                    // boot flag
    mbr[447] = 0x00; mbr[448] = 0x02; mbr[449] = 0x00;   // CHS start
    mbr[450] = 0x07;                    // 分区类型: exFAT
    mbr[451] = 0x00; mbr[452] = 0x3F; mbr[453] = 0xFF;   // CHS end
    *((uint32_t*)(mbr + 454)) = 1;      // LBA start = 1
    *((uint32_t*)(mbr + 458)) = 32767;  // 分区扇区数
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
    if (exfat_write_sector(0, mbr) != 0) return -1;

    // 1. 标准 exFAT VBR（分区起始扇区）
    uint8_t vbr[512];
    for (int i = 0; i < 512; i++) vbr[i] = 0;
    vbr[0] = 0xEB; vbr[1] = 0x76; vbr[2] = 0x90;
    vbr[3] = 'E'; vbr[4] = 'X'; vbr[5] = 'F'; vbr[6] = 'A'; vbr[7] = 'T';
    vbr[8] = ' '; vbr[9] = ' '; vbr[10] = ' ';
    *((uint64_t*)(vbr + 0x40)) = exfat_partition_start;          // PartitionOffset
    *((uint64_t*)(vbr + 0x48)) = exfat_info.volume_length;       // VolumeLength
    *((uint32_t*)(vbr + 0x50)) = exfat_info.fat_offset;          // FatOffset
    *((uint32_t*)(vbr + 0x54)) = exfat_info.fat_length;          // FatLength
    *((uint32_t*)(vbr + 0x58)) = exfat_info.cluster_heap_offset; // ClusterHeapOffset
    *((uint32_t*)(vbr + 0x5C)) = exfat_info.cluster_count;       // ClusterCount
    *((uint32_t*)(vbr + 0x60)) = exfat_info.root_dir_cluster;    // RootDirectoryCluster
    *((uint32_t*)(vbr + 0x64)) = 0x12345678;                     // VolumeSerialNumber
    *((uint16_t*)(vbr + 0x68)) = 0x0000;                         // VolumeFlags
    vbr[0x6A] = 0;       // ActiveFat
    vbr[0x6B] = 0;       // Reserved1
    vbr[0x6C] = 0xFF;    // PercentInUse（0xFF = 未知）
    vbr[0x6D] = 0;       // Reserved2
    vbr[0x6E] = 9;       // BytesPerSectorShift（512 = 1<<9）
    vbr[0x6F] = 0;       // SectorsPerClusterShift（1 扇区/簇 = 1<<0）
    // 0x70-0x73 Reserved2 全 0（vbr 已清零）
    vbr[510] = 0x55;
    vbr[511] = 0xAA;
    if (exfat_write_sector(exfat_partition_start, vbr) != 0) return -1;

    // 1.5 Boot Region 校验（sector 11 = Boot Checksum 扇区）
    // VolumeChecksum @0：对 VBR 偏移 0-10 + 90-109（31 字节）计算（exFAT 规范）
    uint8_t boot_region[11 * 512];
    for (int s = 0; s < 11; s++) {
        if (exfat_read_sector(s, boot_region + s * 512) != 0) return -1;   // BootChecksum 覆盖 LBA 0-10（MBR+主启动区）
    }
    boot_region[106] = 0; boot_region[107] = 0;
    uint8_t vchk_buf[31];
    for (int i = 0; i < 11; i++) vchk_buf[i] = vbr[i];
    for (int i = 0; i < 20; i++) vchk_buf[11 + i] = vbr[90 + i];
    uint32_t vchk = exfat_checksum(vchk_buf, 31);
    uint8_t boot_chk_sector[512];
    for (int i = 0; i < 512; i++) boot_chk_sector[i] = 0;
    *((uint32_t*)(boot_chk_sector + 0)) = vchk;        // VolumeChecksum @0
    // BootChecksum @508：仅对前 11 个扇区（sector 0-10 = 5632 字节）计算，
    // 第一个扇区偏移 0x170-0x173 按 0 处理（exFAT 规范 7.2.2）
    {
        uint8_t bcs_buf[11 * 512];
        for (int i = 0; i < 11 * 512; i++) bcs_buf[i] = boot_region[i];
        bcs_buf[0x170] = 0; bcs_buf[0x171] = 0;
        bcs_buf[0x172] = 0; bcs_buf[0x173] = 0;
        uint32_t bchk = exfat_checksum(bcs_buf, 11 * 512);
        *((uint32_t*)(boot_chk_sector + 508)) = bchk;      // BootChecksum @508（规范：508-511 全为校验和，无签名）
    }
    if (exfat_write_sector(exfat_partition_start + 11, boot_chk_sector) != 0) return -1;

    // 1.6 Backup Boot Region（sector 12 = sector 0 副本，sector 23 = sector 11 副本）
    if (exfat_write_sector(exfat_partition_start + 12, vbr) != 0) return -1;
    if (exfat_write_sector(exfat_partition_start + 23, boot_chk_sector) != 0) return -1;

    // 2. FAT 表
    // FAT[0]=0xFFFFFFF8, FAT[1]=0xFFFFFFFF
    // 簇2 根目录、簇3 bitmap、簇4-5 upcase
    uint8_t fat_sector[512];
    for (int i = 0; i < 512; i++) fat_sector[i] = 0;
    *((uint32_t*)(fat_sector + 0)) = 0xFFFFFFF8;
    *((uint32_t*)(fat_sector + 4)) = 0xFFFFFFFF;
    *((uint32_t*)(fat_sector + 8)) = 0xFFFFFFFF;   // 簇2 根目录链尾
    *((uint32_t*)(fat_sector + 12)) = 0xFFFFFFFF;  // 簇3 bitmap 链尾
    *((uint32_t*)(fat_sector + 16)) = 0xFFFFFFFF;  // 簇4 upcase 链尾
    if (exfat_write_sector(exfat_partition_start + exfat_info.fat_offset, fat_sector) != 0) return -1;

    // 3. 根目录簇（簇2）：0x83 label + 0x81 bitmap + 0x82 upcase（对齐 Windows 原生顺序）
    uint8_t root_cluster[512];
    for (int i = 0; i < 512; i++) root_cluster[i] = 0;
    // 0x83 Volume Label（无卷标，SecondaryCount=2）
    root_cluster[0] = 0x83;
    root_cluster[1] = 0x02;   // SecondaryCount = 2（0x81 + 0x82）
    root_cluster[4] = 0x00; root_cluster[5] = 0x00;   // CharacterCount = 0
    // 0x81 Allocation Bitmap（条目内 FirstCluster@20, DataLength@24）
    root_cluster[32] = 0x81;
    root_cluster[33] = 0x00;   // BitmapFlags
    *((uint32_t*)(root_cluster + 32 + 0x14)) = 3;   // FirstCluster
    *((uint64_t*)(root_cluster + 32 + 0x18)) = 13;  // DataLength (100簇/8=12.5→13)
    // 0x82 Up-case Table（条目内 FirstCluster@20, DataLength@24）
    root_cluster[64] = 0x82;
    *((uint32_t*)(root_cluster + 64 + 0x14)) = 4;   // FirstCluster
    *((uint64_t*)(root_cluster + 64 + 0x18)) = 124; // DataLength（压缩 upcase 表：4 校验和 + 12 保留 + 26*4 映射 + 4 终止符）
    // EntrySetChecksum（覆盖 0x83+0x81+0x82 共 96 字节，每个条目的字节 2-3 按 0 处理，16 位算法）
    uint16_t esc = 0;
    for (int i = 0; i < 96; i++) {
        uint8_t eb = (i % 32 == 2 || i % 32 == 3) ? 0 : root_cluster[i]; // 条目字节2-3按0
        // 16位循环右移1位 + 字节（必须避免 int 提升导致高位干扰）
        esc = (uint16_t)((esc >> 1) | (uint16_t)(esc << 15)) + eb;
    }
    root_cluster[2] = (uint8_t)(esc & 0xFF);   // SetChecksum → 0x83 条目字节 2-3
    root_cluster[3] = (uint8_t)((esc >> 8) & 0xFF);
    if (exfat_write_cluster(exfat_info.root_dir_cluster, root_cluster) != 0) return -1;

    // 4. Allocation Bitmap 簇（簇3）
    uint8_t bmp[512];
    for (int i = 0; i < 512; i++) bmp[i] = 0;
    bmp[0] = 0x0F;   // 簇2,3,4,5 已用（bit0-3）
    if (exfat_write_cluster(3, bmp) != 0) return -1;

    // 5. Up-case Table 簇（簇4）：压缩表，仅含非恒等映射
    uint8_t upcase[512];
    for (int i = 0; i < 512; i++) upcase[i] = 0;
    int pos = 16;  // 跳过 4 字节 TableChecksum + 12 字节保留（规范表头 16 字节）
    for (int c = 0; c < 128; c++) {
        uint16_t uc = (uint16_t)c;
        if (c >= 'a' && c <= 'z') uc = (uint16_t)(c - 32);
        if (uc != (uint16_t)c) {   // 仅非恒等映射
            *((uint16_t*)(upcase + pos)) = (uint16_t)c;
            *((uint16_t*)(upcase + pos + 2)) = uc;
            pos += 4;
        }
    }
    *((uint16_t*)(upcase + pos)) = 0xFFFF;      // 终止符 code
    *((uint16_t*)(upcase + pos + 2)) = 0xFFFF;  // 终止符 mapped
    pos += 4;
    // 计算 TableChecksum（对整个 512 字节簇计算，前 4 字节按 0）
    uint32_t chk = 0;
    for (int i = 0; i < 512; i++) {
        chk = ((chk << 31) | (chk >> 1)) + upcase[i];
    }
    *((uint32_t*)(upcase + 0)) = chk;
    if (exfat_write_cluster(4, upcase) != 0) return -1;

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

    // 标准 exFAT VBR 解析
    uint8_t bps_shift = vbr[0x6E];   // BytesPerSectorShift @0x6E
    uint8_t spc_shift = vbr[0x6F];   // SectorsPerClusterShift @0x6F
    if (bps_shift < 9 || bps_shift > 12) return -1;
    exfat_info.bytes_per_sector = 1 << bps_shift;
    exfat_info.sectors_per_cluster = 1 << spc_shift;
    exfat_info.fat_offset = *((uint32_t*)(vbr + 0x50));
    exfat_info.fat_length = *((uint32_t*)(vbr + 0x54));
    exfat_info.cluster_heap_offset = *((uint32_t*)(vbr + 0x58));
    exfat_info.cluster_count = *((uint32_t*)(vbr + 0x5C));
    exfat_info.root_dir_cluster = *((uint32_t*)(vbr + 0x60));
    exfat_info.volume_length = *((uint64_t*)(vbr + 0x48));
    exfat_info.partition_offset = exfat_partition_start;

    if (exfat_info.bytes_per_sector != 512) return -1;
    if (exfat_info.sectors_per_cluster == 0) return -1;

    exfat_ready = 1;
    return 0;
}

/* 导出卷信息（供 kernel.c 打印带时间戳的 exFAT 启动日志） */
const exfat_info_t *exfat_get_info(void) {
    return &exfat_info;
}

int exfat_list_root(void) {
    if (!exfat_ready) return -1;

    static uint8_t root_cluster_data[512 * 16];
    uint8_t *buffer = root_cluster_data;

    uint32_t cluster = exfat_cwd_cluster();
    uint32_t cluster_size = exfat_info.bytes_per_sector * exfat_info.sectors_per_cluster;
    uint32_t cluster_count = exfat_read_dir_chain(cluster, buffer, 16);
    if (cluster_count == 0) return -1;

    uint32_t offset = 0;
    uint32_t total_size = cluster_count * cluster_size;
    while (offset < total_size) {
        uint8_t *entry = buffer + offset;
        if (entry[0] == 0x00) break;
        if (entry[0] == 0x85) {
            int sec_count = entry[1];
            uint8_t merged[512];
            if (exfat_parse_entry_set(entry, merged) == 0) {
                uint8_t is_dir = (merged[1] & 0x10) ? 1 : 0;
                char name[256];
                int name_len = merged[2];
                int nlen = 0;
                for (int i = 0; i < name_len && i < 255; i++) {
                    uint16_t ch = *((uint16_t*)(merged + 4 + i * 2));
                    if (ch >= 32 && ch <= 126) {
                        name[nlen++] = (char)ch;
                    } else {
                        name[nlen++] = '.';   // 用 '.' 替代无法显示的字符
                    }
                }
                name[nlen] = '\0';
                terminal_writestring(name);
                if (is_dir) terminal_writestring("/");
                terminal_putchar('\n');
            }
            offset += (1 + sec_count) * 32;
        } else {
            offset += 32;
        }
    }
    return 0;
}

// 读取当前目录全部目录项到结构体数组（供 desktop 文件管理器使用）
int exfat_read_dir(exfat_dir_entry_t *entries, int max_entries) {
    if (!exfat_ready) return -1;

    static uint8_t dir_data[512 * 16];
    uint8_t *buffer = dir_data;

    uint32_t cluster = exfat_cwd_cluster();
    uint32_t cluster_size = exfat_info.bytes_per_sector * exfat_info.sectors_per_cluster;
    uint32_t cluster_count = exfat_read_dir_chain(cluster, buffer, 16);
    if (cluster_count == 0) return -1;

    int count = 0;
    uint32_t offset = 0;
    uint32_t total_size = cluster_count * cluster_size;
    while (offset < total_size && count < max_entries) {
        uint8_t *entry = buffer + offset;
        if (entry[0] == 0x00) break;
        if (entry[0] == 0x85) {
            int sec_count = entry[1];
            uint8_t merged[512];
            if (exfat_parse_entry_set(entry, merged) == 0) {
                int name_len = merged[2];
                int nlen = 0;
                for (int i = 0; i < name_len && i < 255; i++) {
                    uint16_t ch = *((uint16_t*)(merged + 4 + i * 2));
                    if (ch >= 32 && ch <= 126) {
                        entries[count].name[nlen++] = (char)ch;
                    } else {
                        entries[count].name[nlen++] = '.';
                    }
                }
                entries[count].name[nlen] = '\0';
                entries[count].size = *((uint32_t*)(merged + EXFAT_MERGED_SIZE_OFF));
                entries[count].is_dir = (merged[1] & EXFAT_ATTR_DIRECTORY) ? 1 : 0;
                count++;
            }
            offset += (1 + sec_count) * 32;
        } else {
            offset += 32;
        }
    }
    return count;
}

uint32_t exfat_get_file_size(const char *name) {
    if (!exfat_ready) return 0;

    uint8_t entry[512];
    if (exfat_find_entry(exfat_cwd_cluster(), name, entry) < 0) return 0;
    return *((uint32_t*)(entry + EXFAT_MERGED_SIZE_OFF));
}

// 统计 FAT 表中已分配的簇数（供 df 使用）：扫描全部 FAT 表项，非 0 即已用
uint32_t exfat_count_used_clusters(void) {
    if (!exfat_ready) return 0;
    uint32_t used = 0;
    for (uint32_t c = 2; c < exfat_info.cluster_count + 2; c++) {
        if (exfat_read_fat_entry(c) != 0) used++;
    }
    return used;
}

// 沿 FAT 链统计文件/目录占用的簇数（供 du 使用）
uint32_t exfat_get_file_clusters(const char *name) {
    if (!exfat_ready) return 0;
    uint8_t entry[512];
    if (exfat_find_entry(exfat_cwd_cluster(), name, entry) < 0) return 0;
    uint32_t first = *((uint32_t*)(entry + EXFAT_MERGED_CLUSTER_OFF));
    if (first < 2) return 0;   // 空文件（0 簇）
    uint32_t n = 0, cur = first;
    while (cur >= 2 && cur < exfat_info.cluster_count + 2) {
        n++;
        cur = exfat_read_fat_entry(cur);
        if (cur >= 0xFFFFFFF8) break;
    }
    return n;
}

int exfat_read_file(const char *name, uint8_t *buffer, uint32_t max_size) {
    if (!exfat_ready) return -1;

    uint8_t entry[512];
    if (exfat_find_entry(exfat_cwd_cluster(), name, entry) < 0) return -1;
    uint32_t file_start_cluster = *((uint32_t*)(entry + EXFAT_MERGED_CLUSTER_OFF));
    uint32_t file_size = *((uint32_t*)(entry + EXFAT_MERGED_SIZE_OFF));
    if (file_start_cluster == 0) return -1;

    uint32_t cluster_size = exfat_info.bytes_per_sector * exfat_info.sectors_per_cluster;
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

    int name_len = 0;
    while (name[name_len]) name_len++;
    if (name_len == 0) return -1;

    // 重名检查
    uint8_t exist_entry[512];
    if (exfat_find_entry(exfat_cwd_cluster(), name, exist_entry) >= 0) return -1;

    uint32_t root_cluster = exfat_cwd_cluster();
    static uint8_t root_cluster_data[512 * 16];
    uint8_t *root_buffer = root_cluster_data;
    uint32_t dir_clusters = exfat_read_dir_chain(root_cluster, root_buffer, 16);
    if (dir_clusters == 0) return -1;

    int need_entries = exfat_entry_set_count(name_len);

    uint32_t cluster_size = exfat_info.bytes_per_sector * exfat_info.sectors_per_cluster;
    int free_entry_offset = exfat_find_free_set(root_buffer, dir_clusters * cluster_size, need_entries);
    if (free_entry_offset == -1) {
        // 目录已满，尝试扩展
        uint32_t new_dir_cl = exfat_extend_dir(root_cluster);
        if (new_dir_cl == 0) return -1;
        dir_clusters = exfat_read_dir_chain(root_cluster, root_buffer, 16);
        free_entry_offset = exfat_find_free_set(root_buffer, dir_clusters * cluster_size, need_entries);
        if (free_entry_offset == -1) return -1;
    }

    uint32_t data_clusters = (size + cluster_size - 1) / cluster_size;
    if (data_clusters == 0) data_clusters = 1;

    // 逐个分配空闲簇并串成 FAT 链（碎片化安全，不依赖连续簇），并同步 bitmap
    uint32_t first_cluster = 0;
    uint32_t prev_cluster = 0;
    for (uint32_t i = 0; i < data_clusters; i++) {
        uint32_t cl = exfat_find_free_cluster();
        if (cl == 0) {
            // 分配失败，回滚已占用的簇（含 bitmap）
            uint32_t cur = first_cluster;
            while (cur >= 2 && cur < exfat_info.cluster_count + 2) {
                uint32_t next = exfat_read_fat_entry(cur);
                exfat_write_fat_entry(cur, 0);
                exfat_bitmap_set(cur, 0);
                if (next >= 0xFFFFFFF8) break;
                cur = next;
            }
            return -1;
        }
        exfat_write_fat_entry(cl, 0xFFFFFFFF);  // 先标记为链尾
        exfat_bitmap_set(cl, 1);
        if (first_cluster == 0) first_cluster = cl;
        else exfat_write_fat_entry(prev_cluster, cl);
        prev_cluster = cl;
    }
    uint32_t start_cluster = first_cluster;

    // 写数据
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

    // 写 entry set
    exfat_write_entry_set(root_buffer, free_entry_offset, name, start_cluster, size, 0);
    if (exfat_write_cluster(root_cluster, root_buffer) != 0) return -1;
    if (dir_clusters > 1) {
        // 写回目录链其余簇
        uint32_t cur = exfat_read_fat_entry(root_cluster);
        uint32_t idx = 1;
        while (cur >= 2 && cur < exfat_info.cluster_count + 2 && idx < dir_clusters) {
            if (exfat_write_cluster(cur, root_buffer + idx * cluster_size) != 0) return -1;
            idx++;
            cur = exfat_read_fat_entry(cur);
        }
    }

    return 0;
}

int exfat_delete_file(const char *name) {
    if (!exfat_ready) return -1;

    static uint8_t root_cluster_data[512 * 16];
    uint8_t *root_buffer = root_cluster_data;
    uint32_t root_cluster = exfat_cwd_cluster();
    uint32_t cluster_size = exfat_info.bytes_per_sector * exfat_info.sectors_per_cluster;
    uint32_t dir_clusters = exfat_read_dir_chain(root_cluster, root_buffer, 16);
    if (dir_clusters == 0) return -1;

    uint32_t total_size = dir_clusters * cluster_size;
    for (uint32_t off = 0; off < total_size; ) {
        uint8_t *entry = root_buffer + off;
        if (entry[0] == 0x00) break;
        if (entry[0] == 0x85) {
            int sec_count = entry[1];
            uint8_t merged[512];
            if (exfat_parse_entry_set(entry, merged) == 0) {
                char entry_name[256];
                int name_len = merged[2];
                int nlen = 0;
                for (int i = 0; i < name_len && i < 255; i++) {
                    uint16_t ch = *((uint16_t*)(merged + 4 + i * 2));
                    if (ch < 128) entry_name[nlen++] = (char)ch;
                    else entry_name[nlen++] = '.';
                }
                entry_name[nlen] = '\0';
                if (my_strcmp(entry_name, name) == 0) {
                    // 目录不可直接删除（非空检查）
                    if ((merged[1] & EXFAT_ATTR_DIRECTORY) != 0) {
                        // 检查目录是否为空：读其第一簇，看是否有有效条目
                        uint32_t sub = *((uint32_t*)(merged + EXFAT_MERGED_CLUSTER_OFF));
                        static uint8_t sub_buf[512 * 16];
                        if (sub >= 2 && sub < exfat_info.cluster_count + 2) {
                            uint32_t sub_clusters = exfat_read_dir_chain(sub, sub_buf, 16);
                            uint32_t sub_total = sub_clusters * cluster_size;
                            int nonempty = 0;
                            for (uint32_t so = 0; so < sub_total; so += 32) {
                                uint8_t *se = sub_buf + so;
                                if (se[0] == 0x00) break;
                                if ((se[0] & 0x80) != 0) { nonempty = 1; break; }
                            }
                            if (nonempty) return -1;   // 目录非空，拒绝删除
                        }
                        // 空目录：释放目录簇链
                        uint32_t cl2 = sub;
                        while (cl2 >= 2 && cl2 < exfat_info.cluster_count + 2) {
                            uint32_t next = exfat_read_fat_entry(cl2);
                            exfat_write_fat_entry(cl2, 0);
                            exfat_bitmap_set(cl2, 0);
                            if (next >= 0xFFFFFFF8) break;
                            cl2 = next;
                        }
                    } else {
                        // 释放该文件占用的 FAT 链（含 bitmap）
                        uint32_t cl = *((uint32_t*)(merged + EXFAT_MERGED_CLUSTER_OFF));
                        while (cl >= 2 && cl < exfat_info.cluster_count + 2) {
                            uint32_t next = exfat_read_fat_entry(cl);
                            exfat_write_fat_entry(cl, 0);   // 标记为空闲
                            exfat_bitmap_set(cl, 0);
                            if (next >= 0xFFFFFFF8) break;
                            cl = next;
                        }
                    }
                    // 标记整个 entry set 为删除（清最高位：0x85→0x05, 0xC0→0x40, 0xC1→0x41）
                    for (int i = 0; i <= sec_count; i++) {
                        root_buffer[off + i * 32] &= 0x7F;
                    }
                    if (exfat_write_cluster(root_cluster, root_buffer) != 0) return -1;
                    if (dir_clusters > 1) {
                        // 写回目录链其余簇
                        uint32_t cur = exfat_read_fat_entry(root_cluster);
                        uint32_t idx = 1;
                        while (cur >= 2 && cur < exfat_info.cluster_count + 2 && idx < dir_clusters) {
                            if (exfat_write_cluster(cur, root_buffer + idx * cluster_size) != 0) return -1;
                            idx++;
                            cur = exfat_read_fat_entry(cur);
                        }
                    }
                    return 0;
                }
            }
            off += (1 + sec_count) * 32;
        } else {
            off += 32;
        }
    }
    return -1;
}
