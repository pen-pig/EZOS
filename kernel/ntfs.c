/*
 * ntfs.c - NTFS 只读驱动
 *
 * 支持范围（只读）：
 *   - BPB 解析（512B 扇区；簇大小 2^n 扇区）
 *   - MFT 记录读取（fixup 序列恢复）与 $MFT 自身 run list 定位
 *   - 属性遍历：resident / non-resident $DATA，run list 逐扇区映射
 *   - 目录：$INDEX_ROOT（驻留）+ $INDEX_ALLOCATION/$BITMAP（INDX 块）
 *   - $FILE_NAME（UTF-16LE）目录项，跳过 DOS 命名空间重复项
 *   - $Bitmap popcount 提供 df 已用簇数
 *   - 不支持：压缩/加密属性、$ATTRIBUTE_LIST 扩展、符号链接
 *
 * refs:
 *   - GRUB grub-core/fs/ntfs.c (GPLv3+) - fixup()/read_run_data()/
 *     grub_ntfs_read_run_list()/list_file()/grub_ntfs_iterate_dir() 逻辑
 *   - include/grub/ntfs.h - BPB/属性类型/文件属性常量
 *   - https://flatcap.github.io/linux-ntfs/ntfs/ - 磁盘结构文档
 *     (file record header / resident attribute / $INDEX_ROOT / $FILE_NAME)
 */
#include "ntfs.h"
#include "ata.h"

/* ---------- 小端读取助手 ---------- */
static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

/* ---------- 常量（GRUB ntfs.h） ---------- */
#define NT_AT_STANDARD_INFO   0x10u
#define NT_AT_FILENAME        0x30u
#define NT_AT_DATA            0x80u
#define NT_AT_INDEX_ROOT      0x90u
#define NT_AT_INDEX_ALLOC     0xA0u
#define NT_AT_BITMAP          0xB0u

#define NT_ATTR_DIRECTORY     0x10000000u   /* $FILE_NAME flags */

#define NT_MAX_REC    4096                  /* MFT 记录上限 */
#define NT_MAX_IDX    4096                  /* INDX 块上限 */
#define NT_BITMAP_CAP 8192                  /* $Bitmap 读取上限 */
#define NT_ROOT_FILE  5                     /* 根目录 MFT 记录号 */
#define NT_BITMAP_FILE 6                    /* $Bitmap MFT 记录号 */

/* 簇号不存在（sparse / 越界）哨兵 */
#define NT_NO_LCN    0xFFFFFFFFFFFFFFFFull

/* ---------- 挂载状态 ---------- */
static uint8_t  nt_drive;
static uint8_t  nt_mounted;
static uint32_t nt_part_lba;
static uint32_t nt_spc;               /* 每簇扇区数 */
static uint32_t nt_cluster_bytes;
static uint32_t nt_rec_bytes;         /* MFT 记录字节数 */
static uint32_t nt_idx_bytes;         /* INDX 块字节数 */
static uint32_t nt_mft_run_off;       /* $MFT $DATA run list 在 nt_mmft 内偏移 */
static uint32_t nt_vol_sectors;
static uint32_t nt_total_clusters;
static ntfs_info_t nt_info;

/* $MFT 记录 0 挂载后只读；nt_rec 为当前解析记录；nt_child 供目录枚举
 * 读取子记录（不破坏 nt_rec 中的 INDEX_ROOT）；nt_indx 为 INDX 块。 */
/* 大缓冲放高内存段 .bss.hi（1MB+，见 linker.ld）：低 640KB 区留给栈/小数据 */
#define NT_HIBUF __attribute__((section(".bss.hi")))
static uint8_t nt_mmft[NT_MAX_REC] NT_HIBUF;
static uint8_t nt_rec[NT_MAX_REC] NT_HIBUF;
static uint8_t nt_child[NT_MAX_REC] NT_HIBUF;
static uint8_t nt_indx[NT_MAX_IDX] NT_HIBUF;
static uint8_t nt_bitmap[NT_BITMAP_CAP] NT_HIBUF;

static int nt_streq4(const uint8_t *p, const char *s) {
    return p[0] == (uint8_t)s[0] && p[1] == (uint8_t)s[1] &&
           p[2] == (uint8_t)s[2] && p[3] == (uint8_t)s[3];
}

static int nt_is_pow2(uint32_t v) {
    return v && (v & (v - 1)) == 0;
}

/* ---------- fixup：多扇区头部序列号恢复（GRUB fixup()） ---------- */
static int nt_fixup(uint8_t *buf, uint32_t bytes, const char *magic) {
    if (!nt_streq4(buf, magic)) return -1;
    uint32_t cnt = rd16(buf + 6);                 /* = 扇区数 + 1 */
    if (cnt == 0 || (cnt - 1) * 512 != bytes) return -1;
    const uint8_t *pu = buf + rd16(buf + 4);      /* fixup 数组 */
    uint16_t us = rd16(pu);
    for (uint32_t i = 0; i < cnt - 1; i++) {
        uint8_t *tail = buf + i * 512 + 510;
        if (rd16(tail) != us) return -1;
        tail[0] = pu[2 + i * 2];
        tail[1] = pu[3 + i * 2];
    }
    return 0;
}

/* ---------- run list（GRUB grub_ntfs_read_run_list） ---------- */
static uint64_t nt_run_read(const uint8_t *run, int nn, int sig) {
    uint64_t r = 0;
    if (sig && nn && (run[nn - 1] & 0x80))
        r = (uint64_t)-1;                         /* 符号扩展 */
    for (int i = nn - 1; i >= 0; i--)
        r = (r << 8) | run[i];
    return r;
}

/* 在 run list 中查 vcn；返回 LCN，sparse/未覆盖返回 NT_NO_LCN */
static uint64_t nt_run_map(const uint8_t *runlist, uint32_t vcn) {
    const uint8_t *run = runlist;
    uint64_t curr_vcn = 0, lcn = 0;
    for (;;) {
        uint8_t c1 = (*run) & 0x7;
        uint8_t c2 = ((*run) >> 4) & 0x7;
        if (c1 == 0) return NT_NO_LCN;            /* run list 结束 */
        run++;
        uint64_t len = nt_run_read(run, c1, 0);
        run += c1;
        uint64_t off = nt_run_read(run, c2, 1);   /* 有符号 LCN 增量 */
        run += c2;
        if (curr_vcn + len > curr_vcn &&          /* 防溢出 */
            vcn >= curr_vcn && vcn < curr_vcn + len) {
            if (off == 0) return NT_NO_LCN;       /* sparse run */
            return lcn + off;
        }
        curr_vcn += len;
        lcn += off;
        if (curr_vcn > 0x100000000ull) return NT_NO_LCN;
    }
}

/* 读取非常驻属性 a 前 max_len 字节到 buf（sparse 段置零）；返回读取字节数 */
static uint32_t nt_read_nres(const uint8_t *a, uint8_t *buf, uint32_t max_len) {
    const uint8_t *run = a + rd16(a + 0x20);
    static uint8_t sec[512];
    uint32_t done = 0;
    while (done < max_len) {
        uint32_t vcn = done / nt_cluster_bytes;
        uint32_t in = done % nt_cluster_bytes;
        uint64_t lcn = nt_run_map(run, vcn);
        uint32_t chunk = 512 - in % 512;
        if (chunk > max_len - done) chunk = max_len - done;
        if (lcn == NT_NO_LCN || lcn > 0x0FFFFFFFull) {
            for (uint32_t i = 0; i < chunk; i++) buf[done + i] = 0;
        } else {
            uint32_t lba = nt_part_lba + (uint32_t)lcn * nt_spc + in / 512;
            if (in % 512 == 0 && chunk == 512) {
                if (ata_read_sector(nt_drive, lba, buf + done) != 0) break;
            } else {
                if (ata_read_sector(nt_drive, lba, sec) != 0) break;
                for (uint32_t i = 0; i < chunk; i++)
                    buf[done + i] = sec[(in % 512) + i];
            }
        }
        done += chunk;
    }
    return done;
}

/* ---------- 属性遍历 ---------- */

/* 在 MFT 记录 rec 中找类型 type 且名字为 name（name==0 要求未命名）的属性。
 * 返回属性头指针；未找到返回 0。 */
static const uint8_t *nt_find_attr(const uint8_t *rec, uint32_t type, const char *name) {
    uint32_t off = rd16(rec + 0x14);              /* 首属性偏移 */
    uint32_t end = rd32(rec + 0x18);              /* 已用大小 */
    while (off + 16 <= end && off + 16 <= NT_MAX_REC) {
        const uint8_t *a = rec + off;
        uint32_t t = rd32(a);
        uint32_t len = rd32(a + 4);
        if (t == 0xFFFFFFFFu) return 0;           /* $END */
        if (len < 16 || off + len > end) return 0;
        if (t == type) {
            uint8_t name_len = a[9];
            if (name == 0 && name_len == 0) return a;
            if (name != 0 && name_len > 0) {
                uint32_t noff = rd16(a + 0xA);
                const uint8_t *np = a + noff;
                const char *q = name;
                int same = 1;
                for (uint32_t i = 0; i < name_len; i++) {
                    uint16_t c = rd16(np + i * 2);
                    if ((char)c != *q) { same = 0; break; }
                    q++;
                }
                if (same && *q == 0) return a;
            }
        }
        off += len;
    }
    return 0;
}

/* MFT 记录号 -> 记录缓冲（经 $MFT run list 映射 + fixup）。
 * 挂载流程在 nt_mounted 置位前也会调用，此处不检查挂载标志。 */
static int nt_read_record(uint32_t mftno, uint8_t *buf) {
    uint32_t rec_secs = nt_rec_bytes / 512;
    for (uint32_t s = 0; s < rec_secs; s++) {
        uint64_t b = (uint64_t)mftno * nt_rec_bytes + (uint64_t)s * 512;
        uint32_t vcn = (uint32_t)(b / nt_cluster_bytes);
        uint32_t in = (uint32_t)(b % nt_cluster_bytes);
        uint64_t lcn = nt_run_map(nt_mmft + nt_mft_run_off, vcn);
        if (lcn == NT_NO_LCN || lcn > 0x0FFFFFFFull) return -1;
        if (ata_read_sector(nt_drive, nt_part_lba + (uint32_t)lcn * nt_spc + in / 512,
                            buf + s * 512) != 0) return -1;
    }
    return nt_fixup(buf, nt_rec_bytes, "FILE");
}

/* 记录 flags（0x16）：bit0=使用中 bit1=目录 */
static int nt_rec_is_dir(const uint8_t *rec) {
    return rd16(rec + 0x16) & 2;
}

/* ---------- UTF-16 -> ASCII（截断转写） ---------- */
static void nt_utf16_to_ascii(const uint8_t *src, uint32_t nchars, char *dst, uint32_t dstmax) {
    uint32_t i;
    if (nchars > dstmax - 1) nchars = dstmax - 1;
    for (i = 0; i < nchars; i++) {
        uint16_t c = rd16(src + i * 2);
        dst[i] = (c < 128) ? (char)c : '?';
    }
    dst[i] = 0;
}

static char nt_lower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

static int nt_name_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (nt_lower(*a) != nt_lower(*b)) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

/* ---------- 目录遍历 ---------- */
typedef int (*nt_dir_cb)(const char *name, uint32_t ref, int is_dir, void *ctx);

/* 遍历一段 INDEX entries（INDEX_ROOT 驻留内容或 INDX 块内）。
 * GRUB list_file()：entry+0x50 name_len，+0x51 命名空间，+0x48 文件属性。 */
static int nt_walk_entries(const uint8_t *pos, nt_dir_cb cb, void *ctx) {
    while (1) {
        uint16_t e_len = rd16(pos + 8);
        uint16_t e_flags = rd16(pos + 0xC);
        if (e_len < 0x10) return -1;
        if (e_flags & 2) return 0;                /* 最后一个条目 */
        uint8_t ns = pos[0x50];
        uint8_t ns_kind = pos[0x51];
        if (ns > 0 && ns_kind != 2) {             /* 跳过 DOS 命名空间 */
            char name[256];
            nt_utf16_to_ascii(pos + 0x52, ns, name, sizeof(name));
            uint32_t ref = (uint32_t)(rd64(pos) & 0xFFFFFFFFFFFFull);
            int is_dir = (rd32(pos + 0x48) & NT_ATTR_DIRECTORY) != 0;
            if (cb(name, ref, is_dir, ctx)) return 0;
        }
        pos += e_len;
    }
}

/* 读 $I30 $BITMAP（驻留或非常驻），返回字节数；失败返回 0 */
static uint32_t nt_load_bitmap(const uint8_t *dirrec) {
    const uint8_t *a = nt_find_attr(dirrec, NT_AT_BITMAP, "$I30");
    if (a == 0) return 0;
    if (a[8] == 0) {                              /* resident */
        uint32_t len = rd32(a + 0x10);
        const uint8_t *src = a + rd16(a + 0x14);
        if (len > NT_BITMAP_CAP) len = NT_BITMAP_CAP;
        for (uint32_t i = 0; i < len; i++) nt_bitmap[i] = src[i];
        return len;
    }
    uint32_t len = (uint32_t)rd64(a + 0x28);      /* alloc size */
    if (len > NT_BITMAP_CAP) len = NT_BITMAP_CAP;
    return nt_read_nres(a, nt_bitmap, len);
}

/* 遍历目录（记录 dirrec）：INDEX_ROOT + INDEX_ALLOCATION */
static int nt_dir_walk(const uint8_t *dirrec, nt_dir_cb cb, void *ctx) {
    const uint8_t *root = nt_find_attr(dirrec, NT_AT_INDEX_ROOT, "$I30");
    if (root == 0 || root[8] != 0) return -1;     /* 必须驻留 */
    const uint8_t *ir = root + rd16(root + 0x14); /* INDEX_ROOT 内容 */
    if (rd32(ir) != NT_AT_FILENAME) return -1;    /* 非 $FILE_NAME 索引 */
    const uint8_t *ih = ir + 0x10;                /* INDEX_HEADER */
    int ret = nt_walk_entries(ih + rd32(ih), cb, ctx);
    if (ret != 0) return ret;

    /* 大目录：$INDEX_ALLOCATION + $BITMAP（GRUB grub_ntfs_iterate_dir） */
    const uint8_t *alloc = nt_find_attr(dirrec, NT_AT_INDEX_ALLOC, "$I30");
    if (alloc == 0) return 0;                     /* 小目录：仅 INDEX_ROOT */
    if (alloc[8] == 0) return -1;                 /* 分配属性必须非常驻 */

    uint32_t bmp_len = nt_load_bitmap(dirrec);
    if (bmp_len == 0) return -1;                  /* $BITMAP 缺失 */

    const uint8_t *run = alloc + rd16(alloc + 0x20);
    uint32_t idx_secs = nt_idx_bytes / 512;
    uint32_t idx_clu_bytes = nt_idx_bytes;        /* INDX 块按字节对齐簇边界 */
    for (uint32_t byte = 0; byte < bmp_len; byte++) {
        uint8_t bits = nt_bitmap[byte];
        if (bits == 0) continue;
        for (int bit = 0; bit < 8; bit++) {
            if (!(bits & (1u << bit))) continue;
            uint32_t blk = byte * 8 + bit;
            /* INDX 块 i 的 VCN = i * idx_bytes / cluster_bytes */
            uint64_t b = (uint64_t)blk * idx_clu_bytes;
            uint32_t vcn = (uint32_t)(b / nt_cluster_bytes);
            uint32_t in = (uint32_t)(b % nt_cluster_bytes);
            uint64_t lcn = nt_run_map(run, vcn);
            if (lcn == NT_NO_LCN || lcn > 0x0FFFFFFFull) return -1;
            uint32_t lba = nt_part_lba + (uint32_t)lcn * nt_spc + in / 512;
            for (uint32_t s = 0; s < idx_secs; s++) {
                if (ata_read_sector(nt_drive, lba + s, nt_indx + s * 512) != 0)
                    return -1;
            }
            if (nt_fixup(nt_indx, nt_idx_bytes, "INDX") != 0) return -1;
            /* INDX: magic(4)+lsn(8)+vcn(8)，INDEX_HEADER@0x18 */
            const uint8_t *ih2 = nt_indx + 0x18;
            ret = nt_walk_entries(ih2 + rd32(ih2), cb, ctx);
            if (ret != 0) return ret;
        }
    }
    return 0;
}

/* ---------- 路径解析 ---------- */
typedef struct {
    const char *name;      /* 待查组件 */
    uint32_t ref;
    int      is_dir;
    int      found;
} nt_lookup_ctx;

static int nt_lookup_cb(const char *name, uint32_t ref, int is_dir, void *ctx) {
    nt_lookup_ctx *c = (nt_lookup_ctx *)ctx;
    if (nt_name_eq(name, c->name)) {
        c->ref = ref;
        c->is_dir = is_dir;
        c->found = 1;
        return 1;
    }
    return 0;
}

/* 解析绝对路径 -> MFT 记录读入 nt_rec。
 * 成功返回记录号；失败返回 0。*is_dir_out / *size_out 可空。 */
static uint32_t nt_resolve(const char *path, int *is_dir_out, uint32_t *size_out) {
    if (!nt_mounted || path == 0 || path[0] != '/') return 0;
    if (nt_read_record(NT_ROOT_FILE, nt_rec) != 0) return 0;

    while (*path == '/') path++;
    while (*path) {
        const char *comp = path;
        uint32_t clen = 0;
        while (path[clen] && path[clen] != '/') clen++;
        if (clen == 0 || clen > 255) return 0;
        char compbuf[256];
        for (uint32_t i = 0; i < clen; i++) compbuf[i] = comp[i];
        compbuf[clen] = 0;

        nt_lookup_ctx ctx;
        ctx.name = compbuf;
        ctx.found = 0;
        if (nt_dir_walk(nt_rec, nt_lookup_cb, &ctx) != 0 || !ctx.found) return 0;

        if (nt_read_record(ctx.ref, nt_rec) != 0) return 0;
        if (!(rd16(nt_rec + 0x16) & 1)) return 0;  /* 记录未使用 */
        path += clen;
        while (*path == '/') path++;
    }

    if (is_dir_out) *is_dir_out = nt_rec_is_dir(nt_rec);
    if (size_out) {
        *size_out = 0;
        const uint8_t *a = nt_find_attr(nt_rec, NT_AT_DATA, 0);
        if (a) {
            if (a[8] == 0) *size_out = rd32(a + 0x10);
            else *size_out = (uint32_t)rd64(a + 0x30);
        }
    }
    return 1;
}

/* ---------- 对外 API ---------- */
int ntfs_is_dir(const char *path) {
    int is_dir;
    if (nt_resolve(path, &is_dir, 0) == 0) return -1;
    return is_dir;
}

uint32_t ntfs_get_file_size(const char *path) {
    uint32_t size;
    if (nt_resolve(path, 0, &size) == 0) return 0;
    return size;
}

int ntfs_read_file(const char *path, uint8_t *buffer, uint32_t max_size) {
    int is_dir;
    if (nt_resolve(path, &is_dir, 0) == 0 || is_dir) return -1;
    const uint8_t *a = nt_find_attr(nt_rec, NT_AT_DATA, 0);
    if (a == 0) return -1;
    if (a[0xC] & 0x0001) return -1;               /* 压缩属性 */
    if (a[0xC] & 0x4000) return -1;               /* 加密属性 */

    if (a[8] == 0) {                              /* resident $DATA */
        uint32_t len = rd32(a + 0x10);
        const uint8_t *src = a + rd16(a + 0x14);
        if (len > max_size) len = max_size;
        for (uint32_t i = 0; i < len; i++) buffer[i] = src[i];
        return (int)len;
    }

    /* non-resident：run list 逐扇区读，sparse 置零 */
    uint32_t size = (uint32_t)rd64(a + 0x30);
    if (size > max_size) size = max_size;
    if (nt_read_nres(a, buffer, size) != size) return -1;
    return (int)size;
}

typedef struct {
    fs_dir_entry_t *entries;
    int max, n;
} nt_fill_ctx;

static int nt_fill_cb(const char *name, uint32_t ref, int is_dir, void *ctx) {
    nt_fill_ctx *c = (nt_fill_ctx *)ctx;
    if (c->n >= c->max) return 1;
    fs_dir_entry_t *e = &c->entries[c->n++];
    uint32_t i = 0;
    while (name[i] && i < 255) { e->name[i] = name[i]; i++; }
    e->name[i] = 0;
    e->is_dir = (uint8_t)(is_dir ? 1 : 0);
    e->size = 0;
    /* 读子记录取大小（nt_child 与 nt_rec/nt_indx 隔离） */
    if (!is_dir && nt_read_record(ref, nt_child) == 0) {
        const uint8_t *a = nt_find_attr(nt_child, NT_AT_DATA, 0);
        if (a) {
            if (a[8] == 0) e->size = rd32(a + 0x10);
            else e->size = (uint32_t)rd64(a + 0x30);
        }
    }
    return 0;
}

int ntfs_read_dir(const char *path, fs_dir_entry_t *entries, int max_entries) {
    int is_dir;
    if (nt_resolve(path, &is_dir, 0) == 0 || !is_dir) return -1;
    nt_fill_ctx ctx;
    ctx.entries = entries;
    ctx.max = max_entries;
    ctx.n = 0;
    if (nt_dir_walk(nt_rec, nt_fill_cb, &ctx) != 0) return -1;
    return ctx.n;
}

uint32_t ntfs_get_file_clusters(const char *path) {
    int is_dir;
    if (nt_resolve(path, &is_dir, 0) == 0) return 0;
    const uint8_t *a = nt_find_attr(nt_rec, NT_AT_DATA, 0);
    if (a == 0) return 0;
    if (a[8] != 0) return (uint32_t)(rd64(a + 0x28) / nt_cluster_bytes);
    uint32_t size = rd32(a + 0x10);
    return (size + nt_cluster_bytes - 1) / nt_cluster_bytes;
}

/* ---------- 挂载 ---------- */
int ntfs_mount(uint8_t drive, uint32_t part_start) {
    uint8_t sec[512];
    nt_mounted = 0;
    if (ata_read_sector(drive, part_start, sec) != 0) return -1;

    /* BPB（GRUB grub_ntfs_mount） */
    if (!nt_streq4(sec + 3, "NTFS")) return -1;
    if (rd16(sec + 0x0B) != 512) return -1;       /* 驱动按 512B 扇区读 */
    uint32_t spc = sec[0x0D];
    if (spc == 0 || !nt_is_pow2(spc)) return -1;
    uint64_t total_secs = rd64(sec + 0x28);
    uint64_t mft_lcn = rd64(sec + 0x30);
    int8_t cpm = (int8_t)sec[0x40];
    int8_t cpi = (int8_t)sec[0x41];

    uint32_t rec_secs, idx_secs;
    if (cpm > 0) rec_secs = (uint32_t)cpm * spc;
    else if (cpm < -9 && cpm > -31) rec_secs = 1u << (-cpm - 9);
    else return -1;
    if (rec_secs * 512 < 512 || rec_secs * 512 > NT_MAX_REC) return -1;

    if (cpi > 0) idx_secs = (uint32_t)cpi * spc;
    else if (cpi < -9 && cpi > -31) idx_secs = 1u << (-cpi - 9);
    else return -1;
    if (idx_secs * 512 < 512 || idx_secs * 512 > NT_MAX_IDX) return -1;

    if (mft_lcn > 0x0FFFFFFFull || total_secs < 64 || total_secs > 0x0FFFFFFFull)
        return -1;

    nt_drive = drive;
    nt_part_lba = part_start;
    nt_spc = spc;
    nt_cluster_bytes = spc * 512;
    nt_rec_bytes = rec_secs * 512;
    nt_idx_bytes = idx_secs * 512;
    nt_vol_sectors = (uint32_t)total_secs;
    nt_total_clusters = (uint32_t)(total_secs / spc);

    /* $MFT 记录 0：直接位于 mft_lcn（GRUB 同样处理） */
    uint32_t mft_lba = part_start + (uint32_t)mft_lcn * spc;
    for (uint32_t s = 0; s < rec_secs; s++)
        if (ata_read_sector(drive, mft_lba + s, nt_mmft + s * 512) != 0) return -1;
    if (nt_fixup(nt_mmft, nt_rec_bytes, "FILE") != 0) return -1;

    /* $MFT 自身 $DATA run list（非常驻） */
    const uint8_t *a = nt_find_attr(nt_mmft, NT_AT_DATA, 0);
    if (a == 0 || a[8] == 0) return -1;
    if (a[0xC] & 0x0001) return -1;               /* 压缩 MFT 不支持 */
    nt_mft_run_off = rd16(a + 0x20);
    if (nt_mft_run_off + 8 > nt_rec_bytes) return -1;

    /* 根目录记录 5 可读且在使用 */
    if (nt_read_record(NT_ROOT_FILE, nt_rec) != 0) return -1;
    if (!(rd16(nt_rec + 0x16) & 1) || !nt_rec_is_dir(nt_rec)) return -1;

    nt_mounted = 1;

    /* df：$Bitmap（$Bitmap 文件的 $DATA）popcount；读失败按 0 处理 */
    uint32_t used = 0;
    if (nt_read_record(NT_BITMAP_FILE, nt_rec) == 0) {
        const uint8_t *b = nt_find_attr(nt_rec, NT_AT_DATA, 0);
        if (b && !(b[0xC] & 0x0001)) {
            uint32_t bmp_bytes = (nt_total_clusters + 7) / 8;
            if (bmp_bytes > NT_BITMAP_CAP) bmp_bytes = NT_BITMAP_CAP;
            uint32_t got = 0;
            if (b[8] == 0) {                      /* resident */
                uint32_t len = rd32(b + 0x10);
                const uint8_t *src = b + rd16(b + 0x14);
                if (len > bmp_bytes) len = bmp_bytes;
                for (uint32_t i = 0; i < len; i++) nt_bitmap[i] = src[i];
                got = len;
            } else {
                got = nt_read_nres(b, nt_bitmap, bmp_bytes);
            }
            for (uint32_t i = 0; i < got; i++) {
                uint8_t bits = nt_bitmap[i];
                for (int bit = 0; bit < 8; bit++)
                    if (bits & (1u << bit)) used++;
            }
        }
    }

    nt_info.part_start = part_start;
    nt_info.bytes_per_sector = 512;
    nt_info.sectors_per_cluster = (uint8_t)spc;
    nt_info.cluster_count = nt_total_clusters;
    nt_info.volume_sectors = nt_vol_sectors;
    nt_info.used_clusters = used;
    return 0;
}

const ntfs_info_t *ntfs_get_info(void) { return &nt_info; }
