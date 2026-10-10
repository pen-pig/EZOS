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
#define SB_OFF_FEAT_RO_COMPAT 100   /* s_feature_ro_compat */
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
#define INO_OFF_LINKS        26    /* i_links_count（2 字节） */
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
#define EXT4F_INCOMPAT_REJECT   (0x0001u /* COMPR */ | 0x0004u /* RECOVER */ \
                                 | 0x0010u /* META_BG */ | 0x0100u /* MMP */ \
                                 | 0x10000u /* ENCRYPT */ | 0x20000u /* CASEFOLD */ \
                                 | 0x40000u /* VERITY */)
/* INLINE_DATA(0x8000) 挂载不拒绝，读到 inline 文件时报错 */

/* ro_compat：卷上开着、但本驱动**写的时候维护不了**的特性。
 * 这些位意味着元数据带校验和或者分配单位不是块——我们一写就会把一个
 * Linux 认为合法的卷写成坏的（fsck 报 checksum mismatch），所以一律
 * 拒绝挂载（fail closed），而不是先挂上再慢慢坏。
 *   0x0010 GDT_CSUM        组描述符校验和
 *   0x0200 BIGALLOC        以 cluster（多块）为单位分配，位图语义不同
 *   0x0400 METADATA_CSUM   全元数据校验和（mkfs.ext4 现代默认开）
 *   0x0800 SNAPSHOT        ...
 * 不拒绝的：0x0001 SPARSE_SUPER / 0x0002 LARGE_FILE / 0x0008 EXTRA_ISIZE /
 * 0x0100 QUOTA / 0x1000 DIR_NLINK / 0x8000 HUGE_FILE（只读不写，无副作用）。 */
#define EXT4F_ROCOMPAT_REJECT  (0x0010u | 0x0200u | 0x0400u | 0x0800u)

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

/* extent 节点头 / index / extent 项都是 12 字节；inode 的 i_block 共 60 字节，
 * 所以 extent 根节点最多放 4 项。这两个常量给"磁盘上的 entries 上界"用。 */
#define EXT4_EXT_HDR_SIZE    12
#define EXT4_IBLOCK_SIZE     60

/* ---------- 挂载状态 ---------- */
static uint8_t  e4_drive;
static uint8_t  e4_mounted;
static uint32_t e4_part_lba;            /* 卷起始 LBA */
static uint32_t e4_blksize;             /* 1024<<log */
static uint32_t e4_blk_per_sec;         /* blocksize/512 */
static uint32_t e4_bpg, e4_ipg, e4_ino_size;
static uint32_t e4_groups;              /* 块组数 = ceil(blocks_total / bpg) */
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
    /* ro_compat 只看"写了会坏卷"的那几位。以前整字段都不看，于是带
     * metadata_csum 的现代 ext4 能被挂上并被写坏——而它还照样报挂载成功。 */
    if (rd32(sb + SB_OFF_FEAT_RO_COMPAT) & EXT4F_ROCOMPAT_REJECT) return -1;

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

    /* 卷不能比分区还大。blocks_total 是磁盘上的字段（64bit 特性下还是高低
     * 32 位拼出来的），坏盘可以写个天文数字，于是 e4_groups 变成上亿，
     * 遍历块组的循环（分配/扫描）一跑就是几亿次块读 —— 挂死。
     * 拿磁盘真实容量做闸（format 路径 1675 行也是这么取的）。 */
    uint32_t cap_secs = ata_capacity(e4_drive);
    uint64_t cap_blk = (cap_secs > part_start)
                       ? (uint64_t)(cap_secs - part_start) / e4_blk_per_sec
                       : 0;
    if (cap_blk == 0) cap_blk = 1u << 28;   /* 拿不到容量时的绝对上限 */
    if (e4_blocks_total > cap_blk) e4_blocks_total = cap_blk;

    e4_first_data_blk = rd32(sb + SB_OFF_FIRST_DATA);
    /* 组边界必须是 first_data_block + g*bpg（Linux ext4_group_first_block_no
     * 的定义）。早期版本按 g*bpg 算，两组之间差 1 个块——1KB 块时 group 1 的
     * 起始是 8193 而不是 8192。差一个块自己完全看不出来（读写都自洽），但
     * e2fsck / 宿主机按规范算组归属时会把块 8192 当成 group 0 的最后一块，
     * 位图与空闲计数立刻对不上。 */
    /* 下溢要挡在减法之前：blocks_total 小于 first_data_blk 时 uint64 一减就
     * 翻成天文数字，e4_groups 跟着爆。 */
    if (e4_blocks_total <= e4_first_data_blk) return -1;
    e4_groups = (uint32_t)((e4_blocks_total - e4_first_data_blk + e4_bpg - 1) /
                           e4_bpg);
    if (e4_groups == 0) return -1;               /* 0 块的卷：不可信，拒绝挂载 */
    if (e4_groups > 65536) return -1;            /* 同上：不可信，拒绝挂载 */
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
    /* 当前节点可用的字节数：根节点在 inode 的 i_block（60 字节），
     * 下潜后换成整块。用来给 entries 定上界。 */
    uint32_t room = EXT4_IBLOCK_SIZE;
    for (int depth_iter = 0; depth_iter < 6; depth_iter++) {
        if (rd16(hdr) != EXT4_EXT_MAGIC) return 0;
        uint32_t entries = rd16(hdr + EH_OFF_ENTRIES);
        uint32_t depth = rd16(hdr + EH_OFF_DEPTH);
        /* entries 是磁盘上随便写的 uint16：不设闸的话 hdr+12+i*12 能跑到
         * 缓冲外七百多 KB —— 那是**真**越界读（不是读到残留，是读别人的
         * 内存）。按节点实际容量截断。 */
        if (room <= EXT4_EXT_HDR_SIZE) return 0;
        uint32_t cap = (room - EXT4_EXT_HDR_SIZE) / 12;
        if (entries > cap) entries = cap;
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
        room = e4_blksize;
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

    /* 步数闸：每走一步要读一个块。cnt 是磁盘上随便写的 uint16（最大 65535），
     * 不设闸的话 6 层 x 65535 ≈ 39 万次 PIO 读 —— ls 会挂好几分钟，看着像死机。
     * 真实 htree 目录的内部节点项数是个位数到几十，1024 步绰绰有余。 */
    uint32_t steps = 0;
    const uint32_t step_max = 1024;

    while (sp >= 0) {
        if (++steps > step_max) return -1;
        e4_read_blk(stk[sp].blk, e4_dxbuf);
        int is_root = (sp == 0);
        /* 根块：'.','..' 24 字节 + info@24 + entries@32；节点块：12 字节头 + entries@12 */
        uint32_t cnt = rd16(e4_dxbuf + (is_root ? DX_ROOT_CNT_OFF : DX_NODE_CNT_OFF));
        uint32_t base = is_root ? 36 : 16;
        /* entries 必须整个落在块内。越界的那一半不会段错误（缓冲是 4KB），
         * 但会读到上一块留下的残数据，凭空长出子节点。按块大小截断。 */
        if (base >= e4_blksize) return -1;
        uint32_t room = (e4_blksize - base) / 8;
        if (cnt > room) cnt = room;

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
                /* 树坏了（坏盘/越界）就退回线性扫描：htree 目录的每个块本身
                 * 仍是合法 dirent 块，线性扫能列出全部条目，只是顺序不同。
                 * 比直接放弃整个目录好——坏盘至少还能 ls 出东西。 */
                if (e4_dx_collect(blk0, leaves, &nleaf, DX_MAX_LEAVES) != 0)
                    indexed = 0;
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

/* 更新 SB 和 **指定组** 的 free blocks / free inodes 计数。
 *
 * 必须传 group：多块组卷里，块 X 属于组 X/bpg、inode N 属于组 (N-1)/ipg。
 * 早期版本一律改组 0 的描述符——单组卷看不出问题，一旦卷超过一个组，
 * 后半盘的分配/释放就把计数记到错误的组上，e2fsck 会报 free blocks 错。 */
static void e4_update_counts(uint32_t group, int delta_blocks, int delta_inodes) {
    if (group >= e4_groups) group = 0;           /* 越界保护：绝不写到别的组 */
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

    /* 该组的组描述符。
     * bg_free_blocks_count_lo / bg_free_inodes_count_lo 是 **u16**（偏移
     * 12 与 14），bg_used_dirs_count_lo 在 16。早期按 u32 写在 12 和 16，
     * 结果 free_inodes 的 4 字节把 used_dirs 覆盖掉、而宿主按规范从 14 读
     * 出来是 0 —— 内核自己 rd32(16) 读的是自己写的位置，所以自证清白。 */
    uint32_t gd_blk;
    uint8_t *gd = e4_gd_ptr(group, &gd_blk);
    uint16_t gfb = rd16(gd + 12);  /* bg_free_blocks_count_lo */
    uint16_t gfi = rd16(gd + 14);  /* bg_free_inodes_count_lo */
    e4_wr16(gd + 12, (uint16_t)((int)gfb + delta_blocks));
    e4_wr16(gd + 14, (uint16_t)((int)gfi + delta_inodes));
    e4_write_blk(gd_blk, e4_blk);

    e4_blocks_free = (uint32_t)((int)e4_blocks_free + delta_blocks);
}

/* 组 g 的第一个**卷内块号** / 块号 -> 组号 / 块号 -> 组内位号。
 * 边界含 s_first_data_block（1KB 块时为 1）：与 Linux 及宿主机参考实现一致。 */
static uint32_t e4_group_first_block(uint32_t g) {
    return e4_first_data_blk + g * e4_bpg;
}
static uint32_t e4_group_of_block(uint32_t blkno) {
    if (blkno < e4_first_data_blk) return 0;
    return (blkno - e4_first_data_blk) / e4_bpg;
}
static uint32_t e4_block_in_group(uint32_t blkno) {
    return blkno - e4_group_first_block(e4_group_of_block(blkno));
}
/* inode 号从 1 开始，与块不同：组 = (ino-1)/ipg，不受 first_data_block 影响 */
static uint32_t e4_group_of_inode(uint32_t ino)   { return (ino - 1) / e4_ipg; }

/* 在块位图中分配一个块；返回**卷内块号**，失败返回 0。
 *
 * 遍历所有块组：单组卷（<8MB @1KB 块）只在组 0 找，行为与以前完全一致；
 * 多组卷上组 0 用完后继续往后找，否则卷再大也只有前 8MB 可写。
 * 每个组的位图是"组内块号 -> 位"的，故位号 = blk - group*bpg。 */
static uint32_t e4_alloc_block(void) {
    /* 注意：e4_gd_* 都会重读 GDT 覆盖 e4_blk，必须先取完 GD 字段再加载位图 */
    uint32_t itbl_blks = (e4_ipg * e4_ino_size + e4_blksize - 1) / e4_blksize;
    for (uint32_t g = 0; g < e4_groups; g++) {
        uint32_t gstart = e4_group_first_block(g);
        uint32_t glimit = e4_blocks_total - gstart;    /* 本组实际块数 */
        if (glimit > e4_bpg) glimit = e4_bpg;
        if (glimit <= e4_first_data_blk) continue;
        uint32_t itbl = e4_gd_inode_table(g);
        uint32_t bmp_blk = e4_gd_block_bitmap(g);
        if (bmp_blk == 0) continue;
        uint32_t start = itbl + itbl_blks - gstart;    /* 转成组内块号 */
        if (start < 2) start = 2;                      /* 组 0：跳过块 0/1 */
        e4_read_blk(bmp_blk, e4_blk);
        for (uint32_t i = start; i < glimit; i++) {
            uint32_t byte_idx = i / 8;
            uint32_t bit_idx = i % 8;
            if (byte_idx >= e4_blksize) break;
            if (!(e4_blk[byte_idx] & (1u << bit_idx))) {
                e4_blk[byte_idx] |= (1u << bit_idx);
                e4_write_blk(bmp_blk, e4_blk);
                e4_update_counts(g, -1, 0);
                return gstart + i;
            }
        }
    }
    return 0;
}

static void e4_free_block(uint32_t blkno) {
    if (blkno == 0) return;
    uint32_t g = e4_group_of_block(blkno);
    if (g >= e4_groups) return;
    uint32_t bmp_blk = e4_gd_block_bitmap(g);
    if (bmp_blk == 0) return;
    e4_read_blk(bmp_blk, e4_blk);
    uint32_t in_g = e4_block_in_group(blkno);
    uint32_t byte_idx = in_g / 8;
    uint32_t bit_idx = in_g % 8;
    if (byte_idx < e4_blksize)
        e4_blk[byte_idx] &= ~(1u << bit_idx);
    e4_write_blk(bmp_blk, e4_blk);
    e4_update_counts(g, 1, 0);
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
    /* 位图扫描区（跳过 SB/GDT/位图/inode 表元数据区）。与 e4_alloc_block
     * 一样要遍历所有块组，否则多组卷上连续段只能落在组 0。 */
    uint32_t itbl_blks = (e4_ipg * e4_ino_size + e4_blksize - 1) / e4_blksize;
    for (uint32_t g = 0; g < e4_groups && *got == 0; g++) {
    uint32_t gstart = e4_group_first_block(g);
    uint32_t glimit = e4_blocks_total - gstart;
    if (glimit > e4_bpg) glimit = e4_bpg;
    if (glimit <= e4_first_data_blk) continue;
    uint32_t itbl = e4_gd_inode_table(g);
    uint32_t bmp_blk = e4_gd_block_bitmap(g);
    if (bmp_blk == 0) continue;
    uint32_t start = itbl + itbl_blks - gstart;
    if (start < 2) start = 2;
    e4_read_blk(bmp_blk, e4_blk);

    /* 贪心：找第一个 >= want 的洞就取 want 块；
     * 否则取整个过程中遇到的最大洞。 */
    uint32_t best_start = 0, best_len = 0;
    uint32_t cur_start = 0, cur_len = 0;
    for (uint32_t i = start; i < glimit; i++) {
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
    if (best_len == 0) continue;

    /* 标记位图 */
    for (uint32_t i = 0; i < best_len; i++) {
        uint32_t b = best_start + i;
        e4_blk[b / 8] |= (1u << (b % 8));
    }
    e4_write_blk(bmp_blk, e4_blk);
    e4_update_counts(g, -(int)best_len, 0);
    *got = best_len;
    return gstart + best_start;
    }   /* for groups */
    return 0;
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
    /* 同 e4_map_extent：entries/depth 都来自磁盘。不设闸的话
     * hdr+12+(entries-1)*12 会读到 inode 缓冲外几百 KB。
     * 写路径直接拒绝（不能截断——截断会让我们改坏别处的数据）。 */
    if (depth > 5) return -1;
    if (entries > (EXT4_IBLOCK_SIZE - EXT4_EXT_HDR_SIZE) / 12) return -1;

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
        if (lents > (e4_blksize - EXT4_EXT_HDR_SIZE) / 12) return -1;
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

/* 分配一个 inode；返回 inode 号，失败返回 0。
 * 遍历所有块组：inode 号 = 组号*ipg + 组内索引 + 1。 */
static uint32_t e4_alloc_inode(void) {
    /* inode 号从 1 开始；每组前 11 个保留（EXT4_FIRST_INO=11 for 1KB 块） */
    uint32_t first = 11;
    for (uint32_t g = 0; g < e4_groups; g++) {
        uint32_t bmp_blk = e4_gd_inode_bitmap(g);
        if (bmp_blk == 0) continue;
        e4_read_blk(bmp_blk, e4_blk);
        for (uint32_t i = first; i < e4_ipg; i++) {
            uint32_t byte_idx = i / 8;
            uint32_t bit_idx = i % 8;
            if (byte_idx >= e4_blksize) break;
            if (!(e4_blk[byte_idx] & (1u << bit_idx))) {
                e4_blk[byte_idx] |= (1u << bit_idx);
                e4_write_blk(bmp_blk, e4_blk);
                e4_update_counts(g, 0, -1);
                return g * e4_ipg + i + 1;   /* inode 号 = 组基 + 索引 + 1 */
            }
        }
    }
    return 0;
}

static void e4_free_inode(uint32_t ino) {
    if (ino == 0) return;
    uint32_t g = e4_group_of_inode(ino);
    if (g >= e4_groups) return;
    uint32_t bmp_blk = e4_gd_inode_bitmap(g);
    if (bmp_blk == 0) return;
    e4_read_blk(bmp_blk, e4_blk);
    uint32_t idx = (ino - 1) % e4_ipg;
    uint32_t byte_idx = idx / 8;
    uint32_t bit_idx = idx % 8;
    if (byte_idx < e4_blksize)
        e4_blk[byte_idx] &= ~(1u << bit_idx);
    e4_write_blk(bmp_blk, e4_blk);
    e4_update_counts(g, 0, 1);
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
            if (rec_len < 8 || (rec_len & 3u)) break;
            if (off + rec_len > e4_blksize) break;
            uint32_t child = rd32(e4_dblk + off + DE_OFF_INODE);
            uint32_t existing_len = e4_dblk[off + DE_OFF_NAMELEN];
            uint32_t actual = (8 + existing_len + 3) & ~3u;
            if (actual > rec_len) break;            /* 目录项损坏：不再往下扫 */
            uint32_t remain = rec_len - actual;

            /* 情况一：复用删除留下的空项（child==0） */
            if (child == 0 && rec_len >= need) {
                e4_wr32(e4_dblk + off + DE_OFF_INODE, target_ino);
                e4_wr16(e4_dblk + off + DE_OFF_RECLEN, (uint16_t)rec_len);
                e4_dblk[off + DE_OFF_NAMELEN] = (uint8_t)name_len;
                e4_dblk[off + DE_OFF_TYPE] = ftype;
                for (uint32_t i = 0; i < name_len; i++)
                    e4_dblk[off + DE_OFF_NAME + i] = (uint8_t)name[i];
                e4_write_blk(pb, e4_dblk);
                return 0;
            }

            /* 情况二：分裂**最后一项**的尾部空间。
             *
             * ext4 目录块的最后一项 rec_len 恒覆盖到块尾的剩余空间（format
             * 写的 '..' 就是 rec_len=1012，实际只占 12）。早期版本只肯在
             * child==0 的空项上分裂，于是每个新文件都走"扩展一个新块"分支
             * ——1KB 的目录块只能放 1 个文件，写 12 个就把 12 个直接块撑满
             * 然后开始失败。症状是"ls 看得到、重启后只剩一个"。 */
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
static void e4_free_extent_node(const uint8_t *hdr, uint32_t depth,
                                uint32_t room) {
    if (rd16(hdr) != EXT4_EXT_MAGIC) return;
    /* depth 也是磁盘上的 uint16。不加闸的话递归 65535 层 —— 每层一个栈帧，
     * 任务内核栈才 16KB，rm 一个坏文件就能把它写穿。ext4 规范上限 5。 */
    if (depth > 5) return;
    uint32_t entries = rd16(hdr + EH_OFF_ENTRIES);
    if (room <= EXT4_EXT_HDR_SIZE) return;
    uint32_t cap = (room - EXT4_EXT_HDR_SIZE) / 12;
    if (entries > cap) entries = cap;
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
            e4_free_extent_node(e4_leaf, depth - 1, e4_blksize);
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
        e4_free_extent_node(hdr, rd16(hdr + EH_OFF_DEPTH),
                            EXT4_IBLOCK_SIZE);
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

/* 删除空目录（rmdir）：只收目录，绝不碰普通文件。
 * 关键顺序：先把目录项从父目录摘掉并落盘，之后才回收数据块/inode，
 * 否则一旦写盘失败会出现"目录项还在却指向已释放块"的悬空引用。 */
static int e4_rmdir_empty_cb(const char *name, uint32_t name_len, uint32_t ino,
                             uint8_t ftype, void *ctx) {
    (void)name; (void)name_len; (void)ino; (void)ftype;
    *(int *)ctx = 0;   /* 发现任何非 '.' / '..' 条目 -> 非空 */
    return 1;          /* 停止遍历 */
}

int ext4_rmdir(const char *name) {
    if (name == 0 || name[0] != '/') return -1;

    /* 根目录 "/" 本身拒绝 */
    if (name[1] == '\0') return -1;

    /* 取末段组件名，并拒绝 "." / ".." */
    const char *slash = 0;
    const char *p = name;
    while (*p) { if (*p == '/') slash = p; p++; }
    if (!slash) return -1;
    const char *fname = slash + 1;
    uint32_t fname_len = 0;
    while (fname[fname_len] && fname[fname_len] != '/') fname_len++;
    if (fname_len == 0 || fname_len > 255) return -1;
    if (fname_len == 1 && fname[0] == '.') return -1;
    if (fname_len == 2 && fname[0] == '.' && fname[1] == '.') return -1;

    /* 解析父目录 inode */
    uint32_t parent_ino;
    if (slash == name) {
        parent_ino = EXT4_ROOT_INO;
    } else {
        char parent_path[256];
        uint32_t plen = (uint32_t)(slash - name);
        if (plen >= 256) return -1;
        for (uint32_t i = 0; i < plen; i++) parent_path[i] = name[i];
        parent_path[plen] = 0;
        int is_dir;
        uint32_t pin = e4_resolve(parent_path, &is_dir, 0, 0);
        if (pin == 0 || !is_dir) return -1;
        parent_ino = pin;
    }

    /* 在父目录中查找目标 */
    uint32_t target_ino;
    uint8_t target_type;
    if (e4_dir_find(parent_ino, fname, fname_len, &target_ino, &target_type) != 0)
        return -1;   /* 不存在或查找出错 */

    /* 读取目标 inode，确为目录 */
    static uint8_t ino[EXT4_MAX_INODESIZE];
    if (e4_read_inode(target_ino, ino) != 0) return -1;
    if (!e4_ino_is_dir(ino)) return -1;   /* 是普通文件 -> 拒绝 */

    /* 拒绝根 inode（理论上到不了，因 "/" 已被拒） */
    if (target_ino == EXT4_ROOT_INO) return -1;

    /* 检查空目录：除 '.' / '..' 外的任何条目都算非空 */
    int empty = 1;
    if (e4_dir_walk(target_ino, e4_rmdir_empty_cb, &empty) != 0) return -1;
    if (!empty) return -1;   /* 非空：不释放任何东西 */

    /* === 开始修改磁盘（元数据优先） === */

    /* 1) 从父目录摘掉目录项并落盘。成功后才回收块。 */
    if (e4_dir_del_entry(parent_ino, target_ino) != 0) return -1;

    /* 2) 回收目标目录的数据块（extent / legacy 任意深度） */
    e4_free_inode_blocks(ino);

    /* 3) 将 links_count 归零并写回 inode（ext4 要求，避免 fsck 误判为活 inode） */
    e4_wr16(ino + INO_OFF_LINKS, 0);
    e4_write_inode(target_ino, ino);

    /* 4) 回收 inode（inode bitmap + counts） */
    e4_free_inode(target_ino);

    return 0;
}

/* ============================================================
 * 格式化
 * refs: e2fsprogs misc/mke2fs.c - 布局参数参考
 *
 * 布局（1KB 块，blocks_per_group=8192，inodes_per_group=256）：
 *   组 0:    块 0 保留 | 块 1 超级块 | 块 2..  GDT | 块位图 | inode 位图
 *            | inode 表(32 块) | 根目录块 | 数据 ...
 *   备份组:  组内开头放 SB + GDT 副本（3/5/7 的幂，含组 1），再放位图/inode 表
 *   普通组:  块位图 | inode 位图 | inode 表 | 数据 ...
 *
 * 卷大小**由磁盘实际容量决定**（ata_capacity）。早期版本写死 1024 块=1MB，
 * 在 16MB 的数据盘上只做出 6% 的可用空间，剩下 94% 的扇区永远摸不到。
 * 拿不到容量就拒绝格式化——猜一个大小写下去会造出越界的文件系统。
 * ============================================================ */

/* 1KB 块时块位图只有 1024 字节 = 8192 位，故 blocks_per_group 上限 8192。
 * 想更大就得换更大的块或让位图占多块——都没必要。 */
#define E4_FMT_BPG        8192u
/* 每组 256 个 inode：inode 表 = 256 x 128B = 32KB = 32 块。
 * 16MB 卷 2 组 = 512 个 inode，够日用（mke2fs 默认 16KB/inode 会给 1024，
 * 这里偏保守，省下 64KB 元数据）。 */
#define E4_FMT_IPG        256u
/* 块组上限：1024 组 x 8MB = 8GB。更大的盘也只吃前 8GB。
 * 运行时 e4_gd() 已经按 gd_off/blksize 跨块读 GDT（1024 组 x 32B = 32KB，
 * 即 32 个 1KB 块），所以放大这里不需要动读路径。
 * 代价是 4 张 static 表各 4KB（共 16KB，落在 19MB 的 .bss 区），
 * 以及 PIO 逐块写元数据随组数线性变慢（8GB 约 34K 次块写）。 */
#define E4_FMT_MAX_GROUPS 1024u
/* 保留 inode 1..11（bad blocks / root / resize / ...），分配从 12 开始 */
#define E4_FMT_FREE_INO_BASE 11u

/* 稀疏超级块（ro_compat SPARSE_SUPER）：只在 3/5/7 的幂（含 1）这些组里
 * 放 SB+GDT 备份。开了这个特性就必须按同一集合写备份，否则 e2fsck 报
 * "backup superblock missing"——两边必须对上。 */
static int e4_is_backup_group(uint32_t g) {
    if (g == 0) return 0;                 /* 组 0 是主超级块，不算备份 */
    uint32_t v;
    v = g; while (v % 3u == 0) v /= 3u;  if (v == 1u) return 1;
    v = g; while (v % 5u == 0) v /= 5u;  if (v == 1u) return 1;
    v = g; while (v % 7u == 0) v /= 7u;  if (v == 1u) return 1;
    return 0;
}

int ext4_format(uint8_t drive, uint32_t part_start) {
    /* 所有结构写入都经 e4_write_secs，而它只用全局 e4_drive；必须先指向目标盘，
     * 否则会写到上一次挂载残留的 e4_drive（默认 0=引导盘），导致 mount 读不到 SB。 */
    e4_drive = drive;

    /* 容量是布局的唯一依据，拿不到就拒绝（fail closed）。 */
    uint32_t disk_secs = ata_capacity(drive);
    if (disk_secs == 0) return -1;

    if (part_start == 0) part_start = 1;
    const uint32_t blksize = 1024;
    const uint32_t blk_per_sec = blksize / 512;
    const uint32_t ino_size = 128;
    const uint32_t itb_blks = E4_FMT_IPG * ino_size / blksize;   /* 32 */
    const uint32_t first_data = 1;         /* 1KB 块：块 0 是引导/保留块 */

    if (disk_secs <= part_start + 64) return -1;      /* 太小的盘不值得格式化 */
    uint32_t vol_blocks = (disk_secs - part_start) / blk_per_sec;
    /* 组数按 **first_data 之后** 的块数算（组边界含 first_data 偏移） */
    uint32_t data_blocks = (vol_blocks > first_data) ? (vol_blocks - first_data) : 0;
    uint32_t groups = (data_blocks + E4_FMT_BPG - 1) / E4_FMT_BPG;
    if (groups == 0) return -1;
    if (groups > E4_FMT_MAX_GROUPS) {
        groups = E4_FMT_MAX_GROUPS;
        vol_blocks = first_data + groups * E4_FMT_BPG;
    }
    uint32_t gdt_blks = (groups * 32 + blksize - 1) / blksize;
    if (gdt_blks == 0) gdt_blks = 1;

    /* 逐组元数据块号。放 static：内核栈只有几 KB，256 项 x 4 数组会爆。 */
    static uint32_t bb_blk[E4_FMT_MAX_GROUPS];
    static uint32_t ib_blk[E4_FMT_MAX_GROUPS];
    static uint32_t it_blk[E4_FMT_MAX_GROUPS];
    static uint32_t g_used[E4_FMT_MAX_GROUPS];

    const uint32_t gdt_blk = 2;           /* 1KB 块时 GDT 恒在块 2 */
    uint32_t root_blk = 0;
    uint32_t total_free = 0;

    for (uint32_t g = 0; g < groups; g++) {
        /* 组边界含 first_data 偏移：组 1 从块 8193 起，不是 8192 */
        uint32_t gstart = first_data + g * E4_FMT_BPG;
        uint32_t next;
        if (g == 0) {
            next = first_data + 1 + gdt_blks;  /* 块 1 = 主 SB，块 2.. = GDT */
        } else if (e4_is_backup_group(g)) {
            next = gstart + 1 + gdt_blks;      /* 备份 SB 在组首，备份 GDT 紧跟 */
        } else {
            next = gstart;
        }
        bb_blk[g] = next++;
        ib_blk[g] = next++;
        it_blk[g] = next;
        next += itb_blks;
        if (g == 0) root_blk = next++;     /* 根目录占一个数据块 */
        g_used[g] = next - gstart;

        uint32_t in_group = vol_blocks - gstart;
        if (in_group > E4_FMT_BPG) in_group = E4_FMT_BPG;
        if (g_used[g] > in_group) return -1;    /* 元数据在本组放不下：拒绝 */
        total_free += in_group - g_used[g];
    }

    /* MBR：分区 0x83 从 LBA 1 起，长度 = 卷扇区数（不是 -1，早期写错过） */
    uint8_t mbr[512];
    for (int i = 0; i < 512; i++) mbr[i] = 0;
    mbr[447] = 0x00; mbr[448] = 0x02; mbr[449] = 0x00;
    mbr[450] = 0x83;
    mbr[451] = 0x00; mbr[452] = 0x3F; mbr[453] = 0xFF;
    e4_wr32(mbr + 454, part_start);
    e4_wr32(mbr + 458, vol_blocks * blk_per_sec);
    mbr[510] = 0x55; mbr[511] = 0xAA;
    if (ata_write_sector(drive, 0, mbr) != 0) return -1;

    /* Superblock（块 1） */
    static uint8_t sb[1024] E4_HIBUF;
    for (uint32_t i = 0; i < 1024; i++) sb[i] = 0;
    e4_wr32(sb + 0, groups * E4_FMT_IPG);              /* s_inodes_count */
    e4_wr32(sb + 4, vol_blocks);                       /* s_blocks_count_lo */
    e4_wr32(sb + 8, 0);                                /* s_r_blocks_count_lo */
    e4_wr32(sb + 12, total_free);                      /* s_free_blocks_count_lo */
    e4_wr32(sb + 16, groups * (E4_FMT_IPG - E4_FMT_FREE_INO_BASE));/* free_inodes */
    e4_wr32(sb + 20, 1);                               /* s_first_data_block */
    e4_wr32(sb + 24, 0);                               /* s_log_block_size = 0 */
    e4_wr32(sb + 28, 0);                               /* s_log_cluster_size */
    e4_wr32(sb + 32, E4_FMT_BPG);                      /* s_blocks_per_group */
    e4_wr32(sb + 36, E4_FMT_BPG);                      /* s_clusters_per_group */
    e4_wr32(sb + 40, E4_FMT_IPG);                      /* s_inodes_per_group */
    e4_wr16(sb + 56, EXT4_MAGIC);
    e4_wr16(sb + 58, 1);                               /* s_state = 干净卸载 */
    e4_wr32(sb + 76, 1);                               /* s_rev_level = dynamic */
    e4_wr32(sb + 84, E4_FMT_FREE_INO_BASE);            /* s_first_ino = 11 */
    e4_wr16(sb + 88, (uint16_t)ino_size);              /* s_inode_size */
    e4_wr32(sb + 96, 0x0042);                          /* incompat: FILETYPE|EXTENTS */
    e4_wr32(sb + 100, 0x0001);                         /* ro_compat: SPARSE_SUPER */
    e4_wr16(sb + 282, 0);                              /* s_desc_size = 0 (32B) */
    sb[510] = 0x53; sb[511] = 0xEF;                    /* ext signature */
    e4_write_secs(part_start + first_data * blk_per_sec, sb, blk_per_sec);

    /* GDT（块 2..）：每组一个 32 字节描述符。
     * 逐块写而不是整表缓冲：1024 组要 32KB，而 .bss.hi 只剩几十 KB 余量
     * （上限 2MB，已用 ~1.97MB），一次性 static 出来会链接失败。 */
    static uint8_t gdbuf[1024] E4_HIBUF;
    const uint32_t gd_per_blk = blksize / 32;           /* 32 */
    for (uint32_t b = 0; b < gdt_blks; b++) {
        for (uint32_t i = 0; i < blksize; i++) gdbuf[i] = 0;
        for (uint32_t k = 0; k < gd_per_blk; k++) {
            uint32_t g = b * gd_per_blk + k;
            if (g >= groups) break;
            uint32_t in_group = vol_blocks - (first_data + g * E4_FMT_BPG);
            if (in_group > E4_FMT_BPG) in_group = E4_FMT_BPG;
            uint8_t *d = gdbuf + k * 32;
            e4_wr32(d + 0, bb_blk[g]);                     /* bg_block_bitmap_lo */
            e4_wr32(d + 4, ib_blk[g]);                     /* bg_inode_bitmap_lo */
            e4_wr32(d + 8, it_blk[g]);                     /* bg_inode_table_lo */
            e4_wr16(d + 12, (uint16_t)(in_group - g_used[g]));   /* free_blocks_lo */
            e4_wr16(d + 14, (uint16_t)(E4_FMT_IPG - E4_FMT_FREE_INO_BASE));
            e4_wr16(d + 16, (uint16_t)((g == 0) ? 1 : 0));      /* bg_used_dirs_lo */
        }
        e4_write_secs(part_start + (gdt_blk + b) * blk_per_sec, gdbuf,
                      blk_per_sec);
    }

    /* 每组的块位图 / inode 位图 / inode 表。
     *
     * inode 表**不整表缓冲**：256 个 inode x 128B = 32KB，而 .bss.hi 只剩
     * 几十 KB 余量（上限 2MB，已用 ~1.97MB），一加上就链接失败。改成
     * 逐块写：组 0 的第一块填好根目录后写出，其余块用同一个全零块刷。
     * 扇区写入次数不变，缓冲从 32KB 降到 2KB。 */
    static uint8_t bmp[1024] E4_HIBUF;
    static uint8_t iblk[1024] E4_HIBUF;    /* inode 表当前块 */
    static uint8_t zblk[1024] E4_HIBUF;    /* 全零块 */
    for (uint32_t i = 0; i < 1024; i++) zblk[i] = 0;
    for (uint32_t g = 0; g < groups; g++) {
        /* 块位图：本组前 g_used 块（元数据 + 根目录）已用。
         * 位号是**组内**块号，不是卷内块号。 */
        for (uint32_t i = 0; i < 1024; i++) bmp[i] = 0;
        for (uint32_t i = 0; i < g_used[g] && i < 8192; i++)
            bmp[i / 8] |= (uint8_t)(1u << (i % 8));
        e4_write_secs(part_start + bb_blk[g] * blk_per_sec, bmp, blk_per_sec);

        /* inode 位图：每组前 11 个保留（与 e4_alloc_inode 的起始下标一致） */
        for (uint32_t i = 0; i < 1024; i++) bmp[i] = 0;
        for (uint32_t i = 0; i < E4_FMT_FREE_INO_BASE; i++)
            bmp[i / 8] |= (uint8_t)(1u << (i % 8));
        e4_write_secs(part_start + ib_blk[g] * blk_per_sec, bmp, blk_per_sec);

        /* inode 表：逐块清零；组 0 的第 0 块里填根目录（inode 2） */
        for (uint32_t i = 0; i < 1024; i++) iblk[i] = 0;
        if (g == 0) {
            uint8_t *root = iblk + 1 * ino_size;
            e4_wr16(root + INO_OFF_MODE, 0x41ED);      /* dir 0755 */
            e4_wr16(root + INO_OFF_LINKS, 2);          /* . 与 .. */
            e4_wr32(root + INO_OFF_SIZE_LO, blksize);  /* 1 block */
            e4_wr32(root + INO_OFF_BLOCKS_LO, blk_per_sec);
            /* 根目录用 **extent 映射**，与 mkdir 出来的目录同构。
             * 早期这里只写 i_block[0] 而不置 EXTENTS_FL → legacy 直接块目录，
             * 最多 12 块（e4_extent_append 对 lblk>=12 直接返回 -1）。1KB 块
             * 下 12 块约 700 个条目就写不进去了，而且这是内核自己 format 出来
             * 的根目录——日用系统里最容易撑满的那个目录。 */
            uint8_t *hdr = root + INO_OFF_IBLOCK;
            e4_wr16(hdr + EH_OFF_MAGIC, EXT4_EXT_MAGIC);
            e4_wr16(hdr + EH_OFF_ENTRIES, 1);
            e4_wr16(hdr + 4, 4);                    /* eh_max = 4（根内嵌） */
            e4_wr16(hdr + EH_OFF_DEPTH, 0);
            uint8_t *ex = hdr + 12;
            e4_wr32(ex + EE_OFF_BLOCK, 0);          /* 逻辑块 0 */
            e4_wr16(ex + EE_OFF_LEN, 1);
            e4_wr16(ex + EE_OFF_START_HI, 0);
            e4_wr32(ex + EE_OFF_START_LO, root_blk);
            e4_wr32(root + INO_OFF_FLAGS, EXT4_EXTENTS_FL);
        }
        e4_write_secs(part_start + it_blk[g] * blk_per_sec, iblk, blk_per_sec);
        for (uint32_t b = 1; b < itb_blks; b++)
            e4_write_secs(part_start + (it_blk[g] + b) * blk_per_sec, zblk,
                          blk_per_sec);
    }

    /* Root directory block */
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
    e4_write_secs(part_start + root_blk * blk_per_sec, rootblk, blk_per_sec);

    /* 备份超级块 + 备份 GDT（稀疏超级块集合）。
     * 必须与 ro_compat SPARSE_SUPER 声明的集合完全一致。 */
    for (uint32_t g = 1; g < groups; g++) {
        if (!e4_is_backup_group(g)) continue;
        uint32_t gstart = first_data + g * E4_FMT_BPG;
        e4_wr16(sb + 254, (uint16_t)g);                /* s_block_group_nr */
        e4_write_secs(part_start + gstart * blk_per_sec, sb, blk_per_sec);
        for (uint32_t b = 0; b < gdt_blks; b++) {
            /* 重填同一份内容（gdbuf 已被后面的块覆盖，这里逐块重建） */
            for (uint32_t i = 0; i < blksize; i++) gdbuf[i] = 0;
            for (uint32_t k = 0; k < gd_per_blk; k++) {
                uint32_t gg = b * gd_per_blk + k;
                if (gg >= groups) break;
                uint32_t in_group = vol_blocks - (first_data + gg * E4_FMT_BPG);
                if (in_group > E4_FMT_BPG) in_group = E4_FMT_BPG;
                uint8_t *d = gdbuf + k * 32;
                e4_wr32(d + 0, bb_blk[gg]);
                e4_wr32(d + 4, ib_blk[gg]);
                e4_wr32(d + 8, it_blk[gg]);
                e4_wr16(d + 12, (uint16_t)(in_group - g_used[gg]));
                e4_wr16(d + 14, (uint16_t)(E4_FMT_IPG - E4_FMT_FREE_INO_BASE));
                e4_wr16(d + 16, (uint16_t)((gg == 0) ? 1 : 0));
            }
            e4_write_secs(part_start + (gstart + 1 + b) * blk_per_sec, gdbuf,
                          blk_per_sec);
        }
    }
    e4_wr16(sb + 254, 0);

    /* 挂载 */
    if (ext4_mount(drive, part_start) != 0) return -1;
    return 0;
}
