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
#include "kmalloc.h"

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

/*
 * 内核静态簇数据缓冲的上限（nt_cbuf）。
 * 卷的簇字节数**必须** <= 这个值，否则读写簇会越过 nt_cbuf 踩坏相邻内核数据。
 * 它是独立于 NT_MAX_IDX 的一个不变式：虽然当前两者同值（nt_write_file 那条
 * 路径要求 INDX 块 == 簇），但语义不同，别让将来有人改了 NT_MAX_IDX 就悄悄
 * 打破 nt_cbuf 的容量假设。
 */
#define NT_MAX_CLUSTER_BYTES 4096
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
            /* LCN = run 起点 + run 内簇偏移（GRUB 同：vcn 映射到
             * run_start_lcn + (vcn - run_start_vcn)，非仅 run 起点） */
            return lcn + off + (vcn - curr_vcn);
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
 * GRUB list_file()：entry+0x50 name_len，+0x51 命名空间，+0x48 文件属性。
 * B 树内部条目（flag bit0=带子节点 VCN）的 key 与叶子条目重复，
 * 枚举时跳过（Windows/ntfs-3g 大目录同样适用，避免重复列举）。 */
static int nt_walk_entries(const uint8_t *pos, nt_dir_cb cb, void *ctx) {
    while (1) {
        uint16_t e_len = rd16(pos + 8);
        uint16_t e_flags = rd16(pos + 0xC);
        if (e_len < 0x10) return -1;
        if (e_flags & 2) return 0;                /* 最后一个条目 */
        if (e_flags & 1) { pos += e_len; continue; } /* 内部条目：跳过 */
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
    /* 契约是 1=目录 / 0=文件 / -1=不存在。
     * nt_rec_is_dir 返回的是**原始旗位**（rd16(rec+0x16) & 2 == 2），
     * 直接透传会让 ro_is_dir 的调用方（fs_change_dir 里 `!= 1` 的判断）
     * 把目录一律当成"不是目录"，NTFS 下 cd 子目录永远失败。
     * 所以这里必须规范化成 0/1。 */
    return is_dir ? 1 : 0;
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
    /* 簇大小上界：spc 是 uint8_t 且只校验了"非零、2 的幂"，最大能到 128，
     * 于是 spc*512 可达 64KB——远超 nt_cbuf 的 NT_MAX_CLUSTER_BYTES。
     * 镜像是外部不可信输入，必须在 probe 阶段就拒绝，否则后面
     * `for (i < nt_cluster_bytes) nt_cbuf[i] = 0` 会越过静态缓冲写约 28KB，
     * 踩坏 .bss.hi 里相邻的内核数据（这条本项目已经踩过一次同类坑）。
     * Fail closed：明确拒绝挂载，好过静默破坏内核。 */
    if (spc * 512 > NT_MAX_CLUSTER_BYTES) return -1;
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
    /* run list 偏移在属性头内是属性相对值（GRUB pa + u16at(pa,0x20)），
     * 本驱动统一存记录内绝对偏移 */
    nt_mft_run_off = (uint32_t)(a - nt_mmft) + rd16(a + 0x20);
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

/* ============================================================
 * 写入支持（mkntfs / ntfs-3g 标准结构）
 * refs: ntfs-3g ntfsprogs/mkntfs.c（记录/属性构造布局）与
 *       GRUB fixup()/read_run_data()（反向写回）
 *
 * 设计（与 mkntfs 一致，Windows/ntfs-3g 可读）：
 *   - $Bitmap（记录 6）$DATA = 卷簇位图（bit i = LCN i，无前缀）
 *   - MFT 记录位图 = $MFT（记录 0）驻留 $BITMAP 属性（0xB0）
 *   - 记录写回：fixup 反向应用（USN 自增，扇区尾字存回 fixup 数组）
 *   - 新文件：$STANDARD_INFORMATION + $FILE_NAME + $DATA
 *     （size<=700B 驻留；更大非驻留，多 run 变长编码 runlist，
 *      run 间不要求连续，簇从位图顺序分配）
 *   - 目录：小目录 $INDEX_ROOT 驻留插删；放不下时升级为
 *     $INDEX_ALLOCATION（INDX 叶块）+ $I30 $BITMAP，
 *     块内按名排序、根节点带子节点 VCN（标准 B+ 叶层布局），
 *     叶块满则分裂（根容量 4 个分隔条目 => 每目录 ~150 文件）
 * ============================================================ */

static void nt_wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF); p[1] = (uint8_t)(v >> 8);
}
static void nt_wr32(uint8_t *p, uint32_t v) {
    nt_wr16(p, (uint16_t)v); nt_wr16(p + 2, (uint16_t)(v >> 16));
}
static void nt_wr64(uint8_t *p, uint64_t v) {
    nt_wr32(p, (uint32_t)v); nt_wr32(p + 4, (uint32_t)(v >> 32));
}

/* MFT 记录写回缓冲 / 簇数据缓冲 */
static uint8_t nt_newrec[NT_MAX_REC] NT_HIBUF;
static uint8_t nt_cbuf[NT_MAX_CLUSTER_BYTES] NT_HIBUF;
/* 卷簇位图（记录 6 $DATA，bit i = LCN i） */
static uint8_t nt_vbmp[NT_BITMAP_CAP] NT_HIBUF;
static uint32_t nt_vbmp_len;
/* MFT 记录位图（$MFT 记录 0 驻留 $BITMAP 属性，bit i = 记录 i）
 * 缓冲 8 字节 = 64 记录（本驱动写入模型上限） */
#define NT_MFT_REC_MAX 64
static uint8_t nt_mbmp[NT_MFT_REC_MAX / 8] NT_HIBUF;
static uint32_t nt_usn = 3;         /* fixup 序列号 */

/* fixup 反向应用 + 经 $MFT run list 写回记录 */
static int nt_write_record(uint32_t mftno, uint8_t *buf) {
    if (!nt_streq4(buf, "FILE")) return -1;
    uint32_t cnt = rd16(buf + 6);
    if (cnt == 0 || (cnt - 1) * 512 != nt_rec_bytes) return -1;
    uint8_t *pu = buf + rd16(buf + 4);
    nt_usn++;
    pu[0] = (uint8_t)(nt_usn & 0xFF);
    pu[1] = (uint8_t)((nt_usn >> 8) & 0xFF);
    for (uint32_t i = 0; i < cnt - 1; i++) {
        uint8_t *tail = buf + i * 512 + 510;
        pu[2 + i * 2] = tail[0];
        pu[3 + i * 2] = tail[1];
        tail[0] = pu[0];
        tail[1] = pu[1];
    }
    uint32_t rec_secs = nt_rec_bytes / 512;
    for (uint32_t s = 0; s < rec_secs; s++) {
        uint64_t b = (uint64_t)mftno * nt_rec_bytes + (uint64_t)s * 512;
        uint32_t vcn = (uint32_t)(b / nt_cluster_bytes);
        uint32_t in = (uint32_t)(b % nt_cluster_bytes);
        uint64_t lcn = nt_run_map(nt_mmft + nt_mft_run_off, vcn);
        if (lcn == NT_NO_LCN || lcn > 0x0FFFFFFFull) return -1;
        if (ata_write_sector(nt_drive,
                             nt_part_lba + (uint32_t)lcn * nt_spc + in / 512,
                             buf + s * 512) != 0) return -1;
    }
    return 0;
}

/* ---------- 两张卷位图（mkntfs 标准布局） ----------
 * 簇位图：$Bitmap（记录 6）驻留 $DATA，bit i = LCN i；
 * MFT 记录位图：$MFT（记录 0）驻留 $BITMAP 属性，bit i = 记录 i */

/* 载入两张位图：簇位图（记录 6 $DATA）+ MFT 记录位图（记录 0 $BITMAP） */
static int nt_maps_load(void) {
    /* 簇位图：$Bitmap 记录 6 驻留 $DATA（真实大卷可能非常驻，驻留足够
     * 本驱动 2MB 模型；非常驻时按 512 字节前缀截断不支持） */
    if (nt_read_record(NT_BITMAP_FILE, nt_child) != 0) return -1;
    const uint8_t *a = nt_find_attr(nt_child, NT_AT_DATA, 0);
    if (a == 0 || a[8] != 0) return -1;        /* 仅支持驻留 */
    uint32_t len = rd32(a + 0x10);
    if (len > NT_BITMAP_CAP) len = NT_BITMAP_CAP;
    const uint8_t *src = a + rd16(a + 0x14);
    for (uint32_t i = 0; i < len; i++) nt_vbmp[i] = src[i];
    nt_vbmp_len = len;

    /* MFT 记录位图：$MFT 记录 0 的驻留 $BITMAP */
    if (nt_read_record(0, nt_child) != 0) return -1;
    a = nt_find_attr(nt_child, NT_AT_BITMAP, 0);
    if (a == 0 || a[8] != 0) return -1;
    len = rd32(a + 0x10);
    if (len > sizeof(nt_mbmp)) len = sizeof(nt_mbmp);
    src = a + rd16(a + 0x14);
    for (uint32_t i = 0; i < len; i++) nt_mbmp[i] = src[i];
    for (uint32_t i = len; i < sizeof(nt_mbmp); i++) nt_mbmp[i] = 0;
    return 0;
}

/* 写回两张位图 */
static int nt_maps_save(void) {
    /* 簇位图 -> 记录 6 */
    if (nt_read_record(NT_BITMAP_FILE, nt_child) != 0) return -1;
    const uint8_t *a = nt_find_attr(nt_child, NT_AT_DATA, 0);
    if (a == 0 || a[8] != 0) return -1;
    uint32_t off = (uint32_t)(a - nt_child) + rd16(a + 0x14);
    uint32_t len = rd32(a + 0x10);
    if (len > nt_vbmp_len) len = nt_vbmp_len;
    for (uint32_t i = 0; i < len; i++) nt_child[off + i] = nt_vbmp[i];
    if (nt_write_record(NT_BITMAP_FILE, nt_child) != 0) return -1;

    /* MFT 记录位图 -> 记录 0 $BITMAP（重读记录 0，别动已写回的记录 6） */
    if (nt_read_record(0, nt_child) != 0) return -1;
    a = nt_find_attr(nt_child, NT_AT_BITMAP, 0);
    if (a == 0 || a[8] != 0) return -1;
    off = (uint32_t)(a - nt_child) + rd16(a + 0x14);
    len = rd32(a + 0x10);
    if (len > sizeof(nt_mbmp)) len = sizeof(nt_mbmp);
    for (uint32_t i = 0; i < len; i++) nt_child[off + i] = nt_mbmp[i];
    if (nt_write_record(0, nt_child) != 0) return -1;
    return 0;
}

/* 分配一个 MFT 记录号；返回记录号，失败返回 0xFFFFFFFF */
static uint32_t nt_alloc_mft(void) {
    for (uint32_t i = 12; i < NT_MFT_REC_MAX; i++) {
        uint32_t byte = i / 8, bit = i % 8;
        if (byte >= sizeof(nt_mbmp)) break;
        if (!(nt_mbmp[byte] & (1u << bit))) {
            nt_mbmp[byte] |= (1u << bit);
            return i;
        }
    }
    return 0xFFFFFFFFu;
}

static void nt_free_mft(uint32_t mftno) {
    if (mftno < NT_MFT_REC_MAX)
        nt_mbmp[mftno / 8] &= ~(1u << (mftno % 8));
}

/* 分配 n 个连续簇（起始簇号返回），失败返回 0xFFFFFFFF */
static uint32_t nt_alloc_clusters(uint32_t n) {
    if (n == 0 || n > 4096) return 0xFFFFFFFFu;
    uint32_t total_bits = nt_vbmp_len * 8;
    for (uint32_t start = 0; start + n <= total_bits; start++) {
        int ok = 1;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t bit = start + i;
            if (nt_vbmp[bit / 8] & (1u << (bit % 8))) { ok = 0; start = bit; break; }
        }
        if (ok) {
            for (uint32_t i = 0; i < n; i++) {
                uint32_t bit = start + i;
                nt_vbmp[bit / 8] |= (1u << (bit % 8));
            }
            return start;
        }
    }
    return 0xFFFFFFFFu;
}

static void nt_free_cluster(uint32_t lcn) {
    uint32_t byte = lcn / 8;
    if (byte < nt_vbmp_len)
        nt_vbmp[byte] &= ~(1u << (lcn % 8));
}

/* ---------- $FILE_NAME 值构造（key：flags@+0x38, EA@+0x3C,
 * name_len@+0x40, ns@+0x41, name@+0x42；与 nt_walk_entries 读取一致） ---------- */
static void nt_build_filename_key(uint8_t *key, uint32_t parent_mftno,
                                   const char *name, uint32_t name_len,
                                   int is_dir, uint32_t alloc_sz, uint32_t real_sz) {
    for (uint32_t i = 0; i < 0x42 + name_len * 2; i++) key[i] = 0;
    nt_wr64(key, (uint64_t)parent_mftno | (1ull << 48));  /* parent ref */
    nt_wr64(key + 0x20, alloc_sz);
    nt_wr64(key + 0x28, real_sz);
    nt_wr32(key + 0x38, is_dir ? NT_ATTR_DIRECTORY : 0);
    key[0x40] = (uint8_t)name_len;
    key[0x41] = 1;                                        /* WINDOWS 命名空间 */
    for (uint32_t i = 0; i < name_len; i++)
        nt_wr16(key + 0x42 + i * 2, (uint16_t)(uint8_t)name[i]);
}

/* ---------- runlist 编码（ntfs-3g mkntfs 同构，变长字段） ----------
 * 每 run：header（低 3 位 = len 字节数，高 3 位 = lcn 增量字节数）+
 * len（小端）+ lcn 增量（小端，恒为正）。终止符 0。 */
static uint8_t *nt_run_encode(uint8_t *p, uint32_t len, uint32_t off_delta) {
    uint32_t nb_len = 1;
    if (len >= 0x100u) nb_len = 2;
    if (len >= 0x10000u) nb_len = 3;
    if (len >= 0x1000000u) nb_len = 4;
    uint32_t nb_off = 1;
    if (off_delta >= 0x100u) nb_off = 2;
    if (off_delta >= 0x10000u) nb_off = 3;
    if (off_delta >= 0x1000000u) nb_off = 4;
    if (nb_off < 2) nb_off = 2;              /* LCN 至少 2 字节（mkntfs 惯例） */
    *p++ = (uint8_t)(nb_len | (nb_off << 4));
    for (uint32_t i = 0; i < nb_len; i++)
        *p++ = (uint8_t)((len >> (8 * i)) & 0xFF);
    for (uint32_t i = 0; i < nb_off; i++)
        *p++ = (uint8_t)((off_delta >> (8 * i)) & 0xFF);
    return p;
}

/* 构造一个新 FILE 记录（文件或目录）。
 * 返回记录 used 大小；data/size 仅文件用（size==0xFFFFFFFF 表示目录）。
 * run_lcn/run_len/run_count：非驻留数据的多 run 列表。 */
static uint32_t nt_build_record(uint8_t *rec, uint32_t mftno, int is_dir,
                                 const char *name, uint32_t name_len,
                                 uint32_t parent_mftno,
                                 const uint8_t *data, uint32_t size,
                                 const uint32_t *run_lcn, const uint32_t *run_len,
                                 uint32_t run_count) {
    for (uint32_t i = 0; i < nt_rec_bytes; i++) rec[i] = 0;
    nt_wr32(rec, 0x454C4946u);                    /* "FILE" */
    nt_wr16(rec + 4, 0x30);                       /* usa_ofs */
    nt_wr16(rec + 6, (uint16_t)(nt_rec_bytes / 512 + 1));  /* usa_cnt */
    nt_wr16(rec + 0x10, 1);                       /* sequence */
    nt_wr16(rec + 0x12, 1);                       /* link count */
    nt_wr16(rec + 0x14, 0x36);                    /* attrs_off */
    nt_wr16(rec + 0x16, is_dir ? 3 : 1);          /* in-use + dir */
    nt_wr32(rec + 0x1C, nt_rec_bytes);            /* alloc size */
    nt_wr32(rec + 0x20, mftno);                   /* 记录号（低 4B） */
    /* fixup 数组占位：0x30..0x35 */
    uint32_t off = 0x36;

    /* $STANDARD_INFORMATION (0x10)：驻留，值 48 字节全零 */
    uint8_t *a = rec + off;
    nt_wr32(a, NT_AT_STANDARD_INFO);
    nt_wr32(a + 4, 0x18 + 48);
    a[8] = 0; a[9] = 0; nt_wr16(a + 0xA, 0); nt_wr16(a + 0xC, 0);
    nt_wr32(a + 0x10, 48);
    nt_wr32(a + 0x14, 0x18);
    off += 0x18 + 48;

    /* $FILE_NAME (0x30)：驻留未命名 */
    uint32_t keylen = 0x42 + name_len * 2;
    uint32_t fn_alen = (0x18 + keylen + 7) & ~7u;
    a = rec + off;
    nt_wr32(a, NT_AT_FILENAME);
    nt_wr32(a + 4, fn_alen);
    a[8] = 0; a[9] = 0; nt_wr16(a + 0xA, 0); nt_wr16(a + 0xC, 0);
    nt_wr32(a + 0x10, keylen);
    nt_wr32(a + 0x14, 0x18);
    nt_build_filename_key(a + 0x18, parent_mftno, name, name_len, is_dir,
                          is_dir ? 0 : size, is_dir ? 0 : size);
    off += fn_alen;

    if (is_dir) {
        /* $INDEX_ROOT (0x90) "$I30"：驻留，仅含末条目（空目录） */
        uint32_t ir_alen = (0x20 + 0x20 + 0x10 + 7) & ~7u;  /* 头+值+末条目 */
        a = rec + off;
        nt_wr32(a, NT_AT_INDEX_ROOT);
        nt_wr32(a + 4, ir_alen);
        a[8] = 0; a[9] = 4;                       /* 驻留，名长 4 字符 */
        nt_wr16(a + 0xA, 0x18);                   /* name_off */
        nt_wr16(a + 0xC, 0);
        nt_wr32(a + 0x10, 0x20 + 0x10);           /* value_len */
        nt_wr32(a + 0x14, 0x20);                  /* value_off（名字之后） */
        /* 属性名 "$I30" UTF-16 @0x18 */
        nt_wr16(a + 0x18, '$'); nt_wr16(a + 0x1A, 'I');
        nt_wr16(a + 0x1C, '3'); nt_wr16(a + 0x1E, '0');
        /* 值 @0x20：INDEX_ROOT + INDEX_HEADER + 末条目 */
        uint8_t *ir = a + 0x20;
        nt_wr32(ir, NT_AT_FILENAME);              /* indexed attr type */
        nt_wr32(ir + 4, 1);                       /* collation */
        nt_wr32(ir + 8, nt_idx_bytes);            /* index block size */
        ir[12] = 1;                               /* clusters per block */
        uint8_t *ih = ir + 0x10;
        nt_wr32(ih, 0x10);                        /* entries_off（相对 ih） */
        nt_wr32(ih + 4, 0x20);                    /* index_len（自 ih 起含末条目） */
        nt_wr32(ih + 8, 0x20);                    /* alloc_size */
        nt_wr32(ih + 12, 0);                      /* flags：小目录 */
        /* 末条目 @ir+0x20 */
        uint8_t *last = ir + 0x20;
        nt_wr16(last + 8, 0x10);                  /* entry_len */
        nt_wr16(last + 0xC, 2);                   /* flags：last */
        off += ir_alen;
    } else if (size <= 700) {
        /* $DATA (0x80)：驻留 */
        uint32_t alen = (0x18 + size + 7) & ~7u;
        a = rec + off;
        nt_wr32(a, NT_AT_DATA);
        nt_wr32(a + 4, alen);
        a[8] = 0; a[9] = 0; nt_wr16(a + 0xA, 0); nt_wr16(a + 0xC, 0);
        nt_wr32(a + 0x10, size);
        nt_wr32(a + 0x14, 0x18);
        for (uint32_t i = 0; i < size; i++) a[0x18 + i] = data[i];
        off += alen;
    } else {
        /* $DATA (0x80)：非常驻多 run（run 列表已分配） */
        uint32_t nclu = 0;
        for (uint32_t i = 0; i < run_count; i++) nclu += run_len[i];
        a = rec + off;
        nt_wr32(a, NT_AT_DATA);
        nt_wr32(a + 4, 0x40 + 48);               /* 头 + runlist 预留 */
        a[8] = 1;                                 /* non-resident */
        a[9] = 0; nt_wr16(a + 0xA, 0); nt_wr16(a + 0xC, 0);
        nt_wr64(a + 0x10, 0);                     /* start_vcn */
        nt_wr64(a + 0x18, nclu - 1);              /* last_vcn */
        nt_wr16(a + 0x20, 0x40);                  /* runlist 偏移 */
        nt_wr16(a + 0x22, 0);                     /* comp unit 0=未压缩 */
        nt_wr64(a + 0x28, (uint64_t)nclu * nt_cluster_bytes);  /* alloc */
        nt_wr64(a + 0x30, size);                  /* real */
        nt_wr64(a + 0x38, size);                  /* init */
        /* runlist @0x40：变长编码，终止符 0 */
        uint8_t *rl = a + 0x40;
        uint32_t prev_lcn = 0;
        for (uint32_t i = 0; i < run_count; i++) {
            rl = nt_run_encode(rl, run_len[i],
                               i == 0 ? run_lcn[i] : run_lcn[i] - prev_lcn);
            prev_lcn = run_lcn[i];
        }
        *rl++ = 0;
        uint32_t used_alen = 0x40 + (uint32_t)(rl - (a + 0x40));
        nt_wr32(a + 4, used_alen);                /* 实际属性长度 */
        off += used_alen;
    }

    /* $END */
    nt_wr32(rec + off, 0xFFFFFFFFu);
    nt_wr32(rec + off + 4, 8);
    off += 8;
    nt_wr32(rec + 0x18, off);                     /* used size */
    return off;
}

/* ---------- 父目录 INDEX_ROOT 条目插删 ---------- */

/* 定位目录记录的 INDEX_ROOT；返回属性指针，0=失败 */
static const uint8_t *nt_get_index_root(const uint8_t *dirrec) {
    const uint8_t *root = nt_find_attr(dirrec, NT_AT_INDEX_ROOT, "$I30");
    if (root == 0 || root[8] != 0) return 0;
    const uint8_t *ir = root + rd16(root + 0x14);
    if (rd32(ir) != NT_AT_FILENAME) return 0;
    return root;
}

/* 在 INDEX_ROOT 条目序列中查找名字（大小写不敏感）。
 * 返回条目偏移（相对目录记录），未找到返回 0；ref_out 可空。 */
static uint32_t nt_find_entry(const uint8_t *dirrec, const char *name,
                              uint32_t *ref_out) {
    const uint8_t *root = nt_get_index_root(dirrec);
    if (root == 0) return 0;
    const uint8_t *ir = root + rd16(root + 0x14);
    const uint8_t *ih = ir + 0x10;
    const uint8_t *pos = ih + rd32(ih);
    while (1) {
        uint16_t e_len = rd16(pos + 8);
        uint16_t e_flags = rd16(pos + 0xC);
        if (e_len < 0x10) return 0;
        if (e_flags & 2) return 0;                /* 到末条目仍未找到 */
        if (e_flags & 1) { pos += e_len; continue; } /* 分隔条目：跳过 */
        uint8_t ns = pos[0x50];
        uint8_t ns_kind = pos[0x51];
        if (ns > 0 && ns_kind != 2) {
            char nb[256];
            nt_utf16_to_ascii(pos + 0x52, ns, nb, sizeof(nb));
            if (nt_name_eq(nb, name)) {
                if (ref_out) *ref_out = (uint32_t)(rd64(pos) & 0xFFFFFFFFFFFFull);
                return (uint32_t)(pos - dirrec);
            }
        }
        pos += e_len;
    }
}

/* 在 INDEX_ROOT 中插入条目（插在末条目之前）。
 * rec 为目录记录缓冲（将被修改并写回）。0=成功。 */
static int nt_insert_entry(uint8_t *dirrec, uint32_t child_mftno,
                            const char *name, uint32_t name_len,
                            int is_dir, uint32_t real_sz) {
    const uint8_t *root = nt_get_index_root(dirrec);
    if (root == 0) return -1;
    uint32_t root_off = (uint32_t)(root - dirrec);
    uint32_t root_alen = rd32(dirrec + root_off + 4);
    const uint8_t *ir = root + rd16(root + 0x14);
    const uint8_t *ih = ir + 0x10;
    const uint8_t *entries = ih + rd32(ih);

    /* 找末条目 */
    const uint8_t *pos = entries;
    while (!(rd16(pos + 0xC) & 2)) {
        uint16_t e_len = rd16(pos + 8);
        if (e_len < 0x10) return -1;
        pos += e_len;
    }
    uint32_t last_off = (uint32_t)(pos - dirrec);

    /* 新条目：头 0x10 + FILE_NAME key */
    uint32_t keylen = 0x42 + name_len * 2;
    uint32_t entry_len = (0x10 + keylen + 7) & ~7u;

    /* 空间检查：记录尾（$END 之后）还有 entry_len 空隙 */
    uint32_t used = rd32(dirrec + 0x18);
    if (used + entry_len > nt_rec_bytes - 8) return -1;

    /* 把 [last_off, used) 右移 entry_len（含末条目与 $END） */
    for (uint32_t i = used; i > last_off; i--)
        dirrec[i + entry_len - 1] = dirrec[i - 1];

    /* 写新条目 @last_off */
    uint8_t *e = dirrec + last_off;
    nt_wr64(e, (uint64_t)child_mftno | (1ull << 48));
    nt_wr16(e + 8, (uint16_t)entry_len);
    nt_wr16(e + 0xA, (uint16_t)keylen);
    nt_wr16(e + 0xC, 0);                          /* flags：非末条目 */
    nt_wr16(e + 0xE, 0);
    nt_build_filename_key(e + 0x10, NT_ROOT_FILE, name, name_len, is_dir,
                          real_sz, real_sz);

    /* 更新属性长度 / used / index_len / alloc（Windows 语义：自 ih 起算） */
    nt_wr32(dirrec + root_off + 4, root_alen + entry_len);
    nt_wr32(dirrec + 0x18, used + entry_len);
    /* ir/ih 偏移可能因 root 属性头不变而稳定；index_len 累加 */
    uint32_t ir_off = root_off + rd16(dirrec + root_off + 0x14);
    nt_wr32(dirrec + ir_off + 0x10 + 4,
            rd32(dirrec + ir_off + 0x10 + 4) + entry_len);
    nt_wr32(dirrec + ir_off + 0x10 + 8,
            rd32(dirrec + ir_off + 0x10 + 8) + entry_len);
    return 0;
}

/* 从 INDEX_ROOT 删除条目（entry_off 为 nt_find_entry 结果）。 */
static int nt_remove_entry(uint8_t *dirrec, uint32_t entry_off) {
    const uint8_t *root = nt_get_index_root(dirrec);
    if (root == 0) return -1;
    uint32_t root_off = (uint32_t)(root - dirrec);
    uint32_t root_alen = rd32(dirrec + root_off + 4);
    uint32_t used = rd32(dirrec + 0x18);
    uint32_t e_len = rd16(dirrec + entry_off + 8);
    /* [entry_off+e_len, used) 左移 e_len */
    for (uint32_t i = entry_off + e_len; i < used; i++)
        dirrec[i - e_len] = dirrec[i];
    /* 尾部清零 */
    for (uint32_t i = used - e_len; i < used; i++) dirrec[i] = 0;
    nt_wr32(dirrec + root_off + 4, root_alen - e_len);
    nt_wr32(dirrec + 0x18, used - e_len);
    uint32_t ir_off = root_off + rd16(dirrec + root_off + 0x14);
    nt_wr32(dirrec + ir_off + 0x10 + 4,
            rd32(dirrec + ir_off + 0x10 + 4) - e_len);
    nt_wr32(dirrec + ir_off + 0x10 + 8,
            rd32(dirrec + ir_off + 0x10 + 8) - e_len);
    return 0;
}

/* 回收非常驻 $DATA 的簇（run list 遍历） */
static void nt_free_data_runs(const uint8_t *a) {
    if (a == 0 || a[8] == 0) return;
    const uint8_t *run = a + rd16(a + 0x20);
    uint64_t lcn = 0;
    for (;;) {
        uint8_t c1 = (*run) & 0x7;
        uint8_t c2 = ((*run) >> 4) & 0x7;
        if (c1 == 0) break;
        run++;
        uint64_t len = nt_run_read(run, c1, 0);
        run += c1;
        uint64_t off = nt_run_read(run, c2, 1);
        run += c2;
        lcn += off;
        if (off != 0)                             /* sparse run 跳过 */
            for (uint64_t i = 0; i < len && lcn + i < 0x10000ull; i++)
                nt_free_cluster((uint32_t)(lcn + i));
    }
}

/* ---------- 大目录 $INDEX_ALLOCATION（INDX 叶块）支持 ----------
 * 布局：每目录预留 4 个连续 INDX 簇（块号=VCN=位图位），单 runlist；
 * 块内条目按名排序，根节点分隔条目（flag bit0 + 尾部 VCN）指向各叶。
 * 上限 ~4 x 30 条目/目录（2MB 卷模型），足够且结构对 Windows 合法。 */

#define NT_INDX_BLOCKS 4

/* 名字比较（entry 为 INDEX 条目，key 在 +0x10；大小写不敏感）。
 * 返回 <0/0/>0 同 strcmp */
static int nt_entry_name_cmp(const uint8_t *entry, const char *name) {
    uint8_t ns = entry[0x50];
    if (ns == 0) return -1;
    char nb[256];
    nt_utf16_to_ascii(entry + 0x52, ns, nb, sizeof(nb));
    uint32_t i = 0;
    while (nb[i] && name[i]) {
        char ca = nt_lower(nb[i]), cb = nt_lower(name[i]);
        if (ca != cb) return (uint8_t)ca < (uint8_t)cb ? -1 : 1;
        i++;
    }
    if (!nb[i] && !name[i]) return 0;
    return nb[i] ? 1 : -1;
}

/* 目录是否为大目录 */
static int nt_dir_is_large(const uint8_t *dirrec) {
    return nt_find_attr(dirrec, NT_AT_INDEX_ALLOC, "$I30") != 0;
}

/* 读 VCN 对应 INDX 块到 nt_indx（fixup 校正）。0=成功 */
static int nt_read_indx(const uint8_t *dirrec, uint32_t vcn) {
    const uint8_t *alloc = nt_find_attr(dirrec, NT_AT_INDEX_ALLOC, "$I30");
    if (alloc == 0 || alloc[8] == 0) return -1;
    const uint8_t *run = alloc + rd16(alloc + 0x20);
    uint64_t lcn = nt_run_map(run, vcn);
    if (lcn == NT_NO_LCN || lcn > 0x0FFFFFFFull) return -1;
    uint32_t secs = nt_idx_bytes / 512;
    uint32_t lba = nt_part_lba + (uint32_t)lcn * nt_spc;
    for (uint32_t s = 0; s < secs; s++)
        if (ata_read_sector(nt_drive, lba + s, nt_indx + s * 512) != 0)
            return -1;
    return nt_fixup(nt_indx, nt_idx_bytes, "INDX");
}

/* 写回 nt_indx 中的 INDX 块（fixup 反向应用）。0=成功 */
static int nt_write_indx(const uint8_t *dirrec, uint32_t vcn) {
    const uint8_t *alloc = nt_find_attr(dirrec, NT_AT_INDEX_ALLOC, "$I30");
    if (alloc == 0 || alloc[8] == 0) return -1;
    const uint8_t *run = alloc + rd16(alloc + 0x20);
    uint64_t lcn = nt_run_map(run, vcn);
    if (lcn == NT_NO_LCN || lcn > 0x0FFFFFFFull) return -1;
    nt_usn++;
    uint8_t *pu = nt_indx + rd16(nt_indx + 4);
    pu[0] = (uint8_t)(nt_usn & 0xFF);
    pu[1] = (uint8_t)((nt_usn >> 8) & 0xFF);
    uint32_t cnt = rd16(nt_indx + 6);
    for (uint32_t i = 0; i + 1 < cnt; i++) {
        uint8_t *tail = nt_indx + i * 512 + 510;
        pu[2 + i * 2] = tail[0];
        pu[3 + i * 2] = tail[1];
        tail[0] = pu[0];
        tail[1] = pu[1];
    }
    uint32_t secs = nt_idx_bytes / 512;
    uint32_t lba = nt_part_lba + (uint32_t)lcn * nt_spc;
    for (uint32_t s = 0; s < secs; s++)
        if (ata_write_sector(nt_drive, lba + s, nt_indx + s * 512) != 0)
            return -1;
    return 0;
}

/* 初始化 INDX 块（叶：无子节点，仅末条目） */
static void nt_indx_init(uint8_t *blk, uint32_t vcn) {
    for (uint32_t i = 0; i < nt_idx_bytes; i++) blk[i] = 0;
    nt_wr32(blk, 0x58444E49u);              /* "INDX" */
    nt_wr16(blk + 4, 0x28);                   /* usa_ofs（fixup 数组区） */
    nt_wr16(blk + 6, (uint16_t)(nt_idx_bytes / 512 + 1));
    nt_wr64(blk + 0x10, vcn);
    uint8_t *ih = blk + 0x18;
    nt_wr32(ih, 0x28);                        /* entries_off（相对 ih） -> 0x40 */
    nt_wr32(ih + 4, 0x28 + 0x10);             /* index_len（自 ih 起，仅末条目） */
    nt_wr32(ih + 8, nt_idx_bytes - 0x18);     /* alloc */
    nt_wr32(ih + 12, 0);                      /* 叶子 */
    /* 末条目 @0x40：无 key 无子节点 */
    nt_wr16(blk + 0x40 + 8, 0x10);
    nt_wr16(blk + 0x40 + 0xC, 2);             /* last */
}

/* $I30 $BITMAP 属性值中置/清块位（驻留 4 字节模型） */
static int nt_i30_bitmap_set(uint8_t *dirrec, uint32_t bit, int set) {
    uint8_t *a = (uint8_t *)nt_find_attr(dirrec, NT_AT_BITMAP, "$I30");
    if (a == 0 || a[8] != 0) return -1;
    uint32_t off = (uint32_t)(a - dirrec) + rd32(a + 0x14);
    if (bit < 8 * 4) {
        if (set) dirrec[off + bit / 8] |= (uint8_t)(1u << (bit % 8));
        else     dirrec[off + bit / 8] &= (uint8_t)~(1u << (bit % 8));
    }
    return 0;
}

/* 小目录 -> 大目录：搬 INDEX_ROOT 全部条目入 INDX 叶块（排序），
 * 根节点只留带右子 VCN 的末条目，追加 $INDEX_ALLOCATION/$BITMAP。
 * dirrec 为记录缓冲；成功后 nt_indx 保留叶块内容。0=成功 */
static int nt_dir_make_large(uint8_t *dirrec) {
    if (nt_idx_bytes != nt_cluster_bytes) return -1;  /* 要求块=簇 */
    const uint8_t *root = nt_get_index_root(dirrec);
    if (root == 0) return -1;
    const uint8_t *ir = root + rd16(root + 0x14);
    uint8_t *ih = (uint8_t *)ir + 0x10;
    uint8_t *ents = ih + rd32(ih);

    /* 收集现有条目偏移（跳过分隔条目与末条目） */
    uint32_t offs[96];
    uint32_t cnt = 0;
    const uint8_t *p = ents;
    while (!(rd16(p + 0xC) & 2)) {
        uint16_t el = rd16(p + 8);
        if (el < 0x10) return -1;
        if (!(rd16(p + 0xC) & 1)) {
            if (cnt >= 96) return -1;
            offs[cnt++] = (uint32_t)(p - ents);
        }
        p += el;
    }
    uint32_t region = (uint32_t)(p - ents);

    /* 分配 4 个连续 INDX 簇 */
    uint32_t base_lcn = nt_alloc_clusters(NT_INDX_BLOCKS);
    if (base_lcn == 0xFFFFFFFFu) return -1;

    /* 构造叶块：条目按名排序拷贝 */
    nt_indx_init(nt_indx, 0);
    for (uint32_t i = 1; i < cnt; i++)         /* 插入排序 */
        for (uint32_t j = i; j > 0; j--) {
            const uint8_t *ea = ents + offs[j];
            const uint8_t *eb = ents + offs[j - 1];
            char na[256], nb2[256];
            nt_utf16_to_ascii(ea + 0x52, ea[0x50], na, sizeof(na));
            nt_utf16_to_ascii(eb + 0x52, eb[0x50], nb2, sizeof(nb2));
            int cmp = 0;
            for (uint32_t k = 0; !cmp; k++) {
                char x = nt_lower(na[k]), y = nt_lower(nb2[k]);
                if (!x && !y) break;
                if (x != y) cmp = (uint8_t)x < (uint8_t)y ? -1 : 1;
            }
            if (cmp < 0) {
                uint32_t t = offs[j]; offs[j] = offs[j - 1]; offs[j - 1] = t;
            } else break;
        }
    uint8_t *dst = nt_indx + 0x40;
    uint32_t total = 0;
    for (uint32_t i = 0; i < cnt; i++) {
        const uint8_t *src = ents + offs[i];
        uint16_t el = rd16(src + 8);
        for (uint16_t k = 0; k < el; k++) dst[k] = src[k];
        dst += el;
        total += el;
    }
    /* 末条目 */
    nt_wr16(dst + 8, 0x10);
    nt_wr16(dst + 0xC, 2);
    total += 0x10;
    nt_wr32(nt_indx + 0x18 + 4, 0x28 + total);          /* index_len */
    if (0x40 + total > nt_idx_bytes) { nt_free_cluster(base_lcn); return -1; }

    /* 尾部追加 $INDEX_ALLOCATION + $BITMAP（覆盖 $END 再补新 $END） */
    uint32_t used = rd32(dirrec + 0x18);
    if (rd32(dirrec + used - 8) != 0xFFFFFFFFu) return -1;
    uint32_t off = used - 8;

    /* $INDEX_ALLOCATION 非驻留 "$I30"：单 run 4 簇 @ base_lcn */
    uint8_t *a = dirrec + off;
    nt_wr32(a, NT_AT_INDEX_ALLOC);
    nt_wr32(a + 4, 0x40 + 8);
    a[8] = 1;
    a[9] = 4;                                  /* 名长 4 字符 */
    nt_wr16(a + 0xA, 0x18);
    nt_wr16(a + 0xC, 0);
    nt_wr64(a + 0x10, 0);                      /* start_vcn */
    nt_wr64(a + 0x18, NT_INDX_BLOCKS - 1);     /* last_vcn */
    nt_wr16(a + 0x20, 0x40);                   /* runlist 偏移 */
    nt_wr16(a + 0x22, 0);
    nt_wr64(a + 0x28, (uint64_t)NT_INDX_BLOCKS * nt_cluster_bytes);
    nt_wr64(a + 0x30, (uint64_t)NT_INDX_BLOCKS * nt_cluster_bytes);
    nt_wr64(a + 0x38, (uint64_t)NT_INDX_BLOCKS * nt_cluster_bytes);
    nt_wr16(a + 0x18, '$'); nt_wr16(a + 0x1A, 'I');
    nt_wr16(a + 0x1C, '3'); nt_wr16(a + 0x1E, '0');
    uint8_t *rl = a + 0x40;
    rl = nt_run_encode(rl, NT_INDX_BLOCKS, base_lcn);
    *rl++ = 0;
    uint32_t alen = 0x40 + (uint32_t)(rl - (a + 0x40));
    nt_wr32(a + 4, alen);
    off += alen;

    /* $BITMAP "$I30" 驻留 4 字节（块 0 已用） */
    a = dirrec + off;
    nt_wr32(a, NT_AT_BITMAP);
    nt_wr32(a + 4, (0x18 + 8 + 4 + 7) & ~7u);
    a[8] = 0;
    a[9] = 4;
    nt_wr16(a + 0xA, 0x18);
    nt_wr16(a + 0xC, 0);
    nt_wr32(a + 0x10, 4);
    nt_wr32(a + 0x14, 0x20);                   /* value_off（名字 8B 之后） */
    nt_wr16(a + 0x18, '$'); nt_wr16(a + 0x1A, 'I');
    nt_wr16(a + 0x1C, '3'); nt_wr16(a + 0x1E, '0');
    a[0x20] = 0x01;
    off += (0x18 + 8 + 4 + 7) & ~7u;

    /* 新 $END + used */
    nt_wr32(dirrec + off, 0xFFFFFFFFu);
    nt_wr32(dirrec + off + 4, 8);
    off += 8;
    if (off > nt_rec_bytes) return -1;
    nt_wr32(dirrec + 0x18, off);

    /* INDEX_ROOT 改为大索引：header flags=1，条目区只留带右子 VCN 的末条目 */
    nt_wr32(ih + 12, 1);                       /* flags：大目录 */
    nt_wr32(ih + 4, rd32(ih) + 0x18);          /* index_len */
    nt_wr32(ih + 8, rd32(ih) + 0x18);          /* alloc */
    uint8_t *e = (uint8_t *)ents;
    nt_wr16(e + 8, 0x18);
    nt_wr16(e + 0xA, 0);
    nt_wr16(e + 0xC, 3);                        /* last + 子节点 */
    nt_wr64(e + 0x10, 0);                       /* 右子 VCN = 0 */
    /* 清零原条目区剩余部分 */
    for (uint32_t i = 0x18; i < region; i++) e[i] = 0;

    /* 落盘叶块 0（属性已就位，runlist 可解析） */
    if (nt_write_indx(dirrec, 0) != 0) return -1;
    return 0;
}

/* $I30 位图测试：返回 1=使用中 0=空闲 -1=越界 */
static int nt_i30_bitmap_test(const uint8_t *dirrec, uint32_t bit) {
    const uint8_t *a = nt_find_attr(dirrec, NT_AT_BITMAP, "$I30");
    if (a == 0 || a[8] != 0) return -1;
    if (bit >= 8 * 4) return -1;
    const uint8_t *val = a + rd32(a + 0x14);
    return (val[bit / 8] & (1u << (bit % 8))) ? 1 : 0;
}

/* 叶块分裂：上半条目移入新块，根节点加分隔条目（新条目由调用者重试）。
 * nt_indx 中为待分裂叶块内容。0=成功 */
static int nt_indx_split(uint8_t *dirrec, uint32_t vcn) {
    /* 找空闲块 */
    uint32_t new_vcn = 0xFFFFFFFFu;
    for (uint32_t b = 0; b < NT_INDX_BLOCKS; b++)
        if (nt_i30_bitmap_test(dirrec, b) == 0) { new_vcn = b; break; }
    if (new_vcn == 0xFFFFFFFFu) return -1;      /* 块全用完 */

    /* 收集旧叶条目偏移与名字，找中点 */
    uint8_t *ih2 = nt_indx + 0x18;
    uint8_t *p = nt_indx + 0x18 + rd32(ih2);
    uint32_t offs[96], cnt = 0, bytes = 0;
    while (!(rd16(p + 0xC) & 2)) {
        if (cnt >= 96) return -1;
        offs[cnt++] = (uint32_t)(p - nt_indx);
        bytes += rd16(p + 8);
        p += rd16(p + 8);
    }
    /* 中点：前半留在旧块（含 newent 将插入的位置语义粗略化） */
    uint32_t half = 0, acc = 0, split_at = 0;
    for (split_at = 0; split_at < cnt; split_at++) {
        if (acc >= bytes / 2) break;
        acc += rd16(nt_indx + offs[split_at] + 8);
    }
    if (split_at == 0 || split_at >= cnt) split_at = cnt / 2;
    (void)half;

    /* 新块：初始化 + 拷贝后半 */
    static uint8_t tmpblk[NT_MAX_IDX] NT_HIBUF;
    for (uint32_t i = 0; i < nt_idx_bytes; i++) tmpblk[i] = 0;
    nt_indx_init(tmpblk, new_vcn);
    uint8_t *dst = tmpblk + 0x40;
    uint32_t total = 0;
    for (uint32_t i = split_at; i < cnt; i++) {
        const uint8_t *src = nt_indx + offs[i];
        uint16_t el = rd16(src + 8);
        for (uint16_t k = 0; k < el; k++) dst[k] = src[k];
        dst += el;
        total += el;
    }
    nt_wr32(tmpblk + 0x18 + 4, 0x28 + total);

    /* 旧块：截断后半 + 补末条目 */
    uint32_t kept = 0;
    for (uint32_t i = 0; i < split_at; i++)
        kept += rd16(nt_indx + offs[i] + 8);
    for (uint32_t i = 0x40 + kept; i < nt_idx_bytes; i++) nt_indx[i] = 0;
    nt_wr16(nt_indx + 0x40 + kept + 8, 0x10);
    nt_wr16(nt_indx + 0x40 + kept + 0xC, 2);
    nt_wr32(ih2 + 4, 0x28 + kept + 0x10);

    /* 根节点插入分隔条目：key = 新块首条目副本 + 子 VCN */
    /* 先腾空间：根条目区末尾（末条目）右移一条目 */
    const uint8_t *root = nt_get_index_root(dirrec);
    if (root == 0) return -1;
    const uint8_t *ir = root + rd16(root + 0x14);
    const uint8_t *ih = ir + 0x10;
    uint8_t *rents = (uint8_t *)ih + rd32(ih);
    /* 分隔条目 = 新块首条目 + VCN（entry_len + 8，8 对齐） */
    const uint8_t *first = tmpblk + 0x40;
    uint16_t src_el = rd16(first + 8);
    uint32_t sep_len = (src_el + 8 + 7) & ~7u;
    /* 找根末条目位置与条目区末 */
    uint8_t *rp = rents;
    while (!(rd16(rp + 0xC) & 2)) rp += rd16(rp + 8);
    uint32_t end_off = (uint32_t)(rp - dirrec);
    uint32_t used = rd32(dirrec + 0x18);
    if (used + sep_len > nt_rec_bytes) return -1;    /* 根满 */
    /* [end_off, used) 右移 sep_len */
    for (uint32_t i = used; i > end_off; i--)
        dirrec[i + sep_len - 1] = dirrec[i - 1];
    /* 写分隔条目 */
    for (uint32_t i = 0; i < src_el; i++) dirrec[end_off + i] = first[i];
    nt_wr16(dirrec + end_off + 8, (uint16_t)sep_len);
    nt_wr16(dirrec + end_off + 0xC, 1);              /* 子节点标志 */
    nt_wr64(dirrec + end_off + src_el, new_vcn);     /* VCN 紧跟 key */
    nt_wr32(dirrec + 0x18, used + sep_len);
    /* 根 index_len/alloc 累加（自 ih 起算语义） */
    uint32_t ir_off = (uint32_t)(root - dirrec) + rd16(root + 0x14);
    nt_wr32(dirrec + ir_off + 0x10 + 4, rd32(dirrec + ir_off + 0x10 + 4) + sep_len);
    nt_wr32(dirrec + ir_off + 0x10 + 8, rd32(dirrec + ir_off + 0x10 + 8) + sep_len);
    /* root 属性长度 */
    nt_wr32(dirrec + (uint32_t)(root - dirrec) + 4,
            rd32(dirrec + (uint32_t)(root - dirrec) + 4) + sep_len);

    /* 新块置位图位 */
    if (nt_i30_bitmap_set(dirrec, new_vcn, 1) != 0) return -1;

    /* 落盘：旧块（含新条目重插由调用者重试）、新块、根记录 */
    if (nt_write_indx(dirrec, vcn) != 0) return -1;
    /* 换 nt_indx 内容写新块 */
    static uint8_t save[NT_MAX_IDX] NT_HIBUF;
    for (uint32_t i = 0; i < nt_idx_bytes; i++) save[i] = nt_indx[i];
    for (uint32_t i = 0; i < nt_idx_bytes; i++) nt_indx[i] = tmpblk[i];
    if (nt_write_indx(dirrec, new_vcn) != 0) return -1;
    for (uint32_t i = 0; i < nt_idx_bytes; i++) nt_indx[i] = save[i];
    return 0;
}

/* 大目录插入单次尝试：返回 0=成功 2=已分裂需重试 -1=失败 */
static int nt_indx_insert_at(uint8_t *dirrec, const char *name,
                             const uint8_t *newent, uint32_t entry_len) {
    uint32_t last_vcn = 0;
    for (uint32_t vcn = 0; vcn < NT_INDX_BLOCKS; vcn++) {
        if (nt_i30_bitmap_test(dirrec, vcn) != 1) continue;
        last_vcn = vcn;
        if (nt_read_indx(dirrec, vcn) != 0) return -1;
        uint8_t *ih2 = nt_indx + 0x18;
        uint8_t *p = nt_indx + 0x18 + rd32(ih2);
        while (!(rd16(p + 0xC) & 2)) {
            if (nt_entry_name_cmp(p, name) > 0) {
                uint32_t idx_len = rd32(ih2 + 4);
                if (0x18 + idx_len + entry_len <= nt_idx_bytes) {
                    uint32_t end_off = 0x18 + idx_len;
                    for (uint32_t i = end_off; i > (uint32_t)(p - nt_indx); i--)
                        nt_indx[i + entry_len - 1] = nt_indx[i - 1];
                    for (uint32_t i = 0; i < entry_len; i++)
                        nt_indx[(uint32_t)(p - nt_indx) + i] = newent[i];
                    nt_wr32(ih2 + 4, idx_len + entry_len);
                    return nt_write_indx(dirrec, vcn);
                }
                /* 叶满：分裂后由调用者重试 */
                return nt_indx_split(dirrec, vcn) == 0 ? 2 : -1;
            }
            p += rd16(p + 8);
        }
        /* 本叶全部 <= name：继续下一叶 */
    }
    /* 插入最后使用中的叶的末尾（末条目前） */
    if (nt_read_indx(dirrec, last_vcn) != 0) return -1;
    uint8_t *ih2 = nt_indx + 0x18;
    uint32_t idx_len = rd32(ih2 + 4);
    if (0x18 + idx_len + entry_len > nt_idx_bytes)
        return nt_indx_split(dirrec, last_vcn) == 0 ? 2 : -1;
    uint32_t ins = 0x18 + idx_len - 0x10;      /* 末条目位置 */
    for (uint32_t i = 0x18 + idx_len; i > ins; i--)
        nt_indx[i + entry_len - 1] = nt_indx[i - 1];
    for (uint32_t i = 0; i < entry_len; i++) nt_indx[ins + i] = newent[i];
    nt_wr32(ih2 + 4, idx_len + entry_len);
    return nt_write_indx(dirrec, last_vcn);
}

/* 大目录插入入口（构造条目；叶满分裂后重试一次） */
static int nt_indx_insert(uint8_t *dirrec, uint32_t child_mftno,
                          const char *name, uint32_t name_len,
                          int is_dir, uint32_t real_sz) {
    static uint8_t newent[0x10 + 0x42 + 512 * 2 + 8] NT_HIBUF;
    uint32_t keylen = 0x42 + name_len * 2;
    uint32_t entry_len = (0x10 + keylen + 7) & ~7u;
    for (uint32_t i = 0; i < entry_len; i++) newent[i] = 0;
    nt_wr64(newent, (uint64_t)child_mftno | (1ull << 48));
    nt_wr16(newent + 8, (uint16_t)entry_len);
    nt_wr16(newent + 0xA, (uint16_t)keylen);
    nt_wr16(newent + 0xC, 0);
    nt_build_filename_key(newent + 0x10, NT_ROOT_FILE, name, name_len,
                          is_dir, real_sz, real_sz);
    for (int retry = 0; retry < 2; retry++) {
        int r = nt_indx_insert_at(dirrec, name, newent, entry_len);
        if (r == 2) continue;                  /* 已分裂，重试 */
        return r;
    }
    return -1;
}

/* 大目录删除：在叶块中摘除条目。loc_vcn/loc_off 为块 VCN 与
 * nt_indx 内偏移。0=成功（空叶保留，位图不清，合法结构） */
static int nt_indx_remove(uint8_t *dirrec, uint32_t loc_vcn, uint32_t loc_off) {
    if (nt_read_indx(dirrec, loc_vcn) != 0) return -1;
    uint32_t e_len = rd16(nt_indx + loc_off + 8);
    uint8_t *ih2 = nt_indx + 0x18;
    uint32_t idx_len = rd32(ih2 + 4);
    /* [loc_off+e_len, 0x18+idx_len) 左移 */
    for (uint32_t i = loc_off + e_len; i < 0x18 + idx_len; i++)
        nt_indx[i - e_len] = nt_indx[i];
    for (uint32_t i = 0x18 + idx_len - e_len; i < 0x18 + idx_len; i++)
        nt_indx[i] = 0;
    nt_wr32(ih2 + 4, idx_len - e_len);
    return nt_write_indx(dirrec, loc_vcn);
}

/* 统一插入入口：小目录直接插；放不下自动升级大目录 */
static int nt_dir_insert(uint8_t *dirrec, uint32_t child_mftno,
                         const char *name, uint32_t name_len,
                         int is_dir, uint32_t real_sz) {
    if (!nt_dir_is_large(dirrec)) {
        if (nt_insert_entry(dirrec, child_mftno, name, name_len,
                            is_dir, real_sz) == 0)
            return 0;
        if (nt_dir_make_large(dirrec) != 0) return -1;
    }
    return nt_indx_insert(dirrec, child_mftno, name, name_len,
                          is_dir, real_sz);
}

/* 统一查找：先 INDEX_ROOT 再各 INDX 叶块。
 * 返回 1=找到 0=未找到 -1=错误；in_indx/vcn/off 为条目位置
 * （in_indx=1 时 off 是块内偏移，nt_indx 载有该块；否则记录内偏移）。 */
static int nt_find_loc(uint8_t *dirrec, const char *name, uint32_t *ref_out,
                       int *in_indx, uint32_t *vcn, uint32_t *off) {
    *in_indx = 0;
    *vcn = 0;
    *off = 0;
    uint32_t roff = nt_find_entry(dirrec, name, ref_out);
    if (roff != 0) { *off = roff; return 1; }
    if (!nt_dir_is_large(dirrec)) return 0;
    for (uint32_t b = 0; b < NT_INDX_BLOCKS; b++) {
        if (nt_i30_bitmap_test(dirrec, b) != 1) continue;
        if (nt_read_indx(dirrec, b) != 0) return -1;
        uint8_t *ih2 = nt_indx + 0x18;
        uint8_t *p = nt_indx + 0x18 + rd32(ih2);
        while (!(rd16(p + 0xC) & 2)) {
            uint8_t ns = p[0x50];
            if (ns > 0 && p[0x51] != 2) {
                char nb[256];
                nt_utf16_to_ascii(p + 0x52, ns, nb, sizeof(nb));
                if (nt_name_eq(nb, name)) {
                    if (ref_out) *ref_out = (uint32_t)(rd64(p) & 0xFFFFFFFFFFFFull);
                    *in_indx = 1; *vcn = b; *off = (uint32_t)(p - nt_indx);
                    return 1;
                }
            }
            p += rd16(p + 8);
        }
    }
    return 0;
}

/* ---------- 路径拆分（绝对路径 -> 父目录记录号 + 叶名） ---------- */
static int nt_split_path(const char *path, char *parent_out, uint32_t pmax,
                         char *fname_out) {
    if (path == 0 || path[0] != '/') return -1;
    const char *slash = 0;
    for (const char *p = path; *p; p++)
        if (*p == '/') slash = p;
    if (slash == 0) return -1;
    const char *leaf = slash + 1;
    uint32_t ll = 0;
    while (leaf[ll] && leaf[ll] != '/' && ll < 255) ll++;
    if (ll == 0) return -1;
    for (uint32_t i = 0; i < ll; i++) fname_out[i] = leaf[i];
    fname_out[ll] = 0;

    if (slash == path) {
        if (pmax < 2) return -1;
        parent_out[0] = '/';
        parent_out[1] = 0;
    } else {
        uint32_t plen = (uint32_t)(slash - path);
        if (plen >= pmax) return -1;
        for (uint32_t i = 0; i < plen; i++) parent_out[i] = path[i];
        parent_out[plen] = 0;
    }
    return 0;
}

/* 解析目录路径 -> 记录号（记录留在 nt_rec）；0=失败 */
static uint32_t nt_resolve_dir(const char *path) {
    int is_dir = 0;
    if (nt_resolve(path, &is_dir, 0) == 0 || !is_dir) return 0;
    /* nt_resolve 不返回记录号：重新走一遍记录 5 起的链 */
    if (nt_read_record(NT_ROOT_FILE, nt_rec) != 0) return 0;
    uint32_t cur = NT_ROOT_FILE;
    const char *p = path;
    while (*p == '/') p++;
    while (*p) {
        const char *comp = p;
        uint32_t clen = 0;
        while (p[clen] && p[clen] != '/') clen++;
        char compbuf[256];
        for (uint32_t i = 0; i < clen && i < 255; i++) compbuf[i] = comp[i];
        compbuf[clen > 255 ? 255 : clen] = 0;
        nt_lookup_ctx ctx;
        ctx.name = compbuf;
        ctx.found = 0;
        if (nt_dir_walk(nt_rec, nt_lookup_cb, &ctx) != 0 || !ctx.found) return 0;
        if (!ctx.is_dir) return 0;
        if (nt_read_record(ctx.ref, nt_rec) != 0) return 0;
        cur = ctx.ref;
        p += clen;
        while (*p == '/') p++;
    }
    return cur == 0 ? 0 : cur;
}

/* ---------- 对外写入 API ---------- */

int ntfs_create_file(const char *name, const uint8_t *data, uint32_t size) {
    char parent_path[256];
    char fname[256];
    if (nt_split_path(name, parent_path, sizeof(parent_path), fname) != 0)
        return -1;
    uint32_t name_len = 0;
    while (fname[name_len]) name_len++;

    /* 载入两张位图 */
    if (nt_maps_load() != 0) return -1;

    /* 父目录记录 -> nt_rec */
    uint32_t parent = nt_resolve_dir(parent_path);
    if (parent == 0) return -1;

    /* create-or-replace：删旧条目 */
    uint32_t old_ref;
    int in_indx;
    uint32_t loc_vcn, loc_off;
    if (nt_find_loc(nt_rec, fname, &old_ref, &in_indx, &loc_vcn, &loc_off) == 1) {
        /* 旧记录簇回收 + 标记释放（nt_rec 中父目录暂存到 nt_newrec 再切回） */
        static uint8_t parent_save[NT_MAX_REC] NT_HIBUF;
        for (uint32_t i = 0; i < nt_rec_bytes; i++) parent_save[i] = nt_rec[i];
        if (nt_read_record(old_ref, nt_child) == 0) {
            if (nt_rec_is_dir(nt_child)) return -1;   /* 同名目录 */
            nt_free_data_runs(nt_find_attr(nt_child, NT_AT_DATA, 0));
            nt_wr16(nt_child + 0x16, 0);               /* 标记未使用 */
            nt_write_record(old_ref, nt_child);
            nt_free_mft(old_ref);
        }
        /* 恢复父目录记录并删其条目 */
        for (uint32_t i = 0; i < nt_rec_bytes; i++) nt_rec[i] = parent_save[i];
        if (in_indx) nt_indx_remove(nt_rec, loc_vcn, loc_off);
        else {
            uint32_t eoff = loc_off;
            if (eoff != 0) nt_remove_entry(nt_rec, eoff);
        }
    }

    /* 分配 MFT 记录 */
    uint32_t mftno = nt_alloc_mft();
    if (mftno == 0xFFFFFFFFu) return -1;

    /* 数据簇（大文件：多 run 碎片化分配，每 run <= 256 簇） */
    uint32_t run_lcn[16] = {0}, run_len[16] = {0};
    uint32_t run_count = 0;
    uint32_t nclu = 0;
    if (size > 700)
        nclu = (size + nt_cluster_bytes - 1) / nt_cluster_bytes;
    while (nclu > 0 && run_count < 16) {
        uint32_t want = nclu > 256 ? 256 : nclu;
        uint32_t s = nt_alloc_clusters(want);
        if (s == 0xFFFFFFFFu) {
            for (uint32_t i = 0; i < run_count; i++)
                for (uint32_t j = 0; j < run_len[i]; j++)
                    nt_free_cluster(run_lcn[i] + j);
            nt_free_mft(mftno);
            return -1;
        }
        run_lcn[run_count] = s;
        run_len[run_count] = want;
        run_count++;
        nclu -= want;
    }
    if (nclu > 0) {
        nt_free_mft(mftno);
        return -1;                              /* run 数不够（极端碎片） */
    }

    /* 构造并写新记录 */
    nt_build_record(nt_newrec, mftno, 0, fname, name_len, parent,
                    data, size, run_lcn, run_len, run_count);
    if (nt_write_record(mftno, nt_newrec) != 0) {
        nt_free_mft(mftno);
        for (uint32_t i = 0; i < run_count; i++)
            for (uint32_t j = 0; j < run_len[i]; j++)
                nt_free_cluster(run_lcn[i] + j);
        return -1;
    }

    /* 非驻留数据：按 run 写簇内容 */
    uint32_t done = 0;
    for (uint32_t r = 0; r < run_count; r++) {
        for (uint32_t c = 0; c < run_len[r]; c++) {
            for (uint32_t i = 0; i < nt_cluster_bytes; i++) nt_cbuf[i] = 0;
            uint32_t chunk = size - done;
            if (chunk > nt_cluster_bytes) chunk = nt_cluster_bytes;
            for (uint32_t i = 0; i < chunk; i++) nt_cbuf[i] = data[done + i];
            uint32_t lba = nt_part_lba + (run_lcn[r] + c) * nt_spc;
            for (uint32_t s = 0; s < nt_spc; s++)
                if (ata_write_sector(nt_drive, lba + s, nt_cbuf + s * 512) != 0)
                    return -1;
            done += chunk;
        }
    }

    /* 父目录插条目（小目录满自动升级 INDX 大目录） */
    if (nt_dir_insert(nt_rec, mftno, fname, name_len, 0, size) != 0) {
        nt_free_mft(mftno);
        for (uint32_t i = 0; i < run_count; i++)
            for (uint32_t j = 0; j < run_len[i]; j++)
                nt_free_cluster(run_lcn[i] + j);
        return -1;
    }
    if (nt_write_record(parent, nt_rec) != 0) return -1;
    return nt_maps_save();
}

int ntfs_delete_file(const char *name) {
    char parent_path[256];
    char fname[256];
    if (nt_split_path(name, parent_path, sizeof(parent_path), fname) != 0)
        return -1;

    if (nt_maps_load() != 0) return -1;
    uint32_t parent = nt_resolve_dir(parent_path);
    if (parent == 0) return -1;

    uint32_t ref;
    int in_indx;
    uint32_t loc_vcn, loc_off;
    if (nt_find_loc(nt_rec, fname, &ref, &in_indx, &loc_vcn, &loc_off) != 1)
        return -1;

    /* 保存父目录记录，读子记录回收 */
    static uint8_t parent_save[NT_MAX_REC] NT_HIBUF;
    for (uint32_t i = 0; i < nt_rec_bytes; i++) parent_save[i] = nt_rec[i];
    if (nt_read_record(ref, nt_child) != 0) return -1;
    if (nt_rec_is_dir(nt_child)) return -1;
    nt_free_data_runs(nt_find_attr(nt_child, NT_AT_DATA, 0));
    nt_wr16(nt_child + 0x16, 0);
    if (nt_write_record(ref, nt_child) != 0) return -1;
    nt_free_mft(ref);

    /* 恢复父目录记录删条目 */
    for (uint32_t i = 0; i < nt_rec_bytes; i++) nt_rec[i] = parent_save[i];
    if (in_indx) {
        if (nt_indx_remove(nt_rec, loc_vcn, loc_off) != 0) return -1;
    } else {
        if (nt_remove_entry(nt_rec, loc_off) != 0) return -1;
    }
    if (nt_write_record(parent, nt_rec) != 0) return -1;
    return nt_maps_save();
}

int ntfs_mkdir(const char *name) {
    char parent_path[256];
    char fname[256];
    if (nt_split_path(name, parent_path, sizeof(parent_path), fname) != 0)
        return -1;
    uint32_t name_len = 0;
    while (fname[name_len]) name_len++;

    if (nt_maps_load() != 0) return -1;
    uint32_t parent = nt_resolve_dir(parent_path);
    if (parent == 0) return -1;

    int in_indx;
    uint32_t loc_vcn, loc_off;
    if (nt_find_loc(nt_rec, fname, 0, &in_indx, &loc_vcn, &loc_off) == 1)
        return -1;                              /* 已存在 */
    (void)in_indx; (void)loc_vcn; (void)loc_off;

    uint32_t mftno = nt_alloc_mft();
    if (mftno == 0xFFFFFFFFu) return -1;

    nt_build_record(nt_newrec, mftno, 1, fname, name_len, parent, 0, 0, 0, 0, 0);
    if (nt_write_record(mftno, nt_newrec) != 0) {
        nt_free_mft(mftno);
        return -1;
    }
    if (nt_dir_insert(nt_rec, mftno, fname, name_len, 1, 0) != 0) {
        nt_free_mft(mftno);
        return -1;
    }
    if (nt_write_record(parent, nt_rec) != 0) return -1;
    return nt_maps_save();
}

/* 统计真实子项（跳过 "."/".."/DOS/内部条目）。发现 >=1 项即返回 1 早停。 */
static int nt_rmdir_count_cb(const char *name, uint32_t ref, int is_dir, void *ctx) {
    (void)name; (void)ref; (void)is_dir;
    int *c = (int *)ctx;
    if (++(*c) >= 1) return 1;            /* 非空：停止遍历 */
    return 0;
}

/* 删除一个空目录（语义对齐 ext4/F2FS 的 rmdir）：
 *   - 不存在 / 目标是普通文件 / 非空 / 根 "/" / "." / ".." -> -1 且零副作用
 *   - 成功：父目录索引项摘除并落盘（元数据先行），再回收目录的
 *     $INDEX_ALLOCATION 簇与 MFT 记录槽（含 $MFT 位图同步）
 * 落盘顺序严格为：父索引项 -> 子记录标记未用 -> 两张位图，任何一步
 * 失败均按镜像回滚到操作前状态，避免悬空引用 / 半提交。 */
int ntfs_rmdir(const char *path) {
    char parent_path[256];
    char fname[256];
    uint32_t name_len = 0;

    if (path == 0 || path[0] != '/') return -1;

    /* 抽取父路径与叶名；根 "/" 与无叶名情形由 split 失败覆盖 */
    if (nt_split_path(path, parent_path, sizeof(parent_path), fname) != 0)
        return -1;
    while (fname[name_len]) name_len++;
    if (name_len == 1 && fname[0] == '.') return -1;   /* "." */
    if (name_len == 2 && fname[0] == '.' && fname[1] == '.') return -1; /* ".." */

    if (nt_maps_load() != 0) return -1;

    uint32_t parent = nt_resolve_dir(parent_path);
    if (parent == 0) return -1;

    uint32_t ref;
    int in_indx;
    uint32_t loc_vcn, loc_off;
    if (nt_find_loc(nt_rec, fname, &ref, &in_indx, &loc_vcn, &loc_off) != 1)
        return -1;                       /* 不存在 */

    if (nt_read_record(ref, nt_child) != 0) return -1;
    if (!nt_rec_is_dir(nt_child)) return -1;   /* 目标是普通文件：归 rm 管 */

    /* 非空检查：本驱动目录无 "."/".." 条目，故真实子项数为 0 即空。
     * 非空时零释放、零磁盘写。 */
    int ent_count = 0;
    if (nt_dir_walk(nt_child, nt_rmdir_count_cb, &ent_count) != 0) return -1;
    if (ent_count > 0) return -1;              /* 非空 */

    /* ---- 以下进入变更路径，尚无任何磁盘写 ---- */
    /* 回滚镜像**运行时申请**，不放静态数组：
     * 静态会进 .bss.hi，而 linker.ld:51 给它的预算是 1MB，实测全内核已用到
     * 1043/1024 KB——再放 17KB 镜像直接 LINK FAILED（已经踩过一次）。 */
    if (nt_rec_bytes == 0 || nt_rec_bytes > NT_MAX_REC) return -1;
    if (nt_vbmp_len == 0 || nt_vbmp_len > NT_BITMAP_CAP) return -1;
    uint32_t mbmp_len = (uint32_t)sizeof(nt_mbmp);
    uint8_t *parent_save = (uint8_t *)kmalloc(nt_rec_bytes);
    if (!parent_save) return -1;
    uint8_t *child_save = (uint8_t *)kmalloc(nt_rec_bytes);
    if (!child_save) { kfree(parent_save); return -1; }
    uint8_t *vbmp_save = (uint8_t *)kmalloc(nt_vbmp_len);
    if (!vbmp_save) { kfree(child_save); kfree(parent_save); return -1; }
    uint8_t *mbmp_save = (uint8_t *)kmalloc(mbmp_len);
    if (!mbmp_save) {
        kfree(vbmp_save); kfree(child_save); kfree(parent_save);
        return -1;
    }
    for (uint32_t i = 0; i < nt_rec_bytes; i++) parent_save[i] = nt_rec[i];
    for (uint32_t i = 0; i < nt_rec_bytes; i++) child_save[i] = nt_child[i];
    for (uint32_t i = 0; i < nt_vbmp_len; i++) vbmp_save[i] = nt_vbmp[i];
    for (uint32_t i = 0; i < mbmp_len; i++) mbmp_save[i] = nt_mbmp[i];

    /* 1) 父目录索引项摘除（内存） */
    if (in_indx) {
        if (nt_indx_remove(nt_rec, loc_vcn, loc_off) != 0) goto fail_free;
    } else {
        if (nt_remove_entry(nt_rec, loc_off) != 0) goto fail_free;
    }

    /* 2) 回收目录的 $INDEX_ALLOCATION 簇（仅大目录有；小目录无，空操作） */
    const uint8_t *ia = nt_find_attr(nt_child, NT_AT_INDEX_ALLOC, "$I30");
    nt_free_data_runs(ia);

    /* 3) 标记子记录未用 + 回收 MFT 槽（内存） */
    nt_wr16(nt_child + 0x16, 0);
    nt_free_mft(ref);

    /* ---- 落盘：元数据（父索引项）先行，簇/MFT 位图最后 ---- */
    if (nt_write_record(parent, nt_rec) != 0) goto rollback;
    if (nt_write_record(ref, nt_child) != 0) goto rollback;
    if (nt_maps_save() != 0) goto rollback;

    kfree(mbmp_save); kfree(vbmp_save); kfree(child_save); kfree(parent_save);
    return 0;

rollback:
    /* 对称回滚：用镜像把父/子记录与两张位图恢复到操作前。
     * 这里不释放数据簇：簇的回收在 step 2/3 里只改了内存位图，
     * 只要 nt_maps_save() 没成功，磁盘上的位图还是原样。 */
    for (uint32_t i = 0; i < nt_rec_bytes; i++) nt_rec[i] = parent_save[i];
    if (nt_write_record(parent, nt_rec) == 0) {
        for (uint32_t i = 0; i < nt_rec_bytes; i++) nt_child[i] = child_save[i];
        (void)nt_write_record(ref, nt_child);
        for (uint32_t i = 0; i < nt_vbmp_len; i++) nt_vbmp[i] = vbmp_save[i];
        for (uint32_t i = 0; i < mbmp_len; i++) nt_mbmp[i] = mbmp_save[i];
        (void)nt_maps_save();
    }
    /* fallthrough */
fail_free:
    kfree(mbmp_save); kfree(vbmp_save); kfree(child_save); kfree(parent_save);
    return -1;
}

/* ============================================================
 * 格式化（mkntfs 标准结构，Windows/ntfs-3g 可读）
 * 布局（2MB 盘，4KB 簇 = 8 扇区，1KB MFT 记录，4KB INDX 块）：
 *   LCN 0:     BPB（分区 LBA 1）
 *   LCN 1-7:   保留全零
 *   LCN 8-11:  $MFT（16KB = 16 条 1024B 记录，记录 0-11 占用）
 *   LCN 12+:   数据簇
 * $Bitmap（记录 6）驻留 $DATA = 簇位图（bit i = LCN i）；
 * MFT 记录位图 = $MFT（记录 0）驻留 $BITMAP 属性（16 位）
 * ============================================================ */
int ntfs_format(uint8_t drive) {
    if (drive > 3) return -1;

    uint32_t part_start = 1;
    uint32_t spc = 8;                       /* 4KB 簇 */
    uint32_t total_secs = 4095;             /* 2MB 盘 - MBR */
    uint32_t total_clusters = total_secs / spc;   /* 511 */
    uint32_t mft_lcn = 8;
    uint32_t mft_clu = 4;                   /* 16KB / 4KB */
    uint32_t rec_bytes = 1024;
    uint32_t idx_bytes = 4096;              /* INDX 块 = 簇大小（mkntfs 同构） */
    uint32_t i;

    /* nt_build_record 依赖全局挂载参数：先按 format 布局就位
     * （末尾 ntfs_mount 会重新解析并覆盖） */
    nt_mounted = 0;
    nt_drive = drive;
    nt_part_lba = part_start;
    nt_spc = spc;
    nt_cluster_bytes = spc * 512;
    nt_rec_bytes = rec_bytes;
    nt_idx_bytes = idx_bytes;

    /* MBR: 分区 0x07 */
    uint8_t mbr[512];
    for (i = 0; i < 512; i++) mbr[i] = 0;
    mbr[450] = 0x07;
    nt_wr32(mbr + 454, part_start);
    nt_wr32(mbr + 458, total_secs);
    mbr[510] = 0x55; mbr[511] = 0xAA;
    if (ata_write_sector(drive, 0, mbr) != 0) return -1;

    /* BPB（LCN 0 = LBA 1） */
    uint8_t bs[512];
    for (i = 0; i < 512; i++) bs[i] = 0;
    bs[0] = 0xEB; bs[1] = 0x52; bs[2] = 0x90;
    bs[3] = 'N'; bs[4] = 'T'; bs[5] = 'F'; bs[6] = 'S';
    bs[7] = ' '; bs[8] = ' '; bs[9] = ' '; bs[10] = ' ';
    nt_wr16(bs + 0x0B, 512);
    bs[0x0D] = (uint8_t)spc;
    nt_wr64(bs + 0x28, total_secs);
    nt_wr64(bs + 0x30, mft_lcn);            /* $MFT LCN */
    nt_wr64(bs + 0x38, mft_lcn + mft_clu);  /* $MFTMirr（无实际内容） */
    bs[0x40] = (uint8_t)(int8_t)-10;        /* 1024B MFT 记录 */
    bs[0x41] = (uint8_t)(int8_t)-12;        /* 4096B INDX 块（=簇） */
    bs[510] = 0x55; bs[511] = 0xAA;
    if (ata_write_sector(drive, part_start, bs) != 0) return -1;

    /* 清零 MFT 区域（LCN 8-11）与其余保留簇 */
    uint8_t zero[512];
    for (i = 0; i < 512; i++) zero[i] = 0;
    for (i = 1; i < 12 * spc; i++)
        if (ata_write_sector(drive, part_start + i, zero) != 0) return -1;

    /* $Bitmap 数据（驻留于记录 6）：纯簇位图，LCN 0-11 用 */
    uint8_t vbmp[NT_BITMAP_CAP];
    for (i = 0; i < NT_BITMAP_CAP; i++) vbmp[i] = 0;
    uint32_t bmp_total = (total_clusters + 7) / 8;
    vbmp[0] = 0xFF; vbmp[1] = 0x0F;         /* 簇 0-11 */

    /* ---- 构造 MFT 记录 ---- */
    uint8_t rec[NT_MAX_REC];

    /* 记录 0：$MFT，非常驻 $DATA（4 簇 @ LCN 8） */
    {
        /* 通用记录头 + fixup 占位 */
        for (i = 0; i < rec_bytes; i++) rec[i] = 0;
        nt_wr32(rec, 0x454C4946u);
        nt_wr16(rec + 4, 0x30);
        nt_wr16(rec + 6, rec_bytes / 512 + 1);
        nt_wr16(rec + 0x10, 1);
        nt_wr16(rec + 0x12, 1);
        nt_wr16(rec + 0x14, 0x36);
        nt_wr32(rec + 0x1C, rec_bytes);
        nt_wr32(rec + 0x20, 0);
        uint32_t off = 0x36;

        /* $STANDARD_INFORMATION */
        nt_wr32(rec + off, NT_AT_STANDARD_INFO);
        nt_wr32(rec + off + 4, 0x18 + 48);
        nt_wr32(rec + off + 0x10, 48);
        nt_wr32(rec + off + 0x14, 0x18);
        off += 0x18 + 48;

        /* $DATA 非常驻：run = 4 簇 @ LCN 8 */
        uint8_t *a = rec + off;
        nt_wr32(a, NT_AT_DATA);
        nt_wr32(a + 4, 0x40 + 5);
        a[8] = 1;
        nt_wr64(a + 0x10, 0);
        nt_wr64(a + 0x18, mft_clu - 1);
        nt_wr16(a + 0x20, 0x40);
        nt_wr64(a + 0x28, (uint64_t)mft_clu * spc * 512);
        nt_wr64(a + 0x30, (uint64_t)mft_clu * spc * 512);
        nt_wr64(a + 0x38, (uint64_t)mft_clu * spc * 512);
        a[0x40] = 0x21;
        a[0x41] = mft_clu;
        nt_wr16(a + 0x42, mft_lcn);
        a[0x44] = 0;
        off += 0x40 + 5;

        /* $BITMAP 驻留（无属性名）：MFT 记录位图，16 位 = 记录 0-11 用 */
        a = rec + off;
        nt_wr32(a, NT_AT_BITMAP);
        nt_wr32(a + 4, 0x18 + 2);
        a[8] = 0; a[9] = 0;
        nt_wr16(a + 0xA, 0); nt_wr16(a + 0xC, 0);
        nt_wr32(a + 0x10, 2);
        nt_wr32(a + 0x14, 0x18);
        a[0x18] = 0xFF; a[0x19] = 0x0F;
        off += 0x18 + 2;

        nt_wr32(rec + off, 0xFFFFFFFFu);
        nt_wr32(rec + off + 4, 8);
        off += 8;
        nt_wr32(rec + 0x18, off);
        nt_wr16(rec + 0x16, 1);
    }
    /* 直接写记录 0 的原始字节（无 fixup，稍后统一重读；先存缓冲数组） */
    /* 为简化：先把 16 条记录在内存中备好再统一落盘 */

    static uint8_t mft_all[16 * NT_MAX_REC] NT_HIBUF;
    for (i = 0; i < 16 * NT_MAX_REC; i++) mft_all[i] = 0;

    /* 记录 0（上面 rec 即为 $MFT） */
    for (i = 0; i < rec_bytes; i++) mft_all[i] = rec[i];

    /* 记录 1-4, 7-11：空占位（in-use，仅 $END） */
    for (uint32_t r = 1; r < 12; r++) {
        if (r == 5 || r == 6) continue;
        uint8_t *p = mft_all + r * rec_bytes;
        nt_wr32(p, 0x454C4946u);
        nt_wr16(p + 4, 0x30);
        nt_wr16(p + 6, rec_bytes / 512 + 1);
        nt_wr16(p + 0x10, 1);
        nt_wr16(p + 0x12, 1);
        nt_wr16(p + 0x14, 0x36);
        nt_wr16(p + 0x16, 1);
        nt_wr32(p + 0x18, 0x36 + 8);
        nt_wr32(p + 0x1C, rec_bytes);
        nt_wr32(p + 0x20, r);
        nt_wr32(p + 0x36, 0xFFFFFFFFu);
        nt_wr32(p + 0x3A, 8);
    }

    /* 记录 5：根目录（SI + FILE_NAME(".") + INDEX_ROOT 空） */
    nt_build_record(mft_all + 5 * rec_bytes, 5, 1, ".", 1, 5, 0, 0, 0, 0, 0);

    /* 记录 6：$Bitmap（SI + 驻留 $DATA = vbmp） */
    {
        uint8_t *p = mft_all + 6 * rec_bytes;
        nt_wr32(p, 0x454C4946u);
        nt_wr16(p + 4, 0x30);
        nt_wr16(p + 6, rec_bytes / 512 + 1);
        nt_wr16(p + 0x10, 1);
        nt_wr16(p + 0x12, 1);
        nt_wr16(p + 0x14, 0x36);
        nt_wr16(p + 0x16, 1);
        nt_wr32(p + 0x1C, rec_bytes);
        nt_wr32(p + 0x20, 6);
        uint32_t off = 0x36;
        /* SI */
        nt_wr32(p + off, NT_AT_STANDARD_INFO);
        nt_wr32(p + off + 4, 0x18 + 48);
        nt_wr32(p + off + 0x10, 48);
        nt_wr32(p + off + 0x14, 0x18);
        off += 0x18 + 48;
        /* $DATA 驻留 */
        nt_wr32(p + off, NT_AT_DATA);
        nt_wr32(p + off + 4, (0x18 + bmp_total + 7) & ~7u);
        p[off + 8] = 0;
        nt_wr32(p + off + 0x10, bmp_total);
        nt_wr32(p + off + 0x14, 0x18);
        for (i = 0; i < bmp_total; i++) p[off + 0x18 + i] = vbmp[i];
        off += (0x18 + bmp_total + 7) & ~7u;
        nt_wr32(p + off, 0xFFFFFFFFu);
        nt_wr32(p + off + 4, 8);
        off += 8;
        nt_wr32(p + 0x18, off);
    }

    /* 记录统一落盘：对每条记录应用 fixup（USN 起始 3）后写扇区。
     * nt_write_record 依赖已挂载状态，这里手动做 fixup + 直接写。 */
    uint32_t mft_lba = part_start + mft_lcn * spc;
    for (uint32_t r = 0; r < 16; r++) {
        uint8_t *p = mft_all + r * rec_bytes;
        if (rd32(p) != 0x454C4946u) continue;
        /* fixup：USN = 3 + r */
        uint16_t usn = (uint16_t)(3 + r);
        uint8_t *pu = p + rd16(p + 4);
        pu[0] = (uint8_t)(usn & 0xFF);
        pu[1] = (uint8_t)((usn >> 8) & 0xFF);
        uint32_t cnt = rd16(p + 6);
        for (uint32_t s = 0; s + 1 < cnt; s++) {
            uint8_t *tail = p + s * 512 + 510;
            pu[2 + s * 2] = tail[0];
            pu[3 + s * 2] = tail[1];
            tail[0] = pu[0];
            tail[1] = pu[1];
        }
        for (uint32_t s = 0; s < rec_bytes / 512; s++)
            if (ata_write_sector(drive, mft_lba + r * (rec_bytes / 512) + s,
                                 p + s * 512) != 0) return -1;
    }

    /* 挂载 */
    if (ntfs_mount(drive, part_start) != 0) return -1;
    return 0;
}
