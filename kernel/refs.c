/*
 * refs.c - ReFS 驱动（读 + 写，简化布局）
 *
 * 磁盘布局（相对卷起始 LBA，512B 扇区，4KB 簇，2MB 卷）：
 *   LBA 0      : ReFS 卷头（真实结构：0x03 "ReFS\0\0\0\0" + 0x10 "FSRS"，
 *                0x40 起为自有布局字段 "EZOSREFS"）
 *   LBA 8..15  : 簇位图（4KB，1 bit/簇，first-fit 分配）
 *   LBA 16..143: 目录项数组（128 项 x 512B）
 *   LBA 144..  : 数据区（4KB 簇；文件数据按连续簇分配）
 *
 * 目录项（512B）：
 *   +0x00 "RFSE" magic / +0x04 flags(1=file 2=dir) / +0x08 parent_idx
 *   (0xFFFFFFFF=根直接子项) / +0x0C first_cluster / +0x10 n_clusters
 *   / +0x14 size / +0x18 name[220]（ASCII，NUL 结尾）
 *
 * 真实 ReFS（B+ 树 + 完整性流）不支持：非 "EZOSREFS" 标识的 ReFS 盘
 * 挂载失败，交给后续探测（见 refs.h 的 refs 列表）。
 */
#include "refs.h"
#include "ata.h"

/* ---------- 小端读写助手 ---------- */
static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* ---------- 常量 ---------- */
#define REFS_SIG_OFF        0x03    /* "ReFS\0\0\0\0"（libfsrefs 卷头表） */
#define REFS_FSRS_OFF       0x10    /* "FSRS" */
#define REFS_FSRS_LEN_OFF   0x14    /* 0x0200 */
#define REFS_FSRS_CKSUM_OFF 0x16    /* WORD 递加和的补码（MSDN 算法） */
#define REFS_VHDR_NSECS     0x18    /* 卷扇区数 u64 */
#define REFS_VHDR_SEC_SIZE  0x20    /* 512 */
#define REFS_VHDR_SPC       0x24    /* 每簇扇区数 */
#define REFS_VHDR_VER       0x28    /* major/minor */
/* 自有布局字段（真实 ReFS 此区为卷序列号/保留区） */
#define REFS_EZ_MAGIC_OFF   0x40    /* "EZOSREFS" */
#define REFS_EZ_LAYOUT_OFF  0x48    /* 布局版本 = 1 */
#define REFS_EZ_BITMAP_OFF  0x4C    /* 位图起始 LBA（相对卷） */
#define REFS_EZ_META_OFF    0x50    /* 目录项区起始 LBA */
#define REFS_EZ_META_N_OFF  0x54    /* 目录项数 */
#define REFS_EZ_DATA_OFF    0x58    /* 数据区起始 LBA */
#define REFS_EZ_DCLUST_OFF  0x5C    /* 数据区总簇数 */

#define RS_CLUSTER_SECS     8       /* 4KB / 512 */
#define RS_CLUSTER_SIZE     4096
#define RS_BITMAP_SECS      8       /* 位图 4KB（覆盖 32768 簇） */

#define RE_MAGIC            0x00
#define RE_FLAGS            0x04
#define RE_PARENT           0x08
#define RE_FIRST_CL         0x0C
#define RE_NCLUSTERS        0x10
#define RE_SIZE             0x14
#define RE_NAME             0x18
#define RE_NAME_MAX         220

#define RE_FLAG_FILE        1
#define RE_FLAG_DIR         2

#define RS_ROOT_PARENT      0xFFFFFFFFu
#define RS_NO_CLUSTER       0xFFFFFFFFu

/* format 布局（2MB 卷） */
#define RS_FMT_VOL_SECS     4096
#define RS_FMT_BITMAP_LBA   8
#define RS_FMT_META_LBA     16
#define RS_FMT_META_N       128
#define RS_FMT_DATA_LBA     144     /* 128 项 x 512B = 64KB 后对齐簇 */

/* ---------- 驱动状态 ---------- */
static uint8_t  rs_drive;
static uint8_t  rs_mounted;
static uint32_t rs_part_lba;
static uint32_t rs_bitmap_lba, rs_meta_lba, rs_data_lba;
static uint32_t rs_meta_entries, rs_data_clusters;
static refs_info_t rs_info;

/* 大缓冲放高内存段 .bss.hi（1MB+，见 linker.ld）：低 640KB 区留给栈/小数据 */
#define RS_HIBUF __attribute__((section(".bss.hi")))
static uint8_t rs_bitmap[RS_CLUSTER_SIZE] RS_HIBUF; /* 位图缓存（惰性加载） */
static uint8_t rs_sec[512] RS_HIBUF;                /* 目录项暂存 */
static uint8_t rs_dcl[RS_CLUSTER_SIZE] RS_HIBUF;    /* 数据簇读写中转 */
static uint8_t  rs_bitmap_loaded;

/* ---------- 底层读写 ---------- */
static int rs_read_secs(uint32_t lba, uint8_t *buf, uint32_t nsecs) {
    for (uint32_t i = 0; i < nsecs; i++)
        if (ata_read_sector(rs_drive, lba + i, buf + i * 512) != 0) return -1;
    return 0;
}
static int rs_write_secs(uint32_t lba, const uint8_t *buf, uint32_t nsecs) {
    for (uint32_t i = 0; i < nsecs; i++)
        if (ata_write_sector(rs_drive, lba + i, buf + i * 512) != 0) return -1;
    return 0;
}

static int rs_meta_read(uint32_t idx, uint8_t *buf) {
    if (idx >= rs_meta_entries) return -1;
    return rs_read_secs(rs_part_lba + rs_meta_lba + idx, buf, 1);
}
static int rs_meta_write(uint32_t idx, const uint8_t *buf) {
    if (idx >= rs_meta_entries) return -1;
    return rs_write_secs(rs_part_lba + rs_meta_lba + idx, buf, 1);
}

/* ---------- 名字比较（大小写不敏感，与其他驱动一致） ---------- */
static char rs_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}
static int rs_name_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (rs_lower(*a) != rs_lower(*b)) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

/* ---------- 位图 ---------- */
static int rs_bitmap_load(void) {
    if (rs_bitmap_loaded) return 0;
    if (rs_read_secs(rs_part_lba + rs_bitmap_lba, rs_bitmap, RS_BITMAP_SECS) != 0)
        return -1;
    rs_bitmap_loaded = 1;
    return 0;
}
static int rs_bitmap_save(void) {
    if (!rs_bitmap_loaded) return 0;
    return rs_write_secs(rs_part_lba + rs_bitmap_lba, rs_bitmap, RS_BITMAP_SECS);
}
static int rs_bit_test(uint32_t cl) {
    return rs_bitmap[cl >> 3] & (1u << (cl & 7));
}
static void rs_bit_set(uint32_t cl, int v) {
    if (v) rs_bitmap[cl >> 3] |= (uint8_t)(1u << (cl & 7));
    else   rs_bitmap[cl >> 3] &= (uint8_t)~(1u << (cl & 7));
}
/* first-fit 找连续 n 空闲簇；0xFFFFFFFF=无 */
static uint32_t rs_alloc_contig(uint32_t n) {
    if (n == 0) return RS_NO_CLUSTER;
    if (rs_bitmap_load() != 0) return RS_NO_CLUSTER;
    uint32_t run = 0;
    for (uint32_t cl = 0; cl + n <= rs_data_clusters; cl++) {
        if (!rs_bit_test(cl)) {
            run++;
            if (run == n) {
                uint32_t first = cl - n + 1;
                for (uint32_t k = 0; k < n; k++) rs_bit_set(first + k, 1);
                return first;
            }
        } else {
            run = 0;
        }
    }
    return RS_NO_CLUSTER;
}
static void rs_free_clusters(uint32_t first, uint32_t n) {
    if (first == RS_NO_CLUSTER || n == 0) return;
    if (rs_bitmap_load() != 0) return;
    for (uint32_t k = 0; k < n; k++) rs_bit_set(first + k, 0);
}

/* ---------- 目录项查找 ---------- */
static int rs_entry_valid(const uint8_t *e) {
    return e[0] == 'R' && e[1] == 'F' && e[2] == 'S' && e[3] == 'E';
}

/* 在 parent 下找 name（大小写不敏感）；返回目录项 idx，未找到 0xFFFFFFFF */
static uint32_t rs_find(uint32_t parent, const char *name) {
    char nm[RE_NAME_MAX + 1];
    uint32_t l = 0;
    while (name[l] && l < RE_NAME_MAX) { nm[l] = name[l]; l++; }
    nm[l] = 0;

    for (uint32_t i = 0; i < rs_meta_entries; i++) {
        if (rs_meta_read(i, rs_sec) != 0) return RS_ROOT_PARENT;
        if (!rs_entry_valid(rs_sec)) continue;
        if (rd32(rs_sec + RE_PARENT) != parent) continue;
        if (rs_name_eq((const char *)rs_sec + RE_NAME, nm))
            return i;
    }
    return RS_ROOT_PARENT;
}

/* 找空闲目录项槽位（magic 无效即可复用）；无则 0xFFFFFFFF */
static uint32_t rs_free_slot(void) {
    for (uint32_t i = 0; i < rs_meta_entries; i++) {
        if (rs_meta_read(i, rs_sec) != 0) return RS_ROOT_PARENT;
        if (!rs_entry_valid(rs_sec)) return i;
    }
    return RS_ROOT_PARENT;
}

/* ---------- 路径解析 ---------- */
/* 路径是否为根（"/" 或全斜杠） */
static int rs_is_root_path(const char *path) {
    while (*path == '/') path++;
    return *path == 0;
}

/* 绝对路径 -> 目录项 idx；返回 RS_ROOT_PARENT 表示不存在（根无实体项，
 * 调用方按需特判）。parent_out/is_dir_out/size_out 可空。 */
static uint32_t rs_resolve(const char *path, uint32_t *parent_out,
                           int *is_dir_out, uint32_t *size_out) {
    if (!rs_mounted || path == 0 || path[0] != '/') return RS_ROOT_PARENT;

    uint32_t prev = RS_ROOT_PARENT;   /* 最终项的父目录 idx */
    uint32_t cur = RS_ROOT_PARENT;    /* 当前所在目录 idx（初始 = 根） */
    while (*path == '/') path++;

    while (*path) {
        char comp[RE_NAME_MAX + 1];
        uint32_t clen = 0;
        while (path[clen] && path[clen] != '/') clen++;
        if (clen == 0 || clen > RE_NAME_MAX) return RS_ROOT_PARENT;
        for (uint32_t i = 0; i < clen; i++) comp[i] = path[i];
        comp[clen] = 0;

        uint32_t next = rs_find(cur, comp);
        if (next == RS_ROOT_PARENT) return RS_ROOT_PARENT;
        if (rs_meta_read(next, rs_sec) != 0) return RS_ROOT_PARENT;
        /* 中间段必须是目录（末段允许文件） */
        if (rd32(rs_sec + RE_FLAGS) != RE_FLAG_DIR && path[clen] != 0)
            return RS_ROOT_PARENT;

        prev = cur;    /* next 的父 = 当前目录 */
        cur = next;
        path += clen;
        while (*path == '/') path++;
    }

    if (cur == RS_ROOT_PARENT) return RS_ROOT_PARENT;
    if (rs_meta_read(cur, rs_sec) != 0) return RS_ROOT_PARENT;
    if (parent_out) *parent_out = prev;
    if (is_dir_out) *is_dir_out = rd32(rs_sec + RE_FLAGS) == RE_FLAG_DIR;
    if (size_out) *size_out = rd32(rs_sec + RE_SIZE);
    return cur;
}

/* ---------- 挂载 ---------- */
int refs_mount(uint8_t drive, uint32_t part_start) {
    rs_mounted = 0;
    rs_bitmap_loaded = 0;
    uint8_t vh[512];

    if (ata_read_sector(drive, part_start, vh) != 0) return -1;

    /* 真实 ReFS 卷头签名（libfsrefs：0x03 "ReFS\0\0\0\0" + 0x10 "FSRS"） */
    if (vh[0x03] != 'R' || vh[0x04] != 'e' || vh[0x05] != 'F' ||
        vh[0x06] != 'S' || vh[0x07] != 0 || vh[0x08] != 0 || vh[0x09] != 0)
        return -1;
    if (vh[0x10] != 'F' || vh[0x11] != 'S' || vh[0x12] != 'R' || vh[0x13] != 'S')
        return -1;
    /* 自布局标识（真实 Windows ReFS 盘无此标记：B+ 树布局不支持） */
    if (vh[REFS_EZ_MAGIC_OFF + 0] != 'E' || vh[REFS_EZ_MAGIC_OFF + 1] != 'Z' ||
        vh[REFS_EZ_MAGIC_OFF + 2] != 'O' || vh[REFS_EZ_MAGIC_OFF + 3] != 'S' ||
        vh[REFS_EZ_MAGIC_OFF + 4] != 'R' || vh[REFS_EZ_MAGIC_OFF + 5] != 'E' ||
        vh[REFS_EZ_MAGIC_OFF + 6] != 'F' || vh[REFS_EZ_MAGIC_OFF + 7] != 'S')
        return -1;
    if (rd32(vh + REFS_EZ_LAYOUT_OFF) != 1) return -1;

    rs_drive = drive;
    rs_part_lba = part_start;
    rs_bitmap_lba = rd32(vh + REFS_EZ_BITMAP_OFF);
    rs_meta_lba = rd32(vh + REFS_EZ_META_OFF);
    rs_meta_entries = rd32(vh + REFS_EZ_META_N_OFF);
    rs_data_lba = rd32(vh + REFS_EZ_DATA_OFF);
    rs_data_clusters = rd32(vh + REFS_EZ_DCLUST_OFF);

    /* 布局字段合法性 */
    if (rs_meta_entries == 0 || rs_meta_entries > 4096) return -1;
    if (rs_bitmap_lba < 1 || rs_meta_lba <= rs_bitmap_lba ||
        rs_data_lba <= rs_meta_lba || rs_data_clusters == 0)
        return -1;

    /* df：遍历目录项累加 n_clusters */
    uint32_t used = 0;
    for (uint32_t i = 0; i < rs_meta_entries; i++) {
        if (rs_meta_read(i, rs_sec) != 0) return -1;
        if (rs_entry_valid(rs_sec))
            used += rd32(rs_sec + RE_NCLUSTERS);
    }

    rs_info.part_start = part_start;
    rs_info.bytes_per_sector = 512;
    rs_info.sectors_per_cluster = RS_CLUSTER_SECS;
    rs_info.cluster_count = rs_data_clusters;
    rs_info.volume_sectors = rd32(vh + REFS_VHDR_NSECS);   /* 2MB 卷低 32 位足够 */
    rs_info.used_clusters = used;

    rs_mounted = 1;
    return 0;
}

const refs_info_t *refs_get_info(void) { return &rs_info; }

/* ---------- 对外读 API ---------- */
int refs_is_dir(const char *path) {
    if (rs_is_root_path(path) && rs_mounted) return 1;   /* 根无实体项，特判 */
    int is_dir;
    if (rs_resolve(path, 0, &is_dir, 0) == RS_ROOT_PARENT) return -1;
    return is_dir;
}

uint32_t refs_get_file_size(const char *path) {
    uint32_t size;
    if (rs_resolve(path, 0, 0, &size) == RS_ROOT_PARENT) return 0;
    return size;
}

int refs_read_file(const char *path, uint8_t *buffer, uint32_t max_size) {
    uint32_t idx, size;
    int is_dir;
    idx = rs_resolve(path, 0, &is_dir, &size);
    if (idx == RS_ROOT_PARENT || is_dir) return -1;
    if (rs_meta_read(idx, rs_sec) != 0) return -1;
    uint32_t first = rd32(rs_sec + RE_FIRST_CL);

    if (size > max_size) size = max_size;
    uint32_t done = 0;
    while (done < size) {
        uint32_t chunk = RS_CLUSTER_SIZE;
        if (chunk > size - done) chunk = size - done;
        if (first == RS_NO_CLUSTER) {
            /* size>0 但无簇：洞读 0 */
            for (uint32_t i = 0; i < chunk; i++) buffer[done + i] = 0;
        } else {
            uint32_t cl = first + done / RS_CLUSTER_SIZE;
            /* 经 rs_dcl 中转：尾部截断簇不能整簇直读进 buffer（防越界） */
            if (rs_read_secs(rs_part_lba + rs_data_lba +
                             cl * RS_CLUSTER_SECS, rs_dcl,
                             RS_CLUSTER_SECS) != 0) return -1;
            for (uint32_t i = 0; i < chunk; i++)
                buffer[done + i] = rs_dcl[(done % RS_CLUSTER_SIZE) + i];
        }
        done += chunk;
    }
    return (int)size;
}

int refs_read_dir(const char *path, fs_dir_entry_t *entries, int max_entries) {
    /* 根目录的子项 parent = RS_ROOT_PARENT；子目录的 parent = 其目录项 idx */
    uint32_t want_parent;
    if (rs_is_root_path(path)) {
        want_parent = RS_ROOT_PARENT;
    } else {
        int is_dir;
        uint32_t dir_idx = rs_resolve(path, 0, &is_dir, 0);
        if (dir_idx == RS_ROOT_PARENT || !is_dir) return -1;
        want_parent = dir_idx;
    }

    int n = 0;
    for (uint32_t i = 0; i < rs_meta_entries && n < max_entries; i++) {
        if (rs_meta_read(i, rs_sec) != 0) return -1;
        if (!rs_entry_valid(rs_sec)) continue;
        if (rd32(rs_sec + RE_PARENT) != want_parent) continue;

        const char *nm = (const char *)rs_sec + RE_NAME;
        uint32_t j = 0;
        while (nm[j] && j < RE_NAME_MAX && j < sizeof(entries[n].name) - 1) {
            entries[n].name[j] = nm[j];
            j++;
        }
        entries[n].name[j] = 0;
        entries[n].size = rd32(rs_sec + RE_SIZE);
        entries[n].is_dir = rd32(rs_sec + RE_FLAGS) == RE_FLAG_DIR;
        n++;
    }
    return n;
}

uint32_t refs_get_file_clusters(const char *path) {
    uint32_t idx = rs_resolve(path, 0, 0, 0);
    if (idx == RS_ROOT_PARENT) return 0;
    if (rs_meta_read(idx, rs_sec) != 0) return 0;
    return rd32(rs_sec + RE_NCLUSTERS);
}

/* ---------- 写 API ---------- */
static int rs_name_copy(uint8_t *e, const char *name) {
    uint32_t l = 0;
    while (name[l] && l < RE_NAME_MAX) { e[RE_NAME + l] = (uint8_t)name[l]; l++; }
    e[RE_NAME + l] = 0;
    return (int)l;
}

/* 解析父目录路径与文件名："a/b/c.txt" -> parent 项 idx + fname。
 * 成功 0；路径非法或父不存在 -1 */
static int rs_split_parent(const char *path, uint32_t *parent_out,
                           const char **fname_out) {
    if (!rs_mounted || path == 0 || path[0] != '/') return -1;
    const char *p = path;
    while (*p == '/') p++;

    uint32_t parent = RS_ROOT_PARENT;
    while (1) {
        const char *comp = p;
        uint32_t clen = 0;
        while (p[clen] && p[clen] != '/') clen++;
        /* 末段 = 文件名 */
        if (p[clen] == 0) {
            if (clen == 0 || clen > RE_NAME_MAX) return -1;
            *parent_out = parent;
            *fname_out = comp;
            return 0;
        }
        if (clen == 0 || clen > RE_NAME_MAX) return -1;
        char compbuf[RE_NAME_MAX + 1];
        for (uint32_t i = 0; i < clen; i++) compbuf[i] = comp[i];
        compbuf[clen] = 0;

        uint32_t next = rs_find(parent, compbuf);
        if (next == RS_ROOT_PARENT) return -1;
        if (rs_meta_read(next, rs_sec) != 0) return -1;
        if (rd32(rs_sec + RE_FLAGS) != RE_FLAG_DIR) return -1;
        parent = next;
        p += clen;
        while (*p == '/') p++;
        if (*p == 0) return -1;   /* 尾随 '/' 视为非法 */
    }
}

int refs_create_file(const char *path, const uint8_t *data, uint32_t size) {
    uint32_t parent;
    const char *fname;
    if (rs_split_parent(path, &parent, &fname) != 0) return -1;

    /* create-or-replace：同父同名旧文件释放簇、复用槽位 */
    uint32_t idx = rs_find(parent, fname);
    if (idx != RS_ROOT_PARENT) {
        if (rs_meta_read(idx, rs_sec) != 0) return -1;
        if (rd32(rs_sec + RE_FLAGS) != RE_FLAG_FILE) return -1;  /* 目录不让覆盖 */
        uint32_t old_ncl = rd32(rs_sec + RE_NCLUSTERS);
        rs_free_clusters(rd32(rs_sec + RE_FIRST_CL), old_ncl);
        if (old_ncl <= rs_info.used_clusters) rs_info.used_clusters -= old_ncl;
    } else {
        idx = rs_free_slot();
        if (idx == RS_ROOT_PARENT) return -1;   /* 目录项满 */
    }

    /* 分配连续簇并写数据 */
    uint32_t ncl = (size + RS_CLUSTER_SIZE - 1) / RS_CLUSTER_SIZE;
    uint32_t first = RS_NO_CLUSTER;
    if (ncl > 0) {
        first = rs_alloc_contig(ncl);
        if (first == RS_NO_CLUSTER) return -1;
        for (uint32_t k = 0; k < ncl; k++) {
            uint32_t off = k * RS_CLUSTER_SIZE;
            uint32_t chunk = size - off > RS_CLUSTER_SIZE ?
                             RS_CLUSTER_SIZE : size - off;
            for (uint32_t i = 0; i < RS_CLUSTER_SIZE; i++)
                rs_dcl[i] = (i < chunk) ? data[off + i] : 0;
            if (rs_write_secs(rs_part_lba + rs_data_lba +
                              (first + k) * RS_CLUSTER_SECS, rs_dcl,
                              RS_CLUSTER_SECS) != 0)
                return -1;
        }
    }
    /* 覆盖为空文件（ncl=0）时也要把释放的旧簇位图落盘 */
    if (rs_bitmap_save() != 0) return -1;

    /* 写目录项 */
    for (uint32_t i = 0; i < 512; i++) rs_sec[i] = 0;
    rs_sec[RE_MAGIC + 0] = 'R'; rs_sec[RE_MAGIC + 1] = 'F';
    rs_sec[RE_MAGIC + 2] = 'S'; rs_sec[RE_MAGIC + 3] = 'E';
    wr32(rs_sec + RE_FLAGS, RE_FLAG_FILE);
    wr32(rs_sec + RE_PARENT, parent);
    wr32(rs_sec + RE_FIRST_CL, first);
    wr32(rs_sec + RE_NCLUSTERS, ncl);
    wr32(rs_sec + RE_SIZE, size);
    rs_name_copy(rs_sec, fname);
    if (rs_meta_write(idx, rs_sec) != 0) return -1;

    rs_info.used_clusters += ncl;
    return 0;
}

int refs_delete_file(const char *path) {
    uint32_t idx, parent;
    int is_dir;
    idx = rs_resolve(path, &parent, &is_dir, 0);
    if (idx == RS_ROOT_PARENT) return -1;
    if (rs_meta_read(idx, rs_sec) != 0) return -1;

    if (is_dir) {
        /* 目录须为空（借 rs_dcl 低 512B 扫描，不覆盖 rs_sec 当前项） */
        for (uint32_t i = 0; i < rs_meta_entries; i++) {
            if (i == idx) continue;
            uint8_t *tmp = rs_dcl;
            if (rs_meta_read(i, tmp) != 0) return -1;
            if (rs_entry_valid(tmp) && rd32(tmp + RE_PARENT) == idx)
                return -1;   /* 非空目录 */
        }
    }

    uint32_t first = rd32(rs_sec + RE_FIRST_CL);
    uint32_t ncl = rd32(rs_sec + RE_NCLUSTERS);
    if (ncl > rs_info.used_clusters) ncl = rs_info.used_clusters;
    rs_info.used_clusters -= ncl;

    /* 清目录项（整扇区清零） */
    for (uint32_t i = 0; i < 512; i++) rs_sec[i] = 0;
    if (rs_meta_write(idx, rs_sec) != 0) return -1;

    rs_free_clusters(first, ncl);
    if (rs_bitmap_save() != 0) return -1;
    return 0;
}

int refs_mkdir(const char *path) {
    uint32_t parent;
    const char *fname;
    if (rs_split_parent(path, &parent, &fname) != 0) return -1;
    if (rs_find(parent, fname) != RS_ROOT_PARENT) return -1;  /* 已存在 */

    uint32_t idx = rs_free_slot();
    if (idx == RS_ROOT_PARENT) return -1;

    for (uint32_t i = 0; i < 512; i++) rs_sec[i] = 0;
    rs_sec[RE_MAGIC + 0] = 'R'; rs_sec[RE_MAGIC + 1] = 'F';
    rs_sec[RE_MAGIC + 2] = 'S'; rs_sec[RE_MAGIC + 3] = 'E';
    wr32(rs_sec + RE_FLAGS, RE_FLAG_DIR);
    wr32(rs_sec + RE_PARENT, parent);
    wr32(rs_sec + RE_FIRST_CL, RS_NO_CLUSTER);
    wr32(rs_sec + RE_NCLUSTERS, 0);
    wr32(rs_sec + RE_SIZE, 0);
    rs_name_copy(rs_sec, fname);
    if (rs_meta_write(idx, rs_sec) != 0) return -1;
    return 0;
}

/* ---------- 格式化 ---------- */
/* MSDN "Computing a File System Recognition Checksum"：
 * 结构前 0x18 字节按 WORD 递加（跳过 0x16 处校验和自身），取补码 */
static uint16_t rs_fsrs_checksum(const uint8_t *vh) {
    uint32_t sum = 0;
    for (uint32_t off = 0; off < 0x18; off += 2) {
        if (off == REFS_FSRS_CKSUM_OFF) continue;
        sum += rd16(vh + off);
    }
    return (uint16_t)(0x10000 - (sum & 0xFFFF));
}

int refs_format(uint8_t drive) {
    if (drive > 3) return -1;

    const uint32_t part_start = 1;
    const uint32_t vol_secs = RS_FMT_VOL_SECS;           /* 2MB */
    const uint32_t data_clusters =
        (vol_secs - RS_FMT_DATA_LBA) / RS_CLUSTER_SECS;

    /* format 期间直写需要驱动器状态（mount 前置好） */
    rs_mounted = 0;
    rs_bitmap_loaded = 0;
    rs_drive = drive;
    rs_part_lba = part_start;
    rs_bitmap_lba = RS_FMT_BITMAP_LBA;
    rs_meta_lba = RS_FMT_META_LBA;
    rs_meta_entries = RS_FMT_META_N;
    rs_data_lba = RS_FMT_DATA_LBA;
    rs_data_clusters = data_clusters;

    /* MBR：分区 0x07（NTFS/exFAT/ReFS 系列 ID）@LBA1（与其他 format 一致） */
    static uint8_t mbr[512] RS_HIBUF;
    for (int i = 0; i < 512; i++) mbr[i] = 0;
    mbr[446 + 4] = 0x07;                    /* 分区类型 */
    wr32(mbr + 446 + 8, part_start);
    wr32(mbr + 446 + 12, vol_secs - 1);
    mbr[510] = 0x55; mbr[511] = 0xAA;
    if (ata_write_sector(drive, 0, mbr) != 0) return -1;

    /* 卷头（真实 ReFS 结构 + 自布局字段） */
    static uint8_t vh[512] RS_HIBUF;
    for (int i = 0; i < 512; i++) vh[i] = 0;
    vh[0x00] = 0; vh[0x01] = 0; vh[0x02] = 0;            /* boot entry：ReFS 不可启动 */
    vh[0x03] = 'R'; vh[0x04] = 'e'; vh[0x05] = 'F'; vh[0x06] = 'S';
    vh[0x07] = 0; vh[0x08] = 0; vh[0x09] = 0; vh[0x0A] = 0;
    vh[0x10] = 'F'; vh[0x11] = 'S'; vh[0x12] = 'R'; vh[0x13] = 'S';
    wr16(vh + REFS_FSRS_LEN_OFF, 0x0200);
    wr16(vh + REFS_FSRS_CKSUM_OFF, rs_fsrs_checksum(vh));
    wr32(vh + REFS_VHDR_NSECS, vol_secs);               /* u64 低 32 位（小卷） */
    wr32(vh + REFS_VHDR_NSECS + 4, 0);
    wr32(vh + REFS_VHDR_SEC_SIZE, 512);
    wr32(vh + REFS_VHDR_SPC, RS_CLUSTER_SECS);
    vh[0x28] = 1; vh[0x29] = 2;                         /* ReFS 1.2 */
    vh[REFS_EZ_MAGIC_OFF + 0] = 'E'; vh[REFS_EZ_MAGIC_OFF + 1] = 'Z';
    vh[REFS_EZ_MAGIC_OFF + 2] = 'O'; vh[REFS_EZ_MAGIC_OFF + 3] = 'S';
    vh[REFS_EZ_MAGIC_OFF + 4] = 'R'; vh[REFS_EZ_MAGIC_OFF + 5] = 'E';
    vh[REFS_EZ_MAGIC_OFF + 6] = 'F'; vh[REFS_EZ_MAGIC_OFF + 7] = 'S';
    wr32(vh + REFS_EZ_LAYOUT_OFF, 1);
    wr32(vh + REFS_EZ_BITMAP_OFF, RS_FMT_BITMAP_LBA);
    wr32(vh + REFS_EZ_META_OFF, RS_FMT_META_LBA);
    wr32(vh + REFS_EZ_META_N_OFF, RS_FMT_META_N);
    wr32(vh + REFS_EZ_DATA_OFF, RS_FMT_DATA_LBA);
    wr32(vh + REFS_EZ_DCLUST_OFF, data_clusters);
    if (ata_write_sector(drive, part_start, vh) != 0) return -1;

    /* 位图区清零（LBA 8..15）+ meta 区清零（LBA 16..143） */
    static uint8_t zero[512] RS_HIBUF;
    for (int i = 0; i < 512; i++) zero[i] = 0;
    for (uint32_t lba = RS_FMT_BITMAP_LBA;
         lba < RS_FMT_DATA_LBA; lba++)
        if (ata_write_sector(drive, part_start + lba, zero) != 0) return -1;

    /* 挂载验证 */
    if (refs_mount(drive, part_start) != 0) return -1;
    return 0;
}
