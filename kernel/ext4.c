/*
 * ext4.c - ext2/ext3/ext4 只读驱动
 *
 * 支持范围（只读）：
 *   - 块大小 1024/2048/4096
 *   - extent 映射（ext4）与旧式直接/间接块（ext2/3）
 *   - 线性目录与 htree(dx) 索引目录（含 indirect_levels 递归）
 *   - 64bit 特性（高位块数/组描述符）
 *
 * refs:
 *   - GRUB grub-core/fs/ext2.c (GPLv3+) - extent 树/htree 只读遍历逻辑
 *   - Linux fs/ext4/{ext4.h,extents.c,namei.c,inode.c} - 磁盘结构与遍历规则
 *   - e2fsprogs lib/ext2fs/ext2_fs.h - superblock/group desc 字段偏移
 */
#include "ext4.h"
#include "ata.h"

/* ---------- 小端读取助手（无对齐/别名问题） ---------- */
static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---------- superblock / group desc 字段偏移（e2fsprogs ext2_fs.h） ---------- */
#define SB_OFF_BLOCKS_LO     4     /* s_blocks_count_lo */
#define SB_OFF_FREE_BLKS_LO  12    /* s_free_blocks_count_lo */
#define SB_OFF_FIRST_DATA    20    /* s_first_data_block */
#define SB_OFF_LOG_BLK       24    /* s_log_block_size */
#define SB_OFF_BPG           32    /* s_blocks_per_group */
#define SB_OFF_IPG           40    /* s_inodes_per_group */
#define SB_OFF_MAGIC         56    /* s_magic 0xEF53 */
#define SB_OFF_INODE_SIZE    88    /* s_inode_size */
#define SB_OFF_FEAT_INCOMPAT 96
#define SB_OFF_DESC_SIZE     282   /* 0x11A s_desc_size（64bit 组描述符） */
#define SB_OFF_BLOCKS_HI     336   /* 0x150 s_blocks_count_hi */
#define SB_OFF_FREE_BLKS_HI  344

#define GD_OFF_INO_TABLE_LO  8     /* bg_inode_table_lo */
#define GD_OFF_INO_TABLE_HI  40    /* bg_inode_table_hi（64bit desc） */

/* ---------- inode 字段偏移 ---------- */
#define INO_OFF_MODE         0
#define INO_OFF_SIZE_LO      4
#define INO_OFF_BLOCKS_LO    28    /* i_blocks_lo（512B 扇区数） */
#define INO_OFF_FLAGS        32
#define INO_OFF_IBLOCK       40    /* i_block[15]，60 字节 */
#define INO_OFF_SIZE_HI      108

#define EXT4_EXTENTS_FL      0x80000u
#define EXT4_INDEX_FL        0x1000u
#define EXT4_INLINE_DATA_FL  0x10000000u

/* incompat 特性位 */
#define EXT4F_INCOMPAT_FILETYPE  0x0002u
#define EXT4F_INCOMPAT_EXTENTS   0x0040u
#define EXT4F_INCOMPAT_64BIT     0x0080u
#define EXT4F_INCOMPAT_FLEX_BG   0x0200u
/* 读不了/不安全的一律拒绝挂载 */
#define EXT4F_INCOMPAT_REJECT   (0x0001u /* COMPR */ | 0x0010u /* META_BG */ \
                                 | 0x0100u /* MMP */ | 0x10000u /* ENCRYPT */ \
                                 | 0x20000u /* CASEFOLD */ | 0x40000u /* VERITY */)
/* INLINE_DATA(0x8000) 挂载不拒绝，读到 inline 文件时报错 */

#define EXT4_ROOT_INO        2
#define EXT4_MAGIC           0xEF53u

/* extent 头（位于 i_block 或索引块开头） */
#define EH_OFF_MAGIC         0     /* 0xF30A */
#define EH_OFF_ENTRIES       2
#define EH_OFF_DEPTH         6
#define EXT4_EXT_MAGIC       0xF30Au
/* extent 项（叶子，12 字节） */
#define EE_OFF_BLOCK         0     /* 逻辑块号 */
#define EE_OFF_LEN           4
#define EE_OFF_START_HI      6
#define EE_OFF_START_LO      8
/* index 项（内部，12 字节） */
#define EI_OFF_BLOCK         0
#define EI_OFF_LEAF_LO       4
#define EI_OFF_LEAF_HI       6
#define EXT4_EXT_LEN(l)      ((l) & 0x7FFFu)   /* 高位为 1 表示 unwritten */

/* 目录项 */
#define DE_OFF_INODE         0
#define DE_OFF_RECLEN        4
#define DE_OFF_NAMELEN       6
#define DE_OFF_TYPE          7
#define DE_OFF_NAME          8

/* htree dx_root：'.','..'(24) + dx_root_info(8) @24 + entries @32 */
#define DX_ROOT_INFO         24
#define DX_INFO_LEVELS       (DX_ROOT_INFO + 3)
#define DX_ROOT_CNT_OFF      34    /* entries[0].hash 被 countlimit 覆盖 */
#define DX_ROOT_BLK(i)       (36 + (i) * 8)
/* dx_node：fake dirent(8)+reserved(4)，entries @12 */
#define DX_NODE_CNT_OFF      14
#define DX_NODE_BLK(i)       (16 + (i) * 8)
#define DX_MAX_LEAVES        64

#define EXT4_MAX_BLOCKSIZE   4096
#define EXT4_MAX_INODESIZE   256

/* ---------- 挂载状态 ---------- */
static uint8_t  e4_drive;
static uint8_t  e4_mounted;
static uint32_t e4_part_lba;            /* 卷起始 LBA */
static uint32_t e4_blksize;             /* 1024<<log */
static uint32_t e4_blk_per_sec;         /* blocksize/512 */
static uint32_t e4_bpg, e4_ipg, e4_ino_size;
static uint32_t e4_first_data_blk;
static uint32_t e4_desc_size;           /* 32 或 64 */
static uint64_t e4_blocks_total;
static uint32_t e4_blocks_free;
static uint32_t e4_vol_sectors;
static uint32_t e4_gdt_blk;             /* 组描述符表起始块 */
static ext4_info_t e4_info;

/* 通用块缓冲（单线程内核，非重入安全由调用约定保证）。
 * e4_blk 仅作 map_extent/read_inode 内部暂存；e4_dblk 专用于目录项扫描
 * （目录回调会再读子 inode，二者必须隔离）；e4_dxbuf 专用于 dx 树遍历。 */
/* 大缓冲放高内存段 .bss.hi（1MB+，见 linker.ld）：低 640KB 区留给栈/小数据 */
#define E4_HIBUF __attribute__((section(".bss.hi")))
static uint8_t e4_blk[EXT4_MAX_BLOCKSIZE] E4_HIBUF;
static uint8_t e4_dblk[EXT4_MAX_BLOCKSIZE] E4_HIBUF;
static uint8_t e4_dxbuf[EXT4_MAX_BLOCKSIZE] E4_HIBUF;
static uint8_t e4_ind[2][EXT4_MAX_BLOCKSIZE] E4_HIBUF;  /* 间接块链缓冲 */

static void e4_read_secs(uint32_t lba, uint8_t *buf, uint32_t nsecs) {
    for (uint32_t i = 0; i < nsecs; i++)
        ata_read_sector(e4_drive, lba + i, buf + i * 512);
}

/* 读第 n 块文件系统块到 buf */
static void e4_read_blk(uint32_t blkno, uint8_t *buf) {
    e4_read_secs(e4_part_lba + blkno * e4_blk_per_sec, buf, e4_blk_per_sec);
}

int ext4_mount(uint8_t drive, uint32_t part_start) {
    static uint8_t sb[1024];
    e4_mounted = 0;
    /* superblock 恒在卷内字节偏移 1024 = LBA+2 */
    e4_drive = drive;
    e4_read_secs(part_start + 2, sb, 2);
    if (rd16(sb + SB_OFF_MAGIC) != EXT4_MAGIC) return -1;

    uint32_t logb = rd32(sb + SB_OFF_LOG_BLK);
    if (logb > 2) return -1;                     /* 1024/2048/4096 */
    e4_blksize = 1024u << logb;
    e4_blk_per_sec = e4_blksize >> 9;

    e4_ino_size = rd16(sb + SB_OFF_INODE_SIZE);
    if (e4_ino_size != 128 && e4_ino_size != 256) return -1;

    uint32_t incompat = rd32(sb + SB_OFF_FEAT_INCOMPAT);
    if (incompat & EXT4F_INCOMPAT_REJECT) return -1;

    e4_bpg = rd32(sb + SB_OFF_BPG);
    e4_ipg = rd32(sb + SB_OFF_IPG);
    if (e4_bpg == 0 || e4_ipg == 0) return -1;

    e4_desc_size = rd16(sb + SB_OFF_DESC_SIZE);
    if (incompat & EXT4F_INCOMPAT_64BIT) {
        if (e4_desc_size < 64) e4_desc_size = 64;
    } else {
        e4_desc_size = 32;
    }

    e4_blocks_total = rd32(sb + SB_OFF_BLOCKS_LO);
    e4_blocks_free = rd32(sb + SB_OFF_FREE_BLKS_LO);
    if (incompat & EXT4F_INCOMPAT_64BIT) {
        e4_blocks_total |= (uint64_t)rd32(sb + SB_OFF_BLOCKS_HI) << 32;
        e4_blocks_free |= (uint64_t)rd32(sb + SB_OFF_FREE_BLKS_HI) << 32;
    }

    e4_first_data_blk = rd32(sb + SB_OFF_FIRST_DATA);
    e4_vol_sectors = (uint32_t)(e4_blocks_total * e4_blk_per_sec);
    /* GDT：superblock 所在块（1K 块时 SB 在第 1 块，否则第 0 块）的下一块 */
    e4_gdt_blk = (e4_blksize == 1024) ? 2 : 1;

    e4_part_lba = part_start;
    e4_mounted = 1;

    e4_info.part_start = part_start;
    e4_info.bytes_per_sector = 512;
    e4_info.sectors_per_cluster = (uint8_t)e4_blk_per_sec;
    e4_info.cluster_count = (uint32_t)e4_blocks_total;
    e4_info.volume_sectors = e4_vol_sectors;
    e4_info.used_clusters = (uint32_t)(e4_blocks_total - e4_blocks_free);
    return 0;
}

const ext4_info_t *ext4_get_info(void) { return &e4_info; }

/* ---------- inode ---------- */
static int e4_read_inode(uint32_t ino, uint8_t *out) {
    if (!e4_mounted || ino == 0) return -1;
    uint32_t group = (ino - 1) / e4_ipg;
    uint32_t idx = (ino - 1) % e4_ipg;
    uint32_t groups = (uint32_t)((e4_blocks_total + e4_bpg - 1) / e4_bpg);
    if (group >= groups) return -1;

    /* 组描述符可能跨块：按字节偏移读取 */
    uint64_t gd_off = (uint64_t)group * e4_desc_size;
    uint32_t gd_blk = e4_gdt_blk + (uint32_t)(gd_off / e4_blksize);
    uint32_t gd_in = (uint32_t)(gd_off % e4_blksize);
    e4_read_blk(gd_blk, e4_blk);
    uint64_t tab = rd32(e4_blk + gd_in + GD_OFF_INO_TABLE_LO);
    if (e4_desc_size == 64)
        tab |= (uint64_t)rd32(e4_blk + gd_in + GD_OFF_INO_TABLE_HI) << 32;
    if (tab == 0 || tab > 0xFFFFFFFFu) return -1;   /* 卷超出 32 位块寻址 */
    uint32_t ino_table = (uint32_t)tab;

    uint64_t ioff = (uint64_t)idx * e4_ino_size;
    uint32_t iblk = ino_table + (uint32_t)(ioff / e4_blksize);
    uint32_t iin = (uint32_t)(ioff % e4_blksize);
    if (iin + e4_ino_size > e4_blksize) return -1;   /* 不跨块（mkfs 保证） */
    e4_read_blk(iblk, e4_blk);
    for (uint32_t i = 0; i < e4_ino_size; i++)
        out[i] = e4_blk[iin + i];
    return 0;
}

static uint32_t e4_ino_size_of(const uint8_t *ino, uint32_t mode) {
    uint32_t lo = rd32(ino + INO_OFF_SIZE_LO);
    uint32_t hi = rd32(ino + INO_OFF_SIZE_HI);
    /* 仅普通文件的 i_size 字段为 64 位（i_dir_acl 历史字段） */
    if ((mode & 0xF000u) == 0x8000u && hi != 0)
        return lo;                                   /* >4GB 文件截断为低 32 位 */
    (void)hi;
    return lo;
}

static int e4_ino_is_dir(const uint8_t *ino) {
    return (rd16(ino + INO_OFF_MODE) & 0xF000u) == 0x4000u;
}

/* ---------- 块映射 ---------- */

/* extent 树：自顶向下迭代下降（深度 <=5） */
static uint32_t e4_map_extent(const uint8_t *ino, uint32_t lblk) {
    /* 根节点（depth 最大）在 inode i_block 前 60 字节 */
    const uint8_t *hdr = ino + INO_OFF_IBLOCK;
    for (int depth_iter = 0; depth_iter < 6; depth_iter++) {
        if (rd16(hdr) != EXT4_EXT_MAGIC) return 0;
        uint32_t entries = rd16(hdr + EH_OFF_ENTRIES);
        uint32_t depth = rd16(hdr + EH_OFF_DEPTH);
        if (entries == 0) return 0;

        if (depth == 0) {
            for (uint32_t i = 0; i < entries; i++) {
                const uint8_t *ex = hdr + 12 + i * 12;
                uint32_t ex_blk = rd32(ex + EE_OFF_BLOCK);
                uint32_t ex_len = EXT4_EXT_LEN(rd16(ex + EE_OFF_LEN));
                if (lblk >= ex_blk && lblk < ex_blk + ex_len && ex_len) {
                    uint64_t start = (uint64_t)rd32(ex + EE_OFF_START_LO) |
                                     ((uint64_t)rd16(ex + EE_OFF_START_HI) << 32);
                    return (uint32_t)(start + (lblk - ex_blk));
                }
            }
            return 0;                                /* 洞 */
        }
        /* 内部节点：找覆盖 lblk 的最后一个 index */
        uint64_t child = 0;
        for (uint32_t i = 0; i < entries; i++) {
            const uint8_t *ix = hdr + 12 + i * 12;
            if (rd32(ix + EI_OFF_BLOCK) <= lblk)
                child = (uint64_t)rd32(ix + EI_OFF_LEAF_LO) |
                        ((uint64_t)rd16(ix + EI_OFF_LEAF_HI) << 32);
            else break;
        }
        if (child == 0 || child > 0xFFFFFFFFu) return 0;
        e4_read_blk((uint32_t)child, e4_blk);
        hdr = e4_blk;
    }
    return 0;
}

/* 旧式直接/间接块 */
static uint32_t e4_map_legacy(const uint8_t *ino, uint32_t lblk) {
    const uint8_t *ib = ino + INO_OFF_IBLOCK;
    uint32_t per_blk = e4_blksize / 4;
    if (lblk < 12)
        return rd32(ib + lblk * 4);
    lblk -= 12;
    /* 一级间接 i_block[12] */
    if (lblk < per_blk) {
        uint32_t ind = rd32(ib + 12 * 4);
        if (!ind) return 0;
        e4_read_blk(ind, e4_ind[0]);
        return rd32(e4_ind[0] + lblk * 4);
    }
    lblk -= per_blk;
    /* 二级间接 i_block[13] */
    if (lblk < per_blk * per_blk) {
        uint32_t ind = rd32(ib + 13 * 4);
        if (!ind) return 0;
        e4_read_blk(ind, e4_ind[0]);
        uint32_t ind2 = rd32(e4_ind[0] + (lblk / per_blk) * 4);
        if (!ind2) return 0;
        e4_read_blk(ind2, e4_ind[1]);
        return rd32(e4_ind[1] + (lblk % per_blk) * 4);
    }
    lblk -= per_blk * per_blk;
    /* 三级间接 i_block[14]（>4GB 文件才用到，仍支持） */
    {
        uint32_t ind = rd32(ib + 14 * 4);
        if (!ind) return 0;
        e4_read_blk(ind, e4_ind[0]);
        uint32_t off2 = lblk / per_blk;
        uint32_t ind2 = rd32(e4_ind[0] + (off2 / per_blk) * 4);
        if (!ind2) return 0;
        e4_read_blk(ind2, e4_ind[1]);
        uint32_t ind3 = rd32(e4_ind[1] + (off2 % per_blk) * 4);
        if (!ind3) return 0;
        e4_read_blk(ind3, e4_ind[0]);
        return rd32(e4_ind[0] + (lblk % per_blk) * 4);
    }
}

static uint32_t e4_map_block(const uint8_t *ino, uint32_t lblk) {
    if (rd32(ino + INO_OFF_FLAGS) & EXT4_EXTENTS_FL)
        return e4_map_extent(ino, lblk);
    return e4_map_legacy(ino, lblk);
}

/* ---------- 目录遍历 ---------- */

/* 迭代 DFS 收集 dx 树全部叶子块号（根 indirect_levels 层内部节点之下）。
 * 不能递归共享缓冲：子调用会覆盖父节点块，故用显式栈 + 独立 e4_dxbuf。 */
static int e4_dx_collect(uint32_t root_blk, uint32_t *out, int *n, int max) {
    e4_read_blk(root_blk, e4_dxbuf);
    uint32_t levels = e4_dxbuf[DX_INFO_LEVELS];      /* indirect_levels */
    if (levels > 5) return -1;

    struct { uint32_t blk; uint32_t idx; } stk[6];
    int sp = 0;
    stk[0].blk = root_blk;
    stk[0].idx = 0;

    while (sp >= 0) {
        e4_read_blk(stk[sp].blk, e4_dxbuf);
        int is_root = (sp == 0);
        /* 根块：'.','..' 24 字节 + info@24 + entries@32；节点块：12 字节头 + entries@12 */
        uint32_t cnt = rd16(e4_dxbuf + (is_root ? DX_ROOT_CNT_OFF : DX_NODE_CNT_OFF));
        uint32_t base = is_root ? 36 : 16;

        if ((uint32_t)sp == levels) {
            /* 叶子层：全部收下 */
            for (uint32_t i = 0; i < cnt && *n < max; i++)
                out[(*n)++] = rd32(e4_dxbuf + base + i * 8);
            sp--;
            continue;
        }
        /* 内部层：处理下一个未访问的子节点 */
        if (stk[sp].idx < cnt) {
            uint32_t child = rd32(e4_dxbuf + base + stk[sp].idx * 8);
            stk[sp].idx++;
            sp++;
            stk[sp].blk = child;
            stk[sp].idx = 0;
        } else {
            sp--;
        }
    }
    return 0;
}

/* 目录回调：返回 1 停止遍历 */
typedef int (*e4_dir_cb)(const char *name, uint32_t name_len, uint32_t ino,
                         uint8_t ftype, void *ctx);

static int e4_dir_walk(uint32_t dir_ino, e4_dir_cb cb, void *ctx) {
    static uint8_t dino[EXT4_MAX_INODESIZE];
    if (e4_read_inode(dir_ino, dino) != 0) return -1;
    if (!e4_ino_is_dir(dino)) return -1;

    uint32_t size = e4_ino_size_of(dino, rd16(dino + INO_OFF_MODE));
    if (rd32(dino + INO_OFF_FLAGS) & EXT4_INLINE_DATA_FL) return -1;
    uint32_t nblk = (size + e4_blksize - 1) / e4_blksize;
    if (nblk == 0) return 0;

    /* htree 索引目录：收集全部叶子块后线性扫描 */
    uint32_t leaves[DX_MAX_LEAVES];
    int nleaf = 0;
    int indexed = (rd32(dino + INO_OFF_FLAGS) & EXT4_INDEX_FL) && nblk > 0;
    if (indexed) {
        /* 先试探第一块是否真是 dx 根（小目录可能仅置位未建树） */
        uint32_t blk0 = e4_map_block(dino, 0);
        if (blk0) {
            e4_read_blk(blk0, e4_dblk);
            /* dx 根特征：'..' 的 rec_len == blocksize - 12 */
            if (rd16(e4_dblk + 12 + DE_OFF_RECLEN) == e4_blksize - 12) {
                if (e4_dx_collect(blk0, leaves, &nleaf, DX_MAX_LEAVES) != 0)
                    return -1;
            } else {
                indexed = 0;
            }
        } else {
            indexed = 0;
        }
    }

    uint32_t scan_blocks = indexed ? (uint32_t)nleaf : nblk;
    for (uint32_t b = 0; b < scan_blocks; b++) {
        uint32_t pb = indexed ? leaves[b] : e4_map_block(dino, b);
        if (pb == 0) continue;                       /* 洞：跳过 */
        /* 目录项扫描用独立缓冲：回调（读子 inode）会破坏 e4_blk */
        e4_read_blk(pb, e4_dblk);
        uint32_t off = 0;
        while (off + 8 <= e4_blksize) {
            uint32_t rec_len = rd16(e4_dblk + off + DE_OFF_RECLEN);
            if (rec_len < 8 || off + rec_len > e4_blksize) break;
            uint32_t child = rd32(e4_dblk + off + DE_OFF_INODE);
            uint32_t name_len = e4_dblk[off + DE_OFF_NAMELEN];
            uint8_t ftype = e4_dblk[off + DE_OFF_TYPE];
            if (child != 0 && name_len > 0 && off + 8 + name_len <= e4_blksize) {
                char namebuf[256];
                if (name_len > 255) name_len = 255;
                for (uint32_t i = 0; i < name_len; i++)
                    namebuf[i] = (char)e4_dblk[off + DE_OFF_NAME + i];
                namebuf[name_len] = 0;
                /* 跳过 "." / ".." */
                if (!(name_len == 1 && namebuf[0] == '.') &&
                    !(name_len == 2 && namebuf[0] == '.' && namebuf[1] == '.')) {
                    if (cb(namebuf, name_len, child, ftype, ctx)) return 0;
                }
            }
            off += rec_len;
        }
    }
    return 0;
}

/* ---------- 路径解析 ---------- */
typedef struct {
    const char *name;      /* 待查组件 */
    uint32_t name_len;
    uint32_t found_ino;
    uint8_t  found_type;   /* dirent file_type: 1=reg 2=dir */
    int      found;
} e4_lookup_ctx;

static char e4_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* 本 OS 约定文件名 ASCII 大小写不敏感（与 exFAT/FAT 一致；区别于 Linux 原生 ext4） */
static int e4_lookup_cb(const char *name, uint32_t name_len, uint32_t ino,
                        uint8_t ftype, void *ctx) {
    e4_lookup_ctx *c = (e4_lookup_ctx *)ctx;
    if (name_len == c->name_len) {
        int same = 1;
        for (uint32_t i = 0; i < name_len; i++) {
            if (e4_lower(name[i]) != e4_lower(c->name[i])) { same = 0; break; }
        }
        if (same) {
            c->found_ino = ino;
            c->found_type = ftype;
            c->found = 1;
            return 1;
        }
    }
    return 0;
}

/* 解析绝对路径 -> inode；成功返回 inode 号并填 *is_dir 与 *size_out(可空) */
static uint32_t e4_resolve(const char *path, int *is_dir, uint32_t *size_out,
                           const uint8_t **ino_out) {
    static uint8_t ino[EXT4_MAX_INODESIZE];
    if (!e4_mounted || path == 0 || path[0] != '/') return 0;
    uint32_t cur = EXT4_ROOT_INO;

    while (*path == '/') path++;
    while (*path) {
        /* 取一个组件 */
        const char *comp = path;
        uint32_t clen = 0;
        while (path[clen] && path[clen] != '/') clen++;
        if (clen == 0 || clen > 255) return 0;

        e4_lookup_ctx ctx;
        ctx.name = comp;
        ctx.name_len = clen;
        ctx.found = 0;
        if (e4_dir_walk(cur, e4_lookup_cb, &ctx) != 0) return 0;
        if (!ctx.found) return 0;
        cur = ctx.found_ino;
        path += clen;
        while (*path == '/') path++;
    }

    if (e4_read_inode(cur, ino) != 0) return 0;
    uint32_t mode = rd16(ino + INO_OFF_MODE);
    if (is_dir) *is_dir = e4_ino_is_dir(ino);
    if (size_out) *size_out = e4_ino_size_of(ino, mode);
    if (ino_out) *ino_out = ino;
    return cur;
}

/* ---------- 对外 API ---------- */
int ext4_is_dir(const char *path) {
    int is_dir;
    if (e4_resolve(path, &is_dir, 0, 0) == 0) return -1;
    return is_dir;
}

uint32_t ext4_get_file_size(const char *path) {
    uint32_t size;
    if (e4_resolve(path, 0, &size, 0) == 0) return 0;
    return size;
}

int ext4_read_file(const char *path, uint8_t *buffer, uint32_t max_size) {
    const uint8_t *ino;
    uint32_t size;
    if (e4_resolve(path, 0, &size, &ino) == 0) return -1;
    if ((rd16(ino + INO_OFF_MODE) & 0xF000u) != 0x8000u) return -1;
    if (rd32(ino + INO_OFF_FLAGS) & EXT4_INLINE_DATA_FL) return -1;
    if (size > max_size) size = max_size;

    uint32_t done = 0;
    while (done < size) {
        uint32_t lblk = done / e4_blksize;
        uint32_t chunk = e4_blksize - (done % e4_blksize);
        if (chunk > size - done) chunk = size - done;
        uint32_t pb = e4_map_block(ino, lblk);
        if (pb == 0) {
            /* 洞：零填充 */
            for (uint32_t i = 0; i < chunk; i++) buffer[done + i] = 0;
        } else {
            uint32_t in_off = done % e4_blksize;
            if (in_off == 0 && chunk == e4_blksize) {
                e4_read_blk(pb, buffer + done);      /* 整块直达 */
            } else {
                e4_read_blk(pb, e4_blk);
                for (uint32_t i = 0; i < chunk; i++)
                    buffer[done + i] = e4_blk[in_off + i];
            }
        }
        done += chunk;
    }
    return (int)size;
}

typedef struct {
    fs_dir_entry_t *entries;
    int max, n;
} e4_fill_ctx;

static int e4_fill_cb(const char *name, uint32_t name_len, uint32_t ino,
                      uint8_t ftype, void *ctx) {
    (void)ftype;    /* file_type 可能为 0（无 FILETYPE 特性的 ext2），以 inode 模式为准 */
    e4_fill_ctx *c = (e4_fill_ctx *)ctx;
    if (c->n >= c->max) return 1;
    fs_dir_entry_t *e = &c->entries[c->n++];
    for (uint32_t i = 0; i < name_len && i < 255; i++) e->name[i] = name[i];
    e->name[name_len > 255 ? 255 : name_len] = 0;
    e->is_dir = 0;
    e->size = 0;
    static uint8_t cino[EXT4_MAX_INODESIZE];
    if (e4_read_inode(ino, cino) == 0) {
        e->is_dir = e4_ino_is_dir(cino);
        e->size = e4_ino_size_of(cino, rd16(cino + INO_OFF_MODE));
    }
    return 0;
}

int ext4_read_dir(const char *path, fs_dir_entry_t *entries, int max_entries) {
    int is_dir;
    uint32_t dir = e4_resolve(path, &is_dir, 0, 0);
    if (dir == 0 || !is_dir) return -1;
    e4_fill_ctx ctx;
    ctx.entries = entries;
    ctx.max = max_entries;
    ctx.n = 0;
    if (e4_dir_walk(dir, e4_fill_cb, &ctx) != 0) return -1;
    return ctx.n;
}

uint32_t ext4_get_file_clusters(const char *path) {
    const uint8_t *ino;
    if (e4_resolve(path, 0, 0, &ino) == 0) return 0;
    /* i_blocks 以 512B 扇区计（含元数据块），换算为块数 */
    uint32_t secs = rd32(ino + INO_OFF_BLOCKS_LO);
    return secs / e4_blk_per_sec;
}
