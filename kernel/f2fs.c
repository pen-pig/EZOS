/*
 * f2fs.c - F2FS 只读驱动
 *
 * 支持范围（只读，4KB 块）：
 *   - 双 CP pack 校验（CRC32，init=F2FS_SUPER_MAGIC）取高版本
 *   - NAT 查找：CP NAT journal 优先，回退 NAT 区块（含版本位图偏移）
 *   - inline data（i_addr[1] 起）与 inline dentry
 *   - 常规文件：直接块 + 一级/二级间接节点链（grub_get_node_path）
 *   - 常规目录：dentry block 位图遍历
 *   - 不支持：压缩（LZO/LZ4）、加密、符号链接、cp_payload 布局
 *
 * refs:
 *   - GRUB grub-core/fs/f2fs.c (GPLv3+) - 全部解析流程与磁盘布局常量
 *   - Linux include/uapi/linux/f2fs.h - superblock/checkpoint/node 结构
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

/* ---------- 常量（GRUB f2fs.c / Linux f2fs.h） ---------- */
#define F2FS_SUPER_MAGIC        0xF2F52010u
#define F2FS_BLKSIZE            4096u
#define F2FS_BLK_SECS           8           /* 4KB / 512 */
#define F2FS_MIN_LOG_SEC        9
#define F2FS_BLK_BITS           12

#define CHECKSUM_OFFSET         4092
#define CRCPOLY_LE              0xEDB88320u

#define CP_COMPACT_SUM_FLAG     0x00000004u
#define CP_UMOUNT_FLAG          0x00000001u

#define NR_CURSEG_DATA_TYPE     3
#define NR_CURSEG_TYPE          6
#define CURSEG_HOT_DATA         0

#define SUM_ENTRIES_SIZE        (7 * 512)   /* SUMMARY_SIZE * ENTRIES_IN_SUM */
#define SUM_JOURNAL_SIZE        (F2FS_BLKSIZE - 5 - SUM_ENTRIES_SIZE)  /* 507 */
#define JENTRY_SIZE             13          /* nid(4) + nat_entry(9) */

#define NAT_ENTRY_SIZE          9           /* version(1) ino(4) block_addr(4) */
#define NAT_ENTRY_PER_BLOCK     (F2FS_BLKSIZE / NAT_ENTRY_SIZE)        /* 455 */

#define F2FS_SLOT_LEN           8
#define NR_DENTRY_IN_BLOCK      214
#define SIZE_OF_DIR_ENTRY       11
#define SIZE_OF_DENTRY_BITMAP   ((NR_DENTRY_IN_BLOCK + 7) / 8)         /* 27 */
#define SIZE_OF_RESERVED        (F2FS_BLKSIZE - \
                                 ((SIZE_OF_DIR_ENTRY + F2FS_SLOT_LEN) * \
                                  NR_DENTRY_IN_BLOCK + SIZE_OF_DENTRY_BITMAP)) /* 3 */

#define F2FS_INLINE_XATTR_ADDRS 50
#define DEF_ADDRS_PER_INODE     923
#define ADDRS_PER_BLOCK         1018
#define NIDS_PER_BLOCK          1018
#define NODE_DIR1_BLOCK         (DEF_ADDRS_PER_INODE + 1)
#define NODE_DIR2_BLOCK         (DEF_ADDRS_PER_INODE + 2)
#define NODE_IND1_BLOCK         (DEF_ADDRS_PER_INODE + 3)
#define NODE_IND2_BLOCK         (DEF_ADDRS_PER_INODE + 4)
#define NODE_DIND_BLOCK         (DEF_ADDRS_PER_INODE + 5)

#define MAX_INLINE_DATA         (4 * (DEF_ADDRS_PER_INODE - \
                                      F2FS_INLINE_XATTR_ADDRS - 1))    /* 3488 */
#define NR_INLINE_DENTRY        (MAX_INLINE_DATA * 8 / \
                                 ((SIZE_OF_DIR_ENTRY + F2FS_SLOT_LEN) * 8 + 1)) /* 182 */
#define INLINE_DENTRY_BITMAP_SIZE ((NR_INLINE_DENTRY + 7) / 8)         /* 23 */
#define INLINE_RESERVED_SIZE    (MAX_INLINE_DATA - \
                                 ((SIZE_OF_DIR_ENTRY + F2FS_SLOT_LEN) * \
                                  NR_INLINE_DENTRY + INLINE_DENTRY_BITMAP_SIZE))

/* i_inline 位 */
#define F2FS_INLINE_XATTR       0x01
#define F2FS_INLINE_DATA        0x02
#define F2FS_INLINE_DENTRY      0x04
#define F2FS_DATA_EXIST         0x08

/* 目录项 file_type */
#define F2FS_FT_REG_FILE        1
#define F2FS_FT_DIR             2

/* superblock 偏移（Linux f2fs.h；GRUB dummy2 覆盖区含 block_count） */
#define SB_OFF_MAGIC            0
#define SB_OFF_LOG_SEC          8
#define SB_OFF_LOG_SPB          12
#define SB_OFF_LOG_BLK          16
#define SB_OFF_LOG_BPS          20
#define SB_OFF_BLOCK_COUNT      0x28        /* __le64 */
#define SB_OFF_CP_BLKADDR       76
#define SB_OFF_SIT_BLKADDR      80
#define SB_OFF_NAT_BLKADDR      84
#define SB_OFF_SSA_BLKADDR      88
#define SB_OFF_MAIN_BLKADDR     92
#define SB_OFF_ROOT_INO         96
#define SB_OFF_CP_PAYLOAD       1152

/* checkpoint 偏移 */
#define CP_OFF_VER              0           /* __le64 */
#define CP_OFF_USER_BLOCKS      8
#define CP_OFF_VALID_BLOCKS     16
#define CP_OFF_CKPT_FLAGS       0x84
#define CP_OFF_PACK_TOTAL       0x88
#define CP_OFF_PACK_START_SUM   0x8C
#define CP_OFF_VALID_NODES      0x90
#define CP_OFF_VALID_INODES     0x94
#define CP_OFF_SIT_VER_BYTES    0x9C
#define CP_OFF_NAT_VER_BYTES    0xA0
#define CP_OFF_CKSUM_OFF        0xA4
#define CP_OFF_BITMAP           0xC0        /* sit_nat_version_bitmap */

/* inode 偏移（f2fs_node 前 4072 字节内的 f2fs_inode 部分） */
#define INO_OFF_MODE            0
#define INO_OFF_INLINE          3
#define INO_OFF_SIZE            16
#define INO_OFF_BLOCKS          24
#define INO_OFF_PINO            84
#define INO_OFF_NAMELEN         88
#define INO_OFF_NAME            92
#define INO_OFF_ADDR            360         /* i_addr[923] */
#define INO_OFF_NID             4052        /* i_nid[5]，节点尾 40 字节前 */

#define F2FS_MAX_NAME           255

/* ---------- 挂载状态 ---------- */
static uint8_t  f2_drive;
static uint8_t  f2_mounted;
static uint32_t f2_part_lba;
static uint32_t f2_bps;                 /* blocks_per_seg */
static uint32_t f2_cp_blkaddr, f2_nat_blkaddr, f2_main_blkaddr;
static uint32_t f2_root_ino;
static uint32_t f2_total_blocks;        /* block_count */
static uint32_t f2_start_cp;            /* 选中的 CP pack 起始块 */
static uint32_t f2_sit_bytes;           /* SIT 位图字节数（cp_payload>0 时为 0） */
static uint8_t  f2_cp_payload_flag;     /* superblock cp_payload>0 */
static f2fs_info_t f2_info;

/* CP 副本（持久）、NAT journal 副本、节点/NAT/目录块暂存 */
/* 大缓冲放高内存段 .bss.hi（1MB+，见 linker.ld）：低 640KB 区留给栈/小数据 */
#define F2_HIBUF __attribute__((section(".bss.hi")))
static uint8_t f2_cp[F2FS_BLKSIZE] F2_HIBUF;
static uint8_t f2_natj[SUM_JOURNAL_SIZE] F2_HIBUF;
static uint8_t f2_node[F2FS_BLKSIZE] F2_HIBUF;   /* 当前解析节点 */
static uint8_t f2_child[F2FS_BLKSIZE] F2_HIBUF;  /* 枚举子节点（隔离于 f2_node） */
static uint8_t f2_blk[F2FS_BLKSIZE] F2_HIBUF;    /* NAT 块 / 间接节点链暂存 */
static uint8_t f2_dblk[F2FS_BLKSIZE] F2_HIBUF;   /* 非 inline 目录块 */

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

/* 大端位序测试（GRUB grub_f2fs_test_bit：MSB 在前） */
static int f2_test_bit_be(uint32_t nr, const uint8_t *p) {
    return p[nr >> 3] & (1u << (7 - (nr & 7)));
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

/* 读 NAT journal（GRUB get_nat_journal）到 f2_natj */
static int f2_load_nat_journal(void) {
    uint32_t block;
    uint32_t flags = rd32(f2_cp + CP_OFF_CKPT_FLAGS);
    if (flags & CP_COMPACT_SUM_FLAG)
        block = f2_start_cp + rd32(f2_cp + CP_OFF_PACK_START_SUM);
    else if (flags & CP_UMOUNT_FLAG)
        block = f2_start_cp + rd32(f2_cp + CP_OFF_PACK_TOTAL) -
                (NR_CURSEG_TYPE + 1) + CURSEG_HOT_DATA;
    else
        block = f2_start_cp + rd32(f2_cp + CP_OFF_PACK_TOTAL) -
                (NR_CURSEG_DATA_TYPE + 1) + CURSEG_HOT_DATA;

    f2_read_block(block, f2_blk);
    if (flags & CP_COMPACT_SUM_FLAG) {
        for (uint32_t i = 0; i < SUM_JOURNAL_SIZE; i++)
            f2_natj[i] = f2_blk[i];
    } else {
        for (uint32_t i = 0; i < SUM_JOURNAL_SIZE; i++)
            f2_natj[i] = f2_blk[SUM_ENTRIES_SIZE + i];
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
    for (uint16_t i = 0; i < n && i < SUM_JOURNAL_SIZE / JENTRY_SIZE; i++) {
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
        const uint8_t *bitmap = base;
        const uint8_t *dentry = base + INLINE_DENTRY_BITMAP_SIZE +
                                INLINE_RESERVED_SIZE;
        const uint8_t *filename = dentry + NR_INLINE_DENTRY * SIZE_OF_DIR_ENTRY;
        return f2_check_dentries(bitmap, dentry, filename,
                                 NR_INLINE_DENTRY, cb, ctx);
    }

    /* 常规目录：逐 dentry block */
    uint64_t size = rd64(node + INO_OFF_SIZE);
    for (uint64_t fpos = 0; fpos < size; fpos += F2FS_BLKSIZE) {
        uint32_t pb = f2_get_block(node, (uint32_t)(fpos / F2FS_BLKSIZE));
        if (pb == 0) continue;
        f2_read_block(pb, f2_dblk);
        const uint8_t *bitmap = f2_dblk;
        const uint8_t *dentry = f2_dblk + SIZE_OF_DENTRY_BITMAP + SIZE_OF_RESERVED;
        const uint8_t *filename = dentry + NR_DENTRY_IN_BLOCK * SIZE_OF_DIR_ENTRY;
        if (f2_check_dentries(bitmap, dentry, filename,
                              NR_DENTRY_IN_BLOCK, cb, ctx))
            return 1;
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
            f2_read_secs(f2_part_lba + pb * F2FS_BLK_SECS,
                         buffer + done, F2FS_BLK_SECS);
            if (chunk < F2FS_BLKSIZE) {
                /* 搬移：尾部截断（buffer 已整块读入，无需搬移） */
            }
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
    f2_nat_blkaddr = rd32(sb + SB_OFF_NAT_BLKADDR);
    f2_main_blkaddr = rd32(sb + SB_OFF_MAIN_BLKADDR);
    f2_root_ino = rd32(sb + SB_OFF_ROOT_INO);
    f2_total_blocks = (uint32_t)rd64(sb + SB_OFF_BLOCK_COUNT);
    uint32_t cp_payload = rd32(sb + SB_OFF_CP_PAYLOAD);
    if (f2_root_ino < 3 || f2_total_blocks < 16 ||
        f2_main_blkaddr >= f2_total_blocks) return -1;

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
