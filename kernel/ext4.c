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
    static uint8_t sb[1024] E4_HIBUF;
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

/* ============================================================
 * 写入支持
 * refs: e2fsprogs lib/ext2fs/{alloc_tables.c,block.c,dir_block.c,
 *       mkdir.c,expand_dir.c,free.c,new_inode.c} 算法参考
 *
 * 设计（结构全为标准 ext4，Linux/e2fsprogs 可直接读）：
 *   - 新文件：size<=1 块用旧式直接块 i_block[0]（与 read 路径 legacy 兼容）；
 *             >1 块用 extent 树：碎片化多 extent 分配（first-fit 取洞），
 *             <=4 段放根 depth-0，超 4 段自动升级 depth-1（根 index +
 *             叶子 extent 块，1KB 块叶容 84 项 x 4 叶 = 336 段上限）
 *   - 新目录：线性无 htree（EXT4_INDEX_FL 不置位），自动扩展目录块链，
 *             extent 树增长与文件同一路径（满 4 项自动升级 depth-1）
 *   - 删除：任意深度 extent 树递归回收数据块与叶子块 + 清 inode 位图
 *           + 清目录项（inode=0）
 *   - 计数同步：SB free + GDT free count + free inodes count
 * ============================================================ */

static void e4_write_secs(uint32_t lba, const uint8_t *buf, uint32_t nsecs) {
    for (uint32_t i = 0; i < nsecs; i++)
        ata_write_sector(e4_drive, lba + i, buf + i * 512);
}

static void e4_write_blk(uint32_t blkno, const uint8_t *buf) {
    e4_write_secs(e4_part_lba + blkno * e4_blk_per_sec, buf, e4_blk_per_sec);
}

static void e4_wr32(uint8_t *p, uint32_t v) {
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
static void e4_wr16(uint8_t *p, uint16_t v) {
    p[0] = v; p[1] = v >> 8;
}

/* GDT 块号 + 偏移读组描述符到 e4_blk；返回 e4_blk 中 GD 指针 */
static uint8_t *e4_gd_ptr(uint32_t group, uint32_t *gd_blk_out) {
    uint64_t gd_off = (uint64_t)group * e4_desc_size;
    uint32_t gd_blk = e4_gdt_blk + (uint32_t)(gd_off / e4_blksize);
    uint32_t gd_in = (uint32_t)(gd_off % e4_blksize);
    e4_read_blk(gd_blk, e4_blk);
    if (gd_blk_out) *gd_blk_out = gd_blk;
    return e4_blk + gd_in;
}

/* 从组描述符取 inode 位图块号 */
static uint32_t e4_gd_inode_bitmap(uint32_t group) {
    uint8_t *gd = e4_gd_ptr(group, 0);
    return rd32(gd + 0x04);        /* bg_inode_bitmap_lo */
}

/* 从组描述符取块位图块号 */
static uint32_t e4_gd_block_bitmap(uint32_t group) {
    uint8_t *gd = e4_gd_ptr(group, 0);
    return rd32(gd + 0x00);        /* bg_block_bitmap_lo */
}

/* 从组描述符取 inode 表块号 */
static uint32_t e4_gd_inode_table(uint32_t group) {
    uint8_t *gd = e4_gd_ptr(group, 0);
    return rd32(gd + GD_OFF_INO_TABLE_LO);
}

/* 更新 GDT 和 SB 的 free blocks / free inodes 计数 */
static void e4_update_counts(int delta_blocks, int delta_inodes) {
    /* SB */
    static uint8_t sb[1024] E4_HIBUF;
    e4_read_secs(e4_part_lba + (e4_blksize == 1024 ? 2 : 0), sb,
                 e4_blksize == 1024 ? 2 : (e4_blksize / 512));
    uint32_t fb = rd32(sb + SB_OFF_FREE_BLKS_LO);
    e4_wr32(sb + SB_OFF_FREE_BLKS_LO, (uint32_t)((int)fb + delta_blocks));
    uint32_t fi = rd32(sb + 16);   /* s_free_inodes_count */
    e4_wr32(sb + 16, (uint32_t)((int)fi + delta_inodes));
    e4_write_secs(e4_part_lba + (e4_blksize == 1024 ? 2 : 0), sb,
                  e4_blksize == 1024 ? 2 : (e4_blksize / 512));

    /* GDT group 0 */
    uint32_t gd_blk;
    uint8_t *gd = e4_gd_ptr(0, &gd_blk);
    uint32_t gfb = rd32(gd + 12);  /* bg_free_blocks_count_lo */
    uint32_t gfi = rd32(gd + 16);  /* bg_free_inodes_count_lo */
    e4_wr32(gd + 12, (uint32_t)((int)gfb + delta_blocks));
    e4_wr32(gd + 16, (uint32_t)((int)gfi + delta_inodes));
    e4_write_blk(gd_blk, e4_blk);

    e4_blocks_free = (uint32_t)((int)e4_blocks_free + delta_blocks);
}

/* 在块位图中分配一个块；返回块号，失败返回 0 */
static uint32_t e4_alloc_block(void) {
    /* 仅组 0（单块组卷）；位图位号 = 组内块号（组 0 即卷块号）。
     * 注意：e4_gd_* 都会重读 GDT 覆盖 e4_blk，必须先取完 GD 字段再加载位图 */
    uint32_t itbl = e4_gd_inode_table(0);
    uint32_t bmp_blk = e4_gd_block_bitmap(0);
    uint32_t itbl_blks = (e4_ipg * e4_ino_size + e4_blksize - 1) / e4_blksize;
    uint32_t start = itbl + itbl_blks;
    if (start < e4_first_data_blk + 1) start = e4_first_data_blk + 1;
    uint32_t limit = e4_blocks_total < e4_bpg ? (uint32_t)e4_blocks_total : e4_bpg;
    e4_read_blk(bmp_blk, e4_blk);
    for (uint32_t i = start; i < limit; i++) {
        uint32_t byte_idx = i / 8;
        uint32_t bit_idx = i % 8;
        if (byte_idx >= e4_blksize) break;
        if (!(e4_blk[byte_idx] & (1u << bit_idx))) {
            e4_blk[byte_idx] |= (1u << bit_idx);
            e4_write_blk(bmp_blk, e4_blk);
            e4_update_counts(-1, 0);
            return i;
        }
    }
    return 0;
}

static void e4_free_block(uint32_t blkno) {
    uint32_t bmp_blk = e4_gd_block_bitmap(0);
    e4_read_blk(bmp_blk, e4_blk);
    uint32_t byte_idx = blkno / 8;
    uint32_t bit_idx = blkno % 8;
    if (byte_idx < e4_blksize)
        e4_blk[byte_idx] &= ~(1u << bit_idx);
    e4_write_blk(bmp_blk, e4_blk);
    e4_update_counts(1, 0);
}

/* ---------- 多 extent 碎片化写入支持 ----------
 * 标准结构：i_block 根节点放 4 个 extent（depth-0）；不够时升级为
 * depth-1 索引（根 4 个 index，每个指向一个叶子 extent 块，
 * 1KB 块叶子可放 84 项 / 4KB 块 340 项）。读路径 e4_map_extent
 * 已支持任意深度（GRUB 移植），这里补齐写路径。 */

/* 新文件数据 extent 记录上限：根 depth-1 最多 4 个索引项 x
 * 叶子容量 84（1KB 块；4KB 块为 340）= 336 段 */
#define E4_MAX_EXTENTS 336
static uint32_t e4_x_logical[E4_MAX_EXTENTS] E4_HIBUF;
static uint32_t e4_x_phys[E4_MAX_EXTENTS] E4_HIBUF;
static uint32_t e4_x_len[E4_MAX_EXTENTS] E4_HIBUF;
/* extent 树叶子块缓冲（升级 depth-1 / 追加目录 extent 用） */
static uint8_t e4_leaf[EXT4_MAX_BLOCKSIZE] E4_HIBUF;

/* 一次分配一段最长 want 的连续空闲块（first-fit）。
 * 返回起始块号，*got 输出实际分到的块数；无空闲返回 0。 */
static uint32_t e4_alloc_run(uint32_t want, uint32_t *got) {
    *got = 0;
    /* 位图扫描区（跳过 SB/GDT/位图/inode 表元数据区） */
    uint32_t itbl = e4_gd_inode_table(0);
    uint32_t bmp_blk = e4_gd_block_bitmap(0);
    uint32_t itbl_blks = (e4_ipg * e4_ino_size + e4_blksize - 1) / e4_blksize;
    uint32_t start = itbl + itbl_blks;
    if (start < e4_first_data_blk + 1) start = e4_first_data_blk + 1;
    uint32_t limit = e4_blocks_total < e4_bpg ? (uint32_t)e4_blocks_total : e4_bpg;
    e4_read_blk(bmp_blk, e4_blk);

    /* 贪心：找第一个 >= want 的洞就取 want 块；
     * 否则取整个过程中遇到的最大洞。 */
    uint32_t best_start = 0, best_len = 0;
    uint32_t cur_start = 0, cur_len = 0;
    for (uint32_t i = start; i < limit; i++) {
        uint32_t byte_idx = i / 8;
        uint32_t bit_idx = i % 8;
        if (byte_idx >= e4_blksize) break;
        if (!(e4_blk[byte_idx] & (1u << bit_idx))) {
            if (cur_len == 0) cur_start = i;
            cur_len++;
            if (cur_len == want) break;   /* 找到足够大的洞 */
        } else {
            if (cur_len > best_len) { best_len = cur_len; best_start = cur_start; }
            cur_len = 0;
        }
    }
    if (cur_len > best_len) { best_len = cur_len; best_start = cur_start; }
    if (best_len == 0) return 0;

    /* 标记位图 */
    for (uint32_t i = 0; i < best_len; i++) {
        uint32_t b = best_start + i;
        e4_blk[b / 8] |= (1u << (b % 8));
    }
    e4_write_blk(bmp_blk, e4_blk);
    e4_update_counts(-(int)best_len, 0);
    *got = best_len;
    return best_start;
}

/* 叶子块容量（extent 块除 12B 头外每项 12B） */
static uint32_t e4_leaf_capacity(void) {
    return (e4_blksize - 12) / 12;
}

/* 写一个 extent 树叶子块头 */
static void e4_leaf_init(uint8_t *leaf, uint32_t entries) {
    e4_wr16(leaf + EH_OFF_MAGIC, EXT4_EXT_MAGIC);
    e4_wr16(leaf + EH_OFF_ENTRIES, (uint16_t)entries);
    e4_wr16(leaf + 4, (uint16_t)e4_leaf_capacity());   /* eh_max */
    e4_wr16(leaf + EH_OFF_DEPTH, 0);
}

/* 向 extent 树叶子块追加一个 extent 项；满返回 -1 */
static int e4_leaf_append(uint8_t *leaf, uint32_t logical,
                          uint32_t phys, uint32_t len) {
    uint32_t entries = rd16(leaf + EH_OFF_ENTRIES);
    if (entries >= e4_leaf_capacity()) return -1;
    uint8_t *ex = leaf + 12 + entries * 12;
    e4_wr32(ex + EE_OFF_BLOCK, logical);
    e4_wr16(ex + EE_OFF_LEN, (uint16_t)len);
    e4_wr16(ex + EE_OFF_START_HI, 0);
    e4_wr32(ex + EE_OFF_START_LO, phys);
    e4_wr16(leaf + EH_OFF_ENTRIES, (uint16_t)(entries + 1));
    return 0;
}

/* 向 inode 的 extent 树追加一个逻辑块映射（目录增长用）。
 * 支持：legacy 直接块（lblk<12）、depth-0 根（满 4 项自动升级
 * depth-1）、depth-1（末叶满则新叶）。返回 0 成功。
 * 注意：成功时会写叶子块并修改 dino 中的根；调用者负责把
 * dino 写回 inode 表。 */
static int e4_extent_append(uint8_t *dino, uint32_t lblk, uint32_t pb) {
    if (!(rd32(dino + INO_OFF_FLAGS) & EXT4_EXTENTS_FL)) {
        /* 旧式直接块目录（本驱动 format 的根目录为 extent，自定义 mkdir 也是
         * extent；此分支兼容外部 mkfs 出的 legacy 小目录） */
        if (lblk < 12) {
            e4_wr32(dino + INO_OFF_IBLOCK + lblk * 4, pb);
            return 0;
        }
        return -1;
    }
    uint8_t *hdr = dino + INO_OFF_IBLOCK;
    if (rd16(hdr) != EXT4_EXT_MAGIC) return -1;
    uint32_t entries = rd16(hdr + EH_OFF_ENTRIES);
    uint32_t depth = rd16(hdr + EH_OFF_DEPTH);

    if (depth == 0) {
        if (entries > 0) {
            uint8_t *ex = hdr + 12 + (entries - 1) * 12;
            uint32_t old_len = EXT4_EXT_LEN(rd16(ex + EE_OFF_LEN));
            uint32_t old_start = rd32(ex + EE_OFF_START_LO);
            uint32_t old_logical = rd32(ex + EE_OFF_BLOCK);
            /* 物理与逻辑均连续：扩展末 extent */
            if (old_start + old_len == pb &&
                old_logical + old_len == lblk) {
                e4_wr16(ex + EE_OFF_LEN, (uint16_t)(old_len + 1));
                return 0;
            }
        }
        if (entries < 4) {
            uint8_t *nex = hdr + 12 + entries * 12;
            e4_wr32(nex + EE_OFF_BLOCK, lblk);
            e4_wr16(nex + EE_OFF_LEN, 1);
            e4_wr16(nex + EE_OFF_START_HI, 0);
            e4_wr32(nex + EE_OFF_START_LO, pb);
            e4_wr16(hdr + EH_OFF_ENTRIES, (uint16_t)(entries + 1));
            return 0;
        }
        /* 根满：升级 depth-1 —— 现有 4 项搬入新叶，根变 1 个 index */
        uint32_t lb = e4_alloc_block();
        if (lb == 0) return -1;
        for (uint32_t i = 0; i < e4_blksize; i++) e4_leaf[i] = 0;
        e4_leaf_init(e4_leaf, 4);
        for (uint32_t i = 0; i < 4; i++) {
            uint8_t *src = hdr + 12 + i * 12;
            uint8_t *dst = e4_leaf + 12 + i * 12;
            for (uint32_t k = 0; k < 12; k++) dst[k] = src[k];
        }
        if (e4_leaf_append(e4_leaf, lblk, pb, 1) != 0) {
            e4_free_block(lb);
            return -1;
        }
        e4_write_blk(lb, e4_leaf);
        /* 根重写为 depth-1 单 index */
        e4_wr16(hdr + EH_OFF_ENTRIES, 1);
        e4_wr16(hdr + 4, 4);
        e4_wr16(hdr + EH_OFF_DEPTH, 1);
        uint8_t *ix = hdr + 12;
        e4_wr32(ix + EI_OFF_BLOCK, 0);        /* 首叶覆盖逻辑块 0 起 */
        e4_wr16(ix + EI_OFF_LEAF_HI, 0);
        e4_wr32(ix + EI_OFF_LEAF_LO, lb);
        return 0;
    }

    if (depth == 1) {
        if (entries == 0) return -1;
        uint8_t *last_ix = hdr + 12 + (entries - 1) * 12;
        uint32_t lb = rd32(last_ix + EI_OFF_LEAF_LO);
        if (lb == 0) return -1;
        e4_read_blk(lb, e4_leaf);
        uint32_t lents = rd16(e4_leaf + EH_OFF_ENTRIES);
        if (lents > 0) {
            uint8_t *ex = e4_leaf + 12 + (lents - 1) * 12;
            uint32_t old_len = EXT4_EXT_LEN(rd16(ex + EE_OFF_LEN));
            uint32_t old_start = rd32(ex + EE_OFF_START_LO);
            uint32_t old_logical = rd32(ex + EE_OFF_BLOCK);
            if (old_start + old_len == pb &&
                old_logical + old_len == lblk) {
                e4_wr16(ex + EE_OFF_LEN, (uint16_t)(old_len + 1));
                e4_write_blk(lb, e4_leaf);
                return 0;
            }
        }
        if (e4_leaf_append(e4_leaf, lblk, pb, 1) == 0) {
            e4_write_blk(lb, e4_leaf);
            return 0;
        }
        /* 末叶满：新叶 + 根加 index（根最多 4 个 index，足够 336 段） */
        if (entries >= 4) return -1;
        uint32_t nb = e4_alloc_block();
        if (nb == 0) return -1;
        for (uint32_t i = 0; i < e4_blksize; i++) e4_leaf[i] = 0;
        e4_leaf_init(e4_leaf, 0);
        e4_leaf_append(e4_leaf, lblk, pb, 1);
        e4_write_blk(nb, e4_leaf);
        uint8_t *nix = hdr + 12 + entries * 12;
        e4_wr32(nix + EI_OFF_BLOCK, lblk);
        e4_wr16(nix + EI_OFF_LEAF_HI, 0);
        e4_wr32(nix + EI_OFF_LEAF_LO, nb);
        e4_wr16(hdr + EH_OFF_ENTRIES, (uint16_t)(entries + 1));
        return 0;
    }
    return -1;   /* depth >=2 目录不做增量增长（删除重写即可恢复） */
}

/* 分配一个 inode；返回 inode 号，失败返回 0 */
static uint32_t e4_alloc_inode(void) {
    uint32_t bmp_blk = e4_gd_inode_bitmap(0);
    e4_read_blk(bmp_blk, e4_blk);
    /* inode 号从 1 开始；前 11 个保留（EXT4_FIRST_INO=11 for 1KB 块） */
    uint32_t first = 11;
    for (uint32_t i = first; i < e4_ipg; i++) {
        uint32_t byte_idx = i / 8;
        uint32_t bit_idx = i % 8;
        if (!(e4_blk[byte_idx] & (1u << bit_idx))) {
            e4_blk[byte_idx] |= (1u << bit_idx);
            e4_write_blk(bmp_blk, e4_blk);
            e4_update_counts(0, -1);
            return i + 1;         /* inode 号 = 索引 + 1 */
        }
    }
    return 0;
}

static void e4_free_inode(uint32_t ino) {
    uint32_t bmp_blk = e4_gd_inode_bitmap(0);
    e4_read_blk(bmp_blk, e4_blk);
    uint32_t idx = ino - 1;
    uint32_t byte_idx = idx / 8;
    uint32_t bit_idx = idx % 8;
    if (byte_idx < e4_blksize)
        e4_blk[byte_idx] &= ~(1u << bit_idx);
    e4_write_blk(bmp_blk, e4_blk);
    e4_update_counts(0, 1);
}

/* 写 inode 回 inode 表 */
static int e4_write_inode(uint32_t ino, const uint8_t *inode_data) {
    uint32_t group = (ino - 1) / e4_ipg;
    uint32_t idx = (ino - 1) % e4_ipg;
    uint32_t ino_table = e4_gd_inode_table(group);
    uint64_t ioff = (uint64_t)idx * e4_ino_size;
    uint32_t iblk = ino_table + (uint32_t)(ioff / e4_blksize);
    uint32_t iin = (uint32_t)(ioff % e4_blksize);
    e4_read_blk(iblk, e4_blk);
    for (uint32_t i = 0; i < e4_ino_size; i++)
        e4_blk[iin + i] = inode_data[i];
    e4_write_blk(iblk, e4_blk);
    return 0;
}

/* 在目录中插入一条 dirent。
 * 策略：线性扫描目录块链，找尾部 rec_len > 需求 的条目，
 * 截断它的 rec_len 并在后面追加新条目。空间不足时扩展一个新块。 */
static int e4_dir_add_entry(uint32_t dir_ino, const char *name,
                            uint32_t name_len, uint32_t target_ino, uint8_t ftype) {
    static uint8_t dino[EXT4_MAX_INODESIZE];
    if (e4_read_inode(dir_ino, dino) != 0) return -1;
    uint32_t size = e4_ino_size_of(dino, rd16(dino + INO_OFF_MODE));
    uint32_t nblk = (size + e4_blksize - 1) / e4_blksize;
    if (nblk == 0) nblk = 1;       /* 空目录也至少 1 块 */

    uint32_t need = (8 + name_len + 3) & ~3u;   /* 对齐到 4 字节 */

    for (uint32_t b = 0; b < nblk; b++) {
        uint32_t pb = e4_map_block(dino, b);
        if (pb == 0) {
            /* 目录块未分配（洞）：分配新块并链接 */
            pb = e4_alloc_block();
            if (pb == 0) return -1;
            /* 清零新块 */
            static uint8_t zbuf[EXT4_MAX_BLOCKSIZE] E4_HIBUF;
            for (uint32_t i = 0; i < e4_blksize; i++) zbuf[i] = 0;
            e4_write_blk(pb, zbuf);
            /* 更新目录 inode 的 i_block 映射（任意深度 extent 树） */
            if (e4_extent_append(dino, b, pb) != 0) {
                e4_free_block(pb);
                return -1;
            }
            /* 更新目录大小和 i_blocks */
            uint32_t new_size = (b + 1) * e4_blksize;
            if (new_size > size) {
                e4_wr32(dino + INO_OFF_SIZE_LO, new_size);
                e4_wr32(dino + INO_OFF_BLOCKS_LO,
                         rd32(dino + INO_OFF_BLOCKS_LO) + e4_blk_per_sec);
                e4_write_inode(dir_ino, dino);
            }
        }

        e4_read_blk(pb, e4_dblk);
        uint32_t off = 0;
        while (off + 8 <= e4_blksize) {
            uint32_t rec_len = rd16(e4_dblk + off + DE_OFF_RECLEN);
            if (rec_len < 8) break;
            uint32_t child = rd32(e4_dblk + off + DE_OFF_INODE);
            uint32_t existing_len = e4_dblk[off + DE_OFF_NAMELEN];
            uint32_t actual = (8 + existing_len + 3) & ~3u;
            /* 尾部空闲条目：child==0 或 rec_len 远大于 actual */
            if (child == 0 && rec_len >= need) {
                /* 在此处插入，截断 rec_len */
                uint32_t remain = rec_len - actual;
                if (remain >= need) {
                    e4_wr16(e4_dblk + off + DE_OFF_RECLEN, (uint16_t)actual);
                    uint32_t noff = off + actual;
                    e4_wr32(e4_dblk + noff + DE_OFF_INODE, target_ino);
                    e4_wr16(e4_dblk + noff + DE_OFF_RECLEN, (uint16_t)remain);
                    e4_dblk[noff + DE_OFF_NAMELEN] = (uint8_t)name_len;
                    e4_dblk[noff + DE_OFF_TYPE] = ftype;
                    for (uint32_t i = 0; i < name_len; i++)
                        e4_dblk[noff + DE_OFF_NAME + i] = (uint8_t)name[i];
                    e4_write_blk(pb, e4_dblk);
                    return 0;
                }
            }
            off += rec_len;
        }
    }

    /* 所有块都满了：扩展目录块 */
    uint32_t new_blk = e4_alloc_block();
    if (new_blk == 0) return -1;
    static uint8_t nbuf[EXT4_MAX_BLOCKSIZE] E4_HIBUF;
    for (uint32_t i = 0; i < e4_blksize; i++) nbuf[i] = 0;
    /* 新块的第一个 dirent 就是新条目，rec_len 覆盖整块 */
    e4_wr32(nbuf + DE_OFF_INODE, target_ino);
    e4_wr16(nbuf + DE_OFF_RECLEN, (uint16_t)e4_blksize);
    nbuf[DE_OFF_NAMELEN] = (uint8_t)name_len;
    nbuf[DE_OFF_TYPE] = ftype;
    for (uint32_t i = 0; i < name_len; i++)
        nbuf[DE_OFF_NAME + i] = (uint8_t)name[i];
    e4_write_blk(new_blk, nbuf);

    /* 链接新块到目录 inode（任意深度 extent 树） */
    if (e4_extent_append(dino, nblk, new_blk) != 0) {
        e4_free_block(new_blk);
        return -1;
    }
    uint32_t new_size = (nblk + 1) * e4_blksize;
    e4_wr32(dino + INO_OFF_SIZE_LO, new_size);
    e4_wr32(dino + INO_OFF_BLOCKS_LO,
             rd32(dino + INO_OFF_BLOCKS_LO) + e4_blk_per_sec);
    e4_write_inode(dir_ino, dino);
    return 0;
}

/* 从目录中删除指定 inode 的 dirent */
static int e4_dir_del_entry(uint32_t dir_ino, uint32_t target_ino) {
    static uint8_t dino[EXT4_MAX_INODESIZE];
    if (e4_read_inode(dir_ino, dino) != 0) return -1;
    uint32_t size = e4_ino_size_of(dino, rd16(dino + INO_OFF_MODE));
    uint32_t nblk = (size + e4_blksize - 1) / e4_blksize;

    for (uint32_t b = 0; b < nblk; b++) {
        uint32_t pb = e4_map_block(dino, b);
        if (pb == 0) continue;
        e4_read_blk(pb, e4_dblk);
        uint32_t off = 0;
        uint32_t prev_off = 0;
        while (off + 8 <= e4_blksize) {
            uint32_t rec_len = rd16(e4_dblk + off + DE_OFF_RECLEN);
            if (rec_len < 8) break;
            uint32_t child = rd32(e4_dblk + off + DE_OFF_INODE);
            if (child == target_ino) {
                /* 合并到前一条目（或自身置 0） */
                if (off > 0) {
                    uint32_t prev_rl = rd16(e4_dblk + prev_off + DE_OFF_RECLEN);
                    e4_wr16(e4_dblk + prev_off + DE_OFF_RECLEN,
                            (uint16_t)(prev_rl + rec_len));
                } else {
                    /* 首条目：置 inode=0 保留 rec_len（不真正回收块） */
                    e4_wr32(e4_dblk + off + DE_OFF_INODE, 0);
                }
                e4_write_blk(pb, e4_dblk);
                return 0;
            }
            prev_off = off;
            off += rec_len;
        }
    }
    return -1;  /* 未找到 */
}

/* 在目录中查找指定名称，返回 inode 号与 ftype（用于 create-or-replace 先删旧） */
static int e4_dir_find(uint32_t dir_ino, const char *name, uint32_t name_len,
                        uint32_t *out_ino, uint8_t *out_type) {
    e4_lookup_ctx ctx;
    ctx.name = name;
    ctx.name_len = name_len;
    ctx.found = 0;
    if (e4_dir_walk(dir_ino, e4_lookup_cb, &ctx) != 0) return -1;
    if (!ctx.found) return 1;  /* 未找到 */
    if (out_ino) *out_ino = ctx.found_ino;
    if (out_type) *out_type = ctx.found_type;
    return 0;
}

/* 解析路径返回父目录 inode 号与文件名组件 */
static int e4_split_path(const char *path, uint32_t *parent_ino,
                         const char **name, uint32_t *name_len) {
    /* 找最后一个 '/' */
    const char *slash = 0;
    const char *p = path;
    while (*p) {
        if (*p == '/') slash = p;
        p++;
    }
    if (!slash) return -1;

    *name = slash + 1;
    *name_len = 0;
    while ((*name)[*name_len] && (*name)[*name_len] != '/') (*name_len)++;

    if (*name_len == 0 || *name_len > 255) return -1;

    /* 父目录路径 = path[0..slash-1] */
    if (slash == path) {
        *parent_ino = EXT4_ROOT_INO;
    } else {
        char parent_path[256];
        uint32_t plen = (uint32_t)(slash - path);
        if (plen >= 256) return -1;
        for (uint32_t i = 0; i < plen; i++) parent_path[i] = path[i];
        parent_path[plen] = 0;
        int is_dir;
        uint32_t pin = e4_resolve(parent_path, &is_dir, 0, 0);
        if (pin == 0 || !is_dir) return -1;
        *parent_ino = pin;
    }
    return 0;
}

/* 回收 extent 节点：depth=0 释放全部数据块；depth>0 逐 index 读
 * 叶子块递归释放数据块，并释放叶子块本身 */
static void e4_free_extent_node(const uint8_t *hdr, uint32_t depth) {
    uint32_t entries = rd16(hdr + EH_OFF_ENTRIES);
    if (rd16(hdr) != EXT4_EXT_MAGIC) return;
    if (depth == 0) {
        for (uint32_t i = 0; i < entries; i++) {
            const uint8_t *ex = hdr + 12 + i * 12;
            uint32_t len = EXT4_EXT_LEN(rd16(ex + EE_OFF_LEN));
            uint32_t start = rd32(ex + EE_OFF_START_LO);
            for (uint32_t j = 0; j < len; j++)
                if (start + j != 0)
                    e4_free_block(start + j);
        }
    } else {
        for (uint32_t i = 0; i < entries; i++) {
            const uint8_t *ix = hdr + 12 + i * 12;
            uint32_t leaf = rd32(ix + EI_OFF_LEAF_LO);
            if (leaf == 0) continue;
            e4_read_blk(leaf, e4_leaf);
            e4_free_extent_node(e4_leaf, depth - 1);
            e4_free_block(leaf);
        }
    }
}

/* 回收 inode 的全部数据块（用于 delete_file；任意深度 extent 树） */
static void e4_free_inode_blocks(uint8_t *ino) {
    uint32_t mode = rd16(ino + INO_OFF_MODE);
    if ((mode & 0xF000u) != 0x8000u && (mode & 0xF000u) != 0x4000u)
        return;  /* 非文件/目录 */

    uint32_t size = e4_ino_size_of(ino, mode);
    uint32_t nblk = (size + e4_blksize - 1) / e4_blksize;

    if (rd32(ino + INO_OFF_FLAGS) & EXT4_EXTENTS_FL) {
        uint8_t *hdr = ino + INO_OFF_IBLOCK;
        e4_free_extent_node(hdr, rd16(hdr + EH_OFF_DEPTH));
    } else {
        /* 旧式直接块 */
        for (uint32_t b = 0; b < nblk && b < 12; b++) {
            uint32_t blk = rd32(ino + INO_OFF_IBLOCK + b * 4);
            if (blk) e4_free_block(blk);
        }
    }
}

int ext4_create_file(const char *name, const uint8_t *data, uint32_t size) {
    /* 解析路径 -> 父目录 inode + 文件名 */
    uint32_t parent_ino;
    const char *fname;
    uint32_t fname_len;
    if (e4_split_path(name, &parent_ino, &fname, &fname_len) != 0) return -1;

    /* create-or-replace：先删旧文件 */
    uint32_t old_ino;
    uint8_t old_type;
    if (e4_dir_find(parent_ino, fname, fname_len, &old_ino, &old_type) == 0) {
        /* 旧文件存在：回收块+inode，删目录项 */
        static uint8_t old_inode[EXT4_MAX_INODESIZE];
        if (e4_read_inode(old_ino, old_inode) != 0) return -1;
        if (e4_ino_is_dir(old_inode)) return -1;  /* 同名目录存在 */
        e4_free_inode_blocks(old_inode);
        e4_free_inode(old_ino);
        e4_dir_del_entry(parent_ino, old_ino);
    }

    /* 分配新 inode */
    uint32_t new_ino = e4_alloc_inode();
    if (new_ino == 0) return -1;

    static uint8_t ino[EXT4_MAX_INODESIZE];
    for (uint32_t i = 0; i < e4_ino_size; i++) ino[i] = 0;
    e4_wr16(ino + INO_OFF_MODE, 0x81A4);   /* regular file 0644 */
    e4_wr32(ino + INO_OFF_SIZE_LO, size);

    /* 分配数据块并写入 */
    if (size == 0) {
        /* 空文件：无数据块 */
        e4_wr32(ino + INO_OFF_BLOCKS_LO, 0);
    } else if (size <= e4_blksize) {
        /* 单块：旧式直接块 */
        uint32_t blk = e4_alloc_block();
        if (blk == 0) { e4_free_inode(new_ino); return -1; }
        static uint8_t fbuf[EXT4_MAX_BLOCKSIZE] E4_HIBUF;
        for (uint32_t i = 0; i < e4_blksize; i++) fbuf[i] = 0;
        for (uint32_t i = 0; i < size; i++) fbuf[i] = data[i];
        e4_write_blk(blk, fbuf);
        e4_wr32(ino + INO_OFF_IBLOCK, blk);
        e4_wr32(ino + INO_OFF_BLOCKS_LO, e4_blk_per_sec);
    } else {
        /* 多块：extent 映射（碎片化多 extent；超 4 段升级 depth-1 索引树） */
        uint32_t nblk = (size + e4_blksize - 1) / e4_blksize;
        uint32_t ext_count = 0;
        uint32_t nalloc = 0, next_logical = 0, done = 0;
        static uint8_t fbuf[EXT4_MAX_BLOCKSIZE] E4_HIBUF;
        while (nalloc < nblk) {
            uint32_t want = nblk - nalloc;
            uint32_t got = 0;
            uint32_t s = e4_alloc_run(want, &got);
            if (s == 0 || got == 0) {
                /* 空间不足：按 extent 记录精确回滚 */
                for (uint32_t e = 0; e < ext_count; e++)
                    for (uint32_t j = 0; j < e4_x_len[e]; j++)
                        e4_free_block(e4_x_phys[e] + j);
                e4_free_inode(new_ino);
                return -1;
            }
            /* 写这段数据（整段连续，逐块写） */
            for (uint32_t k = 0; k < got; k++) {
                for (uint32_t j = 0; j < e4_blksize; j++) fbuf[j] = 0;
                uint32_t chunk = size - done;
                if (chunk > e4_blksize) chunk = e4_blksize;
                for (uint32_t j = 0; j < chunk; j++) fbuf[j] = data[done + j];
                e4_write_blk(s + k, fbuf);
                done += chunk;
            }
            if (ext_count < E4_MAX_EXTENTS) {
                e4_x_logical[ext_count] = next_logical;
                e4_x_phys[ext_count] = s;
                e4_x_len[ext_count] = got;
                ext_count++;
            }
            next_logical += got;
            nalloc += got;
        }

        uint8_t *hdr = ino + INO_OFF_IBLOCK;
        uint32_t leaf_blks = 0;
        e4_wr32(ino + INO_OFF_FLAGS, EXT4_EXTENTS_FL);
        if (ext_count <= 4) {
            /* 根 depth-0：extent 直接放 inode i_block */
            e4_wr16(hdr + EH_OFF_MAGIC, EXT4_EXT_MAGIC);
            e4_wr16(hdr + EH_OFF_ENTRIES, (uint16_t)ext_count);
            e4_wr16(hdr + 4, 4);    /* eh_max */
            e4_wr16(hdr + EH_OFF_DEPTH, 0);
            for (uint32_t i = 0; i < ext_count; i++) {
                uint8_t *ex = hdr + 12 + i * 12;
                e4_wr32(ex + EE_OFF_BLOCK, e4_x_logical[i]);
                e4_wr16(ex + EE_OFF_LEN, (uint16_t)e4_x_len[i]);
                e4_wr16(ex + EE_OFF_START_HI, 0);
                e4_wr32(ex + EE_OFF_START_LO, e4_x_phys[i]);
            }
        } else {
            /* 根 depth-1：extent 分批放叶子块，根放 index（最多 4 叶） */
            uint32_t cap = e4_leaf_capacity();
            leaf_blks = (ext_count + cap - 1) / cap;
            static uint32_t leaves[16] E4_HIBUF;   /* <=4（1KB 块），留裕量 */
            uint32_t nleaves = 0, next_ext = 0;
            int fail = 0;
            for (uint32_t L = 0; L < leaf_blks && !fail; L++) {
                uint32_t lb = e4_alloc_block();
                if (lb == 0) { fail = 1; break; }
                for (uint32_t i = 0; i < e4_blksize; i++) e4_leaf[i] = 0;
                e4_leaf_init(e4_leaf, 0);
                while (next_ext < ext_count) {
                    if (e4_leaf_append(e4_leaf, e4_x_logical[next_ext],
                                       e4_x_phys[next_ext], e4_x_len[next_ext]) != 0)
                        break;
                    next_ext++;
                }
                e4_write_blk(lb, e4_leaf);
                leaves[nleaves++] = lb;
            }
            if (fail || next_ext < ext_count || leaf_blks > 4) {
                /* 叶子分配失败/超根容量：回滚全部（数据块+叶子块+inode） */
                for (uint32_t e = 0; e < ext_count; e++)
                    for (uint32_t j = 0; j < e4_x_len[e]; j++)
                        e4_free_block(e4_x_phys[e] + j);
                for (uint32_t L = 0; L < nleaves; L++) e4_free_block(leaves[L]);
                e4_free_inode(new_ino);
                return -1;
            }
            e4_wr16(hdr + EH_OFF_MAGIC, EXT4_EXT_MAGIC);
            e4_wr16(hdr + EH_OFF_ENTRIES, (uint16_t)leaf_blks);
            e4_wr16(hdr + 4, 4);
            e4_wr16(hdr + EH_OFF_DEPTH, 1);
            uint32_t le = 0;
            for (uint32_t L = 0; L < leaf_blks; L++) {
                uint8_t *ix = hdr + 12 + L * 12;
                e4_wr32(ix + EI_OFF_BLOCK, e4_x_logical[le]);
                e4_wr16(ix + EI_OFF_LEAF_HI, 0);
                e4_wr32(ix + EI_OFF_LEAF_LO, leaves[L]);
                le += cap;   /* 每叶 cap 项（末叶可少） */
            }
        }
        e4_wr32(ino + INO_OFF_BLOCKS_LO, (nblk + leaf_blks) * e4_blk_per_sec);
    }

    e4_write_inode(new_ino, ino);

    /* 在父目录中插入目录项 */
    if (e4_dir_add_entry(parent_ino, fname, fname_len, new_ino, 1) != 0) {
        /* 插入失败：回滚 */
        e4_free_inode_blocks(ino);
        e4_free_inode(new_ino);
        return -1;
    }
    return 0;
}

int ext4_delete_file(const char *name) {
    /* 解析路径 -> 父目录 + 文件名 */
    uint32_t parent_ino;
    const char *fname;
    uint32_t fname_len;
    if (e4_split_path(name, &parent_ino, &fname, &fname_len) != 0) return -1;

    /* 查找文件 inode */
    uint32_t target_ino;
    uint8_t target_type;
    if (e4_dir_find(parent_ino, fname, fname_len, &target_ino, &target_type) != 0)
        return -1;  /* 不存在 */

    static uint8_t ino[EXT4_MAX_INODESIZE];
    if (e4_read_inode(target_ino, ino) != 0) return -1;
    if (e4_ino_is_dir(ino)) return -1;  /* 是目录，不是文件 */

    /* 回收数据块 + inode */
    e4_free_inode_blocks(ino);
    e4_free_inode(target_ino);

    /* 从父目录删除 dirent */
    if (e4_dir_del_entry(parent_ino, target_ino) != 0) return -1;
    return 0;
}

int ext4_mkdir(const char *name) {
    /* 解析路径 -> 父目录 + 目录名 */
    uint32_t parent_ino;
    const char *dirname;
    uint32_t dirname_len;
    if (e4_split_path(name, &parent_ino, &dirname, &dirname_len) != 0) return -1;

    /* 检查同名是否存在 */
    uint32_t existing;
    uint8_t etype;
    if (e4_dir_find(parent_ino, dirname, dirname_len, &existing, &etype) == 0)
        return -1;  /* 已存在 */

    /* 分配新 inode */
    uint32_t new_ino = e4_alloc_inode();
    if (new_ino == 0) return -1;

    /* 分配目录数据块 */
    uint32_t dir_blk = e4_alloc_block();
    if (dir_blk == 0) { e4_free_inode(new_ino); return -1; }

    /* 构造目录块：'.' + '..' + 尾部空闲 */
    static uint8_t dbuf[EXT4_MAX_BLOCKSIZE] E4_HIBUF;
    for (uint32_t i = 0; i < e4_blksize; i++) dbuf[i] = 0;
    /* '.' dirent */
    e4_wr32(dbuf + 0, new_ino);
    e4_wr16(dbuf + DE_OFF_RECLEN, 12);
    dbuf[DE_OFF_NAMELEN] = 1;
    dbuf[DE_OFF_TYPE] = 2;
    dbuf[DE_OFF_NAME] = '.';
    /* '..' dirent */
    e4_wr32(dbuf + 12, parent_ino);
    e4_wr16(dbuf + 12 + DE_OFF_RECLEN, (uint16_t)(e4_blksize - 12));
    dbuf[12 + DE_OFF_NAMELEN] = 2;
    dbuf[12 + DE_OFF_TYPE] = 2;
    dbuf[12 + DE_OFF_NAME] = '.';
    dbuf[12 + DE_OFF_NAME + 1] = '.';
    e4_write_blk(dir_blk, dbuf);

    /* 构造 inode */
    static uint8_t ino[EXT4_MAX_INODESIZE];
    for (uint32_t i = 0; i < e4_ino_size; i++) ino[i] = 0;
    e4_wr16(ino + INO_OFF_MODE, 0x41ED);   /* directory 0755 */
    e4_wr32(ino + INO_OFF_SIZE_LO, e4_blksize);
    e4_wr32(ino + INO_OFF_IBLOCK, dir_blk);  /* 直接块 0 */
    e4_wr32(ino + INO_OFF_BLOCKS_LO, e4_blk_per_sec);
    e4_write_inode(new_ino, ino);

    /* 在父目录插入 dirent */
    if (e4_dir_add_entry(parent_ino, dirname, dirname_len, new_ino, 2) != 0) {
        e4_free_block(dir_blk);
        e4_free_inode(new_ino);
        return -1;
    }
    return 0;
}

/* ============================================================
 * 格式化
 * refs: e2fsprogs misc/mke2fs.c - 布局参数参考
 * 简化布局（单块组，1024B 块，与 gen_diskimg.py 对齐）：
 *   块 0: 全零（1K 块保留）
 *   块 1: superblock
 *   块 2: GDT
 *   块 3: block bitmap
 *   块 4: inode bitmap
 *   块 5-8: inode table (32 inodes x 128B)
 *   块 9+: data blocks
 * ============================================================ */
int ext4_format(uint8_t drive) {
    if (drive > 3) return -1;

    uint32_t part_start = 1;
    uint32_t vol_blocks = 1024;     /* 1MB 卷 */
    uint32_t blksize = 1024;
    uint32_t blk_per_sec = blksize / 512;
    uint32_t bpg = 8192;
    uint32_t ipg = 32;
    uint32_t ino_size = 128;
    uint32_t first_data_blk = 1;   /* 1K 块恒为 1 */

    /* MBR: 分区 0x83 从 LBA 1 起 */
    uint8_t mbr[512];
    for (int i = 0; i < 512; i++) mbr[i] = 0;
    mbr[447] = 0x00; mbr[448] = 0x02; mbr[449] = 0x00;
    mbr[450] = 0x83;
    mbr[451] = 0x00; mbr[452] = 0x3F; mbr[453] = 0xFF;
    e4_wr32(mbr + 454, part_start);
    e4_wr32(mbr + 458, vol_blocks * blk_per_sec - 1);
    mbr[510] = 0x55; mbr[511] = 0xAA;
    if (ata_write_sector(drive, 0, mbr) != 0) return -1;

    /* Superblock（块 1 = LBA 2-3） */
    static uint8_t sb[1024] E4_HIBUF;
    for (uint32_t i = 0; i < 1024; i++) sb[i] = 0;
    e4_wr32(sb + 0, ipg);                  /* s_inodes_count */
    e4_wr32(sb + 4, vol_blocks);            /* s_blocks_count_lo */
    e4_wr32(sb + 12, vol_blocks - 10);     /* s_free_blocks_count_lo */
    e4_wr32(sb + 16, ipg - 2);             /* s_free_inodes_count */
    e4_wr32(sb + 20, first_data_blk);      /* s_first_data_block */
    e4_wr32(sb + 24, 0);                   /* s_log_block_size = 0 (1024B) */
    e4_wr32(sb + 28, 0);                   /* s_log_cluster_size */
    e4_wr32(sb + 32, bpg);                 /* s_blocks_per_group */
    e4_wr32(sb + 36, bpg);                 /* s_clusters_per_group */
    e4_wr32(sb + 40, ipg);                 /* s_inodes_per_group */
    e4_wr16(sb + 56, EXT4_MAGIC);
    e4_wr32(sb + 76, 1);                   /* s_rev_level = dynamic */
    e4_wr32(sb + 84, 11);                  /* s_first_ino */
    e4_wr16(sb + 88, ino_size);            /* s_inode_size */
    e4_wr32(sb + 96, 0x0042);             /* incompat: FILETYPE | EXTENTS */
    e4_wr32(sb + 100, 0);                 /* ro_compat */
    e4_wr16(sb + 282, 0);                  /* s_desc_size = 0 (32B GDT) */
    sb[510] = 0x53; sb[511] = 0xEF;       /* ext signature */
    e4_write_secs(part_start + 2, sb, 2);

    /* GDT（块 2）：组 0 描述符 */
    static uint8_t gd[1024] E4_HIBUF;
    for (uint32_t i = 0; i < 1024; i++) gd[i] = 0;
    e4_wr32(gd + 0, 3);                    /* bg_block_bitmap_lo = block 3 */
    e4_wr32(gd + 4, 4);                    /* bg_inode_bitmap_lo = block 4 */
    e4_wr32(gd + 8, 5);                    /* bg_inode_table_lo = block 5 */
    e4_wr32(gd + 12, vol_blocks - 10);     /* bg_free_blocks_count_lo */
    e4_wr32(gd + 16, ipg - 2);             /* bg_free_inodes_count_lo */
    e4_write_secs(part_start + 2 * blk_per_sec, gd, blk_per_sec);

    /* Block bitmap（块 3）：前 10 块已用（块 1-9），bit 0 = 块 0(保留) */
    static uint8_t bmp[1024] E4_HIBUF;
    for (uint32_t i = 0; i < 1024; i++) bmp[i] = 0;
    /* 块 0-9 已用：bits 0-9 置 1 */
    for (uint32_t i = 0; i <= 9; i++)
        bmp[i / 8] |= (1u << (i % 8));
    e4_write_secs(part_start + 3 * blk_per_sec, bmp, blk_per_sec);

    /* Inode bitmap（块 4）：inode 1-2 已用（保留 + root） */
    for (uint32_t i = 0; i < 1024; i++) bmp[i] = 0;
    bmp[0] |= 0x03;  /* inode 1 + 2 */
    e4_write_secs(part_start + 4 * blk_per_sec, bmp, blk_per_sec);

    /* Inode table（块 5-8 = 4 块 x 1024 = 4096B = 32 x 128B） */
    static uint8_t itab[4096] E4_HIBUF;
    for (uint32_t i = 0; i < 4096; i++) itab[i] = 0;
    /* inode 1 (index 0): 保留（bad blocks），mode=0 */
    /* inode 2 (index 1): root directory */
    uint8_t *root = itab + 1 * 128;
    e4_wr16(root + INO_OFF_MODE, 0x41ED);      /* dir 0755 */
    e4_wr32(root + INO_OFF_SIZE_LO, blksize);   /* 1 block */
    e4_wr32(root + INO_OFF_BLOCKS_LO, blk_per_sec);
    e4_wr32(root + INO_OFF_IBLOCK, 9);          /* root dir = block 9 */
    e4_write_secs(part_start + 5 * blk_per_sec, itab, 4 * blk_per_sec);

    /* Root directory block（块 9） */
    static uint8_t rootblk[1024] E4_HIBUF;
    for (uint32_t i = 0; i < 1024; i++) rootblk[i] = 0;
    /* '.' entry */
    e4_wr32(rootblk + 0, 2);                     /* inode 2 */
    e4_wr16(rootblk + DE_OFF_RECLEN, 12);
    rootblk[DE_OFF_NAMELEN] = 1;
    rootblk[DE_OFF_TYPE] = 2;
    rootblk[DE_OFF_NAME] = '.';
    /* '..' entry (same as '.' for root) */
    e4_wr32(rootblk + 12, 2);
    e4_wr16(rootblk + 12 + DE_OFF_RECLEN, (uint16_t)(blksize - 12));
    rootblk[12 + DE_OFF_NAMELEN] = 2;
    rootblk[12 + DE_OFF_TYPE] = 2;
    rootblk[12 + DE_OFF_NAME] = '.';
    rootblk[12 + DE_OFF_NAME + 1] = '.';
    e4_write_secs(part_start + 9 * blk_per_sec, rootblk, blk_per_sec);

    /* 挂载 */
    if (ext4_mount(drive, part_start) != 0) return -1;
    return 0;
}
