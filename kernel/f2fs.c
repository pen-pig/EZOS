/*
 * f2fs.c - F2FS 驱动（读 + 写，标准卷）
 *
 * 支持范围（4KB 块）：
 *   - 双 CP pack 校验（CRC32，init=F2FS_SUPER_MAGIC）取高版本
 *   - NAT 查找：CP NAT journal 优先，回退 NAT 区块（含版本位图偏移，多块 NAT）
 *   - inline data（i_addr[1] 起）与 inline dentry
 *   - 常规文件：直接块 + 一级/二级间接节点链（grub_get_node_path）
 *   - 常规目录：dentry block 位图遍历（含多级哈希桶布局）
 *   - 写入：create/delete/mkdir + format，按 f2fs-tools mkfs.f2fs 磁盘布局：
 *     官方 SB/CP 字段偏移、CP pack 交替（版本号奇偶选 pack）、NAT/SIT 区块、
 *     SSA summary、标准 TEA 文件名哈希、curseg 游标分配、SIT valid_map 回收
 *   - 不支持：压缩（LZO/LZ4）、加密、符号链接、cp_payload 布局、NAT bits
 *
 * refs:
 *   - GRUB grub-core/fs/f2fs.c (GPLv3+) - 全部解析流程与磁盘布局常量
 *   - Linux include/linux/f2fs_fs.h - superblock/checkpoint/node 结构
 *   - f2fs-tools mkfs/f2fs_format.c + lib/libf2fs.c - 写入布局与 TEA hash
 *   - f2fs-tools fsck/fsck.c - SIT/SSA/NAT 语义交叉验证
 */
#include "f2fs.h"
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

/* ---------- 常量（GRUB f2fs.c / f2fs-tools f2fs_fs.h） ---------- */
#define F2FS_SUPER_MAGIC        0xF2F52010u
#define F2FS_BLKSIZE            4096u
#define F2FS_BLK_SECS           8           /* 4KB / 512 */
#define F2FS_MIN_LOG_SEC        9
#define F2FS_BLK_BITS           12

#define CHECKSUM_OFFSET         4092        /* CP_CHKSUM_OFFSET */
#define CRCPOLY_LE              0xEDB88320u

#define CP_COMPACT_SUM_FLAG     0x00000004u
#define CP_UMOUNT_FLAG          0x00000001u

#define NR_CURSEG_DATA_TYPE     3
#define NR_CURSEG_TYPE          6
#define CURSEG_HOT_DATA         0
#define CURSEG_WARM_DATA       1
#define CURSEG_COLD_DATA        2
#define CURSEG_HOT_NODE         3
#define CURSEG_WARM_NODE        4
#define CURSEG_COLD_NODE        5

#define SUM_ENTRIES_SIZE        (7 * 512)   /* SUMMARY_SIZE * ENTRIES_IN_SUM */
#define SUM_JOURNAL_SIZE        (F2FS_BLKSIZE - 5 - SUM_ENTRIES_SIZE)  /* 507 */
#define JENTRY_SIZE             13          /* nid(4) + nat_entry(9) */
#define SIT_JENTRY_SIZE         78          /* segno(4) + sit_entry(74) */

#define NAT_ENTRY_SIZE          9           /* version(1) ino(4) block_addr(4) */
#define NAT_ENTRY_PER_BLOCK     (F2FS_BLKSIZE / NAT_ENTRY_SIZE)        /* 455 */
#define SIT_ENTRY_SIZE          74           /* vblocks(2) valid_map(64) mtime(8) */
#define SIT_ENTRY_PER_BLOCK     (F2FS_BLKSIZE / SIT_ENTRY_SIZE)       /* 55 */
#define SIT_VBLOCK_MAP_SIZE     64
#define SIT_VBLOCKS_SHIFT       10
#define SIT_VBLOCKS_MASK       ((1 << SIT_VBLOCKS_SHIFT) - 1)

#define F2FS_SLOT_LEN           8
#define NR_DENTRY_IN_BLOCK      214
#define SIZE_OF_DIR_ENTRY       11
#define SIZE_OF_DENTRY_BITMAP   ((NR_DENTRY_IN_BLOCK + 7) / 8)         /* 27 */
#define SIZE_OF_RESERVED        (F2FS_BLKSIZE - \
                                 ((SIZE_OF_DIR_ENTRY + F2FS_SLOT_LEN) * \
                                  NR_DENTRY_IN_BLOCK + SIZE_OF_DENTRY_BITMAP)) /* 3 */

#define F2FS_INLINE_XATTR_ADDRS 50
#define DEF_ADDRS_PER_INODE     923         /* (4096-360-20-24)/4，官方公式 */
#define ADDRS_PER_BLOCK         1018
#define NIDS_PER_BLOCK          1018
#define NODE_DIR1_BLOCK         (DEF_ADDRS_PER_INODE + 1)
#define NODE_DIR2_BLOCK         (DEF_ADDRS_PER_INODE + 2)
#define NODE_IND1_BLOCK         (DEF_ADDRS_PER_INODE + 3)
#define NODE_IND2_BLOCK         (DEF_ADDRS_PER_INODE + 4)
#define NODE_DIND_BLOCK         (DEF_ADDRS_PER_INODE + 5)

/* inline 容量（官方 MAX_INLINE_DATA 公式，无 xattr/extra 字段）：
 *   INLINE_DATA 文件：xattr 预留 0 -> 4*(923-0-1) = 3688
 *   INLINE_DENTRY 目录：xattr 预留 50（get_inline_xattr_addrs 对
 *   INLINE_DENTRY 也返回 DEFAULT_INLINE_XATTR_ADDRS）-> 3488 */
#define MAX_INLINE_DATA         (4 * (DEF_ADDRS_PER_INODE - 1))         /* 3688 */
#define MAX_INLINE_DIR_DATA    (4 * (DEF_ADDRS_PER_INODE - \
                                      F2FS_INLINE_XATTR_ADDRS - 1))     /* 3488 */
#define NR_INLINE_DENTRY        (MAX_INLINE_DIR_DATA * 8 / \
                                 ((SIZE_OF_DIR_ENTRY + F2FS_SLOT_LEN) * 8 + 1)) /* 182 */
#define INLINE_DENTRY_BITMAP_SIZE ((NR_INLINE_DENTRY + 7) / 8)         /* 23 */
#define INLINE_RESERVED_SIZE    (MAX_INLINE_DIR_DATA - \
                                 ((SIZE_OF_DIR_ENTRY + F2FS_SLOT_LEN) * \
                                  NR_INLINE_DENTRY + INLINE_DENTRY_BITMAP_SIZE))

/* i_inline 位 */
#define F2FS_INLINE_XATTR       0x01
#define F2FS_INLINE_DATA        0x02
#define F2FS_INLINE_DENTRY      0x04
#define F2FS_DATA_EXIST         0x08

/* 节点 footer（节点块 4096-24 起） */
#define FOOT_OFF_NID            0
#define FOOT_OFF_INO            4
#define FOOT_OFF_FLAG           8
#define FOOT_OFF_CPVER          12          /* __le64 */
#define FOOT_OFF_NEXTADDR       20          /* next_blkaddr */

/* 目录项 file_type */
#define F2FS_FT_REG_FILE        1
#define F2FS_FT_DIR             2

/* superblock 偏移（官方 struct f2fs_super_block，F2FS_SUPER_OFFSET=1024） */
#define SB_OFF_MAGIC            0
#define SB_OFF_LOG_SEC          8           /* log_sectorsize */
#define SB_OFF_LOG_SPB          12          /* log_sectors_per_block */
#define SB_OFF_LOG_BLK          16
#define SB_OFF_LOG_BPS          20
#define SB_OFF_SEG_SPSEC        24          /* segs_per_sec */
#define SB_OFF_SECS_PZONE       28
#define SB_OFF_BLOCK_COUNT      36          /* dummy2 覆盖区，__le64 */
#define SB_OFF_SECTION_COUNT    44
#define SB_OFF_SEGMENT_COUNT    48
#define SB_OFF_SEG_CKPT         52
#define SB_OFF_SEG_SIT          56
#define SB_OFF_SEG_NAT          60
#define SB_OFF_SEG_SSA          64
#define SB_OFF_SEG_MAIN         68
#define SB_OFF_SEG0_BLKADDR     72          /* dummy2 尾 4 字节 */
#define SB_OFF_CP_BLKADDR       76
#define SB_OFF_SIT_BLKADDR      80
#define SB_OFF_NAT_BLKADDR      84
#define SB_OFF_SSA_BLKADDR      88
#define SB_OFF_MAIN_BLKADDR     92
#define SB_OFF_ROOT_INO         96
#define SB_OFF_NODE_INO         100
#define SB_OFF_META_INO         104
#define SB_OFF_CP_PAYLOAD       1664        /* 扩展名表 64*8=512 字节之后 */

/* checkpoint 偏移（官方 struct f2fs_checkpoint） */
#define CP_OFF_VER              0           /* __le64 */
#define CP_OFF_USER_BLOCKS      8
#define CP_OFF_VALID_BLOCKS     16
#define CP_OFF_RSVD_SEGS        24
#define CP_OFF_OVERPROV_SEGS    28
#define CP_OFF_FREE_SEGS        32
#define CP_OFF_CUR_NODE_SEG     36          /* [8] __le32 */
#define CP_OFF_CUR_NODE_BLKOFF  68          /* [8] __le16 */
#define CP_OFF_CUR_DATA_SEG     84          /* [8] __le32 */
#define CP_OFF_CUR_DATA_BLKOFF  116         /* [8] __le16 */
#define CP_OFF_CKPT_FLAGS       132
#define CP_OFF_PACK_TOTAL       136
#define CP_OFF_PACK_START_SUM   140
#define CP_OFF_VALID_NODES      144
#define CP_OFF_VALID_INODES     148
#define CP_OFF_NEXT_FREE_NID    152
#define CP_OFF_SIT_VER_BYTES    156
#define CP_OFF_NAT_VER_BYTES    160
#define CP_OFF_CKSUM_OFF        164
#define CP_OFF_ELAPSED          168         /* __le64 */
#define CP_OFF_ALLOC_TYPE       176         /* [16] */
#define CP_OFF_BITMAP           192         /* sit_nat_version_bitmap */

/* inode 偏移（f2fs_node 前 4072 字节内的 f2fs_inode 部分） */
#define INO_OFF_MODE            0
#define INO_OFF_INLINE          3
#define INO_OFF_SIZE            16
#define INO_OFF_BLOCKS          24
#define INO_OFF_GENERATION      68
#define INO_OFF_CURRENT_DEPTH   72
#define INO_OFF_XATTR_NID       76
#define INO_OFF_PINO            84
#define INO_OFF_NAMELEN         88
#define INO_OFF_NAME            92
#define INO_OFF_DIR_LEVEL       347        /* i_name[255] 之后 */
#define INO_OFF_ADDR            360        /* i_addr[923] */
#define INO_OFF_NID             4052       /* i_nid[5]，节点尾 40 字节前 */

#define F2FS_MAX_NAME           255

/* NAT journal 项数上限（507-2)/13 = 38（libf2fs NAT_JOURNAL_ENTRIES） */
#define NAT_JOURNAL_MAX         ((SUM_JOURNAL_SIZE - 2) / JENTRY_SIZE)

/* ---------- 挂载状态 ---------- */
static uint8_t  f2_drive;
static uint8_t  f2_mounted;
static uint32_t f2_part_lba;
static uint32_t f2_bps;                 /* blocks_per_seg */
static uint32_t f2_cp_blkaddr, f2_nat_blkaddr, f2_main_blkaddr;
static uint32_t f2_sit_blkaddr, f2_ssa_blkaddr;
static uint32_t f2_root_ino;
static uint32_t f2_total_blocks;        /* block_count */
static uint32_t f2_main_segs;
static uint32_t f2_start_cp;            /* 选中的 CP pack 起始块 */
static uint32_t f2_sit_bytes;           /* SIT 位图字节数（cp_payload>0 时为 0） */
static uint8_t  f2_cp_payload_flag;     /* superblock cp_payload>0 */
static uint32_t f2_cp_ver;              /* 当前 CP 版本（低 32 位） */
static f2fs_info_t f2_info;

/* CP 副本（持久）、NAT/SIT journal 副本、节点/NAT/目录块暂存 */
/* 大缓冲放高内存段 .bss.hi（1MB+，见 linker.ld）：低 640KB 区留给栈/小数据 */
#define F2_HIBUF __attribute__((section(".bss.hi")))
static uint8_t f2_cp[F2FS_BLKSIZE] F2_HIBUF;
static uint8_t f2_natj[SUM_JOURNAL_SIZE] F2_HIBUF;
static uint8_t f2_sitj[SUM_JOURNAL_SIZE] F2_HIBUF;
static uint8_t f2_node[F2FS_BLKSIZE] F2_HIBUF;   /* 当前解析节点 */
static uint8_t f2_child[F2FS_BLKSIZE] F2_HIBUF;  /* 枚举子节点（隔离于 f2_node） */
static uint8_t f2_blk[F2FS_BLKSIZE] F2_HIBUF;    /* NAT 块 / 间接节点链暂存 */
static uint8_t f2_dblk[F2FS_BLKSIZE] F2_HIBUF;   /* 非 inline 目录块 */
static uint8_t f2_wnode[F2FS_BLKSIZE] F2_HIBUF;  /* 写路径子节点 */
static uint8_t f2_wblk[F2FS_BLKSIZE] F2_HIBUF;   /* 写路径数据/间接块 */
static uint8_t f2_meta[F2FS_BLKSIZE] F2_HIBUF;   /* 写路径 NAT/SIT/SSA 块 */
static uint8_t f2_fmt[F2FS_BLKSIZE] F2_HIBUF;    /* format 通用 4KB 暂存 */

static void f2_read_secs(uint32_t lba, uint8_t *buf, uint32_t nsecs) {
    for (uint32_t i = 0; i < nsecs; i++)
        ata_read_sector(f2_drive, lba + i, buf + i * 512);
}

/* 读 4KB 块 blkaddr（卷内块号） */
static void f2_read_block(uint32_t blkaddr, uint8_t *buf) {
    f2_read_secs(f2_part_lba + blkaddr * F2FS_BLK_SECS, buf, F2FS_BLK_SECS);
}

/* CRC32：init = F2FS_SUPER_MAGIC（GRUB grub_f2fs_cal_crc32 / Linux f2fs_crc32） */
static uint32_t f2_crc32(const uint8_t *buf, uint32_t len) {
    uint32_t crc = F2FS_SUPER_MAGIC;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ ((crc & 1) ? CRCPOLY_LE : 0);
    }
    return crc;
}

/* 大端位序（GRUB grub_f2fs_test_bit：MSB 在前；SIT valid_map 同序） */
static int f2_test_bit_be(uint32_t nr, const uint8_t *p) {
    return p[nr >> 3] & (1u << (7 - (nr & 7)));
}
static void f2_set_bit_be(uint32_t nr, uint8_t *p) {
    p[nr >> 3] |= (uint8_t)(1u << (7 - (nr & 7)));
}
static void f2_clear_bit_be(uint32_t nr, uint8_t *p) {
    p[nr >> 3] &= (uint8_t)~(1u << (7 - (nr & 7)));
}

/* ---------- CP pack 校验（GRUB validate_checkpoint） ---------- */
static int f2_validate_cp(uint32_t cp_addr, uint64_t *version) {
    uint32_t crc_offset, crc;
    f2_read_block(cp_addr, f2_blk);
    crc_offset = rd32(f2_blk + CP_OFF_CKSUM_OFF);
    if (crc_offset != CHECKSUM_OFFSET) return -1;
    crc = rd32(f2_blk + CHECKSUM_OFFSET);
    if (f2_crc32(f2_blk, crc_offset) != crc) return -1;
    uint64_t pre_ver = rd64(f2_blk + CP_OFF_VER);

    /* CP pack 尾块 */
    uint32_t total = rd32(f2_blk + CP_OFF_PACK_TOTAL);
    if (total == 0 || total > f2_bps) return -1;
    f2_read_block(cp_addr + total - 1, f2_child);
    crc_offset = rd32(f2_child + CP_OFF_CKSUM_OFF);
    if (crc_offset != CHECKSUM_OFFSET) return -1;
    crc = rd32(f2_child + CHECKSUM_OFFSET);
    if (f2_crc32(f2_child, crc_offset) != crc) return -1;
    uint64_t cur_ver = rd64(f2_child + CP_OFF_VER);
    if (cur_ver != pre_ver) return -1;

    *version = cur_ver;
    return 0;
}

/* 读 NAT/SIT journal（compact 卷：nat_j + sit_j 拼在 summary 块
 * [0,507)+[507,1014)；非 compact：NAT journal 在 HOT_DATA summary 尾部） */
static int f2_load_nat_journal(void) {
    uint32_t block;
    uint32_t flags = rd32(f2_cp + CP_OFF_CKPT_FLAGS);

    if (flags & CP_COMPACT_SUM_FLAG) {
        block = f2_start_cp + rd32(f2_cp + CP_OFF_PACK_START_SUM);
        f2_read_block(block, f2_blk);
        for (uint32_t i = 0; i < SUM_JOURNAL_SIZE; i++)
            f2_natj[i] = f2_blk[i];
        for (uint32_t i = 0; i < SUM_JOURNAL_SIZE; i++)
            f2_sitj[i] = f2_blk[SUM_JOURNAL_SIZE + i];
    } else {
        if (flags & CP_UMOUNT_FLAG)
            block = f2_start_cp + rd32(f2_cp + CP_OFF_PACK_TOTAL) -
                    (NR_CURSEG_TYPE + 1) + CURSEG_HOT_DATA;
        else
            block = f2_start_cp + rd32(f2_cp + CP_OFF_PACK_TOTAL) -
                    (NR_CURSEG_DATA_TYPE + 1) + CURSEG_HOT_DATA;
        f2_read_block(block, f2_blk);
        for (uint32_t i = 0; i < SUM_JOURNAL_SIZE; i++)
            f2_natj[i] = f2_blk[SUM_ENTRIES_SIZE + i];
        for (uint32_t i = 0; i < SUM_JOURNAL_SIZE; i++)
            f2_sitj[i] = 0;
    }
    return 0;
}

/* nat bitmap 指针（GRUB nat_bitmap_ptr：cp_payload>0 时在 bitmap 头部，
 * 否则跳过 SIT 位图 sit_ver_bitmap_bytesize 字节） */
static const uint8_t *f2_nat_bitmap(void) {
    (void)f2_cp_payload_flag;
    return f2_cp + CP_OFF_BITMAP + f2_sit_bytes;
}

/* ---------- NAT 查找（GRUB get_node_blkaddr） ---------- */
static uint32_t f2_journal_lookup(uint32_t nid) {
    uint16_t n = rd16(f2_natj);
    if (n > NAT_JOURNAL_MAX) n = NAT_JOURNAL_MAX;
    for (uint16_t i = 0; i < n; i++) {
        const uint8_t *e = f2_natj + 2 + (uint32_t)i * JENTRY_SIZE;
        if (rd32(e) == nid)
            return rd32(e + 4 + 5);       /* nid(4) + version(1) 后 block_addr */
    }
    return 0;
}

static uint32_t f2_nat_lookup(uint32_t nid) {
    uint32_t blkaddr = f2_journal_lookup(nid);
    if (blkaddr) return blkaddr;

    uint32_t block_off = nid / NAT_ENTRY_PER_BLOCK;
    uint32_t entry_off = nid % NAT_ENTRY_PER_BLOCK;
    uint32_t seg_off = block_off / f2_bps;
    uint32_t block_addr = f2_nat_blkaddr +
                          ((seg_off * f2_bps) << 1) +
                          (block_off & (f2_bps - 1));
    if (f2_test_bit_be(block_off, f2_nat_bitmap()))
        block_addr += f2_bps;

    f2_read_block(block_addr, f2_blk);
    return rd32(f2_blk + entry_off * NAT_ENTRY_SIZE + 5);  /* version(1) ino(4) 后 */
}

/* 读 nid 节点到 buf（GRUB grub_f2fs_read_node） */
static int f2_read_node(uint32_t nid, uint8_t *buf) {
    uint32_t blkaddr = f2_nat_lookup(nid);
    if (blkaddr == 0 || blkaddr < f2_main_blkaddr) return -1;
    f2_read_block(blkaddr, buf);
    return 0;
}

/* ---------- 节点路径映射（GRUB grub_get_node_path） ---------- */
static int f2_node_path(const uint8_t *inode, uint32_t block,
                        uint32_t off[4]) {
    uint32_t direct_blks = ADDRS_PER_BLOCK;
    uint32_t dptrs_per_blk = NIDS_PER_BLOCK;
    uint32_t indirect_blks = ADDRS_PER_BLOCK * NIDS_PER_BLOCK;
    uint32_t direct_index = DEF_ADDRS_PER_INODE;
    int n = 0;

    if (inode[INO_OFF_INLINE] & F2FS_INLINE_XATTR)
        direct_index -= F2FS_INLINE_XATTR_ADDRS;

    if (block < direct_index) {
        off[n] = block;
        return 0;
    }
    block -= direct_index;
    if (block < direct_blks) {
        off[n++] = NODE_DIR1_BLOCK;
        off[n] = block;
        return 1;
    }
    block -= direct_blks;
    if (block < direct_blks) {
        off[n++] = NODE_DIR2_BLOCK;
        off[n] = block;
        return 1;
    }
    block -= direct_blks;
    if (block < indirect_blks) {
        off[n++] = NODE_IND1_BLOCK;
        off[n++] = block / direct_blks;
        off[n] = block % direct_blks;
        return 2;
    }
    block -= indirect_blks;
    if (block < indirect_blks) {
        off[n++] = NODE_IND2_BLOCK;
        off[n++] = block / direct_blks;
        off[n] = block % direct_blks;
        return 2;
    }
    block -= indirect_blks;
    if (block < indirect_blks * NIDS_PER_BLOCK) {
        off[n++] = NODE_DIND_BLOCK;
        off[n++] = block / indirect_blks;
        off[n++] = (block / direct_blks) % dptrs_per_blk;
        off[n] = block % direct_blks;
        return 3;
    }
    return -1;
}

/* 取 inode 数据块 block_ofs 的物理块地址（GRUB grub_f2fs_get_block） */
static uint32_t f2_get_block(const uint8_t *inode, uint32_t block_ofs) {
    uint32_t off[4];
    int level = f2_node_path(inode, block_ofs, off);
    if (level < 0) return 0;

    if (level == 0)
        return rd32(inode + INO_OFF_ADDR + off[0] * 4);

    /* 从 i_nid 取子节点 id 后逐级下降 */
    uint32_t nid = rd32(inode + INO_OFF_NID + (off[0] - NODE_DIR1_BLOCK) * 4);
    for (int i = 1; i <= level; i++) {
        if (f2_read_node(nid, f2_blk) != 0) return 0;
        if (i < level)
            nid = rd32(f2_blk + off[i] * 4);   /* indirect: nid[] */
    }
    return rd32(f2_blk + off[level] * 4);      /* direct: addr[] */
}

/* ---------- 目录桶定位（fsck dir_buckets/bucket_blocks/dir_block_index） ---------- */
static uint32_t f2_dir_buckets(unsigned int level, uint8_t dir_level) {
    unsigned int t = level + (dir_level ? dir_level - 1 : 0);
    return 1u << ((t > 2) ? (t - 2) : 0);   /* level0/1: 1, level2: 2, ... */
}
static unsigned int f2_bucket_blocks(unsigned int level) {
    if (level < 2) return 2;                /* level 0/1: 2 块 */
    return 4;
}
static uint32_t f2_dir_block_index(unsigned int level, uint8_t dir_level,
                                   uint32_t nbucket) {
    uint32_t bidx = 0;
    for (unsigned int i = 0; i < level; i++)
        bidx += f2_dir_buckets(i, dir_level) * f2_bucket_blocks(i);
    return bidx + nbucket * f2_bucket_blocks(level);
}

/* ---------- 目录遍历 ---------- */
typedef int (*f2_dir_cb)(const char *name, uint32_t ino, int is_dir, void *ctx);

static char f2_lower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
}

static int f2_name_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (f2_lower(*a) != f2_lower(*b)) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

/* 遍历一段 dentry 区（inline 或 block；GRUB grub_f2fs_check_dentries） */
static int f2_check_dentries(const uint8_t *bitmap, const uint8_t *dentry,
                             const uint8_t *filename, int max,
                             f2_dir_cb cb, void *ctx) {
    for (int i = 0; i < max;) {
        if (!(bitmap[i >> 3] & (1u << (i & 7)))) {    /* 小端位序 */
            i++;
            continue;
        }
        const uint8_t *de = dentry + (uint32_t)i * SIZE_OF_DIR_ENTRY;
        uint32_t ino = rd32(de + 4);
        uint16_t name_len = rd16(de + 8);
        uint8_t ftype = de[10];
        if (name_len == 0 || name_len > F2FS_MAX_NAME) {
            i++;
            continue;
        }
        char name[256];
        uint32_t slots = (name_len + F2FS_SLOT_LEN - 1) / F2FS_SLOT_LEN;
        for (uint32_t k = 0; k < name_len; k++)
            name[k] = (char)filename[(uint32_t)i * F2FS_SLOT_LEN + k];
        name[name_len] = 0;
        int is_dir = (ftype == F2FS_FT_DIR);
        if (cb(name, ino, is_dir, ctx)) return 1;
        i += slots;
    }
    return 0;
}

/* 遍历目录节点（节点已在 node 缓冲） */
static int f2_dir_walk(const uint8_t *node, f2_dir_cb cb, void *ctx) {
    if (node[INO_OFF_INLINE] & F2FS_INLINE_DENTRY) {
        /* inline dentry 起始 = i_addr[1]（GRUB get_inline_addr） */
        const uint8_t *base = node + INO_OFF_ADDR + 4;
        return f2_check_dentries(base,
                                base + INLINE_DENTRY_BITMAP_SIZE +
                                INLINE_RESERVED_SIZE,
                                base + INLINE_DENTRY_BITMAP_SIZE +
                                INLINE_RESERVED_SIZE +
                                NR_INLINE_DENTRY * SIZE_OF_DIR_ENTRY,
                                NR_INLINE_DENTRY, cb, ctx);
    }

    /* 常规目录：按哈希桶多级布局逐块遍历 */
    uint64_t size = rd64(node + INO_OFF_SIZE);
    uint32_t depth = rd32(node + INO_OFF_CURRENT_DEPTH);
    uint8_t dir_level = node[INO_OFF_DIR_LEVEL];
    uint64_t max_blocks = size / F2FS_BLKSIZE;
    if (max_blocks == 0) max_blocks = 1;

    for (unsigned int level = 0; level <= depth && level < 64; level++) {
        uint32_t nbucket = f2_dir_buckets(level, dir_level);
        uint32_t bidx;
        int done_level = 1;
        for (uint32_t b = 0; b < nbucket; b++) {
            for (unsigned int k = 0; k < f2_bucket_blocks(level); k++) {
                bidx = f2_dir_block_index(level, dir_level, b) + k;
                if ((uint64_t)bidx >= max_blocks) { done_level = 0; break; }
                uint32_t pb = f2_get_block(node, bidx);
                if (pb == 0) continue;
                f2_read_block(pb, f2_dblk);
                if (f2_check_dentries(f2_dblk,
                                      f2_dblk + SIZE_OF_DENTRY_BITMAP +
                                      SIZE_OF_RESERVED,
                                      f2_dblk + SIZE_OF_DENTRY_BITMAP +
                                      SIZE_OF_RESERVED +
                                      NR_DENTRY_IN_BLOCK * SIZE_OF_DIR_ENTRY,
                                      NR_DENTRY_IN_BLOCK, cb, ctx))
                    return 1;
            }
            if (!done_level) break;
        }
        if (!done_level) break;    /* 桶超出已分配范围：更深层也必为空 */
    }
    return 0;
}

/* ---------- 路径解析 ---------- */
typedef struct {
    const char *name;
    uint32_t ino;
    int      is_dir;
    int      found;
} f2_lookup_ctx;

static int f2_lookup_cb(const char *name, uint32_t ino, int is_dir, void *ctx) {
    f2_lookup_ctx *c = (f2_lookup_ctx *)ctx;
    if (f2_name_eq(name, c->name)) {
        c->ino = ino;
        c->is_dir = is_dir;
        c->found = 1;
        return 1;
    }
    return 0;
}

/* 解析绝对路径 -> 节点读入 f2_node。
 * 成功返回 nid；失败返回 0。*is_dir 与 *size_out 可空。 */
static uint32_t f2_resolve(const char *path, int *is_dir_out, uint64_t *size_out) {
    if (!f2_mounted || path == 0 || path[0] != '/') return 0;
    if (f2_read_node(f2_root_ino, f2_node) != 0) return 0;
    uint32_t cur = f2_root_ino;

    while (*path == '/') path++;
    while (*path) {
        const char *comp = path;
        uint32_t clen = 0;
        while (path[clen] && path[clen] != '/') clen++;
        if (clen == 0 || clen > 255) return 0;
        char compbuf[256];
        for (uint32_t i = 0; i < clen; i++) compbuf[i] = comp[i];
        compbuf[clen] = 0;

        f2_lookup_ctx ctx;
        ctx.name = compbuf;
        ctx.found = 0;
        if (f2_dir_walk(f2_node, f2_lookup_cb, &ctx) == 1 && ctx.found) {
            cur = ctx.ino;
            if (f2_read_node(cur, f2_node) != 0) return 0;
        } else {
            return 0;
        }
        path += clen;
        while (*path == '/') path++;
    }

    if (is_dir_out)
        *is_dir_out = (rd16(f2_node + INO_OFF_MODE) & 0xF000u) == 0x4000u;
    if (size_out)
        *size_out = rd64(f2_node + INO_OFF_SIZE);
    return cur;
}

/* ---------- 对外 API ---------- */
int f2fs_is_dir(const char *path) {
    int is_dir;
    if (f2_resolve(path, &is_dir, 0) == 0) return -1;
    return is_dir;
}

uint32_t f2fs_get_file_size(const char *path) {
    uint64_t size;
    if (f2_resolve(path, 0, &size) == 0) return 0;
    return (size > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)size;
}

int f2fs_read_file(const char *path, uint8_t *buffer, uint32_t max_size) {
    int is_dir;
    uint64_t size;
    if (f2_resolve(path, &is_dir, &size) == 0 || is_dir) return -1;

    if (f2_node[INO_OFF_INLINE] & F2FS_INLINE_DATA) {
        if (size > MAX_INLINE_DATA) return -1;
        if (size > max_size) size = max_size;
        const uint8_t *src = f2_node + INO_OFF_ADDR + 4;
        for (uint32_t i = 0; i < size; i++) buffer[i] = src[i];
        return (int)size;
    }

    if (size > max_size) size = max_size;
    uint32_t done = 0;
    while (done < size) {
        uint32_t pb = f2_get_block(f2_node, done / F2FS_BLKSIZE);
        uint32_t chunk = F2FS_BLKSIZE;
        if (chunk > size - done) chunk = (uint32_t)(size - done);
        if (pb == 0) {
            for (uint32_t i = 0; i < chunk; i++) buffer[done + i] = 0;
        } else {
            /* 经 f2_dblk 中转：尾部截断块不能整块直读进 buffer（防越界） */
            f2_read_block(pb, f2_dblk);
            for (uint32_t i = 0; i < chunk; i++) buffer[done + i] = f2_dblk[i];
        }
        done += F2FS_BLKSIZE;
    }
    return (int)size;
}

typedef struct {
    fs_dir_entry_t *entries;
    int max, n;
} f2_fill_ctx;

static int f2_fill_cb(const char *name, uint32_t ino, int is_dir, void *ctx) {
    f2_fill_ctx *c = (f2_fill_ctx *)ctx;
    if (c->n >= c->max) return 1;
    fs_dir_entry_t *e = &c->entries[c->n++];
    uint32_t i = 0;
    while (name[i] && i < 255) { e->name[i] = name[i]; i++; }
    e->name[i] = 0;
    e->is_dir = (uint8_t)(is_dir ? 1 : 0);
    e->size = 0;
    /* 读子节点取大小（f2_child 与 f2_node/f2_dblk 隔离） */
    if (!is_dir && f2_read_node(ino, f2_child) == 0)
        e->size = (uint32_t)rd64(f2_child + INO_OFF_SIZE);
    return 0;
}

int f2fs_read_dir(const char *path, fs_dir_entry_t *entries, int max_entries) {
    int is_dir;
    if (f2_resolve(path, &is_dir, 0) == 0 || !is_dir) return -1;
    f2_fill_ctx ctx;
    ctx.entries = entries;
    ctx.max = max_entries;
    ctx.n = 0;
    if (f2_dir_walk(f2_node, f2_fill_cb, &ctx)) return -1;
    return ctx.n;
}

uint32_t f2fs_get_file_clusters(const char *path) {
    int is_dir;
    uint64_t size;
    if (f2_resolve(path, &is_dir, &size) == 0) return 0;
    uint64_t blocks = rd64(f2_node + INO_OFF_BLOCKS);
    if (blocks >= 8) return (uint32_t)(blocks / 8);   /* i_blocks 按 512B 扇区计 */
    return (uint32_t)((size + F2FS_BLKSIZE - 1) / F2FS_BLKSIZE);
}

/* ---------- 挂载 ---------- */
int f2fs_mount(uint8_t drive, uint32_t part_start) {
    uint8_t sb[F2FS_BLKSIZE];
    f2_mounted = 0;

    /* superblock @1024（GRUB F2FS_SUPER_OFFSET） */
    f2_drive = drive;
    f2_part_lba = part_start;
    f2_read_secs(part_start + 2, sb, 2);
    if (rd32(sb + SB_OFF_MAGIC) != F2FS_SUPER_MAGIC) return -1;
    if (rd32(sb + SB_OFF_LOG_BLK) != F2FS_BLK_BITS) return -1;
    uint32_t log_sec = rd32(sb + SB_OFF_LOG_SEC);
    uint32_t log_spb = rd32(sb + SB_OFF_LOG_SPB);
    if (log_sec < F2FS_MIN_LOG_SEC || log_sec > F2FS_BLK_BITS) return -1;
    if (log_sec + log_spb != F2FS_BLK_BITS) return -1;

    uint32_t log_bps = rd32(sb + SB_OFF_LOG_BPS);
    if (log_bps < 1 || log_bps > 10) return -1;
    f2_bps = 1u << log_bps;
    f2_cp_blkaddr = rd32(sb + SB_OFF_CP_BLKADDR);
    f2_sit_blkaddr = rd32(sb + SB_OFF_SIT_BLKADDR);
    f2_nat_blkaddr = rd32(sb + SB_OFF_NAT_BLKADDR);
    f2_ssa_blkaddr = rd32(sb + SB_OFF_SSA_BLKADDR);
    f2_main_blkaddr = rd32(sb + SB_OFF_MAIN_BLKADDR);
    f2_root_ino = rd32(sb + SB_OFF_ROOT_INO);
    f2_total_blocks = (uint32_t)rd64(sb + SB_OFF_BLOCK_COUNT);
    f2_main_segs = rd32(sb + SB_OFF_SEG_MAIN);
    uint32_t cp_payload = rd32(sb + SB_OFF_CP_PAYLOAD);
    if (f2_root_ino < 3 || f2_total_blocks < 16 ||
        f2_main_blkaddr >= f2_total_blocks ||
        f2_main_segs == 0) return -1;

    /* 双 CP pack 校验取高版本（GRUB grub_f2fs_read_cp） */
    uint64_t v1 = 0, v2 = 0;
    int ok1 = f2_validate_cp(f2_cp_blkaddr, &v1) == 0;
    int ok2 = f2_validate_cp(f2_cp_blkaddr + f2_bps, &v2) == 0;
    uint32_t cp_addr;
    if (ok1 && ok2) cp_addr = (v2 > v1) ? f2_cp_blkaddr + f2_bps : f2_cp_blkaddr;
    else if (ok1) cp_addr = f2_cp_blkaddr;
    else if (ok2) cp_addr = f2_cp_blkaddr + f2_bps;
    else return -1;

    /* start_cp_addr：版本号为偶数时 CP pack 在下一段（GRUB start_cp_addr） */
    f2_read_block(cp_addr, f2_cp);
    uint64_t ver = rd64(f2_cp + CP_OFF_VER);
    f2_start_cp = (ver & 1) ? f2_cp_blkaddr : f2_cp_blkaddr + f2_bps;
    /* 重读选中 pack 的头块（cp_addr 即头块） */
    f2_read_block(f2_start_cp, f2_cp);
    f2_cp_ver = (uint32_t)ver;

    /* nat bitmap：cp_payload>0 时在 bitmap 数组头部，否则跳过 SIT 位图 */
    uint32_t sit_bytes = rd32(f2_cp + CP_OFF_SIT_VER_BYTES);
    if (cp_payload == 0 && sit_bytes > F2FS_BLKSIZE - CP_OFF_BITMAP - 1)
        return -1;
    f2_sit_bytes = (cp_payload > 0) ? 0 : sit_bytes;
    f2_cp_payload_flag = (cp_payload > 0);

    if (f2_load_nat_journal() != 0) return -1;

    /* 根节点可读且为目录 */
    if (f2_read_node(f2_root_ino, f2_node) != 0) return -1;
    if ((rd16(f2_node + INO_OFF_MODE) & 0xF000u) != 0x4000u) return -1;

    f2_mounted = 1;

    f2_info.part_start = part_start;
    f2_info.bytes_per_sector = 512;
    f2_info.sectors_per_cluster = F2FS_BLK_SECS;
    f2_info.cluster_count = f2_total_blocks - f2_main_blkaddr;
    f2_info.volume_sectors = f2_total_blocks * F2FS_BLK_SECS;
    f2_info.used_clusters = (uint32_t)rd64(f2_cp + CP_OFF_VALID_BLOCKS);
    return 0;
}

const f2fs_info_t *f2fs_get_info(void) { return &f2_info; }

/* ============================================================
 * 写入支持（标准卷，与 mkfs.f2fs / Linux 语义一致）
 * refs: f2fs-tools mkfs/f2fs_format.c - 布局与初始化
 *       f2fs-tools fsck/fsck.c - SIT/SSA/NAT 交叉验证
 *       Linux fs/f2fs/{namei.c,node.c,dir.c,segment.c}
 *
 * 设计：
 *   - 分配走 curseg：cur_node_segno/cur_data_segno + blkoff 游标
 *   - SIT 区块维护 valid_map/vblocks（大端位序，fsck 语义）
 *   - SSA summary 块同步 nid/ofs_in_node（fsck 校验要求）
 *   - NAT 区块多块支持（block_off = nid/455），双副本同步写
 *   - CP 版本号每次提交 +2，pack 交替（奇数 pack1 / 偶数 pack2）
 *   - 文件名哈希：官方 TEA 算法（libf2fs __f2fs_dentry_hash）
 *   - 目录：inline dentry 起步，满后转常规哈希桶 dentry block
 *   - 文件：inline data <= 3488 字节，否则直接块 + direct node
 *   - 删除：SIT valid_map 回收 + NAT 清项 + CP valid 计数
 *   - mkfs 兼容：root 用常规 dentry block（'.'/'..'）
 * ============================================================ */

static void f2_write_secs(uint32_t lba, const uint8_t *buf, uint32_t nsecs) {
    for (uint32_t i = 0; i < nsecs; i++)
        ata_write_sector(f2_drive, lba + i, buf + i * 512);
}

/* 写 4KB 块 blkaddr（卷内块号） */
static void f2_write_block(uint32_t blkaddr, const uint8_t *buf) {
    f2_write_secs(f2_part_lba + blkaddr * F2FS_BLK_SECS, buf, F2FS_BLK_SECS);
}

static void f2_wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static void f2_wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void f2_wr64(uint8_t *p, uint64_t v) {
    f2_wr32(p, (uint32_t)v);
    f2_wr32(p + 4, (uint32_t)(v >> 32));
}

/* ---------- 标准 TEA 文件名哈希（libf2fs __f2fs_dentry_hash） ---------- */
#define F2FS_TEA_DELTA          0x9E3779B9u

static void f2_str2hashbuf(const uint8_t *msg, uint32_t len,
                           uint32_t buf[4]) {
    uint32_t pad, val;
    int num = 4;
    int i;

    pad = len | (len << 8);
    pad |= pad << 16;
    val = pad;
    if (len > 16)
        len = 16;
    for (i = 0; i < (int)len; i++) {
        if ((i % 4) == 0)
            val = pad;
        val = msg[i] + (val << 8);
        if (i % 4 == 3) {
            *buf++ = val;
            val = pad;
            num--;
        }
    }
    if (--num >= 0)
        *buf++ = val;
    while (--num >= 0)
        *buf++ = pad;
}

static void f2_tea_transform(uint32_t buf[4], const uint32_t in[4]) {
    uint32_t sum = 0;
    uint32_t b0 = buf[0], b1 = buf[1];
    uint32_t a = in[0], b = in[1], c = in[2], d = in[3];
    int n = 16;

    do {
        sum += F2FS_TEA_DELTA;
        b0 += ((b1 << 4) + a) ^ (b1 + sum) ^ ((b1 >> 5) + b);
        b1 += ((b0 << 4) + c) ^ (b0 + sum) ^ ((b0 >> 5) + d);
    } while (--n);

    buf[0] += b0;
    buf[1] += b1;
}

static uint32_t f2_name_hash(const char *name, uint32_t len) {
    /* '.' 与 '..' 固定 0（libf2fs 特例） */
    if (len <= 2 && name[0] == '.' &&
        (len == 1 || name[1] == '.'))
        return 0;

    uint32_t buf[4], in[4];
    buf[0] = 0x67452301; buf[1] = 0xefcdab89;
    buf[2] = 0x98badcfe; buf[3] = 0x10325476;

    const uint8_t *p = (const uint8_t *)name;
    uint32_t remain = len;
    while (1) {
        f2_str2hashbuf(p, remain, in);
        f2_tea_transform(buf, in);
        p += 16;
        if (remain <= 16)
            break;
        remain -= 16;
    }
    return buf[0] & ~0x80000000u;   /* 去 F2FS_HASH_COL_BIT */
}

/* ---------- 段/块几何 ---------- */
static uint32_t f2_segno_of(uint32_t blkaddr) {
    return (blkaddr - f2_main_blkaddr) / f2_bps;
}
static uint32_t f2_off_in_seg(uint32_t blkaddr) {
    return (blkaddr - f2_main_blkaddr) % f2_bps;
}

/* NAT journal 项数上限（507-2)/13 = 38（libf2fs NAT_JOURNAL_ENTRIES） */
#define NAT_JOURNAL_MAX    ((SUM_JOURNAL_SIZE - 2) / JENTRY_SIZE)

/* journal 中 nid 已存在则原地更新，否则追加（读路径两来源一致） */
static void f2_journal_add(uint32_t nid, uint32_t blkaddr) {
    uint16_t n = rd16(f2_natj);
    if (n > NAT_JOURNAL_MAX) n = NAT_JOURNAL_MAX;
    for (uint16_t i = 0; i < n; i++) {
        uint8_t *e = f2_natj + 2 + (uint32_t)i * JENTRY_SIZE;
        if (rd32(e) == nid) {
            f2_wr32(e + 9, blkaddr);
            return;
        }
    }
    if (n < NAT_JOURNAL_MAX) {
        uint8_t *e = f2_natj + 2 + (uint32_t)n * JENTRY_SIZE;
        f2_wr32(e, nid);
        e[4] = 0;                 /* version */
        f2_wr32(e + 5, nid);      /* ino */
        f2_wr32(e + 9, blkaddr);  /* block_addr */
        f2_wr16(f2_natj, n + 1);
    }
}

/* NAT 项写回：区块双副本同步 + journal 更新（读路径任一来源均得新值） */
static int f2_nat_set(uint32_t nid, uint32_t blkaddr) {
    uint32_t block_off = nid / NAT_ENTRY_PER_BLOCK;
    uint32_t entry_off = nid % NAT_ENTRY_PER_BLOCK;
    uint32_t seg_off = block_off / f2_bps;
    uint32_t block_addr = f2_nat_blkaddr +
                          ((seg_off * f2_bps) << 1) +
                          (block_off & (f2_bps - 1));
    /* 未被位图选中的副本（两块都写保证 pack 切换后一致） */
    uint32_t alt_addr = (f2_test_bit_be(block_off, f2_nat_bitmap()))
                        ? block_addr : block_addr + f2_bps;

    f2_read_block(block_addr, f2_meta);
    uint8_t *e = f2_meta + entry_off * NAT_ENTRY_SIZE;
    e[0] = 0;
    f2_wr32(e + 1, blkaddr ? nid : 0);
    f2_wr32(e + 5, blkaddr);
    f2_write_block(block_addr, f2_meta);

    f2_read_block(alt_addr, f2_meta);
    e = f2_meta + entry_off * NAT_ENTRY_SIZE;
    e[0] = 0;
    f2_wr32(e + 1, blkaddr ? nid : 0);
    f2_wr32(e + 5, blkaddr);
    f2_write_block(alt_addr, f2_meta);

    f2_journal_add(nid, blkaddr);
    return 0;
}

/* 分配一个空闲 nid：NAT 区块顺序扫描（journal 优先已含于 f2_nat_lookup） */
static uint32_t f2_alloc_nid(void) {
    uint32_t nat_blocks_cap = (f2_total_blocks - f2_nat_blkaddr) / 2;
    if (nat_blocks_cap > f2_bps) nat_blocks_cap = f2_bps;  /* 单段 NAT 上限 */
    for (uint32_t nid = 4; ; nid++) {    /* 0..2 保留，3 为 root */
        if (nid / NAT_ENTRY_PER_BLOCK >= nat_blocks_cap) return 0;
        if (f2_nat_lookup(nid) == 0)
            return nid;
    }
}

/* ---------- SIT 区块操作（valid_map 大端位序，fsck 语义） ----------
 * 同时同步 SIT journal 对应 curseg 槽位（Linux build_sit_entries 语义：
 * 挂载时 journal 条目覆盖 SIT 区，stale journal 会破坏正确状态）。
 * journal 条目按 curseg 类型固定槽位 idx：0..5 = HOT/WARM/COLD_DATA, NODE */
static int f2_sit_update(uint32_t segno, uint32_t off_in_seg, int set) {
    if (segno >= f2_main_segs) return -1;
    uint32_t blk = f2_sit_blkaddr + segno / SIT_ENTRY_PER_BLOCK;
    uint32_t ent = segno % SIT_ENTRY_PER_BLOCK;

    f2_read_block(blk, f2_meta);
    uint8_t *se = f2_meta + ent * SIT_ENTRY_SIZE;
    uint32_t vblocks = rd16(se) & SIT_VBLOCKS_MASK;
    uint32_t type = rd16(se) & ~SIT_VBLOCKS_MASK;

    if (set) {
        f2_set_bit_be(off_in_seg, se + 2);
        vblocks++;
    } else {
        f2_clear_bit_be(off_in_seg, se + 2);
        if (vblocks) vblocks--;
    }
    f2_wr16(se, (uint16_t)(type | vblocks));
    f2_wr64(se + 2 + SIT_VBLOCK_MAP_SIZE, f2_cp_ver);
    f2_write_block(blk, f2_meta);

    /* SIT journal 同步：该段为某 curseg 当前段时更新对应槽位
     * （type 位即 curseg 类型；本驱动只走 HOT_DATA/HOT_NODE 分配） */
    uint32_t cur_type = type >> SIT_VBLOCKS_SHIFT;
    if (cur_type < NR_CURSEG_TYPE) {
        uint32_t cur_seg;
        if (cur_type < NR_CURSEG_DATA_TYPE)
            cur_seg = rd32(f2_cp + CP_OFF_CUR_DATA_SEG + cur_type * 4);
        else
            cur_seg = rd32(f2_cp + CP_OFF_CUR_NODE_SEG +
                           (cur_type - NR_CURSEG_DATA_TYPE) * 4);
        if (cur_seg == segno) {
            uint8_t *sj = f2_sitj + 2 + cur_type * SIT_JENTRY_SIZE;
            f2_wr32(sj, segno);
            f2_wr16(sj + 4, (uint16_t)(type | vblocks));
            /* valid_map 拷贝 + mtime */
            for (uint32_t i = 0; i < SIT_VBLOCK_MAP_SIZE; i++)
                sj[6 + i] = se[2 + i];
            f2_wr64(sj + 4 + 2 + SIT_VBLOCK_MAP_SIZE, f2_cp_ver);
        }
    }
    return 0;
}

/* ---------- SSA summary 维护（fsck 校验 nid 一致性） ---------- */
/* summary 块：entries[512] + journal(1014) + footer(entry_type@4091, check_sum@4092) */
static int f2_ssa_update(uint32_t segno, uint32_t off_in_seg, uint32_t nid,
                         uint32_t ofs_in_node, int is_node) {
    uint32_t blk = f2_ssa_blkaddr + segno;   /* 非 packed SSA：一段一块 */
    f2_read_block(blk, f2_meta);
    if (f2_meta[4091] != (uint8_t)(is_node ? 1 : 0)) {
        /* footer 类型不符（段首次使用）：清 journal 区初始化 */
        for (uint32_t i = SUM_ENTRIES_SIZE; i < 4091; i++) f2_meta[i] = 0;
        f2_meta[4091] = (uint8_t)(is_node ? 1 : 0);
    }
    uint8_t *s = f2_meta + off_in_seg * 7;
    f2_wr32(s, nid);
    s[4] = 0;                            /* version */
    f2_wr16(s + 5, (uint16_t)ofs_in_node);
    f2_write_block(blk, f2_meta);
    return 0;
}

/* ---------- curseg 分配（Linux allocate_data_block 简化） ---------- */
static uint32_t f2_curseg_get(uint32_t type) {
    uint32_t idx = (type < NR_CURSEG_DATA_TYPE)
                   ? type : type - NR_CURSEG_DATA_TYPE;
    uint32_t base = (type < NR_CURSEG_DATA_TYPE)
                    ? CP_OFF_CUR_DATA_SEG : CP_OFF_CUR_NODE_SEG;
    uint32_t boff_base = (type < NR_CURSEG_DATA_TYPE)
                         ? CP_OFF_CUR_DATA_BLKOFF : CP_OFF_CUR_NODE_BLKOFF;
    uint32_t segno = rd32(f2_cp + base + idx * 4);
    uint16_t blkoff = rd16(f2_cp + boff_base + idx * 2);

    if (segno == 0xFFFFFFFFu || blkoff >= f2_bps) {
        /* 段耗尽：顺序找下一个空段（SIT vblocks==0） */
        uint32_t next = (segno == 0xFFFFFFFFu) ? 0 : segno + 1;
        for (; next < f2_main_segs; next++) {
            uint32_t blk = f2_sit_blkaddr + next / SIT_ENTRY_PER_BLOCK;
            f2_read_block(blk, f2_meta);
            uint32_t vb = rd16(f2_meta + (next % SIT_ENTRY_PER_BLOCK) *
                               SIT_ENTRY_SIZE) & SIT_VBLOCKS_MASK;
            if (vb == 0) break;
        }
        if (next >= f2_main_segs) return 0;   /* 卷满 */
        /* 旧段 type 位清零（fsck: se->type 必须与 curseg 匹配） */
        if (segno != 0xFFFFFFFFu && segno < f2_main_segs) {
            uint32_t blk = f2_sit_blkaddr + segno / SIT_ENTRY_PER_BLOCK;
            f2_read_block(blk, f2_meta);
            uint8_t *se = f2_meta + (segno % SIT_ENTRY_PER_BLOCK) *
                          SIT_ENTRY_SIZE;
            f2_wr16(se, (uint16_t)(rd16(se) & SIT_VBLOCKS_MASK));
            f2_write_block(blk, f2_meta);
        }
        /* 新段 type 位设置 */
        {
            uint32_t blk = f2_sit_blkaddr + next / SIT_ENTRY_PER_BLOCK;
            f2_read_block(blk, f2_meta);
            uint8_t *se = f2_meta + (next % SIT_ENTRY_PER_BLOCK) *
                          SIT_ENTRY_SIZE;
            f2_wr16(se, (uint16_t)((rd16(se) & SIT_VBLOCKS_MASK) |
                                   (type << SIT_VBLOCKS_SHIFT)));
            f2_write_block(blk, f2_meta);
            /* SIT journal 同槽位更新 */
            uint8_t *sj = f2_sitj + 2 + type * SIT_JENTRY_SIZE;
            f2_wr32(sj, next);
            f2_wr16(sj + 4, (uint16_t)((type << SIT_VBLOCKS_SHIFT) |
                                       (rd16(sj + 4) & SIT_VBLOCKS_MASK)));
        }
        f2_wr32(f2_cp + base + idx * 4, next);
        f2_wr16(f2_cp + boff_base + idx * 2, 0);
        segno = next;
        blkoff = 0;
    }
    f2_wr16(f2_cp + boff_base + idx * 2, blkoff + 1);
    return f2_main_blkaddr + segno * f2_bps + blkoff;
}

/* 分配一个数据块（HOT_DATA 日）并登记 SIT/SSA */
static uint32_t f2_alloc_data_block(uint32_t nid, uint32_t ofs_in_node) {
    uint32_t blk = f2_curseg_get(CURSEG_HOT_DATA);
    if (blk == 0) return 0;
    if (f2_sit_update(f2_segno_of(blk), f2_off_in_seg(blk), 1) != 0) return 0;
    f2_ssa_update(f2_segno_of(blk), f2_off_in_seg(blk), nid, ofs_in_node, 0);
    return blk;
}

/* 分配一个节点块（HOT_NODE 日）并登记 SIT/SSA */
static uint32_t f2_alloc_node_block(uint32_t nid) {
    uint32_t blk = f2_curseg_get(CURSEG_HOT_NODE);
    if (blk == 0) return 0;
    if (f2_sit_update(f2_segno_of(blk), f2_off_in_seg(blk), 1) != 0) return 0;
    f2_ssa_update(f2_segno_of(blk), f2_off_in_seg(blk), nid, 0, 1);
    return blk;
}

/* 回收一个块（数据/节点通用）：SIT 清位 */
static void f2_free_block(uint32_t blkaddr) {
    if (blkaddr == 0 || blkaddr < f2_main_blkaddr) return;
    uint32_t segno = f2_segno_of(blkaddr);
    if (segno < f2_main_segs)
        f2_sit_update(segno, f2_off_in_seg(blkaddr), 0);
}

/* ---------- CP 提交：计数 + 版本 +2 + pack 交替 + CRC + journal 块 ---------- */
static void f2_cp_commit(int d_blocks, int d_nodes, int d_inodes) {
    f2_wr64(f2_cp + CP_OFF_VALID_BLOCKS,
            (uint64_t)((int64_t)rd64(f2_cp + CP_OFF_VALID_BLOCKS) + d_blocks));
    f2_wr32(f2_cp + CP_OFF_VALID_NODES,
            (uint32_t)((int)rd32(f2_cp + CP_OFF_VALID_NODES) + d_nodes));
    f2_wr32(f2_cp + CP_OFF_VALID_INODES,
            (uint32_t)((int)rd32(f2_cp + CP_OFF_VALID_INODES) + d_inodes));

    /* 版本 +1 翻转奇偶；写入对侧 pack（奇数 ver 落 pack1，偶数落 pack2）。
     * mount 按 start_cp_addr 规则选中该 pack：GRUB/Linux 语义 ver 奇数
     * -> start_cp=cp_blkaddr(pack1)，偶数 -> pack2。双 pack 轮换备份。 */
    uint64_t ver = rd64(f2_cp + CP_OFF_VER) + 1;
    f2_wr64(f2_cp + CP_OFF_VER, ver);
    uint32_t next_pack = (ver & 1) ? f2_cp_blkaddr : f2_cp_blkaddr + f2_bps;

    /* CRC + 头/尾块 */
    f2_wr32(f2_cp + CP_OFF_CKSUM_OFF, CHECKSUM_OFFSET);
    f2_wr32(f2_cp + CHECKSUM_OFFSET, f2_crc32(f2_cp, CHECKSUM_OFFSET));
    f2_write_block(next_pack, f2_cp);
    uint32_t total = rd32(f2_cp + CP_OFF_PACK_TOTAL);
    f2_write_block(next_pack + total - 1, f2_cp);

    /* compact summary 块（journal 落盘：nat_j + sit_j 拼接） */
    for (uint32_t i = 0; i < F2FS_BLKSIZE; i++) f2_meta[i] = 0;
    for (uint32_t i = 0; i < SUM_JOURNAL_SIZE; i++)
        f2_meta[i] = f2_natj[i];
    for (uint32_t i = 0; i < SUM_JOURNAL_SIZE; i++)
        f2_meta[SUM_JOURNAL_SIZE + i] = f2_sitj[i];
    f2_meta[4091] = 0;    /* SUM_TYPE_DATA（compact 卷 footer 恒 data 型） */
    f2_write_block(next_pack + rd32(f2_cp + CP_OFF_PACK_START_SUM), f2_meta);

    /* node summary 块（pack 布局 pack+2/3/4 = HOT/WARM/COLD_NODE 的
     * curseg summary；Linux 挂载时缓存这些块作为 curseg 起始状态，
     * stale 会导致后续分配覆盖已有数据。同步从 SSA 区读出写回） */
    for (int t = 0; t < NR_CURSEG_TYPE - NR_CURSEG_DATA_TYPE; t++) {
        uint32_t segno = rd32(f2_cp + CP_OFF_CUR_NODE_SEG + t * 4);
        if (segno == 0xFFFFFFFFu || segno >= f2_main_segs) {
            for (uint32_t i = 0; i < F2FS_BLKSIZE; i++) f2_meta[i] = 0;
            f2_meta[4091] = 1;                        /* SUM_TYPE_NODE 空 */
        } else {
            f2_read_block(f2_ssa_blkaddr + segno, f2_meta);
            if (f2_meta[4091] != 1) {                 /* footer 类型校正 */
                for (uint32_t i = SUM_ENTRIES_SIZE; i < 4091; i++)
                    f2_meta[i] = 0;
                f2_meta[4091] = 1;
            }
        }
        f2_write_block(next_pack + 2 + t, f2_meta);
    }

    f2_start_cp = next_pack;
    f2_cp_ver = (uint32_t)ver;
    f2_info.used_clusters = (uint32_t)rd64(f2_cp + CP_OFF_VALID_BLOCKS);
}

/* ---------- 目录 dentry 布局助手 ---------- */
static uint8_t *f2_inline_base(uint8_t *node) {
    return node + INO_OFF_ADDR + 4;
}
static uint8_t *f2_dentry_base(uint8_t *dblk) {
    return dblk + SIZE_OF_DENTRY_BITMAP + SIZE_OF_RESERVED;
}
static uint8_t *f2_filename_base(uint8_t *dblk) {
    return f2_dentry_base(dblk) + NR_DENTRY_IN_BLOCK * SIZE_OF_DIR_ENTRY;
}

typedef struct {
    int      slot;
    uint32_t ino;
    uint16_t name_len;
    uint8_t  ftype;
} f2_dentry_hit;

static uint32_t f2_strlen(const char *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

/* 在一段 dentry 区找 name（小端位序位图） */
static int f2_dent_find(uint8_t *bitmap, uint8_t *dentry, uint8_t *filename,
                        int max, const char *name, f2_dentry_hit *out) {
    for (int i = 0; i < max;) {
        if (!(bitmap[i >> 3] & (1u << (i & 7)))) { i++; continue; }
        uint8_t *de = dentry + (uint32_t)i * SIZE_OF_DIR_ENTRY;
        uint16_t name_len = rd16(de + 8);
        if (name_len == 0 || name_len > F2FS_MAX_NAME) { i++; continue; }
        char dn[256];
        for (uint16_t k = 0; k < name_len; k++)
            dn[k] = (char)filename[(uint32_t)i * F2FS_SLOT_LEN + k];
        dn[name_len] = 0;
        if (f2_name_eq(dn, name)) {
            if (out) {
                out->slot = i;
                out->ino = rd32(de + 4);
                out->name_len = name_len;
                out->ftype = de[10];
            }
            return 0;
        }
        i += (name_len + F2FS_SLOT_LEN - 1) / F2FS_SLOT_LEN;
    }
    return -1;
}

/* 目录中查找 name（inline 或常规块；常规路径按哈希桶定位）
 * 成功时 f2_node 为父目录节点，f2_dblk 为命中 dentry 块。 */
static int f2_dir_find(uint8_t *node, const char *name, f2_dentry_hit *out) {
    if (node[INO_OFF_INLINE] & F2FS_INLINE_DENTRY) {
        uint8_t *base = f2_inline_base(node);
        return f2_dent_find(base,
                            base + INLINE_DENTRY_BITMAP_SIZE +
                            INLINE_RESERVED_SIZE,
                            base + INLINE_DENTRY_BITMAP_SIZE +
                            INLINE_RESERVED_SIZE +
                            NR_INLINE_DENTRY * SIZE_OF_DIR_ENTRY,
                            NR_INLINE_DENTRY, name, out);
    }

    uint32_t name_len = f2_strlen(name);
    uint32_t hash = f2_name_hash(name, name_len);
    uint32_t depth = rd32(node + INO_OFF_CURRENT_DEPTH);
    uint8_t dir_level = node[INO_OFF_DIR_LEVEL];

    for (unsigned int level = 0; level <= depth && level < 64; level++) {
        uint32_t nbucket = f2_dir_buckets(level, dir_level);
        uint32_t bidx = f2_dir_block_index(level, dir_level, hash % nbucket);
        for (unsigned int k = 0; k < f2_bucket_blocks(level); k++) {
            uint32_t pb = f2_get_block(node, bidx + k);
            if (pb == 0) continue;
            f2_read_block(pb, f2_dblk);
            if (f2_dent_find(f2_dblk, f2_dentry_base(f2_dblk),
                             f2_filename_base(f2_dblk),
                             NR_DENTRY_IN_BLOCK, name, out) == 0)
                return 0;
        }
    }
    return -1;
}

/* 在 dentry 块插入；返回起始 slot，失败 -1 */
static int f2_dent_insert(uint8_t *dblk, const char *name,
                          uint32_t ino, uint8_t ftype) {
    uint32_t name_len = f2_strlen(name);
    if (name_len == 0 || name_len > F2FS_MAX_NAME) return -1;
    uint32_t slots = (name_len + F2FS_SLOT_LEN - 1) / F2FS_SLOT_LEN;

    uint8_t *bitmap = dblk;
    uint8_t *dentry = f2_dentry_base(dblk);
    uint8_t *filename = f2_filename_base(dblk);
    for (uint32_t i = 0; i + slots <= NR_DENTRY_IN_BLOCK; i++) {
        int ok = 1;
        for (uint32_t k = 0; k < slots; k++)
            if (bitmap[(i + k) >> 3] & (1u << ((i + k) & 7))) { ok = 0; break; }
        if (!ok) continue;
        for (uint32_t k = 0; k < slots; k++) {
            uint32_t b = i + k;
            bitmap[b >> 3] |= (uint8_t)(1u << (b & 7));
        }
        uint32_t hash = f2_name_hash(name, name_len);
        uint8_t *de = dentry + i * SIZE_OF_DIR_ENTRY;
        f2_wr32(de, hash);
        f2_wr32(de + 4, ino);
        f2_wr16(de + 8, (uint16_t)name_len);
        de[10] = ftype;
        for (uint32_t k = 0; k < name_len; k++)
            filename[i * F2FS_SLOT_LEN + k] = (uint8_t)name[k];
        return (int)i;
    }
    return -1;
}

/* inline 目录插入；返回起始 slot，失败 -1 */
static int f2_inline_insert(uint8_t *node, const char *name,
                            uint32_t ino, uint8_t ftype) {
    uint32_t name_len = f2_strlen(name);
    if (name_len == 0 || name_len > F2FS_MAX_NAME) return -1;
    uint32_t slots = (name_len + F2FS_SLOT_LEN - 1) / F2FS_SLOT_LEN;

    uint8_t *base = f2_inline_base(node);
    uint8_t *bitmap = base;
    uint8_t *dentry = base + INLINE_DENTRY_BITMAP_SIZE + INLINE_RESERVED_SIZE;
    uint8_t *filename = dentry + NR_INLINE_DENTRY * SIZE_OF_DIR_ENTRY;

    for (uint32_t i = 0; i + slots <= NR_INLINE_DENTRY; i++) {
        int ok = 1;
        for (uint32_t k = 0; k < slots; k++)
            if (bitmap[(i + k) >> 3] & (1u << ((i + k) & 7))) { ok = 0; break; }
        if (!ok) continue;
        for (uint32_t k = 0; k < slots; k++) {
            uint32_t b = i + k;
            bitmap[b >> 3] |= (uint8_t)(1u << (b & 7));
        }
        uint32_t hash = f2_name_hash(name, name_len);
        uint8_t *de = dentry + i * SIZE_OF_DIR_ENTRY;
        f2_wr32(de, hash);
        f2_wr32(de + 4, ino);
        f2_wr16(de + 8, (uint16_t)name_len);
        de[10] = ftype;
        for (uint32_t k = 0; k < name_len; k++)
            filename[i * F2FS_SLOT_LEN + k] = (uint8_t)name[k];
        return (int)i;
    }
    return -1;
}

/* ---------- 节点 footer 初始化（mkfs set_node_footer 语义） ----------
 * cp_ver = 当前 CP 版本（Linux roll-forward 恢复判定依据；mkfs 时 CP
 * 版本即 CP 头里的 checkpoint_ver）。flag: DENT 位(bit2) 需按节点类型设置
 * —— fsck sanity_check_nid 校验 IS_DNODE 时用 footer.flag 判 dnode。 */
static void f2_set_footer(uint8_t *node, uint32_t nid, uint32_t ino,
                          uint32_t next_blkaddr) {
    uint8_t *ft = node + F2FS_BLKSIZE - 24;
    f2_wr32(ft + FOOT_OFF_NID, nid);
    f2_wr32(ft + FOOT_OFF_INO, ino);
    f2_wr32(ft + FOOT_OFF_FLAG, 0);
    f2_wr64(ft + FOOT_OFF_CPVER, f2_cp_ver ? f2_cp_ver : 1);
    f2_wr32(ft + FOOT_OFF_NEXTADDR, next_blkaddr);
}

/* direct node footer：flag 带 DENT 位（fsck IS_DNODE 语义，Linux
 * OFS_OF_NODE(dn)=0 -> dnode flag bit2=4） */
static void f2_set_footer_dnode(uint8_t *node, uint32_t nid, uint32_t ino,
                                uint32_t next_blkaddr) {
    f2_set_footer(node, nid, ino, next_blkaddr);
    uint8_t *ft = node + F2FS_BLKSIZE - 24;
    f2_wr32(ft + FOOT_OFF_FLAG, 4);            /* DENT_BIT_SHIFT=2 */
}

static uint32_t f2_dir_get_or_alloc_block(uint8_t *node, uint32_t dir_nid,
                                          uint32_t bidx, int *dnode_alloced);
static int f2_dir_add_dentry(uint8_t *node, uint32_t dir_nid,
                             const char *name, uint32_t ino, uint8_t ftype);

/* ---------- 目录块定位（写入路径：哈希 -> 桶 -> 块链） ---------- */
/* 取目录（node）第 bidx 个数据块地址；不存在则分配（含 direct node 挂接）。
 * node 同步更新（i_nid/i_addr）；调用者负责父节点落盘。
 * 返回数据块地址，失败 0。*dnode_alloced 置 1 表示新挂了 direct node。 */
static uint32_t f2_dir_get_or_alloc_block(uint8_t *node, uint32_t dir_nid,
                                          uint32_t bidx, int *dnode_alloced) {
    uint32_t off[4];
    int level = f2_node_path(node, bidx, off);
    if (level < 0) return 0;
    *dnode_alloced = 0;

    if (level == 0) {
        uint32_t pb = rd32(node + INO_OFF_ADDR + off[0] * 4);
        if (pb != 0) return pb;
        pb = f2_alloc_data_block(dir_nid, off[0]);
        if (pb == 0) return 0;
        for (uint32_t i = 0; i < F2FS_BLKSIZE; i++) f2_dblk[i] = 0;
        f2_write_block(pb, f2_dblk);
        f2_wr32(node + INO_OFF_ADDR + off[0] * 4, pb);
        return pb;
    }

    if (level != 1) return 0;   /* 目录桶仅到 DIR1/DIR2 层（本驱动范围） */

    /* 一级路径：i_nid[0/1] -> direct node -> addr[off[1]] */
    uint32_t dnode_nid = rd32(node + INO_OFF_NID +
                              (off[0] - NODE_DIR1_BLOCK) * 4);
    uint32_t dnode_blk;
    if (dnode_nid == 0 || (dnode_blk = f2_nat_lookup(dnode_nid)) == 0) {
        /* 新挂 direct node（footer.ino = 目录自身 ino） */
        uint32_t new_nid = f2_alloc_nid();
        if (new_nid == 0) return 0;
        for (uint32_t i = 0; i < F2FS_BLKSIZE; i++) f2_wblk[i] = 0;
        uint32_t dir_ino = rd32(node + F2FS_BLKSIZE - 24 + FOOT_OFF_INO);
        f2_set_footer_dnode(f2_wblk, new_nid, dir_ino, 0);
        dnode_blk = f2_alloc_node_block(new_nid);
        if (dnode_blk == 0) return 0;
        f2_write_block(dnode_blk, f2_wblk);
        f2_nat_set(new_nid, dnode_blk);
        f2_wr32(node + INO_OFF_NID + (off[0] - NODE_DIR1_BLOCK) * 4, new_nid);
        *dnode_alloced = 1;
    } else {
        f2_read_block(dnode_blk, f2_wblk);
    }

    uint32_t pb = rd32(f2_wblk + off[1] * 4);
    if (pb == 0) {
        pb = f2_alloc_data_block(dir_nid, off[1]);
        if (pb == 0) return 0;
        for (uint32_t i = 0; i < F2FS_BLKSIZE; i++) f2_dblk[i] = 0;
        f2_write_block(pb, f2_dblk);
        f2_wr32(f2_wblk + off[1] * 4, pb);
        f2_write_block(dnode_blk, f2_wblk);
    }
    return pb;
}

/* inline 目录转常规哈希桶目录（Linux f2fs_do_convert_inline_dir 语义）：
 * 1) 清 inline dentry 标志（条目先快照到局部，i_addr 区随后复用为块指针）
 * 2) 每个既有条目按其哈希重新放入 level0 桶 0 的两块（查找/删除同走
 *    哈希桶定位，保证三方一致）
 * 返回 0 成功（node 已转换，调用者随后落盘） */
static int f2_dir_convert_inline(uint8_t *node, uint32_t dir_nid) {
    if (!(node[INO_OFF_INLINE] & F2FS_INLINE_DENTRY)) return 0;

    /* 条目快照（i_addr 区将被块指针覆盖，先拷出 inline 区） */
    static uint8_t isnap[MAX_INLINE_DIR_DATA] F2_HIBUF;
    uint8_t *ibase = f2_inline_base(node);
    for (uint32_t i = 0; i < MAX_INLINE_DIR_DATA; i++) isnap[i] = ibase[i];
    const uint8_t *ibitmap = isnap;
    const uint8_t *identry = isnap + INLINE_DENTRY_BITMAP_SIZE +
                             INLINE_RESERVED_SIZE;
    const uint8_t *ifilename = identry + NR_INLINE_DENTRY * SIZE_OF_DIR_ENTRY;

    /* 节点字段转换：清 inline 标志 + size/depth 重置 + 清 i_addr/i_nid */
    node[INO_OFF_INLINE] &= (uint8_t)~F2FS_INLINE_DENTRY;
    for (uint32_t i = 0; i < DEF_ADDRS_PER_INODE; i++)
        f2_wr32(node + INO_OFF_ADDR + i * 4, 0);
    for (uint32_t i = 0; i < 5; i++)
        f2_wr32(node + INO_OFF_NID + i * 4, 0);
    f2_wr64(node + INO_OFF_SIZE, 0);
    f2_wr64(node + INO_OFF_BLOCKS, F2FS_BLK_SECS);   /* 节点块本身 */
    f2_wr32(node + INO_OFF_CURRENT_DEPTH, 0);
    node[INO_OFF_DIR_LEVEL] = 0;

    /* 逐条目按名哈希插入（f2_dir_add_dentry 分配桶内块） */
    for (int i = 0; i < NR_INLINE_DENTRY;) {
        if (!(ibitmap[i >> 3] & (1u << (i & 7)))) { i++; continue; }
        const uint8_t *de = identry + (uint32_t)i * SIZE_OF_DIR_ENTRY;
        uint16_t name_len = rd16(de + 8);
        if (name_len == 0 || name_len > F2FS_MAX_NAME) { i++; continue; }
        uint32_t slots = (name_len + F2FS_SLOT_LEN - 1) / F2FS_SLOT_LEN;
        char name[256];
        for (uint32_t k = 0; k < name_len; k++)
            name[k] = (char)ifilename[(uint32_t)i * F2FS_SLOT_LEN + k];
        name[name_len] = 0;
        if (f2_dir_add_dentry(node, dir_nid, name, rd32(de + 4),
                              de[10]) != 0)
            return -1;
        i += slots;
    }
    return 0;
}

/* 常规目录插入 dentry：按哈希定位桶 -> 空槽插入；层满加深 current_depth */
static int f2_dir_add_dentry(uint8_t *node, uint32_t dir_nid,
                             const char *name, uint32_t ino, uint8_t ftype) {
    uint32_t depth = rd32(node + INO_OFF_CURRENT_DEPTH);
    uint8_t dir_level = node[INO_OFF_DIR_LEVEL];
    uint32_t name_len = f2_strlen(name);
    uint32_t hash = f2_name_hash(name, name_len);

    for (unsigned int level = 0; level <= depth && level < 64; level++) {
        uint32_t nbucket = f2_dir_buckets(level, dir_level);
        uint32_t bidx = f2_dir_block_index(level, dir_level, hash % nbucket);
        for (unsigned int k = 0; k < f2_bucket_blocks(level); k++) {
            uint32_t target = bidx + k;
            int dnode_alloced;
            uint32_t pb = f2_dir_get_or_alloc_block(node, dir_nid, target,
                                                    &dnode_alloced);
            if (pb == 0) continue;
            f2_read_block(pb, f2_dblk);
            if (f2_dent_insert(f2_dblk, name, ino, ftype) >= 0) {
                f2_write_block(pb, f2_dblk);
                /* size 扩展 + i_blocks 记账（dentry 块 + 新 direct node） */
                uint64_t sz = rd64(node + INO_OFF_SIZE);
                if ((uint64_t)(target + 1) * F2FS_BLKSIZE > sz) {
                    f2_wr64(node + INO_OFF_SIZE,
                            (uint64_t)(target + 1) * F2FS_BLKSIZE);
                    f2_wr64(node + INO_OFF_BLOCKS,
                            rd64(node + INO_OFF_BLOCKS) + F2FS_BLK_SECS);
                }
                if (dnode_alloced)
                    f2_wr64(node + INO_OFF_BLOCKS,
                            rd64(node + INO_OFF_BLOCKS) + F2FS_BLK_SECS);
                return 0;
            }
        }
    }
    /* 所有层满：加深一层递归重试（Linux f2fs 多级哈希语义） */
    if (depth + 1 < 64) {
        f2_wr32(node + INO_OFF_CURRENT_DEPTH, depth + 1);
        return f2_dir_add_dentry(node, dir_nid, name, ino, ftype);
    }
    return -1;
}

/* ---------- 路径拆分：父目录节点读入 f2_node ---------- */
static int f2_split_path(const char *path, uint32_t *parent_nid,
                         char *fname, uint32_t fname_cap) {
    const char *slash = 0;
    const char *p = path;
    while (*p) { if (*p == '/') slash = p; p++; }
    if (!slash) return -1;

    const char *n = slash + 1;
    uint32_t nlen = 0;
    while (n[nlen] && n[nlen] != '/') nlen++;
    if (nlen == 0 || nlen >= fname_cap) return -1;
    for (uint32_t i = 0; i < nlen; i++) fname[i] = n[i];
    fname[nlen] = 0;

    if (slash == path) {
        if (f2_read_node(f2_root_ino, f2_node) != 0) return -1;
        *parent_nid = f2_root_ino;
    } else {
        char ppath[256];
        uint32_t plen = (uint32_t)(slash - path);
        if (plen >= 256) return -1;
        for (uint32_t i = 0; i < plen; i++) ppath[i] = path[i];
        ppath[plen] = 0;
        int is_dir;
        uint32_t pin = f2_resolve(ppath, &is_dir, 0);
        if (pin == 0 || !is_dir) return -1;
        *parent_nid = pin;
    }
    return 0;
}

/* ---------- 文件数据块写入（直接块 + 一级 direct node） ---------- */
/* 分配数据块并写入（f2_wnode 的 i_addr/i_nid 同步更新）。
 * 返回分配总块数（数据 + direct node），失败 0。*/
static uint32_t f2_file_write_blocks(uint32_t nid, const uint8_t *data,
                                     uint32_t size) {
    uint32_t nblk = (size + F2FS_BLKSIZE - 1) / F2FS_BLKSIZE;
    uint32_t used = 0;

    /* 直接块 i_addr[0..923) */
    uint32_t direct = (nblk < DEF_ADDRS_PER_INODE) ? nblk : DEF_ADDRS_PER_INODE;
    for (uint32_t b = 0; b < direct; b++) {
        uint32_t pb = f2_alloc_data_block(nid, b);
        if (pb == 0) return 0;
        for (uint32_t i = 0; i < F2FS_BLKSIZE; i++) f2_wblk[i] = 0;
        uint32_t chunk = size - b * F2FS_BLKSIZE;
        if (chunk > F2FS_BLKSIZE) chunk = F2FS_BLKSIZE;
        for (uint32_t i = 0; i < chunk; i++)
            f2_wblk[i] = data[b * F2FS_BLKSIZE + i];
        f2_write_block(pb, f2_wblk);
        f2_wr32(f2_wnode + INO_OFF_ADDR + b * 4, pb);
        used++;
    }
    if (nblk <= DEF_ADDRS_PER_INODE)
        return used;

    /* i_nid[0] direct node：覆盖块 923..1940（上限 ~7.9MB） */
    uint32_t dnid = f2_alloc_nid();
    if (dnid == 0) return 0;
    for (uint32_t i = 0; i < F2FS_BLKSIZE; i++) f2_wblk[i] = 0;
    f2_set_footer_dnode(f2_wblk, dnid, nid, 0);
    uint32_t cap = DEF_ADDRS_PER_INODE + ADDRS_PER_BLOCK;
    for (uint32_t b = direct; b < nblk && b < cap; b++) {
        uint32_t pb = f2_alloc_data_block(nid, b);
        if (pb == 0) return 0;
        for (uint32_t i = 0; i < F2FS_BLKSIZE; i++) f2_dblk[i] = 0;
        uint32_t chunk = size - b * F2FS_BLKSIZE;
        if (chunk > F2FS_BLKSIZE) chunk = F2FS_BLKSIZE;
        for (uint32_t i = 0; i < chunk; i++)
            f2_dblk[i] = data[b * F2FS_BLKSIZE + i];
        f2_write_block(pb, f2_dblk);
        f2_wr32(f2_wblk + (b - DEF_ADDRS_PER_INODE) * 4, pb);
        used++;
    }
    uint32_t dnode_blk = f2_alloc_node_block(dnid);
    if (dnode_blk == 0) return 0;
    f2_write_block(dnode_blk, f2_wblk);
    f2_nat_set(dnid, dnode_blk);
    f2_wr32(f2_wnode + INO_OFF_NID + 0 * 4, dnid);
    used++;

    if (nblk > cap)
        return 0;   /* 超出直接 + 单 direct node 容量（>1940 块） */
    return used;
}

/* ---------- 按路径写文件（create-or-replace） ---------- */
/*
 * 新建 inode 失败时的回滚：把已经分走的块还回 SIT。
 *
 * 为什么只回收块、不回收 nid：块是稀缺资源，泄漏几轮就把卷写满；而 nid
 * 只是 NAT 表里的一项，浪费掉不会让卷不可用。用"块优先"换取回滚逻辑的
 * 简单与可靠——nid 的回收要动 NAT 与 free_nid 位图，出错代价远高于收益。
 *
 * 前提（调用方必须保证）：失败时该 inode **还没有任何 dentry 指向它**，
 * 因此 f2_wnode 里记录的每一个块都是"只被它引用"的，释放不会误伤别人。
 * create_file / mkdir / write_file 的失败点都满足这一条。
 */
static void f2_rollback_new_inode(uint32_t node_blk) {
    /* inode 里的直接块 */
    for (uint32_t b = 0; b < DEF_ADDRS_PER_INODE; b++) {
        uint32_t pb = rd32(f2_wnode + INO_OFF_ADDR + b * 4);
        if (pb) f2_free_block(pb);
    }
    /* 一级 direct node：它的数据块 + 该 node 块自身 */
    uint32_t dnid = rd32(f2_wnode + INO_OFF_NID + 0 * 4);
    if (dnid) {
        uint32_t dblkaddr = f2_nat_lookup(dnid);
        if (dblkaddr) {
            f2_read_block(dblkaddr, f2_wblk);
            for (uint32_t b = 0; b < ADDRS_PER_BLOCK; b++) {
                uint32_t pb = rd32(f2_wblk + b * 4);
                if (pb) f2_free_block(pb);
            }
            f2_free_block(dblkaddr);
        }
    }
    if (node_blk) f2_free_block(node_blk);
}

int f2fs_create_file(const char *name, const uint8_t *data, uint32_t size) {
    uint32_t parent;
    char fname[256];
    if (f2_split_path(name, &parent, fname, sizeof(fname)) != 0) return -1;

    /* create-or-replace：旧文件先删（同名目录则失败） */
    f2_dentry_hit hit;
    if (f2_dir_find(f2_node, fname, &hit) == 0) {
        if (hit.ftype == F2FS_FT_DIR) return -1;
        if (f2fs_delete_file(name) != 0) return -1;
        if (f2_read_node(parent, f2_node) != 0) return -1;
    }

    uint32_t nid = f2_alloc_nid();
    if (nid == 0) return -1;

    /* 构造子节点 */
    for (uint32_t i = 0; i < F2FS_BLKSIZE; i++) f2_wnode[i] = 0;
    f2_wr16(f2_wnode + INO_OFF_MODE, 0x81A4);        /* regular 0644 */
    f2_wr64(f2_wnode + INO_OFF_SIZE, size);
    f2_wr32(f2_wnode + INO_OFF_PINO, parent);
    f2_wr32(f2_wnode + INO_OFF_NAMELEN, f2_strlen(fname));
    for (uint32_t i = 0; fname[i]; i++)
        f2_wnode[INO_OFF_NAME + i] = (uint8_t)fname[i];

    uint32_t dblk = 1;    /* 节点块本身 */
    if (size <= MAX_INLINE_DATA) {
        f2_wnode[INO_OFF_INLINE] = F2FS_INLINE_DATA | F2FS_DATA_EXIST;
        for (uint32_t i = 0; i < size; i++)
            f2_wnode[INO_OFF_ADDR + 4 + i] = data[i];
        f2_wr64(f2_wnode + INO_OFF_BLOCKS, F2FS_BLK_SECS);
    } else {
        uint32_t used = f2_file_write_blocks(nid, data, size);
        if (used == 0) return -1;
        dblk += used;
        /* i_blocks：数据块 + 节点块（mkfs 记账：总分配块数 × 512B 扇区） */
        f2_wr64(f2_wnode + INO_OFF_BLOCKS, (uint64_t)dblk * F2FS_BLK_SECS);
    }

    /* 节点落盘：footer + NAT */
    uint32_t node_blk = f2_alloc_node_block(nid);
    if (node_blk == 0) {
        f2_rollback_new_inode(0);   /* 数据块已经分出去了，节点块还没 */
        return -1;
    }
    f2_set_footer(f2_wnode, nid, nid, node_blk + 1);
    f2_write_block(node_blk, f2_wnode);
    f2_nat_set(nid, node_blk);

    /* 父目录插入 dentry（inline 目录优先，满则转常规块；常规目录直接走哈希桶）
     * 以下每条失败都要回滚：此时没有任何 dentry 指向新 inode，
     * 不回收的话它连同它的数据块一起成为"只占 SIT、无人引用"的孤儿。 */
    uint32_t pblk = f2_nat_lookup(parent);
    if (pblk == 0 || pblk < f2_main_blkaddr) {
        f2_rollback_new_inode(node_blk);
        return -1;
    }
    if (f2_node[INO_OFF_INLINE] & F2FS_INLINE_DENTRY) {
        if (f2_inline_insert(f2_node, fname, nid, F2FS_FT_REG_FILE) < 0) {
            if (f2_dir_convert_inline(f2_node, parent) != 0) {
                f2_rollback_new_inode(node_blk);
                return -1;
            }
            if (f2_dir_add_dentry(f2_node, parent, fname, nid,
                                  F2FS_FT_REG_FILE) != 0) {
                f2_rollback_new_inode(node_blk);
                return -1;
            }
        }
    } else {
        if (f2_dir_add_dentry(f2_node, parent, fname, nid,
                              F2FS_FT_REG_FILE) != 0) {
            f2_rollback_new_inode(node_blk);
            return -1;
        }
    }
    f2_write_block(pblk, f2_node);

    f2_cp_commit((int)dblk, 1, 1);
    return 0;
}

/* ---------- 删除：按 dentry 命中回收节点/数据块 ---------- */
static int f2_remove_child(uint32_t parent_nid, const f2_dentry_hit *hit,
                           uint8_t *dir_node) {
    if (hit->ino < 3) return -1;

    int dblk = 1;    /* 节点块本身 */
    if (f2_read_node(hit->ino, f2_wnode) == 0) {
        if (!(f2_wnode[INO_OFF_INLINE] & F2FS_INLINE_DATA)) {
            /* 回收直接块 */
            for (uint32_t b = 0; b < DEF_ADDRS_PER_INODE; b++) {
                uint32_t pb = rd32(f2_wnode + INO_OFF_ADDR + b * 4);
                if (pb) { f2_free_block(pb); dblk++; }
            }
            /* 一级 direct node（i_nid[0]）及其数据块 */
            uint32_t dnid = rd32(f2_wnode + INO_OFF_NID + 0 * 4);
            if (dnid) {
                uint32_t dblkaddr = f2_nat_lookup(dnid);
                if (dblkaddr) {
                    f2_read_block(dblkaddr, f2_wblk);
                    for (uint32_t b = 0; b < ADDRS_PER_BLOCK; b++) {
                        uint32_t pb = rd32(f2_wblk + b * 4);
                        if (pb) { f2_free_block(pb); dblk++; }
                    }
                    f2_free_block(dblkaddr);
                    f2_nat_set(dnid, 0);
                    dblk++;
                }
            }
        }
        /* 节点块本身回收 */
        f2_free_block(f2_nat_lookup(hit->ino));
    }
    f2_nat_set(hit->ino, 0);

    /* 摘 inline dentry 位图（常规块由 delete_file 先处理） */
    uint32_t pblk = f2_nat_lookup(parent_nid);
    if (pblk == 0 || pblk < f2_main_blkaddr) return -1;

    if (dir_node[INO_OFF_INLINE] & F2FS_INLINE_DENTRY) {
        uint8_t *base = f2_inline_base(dir_node);
        uint8_t *bitmap = base;
        uint8_t *dentry = base + INLINE_DENTRY_BITMAP_SIZE +
                          INLINE_RESERVED_SIZE;
        uint32_t slots = (hit->name_len + F2FS_SLOT_LEN - 1) / F2FS_SLOT_LEN;
        for (uint32_t k = 0; k < slots && hit->slot + k < NR_INLINE_DENTRY;
             k++) {
            uint32_t b = (uint32_t)hit->slot + k;
            bitmap[b >> 3] &= (uint8_t)~(1u << (b & 7));
        }
        uint8_t *de = dentry + (uint32_t)hit->slot * SIZE_OF_DIR_ENTRY;
        for (int k = 0; k < SIZE_OF_DIR_ENTRY; k++) de[k] = 0;
    }
    f2_write_block(pblk, dir_node);
    f2_cp_commit(-dblk, -1, -1);
    return 0;
}

int f2fs_delete_file(const char *name) {
    uint32_t parent;
    char fname[256];
    if (f2_split_path(name, &parent, fname, sizeof(fname)) != 0) return -1;

    f2_dentry_hit hit;
    if (f2_dir_find(f2_node, fname, &hit) != 0) return -1;
    if (hit.ftype != F2FS_FT_REG_FILE) return -1;

    /* 常规目录：先清 dentry 块位图（inline 由 remove_child 处理） */
    if (!(f2_node[INO_OFF_INLINE] & F2FS_INLINE_DENTRY)) {
        uint32_t name_len = f2_strlen(fname);
        uint32_t hash = f2_name_hash(fname, name_len);
        uint32_t depth = rd32(f2_node + INO_OFF_CURRENT_DEPTH);
        uint8_t dir_level = f2_node[INO_OFF_DIR_LEVEL];
        for (unsigned int level = 0; level <= depth && level < 64; level++) {
            uint32_t nbucket = f2_dir_buckets(level, dir_level);
            uint32_t bidx = f2_dir_block_index(level, dir_level,
                                               hash % nbucket);
            for (unsigned int k = 0; k < f2_bucket_blocks(level); k++) {
                uint32_t pb = f2_get_block(f2_node, bidx + k);
                if (pb == 0) continue;
                f2_read_block(pb, f2_dblk);
                if (f2_dent_find(f2_dblk, f2_dentry_base(f2_dblk),
                                 f2_filename_base(f2_dblk),
                                 NR_DENTRY_IN_BLOCK, fname, &hit) == 0) {
                    uint32_t slots = (hit.name_len + F2FS_SLOT_LEN - 1) /
                                     F2FS_SLOT_LEN;
                    for (uint32_t s = 0; s < slots &&
                         hit.slot + s < NR_DENTRY_IN_BLOCK; s++) {
                        uint32_t b = (uint32_t)hit.slot + s;
                        f2_dblk[b >> 3] &= (uint8_t)~(1u << (b & 7));
                    }
                    f2_write_block(pb, f2_dblk);
                    level = depth + 1;    /* 双重 break */
                    break;
                }
            }
        }
    }
    return f2_remove_child(parent, &hit, f2_node);
}

/* ============================================================
 * rmdir：摘除空目录（元数据先改、块后释放，失败对称回滚）
 *
 * 与 f2fs_delete_file 平行但处理目录。目录的数据块即多级哈希 dentry 块
 * （inline / i_addr / DIR1(i_nid[0]) / DIR2(i_nid[1])）。现有
 * f2_remove_child 只回收单 direct node 且「先释放后写盘」（反向顺序，会
 * 产生悬空引用），也不覆盖 DIR2 与多级哈希块，故此处自实现。
 * ============================================================ */

typedef struct { int empty; } f2_empty_ctx;
static int f2_empty_cb(const char *name, uint32_t ino, int is_dir, void *ctx) {
    (void)ino; (void)is_dir;
    /* EZOS 子目录不含 . / .. 项；即使有也忽略 */
    if (name[0] == '.' && (name[1] == 0 ||
        (name[1] == '.' && name[2] == 0))) return 0;
    ((f2_empty_ctx *)ctx)->empty = 0;
    return 1;   /* 发现真实条目，提前结束 */
}

/* fail closed：校验子目录数据块地址上界（写盘前，零副作用） */
static int f2_rmdir_validate_child(const uint8_t *inode) {
    if (inode[INO_OFF_INLINE] & F2FS_INLINE_DENTRY) return 0;
    for (uint32_t b = 0; b < (uint32_t)DEF_ADDRS_PER_INODE; b++) {
        uint32_t pb = rd32(inode + INO_OFF_ADDR + b * 4);
        if (pb && (pb < f2_main_blkaddr || pb >= f2_total_blocks)) return -1;
    }
    for (uint32_t k = 0; k < 2; k++) {
        uint32_t dnid = rd32(inode + INO_OFF_NID + k * 4);
        if (dnid == 0) continue;
        uint32_t dnblk = f2_nat_lookup(dnid);
        if (dnblk == 0 || dnblk < f2_main_blkaddr || dnblk >= f2_total_blocks)
            return -1;
        f2_read_block(dnblk, f2_wblk);
        for (uint32_t b = 0; b < (uint32_t)ADDRS_PER_BLOCK; b++) {
            uint32_t pb = rd32(f2_wblk + b * 4);
            if (pb && (pb < f2_main_blkaddr || pb >= f2_total_blocks)) return -1;
        }
    }
    return 0;
}

/* 回收子目录数据块 + 直接节点（i_addr + DIR1/DIR2）。
 * 调用方已校验地址上界；*dblk / *dnodes 累加已释放块数。 */
static void f2_rmdir_free_child(uint8_t *inode, int *dblk, int *dnodes) {
    if (inode[INO_OFF_INLINE] & F2FS_INLINE_DENTRY) return;
    for (uint32_t b = 0; b < (uint32_t)DEF_ADDRS_PER_INODE; b++) {
        uint32_t pb = rd32(inode + INO_OFF_ADDR + b * 4);
        if (pb >= f2_main_blkaddr && pb < f2_total_blocks) {
            f2_free_block(pb); (*dblk)++;
        }
    }
    for (uint32_t k = 0; k < 2; k++) {
        uint32_t dnid = rd32(inode + INO_OFF_NID + k * 4);
        if (dnid == 0) continue;
        uint32_t dnblk = f2_nat_lookup(dnid);
        if (dnblk >= f2_main_blkaddr && dnblk < f2_total_blocks) {
            f2_read_block(dnblk, f2_wblk);
            for (uint32_t b = 0; b < (uint32_t)ADDRS_PER_BLOCK; b++) {
                uint32_t pb = rd32(f2_wblk + b * 4);
                if (pb >= f2_main_blkaddr && pb < f2_total_blocks) {
                    f2_free_block(pb); (*dblk)++;
                }
            }
            f2_free_block(dnblk); (*dblk)++;
            f2_nat_set(dnid, 0); (*dnodes)++;
        }
    }
}

/* 常规父目录：按哈希桶定位并摘除 dentry 块位图 + 落盘 */
static int f2_rmdir_detach_regular(uint8_t *parent_node, const char *fname) {
    uint32_t name_len = f2_strlen(fname);
    uint32_t hash = f2_name_hash(fname, name_len);
    uint32_t depth = rd32(parent_node + INO_OFF_CURRENT_DEPTH);
    uint8_t dir_level = parent_node[INO_OFF_DIR_LEVEL];
    for (unsigned int level = 0; level <= depth && level < 64; level++) {
        uint32_t nbucket = f2_dir_buckets(level, dir_level);
        uint32_t bidx = f2_dir_block_index(level, dir_level, hash % nbucket);
        for (unsigned int m = 0; m < f2_bucket_blocks(level); m++) {
            uint32_t pb = f2_get_block(parent_node, bidx + m);
            if (pb == 0) continue;
            if (pb < f2_main_blkaddr || pb >= f2_total_blocks) return -1;
            f2_read_block(pb, f2_dblk);
            f2_dentry_hit h;
            if (f2_dent_find(f2_dblk, f2_dentry_base(f2_dblk),
                             f2_filename_base(f2_dblk), NR_DENTRY_IN_BLOCK,
                             fname, &h) == 0) {
                uint32_t slots = (h.name_len + F2FS_SLOT_LEN - 1) /
                                 F2FS_SLOT_LEN;
                for (uint32_t s = 0; s < slots &&
                     h.slot + s < NR_DENTRY_IN_BLOCK; s++) {
                    uint32_t bb = (uint32_t)h.slot + s;
                    f2_dblk[bb >> 3] &= (uint8_t)~(1u << (bb & 7));
                }
                f2_write_block(pb, f2_dblk);
                return 0;
            }
        }
    }
    return -1;
}

/* 失败回滚：把已被摘除的 dentry 重新插回父目录 */
static int f2_rmdir_restore(uint8_t *parent_node, uint32_t parent_nid,
                            const char *fname, uint32_t child_ino) {
    if (parent_node[INO_OFF_INLINE] & F2FS_INLINE_DENTRY) {
        if (f2_inline_insert(parent_node, fname, child_ino, F2FS_FT_DIR) < 0)
            return -1;
    } else {
        if (f2_dir_add_dentry(parent_node, parent_nid, fname, child_ino,
                              F2FS_FT_DIR) != 0)
            return -1;
    }
    uint32_t pblk = f2_nat_lookup(parent_nid);
    if (pblk == 0 || pblk < f2_main_blkaddr) return -1;
    f2_write_block(pblk, parent_node);
    return 0;
}

int f2fs_rmdir(const char *name) {
    uint32_t parent;
    char fname[256];
    if (f2_split_path(name, &parent, fname, sizeof(fname)) != 0) return -1;
    /* 拒绝 . / ..（根 "/" 已被 split_path 以 nlen==0 拒绝） */
    if (fname[0] == '.' && (fname[1] == 0 ||
        (fname[1] == '.' && fname[2] == 0))) return -1;

    f2_dentry_hit hit;
    if (f2_dir_find(f2_node, fname, &hit) != 0) return -1;   /* 不存在 */
    if (hit.ftype != F2FS_FT_DIR) return -1;                 /* 是文件 */
    if (hit.ino < 3) return -1;                              /* . / .. / 保留 */

    uint32_t child_ino = hit.ino;
    if (f2_read_node(child_ino, f2_wnode) != 0) return -1;   /* 子节点不可读 */
    if ((rd16(f2_wnode + INO_OFF_MODE) & 0xF000u) != 0x4000u) return -1;

    /* 非空检查（只读，零副作用） */
    f2_empty_ctx ec; ec.empty = 1;
    f2_dir_walk(f2_wnode, f2_empty_cb, &ec);
    if (!ec.empty) return -1;

    /* fail closed：子目录块地址上界校验（写盘前） */
    if (f2_rmdir_validate_child(f2_wnode) != 0) return -1;

    /* ---- 元数据先改：摘除父目录 dentry 并落盘 ---- */
    if (f2_node[INO_OFF_INLINE] & F2FS_INLINE_DENTRY) {
        uint32_t pblk = f2_nat_lookup(parent);
        if (pblk == 0 || pblk < f2_main_blkaddr) return -1;
        uint8_t *base = f2_inline_base(f2_node);
        uint8_t *bitmap = base;
        uint32_t slots = (hit.name_len + F2FS_SLOT_LEN - 1) / F2FS_SLOT_LEN;
        for (uint32_t k = 0; k < slots && hit.slot + k < NR_INLINE_DENTRY; k++) {
            uint32_t bb = (uint32_t)hit.slot + k;
            bitmap[bb >> 3] &= (uint8_t)~(1u << (bb & 7));
        }
        f2_write_block(pblk, f2_node);
    } else {
        if (f2_rmdir_detach_regular(f2_node, fname) != 0) return -1;
    }

    /* ---- 校验子 inode 节点块（用于失败回滚判定） ---- */
    uint32_t child_node_blk = f2_nat_lookup(child_ino);
    if (child_node_blk == 0 || child_node_blk < f2_main_blkaddr ||
        child_node_blk >= f2_total_blocks) {
        f2_rmdir_restore(f2_node, parent, fname, child_ino);
        return -1;
    }

    /* ---- 块后释放 ---- */
    int dblk = 1;     /* inode 节点块本身 */
    int dnodes = 1;   /* inode 节点 */
    f2_rmdir_free_child(f2_wnode, &dblk, &dnodes);
    f2_free_block(child_node_blk);
    f2_nat_set(child_ino, 0);

    f2_cp_commit(-dblk, -dnodes, -1);
    return 0;
}

int f2fs_mkdir(const char *name) {
    uint32_t parent;
    char fname[256];
    if (f2_split_path(name, &parent, fname, sizeof(fname)) != 0) return -1;

    f2_dentry_hit hit;
    if (f2_dir_find(f2_node, fname, &hit) == 0) return -1;

    uint32_t nid = f2_alloc_nid();
    if (nid == 0) return -1;

    /* 目录节点：空 inline dentry（root 为常规块布局，子目录 inline 起步） */
    for (uint32_t i = 0; i < F2FS_BLKSIZE; i++) f2_wnode[i] = 0;
    f2_wr16(f2_wnode + INO_OFF_MODE, 0x41ED);        /* dir 0755 */
    f2_wr64(f2_wnode + INO_OFF_SIZE, MAX_INLINE_DIR_DATA);
    f2_wr64(f2_wnode + INO_OFF_BLOCKS, F2FS_BLK_SECS);
    f2_wr32(f2_wnode + INO_OFF_PINO, parent);
    f2_wr32(f2_wnode + INO_OFF_CURRENT_DEPTH, 0);
    f2_wr32(f2_wnode + INO_OFF_NAMELEN, f2_strlen(fname));
    for (uint32_t i = 0; fname[i]; i++)
        f2_wnode[INO_OFF_NAME + i] = (uint8_t)fname[i];
    f2_wnode[INO_OFF_INLINE] = F2FS_INLINE_DENTRY;

    uint32_t node_blk = f2_alloc_node_block(nid);
    if (node_blk == 0) {
        f2_rollback_new_inode(0);   /* 目录自身那一个数据块（'.'/'..'）要还 */
        return -1;
    }
    f2_set_footer(f2_wnode, nid, nid, node_blk + 1);
    f2_write_block(node_blk, f2_wnode);
    f2_nat_set(nid, node_blk);

    /* 父目录插入 dentry（inline 目录优先，满则转常规块；常规目录直接走哈希桶） */
    uint32_t pblk = f2_nat_lookup(parent);
    if (pblk == 0 || pblk < f2_main_blkaddr) {
        f2_rollback_new_inode(node_blk);
        return -1;
    }
    if (f2_node[INO_OFF_INLINE] & F2FS_INLINE_DENTRY) {
        if (f2_inline_insert(f2_node, fname, nid, F2FS_FT_DIR) < 0) {
            if (f2_dir_convert_inline(f2_node, parent) != 0) {
                f2_rollback_new_inode(node_blk);
                return -1;
            }
            if (f2_dir_add_dentry(f2_node, parent, fname, nid, F2FS_FT_DIR) != 0) {
                f2_rollback_new_inode(node_blk);
                return -1;
            }
        }
    } else {
        if (f2_dir_add_dentry(f2_node, parent, fname, nid, F2FS_FT_DIR) != 0) {
            f2_rollback_new_inode(node_blk);
            return -1;
        }
    }
    f2_write_block(pblk, f2_node);

    f2_cp_commit(1, 1, 1);
    return 0;
}

/* ---------- 格式化（mkfs.f2fs 布局） ----------
 * 16MB 卷 / 4KB 块 / 256KB 段（64 块/段，log_blocks_per_seg=6）/
 * segs_per_sec=1 / secs_per_zone=1：
 *   LBA0   : MBR（分区 0x83 @LBA1，32767 扇区）
 *   块 0   : 卷头（SB @ 偏移 1024）
 *   段 0   : SB 区（seg0_blkaddr=63，zone 对齐公式同 mkfs）
 *   段 1-2 : CP pack1/pack2（各 6 块布局：
 *            [CP 头][compact summary: nat_j+sit_j+hot_data sums]
 *            [hot_node sums][warm_node sums][cold_node sums][CP 尾]）
 *   段 3-4 : SIT（两副本各 1 段；条目 74B × 55/块）
 *   段 5-6 : NAT（两副本各 1 段）
 *   段 7   : SSA（56 main 段 × 1 summary 块/段）
 *   段 8.. : main 区（56 段 × 64 块 = 3584 块 @ 块 511..4094）
 *            curseg 初始（main 相对）：HOT_NODE=0 WARM_NODE=1 COLD_NODE=2
 *            HOT_DATA=3 COLD_DATA=13 WARM_DATA=27（mkfs next_zone/last_zone 公式）
 *   root(3) 常规目录：dentry 块（'.'/'..'）@ HOT_DATA 段首，
 *            根 inode @ HOT_NODE 段块 1；node(1)/meta(2) NAT->块1（mkfs 语义）
 *   CP ver=随机奇数（-> pack1 为 start_cp），CRC 覆盖 [0,4092)
 */
int f2fs_format(uint8_t drive) {
    if (drive > 3) return -1;

    /* 卷几何（与 mkfs.f2fs 公式一致，bps=64） */
    const uint32_t part_start = 1;
    const uint32_t vol_blocks = 4095;      /* 32767 扇区 / 8 */
    const uint32_t bps = 64;
    const uint32_t seg0 = 63;              /* zone 对齐结果 */
    const uint32_t cp0 = seg0;             /* 63 */
    const uint32_t sit0 = seg0 + 2 * bps;  /* 191 */
    const uint32_t nat0 = seg0 + 4 * bps;  /* 319 */
    const uint32_t ssa0 = seg0 + 6 * bps;  /* 447 */
    const uint32_t main0 = seg0 + 7 * bps; /* 511 */
    const uint32_t total_segs = 63;
    const uint32_t main_segs = total_segs - 7;   /* 56 */
    const uint32_t root_nid = 3;
    const uint32_t node_ino = 1, meta_ino = 2;
    const uint32_t root_dentry_blk = main0 + 3 * bps;       /* 703 */
    const uint32_t root_inode_blk = main0 + 1;              /* 512 */

    /* CP 版本（mkfs: rand()|0x1 随机奇数；root footer 与 CP 用同值） */
    {
        uint32_t rnd = (uint32_t)(0x9E3779B9u * (uint32_t)(drive + 1) ^
                                  (vol_blocks << 11) ^ 0x5DEECE66Du);
        rnd ^= rnd >> 15; rnd *= 0x2545F491u; rnd ^= rnd >> 13;
        f2_cp_ver = rnd | 1u;
    }

    /* format 期间 f2_write_block 需要驱动器/分区状态 */
    f2_mounted = 0;
    f2_drive = drive;
    f2_part_lba = part_start;

    /* MBR：分区 0x83 @LBA1（与 ext4_format 一致） */
    static uint8_t mbr[512];
    for (int i = 0; i < 512; i++) mbr[i] = 0;
    mbr[446] = 0x00; mbr[447] = 0x02; mbr[448] = 0x00;
    mbr[449] = 0x83;
    mbr[450] = 0x00; mbr[451] = 0x3F; mbr[452] = 0xFF;
    f2_wr32(mbr + 454, part_start);
    f2_wr32(mbr + 458, vol_blocks * F2FS_BLK_SECS - 1);
    mbr[510] = 0x55; mbr[511] = 0xAA;
    if (ata_write_sector(drive, 0, mbr) != 0) return -1;

    /* SB（块 0，偏移 1024 起 = 扇区 part_start+2） */
    uint8_t *sb = f2_fmt + 1024;
    for (uint32_t i = 0; i < F2FS_BLKSIZE; i++) f2_fmt[i] = 0;
    f2_wr32(sb + SB_OFF_MAGIC, F2FS_SUPER_MAGIC);
    f2_wr32(sb + SB_OFF_LOG_SEC, 9);        /* 512B 扇区 */
    f2_wr32(sb + SB_OFF_LOG_SPB, 3);        /* 8 扇区/块 = 4KB */
    f2_wr32(sb + SB_OFF_LOG_BLK, F2FS_BLK_BITS);
    f2_wr32(sb + SB_OFF_LOG_BPS, 6);        /* 64 块/段 = 256KB */
    f2_wr32(sb + SB_OFF_SEG_SPSEC, 1);      /* segs_per_sec */
    f2_wr32(sb + SB_OFF_SECS_PZONE, 1);     /* secs_per_zone */
    f2_wr64(sb + SB_OFF_BLOCK_COUNT, vol_blocks);
    f2_wr32(sb + SB_OFF_SECTION_COUNT, main_segs);
    f2_wr32(sb + SB_OFF_SEGMENT_COUNT, total_segs);
    f2_wr32(sb + SB_OFF_SEG_CKPT, 2);
    f2_wr32(sb + SB_OFF_SEG_SIT, 2);
    f2_wr32(sb + SB_OFF_SEG_NAT, 2);
    f2_wr32(sb + SB_OFF_SEG_SSA, 1);
    f2_wr32(sb + SB_OFF_SEG_MAIN, main_segs);
    f2_wr32(sb + SB_OFF_SEG0_BLKADDR, seg0);
    f2_wr32(sb + SB_OFF_CP_BLKADDR, cp0);
    f2_wr32(sb + SB_OFF_SIT_BLKADDR, sit0);
    f2_wr32(sb + SB_OFF_NAT_BLKADDR, nat0);
    f2_wr32(sb + SB_OFF_SSA_BLKADDR, ssa0);
    f2_wr32(sb + SB_OFF_MAIN_BLKADDR, main0);
    f2_wr32(sb + SB_OFF_ROOT_INO, root_nid);
    f2_wr32(sb + SB_OFF_NODE_INO, node_ino);
    f2_wr32(sb + SB_OFF_META_INO, meta_ino);
    f2_wr32(sb + SB_OFF_CP_PAYLOAD, 0);
    f2_write_block(0, f2_fmt);

    /* ---- main 区 root 内容（先于 CP：mkfs f2fs_create_root_dir 顺序） ---- */

    /* root dentry 块（HOT_DATA 段 3 块 0）：'.' + '..'（mkfs add_dentry） */
    for (uint32_t i = 0; i < F2FS_BLKSIZE; i++) f2_fmt[i] = 0;
    {
        uint8_t *bitmap = f2_fmt;
        uint8_t *dentry = f2_dentry_base(f2_fmt);
        uint8_t *filename = f2_filename_base(f2_fmt);
        /* slot0 '.' */
        f2_wr32(dentry, 0);                       /* dot hash = 0 */
        f2_wr32(dentry + 4, root_nid);
        f2_wr16(dentry + 8, 1);
        dentry[10] = F2FS_FT_DIR;
        filename[0] = '.';
        bitmap[0] |= 1u << 0;
        /* slot1 '..' */
        f2_wr32(dentry + 11, 0);
        f2_wr32(dentry + 11 + 4, root_nid);
        f2_wr16(dentry + 11 + 8, 2);
        dentry[11 + 10] = F2FS_FT_DIR;
        filename[8] = '.'; filename[9] = '.';
        bitmap[0] |= 1u << 1;
    }
    f2_write_block(root_dentry_blk, f2_fmt);

    /* root inode（HOT_NODE 段 0 块 1；mkfs f2fs_write_root_inode） */
    for (uint32_t i = 0; i < F2FS_BLKSIZE; i++) f2_fmt[i] = 0;
    f2_wr16(f2_fmt + INO_OFF_MODE, 0x41ED);        /* dir 0755 */
    f2_wr32(f2_fmt + 12, 2);                       /* i_links */
    f2_wr64(f2_fmt + INO_OFF_SIZE, F2FS_BLKSIZE);
    f2_wr64(f2_fmt + INO_OFF_BLOCKS, 2 * F2FS_BLK_SECS);
    f2_wr32(f2_fmt + INO_OFF_CURRENT_DEPTH, 1);
    f2_fmt[INO_OFF_DIR_LEVEL] = 0;                 /* DEF_DIR_LEVEL */
    f2_wr32(f2_fmt + INO_OFF_ADDR + 0 * 4, root_dentry_blk);
    f2_set_footer(f2_fmt, root_nid, root_nid, root_inode_blk + 1);
    f2_write_block(root_inode_blk, f2_fmt);

    /* ---- SIT 区（两副本；mkfs init_sit_area + 初始有效位 + curseg 型位） ---- */
    for (uint32_t copy = 0; copy < 2; copy++) {
        for (uint32_t i = 0; i < F2FS_BLKSIZE; i++) f2_fmt[i] = 0;
        /* main 段 0（HOT_NODE）：root inode 块（off 1）有效 */
        {
            uint8_t *se = f2_fmt + 0 * SIT_ENTRY_SIZE;
            f2_set_bit_be(1, se + 2);
            f2_wr16(se, (uint16_t)(1 | (CURSEG_HOT_NODE << SIT_VBLOCKS_SHIFT)));
            f2_wr64(se + 2 + SIT_VBLOCK_MAP_SIZE, 1);
        }
        /* main 段 3（HOT_DATA）：root dentry 块（off 0）有效 */
        {
            uint8_t *se = f2_fmt + 3 * SIT_ENTRY_SIZE;
            f2_set_bit_be(0, se + 2);
            f2_wr16(se, (uint16_t)(1 | (CURSEG_HOT_DATA << SIT_VBLOCKS_SHIFT)));
            f2_wr64(se + 2 + SIT_VBLOCK_MAP_SIZE, 1);
        }
        f2_write_block(sit0 + copy * bps, f2_fmt);
    }

    /* ---- NAT 区（两副本；mkfs init_nat_area + f2fs_update_nat_default） ---- */
    for (uint32_t copy = 0; copy < 2; copy++) {
        for (uint32_t i = 0; i < F2FS_BLKSIZE; i++) f2_fmt[i] = 0;
        /* root(3) -> root inode 块；node(1)/meta(2) -> 块 1（mkfs 语义） */
        {
            uint8_t *e = f2_fmt + root_nid * NAT_ENTRY_SIZE;
            e[0] = 0;
            f2_wr32(e + 1, root_nid);
            f2_wr32(e + 5, root_inode_blk);
            e = f2_fmt + node_ino * NAT_ENTRY_SIZE;
            f2_wr32(e + 1, node_ino);
            f2_wr32(e + 5, 1);
            e = f2_fmt + meta_ino * NAT_ENTRY_SIZE;
            f2_wr32(e + 1, meta_ino);
            f2_wr32(e + 5, 1);
        }
        f2_write_block(nat0 + copy * bps, f2_fmt);
    }

    /* ---- SSA（先整段清零防旧盘残留，再写 main 段 0/3 的 summary） ---- */
    {
        for (uint32_t b = 0; b < bps; b++) {
            for (uint32_t i = 0; i < F2FS_BLKSIZE; i++) f2_fmt[i] = 0;
            f2_write_block(ssa0 + b, f2_fmt);
        }
        for (uint32_t i = 0; i < F2FS_BLKSIZE; i++) f2_fmt[i] = 0;
        f2_fmt[4091] = 1;                          /* SUM_TYPE_NODE */
        uint8_t *s = f2_fmt + 1 * 7;
        f2_wr32(s, root_nid);
        f2_wr16(s + 5, 0);
        f2_write_block(ssa0 + 0, f2_fmt);

        for (uint32_t i = 0; i < F2FS_BLKSIZE; i++) f2_fmt[i] = 0;
        f2_fmt[4091] = 0;                          /* SUM_TYPE_DATA */
        s = f2_fmt + 0 * 7;
        f2_wr32(s, root_nid);
        f2_wr16(s + 5, 0);
        f2_write_block(ssa0 + 3, f2_fmt);
    }

    /* ---- CP pack（mkfs write_check_point_pack） ---- */
    uint8_t *cp = f2_fmt;
    for (uint32_t i = 0; i < F2FS_BLKSIZE; i++) cp[i] = 0;
    /* checkpoint_ver：format 开头已生成的随机奇数（root footer 同值） */
    f2_wr64(cp + CP_OFF_VER, f2_cp_ver);
    f2_wr64(cp + CP_OFF_USER_BLOCKS, (uint64_t)main_segs * bps);
    f2_wr64(cp + CP_OFF_VALID_BLOCKS, 2);          /* root inode + dentry */
    f2_wr32(cp + CP_OFF_RSVD_SEGS, 2);
    f2_wr32(cp + CP_OFF_OVERPROV_SEGS, 2);
    /* curseg 初始（mkfs verify_cur_segs；节点型 idx=type-3，数据型 idx=type；
     * [3..7] = 0xffffffff 无效占位，mkfs 同） */
    f2_wr32(cp + CP_OFF_CUR_NODE_SEG + (CURSEG_HOT_NODE - 3) * 4, 0);
    f2_wr32(cp + CP_OFF_CUR_NODE_SEG + (CURSEG_WARM_NODE - 3) * 4, 1);
    f2_wr32(cp + CP_OFF_CUR_NODE_SEG + (CURSEG_COLD_NODE - 3) * 4, 2);
    for (int i = 3; i < 8; i++)
        f2_wr32(cp + CP_OFF_CUR_NODE_SEG + i * 4, 0xFFFFFFFFu);
    f2_wr32(cp + CP_OFF_CUR_DATA_SEG + CURSEG_HOT_DATA * 4, 3);
    f2_wr32(cp + CP_OFF_CUR_DATA_SEG + CURSEG_COLD_DATA * 4, 13);
    f2_wr32(cp + CP_OFF_CUR_DATA_SEG + CURSEG_WARM_DATA * 4, 27);
    for (int i = 3; i < 8; i++)
        f2_wr32(cp + CP_OFF_CUR_DATA_SEG + i * 4, 0xFFFFFFFFu);
    f2_wr16(cp + CP_OFF_CUR_NODE_BLKOFF + (CURSEG_HOT_NODE - 3) * 2, 2);
    for (int i = 3; i < 8; i++)
        f2_wr16(cp + CP_OFF_CUR_NODE_BLKOFF + i * 2, 0);
    f2_wr16(cp + CP_OFF_CUR_DATA_BLKOFF + CURSEG_HOT_DATA * 2, 1);
    for (int i = 3; i < 8; i++)
        f2_wr16(cp + CP_OFF_CUR_DATA_BLKOFF + i * 2, 0);
    f2_wr32(cp + CP_OFF_FREE_SEGS, main_segs - 6);  /* mkfs: usable - 6 used */
    f2_wr32(cp + CP_OFF_CKPT_FLAGS, CP_UMOUNT_FLAG | CP_COMPACT_SUM_FLAG);
    f2_wr32(cp + CP_OFF_PACK_TOTAL, 6);
    f2_wr32(cp + CP_OFF_PACK_START_SUM, 1);        /* journal @ pack+1 */
    f2_wr32(cp + CP_OFF_VALID_NODES, 1);
    f2_wr32(cp + CP_OFF_VALID_INODES, 1);
    f2_wr32(cp + CP_OFF_NEXT_FREE_NID, 4);
    f2_wr32(cp + CP_OFF_SIT_VER_BYTES, 8);        /* (2/2 段 × 64 块)/8 */
    f2_wr32(cp + CP_OFF_NAT_VER_BYTES, 8);
    f2_wr32(cp + CP_OFF_CKSUM_OFF, CHECKSUM_OFFSET);
    cp[CP_OFF_ALLOC_TYPE + CURSEG_HOT_NODE] = 0;   /* ALLOC_NEXT_SEG */
    cp[CP_OFF_ALLOC_TYPE + CURSEG_HOT_DATA] = 0;
    /* sit_nat_version_bitmap：全 0（副本 0 生效） */

    /* journal（compact summary 块内容；mkfs update_nat/sit_journal）
     * SIT journal 语义（libf2fs/mkfs）：条目按 curseg 类型固定槽位 idx：
     *   idx 0=HOT_DATA 1=WARM_DATA 2=COLD_DATA 3=HOT_NODE 4=WARM_NODE 5=COLD_NODE
     * 条目内 segno 字段 = 该 curseg 的段号。mkfs 写 n_sits=6（全部槽位） */
    for (uint32_t i = 0; i < SUM_JOURNAL_SIZE; i++) f2_natj[i] = 0;
    for (uint32_t i = 0; i < SUM_JOURNAL_SIZE; i++) f2_sitj[i] = 0;
    {
        /* NAT journal: root -> root inode 块（mkfs update_nat_journal） */
        uint8_t *e = f2_natj + 2;
        f2_wr32(e, root_nid);
        e[4] = 0;
        f2_wr32(e + 5, root_nid);
        f2_wr32(e + 9, root_inode_blk);
        f2_wr16(f2_natj, 1);

        /* SIT journal: 6 槽位（idx=curseg 类型；segno=该 curseg 段；
         * 仅 HOT_NODE(0 段,off1 有效) 与 HOT_DATA(3 段,off0 有效) 有内容） */
        static const uint32_t curseg_seg[6] = { 3, 27, 13, 0, 1, 2 };
        for (int t = 0; t < 6; t++) {
            uint8_t *sj = f2_sitj + 2 + (uint32_t)t * SIT_JENTRY_SIZE;
            f2_wr32(sj, curseg_seg[t]);
            uint16_t vb = (uint16_t)(t << SIT_VBLOCKS_SHIFT);
            uint8_t *vmap = sj + 4 + 2;
            if (t == CURSEG_HOT_NODE) {
                vb |= 1;
                f2_set_bit_be(1, vmap);              /* root inode @ off 1 */
            } else if (t == CURSEG_HOT_DATA) {
                vb |= 1;
                f2_set_bit_be(0, vmap);              /* root dentry @ off 0 */
            }
            f2_wr16(sj + 4, vb);
            f2_wr64(sj + 4 + 2 + SIT_VBLOCK_MAP_SIZE, 1);
        }
        f2_wr16(f2_sitj, 6);
    }

    /* 写 pack1：[CP 头][compact sum][hot_node sums][warm sums][cold sums][CP 尾]
     * 注意 cp 与 f2_fmt 是同一缓冲：CP 尾块必须在 f2_fmt 被复用清零之前写 */
    f2_wr32(cp + CHECKSUM_OFFSET, f2_crc32(cp, CHECKSUM_OFFSET));
    f2_write_block(cp0, cp);
    f2_write_block(cp0 + 5, cp);                   /* CP 尾块（先于 f2_fmt 复用） */

    for (uint32_t i = 0; i < F2FS_BLKSIZE; i++) f2_fmt[i] = 0;
    for (uint32_t i = 0; i < SUM_JOURNAL_SIZE; i++)
        f2_fmt[i] = f2_natj[i];
    for (uint32_t i = 0; i < SUM_JOURNAL_SIZE; i++)
        f2_fmt[SUM_JOURNAL_SIZE + i] = f2_sitj[i];
    f2_fmt[4091] = 0;                              /* SUM_TYPE_DATA */
    f2_wr32(f2_fmt + 0, root_nid);                 /* hot data: off0 root dentry */
    f2_wr16(f2_fmt + 5, 0);
    f2_write_block(cp0 + 1, f2_fmt);               /* compact summary */

    for (uint32_t i = 0; i < F2FS_BLKSIZE; i++) f2_fmt[i] = 0;
    f2_fmt[4091] = 1;                              /* SUM_TYPE_NODE */
    f2_wr32(f2_fmt + 1 * 7, root_nid);             /* off1 root inode */
    f2_wr16(f2_fmt + 1 * 7 + 5, 0);
    f2_write_block(cp0 + 2, f2_fmt);               /* hot node sums */

    for (uint32_t i = 0; i < F2FS_BLKSIZE; i++) f2_fmt[i] = 0;
    f2_fmt[4091] = 1;
    f2_write_block(cp0 + 3, f2_fmt);               /* warm node sums: 空 */

    for (uint32_t i = 0; i < F2FS_BLKSIZE; i++) f2_fmt[i] = 0;
    f2_fmt[4091] = 1;
    f2_write_block(cp0 + 4, f2_fmt);               /* cold node sums: 空 */

    /* pack2：版本 0（mkfs 语义：mount 校验 pack2 CRC 失败即弃） */
    {
        for (uint32_t i = 0; i < F2FS_BLKSIZE; i++) f2_fmt[i] = 0;
        uint8_t *cp2 = f2_fmt;
        f2_wr64(cp2 + CP_OFF_VER, 0);
        f2_wr32(cp2 + CP_OFF_CKSUM_OFF, CHECKSUM_OFFSET);
        f2_wr32(cp2 + CHECKSUM_OFFSET, f2_crc32(cp2, CHECKSUM_OFFSET));
        f2_write_block(cp0 + bps, cp2);
        f2_write_block(cp0 + bps + 5, cp2);
    }

    /* 挂载验证 */
    if (f2fs_mount(drive, part_start) != 0) return -1;
    return 0;
}
