/*
 * fat.c - FAT12/16/32 read-write driver for EZOS
 *
 * 支持标准 MBR 分区内的 FAT 卷与整盘 FAT（superfloppy）：
 *   - FAT12/16/32 自动识别（按簇数：<4085 / <65525 / >=65525）
 *   - 512/1024/2048/4096 字节扇区（ATA 层按 512B 组合读写）
 *   - LFN（长文件名）读取与创建；8.3 短名自动生成别名
 *   - 文件/目录的读、建、删；目录链扩展（FAT32 根与子目录）
 *   - 大小写不敏感匹配；FAT 表扇区缓存降低 ATA 流量
 *
 * API 语义与 exfat.c 保持一致，由 fs.c 统一分发。
 */
#include "fat.h"
#include "ata.h"
#include "tty.h"
#include "port.h"

#define ATTR_READONLY  0x01
#define ATTR_HIDDEN    0x02
#define ATTR_SYSTEM    0x04
#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_ARCHIVE   0x20
#define ATTR_LFN       0x0F

#define DELETED_ENTRY  0xE5

/* 块/簇缓冲上限（挂载时校验 cluster_size 不超此值） */
#define FAT_MAX_CLUSTER_BYTES  16384
#define FAT_MAX_BPS            4096

static fat_info_t fi;
static int fat_type = 0;          /* 0=未挂载 12/16/32 */
static uint8_t fat_drive = 1;
static uint32_t fat_part_start = 0;

static uint32_t current_dir_cluster = 0;   /* 0 = FAT12/16 根目录区 */
static char cwd_path[256] = "/";
static uint32_t dir_stack[32];
static int dir_depth = 0;

/* FAT 表扇区缓存（最多跨 2 个连续 FAT 扇区，覆盖 FAT12 跨界项） */
static uint8_t fat_cache[2 * FAT_MAX_BPS];
static uint32_t fat_cache_sec = 0xFFFFFFFF;
static int fat_cache_n = 0;

/* 共享块/簇 IO 暂存区：各使用点互不重叠（无嵌套持有） */
static uint8_t io_scratch[FAT_MAX_CLUSTER_BYTES];

/* 簇分配提示（扫描起点，避免每次从头扫 FAT） */
static uint32_t alloc_hint = 2;

/* ============ 基础工具 ============ */

static int up_char(int c) {
    if (c >= 'a' && c <= 'z') return c - 'a' + 'A';
    return c;
}

static int name_eq(const char *a, const char *b) {
    while (*a && *b && up_char((unsigned char)*a) == up_char((unsigned char)*b)) {
        a++; b++;
    }
    return up_char((unsigned char)*a) == up_char((unsigned char)*b);
}

static int my_strlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* 读取卷内一个 FAT 扇区（bps 可能 > 512，按 512 组合） */
static int fat_read_vol_sector(uint32_t vol_sec, uint8_t *buf) {
    uint32_t lba = fat_part_start + vol_sec;
    for (uint32_t i = 0; i < (uint32_t)fi.bytes_per_sector / 512; i++) {
        if (ata_read_sector(fat_drive, lba + i, buf + i * 512) != 0) return -1;
    }
    return 0;
}

static int fat_write_vol_sector(uint32_t vol_sec, const uint8_t *buf) {
    uint32_t lba = fat_part_start + vol_sec;
    for (uint32_t i = 0; i < (uint32_t)fi.bytes_per_sector / 512; i++) {
        if (ata_write_sector(fat_drive, lba + i, buf + i * 512) != 0) return -1;
    }
    return 0;
}

/* ============ FAT 表项访问（含跨扇区处理与缓存） ============ */

/* 装载 FAT 内 [byte_off, byte_off+len) 涉及的 1-2 个扇区到缓存 */
static int fat_cache_load(uint32_t byte_off, uint32_t len) {
    uint32_t sec = fi.reserved_sectors + byte_off / fi.bytes_per_sector;
    uint32_t in = byte_off % fi.bytes_per_sector;
    int n = (int)((in + len + fi.bytes_per_sector - 1) / fi.bytes_per_sector);
    if (n < 1) n = 1;
    if (n > 2) return -1;
    if (fat_cache_sec == sec && fat_cache_n >= n) return 0;
    for (int i = 0; i < n; i++) {
        if (fat_read_vol_sector(sec + (uint32_t)i,
                                fat_cache + (uint32_t)i * fi.bytes_per_sector) != 0)
            return -1;
    }
    fat_cache_sec = sec;
    fat_cache_n = n;
    return 0;
}

static uint32_t fat_entry_get(uint32_t n) {
    if (fat_type == 0 || n < 2 || n >= fi.cluster_count + 2) return 0xFFFFFFFF;
    uint32_t off, len;
    if (fat_type == 12) { off = n + n / 2; len = 2; }
    else if (fat_type == 16) { off = n * 2; len = 2; }
    else { off = n * 4; len = 4; }
    if (fat_cache_load(off, len) != 0) return 0xFFFFFFFF;
    uint32_t in = off % fi.bytes_per_sector;
    if (fat_type == 12) {
        uint32_t b0 = fat_cache[in];
        uint32_t b1 = fat_cache[in + 1];
        return (n & 1) ? ((b1 << 4) | (b0 >> 4)) : (b0 | ((b1 & 0x0F) << 8));
    } else if (fat_type == 16) {
        return fat_cache[in] | ((uint32_t)fat_cache[in + 1] << 8);
    }
    return (fat_cache[in] | ((uint32_t)fat_cache[in + 1] << 8) |
            ((uint32_t)fat_cache[in + 2] << 16) | ((uint32_t)fat_cache[in + 3] << 24)) & 0x0FFFFFFF;
}

static int fat_entry_set(uint32_t n, uint32_t val) {
    if (fat_type == 0 || n < 2 || n >= fi.cluster_count + 2) return -1;
    uint32_t off, len;
    if (fat_type == 12) { off = n + n / 2; len = 2; }
    else if (fat_type == 16) { off = n * 2; len = 2; }
    else { off = n * 4; len = 4; }
    if (fat_cache_load(off, len) != 0) return -1;
    uint32_t in = off % fi.bytes_per_sector;
    if (fat_type == 12) {
        if (n & 1) {
            fat_cache[in] = (uint8_t)((fat_cache[in] & 0x0F) | ((val << 4) & 0xF0));
            fat_cache[in + 1] = (uint8_t)(val >> 4);
        } else {
            fat_cache[in] = (uint8_t)(val & 0xFF);
            fat_cache[in + 1] = (uint8_t)((fat_cache[in + 1] & 0xF0) | ((val >> 8) & 0x0F));
        }
    } else if (fat_type == 16) {
        fat_cache[in] = (uint8_t)(val & 0xFF);
        fat_cache[in + 1] = (uint8_t)((val >> 8) & 0xFF);
    } else {
        fat_cache[in] = (uint8_t)(val & 0xFF);
        fat_cache[in + 1] = (uint8_t)((val >> 8) & 0xFF);
        fat_cache[in + 2] = (uint8_t)((val >> 16) & 0xFF);
        fat_cache[in + 3] = (uint8_t)((val >> 24) & 0xFF);
    }
    /* 写回涉及的扇区 */
    uint32_t sec0 = fi.reserved_sectors + off / fi.bytes_per_sector;
    for (int i = 0; i < fat_cache_n; i++) {
        if (fat_write_vol_sector(sec0 + (uint32_t)i,
                                 fat_cache + (uint32_t)i * fi.bytes_per_sector) != 0)
            return -1;
    }
    return 0;
}

static int fat_is_eoc(uint32_t v) {
    if (fat_type == 12) return v >= 0xFF8;
    if (fat_type == 16) return v >= 0xFFF8;
    return v >= 0x0FFFFFF8;
}

static int fat_is_bad(uint32_t v) {
    if (fat_type == 12) return v == 0xFF7;
    if (fat_type == 16) return v == 0xFFF7;
    return v == 0x0FFFFFF7;
}

static uint32_t fat_eoc_val(void) {
    if (fat_type == 12) return 0xFFF;
    if (fat_type == 16) return 0xFFFF;
    return 0x0FFFFFFF;
}

/* 从提示位置起扫描第一个空闲簇（FAT 缓存使顺序扫描每扇区只读一次） */
static uint32_t fat_alloc_cluster(void) {
    if (!fat_type) return 0;
    uint32_t total = fi.cluster_count + 2;
    if (alloc_hint < 2 || alloc_hint >= total) alloc_hint = 2;
    for (uint32_t i = 0; i < fi.cluster_count; i++) {
        uint32_t c = alloc_hint;
        if (c >= total) c = 2;
        if (fat_entry_get(c) == 0) {
            alloc_hint = c + 1;
            if (alloc_hint >= total) alloc_hint = 2;
            return c;
        }
        alloc_hint = c + 1;
    }
    return 0;
}

/* ============ 探测与挂载 ============ */

static uint32_t root_dir_cluster_value(void);

int fat_probe(const uint8_t *bs) {
    if (!bs) return 0;
    if (bs[510] != 0x55 || bs[511] != 0xAA) return 0;
    if (bs[0] != 0xEB && bs[0] != 0xE9) return 0;
    /* 排除 exFAT（OEM "EXFAT"）与 NTFS（OEM "NTFS"） */
    if (bs[3] == 'E' && bs[4] == 'X' && bs[5] == 'F' && bs[6] == 'A' && bs[7] == 'T') return 0;
    if (bs[3] == 'N' && bs[4] == 'T' && bs[5] == 'F' && bs[6] == 'S') return 0;

    uint32_t bps = bs[11] | ((uint32_t)bs[12] << 8);
    if (bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) return 0;
    uint32_t spc = bs[13];
    if (spc == 0 || (spc & (spc - 1)) != 0) return 0;
    uint32_t reserved = bs[14] | ((uint32_t)bs[15] << 8);
    if (reserved == 0) return 0;
    uint32_t nfats = bs[16];
    if (nfats < 1 || nfats > 2) return 0;
    uint32_t root_entries = bs[17] | ((uint32_t)bs[18] << 8);
    uint32_t total = bs[19] | ((uint32_t)bs[20] << 8);
    if (total == 0)
        total = bs[32] | ((uint32_t)bs[33] << 8) | ((uint32_t)bs[34] << 16) | ((uint32_t)bs[35] << 24);
    if (total == 0) return 0;
    uint32_t media = bs[21];
    if (media < 0xF0) return 0;
    uint32_t fat_size16 = bs[22] | ((uint32_t)bs[23] << 8);
    uint32_t fat_size32 = bs[36] | ((uint32_t)bs[37] << 8) | ((uint32_t)bs[38] << 16) | ((uint32_t)bs[39] << 24);
    uint32_t fat_size = fat_size16 ? fat_size16 : fat_size32;
    if (fat_size == 0) return 0;

    uint32_t root_dir_sectors = (root_entries * 32 + bps - 1) / bps;
    uint32_t first_data = reserved + nfats * fat_size + root_dir_sectors;
    if (first_data >= total) return 0;
    uint32_t clusters = (total - first_data) / spc;
    if (clusters < 2) return 0;

    if (clusters < 4085) return 12;
    if (clusters < 65525) return 16;
    /* FAT32 必要字段：root_entries=0、fat_size16=0、根簇合法 */
    if (root_entries != 0 || fat_size16 != 0) return 0;
    uint32_t root_cluster = bs[44] | ((uint32_t)bs[45] << 8) | ((uint32_t)bs[46] << 16) | ((uint32_t)bs[47] << 24);
    if (root_cluster < 2 || root_cluster >= clusters + 2) return 0;
    return 32;
}

int fat_mount(uint8_t drive, uint32_t part_start) {
    static uint8_t bs[FAT_MAX_BPS];
    fat_type = 0;
    fat_drive = drive;
    fat_part_start = part_start;

    /* 先读 512B 探测，再按 BPS 补足（最多 4KB） */
    if (ata_read_sector(drive, part_start, bs) != 0) return 0;
    uint32_t bps = bs[11] | ((uint32_t)bs[12] << 8);
    if (bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) return 0;
    for (uint32_t i = 1; i < bps / 512; i++) {
        if (ata_read_sector(drive, part_start + i, bs + i * 512) != 0) return 0;
    }

    int type = fat_probe(bs);
    if (type == 0) return 0;

    fi.bytes_per_sector = (uint16_t)bps;
    fi.sectors_per_cluster = bs[13];
    fi.reserved_sectors = (uint16_t)(bs[14] | ((uint32_t)bs[15] << 8));
    fi.num_fats = bs[16];
    fi.root_entry_count = (uint16_t)(bs[17] | ((uint32_t)bs[18] << 8));
    fi.total_sectors = bs[19] | ((uint32_t)bs[20] << 8);
    if (fi.total_sectors == 0)
        fi.total_sectors = bs[32] | ((uint32_t)bs[33] << 8) | ((uint32_t)bs[34] << 16) | ((uint32_t)bs[35] << 24);
    uint32_t fat_size16 = bs[22] | ((uint32_t)bs[23] << 8);
    fi.fat_sectors = fat_size16 ? fat_size16 :
        (bs[36] | ((uint32_t)bs[37] << 8) | ((uint32_t)bs[38] << 16) | ((uint32_t)bs[39] << 24));
    fi.root_dir_sectors = ((uint32_t)fi.root_entry_count * 32 + bps - 1) / bps;
    fi.first_data_sector = fi.reserved_sectors + (uint32_t)fi.num_fats * fi.fat_sectors + fi.root_dir_sectors;
    fi.cluster_count = (fi.total_sectors - fi.first_data_sector) / fi.sectors_per_cluster;
    fi.root_dir_sector = fi.reserved_sectors + (uint32_t)fi.num_fats * fi.fat_sectors;
    fi.root_cluster = (type == 32) ?
        (bs[44] | ((uint32_t)bs[45] << 8) | ((uint32_t)bs[46] << 16) | ((uint32_t)bs[47] << 24)) : 0;

    /* 资源上限校验：簇过大超出内核缓冲 */
    if (bps * fi.sectors_per_cluster > FAT_MAX_CLUSTER_BYTES) return 0;
    if (fi.cluster_count < 2) return 0;
    fi.part_start = part_start;

    /* 先置类型使 FAT 读取生效，做两项抽查后再确认挂载 */
    fat_type = type;
    fat_cache_sec = 0xFFFFFFFF;
    fat_cache_n = 0;
    /* FAT 区可读性抽查：簇 2 表项一定在合法范围内 */
    uint32_t fat_ok = (fat_entry_get(2) != 0xFFFFFFFF);
    uint32_t root_c = root_dir_cluster_value();
    int root_ok;
    if (type == 32) {
        root_ok = (root_c >= 2 && root_c < fi.cluster_count + 2 &&
                   fat_entry_get(root_c) != 0xFFFFFFFF);
    } else {
        /* FAT12/16：根目录区可读性抽查（读第一个根目录扇区） */
        root_ok = (fat_read_vol_sector(fi.root_dir_sector, io_scratch) == 0);
    }
    if (!fat_ok || !root_ok) {
        fat_type = 0;
        return 0;
    }

    alloc_hint = 2;
    /* FAT32 根目录是簇链（0 对 FAT12/16 才表示固定根目录区） */
    current_dir_cluster = (type == 32) ? fi.root_cluster : 0;
    cwd_path[0] = '/'; cwd_path[1] = '\0';
    dir_stack[0] = current_dir_cluster;
    dir_depth = 0;
    return type;
}

int fat_ready(void) { return fat_type != 0; }
const fat_info_t *fat_get_info(void) { return &fi; }

/* ============ 目录块抽象 ============ */

typedef struct {
    uint32_t cluster;   /* 目录块所在簇（FAT12/16 根区为 0） */
    uint32_t sector;    /* 块首扇区（卷相对） */
    uint32_t bytes;     /* 块字节数 */
} dir_block_t;

static uint32_t root_dir_cluster_value(void) {
    return (fat_type == 32) ? fi.root_cluster : 0;
}

/* 目录第 idx 块；返回 0 表示越界/链损坏 */
static int fat_dir_block(uint32_t dir_cluster, uint32_t idx, dir_block_t *out) {
    if (!fat_type) return 0;
    if (dir_cluster == 0 && fat_type != 32) {
        /* FAT12/16 根目录区 */
        if (idx >= fi.root_dir_sectors) return 0;
        out->cluster = 0;
        out->sector = fi.root_dir_sector + idx;
        out->bytes = fi.bytes_per_sector;
        return 1;
    }
    if (dir_cluster < 2 || dir_cluster >= fi.cluster_count + 2) return 0;
    uint32_t cur = dir_cluster;
    for (uint32_t i = 0; i < idx; i++) {
        cur = fat_entry_get(cur);
        if (cur < 2 || cur >= fi.cluster_count + 2 || fat_is_eoc(cur) || fat_is_bad(cur))
            return 0;
    }
    if (cur < 2 || cur >= fi.cluster_count + 2) return 0;
    out->cluster = cur;
    out->sector = fi.first_data_sector + (cur - 2) * fi.sectors_per_cluster;
    out->bytes = (uint32_t)fi.bytes_per_sector * fi.sectors_per_cluster;
    return 1;
}

static int fat_read_block(const dir_block_t *b, uint8_t *buf) {
    for (uint32_t i = 0; i < b->bytes / 512; i++) {
        if (ata_read_sector(fat_drive, fat_part_start + b->sector + i, buf + i * 512) != 0)
            return -1;
    }
    return 0;
}

static int fat_write_block(const dir_block_t *b, const uint8_t *buf) {
    for (uint32_t i = 0; i < b->bytes / 512; i++) {
        if (ata_write_sector(fat_drive, fat_part_start + b->sector + i, buf + i * 512) != 0)
            return -1;
    }
    return 0;
}

/* ============ 目录项枚举（含 LFN 组装） ============ */

typedef struct {
    char     name[256];
    uint32_t first_cluster;
    uint32_t size;
    uint8_t  is_dir;
    uint8_t  attr;
    /* 回写定位 */
    uint32_t block_cluster;
    uint32_t block_sector;
    uint32_t block_bytes;
    uint32_t entry_off;      /* 短名项在块内偏移 */
    int      lfn_count;      /* 前导 LFN 条目数 */
} fat_dirent_t;

/* LFN 校验和：短名 11 字节 */
static uint8_t lfn_checksum(const uint8_t *sn) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + sn[i]);
    return sum;
}

/* 从短名项生成 8.3 显示名 */
static void shortname_to_str(const uint8_t *e, char *out, int out_max) {
    int n = 0;
    int bl = 8; while (bl > 0 && e[bl - 1] == ' ') bl--;
    int el = 3; while (el > 0 && e[8 + el - 1] == ' ') el--;
    uint8_t nt = e[12];
    for (int i = 0; i < bl && n < out_max - 1; i++) {
        char c = (char)e[i];
        if ((uint8_t)c == 0x05) c = (char)0xE5;
        if ((nt & 0x08) && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out[n++] = c;
    }
    if (el > 0) {
        if (n < out_max - 1) out[n++] = '.';
        for (int i = 0; i < el && n < out_max - 1; i++) {
            char c = (char)e[8 + i];
            if ((nt & 0x10) && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            out[n++] = c;
        }
    }
    out[n] = '\0';
}

typedef int (*dirent_cb)(const fat_dirent_t *e, void *ctx);

typedef struct {
    /* LFN 组装状态 */
    uint16_t lfn_chars[256];
    int      lfn_len;
    uint8_t  lfn_cksum;
    int      lfn_seq_expect;   /* 期望的下一个（更小的）序号 */
    int      lfn_blocks;       /* 已见 LFN 条目数（含跨块） */
} lfn_state_t;

static void lfn_reset(lfn_state_t *st) {
    st->lfn_len = 0;
    st->lfn_seq_expect = -1;
    st->lfn_blocks = 0;
}

static void lfn_push(lfn_state_t *st, const uint8_t *e) {
    /* LFN 条目按序号降序出现：0x40|N 最先（名字尾部段），N=1 最后（名字头部） */
    int seq = e[0] & 0x1F;
    if (seq < 1 || seq > 20) { lfn_reset(st); return; }
    if (st->lfn_seq_expect != -1 && seq != st->lfn_seq_expect - 1) { lfn_reset(st); return; }
    st->lfn_seq_expect = seq;
    if (st->lfn_blocks < 24) st->lfn_blocks++;
    static const int off_tab[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    int base = (seq - 1) * 13;
    for (int i = 0; i < 13; i++) {
        uint16_t ch = (uint16_t)(e[off_tab[i]] | ((uint16_t)e[off_tab[i] + 1] << 8));
        int pos = base + i;
        if (ch == 0x0000 || ch == 0xFFFF) continue;
        if (pos < 255) st->lfn_chars[pos] = ch;
    }
    if (seq * 13 > st->lfn_len) {
        int newlen = seq * 13;
        if (newlen > 255) newlen = 255;
        st->lfn_len = newlen;
    }
}

static int dirent_from_short(const uint8_t *e, lfn_state_t *st,
                             const dir_block_t *b, uint32_t off,
                             fat_dirent_t *out) {
    uint8_t attr = e[11];
    if (attr & ATTR_VOLUME_ID) return 0;   /* 卷标跳过 */

    uint32_t clus_lo = e[26] | ((uint32_t)e[27] << 8);
    uint32_t clus_hi = (fat_type == 32) ? (e[20] | ((uint32_t)e[21] << 8)) : 0;

    out->first_cluster = clus_lo | (clus_hi << 16);
    out->size = e[28] | ((uint32_t)e[29] << 8) | ((uint32_t)e[30] << 16) | ((uint32_t)e[31] << 24);
    out->is_dir = (attr & ATTR_DIRECTORY) ? 1 : 0;
    out->attr = attr;
    out->block_cluster = b->cluster;
    out->block_sector = b->sector;
    out->block_bytes = b->bytes;
    out->entry_off = off;
    out->lfn_count = 0;

    /* LFN 校验：序号必须收束到 1 且校验和匹配 */
    if (st->lfn_len > 0 && st->lfn_seq_expect == 1 &&
        st->lfn_cksum == lfn_checksum(e)) {
        int n = 0;
        for (int i = 0; i < st->lfn_len && i < 255; i++) {
            uint16_t ch = st->lfn_chars[i];
            if (ch == 0x0000) break;
            out->name[n++] = (ch >= 32 && ch < 127) ? (char)ch : '_';
        }
        out->name[n] = '\0';
        if (n > 0) {
            out->lfn_count = st->lfn_blocks;
            return 1;
        }
    }
    shortname_to_str(e, out->name, (int)sizeof(out->name));
    return 1;
}

/* 枚举目录；cb 返回 1 停止（命中）；返回枚举到的条目总数或 -1（读错误） */
static int fat_dir_foreach(uint32_t dir_cluster, dirent_cb cb, void *ctx) {
    lfn_state_t st;
    uint32_t idx = 0;
    int count = 0;
    for (;;) {
        dir_block_t b;
        if (!fat_dir_block(dir_cluster, idx, &b)) break;
        if (fat_read_block(&b, io_scratch) != 0) return -1;
        int stop = 0;
        lfn_reset(&st);
        for (uint32_t off = 0; off + 32 <= b.bytes; off += 32) {
            const uint8_t *e = io_scratch + off;
            if (e[0] == 0x00) { stop = 1; break; }   /* 目录结束 */
            if (e[0] == DELETED_ENTRY) { lfn_reset(&st); continue; }
            if ((e[11] & ATTR_LFN) == ATTR_LFN) {
                st.lfn_cksum = e[13];
                lfn_push(&st, e);
                continue;
            }
            fat_dirent_t de;
            if (dirent_from_short(e, &st, &b, off, &de)) {
                count++;
                if (cb && cb(&de, ctx)) { stop = 1; break; }
            }
            lfn_reset(&st);
        }
        if (stop) break;
        idx++;
    }
    return count;
}

/* ============ 查找与枚举输出 ============ */

typedef struct {
    const char *want;
    fat_dirent_t found;
    int hit;
} find_ctx_t;

static int find_cb(const fat_dirent_t *e, void *ctx) {
    find_ctx_t *fc = (find_ctx_t*)ctx;
    if (name_eq(e->name, fc->want)) {
        fc->found = *e;
        fc->hit = 1;
        return 1;
    }
    return 0;
}

static int fat_find_entry(uint32_t dir_cluster, const char *name, fat_dirent_t *out) {
    find_ctx_t fc;
    fc.want = name;
    fc.hit = 0;
    fc.found.name[0] = '\0';
    fat_dir_foreach(dir_cluster, find_cb, &fc);
    if (!fc.hit) return -1;
    if (out) *out = fc.found;
    return 0;
}

typedef struct {
    fat_dir_entry_t *entries;   /* 与 fs_dir_entry_t 布局一致 */
    int max;
    int n;
} list_ctx_t;

static int list_cb(const fat_dirent_t *e, void *ctx) {
    list_ctx_t *lc = (list_ctx_t*)ctx;
    if (e->name[0] == '.' && (e->name[1] == '\0' ||
        (e->name[1] == '.' && e->name[2] == '\0')))
        return 0;
    if (lc->n >= lc->max) return 1;
    int i = 0;
    while (e->name[i] && i < 255) { lc->entries[lc->n].name[i] = e->name[i]; i++; }
    lc->entries[lc->n].name[i] = '\0';
    lc->entries[lc->n].size = e->size;
    lc->entries[lc->n].is_dir = e->is_dir;
    lc->n++;
    return 0;
}

static int print_cb(const fat_dirent_t *e, void *ctx) {
    (void)ctx;
    if (e->name[0] == '.' && (e->name[1] == '\0' ||
        (e->name[1] == '.' && e->name[2] == '\0')))
        return 0;
    terminal_writestring(e->name);
    if (e->is_dir) terminal_writestring("/");
    terminal_putchar('\n');
    return 0;
}

/* ============ cwd 管理（语义对齐 exfat.c） ============ */

uint32_t fat_cwd_cluster(void) {
    if (!fat_type) return 0;
    return current_dir_cluster;
}

const char *fat_cwd_path(void) { return cwd_path; }

int fat_change_dir(const char *name) {
    if (!fat_type) return -1;
    if (name[0] == '\0' || name_eq(name, ".")) return 0;

    uint32_t root = root_dir_cluster_value();
    uint32_t cur_cluster = root;
    int depth = 0;
    char pathbuf[256];
    int pb = 0;
    pathbuf[pb++] = '/';

    if (name[0] == '/') {
        if (name_eq(name, "/")) {
            dir_stack[0] = root; dir_depth = 0;
            current_dir_cluster = root;
            cwd_path[0] = '/'; cwd_path[1] = '\0';
            return 0;
        }
        const char *p = name + 1;
        char comp[128];
        uint32_t walk_stack[32];
        walk_stack[0] = root;
        while (*p) {
            int cl = 0;
            while (*p && *p != '/' && cl < 127) comp[cl++] = *p++;
            while (*p && *p != '/') p++;
            comp[cl] = '\0';
            if (*p == '/') p++;
            if (cl == 0) continue;
            if (name_eq(comp, "..")) {
                if (depth > 0) {
                    depth--;
                    while (pb > 1 && pathbuf[pb-1] != '/') pb--;
                    if (pb > 1) pb--;
                    cur_cluster = (depth == 0) ? root : walk_stack[depth];
                }
                continue;
            }
            if (name_eq(comp, ".")) continue;
            if (depth >= 31) return -1;
            fat_dirent_t de;
            if (fat_find_entry(cur_cluster, comp, &de) != 0) return -1;
            if (!de.is_dir) return -1;
            cur_cluster = de.first_cluster;
            depth++;
            walk_stack[depth] = cur_cluster;
            if (pb + cl + 1 >= (int)sizeof(pathbuf)) return -1;
            if (pb > 1) pathbuf[pb++] = '/';
            for (int i = 0; i < cl; i++) pathbuf[pb++] = comp[i];
        }
        dir_stack[0] = root;
        for (int i = 1; i <= depth; i++) dir_stack[i] = walk_stack[i];
        dir_depth = depth;
        current_dir_cluster = cur_cluster;
        pathbuf[pb] = '\0';
        for (int i = 0; i <= pb; i++) cwd_path[i] = pathbuf[i];
        return 0;
    }

    /* 相对路径 */
    uint32_t saved_cwd = current_dir_cluster;
    int saved_depth = dir_depth;
    uint32_t saved_stack[32];
    for (int i = 0; i <= dir_depth && i < 32; i++) saved_stack[i] = dir_stack[i];
    cur_cluster = current_dir_cluster;
    depth = dir_depth;
    int path_pb = 0;
    char pathbuf2[256];
    for (int i = 0; cwd_path[i]; i++) pathbuf2[path_pb++] = cwd_path[i];
    pathbuf2[path_pb] = '\0';

    const char *p = name;
    while (*p) {
        char comp[128];
        int cl = 0;
        while (*p && *p != '/' && cl < 127) comp[cl++] = *p++;
        while (*p && *p != '/') p++;
        comp[cl] = '\0';
        if (*p == '/') p++;
        if (cl == 0) continue;
        if (name_eq(comp, "..")) {
            if (depth > 0) {
                depth--;
                while (path_pb > 1 && pathbuf2[path_pb-1] != '/') path_pb--;
                if (path_pb > 1) path_pb--;
                pathbuf2[path_pb] = '\0';
                cur_cluster = (depth == 0) ? root : dir_stack[depth];
            }
            continue;
        }
        if (name_eq(comp, ".")) continue;
        if (depth >= 31) goto rollback;
        fat_dirent_t de;
        if (fat_find_entry(cur_cluster, comp, &de) != 0) goto rollback;
        if (!de.is_dir) goto rollback;
        cur_cluster = de.first_cluster;
        depth++;
        dir_stack[depth] = cur_cluster;
        if (path_pb + cl + 1 >= (int)sizeof(pathbuf2)) goto rollback;
        if (path_pb > 1) pathbuf2[path_pb++] = '/';
        for (int i = 0; i < cl; i++) pathbuf2[path_pb++] = comp[i];
        pathbuf2[path_pb] = '\0';
    }
    dir_stack[0] = root;
    dir_depth = depth;
    current_dir_cluster = cur_cluster;
    for (int i = 0; i <= path_pb; i++) cwd_path[i] = pathbuf2[i];
    return 0;

rollback:
    dir_depth = saved_depth;
    for (int i = 0; i <= saved_depth && i < 32; i++) dir_stack[i] = saved_stack[i];
    current_dir_cluster = saved_cwd;
    return -1;
}

/* ============ 簇数据读写 ============ */

static int fat_read_cluster(uint32_t c, uint8_t *buf) {
    if (c < 2 || c >= fi.cluster_count + 2) return -1;
    uint32_t sec = fi.first_data_sector + (c - 2) * fi.sectors_per_cluster;
    uint32_t n = (uint32_t)fi.bytes_per_sector * fi.sectors_per_cluster / 512;
    for (uint32_t i = 0; i < n; i++) {
        if (ata_read_sector(fat_drive, fat_part_start + sec + i, buf + i * 512) != 0)
            return -1;
    }
    return 0;
}

static int fat_write_cluster(uint32_t c, const uint8_t *buf) {
    if (c < 2 || c >= fi.cluster_count + 2) return -1;
    uint32_t sec = fi.first_data_sector + (c - 2) * fi.sectors_per_cluster;
    uint32_t n = (uint32_t)fi.bytes_per_sector * fi.sectors_per_cluster / 512;
    for (uint32_t i = 0; i < n; i++) {
        if (ata_write_sector(fat_drive, fat_part_start + sec + i, buf + i * 512) != 0)
            return -1;
    }
    return 0;
}

/* 释放簇链 */
static int fat_free_chain(uint32_t first) {
    if (first < 2) return 0;
    uint32_t cur = first;
    uint32_t steps = 0;
    while (cur >= 2 && cur < fi.cluster_count + 2 && steps < fi.cluster_count + 2) {
        uint32_t next = fat_entry_get(cur);
        if (fat_entry_set(cur, 0) != 0) return -1;
        if (first < alloc_hint) alloc_hint = first;   /* 提示回拨以复用刚释放的簇 */
        if (fat_is_eoc(next) || next < 2 || fat_is_bad(next)) break;
        cur = next;
        steps++;
    }
    return 0;
}

/* ============ 文件读取 ============ */

int fat_read_file(const char *name, uint8_t *buffer, uint32_t max_size) {
    if (!fat_type) return -1;
    fat_dirent_t de;
    if (fat_find_entry(current_dir_cluster, name, &de) != 0) return -1;
    if (de.is_dir) return -1;
    if (de.size == 0 || de.first_cluster == 0) return 0;

    uint32_t cluster_size = (uint32_t)fi.bytes_per_sector * fi.sectors_per_cluster;
    uint32_t bytes_read = 0;
    uint32_t cur = de.first_cluster;
    uint32_t steps = 0;
    while (bytes_read < de.size && bytes_read < max_size &&
           cur >= 2 && cur < fi.cluster_count + 2 &&
           steps < fi.cluster_count + 2) {
        if (fat_read_cluster(cur, io_scratch) != 0) return -1;
        uint32_t to_copy = de.size - bytes_read;
        if (to_copy > cluster_size) to_copy = cluster_size;
        if (to_copy > max_size - bytes_read) to_copy = max_size - bytes_read;
        for (uint32_t i = 0; i < to_copy; i++) buffer[bytes_read + i] = io_scratch[i];
        bytes_read += to_copy;
        if (bytes_read >= de.size || bytes_read >= max_size) break;
        cur = fat_entry_get(cur);
        if (fat_is_eoc(cur) || fat_is_bad(cur)) break;
        steps++;
    }
    return (int)bytes_read;
}

uint32_t fat_get_file_size(const char *name) {
    if (!fat_type) return 0;
    fat_dirent_t de;
    if (fat_find_entry(current_dir_cluster, name, &de) != 0) return 0;
    return de.size;
}

uint32_t fat_get_file_clusters(const char *name) {
    if (!fat_type) return 0;
    fat_dirent_t de;
    if (fat_find_entry(current_dir_cluster, name, &de) != 0) return 0;
    if (de.first_cluster < 2) return 0;
    uint32_t n = 0, cur = de.first_cluster;
    while (cur >= 2 && cur < fi.cluster_count + 2 && n < fi.cluster_count + 2) {
        n++;
        cur = fat_entry_get(cur);
        if (fat_is_eoc(cur) || fat_is_bad(cur)) break;
    }
    return n;
}

uint32_t fat_count_used_clusters(void) {
    if (!fat_type) return 0;
    uint32_t used = 0;
    for (uint32_t c = 2; c < fi.cluster_count + 2; c++) {
        if (fat_entry_get(c) != 0) used++;
    }
    return used;
}

int fat_list_root(void) {
    if (!fat_type) return -1;
    fat_dir_foreach(current_dir_cluster, print_cb, 0);
    return 0;
}

int fat_read_dir(fat_dir_entry_t *entries, int max_entries) {
    if (!fat_type || !entries) return -1;
    list_ctx_t lc;
    lc.entries = entries;
    lc.max = max_entries;
    lc.n = 0;
    fat_dir_foreach(current_dir_cluster, list_cb, &lc);
    return lc.n;
}

/* ============ 写入：短名/LFN 生成与目录项写回 ============ */

static void write_timestamp(uint8_t *e) {
    uint16_t date = 0x5C21;   /* 2026-01-01 */
    e[14] = 0; e[15] = 0;                          /* CrtTime 00:00:00 */
    e[16] = (uint8_t)(date & 0xFF); e[17] = (uint8_t)(date >> 8);
    e[18] = (uint8_t)(date & 0xFF); e[19] = (uint8_t)(date >> 8);   /* LstAccDate */
    e[22] = 0; e[23] = 0;                          /* WrtTime */
    e[24] = (uint8_t)(date & 0xFF); e[25] = (uint8_t)(date >> 8);
}

/* 判断字符是否可入 8.3 短名 */
static int sfn_char_ok(char c) {
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    static const char ok[] = "!#$%&'()-@^_`{}~";
    for (int i = 0; ok[i]; i++) if (c == ok[i]) return 1;
    return 0;
}

/* 短名 11 字节 → 显示字符串（供冲突探测） */
static void sn11_to_str(const uint8_t *sn11, char *out, int out_max) {
    int n = 0;
    int bl = 8; while (bl > 0 && sn11[bl - 1] == ' ') bl--;
    int el = 3; while (el > 0 && sn11[8 + el - 1] == ' ') el--;
    for (int i = 0; i < bl && n < out_max - 1; i++) out[n++] = (char)sn11[i];
    if (el > 0) {
        if (n < out_max - 1) out[n++] = '.';
        for (int i = 0; i < el && n < out_max - 1; i++) out[n++] = (char)sn11[8 + i];
    }
    out[n] = '\0';
}

/* 尝试将 name 编码为 8.3（全大写、无非法字符、长度合法）；成功返回 1 */
static int try_make_sfn(const char *name, uint8_t *sn11) {
    int len = my_strlen(name);
    int dot = -1;
    for (int i = 0; i < len; i++) {
        if (name[i] == '.') {
            if (dot >= 0) return 0;
            dot = i;
        }
    }
    int base_len = (dot >= 0) ? dot : len;
    int ext_len = (dot >= 0) ? (len - dot - 1) : 0;
    if (base_len < 1 || base_len > 8) return 0;
    if (ext_len > 3) return 0;
    for (int i = 0; i < base_len; i++) {
        char c = (char)up_char((unsigned char)name[i]);
        if (!sfn_char_ok(c)) return 0;
        sn11[i] = (uint8_t)c;
    }
    for (int i = base_len; i < 8; i++) sn11[i] = ' ';
    for (int i = 0; i < ext_len; i++) {
        char c = (char)up_char((unsigned char)name[dot + 1 + i]);
        if (!sfn_char_ok(c)) return 0;
        sn11[8 + i] = (uint8_t)c;
    }
    for (int i = ext_len; i < 3; i++) sn11[8 + i] = ' ';
    return 1;
}

/* 生成别名 BASE~N.EXT（N 从 1 递增直到不冲突）；成功返回 1 */
static int make_alias(const char *name, uint32_t dir_cluster, uint8_t *sn11) {
    char base[8]; int bl = 0;
    char ext[3];  int el = 0;
    int len = my_strlen(name);
    int dot = -1;
    for (int i = 0; i < len; i++) if (name[i] == '.') { dot = i; break; }
    int limit = (dot >= 0) ? dot : len;
    for (int i = 0; i < limit && bl < 8; i++) {
        char c = (char)up_char((unsigned char)name[i]);
        if (sfn_char_ok(c)) base[bl++] = c;
    }
    if (bl == 0) base[bl++] = '_';
    if (dot >= 0) {
        for (int i = dot + 1; i < len && el < 3; i++) {
            char c = (char)up_char((unsigned char)name[i]);
            if (sfn_char_ok(c)) ext[el++] = c;
        }
    }

    for (int n = 1; n <= 999999; n++) {
        /* "~n" 后缀（最长 7 字符，保证 base 至少留 1 字符） */
        char suf[8]; int sl = 0;
        char rev[8]; int rl = 0;
        uint32_t v = (uint32_t)n;
        while (v) { rev[rl++] = (char)('0' + v % 10); v /= 10; }
        while (rl > 0 && sl < 7) suf[sl++] = rev[--rl];
        if (sl > 6) return 0;   /* 别名空间耗尽 */

        int keep = 8 - 1 - sl;
        if (keep > bl) keep = bl;
        if (keep < 1) keep = 1;
        for (int i = 0; i < keep; i++) sn11[i] = (uint8_t)base[i];
        sn11[keep] = '~';
        for (int i = 0; i < sl; i++) sn11[keep + 1 + i] = (uint8_t)suf[i];
        for (int i = keep + 1 + sl; i < 8; i++) sn11[i] = ' ';
        for (int i = 0; i < el; i++) sn11[8 + i] = (uint8_t)ext[i];
        for (int i = el; i < 3; i++) sn11[8 + i] = ' ';

        /* 冲突检查：目录内无同名 8.3 项即采纳 */
        char probe[16];
        sn11_to_str(sn11, probe, (int)sizeof(probe));
        fat_dirent_t de;
        if (fat_find_entry(dir_cluster, probe, &de) != 0) return 1;
    }
    return 0;
}

/* 写单个 32 字节目录项到（块定位，块内偏移） */
static int fat_write_dirent_at(const fat_dirent_t *ref, uint32_t off, const uint8_t *entry) {
    dir_block_t b;
    b.cluster = ref->block_cluster;
    b.sector = ref->block_sector;
    b.bytes = ref->block_bytes;
    if (off + 32 > b.bytes) return -1;
    if (fat_read_block(&b, io_scratch) != 0) return -1;
    for (int i = 0; i < 32; i++) io_scratch[off + i] = entry[i];
    return fat_write_block(&b, io_scratch);
}

/* 在目录中找一段同块连续空闲项（need 个）；找不到且目录为簇链时扩展新簇 */
static int fat_find_free_slots(uint32_t dir_cluster, int need, fat_dirent_t *ref) {
    uint32_t idx = 0;
    for (;;) {
        dir_block_t b;
        if (!fat_dir_block(dir_cluster, idx, &b)) break;
        if (fat_read_block(&b, io_scratch) != 0) return -1;
        int run = 0;
        for (uint32_t off = 0; off + 32 <= b.bytes; off += 32) {
            if (io_scratch[off] == 0x00 || io_scratch[off] == DELETED_ENTRY) {
                run++;
                if (run == need) {
                    ref->block_cluster = b.cluster;
                    ref->block_sector = b.sector;
                    ref->block_bytes = b.bytes;
                    ref->entry_off = off + 32 - (uint32_t)need * 32;
                    return 0;
                }
            } else {
                run = 0;
            }
        }
        idx++;
    }
    /* 未找到：簇链目录尝试扩展一个新簇 */
    if (dir_cluster >= 2) {
        uint32_t tail = dir_cluster;
        uint32_t steps = 0;
        while (steps < fi.cluster_count + 2) {
            uint32_t next = fat_entry_get(tail);
            if (fat_is_eoc(next) || next < 2 || next >= fi.cluster_count + 2) break;
            tail = next;
            steps++;
        }
        uint32_t nc = fat_alloc_cluster();
        if (nc == 0) return -1;
        if (fat_entry_set(tail, nc) != 0) return -1;
        if (fat_entry_set(nc, fat_eoc_val()) != 0) return -1;
        for (uint32_t i = 0; i < (uint32_t)fi.bytes_per_sector * fi.sectors_per_cluster; i++)
            io_scratch[i] = 0;
        if (fat_write_cluster(nc, io_scratch) != 0) return -1;
        ref->block_cluster = nc;
        ref->block_sector = fi.first_data_sector + (nc - 2) * fi.sectors_per_cluster;
        ref->block_bytes = (uint32_t)fi.bytes_per_sector * fi.sectors_per_cluster;
        ref->entry_off = 0;
        return 0;
    }
    return -1;   /* FAT12/16 根目录区固定大小，已满 */
}

/* 为 name 生成短名（必要时含别名），返回 LFN 条目数（0=无需 LFN） */
static int prepare_names(const char *name, uint32_t dir_cluster, uint8_t *sn11) {
    int need_lfn = 0;
    if (try_make_sfn(name, sn11)) {
        /* 8.3 可编码：若原名的显示大小写与 8.3 全大写不一致，仍写 LFN 保留原名 */
        char probe[16];
        sn11_to_str(sn11, probe, (int)sizeof(probe));
        if (!name_eq(probe, name)) need_lfn = 1;
    } else {
        need_lfn = 1;
    }
    if (need_lfn) {
        if (!make_alias(name, dir_cluster, sn11)) return -1;
        int name_len = my_strlen(name);
        return (name_len + 12) / 13;
    }
    return 0;
}

/* 写 LFN 条目 + 短名项（通用：文件与目录共用） */
static int write_dir_entries(const fat_dirent_t *ref, const char *name,
                             const uint8_t *sn11, int lfn_entries,
                             uint8_t attr, uint32_t first_cluster, uint32_t size) {
    uint8_t ck = lfn_checksum(sn11);
    static const int lfn_off_tab[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    int name_len = my_strlen(name);

    for (int n = lfn_entries; n >= 1; n--) {
        uint8_t ent[32];
        for (int i = 0; i < 32; i++) ent[i] = 0;
        ent[0] = (uint8_t)(n | ((n == lfn_entries) ? 0x40 : 0x00));
        ent[11] = ATTR_LFN;
        ent[13] = ck;
        for (int i = 0; i < 13; i++) {
            int idx = (n - 1) * 13 + i;
            uint16_t ch = (idx < name_len) ? (uint8_t)name[idx] : 0xFFFF;
            if (idx == name_len) ch = 0x0000;
            ent[lfn_off_tab[i]] = (uint8_t)(ch & 0xFF);
            ent[lfn_off_tab[i] + 1] = (uint8_t)(ch >> 8);
        }
        /* 物理顺序必须降序：seqN(0x40) 在最前、seq1 紧邻短名项（与 lfn_push 读取一致） */
        uint32_t off = ref->entry_off + (uint32_t)(lfn_entries - n) * 32;
        if (fat_write_dirent_at(ref, off, ent) != 0) return -1;
    }

    uint8_t ent[32];
    for (int i = 0; i < 32; i++) ent[i] = 0;
    for (int i = 0; i < 11; i++) ent[i] = sn11[i];
    ent[11] = attr;
    write_timestamp(ent);
    if (fat_type == 32) {
        ent[20] = (uint8_t)((first_cluster >> 16) & 0xFF);
        ent[21] = (uint8_t)((first_cluster >> 24) & 0xFF);
    }
    ent[26] = (uint8_t)(first_cluster & 0xFF);
    ent[27] = (uint8_t)((first_cluster >> 8) & 0xFF);
    ent[28] = (uint8_t)(size & 0xFF);
    ent[29] = (uint8_t)((size >> 8) & 0xFF);
    ent[30] = (uint8_t)((size >> 16) & 0xFF);
    ent[31] = (uint8_t)((size >> 24) & 0xFF);
    uint32_t off = ref->entry_off + (uint32_t)lfn_entries * 32;
    return fat_write_dirent_at(ref, off, ent);
}

/* ============ 创建文件 / 删除 / mkdir ============ */

int fat_create_file(const char *name, const uint8_t *data, uint32_t size) {
    if (!fat_type) return -1;
    int name_len = my_strlen(name);
    if (name_len == 0 || name_len > 255) return -1;
    if (name[name_len - 1] == '/') return -1;

    /* 重名检查 */
    fat_dirent_t de;
    if (fat_find_entry(current_dir_cluster, name, &de) == 0) return -1;

    uint8_t sn11[11];
    int lfn_entries = prepare_names(name, current_dir_cluster, sn11);
    if (lfn_entries < 0) return -1;
    int need = 1 + lfn_entries;

    /* 分配数据簇链并写入数据 */
    uint32_t cluster_size = (uint32_t)fi.bytes_per_sector * fi.sectors_per_cluster;
    uint32_t data_clusters = (size + cluster_size - 1) / cluster_size;
    uint32_t first_cluster = 0;
    if (data_clusters > 0) {
        uint32_t prev = 0;
        for (uint32_t i = 0; i < data_clusters; i++) {
            uint32_t c = fat_alloc_cluster();
            if (c == 0) {
                fat_free_chain(first_cluster);   /* 回滚已分配簇 */
                return -1;
            }
            if (fat_entry_set(c, fat_eoc_val()) != 0) return -1;
            if (first_cluster == 0) first_cluster = c;
            else if (fat_entry_set(prev, c) != 0) return -1;
            prev = c;
        }
        uint32_t written = 0;
        uint32_t cur = first_cluster;
        for (uint32_t i = 0; i < data_clusters; i++) {
            for (uint32_t j = 0; j < cluster_size; j++) io_scratch[j] = 0;
            uint32_t to_copy = size - written;
            if (to_copy > cluster_size) to_copy = cluster_size;
            for (uint32_t j = 0; j < to_copy; j++) io_scratch[j] = data[written + j];
            if (fat_write_cluster(cur, io_scratch) != 0) return -1;
            written += to_copy;
            if (i + 1 < data_clusters) cur = fat_entry_get(cur);
        }
    }

    /* 找空闲目录项并写入 */
    fat_dirent_t ref;
    if (fat_find_free_slots(current_dir_cluster, need, &ref) != 0) {
        if (first_cluster) fat_free_chain(first_cluster);
        return -1;
    }
    if (write_dir_entries(&ref, name, sn11, lfn_entries,
                          ATTR_ARCHIVE, first_cluster, size) != 0)
        return -1;
    return 0;
}

/* 目录空判定（仅 . 与 ..） */
typedef struct { int nonempty; } empty_ctx_t;

static int empty_check_cb(const fat_dirent_t *e, void *ctx) {
    empty_ctx_t *ec = (empty_ctx_t*)ctx;
    if (e->name[0] == '.' && (e->name[1] == '\0' ||
        (e->name[1] == '.' && e->name[2] == '\0')))
        return 0;
    ec->nonempty = 1;
    return 1;
}

int fat_delete_file(const char *name) {
    if (!fat_type) return -1;
    fat_dirent_t de;
    if (fat_find_entry(current_dir_cluster, name, &de) != 0) return -1;

    if (de.is_dir) {
        empty_ctx_t ec;
        ec.nonempty = 0;
        fat_dir_foreach(de.first_cluster, empty_check_cb, &ec);
        if (ec.nonempty) return -1;   /* 非空目录拒绝删除 */
        fat_free_chain(de.first_cluster);
    } else if (de.first_cluster >= 2) {
        fat_free_chain(de.first_cluster);
    }

    /* 标记删除：同块内短名项前面的连续 LFN 项 + 短名项 */
    dir_block_t b;
    b.cluster = de.block_cluster;
    b.sector = de.block_sector;
    b.bytes = de.block_bytes;
    if (de.entry_off + 32 > b.bytes) return -1;
    if (fat_read_block(&b, io_scratch) != 0) return -1;
    uint32_t off = de.entry_off;
    while (off >= 32) {
        uint8_t *prev = io_scratch + off - 32;
        if ((prev[11] & ATTR_LFN) == ATTR_LFN && prev[0] != DELETED_ENTRY) {
            prev[0] = DELETED_ENTRY;
            off -= 32;
        } else break;
    }
    io_scratch[de.entry_off] = DELETED_ENTRY;
    return fat_write_block(&b, io_scratch);
}

int fat_mkdir(const char *name) {
    if (!fat_type) return -1;
    int name_len = my_strlen(name);
    if (name_len == 0 || name_len > 255) return -1;

    fat_dirent_t de;
    if (fat_find_entry(current_dir_cluster, name, &de) == 0) return -1;

    uint32_t parent_cluster = current_dir_cluster;

    /* 分配目录簇并初始化 . 与 .. */
    uint32_t nc = fat_alloc_cluster();
    if (nc == 0) return -1;
    if (fat_entry_set(nc, fat_eoc_val()) != 0) return -1;

    uint32_t cluster_size = (uint32_t)fi.bytes_per_sector * fi.sectors_per_cluster;
    for (uint32_t i = 0; i < cluster_size; i++) io_scratch[i] = 0;
    /* "." 项 */
    io_scratch[0] = '.';
    for (int i = 1; i < 11; i++) io_scratch[i] = ' ';
    io_scratch[11] = ATTR_DIRECTORY;
    write_timestamp(io_scratch);
    if (fat_type == 32) {
        io_scratch[20] = (uint8_t)((nc >> 16) & 0xFF);
        io_scratch[21] = (uint8_t)((nc >> 24) & 0xFF);
    }
    io_scratch[26] = (uint8_t)(nc & 0xFF);
    io_scratch[27] = (uint8_t)((nc >> 8) & 0xFF);
    /* ".." 项（父目录为 FAT12/16 根目录区时簇号 0） */
    uint32_t parent_for_dotdot = parent_cluster;
    io_scratch[32] = '.'; io_scratch[33] = '.';
    for (int i = 34; i < 43; i++) io_scratch[i] = ' ';
    io_scratch[43] = ATTR_DIRECTORY;
    write_timestamp(io_scratch + 32);
    if (fat_type == 32) {
        io_scratch[52] = (uint8_t)((parent_for_dotdot >> 16) & 0xFF);
        io_scratch[53] = (uint8_t)((parent_for_dotdot >> 24) & 0xFF);
    }
    io_scratch[58] = (uint8_t)(parent_for_dotdot & 0xFF);
    io_scratch[59] = (uint8_t)((parent_for_dotdot >> 8) & 0xFF);
    if (fat_write_cluster(nc, io_scratch) != 0) return -1;

    /* 父目录写目录项 */
    uint8_t sn11[11];
    int lfn_entries = prepare_names(name, parent_cluster, sn11);
    if (lfn_entries < 0) return -1;
    int need = 1 + lfn_entries;

    fat_dirent_t ref;
    if (fat_find_free_slots(parent_cluster, need, &ref) != 0) return -1;
    if (write_dir_entries(&ref, name, sn11, lfn_entries,
                          ATTR_DIRECTORY, nc, 0) != 0)
        return -1;
    return 0;
}
