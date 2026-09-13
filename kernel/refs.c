/*
 * refs.c - ReFS 驱动（读 + 写，ReFS 1.2 标准卷布局）
 *
 * 按 libyal/libfsrefs 逆向规范实现的 ReFS 1.2 卷（Windows 8.1/Win10 1607
 * 时代布局，Win10 之前可原生识别；16MB 卷 / 512B 扇区 / 4KB 簇 / 16KB
 * 元数据块 = v1 固定值）：
 *
 *   块 0          : 卷头 512B（0x03 "ReFS\0\0\0\0" + 0x10 "FSRS" + MSDN
 *                   校验和 + nsecs/扇区/簇/版本 1.2）+ 卷尾副本（rel 32766）
 *   块 30 (0x7800): superblock（v1 metadata block 头 48B + SB 48B +
 *                   2 个 CP 块号 + 自引用 block reference 24B）
 *   块 31/32      : checkpoint A/B（头 16B + trailer v1 28B + offsets
 *                   数组 -> 6 个树 block reference；序列号大者为活动 CP）
 *   块 33..38     : 树 0..5 根节点（0=objects tree，1..5=分配器/模式树，
 *                   单叶空根；树 0 每次提交 CoW 到新块）
 *   块 39..       : 根目录(0x600)及子目录(0x701+)的目录对象叶节点 +
 *                   文件数据块（data run 物理偏移 = 16KB 块号）
 *
 * 关键结构（libfsrefs 偏移依据，见文件尾参考）：
 *   v1 metadata block 头 48B：0=块号 u64 8=序列号 u64 16=对象 ID 16B
 *     32=0x01（v1 无 "SUPB"/"CHKP"/"MSB+" 签名，libfsrefs 不校验内容）
 *   block reference 24B：0=块号 u64 10=校验类型(1=CRC32C) 11=数据偏移
 *     12=数据大小 u16 16=校验数据 8B（libfsrefs 仅校验类型字段 ∈ {1,2}）
 *   ministore 节点：0=节点头偏移 u32（40=块级节点；嵌入节点 = 4+36+头长）
 *     + tree header 36B（0=表数据偏移 40）+ 节点头 32B（0=数据区起
 *     4=数据区止 8=未用 12=层级 13=标志(0x02=根叶) 16=偏移表起 20=数量
 *     24=偏移表止，均相对节点头）+ records（紧凑排列）+ 偏移表 u32[]
 *   node record 头 14B：0=总大小 u32（含头）4=key 偏移 u16（=14）
 *     6=key 大小 u16 8=flags u16（0x0008=值含嵌入 ministore）
 *     10=value 偏移 u16 12=value 大小 u16
 *   objects tree record：key 16B（高 64 位 0 + 低 64 位对象 ID，升序）；
 *     value 48B（bref 24B + 未知 24B）
 *   目录 entry record：key = 0x0030 + 类型(1=文件 2=目录) + UTF-16LE 名；
 *     文件 value 612B = 嵌入 ministore（file values 128B + $DATA 属性
 *     record：flags 0x0008 -> 非驻留属性 380B = 96B 头 + data run record
 *     （key 16B + value 32B：逻辑 0/块数/物理块号/0））；
 *     目录 value 72B = directory values（0=对象 ID u64）
 *
 * 提交协议（真实 ReFS CoW 语义）：每次写操作分配新目录节点块 + 新
 * objects tree 根块，内容写入后翻转 CP（seq+1 写入对侧 CP 块，树 0
 * bref 指向新根）。旧世代块成为孤儿（活动 CP 不可达），bump 分配器
 * 从活动世代最大块号 +1 续分配。
 *
 * refs:
 *   - libyal/libfsrefs documentation/Resilient File System (ReFS).asciidoc
 *   - libfsrefs_volume.c / _superblock.c / _checkpoint.c / _ministore_node.c
 *     / _objects_tree.c / _directory_entry.c / _attribute_values.c
 *   - MSDN FILE_SYSTEM_RECOGNITION_STRUCTURE 校验和算法
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
static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}
static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void wr64(uint8_t *p, uint64_t v) {
    wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32));
}
static void rs_zero(uint8_t *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) p[i] = 0;
}
static void rs_copy(uint8_t *d, const uint8_t *s, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}
/* 重叠安全的移动（前/后向按方向选择） */
static void rs_move(uint8_t *d, const uint8_t *s, uint32_t n) {
    if (n == 0 || d == s) return;
    if (d < s) {
        for (uint32_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (uint32_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
}

/* ---------- 布局常量（libfsrefs 结构偏移） ---------- */
#define RS_META_BLK       16384    /* v1 metadata block = 16KB */
#define RS_META_SECS      32
#define RS_MBH_SIZE       48       /* v1 metadata block 头 */
#define RS_NODE_AREA      (RS_META_BLK - RS_MBH_SIZE)   /* 16336 */
#define RS_NHO_BLOCK      40       /* 块级节点：头偏移 = 4+36 */
#define RS_TABLE_REL      15896    /* 块级节点 record 偏移表（相对节点头） */
#define RS_TABLE_CAP      100      /* (16296-15896)/4 */
#define RS_TH_SIZE        36       /* tree header */
#define RS_NH_SIZE        32       /* node header */
#define RS_NR_HDR         14       /* node record 头 */
#define RS_BREF_SIZE      24       /* block reference */

#define RS_SUPERBLOCK_NO  30
#define RS_CPA_NO         31
#define RS_CPB_NO         32
#define RS_OTREE0_NO      33       /* format 时树 0（objects） */
#define RS_XTREE0_NO      34       /* format 时树 1..5 */
#define RS_ROOTDIR0_NO    39
#define RS_POOL0          40       /* bump 分配起点 */
#define RS_POOL_MAX       1023     /* 可用块号上限（0..1022） */
#define RS_VOL_SECS       32767    /* 卷扇区数（rel 0..32766） */
#define RS_NSECS_FIELD    32766    /* 卷头 nsecs 字段（不含卷头扇区） */

/* 卷头字段 */
#define RVH_NSECS     24
#define RVH_SEC       32
#define RVH_SPC       36
#define RVH_MAJ       40
#define RVH_MIN       41
#define RVH_SERIAL    56

/* metadata block header v1 */
#define MBH_BLKNO     0
#define MBH_SEQ       8

/* superblock（块内偏移） */
#define SB_BLK        RS_MBH_SIZE                 /* 48 */
#define SB_VOLID      (SB_BLK + 0)                /* 16B */
#define SB_SEQ        (SB_BLK + 24)
#define SB_CP_OFF     (SB_BLK + 32)               /* = 96 */
#define SB_CP_N       (SB_BLK + 36)               /* = 2 */
#define SB_SELF_OFF   (SB_BLK + 40)               /* = 128 */
#define SB_SELF_SIZE  (SB_BLK + 44)               /* = 24 */
#define SB_CPARR      96                          /* 2 x u64 块号 */
#define SB_SELF_BREF  128

/* checkpoint（块内偏移） */
#define CP_BLK        RS_MBH_SIZE                 /* 48 */
#define CPH_MAJ       (CP_BLK + 4)                 /* = 1 */
#define CPH_MIN       (CP_BLK + 6)                 /* = 2 */
#define CPH_SELF_OFF  (CP_BLK + 8)                 /* = 128 */
#define CPH_SELF_SIZE (CP_BLK + 12)               /* = 24 */
#define CPT_SEQ       (CP_BLK + 16)               /* 64 */
#define CPT_SIZE      (CP_BLK + 24)               /* 72 */
#define CPT_UNK4      (CP_BLK + 28)               /* = 32 */
#define CPT_NOFF      (CP_BLK + 40)               /* 88 */
#define CP_OFFARR     (CP_BLK + 44)               /* 92 */
#define CP_SELF_BREF  128
#define CP_TREE_BREF  160                         /* 6 x 24B */
#define RS_NTREES     6

/* block reference */
#define BR_BLKNO      0
#define BR_CKTYPE     10
#define BR_CKOFF      11
#define BR_CKSIZE      12

/* ministore 节点（相对节点数据区） */
#define MN_NHO        0
#define TH_TABLE_OFF  4      /* tree header 在节点内偏移 4，表数据偏移字段在 +0 */

/* node header（相对节点头） */
#define NH_DATA_START  0
#define NH_DATA_END    4
#define NH_UNUSED      8
#define NH_LEVEL       12
#define NH_FLAGS       13
#define NH_RECS_OFF    16
#define NH_RECS_N      20
#define NH_RECS_END    24

/* node record 头（相对 record） */
#define NR_SIZE        0
#define NR_KEY_OFF     4
#define NR_KEY_SIZE    6
#define NR_FLAGS       8
#define NR_VAL_OFF     10
#define NR_VAL_SIZE    12

/* 嵌入 ministore（文件 entry value 612B） */
#define FV_VAL_SIZE    612
#define FV_NHO         168    /* 4 + 36 + 128 */
#define FV_HDR         40     /* file values 128B 起点（值内偏移） */
#define FV_SIZE_OFF    (FV_HDR + 64)
#define FV_ALLOC_OFF   (FV_HDR + 72)
#define FV_TABLE_REL   440    /* 相对节点头（168） */

/* $DATA 属性 record（FV 节点内唯一 record） */
#define AT_KEY_SIZE    14
#define AT_TYPE_OFF    8      /* key 内：0x80 = $DATA */

/* 非驻留属性 value（380B） */
#define NRAT_VAL_SIZE  380
#define NRAT_NHO       136    /* 4 + 36 + 96 */
#define NRAT_HDR       40
#define NRAT_ALLOC     (NRAT_HDR + 12)
#define NRAT_SIZE      (NRAT_HDR + 20)
#define NRAT_VALID     (NRAT_HDR + 28)
#define NRAT_TABLE_REL 240

/* data run record（NRAT 节点内 record） */
#define DR_KEY_SIZE    16     /* = 值前 16 字节（逻辑偏移 + 大小） */
#define DR_VAL_SIZE    32

/* 目录 entry */
#define DE_KEY_HDR     4      /* 0x0030 + 类型 */
#define DE_TYPE_OFF    2
#define DE_TYPE_FILE   1
#define DE_TYPE_DIR    2
#define DV_SIZE        72     /* directory values */
#define DV_OBJID       0

/* objects tree record */
#define OT_KEY_SIZE    16
#define OT_VAL_SIZE    48
#define OBJ_ROOT       0x600
#define OBJ_SUB_BASE   0x701
#define OBJ_SUB_MAX    0x7FF

#define RS_NAME_MAX    120    /* 文件名（ASCII 字符数上限） */
#define RS_FILETIME    0x01D0000000000000ull

/* ---------- 驱动状态 ---------- */
static uint8_t  rs_drive;
static uint8_t  rs_mounted;
static uint32_t rs_part_lba;
static uint32_t rs_cp_no;            /* 活动 CP 块号 */
static uint64_t rs_cp_seq;           /* 活动 CP 序列号 */
static uint32_t rs_trees[RS_NTREES]; /* CP 引用的 6 棵树根块号 */
static uint32_t rs_otree_blk;        /* = rs_trees[0] */
static uint32_t rs_root_dirblk;      /* 根目录对象节点块号 */
static uint32_t rs_hint;             /* bump 分配游标 */
static uint32_t rs_used_blocks;      /* 活动世代总占用块数（df/游标恢复） */
static uint64_t rs_next_subid;
static refs_info_t rs_info;

/* 大缓冲放高内存段 .bss.hi（1MB+，见 linker.ld） */
#define RS_HIBUF __attribute__((section(".bss.hi")))
static uint8_t rs_blk[RS_META_BLK] RS_HIBUF;      /* 当前节点读写 */
static uint8_t rs_blk2[RS_META_BLK] RS_HIBUF;     /* 第二节点（otree/新内容） */
static uint8_t rs_databuf[RS_META_BLK] RS_HIBUF;  /* 数据块/嵌入值/CP 中转 */
static uint64_t rs_bfs_q[256] RS_HIBUF;           /* 挂载 BFS 队列（对象 ID） */

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
static uint32_t rs_blk_lba(uint32_t blkno) {
    return rs_part_lba + blkno * RS_META_SECS;
}
static int rs_read_block(uint32_t blkno, uint8_t *buf) {
    return rs_read_secs(rs_blk_lba(blkno), buf, RS_META_SECS);
}
static int rs_write_block(uint32_t blkno, const uint8_t *buf) {
    return rs_write_secs(rs_blk_lba(blkno), buf, RS_META_SECS);
}
/* bump 分配：失败返回 0 */
static uint32_t rs_alloc(void) {
    if (rs_hint >= RS_POOL_MAX) return 0;
    return rs_hint++;
}

/* ---------- MSDN FSRS 校验和（前 0x18 字节 WORD 递加取补，跳过 0x16） ---------- */
static uint16_t rs_fsrs_checksum(const uint8_t *vh) {
    uint32_t sum = 0;
    for (uint32_t off = 0; off < 0x18; off += 2) {
        if (off == 0x16) continue;
        sum += rd16(vh + off);
    }
    return (uint16_t)(0x10000 - (sum & 0xFFFF));
}

/* ---------- metadata block 头 / block reference 填充 ---------- */
static void rs_fill_mbh(uint8_t *blk, uint64_t blkno, uint64_t seq) {
    rs_zero(blk, RS_META_BLK);
    wr64(blk + MBH_BLKNO, blkno);
    wr64(blk + MBH_SEQ, seq);
    wr64(blk + 32, 0x01);
}
static void rs_fill_bref(uint8_t *p, uint32_t blkno) {
    rs_zero(p, RS_BREF_SIZE);
    wr64(p + BR_BLKNO, blkno);
    p[BR_CKTYPE] = 1;          /* CRC32C（libfsrefs 仅校验类型 ∈ {1,2}） */
    p[BR_CKOFF] = 16;
    wr16(p + BR_CKSIZE, 8);
    /* 校验数据 8B 置 0（libfsrefs 不验证内容） */
}

/* ---------- ministore 节点构造/解析 ----------
 * node = 节点数据区起点（块级节点 = 块内 48B 之后；嵌入节点 = record value）。
 * 嵌入节点语义（libfsrefs）：nho > 40 时 data[40..nho) = 结构区
 * （file values 128B / 非驻留属性头 96B）。 */
static uint32_t rs_node_nho(const uint8_t *node) {
    return rd32(node + MN_NHO);
}
static uint32_t rs_node_count(const uint8_t *node) {
    return rd32(node + rs_node_nho(node) + NH_RECS_N);
}

/* 初始化空根叶节点（node 已清零 area_size 字节） */
static void rs_node_init(uint8_t *node, uint32_t nho, uint32_t table_rel) {
    wr32(node + MN_NHO, nho);
    wr16(node + TH_TABLE_OFF, 40);       /* 表数据偏移恒 40 */
    uint8_t *nh = node + nho;
    wr32(nh + NH_DATA_START, RS_NH_SIZE);/* 首 record 相对节点头 = 32 */
    wr32(nh + NH_DATA_END, RS_NH_SIZE);
    wr32(nh + NH_UNUSED, 0);
    nh[NH_LEVEL] = 0;
    nh[NH_FLAGS] = 0x02;                 /* 根叶 */
    wr32(nh + NH_RECS_OFF, table_rel);
    wr32(nh + NH_RECS_N, 0);
    wr32(nh + NH_RECS_END, table_rel);
}

/* 追加一条 record（record 偏移表按位置序）。返回条目序号，-1=满/超界 */
static int rs_node_add_rec(uint8_t *node, uint32_t area_size, uint32_t table_rel,
                           const uint8_t *key, uint32_t ksize,
                           uint16_t flags, const uint8_t *value, uint32_t vsize) {
    uint32_t nho = rs_node_nho(node);
    uint8_t *nh = node + nho;
    uint32_t n = rd32(nh + NH_RECS_N);
    if (n >= RS_TABLE_CAP || ksize > 0xFFFF || vsize > 0xFFFF) return -1;
    uint32_t end = rd32(nh + NH_DATA_END);
    uint32_t total = RS_NR_HDR + ksize + vsize;
    if (end + total > table_rel) return -1;
    if (table_rel + (n + 1) * 4 > area_size - nho) return -1;

    uint8_t *rec = node + nho + end;
    wr32(rec + NR_SIZE, total);
    wr16(rec + NR_KEY_OFF, RS_NR_HDR);
    wr16(rec + NR_KEY_SIZE, (uint16_t)ksize);
    wr16(rec + NR_FLAGS, flags);
    wr16(rec + NR_VAL_OFF, RS_NR_HDR + (uint16_t)ksize);
    wr16(rec + NR_VAL_SIZE, (uint16_t)vsize);
    rs_copy(rec + RS_NR_HDR, key, ksize);
    if (value && vsize) rs_copy(rec + RS_NR_HDR + ksize, value, vsize);

    wr32(node + nho + table_rel + n * 4, end);    /* 偏移表项（相对节点头） */
    wr32(nh + NH_DATA_END, end + total);
    wr32(nh + NH_RECS_END, table_rel + (n + 1) * 4);
    wr32(nh + NH_RECS_N, n + 1);
    return (int)n;
}

/* 取第 idx 条 record 指针；越界返回 0 */
static uint8_t *rs_node_rec(uint8_t *node, uint32_t idx) {
    uint32_t nho = rs_node_nho(node);
    uint8_t *nh = node + nho;
    if (idx >= rd32(nh + NH_RECS_N)) return 0;
    uint32_t rel = rd32(node + nho + rd32(nh + NH_RECS_OFF) + idx * 4);
    return node + nho + rel;
}
static const uint8_t *rs_node_rec_c(const uint8_t *node, uint32_t idx) {
    return rs_node_rec((uint8_t *)node, idx);
}

/* 按 key 精确查找（16B 对象 ID key）；返回序号，-1=未找到 */
static int rs_node_find16(const uint8_t *node, uint64_t objid) {
    uint32_t n = rs_node_count(node);
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *rec = rs_node_rec_c(node, i);
        if (rd16(rec + NR_KEY_SIZE) != OT_KEY_SIZE) continue;
        if (rd64(rec + RS_NR_HDR + 8) == objid) return (int)i;
    }
    return -1;
}

/* 物理删除第 idx 条 record（后续 record 左移，偏移表重建） */
static int rs_node_remove_rec(uint8_t *node, uint32_t idx) {
    uint32_t nho = rs_node_nho(node);
    uint8_t *nh = node + nho;
    uint32_t n = rd32(nh + NH_RECS_N);
    uint32_t table_rel = rd32(nh + NH_RECS_OFF);
    if (idx >= n) return -1;

    uint8_t *victim = rs_node_rec(node, idx);
    uint32_t vsize = rd32(victim + NR_SIZE);
    uint32_t end = rd32(nh + NH_DATA_END);          /* 相对节点头 */
    uint32_t victim_rel = (uint32_t)(victim - node - nho);
    /* 左移 [victim+vsize, end) */
    rs_move(victim, victim + vsize, end - victim_rel - vsize);
    wr32(nh + NH_DATA_END, end - vsize);

    /* 偏移表重建（record 区自 32 起紧凑排列） */
    uint32_t rel = RS_NH_SIZE;
    for (uint32_t k = 0; k < n - 1; k++) {
        wr32(node + nho + table_rel + k * 4, rel);
        rel += rd32(node + nho + rel + NR_SIZE);
    }
    wr32(nh + NH_RECS_N, n - 1);
    wr32(nh + NH_RECS_END, table_rel + (n - 1) * 4);
    return 0;
}

/* 节点结构校验（严格：本驱动自产自销，异常即拒绝挂载） */
static int rs_node_check(const uint8_t *node, uint32_t area_size, uint32_t nho) {
    if (rd32(node + MN_NHO) != nho) return -1;
    if (rd16(node + TH_TABLE_OFF) != 40) return -1;
    const uint8_t *nh = node + nho;
    uint32_t ds = rd32(nh + NH_DATA_START);
    uint32_t de = rd32(nh + NH_DATA_END);
    uint32_t ro = rd32(nh + NH_RECS_OFF);
    uint32_t rn = rd32(nh + NH_RECS_N);
    uint32_t re = rd32(nh + NH_RECS_END);
    if (nh[NH_LEVEL] != 0 || nh[NH_FLAGS] != 0x02) return -1;
    if (ds != RS_NH_SIZE || de < ds) return -1;
    if (ro < RS_NH_SIZE || ro + 4 * rn > area_size - nho) return -1;
    if (re != ro + 4 * rn) return -1;
    if (de > ro) return -1;                        /* record 区在偏移表之前 */
    if (rn > RS_TABLE_CAP) return -1;
    uint32_t rel = ds;
    for (uint32_t k = 0; k < rn; k++) {
        uint32_t toff = rd32(nh + NH_RECS_OFF) + nho + k * 4;
        if (rd32(node + toff) != rel) return -1;   /* 紧凑排列不变式 */
        uint32_t sz = rd32(node + nho + rel + NR_SIZE);
        if (sz < RS_NR_HDR || rel + sz > de) return -1;
        uint16_t ko = rd16(node + nho + rel + NR_KEY_OFF);
        uint16_t ks = rd16(node + nho + rel + NR_KEY_SIZE);
        uint16_t vo = rd16(node + nho + rel + NR_VAL_OFF);
        uint16_t vs = rd16(node + nho + rel + NR_VAL_SIZE);
        if (ko != RS_NR_HDR || vo < RS_NR_HDR) return -1;
        if ((uint32_t)ko + ks > sz || (uint32_t)vo + vs > sz) return -1;
        rel += sz;
    }
    if (rel != de) return -1;
    return 0;
}

/* ---------- UTF-16LE 名字工具 ---------- */
static char rs_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}
/* 编码 entry key：0x0030 + 类型 + UTF-16LE 名。返回 key 大小 */
static uint32_t rs_name_encode(uint8_t *key, const char *name, uint16_t etype) {
    wr16(key + 0, 0x0030);
    wr16(key + DE_TYPE_OFF, etype);
    uint32_t l = 0;
    while (name[l] && l < RS_NAME_MAX) {
        wr16(key + DE_KEY_HDR + l * 2, (uint16_t)(uint8_t)name[l]);
        l++;
    }
    return DE_KEY_HDR + l * 2;
}
/* entry key 名字与 ASCII 名比较（大小写不敏感）；0=相等 */
static int rs_key_name_eq(const uint8_t *key, uint32_t ksize, const char *name) {
    uint32_t namesz = ksize - DE_KEY_HDR;
    uint32_t l = 0;
    while (name[l]) l++;
    if (l * 2 != namesz) return 1;
    for (uint32_t i = 0; i < l; i++) {
        uint16_t c = rd16(key + DE_KEY_HDR + i * 2);
        if (c >= 0x80) return 1;                    /* 非 ASCII 不匹配 */
        if ((char)rs_lower((char)c) != rs_lower(name[i])) return 1;
    }
    return 0;
}
/* 名字排序比较（key1 vs key2，大小写不敏感，升序） */
static int rs_key_name_cmp(const uint8_t *k1, uint32_t s1,
                           const uint8_t *k2, uint32_t s2) {
    uint32_t n1 = (s1 - DE_KEY_HDR) / 2;
    uint32_t n2 = (s2 - DE_KEY_HDR) / 2;
    uint32_t n = (n1 < n2) ? n1 : n2;
    for (uint32_t i = 0; i < n; i++) {
        uint16_t c1 = rd16(k1 + DE_KEY_HDR + i * 2);
        uint16_t c2 = rd16(k2 + DE_KEY_HDR + i * 2);
        char a = (c1 < 0x80) ? rs_lower((char)c1) : (char)0x7F;
        char b = (c2 < 0x80) ? rs_lower((char)c2) : (char)0x7F;
        if (a != b) return (a < b) ? -1 : 1;
    }
    if (n1 == n2) return 0;
    return (n1 < n2) ? -1 : 1;
}
/* entry key 名字 -> ASCII（有损转换，fs_dir_entry_t.name 用） */
static void rs_key_name_asc(const uint8_t *key, uint32_t ksize,
                            char *out, uint32_t cap) {
    uint32_t namesz = (ksize - DE_KEY_HDR) / 2;
    uint32_t l = 0;
    while (l < namesz && l + 1 < cap) {
        uint16_t c = rd16(key + DE_KEY_HDR + l * 2);
        out[l] = (c < 0x80) ? (char)c : '?';
        l++;
    }
    out[l] = 0;
}

/* ---------- 目录 entry 解析 ---------- */
typedef struct {
    uint8_t  type;      /* DE_TYPE_FILE / DE_TYPE_DIR */
    uint64_t objid;     /* 目录：子目录对象 ID */
    uint64_t size;      /* 文件：数据大小 */
    uint32_t dblk;      /* 文件：数据起始块号 */
    uint32_t nblk;      /* 文件：数据块数 */
} rs_ent_t;

/* 从 record value 解析 data run（单游程）；空文件 nblk=0 */
static int rs_fileval_parse(const uint8_t *val, uint32_t vsize,
                             uint64_t *size_out, uint32_t *dblk, uint32_t *nblk) {
    if (vsize < FV_VAL_SIZE) return -1;
    if (rs_node_nho(val) != FV_NHO) return -1;
    if (rs_node_check(val, vsize, FV_NHO) != 0) return -1;
    *size_out = rd64(val + FV_SIZE_OFF);

    /* 找 $DATA 非驻留属性 record（key[8]=0x80，flags 0x0008） */
    uint32_t n = rs_node_count(val);
    const uint8_t *nrat = 0;
    for (uint32_t k = 0; k < n; k++) {
        const uint8_t *rec = rs_node_rec_c(val, k);
        if (!(rd16(rec + NR_FLAGS) & 0x0008)) continue;
        if (rd32(rec + RS_NR_HDR + AT_TYPE_OFF) != 0x80) continue;
        uint32_t vs = rd16(rec + NR_VAL_SIZE);
        const uint8_t *av = rec + rd16(rec + NR_VAL_OFF);
        if (vs >= NRAT_VAL_SIZE && rs_node_nho(av) == NRAT_NHO) { nrat = av; break; }
    }
    if (!nrat) return -1;
    if (rs_node_check(nrat, NRAT_VAL_SIZE, NRAT_NHO) != 0) return -1;

    uint32_t dn = rs_node_count(nrat);
    if (dn == 0) { *dblk = 0; *nblk = 0; return 0; }   /* 空文件 */
    const uint8_t *rec = rs_node_rec_c(nrat, 0);
    if (rd16(rec + NR_VAL_SIZE) != DR_VAL_SIZE) return -1;
    const uint8_t *dr = rec + rd16(rec + NR_VAL_OFF);
    uint64_t nblocks = rd64(dr + 8);
    uint64_t phys = rd64(dr + 16);
    if (nblocks == 0 || nblocks >= RS_POOL_MAX || phys >= RS_POOL_MAX ||
        phys + nblocks > RS_POOL_MAX) return -1;
    *nblk = (uint32_t)nblocks;
    *dblk = (uint32_t)phys;
    return 0;
}

/* 解析 entry record -> rs_ent_t；非 entry（key!=0x0030）返回 -2 */
static int rs_ent_parse(const uint8_t *rec, rs_ent_t *e) {
    uint32_t ksize = rd16(rec + NR_KEY_SIZE);
    if (ksize < DE_KEY_HDR) return -2;
    if (rd16(rec + RS_NR_HDR) != 0x0030) return -2;
    e->type = (uint8_t)rd16(rec + RS_NR_HDR + DE_TYPE_OFF);
    const uint8_t *val = rec + rd16(rec + NR_VAL_OFF);
    uint32_t vsize = rd16(rec + NR_VAL_SIZE);
    if (e->type == DE_TYPE_DIR) {
        if (vsize != DV_SIZE) return -1;
        e->objid = rd64(val + DV_OBJID);
        return 0;
    }
    if (e->type != DE_TYPE_FILE) return -1;
    return rs_fileval_parse(val, vsize, &e->size, &e->dblk, &e->nblk);
}

/* ---------- objects tree 查找：对象 ID -> 目录节点块号 ---------- */
static int rs_obj_blk(const uint8_t *otree, uint64_t objid, uint32_t *blk_out) {
    int idx = rs_node_find16(otree, objid);
    if (idx < 0) return -1;
    const uint8_t *rec = rs_node_rec_c(otree, (uint32_t)idx);
    if (rd16(rec + NR_VAL_SIZE) < RS_BREF_SIZE) return -1;
    uint64_t b = rd64(rec + rd16(rec + NR_VAL_OFF) + BR_BLKNO);
    if (b == 0 || b >= RS_POOL_MAX) return -1;
    *blk_out = (uint32_t)b;
    return 0;
}

/* ---------- 嵌入值构造 ---------- */
/* 构造文件 entry value（612B）：file values + $DATA 非驻留属性 + data run */
static uint32_t rs_build_file_value(uint8_t *v, uint64_t size,
                                    uint32_t dblk, uint32_t nblk) {
    rs_zero(v, FV_VAL_SIZE);
    /* file-values 嵌入节点 */
    rs_node_init(v, FV_NHO, FV_TABLE_REL);
    uint8_t *fv = v + FV_HDR;
    wr64(fv + 0, RS_FILETIME);                 /* 创建时间 */
    wr64(fv + 8, RS_FILETIME);                 /* 修改时间 */
    wr64(fv + 16, RS_FILETIME);
    wr64(fv + 24, RS_FILETIME);
    wr32(fv + 32, 0x80);                       /* FILE_ATTRIBUTE_NORMAL */
    wr64(fv + 40, 0x100000 + dblk);            /* 文件系统标识低 64 位 */
    wr64(fv + 48, 0);                          /* 高 64 位 */
    wr64(fv + FV_SIZE_OFF - FV_HDR, size);     /* 数据大小（值内 104） */
    wr64(fv + FV_ALLOC_OFF - FV_HDR, (uint64_t)(nblk ? nblk : 0) * RS_META_BLK);

    /* 非驻留属性 value（380B）先构造于 612B 缓冲尾部再作为 record value 插入 */
    uint8_t *nrat = v + FV_VAL_SIZE - NRAT_VAL_SIZE;
    rs_zero(nrat, NRAT_VAL_SIZE);
    rs_node_init(nrat, NRAT_NHO, NRAT_TABLE_REL);
    uint8_t *nh_hdr = nrat + NRAT_HDR;
    wr32(nh_hdr + 0, 1);                       /* 非驻留标记 */
    wr64(nh_hdr + NRAT_ALLOC - NRAT_HDR, (uint64_t)(nblk ? nblk : 0) * RS_META_BLK);
    wr64(nh_hdr + NRAT_SIZE - NRAT_HDR, size);
    wr64(nh_hdr + NRAT_VALID - NRAT_HDR, size);
    if (nblk) {
        /* data run record：key = 值前 16B（逻辑 0 + 块数），value = 32B */
        uint8_t dr_val[DR_VAL_SIZE];
        rs_zero(dr_val, DR_VAL_SIZE);
        wr64(dr_val + 0, 0);
        wr64(dr_val + 8, nblk);
        wr64(dr_val + 16, dblk);
        uint8_t dr_key[DR_KEY_SIZE];
        rs_copy(dr_key, dr_val, DR_KEY_SIZE);
        if (rs_node_add_rec(nrat, NRAT_VAL_SIZE, NRAT_TABLE_REL,
                            dr_key, DR_KEY_SIZE, 0, dr_val, DR_VAL_SIZE) < 0)
            return 0;
    }

    /* $DATA 属性 record 插入 file-values 节点（flags 0x0008） */
    uint8_t at_key[AT_KEY_SIZE];
    rs_zero(at_key, AT_KEY_SIZE);
    wr32(at_key + AT_TYPE_OFF, 0x80);          /* $DATA */
    wr16(at_key + 12, 0);                      /* 无名属性 L'\0' */
    if (rs_node_add_rec(v, FV_VAL_SIZE, FV_TABLE_REL,
                        at_key, AT_KEY_SIZE, 0x0008, nrat, NRAT_VAL_SIZE) < 0)
        return 0;
    return FV_VAL_SIZE;
}

/* 构造目录 entry value（72B directory values） */
static void rs_build_dir_value(uint8_t *v, uint64_t objid) {
    rs_zero(v, DV_SIZE);
    wr64(v + DV_OBJID, objid);
    wr64(v + 16, RS_FILETIME);                 /* 创建时间 */
    wr64(v + 24, RS_FILETIME);
    wr64(v + 32, RS_FILETIME);
    wr64(v + 40, RS_FILETIME);
    wr32(v + 64, 0x10);                        /* FILE_ATTRIBUTE_DIRECTORY */
}

/* ---------- 挂载辅助：块级节点读取 + 校验 ---------- */
static int rs_load_node(uint32_t blkno, uint8_t *buf) {
    if (blkno == 0 || blkno >= RS_POOL_MAX) return -1;
    if (rs_read_block(blkno, buf) != 0) return -1;
    return rs_node_check(buf + RS_MBH_SIZE, RS_NODE_AREA, RS_NHO_BLOCK);
}
/* 读取后返回节点数据区指针（buf + 48） */
static uint8_t *rs_loaded_node(uint8_t *buf) { return buf + RS_MBH_SIZE; }

/* ---------- BFS 全卷扫描：恢复分配游标 + 占用统计 + 一致性校验 ----------
 * 活动世代最大块必为当前 objects tree 根（每次提交最后分配），故
 * hint = max(活块)+1 可安全覆盖孤儿块。 */
static int rs_scan_live(void) {
    uint32_t maxb = RS_ROOTDIR0_NO > RS_XTREE0_NO + 4 ? RS_ROOTDIR0_NO
                                                      : RS_XTREE0_NO + 4;
    uint32_t used = 1 /*SB*/ + 2 /*CP*/ + RS_NTREES;
    uint32_t qh = 0, qt = 0;
    rs_bfs_q[qt++] = OBJ_ROOT;

    /* objects tree 根在 rs_blk2（扫描期间保持） */
    if (rs_load_node(rs_otree_blk, rs_blk2) != 0) return -1;
    uint8_t *otree = rs_loaded_node(rs_blk2);

    while (qh < qt) {
        uint64_t objid = rs_bfs_q[qh++];
        uint32_t blk;
        if (rs_obj_blk(otree, objid, &blk) != 0) return -1;
        if (blk > maxb) maxb = blk;
        used++;
        if (rs_load_node(blk, rs_blk) != 0) return -1;
        uint8_t *node = rs_loaded_node(rs_blk);
        uint32_t n = rs_node_count(node);
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *rec = rs_node_rec_c(node, i);
            rs_ent_t ent;
            int r = rs_ent_parse(rec, &ent);
            if (r != 0) return -1;
            if (ent.type == DE_TYPE_DIR) {
                if (qt >= 256) return -1;   /* 队列满（上限 255 子目录） */
                rs_bfs_q[qt++] = ent.objid;
            } else {
                used += ent.nblk;
                if (ent.nblk) {
                    uint32_t top = ent.dblk + ent.nblk - 1;
                    if (top > maxb) maxb = top;
                }
            }
        }
    }
    rs_hint = maxb + 1;
    rs_used_blocks = used;
    return 0;
}

/* ---------- 挂载 ---------- */
int refs_mount(uint8_t drive, uint32_t part_start) {
    rs_mounted = 0;
    uint8_t vh[512];

    rs_drive = drive;
    rs_part_lba = part_start;
    if (ata_read_sector(drive, part_start, vh) != 0) return -1;

    /* 卷头签名（libfsrefs：0x03 "ReFS\0\0\0\0" + 0x10 "FSRS"） */
    if (vh[0x03] != 'R' || vh[0x04] != 'e' || vh[0x05] != 'F' ||
        vh[0x06] != 'S' || vh[0x07] != 0 || vh[0x08] != 0 || vh[0x09] != 0)
        return -1;
    if (vh[0x10] != 'F' || vh[0x11] != 'S' || vh[0x12] != 'R' || vh[0x13] != 'S')
        return -1;
    if (rd32(vh + RVH_SEC) != 512) return -1;
    if (rd32(vh + RVH_SPC) != 8) return -1;       /* 4KB 簇 */
    if (vh[RVH_MAJ] != 1 || vh[RVH_MIN] != 2) return -1;
    uint32_t nsecs = rd32(vh + RVH_NSECS);
    if (nsecs == 0 || nsecs > 0x10000000u) return -1;

    /* superblock @ 块 30 */
    if (rs_read_block(RS_SUPERBLOCK_NO, rs_blk) != 0) return -1;
    if (rd32(rs_blk + SB_CP_N) != 2) return -1;
    uint32_t cp_off = rd32(rs_blk + SB_CP_OFF);
    uint32_t self_off = rd32(rs_blk + SB_SELF_OFF);
    uint32_t self_size = rd32(rs_blk + SB_SELF_SIZE);
    if (cp_off < SB_BLK + 48 || cp_off + 16 > RS_META_BLK) return -1;
    if (self_off + RS_BREF_SIZE > RS_META_BLK || self_size != RS_BREF_SIZE) return -1;
    uint32_t cp_a = (uint32_t)rd64(rs_blk + cp_off);
    uint32_t cp_b = (uint32_t)rd64(rs_blk + cp_off + 8);
    if (cp_a == cp_b) return -1;

    /* 双 checkpoint：取序列号大者（libfsrefs 同规则） */
    uint64_t seq[2] = {0, 0};
    uint32_t trees[2][RS_NTREES];
    int ok[2] = {0, 0};
    uint32_t cpblk[2] = {cp_a, cp_b};
    for (int c = 0; c < 2; c++) {
        if (cpblk[c] == 0 || cpblk[c] >= RS_POOL_MAX) continue;
        if (rs_read_block(cpblk[c], rs_blk2) != 0) continue;
        uint8_t *cp = rs_blk2;
        if (rd16(cp + CPH_MAJ) != 1 || rd16(cp + CPH_MIN) != 2) continue;
        uint32_t cself = rd32(cp + CPH_SELF_OFF);
        if (cself + RS_BREF_SIZE > RS_META_BLK) continue;
        uint32_t noff = rd32(cp + CPT_NOFF);
        if (noff < 1 || noff > RS_NTREES) continue;
        if (CP_OFFARR + noff * 4 > cself) continue;
        int good = 1;
        for (uint32_t t = 0; t < noff; t++) {
            uint32_t boff = rd32(cp + CP_OFFARR + t * 4);
            if (boff < cself + RS_BREF_SIZE || boff + RS_BREF_SIZE > RS_META_BLK) {
                good = 0; break;
            }
            uint64_t b = rd64(cp + boff + BR_BLKNO);
            if (b == 0 || b >= RS_POOL_MAX || cp[boff + BR_CKTYPE] == 0 ||
                cp[boff + BR_CKTYPE] > 2) { good = 0; break; }
            trees[c][t] = (uint32_t)b;
        }
        if (!good) continue;
        for (uint32_t t = noff; t < RS_NTREES; t++) trees[c][t] = 0;
        seq[c] = rd64(cp + CPT_SEQ);
        ok[c] = 1;
    }
    int pick;
    if (ok[0] && ok[1]) pick = (seq[1] > seq[0]) ? 1 : 0;
    else if (ok[0]) pick = 0;
    else if (ok[1]) pick = 1;
    else return -1;

    rs_cp_no = cpblk[pick];
    rs_cp_seq = seq[pick];
    for (int t = 0; t < RS_NTREES; t++) rs_trees[t] = trees[pick][t];
    if (rs_trees[0] == 0 || rs_trees[0] >= RS_POOL_MAX) return -1;
    rs_otree_blk = rs_trees[0];

    /* objects tree -> 根目录对象 */
    if (rs_load_node(rs_otree_blk, rs_blk2) != 0) return -1;
    if (rs_obj_blk(rs_loaded_node(rs_blk2), OBJ_ROOT, &rs_root_dirblk) != 0)
        return -1;
    if (rs_load_node(rs_root_dirblk, rs_blk) != 0) return -1;

    /* 子目录对象 ID 游标 + 分配游标 + 占用（BFS 全卷校验） */
    rs_next_subid = OBJ_SUB_BASE;
    uint32_t n = rs_node_count(rs_loaded_node(rs_blk2));
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *rec = rs_node_rec_c(rs_loaded_node(rs_blk2), i);
        if (rd16(rec + NR_KEY_SIZE) != OT_KEY_SIZE) continue;
        uint64_t id = rd64(rec + RS_NR_HDR + 8);
        if (id >= rs_next_subid && id <= OBJ_SUB_MAX) rs_next_subid = id + 1;
    }
    if (rs_scan_live() != 0) return -1;

    rs_info.part_start = part_start;
    rs_info.bytes_per_sector = 512;
    rs_info.sectors_per_cluster = 8;
    rs_info.cluster_count = nsecs / 8 * 7 / 8 + nsecs / 8;   /* ~4KB 簇数 */
    rs_info.cluster_count = (nsecs * 512 / 4096) & ~0u;
    if (rs_info.cluster_count == 0) rs_info.cluster_count = 1;
    rs_info.volume_sectors = nsecs;
    rs_info.used_clusters = rs_used_blocks * 4;
    rs_mounted = 1;
    return 0;
}

const refs_info_t *refs_get_info(void) { return &rs_info; }

/* ---------- 路径解析 ---------- */
typedef struct {
    int      found;        /* 1=命中 */
    rs_ent_t ent;          /* 命中条目（type/size/dblk/nblk/objid） */
    uint32_t dir_blk;     /* 命中目录的节点块号（type=dir 有效） */
    uint32_t rec_idx;     /* 父目录内 record 序号 */
    uint32_t parent_blk;  /* 父目录节点块号 */
    uint64_t parent_objid;
} rs_hit_t;

static int rs_is_root_path(const char *path) {
    while (*path == '/') path++;
    return *path == 0;
}

/* 解析绝对路径。found=1 返回 0；不存在 -1。
 * 约定：命中后 rs_blk = 父目录节点内容（或目录自身内容在 dir_blk），
 * rs_blk2 = objects tree 根。 */
static int rs_walk(const char *path, rs_hit_t *hit) {
    if (!rs_mounted || path == 0 || path[0] != '/') return -1;
    hit->found = 0;
    if (rs_is_root_path(path)) {
        hit->found = 1;
        hit->ent.type = DE_TYPE_DIR;
        hit->ent.objid = OBJ_ROOT;
        hit->dir_blk = rs_root_dirblk;
        hit->parent_blk = 0;
        hit->parent_objid = 0;
        hit->rec_idx = 0;
        return 0;
    }
    if (rs_load_node(rs_otree_blk, rs_blk2) != 0) return -1;
    uint8_t *otree = rs_loaded_node(rs_blk2);

    uint64_t cur_obj = OBJ_ROOT;
    uint32_t cur_blk = rs_root_dirblk;
    const char *p = path;
    while (*p == '/') p++;

    while (*p) {
        const char *comp = p;
        uint32_t clen = 0;
        while (p[clen] && p[clen] != '/') clen++;
        if (clen == 0 || clen > RS_NAME_MAX) return -1;
        char name[RS_NAME_MAX + 1];
        for (uint32_t i = 0; i < clen; i++) name[i] = comp[i];
        name[clen] = 0;
        p += clen;
        while (*p == '/') p++;
        int last = (*p == 0);

        if (rs_load_node(cur_blk, rs_blk) != 0) return -1;
        uint8_t *node = rs_loaded_node(rs_blk);
        uint32_t n = rs_node_count(node);
        int found = -1;
        rs_ent_t ent;
        for (uint32_t i = 0; i < n; i++) {
            const uint8_t *rec = rs_node_rec_c(node, i);
            if (rd16(rec + NR_KEY_SIZE) < DE_KEY_HDR) continue;
            if (rd16(rec + RS_NR_HDR) != 0x0030) continue;
            if (rs_key_name_eq(rec + RS_NR_HDR,
                              rd16(rec + NR_KEY_SIZE), name) != 0) continue;
            if (rs_ent_parse(rec, &ent) != 0) return -1;
            found = (int)i;
            break;
        }
        if (found < 0) return -1;

        if (!last) {
            if (ent.type != DE_TYPE_DIR) return -1;
            if (rs_obj_blk(otree, ent.objid, &cur_blk) != 0) return -1;
            cur_obj = ent.objid;
            continue;
        }
        hit->found = 1;
        hit->ent = ent;
        hit->parent_blk = cur_blk;
        hit->parent_objid = cur_obj;
        hit->rec_idx = (uint32_t)found;
        hit->dir_blk = 0;
        if (ent.type == DE_TYPE_DIR) {
            if (rs_obj_blk(otree, ent.objid, &hit->dir_blk) != 0) return -1;
        }
        /* rs_blk 留存父目录节点内容（父=cur_blk） */
        return 0;
    }
    return -1;   /* 尾随 '/' 视为不存在 */
}

/* ---------- 对外读 API ---------- */
int refs_is_dir(const char *path) {
    if (!rs_mounted) return -1;
    if (rs_is_root_path(path)) return 1;
    rs_hit_t hit;
    if (rs_walk(path, &hit) != 0) return -1;
    return hit.ent.type == DE_TYPE_DIR;
}

uint32_t refs_get_file_size(const char *path) {
    rs_hit_t hit;
    if (rs_walk(path, &hit) != 0 || !hit.found) return 0;
    if (hit.ent.type == DE_TYPE_DIR) return 0;
    return (uint32_t)hit.ent.size;
}

int refs_read_file(const char *path, uint8_t *buffer, uint32_t max_size) {
    rs_hit_t hit;
    if (rs_walk(path, &hit) != 0 || !hit.found) return -1;
    if (hit.ent.type == DE_TYPE_DIR) return -1;

    uint64_t size = hit.ent.size;
    if (size > max_size) size = max_size;
    uint32_t done = 0;
    while (done < size) {
        uint32_t chunk = RS_META_BLK;
        if (chunk > size - done) chunk = (uint32_t)(size - done);
        uint32_t bi = done / RS_META_BLK;
        if (bi >= hit.ent.nblk) {
            /* 超出 data run 的洞读 0 */
            for (uint32_t i = 0; i < chunk; i++) buffer[done + i] = 0;
        } else {
            if (rs_read_block(hit.ent.dblk + bi, rs_databuf) != 0) return -1;
            for (uint32_t i = 0; i < chunk; i++)
                buffer[done + i] = rs_databuf[(done % RS_META_BLK) + i];
        }
        done += chunk;
    }
    return (int)size;
}

int refs_read_dir(const char *path, fs_dir_entry_t *entries, int max_entries) {
    rs_hit_t hit;
    if (rs_is_root_path(path)) {
        if (!rs_mounted) return -1;
        hit.dir_blk = rs_root_dirblk;
    } else {
        if (rs_walk(path, &hit) != 0 || !hit.found) return -1;
        if (hit.ent.type != DE_TYPE_DIR) return -1;
    }
    if (rs_load_node(hit.dir_blk, rs_blk) != 0) return -1;
    uint8_t *node = rs_loaded_node(rs_blk);
    uint32_t n = rs_node_count(node);

    int out = 0;
    for (uint32_t i = 0; i < n && out < max_entries; i++) {
        const uint8_t *rec = rs_node_rec_c(node, i);
        if (rd16(rec + NR_KEY_SIZE) < DE_KEY_HDR) continue;
        if (rd16(rec + RS_NR_HDR) != 0x0030) continue;
        rs_ent_t ent;
        if (rs_ent_parse(rec, &ent) != 0) continue;
        rs_key_name_asc(rec + RS_NR_HDR, rd16(rec + NR_KEY_SIZE),
                        entries[out].name, sizeof(entries[out].name));
        entries[out].is_dir = (ent.type == DE_TYPE_DIR);
        entries[out].size = (ent.type == DE_TYPE_DIR) ? 0 : (uint32_t)ent.size;
        out++;
    }
    return out;
}

uint32_t refs_get_file_clusters(const char *path) {
    rs_hit_t hit;
    if (rs_walk(path, &hit) != 0 || !hit.found) return 0;
    if (hit.ent.type == DE_TYPE_DIR) return 0;
    return hit.ent.nblk * 4;    /* 16KB 块 -> 4KB 簇 */
}

/* ---------- 提交（CoW + checkpoint 翻转） ----------
 * 前置：新目录节点内容已写入 new_dir_blk；objects tree 根在 rs_blk。
 * 就地更新父目录对象的 bref（记录大小不变），可选增删对象记录。
 * 分配新 objects tree 根块 + 翻转 CP。0=成功。 */
static int rs_commit(uint32_t new_dir_blk, uint64_t parent_obj,
                     int add_obj, uint64_t add_id, uint32_t add_blk,
                     int del_obj, uint64_t del_id) {
    uint8_t *otree = rs_loaded_node(rs_blk);   /* rs_blk = objects tree 根内容 */

    /* 父目录对象 bref 就地更新 */
    int pidx = rs_node_find16(otree, parent_obj);
    if (pidx < 0) return -1;
    uint8_t *prec = rs_node_rec(otree, (uint32_t)pidx);
    if (rd16(prec + NR_VAL_SIZE) < RS_BREF_SIZE) return -1;
    rs_fill_bref(prec + rd16(prec + NR_VAL_OFF), new_dir_blk);

    if (add_obj) {
        uint8_t key[OT_KEY_SIZE];
        uint8_t val[OT_VAL_SIZE];
        rs_zero(key, OT_KEY_SIZE);
        wr64(key + 8, add_id);
        rs_zero(val, OT_VAL_SIZE);
        rs_fill_bref(val, add_blk);
        if (rs_node_add_rec(otree, RS_NODE_AREA, RS_TABLE_REL,
                            key, OT_KEY_SIZE, 0, val, OT_VAL_SIZE) < 0)
            return -1;
    }
    if (del_obj) {
        int didx = rs_node_find16(otree, del_id);
        if (didx < 0) return -1;
        if (rs_node_remove_rec(otree, (uint32_t)didx) != 0) return -1;
    }
    if (rs_node_check(otree, RS_NODE_AREA, RS_NHO_BLOCK) != 0) return -1;

    uint32_t new_otree = rs_alloc();
    if (new_otree == 0) return -1;
    if (rs_write_block(new_otree, rs_blk) != 0) return -1;

    /* 翻转 CP：内容同构，seq+1，树 0 -> 新根 */
    uint32_t next_cp = (rs_cp_no == RS_CPA_NO) ? RS_CPB_NO : RS_CPA_NO;
    uint64_t next_seq = rs_cp_seq + 1;
    uint8_t *cp = rs_databuf;
    rs_fill_mbh(cp, next_cp, next_seq);
    wr32(cp + CP_BLK + 0, 0);
    wr16(cp + CPH_MAJ, 1);
    wr16(cp + CPH_MIN, 2);
    wr32(cp + CPH_SELF_OFF, CP_SELF_BREF);
    wr32(cp + CPH_SELF_SIZE, RS_BREF_SIZE);
    wr64(cp + CPT_SEQ, next_seq);
    wr32(cp + CPT_SIZE, CP_TREE_BREF + RS_NTREES * RS_BREF_SIZE);
    wr32(cp + CPT_UNK4, 32);
    wr32(cp + CPT_NOFF, RS_NTREES);
    for (uint32_t t = 0; t < RS_NTREES; t++)
        wr32(cp + CP_OFFARR + t * 4, CP_TREE_BREF + t * RS_BREF_SIZE);
    rs_fill_bref(cp + CP_SELF_BREF, next_cp);
    uint32_t trees_new[RS_NTREES];
    trees_new[0] = new_otree;
    for (uint32_t t = 1; t < RS_NTREES; t++) trees_new[t] = rs_trees[t];
    for (uint32_t t = 0; t < RS_NTREES; t++)
        rs_fill_bref(cp + CP_TREE_BREF + t * RS_BREF_SIZE, trees_new[t]);
    if (rs_write_block(next_cp, cp) != 0) return -1;

    rs_cp_no = next_cp;
    rs_cp_seq = next_seq;
    for (uint32_t t = 0; t < RS_NTREES; t++) rs_trees[t] = trees_new[t];
    rs_otree_blk = new_otree;
    if (parent_obj == OBJ_ROOT) rs_root_dirblk = new_dir_blk;
    rs_used_blocks += 2;    /* 新目录节点 + 新 objects tree 根 */
    return 0;
}

/* ---------- 写路径：拆分父路径 + 定位父目录 ----------
 * 成功后 rs_blk = 父目录节点内容，rs_blk2 = objects tree 根。 */
static int rs_split_parent(const char *path, uint64_t *parent_obj,
                           uint32_t *parent_blk, char *fname) {
    if (!rs_mounted || path == 0 || path[0] != '/') return -1;
    const char *p = path;
    while (*p == '/') p++;
    if (!*p) return -1;

    const char *slash = 0;
    const char *q = p;
    while (*q) { if (*q == '/') slash = q; q++; }
    if (slash == p - 1) return -1;
    const char *n = slash ? slash + 1 : p;
    uint32_t nlen = 0;
    while (n[nlen] && n[nlen] != '/') nlen++;
    if (nlen == 0 || nlen > RS_NAME_MAX) return -1;
    for (uint32_t i = 0; i < nlen; i++) fname[i] = n[i];
    fname[nlen] = 0;
    if (slash && slash[1] && n + nlen != q) return -1;   /* 名后再带 '/' */
    if (slash == p && !slash[1]) return -1;

    /* 父路径 = 前缀（无则根） */
    if (rs_load_node(rs_otree_blk, rs_blk2) != 0) return -1;
    uint8_t *otree = rs_loaded_node(rs_blk2);
    uint64_t cur_obj = OBJ_ROOT;
    uint32_t cur_blk = rs_root_dirblk;

    const char *pp = path;
    const char *pend = slash ? slash : p + 0;
    /* 逐组件解析到父目录 */
    while (pp < pend) {
        while (pp < pend && *pp == '/') pp++;
        if (pp >= pend) break;
        const char *comp = pp;
        uint32_t clen = 0;
        while (pp < pend && *pp != '/') { pp++; clen++; }
        if (clen == 0 || clen > RS_NAME_MAX) return -1;
        char name[RS_NAME_MAX + 1];
        for (uint32_t i = 0; i < clen; i++) name[i] = comp[i];
        name[clen] = 0;

        if (rs_load_node(cur_blk, rs_blk) != 0) return -1;
        uint8_t *node = rs_loaded_node(rs_blk);
        uint32_t n2 = rs_node_count(node);
        int found = -1;
        uint64_t sub_obj = 0;
        for (uint32_t i = 0; i < n2; i++) {
            const uint8_t *rec = rs_node_rec_c(node, i);
            if (rd16(rec + NR_KEY_SIZE) < DE_KEY_HDR) continue;
            if (rd16(rec + RS_NR_HDR) != 0x0030) continue;
            if (rs_key_name_eq(rec + RS_NR_HDR, rd16(rec + NR_KEY_SIZE), name) != 0)
                continue;
            rs_ent_t ent;
            if (rs_ent_parse(rec, &ent) != 0) return -1;
            if (ent.type != DE_TYPE_DIR) return -1;
            found = 1;
            sub_obj = ent.objid;
            break;
        }
        if (found < 0) return -1;
        if (rs_obj_blk(otree, sub_obj, &cur_blk) != 0) return -1;
        cur_obj = sub_obj;
    }
    /* 末次：父目录节点内容进 rs_blk */
    if (rs_load_node(cur_blk, rs_blk) != 0) return -1;
    *parent_obj = cur_obj;
    *parent_blk = cur_blk;
    return 0;
}

/* 在父目录节点（rs_blk）中找 fname；返回 record 序号，-1=无 */
static int rs_dir_find(uint8_t *node, const char *fname) {
    uint32_t n = rs_node_count(node);
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *rec = rs_node_rec_c(node, i);
        if (rd16(rec + NR_KEY_SIZE) < DE_KEY_HDR) continue;
        if (rd16(rec + RS_NR_HDR) != 0x0030) continue;
        if (rs_key_name_eq(rec + RS_NR_HDR, rd16(rec + NR_KEY_SIZE), fname) == 0)
            return (int)i;
    }
    return -1;
}

/* 按名字序插入 entry record（新内容缓冲 dst 已含父节点副本） */
static int rs_dir_insert_sorted(uint8_t *dst, const uint8_t *key, uint32_t ksize,
                                uint16_t flags, const uint8_t *value, uint32_t vsize) {
    uint32_t nho = rs_node_nho(dst);
    uint8_t *nh = dst + nho;
    uint32_t n = rd32(nh + NH_RECS_N);
    uint32_t table_rel = rd32(nh + NH_RECS_OFF);
    uint32_t end = rd32(nh + NH_DATA_END);
    uint32_t total = RS_NR_HDR + ksize + vsize;
    if (n >= RS_TABLE_CAP || end + total > table_rel) return -1;
    if (table_rel + (n + 1) * 4 > RS_NODE_AREA - nho) return -1;

    /* 找插入位置（首个名字大于新名的 record） */
    uint32_t insert_rel = end;
    for (uint32_t k = 0; k < n; k++) {
        const uint8_t *rec = rs_node_rec_c(dst, k);
        if (rd16(rec + NR_KEY_SIZE) < DE_KEY_HDR) continue;
        if (rd16(rec + RS_NR_HDR) != 0x0030) continue;
        if (rs_key_name_cmp(key, ksize, rec + RS_NR_HDR,
                            rd16(rec + NR_KEY_SIZE)) < 0) {
            insert_rel = rd32(dst + nho + table_rel + k * 4);
            break;
        }
    }
    /* 后移 record 区 [insert_rel, end) */
    rs_move(dst + nho + insert_rel + total, dst + nho + insert_rel,
            end - insert_rel);
    /* 写新 record */
    uint8_t *rec = dst + nho + insert_rel;
    wr32(rec + NR_SIZE, total);
    wr16(rec + NR_KEY_OFF, RS_NR_HDR);
    wr16(rec + NR_KEY_SIZE, (uint16_t)ksize);
    wr16(rec + NR_FLAGS, flags);
    wr16(rec + NR_VAL_OFF, RS_NR_HDR + (uint16_t)ksize);
    wr16(rec + NR_VAL_SIZE, (uint16_t)vsize);
    rs_copy(rec + RS_NR_HDR, key, ksize);
    rs_copy(rec + RS_NR_HDR + ksize, value, vsize);
    wr32(nh + NH_DATA_END, end + total);

    /* 重建偏移表（record 区自 32 紧凑） */
    uint32_t rel = RS_NH_SIZE;
    for (uint32_t k = 0; k <= n; k++) {
        wr32(dst + nho + table_rel + k * 4, rel);
        rel += rd32(dst + nho + rel + NR_SIZE);
    }
    wr32(nh + NH_RECS_N, n + 1);
    wr32(nh + NH_RECS_END, table_rel + (n + 1) * 4);
    return 0;
}

/* ---------- 写 API ---------- */
int refs_create_file(const char *path, const uint8_t *data, uint32_t size) {
    uint64_t parent_obj;
    uint32_t parent_blk;
    char fname[RS_NAME_MAX + 1];
    if (rs_split_parent(path, &parent_obj, &parent_blk, fname) != 0) return -1;
    uint8_t *pnode = rs_loaded_node(rs_blk);

    /* create-or-replace：同名目录拒绝；同名文件移除后重建 */
    int old = rs_dir_find(pnode, fname);
    if (old >= 0) {
        const uint8_t *rec = rs_node_rec_c(pnode, (uint32_t)old);
        rs_ent_t ent;
        if (rs_ent_parse(rec, &ent) != 0) return -1;
        if (ent.type == DE_TYPE_DIR) return -1;
    }

    /* 数据块（16KB 粒度，连续 bump 分配） */
    uint32_t nblk = (size + RS_META_BLK - 1) / RS_META_BLK;
    uint32_t dblk = 0;
    if (nblk) {
        dblk = rs_hint;
        if (dblk + nblk > RS_POOL_MAX) return -1;
        for (uint32_t b = 0; b < nblk; b++) {
            rs_zero(rs_databuf, RS_META_BLK);
            uint32_t off = b * RS_META_BLK;
            uint32_t chunk = (size - off > RS_META_BLK) ? RS_META_BLK
                                                         : size - off;
            for (uint32_t i = 0; i < chunk; i++) rs_databuf[i] = data[off + i];
            if (rs_write_block(dblk + b, rs_databuf) != 0) return -1;
        }
        rs_hint += nblk;
        rs_used_blocks += nblk;
    }

    /* 文件 entry value（612B） */
    uint32_t vsize = rs_build_file_value(rs_databuf, size, dblk, nblk);
    if (vsize == 0) return -1;

    /* 新父目录内容（rs_blk2 = 副本 - 旧条目 + 新条目） */
    rs_copy(rs_blk2, rs_blk, RS_META_BLK);
    uint8_t *nnode = rs_loaded_node(rs_blk2);
    if (old >= 0 && rs_node_remove_rec(nnode, (uint32_t)old) != 0) return -1;
    uint8_t key[DE_KEY_HDR + RS_NAME_MAX * 2];
    uint32_t ksize = rs_name_encode(key, fname, DE_TYPE_FILE);
    if (rs_dir_insert_sorted(nnode, key, ksize, 0x0008,
                             rs_databuf, vsize) != 0) return -1;
    if (rs_node_check(nnode, RS_NODE_AREA, RS_NHO_BLOCK) != 0) return -1;

    uint32_t new_dir = rs_alloc();
    if (new_dir == 0) return -1;
    if (rs_write_block(new_dir, rs_blk2) != 0) return -1;

    /* objects tree：rs_blk 重读根，父 bref -> 新目录块 */
    if (rs_load_node(rs_otree_blk, rs_blk) != 0) return -1;
    if (rs_commit(new_dir, parent_obj, 0, 0, 0, 0, 0) != 0) return -1;
    rs_info.used_clusters = rs_used_blocks * 4;
    return 0;
}

int refs_mkdir(const char *path) {
    uint64_t parent_obj;
    uint32_t parent_blk;
    char fname[RS_NAME_MAX + 1];
    if (rs_split_parent(path, &parent_obj, &parent_blk, fname) != 0) return -1;
    uint8_t *pnode = rs_loaded_node(rs_blk);
    if (rs_dir_find(pnode, fname) >= 0) return -1;      /* 已存在 */

    if (rs_next_subid > OBJ_SUB_MAX) return -1;
    uint64_t subid = rs_next_subid++;

    /* 新目录对象节点（空叶） */
    rs_fill_mbh(rs_databuf, 0, 1);          /* 块号后补 */
    rs_node_init(rs_loaded_node(rs_databuf), RS_NHO_BLOCK, RS_TABLE_REL);
    uint32_t sub_blk = rs_alloc();
    if (sub_blk == 0) return -1;
    wr64(rs_databuf + MBH_BLKNO, sub_blk);
    if (rs_write_block(sub_blk, rs_databuf) != 0) return -1;
    rs_used_blocks++;

    /* entry value（72B directory values） */
    uint8_t dval[DV_SIZE];
    rs_build_dir_value(dval, subid);

    /* 新父目录内容 */
    rs_copy(rs_blk2, rs_blk, RS_META_BLK);
    uint8_t *nnode = rs_loaded_node(rs_blk2);
    uint8_t key[DE_KEY_HDR + RS_NAME_MAX * 2];
    uint32_t ksize = rs_name_encode(key, fname, DE_TYPE_DIR);
    if (rs_dir_insert_sorted(nnode, key, ksize, 0, dval, DV_SIZE) != 0)
        return -1;
    if (rs_node_check(nnode, RS_NODE_AREA, RS_NHO_BLOCK) != 0) return -1;
    uint32_t new_dir = rs_alloc();
    if (new_dir == 0) return -1;
    if (rs_write_block(new_dir, rs_blk2) != 0) return -1;

    /* objects tree：+对象记录 + 父 bref 更新 */
    if (rs_load_node(rs_otree_blk, rs_blk) != 0) return -1;
    if (rs_commit(new_dir, parent_obj, 1, subid, sub_blk, 0, 0) != 0)
        return -1;
    rs_info.used_clusters = rs_used_blocks * 4;
    return 0;
}

int refs_delete_file(const char *path) {
    rs_hit_t hit;
    if (rs_is_root_path(path) || rs_walk(path, &hit) != 0 || !hit.found)
        return -1;
    /* rs_walk 约定已将 rs_blk 留作父目录内容 */

    if (hit.ent.type == DE_TYPE_DIR) {
        /* 目录须为空 */
        if (rs_load_node(hit.dir_blk, rs_blk2) != 0) return -1;
        if (rs_node_count(rs_loaded_node(rs_blk2)) != 0) return -1;
    }

    /* 新父目录内容（移除条目） */
    rs_copy(rs_blk2, rs_blk, RS_META_BLK);
    uint8_t *nnode = rs_loaded_node(rs_blk2);
    if (rs_node_remove_rec(nnode, hit.rec_idx) != 0) return -1;
    if (rs_node_check(nnode, RS_NODE_AREA, RS_NHO_BLOCK) != 0) return -1;
    uint32_t new_dir = rs_alloc();
    if (new_dir == 0) return -1;
    if (rs_write_block(new_dir, rs_blk2) != 0) return -1;

    /* objects tree：父 bref 更新（目录另删对象记录） */
    if (rs_load_node(rs_otree_blk, rs_blk) != 0) return -1;
    int del_obj = (hit.ent.type == DE_TYPE_DIR);
    if (rs_commit(new_dir, hit.parent_objid, 0, 0, 0,
                  del_obj, hit.ent.objid) != 0) return -1;
    rs_info.used_clusters = rs_used_blocks * 4;
    return 0;
}

/* ---------- 格式化 ---------- */
int refs_format(uint8_t drive) {
    if (drive > 3) return -1;
    const uint32_t part_start = 1;

    rs_drive = drive;
    rs_part_lba = part_start;
    rs_mounted = 0;

    /* MBR：分区 0x07 @LBA1，32767 扇区 */
    static uint8_t mbr[512] RS_HIBUF;
    rs_zero(mbr, 512);
    mbr[446 + 4] = 0x07;
    wr32(mbr + 446 + 8, part_start);
    wr32(mbr + 446 + 12, RS_VOL_SECS);
    mbr[510] = 0x55; mbr[511] = 0xAA;
    if (ata_write_sector(drive, 0, mbr) != 0) return -1;

    /* 卷头（FSRS + 布局参数，ReFS 1.2） */
    static uint8_t vh[512] RS_HIBUF;
    rs_zero(vh, 512);
    vh[0x00] = 0; vh[0x01] = 0; vh[0x02] = 0;
    vh[0x03] = 'R'; vh[0x04] = 'e'; vh[0x05] = 'F'; vh[0x06] = 'S';
    vh[0x07] = 0; vh[0x08] = 0; vh[0x09] = 0;
    vh[0x10] = 'F'; vh[0x11] = 'S'; vh[0x12] = 'R'; vh[0x13] = 'S';
    wr16(vh + 0x14, 0x0200);
    wr16(vh + 0x16, rs_fsrs_checksum(vh));
    wr64(vh + RVH_NSECS, RS_NSECS_FIELD);
    wr32(vh + RVH_SEC, 512);
    wr32(vh + RVH_SPC, 8);                    /* 4KB 簇 */
    vh[RVH_MAJ] = 1; vh[RVH_MIN] = 2;         /* ReFS 1.2 */
    wr64(vh + RVH_SERIAL, 0x455A4F5352454653ull);   /* "EZOSREFS" */
    if (ata_write_sector(drive, part_start, vh) != 0) return -1;
    /* 卷头副本（卷尾，libfsrefs 惯例） */
    if (ata_write_sector(drive, part_start + RS_VOL_SECS - 1, vh) != 0)
        return -1;

    /* superblock @ 块 30 */
    rs_fill_mbh(rs_blk, RS_SUPERBLOCK_NO, 1);
    for (int i = 0; i < 16; i++) rs_blk[SB_VOLID + i] = vh[RVH_SERIAL + (i % 8)];
    wr64(rs_blk + SB_SEQ, 1);
    wr32(rs_blk + SB_CP_OFF, SB_CPARR);
    wr32(rs_blk + SB_CP_N, 2);
    wr32(rs_blk + SB_SELF_OFF, SB_SELF_BREF);
    wr32(rs_blk + SB_SELF_SIZE, RS_BREF_SIZE);
    wr64(rs_blk + SB_CPARR + 0, RS_CPA_NO);
    wr64(rs_blk + SB_CPARR + 8, RS_CPB_NO);
    rs_fill_bref(rs_blk + SB_SELF_BREF, RS_SUPERBLOCK_NO);
    if (rs_write_block(RS_SUPERBLOCK_NO, rs_blk) != 0) return -1;

    /* 树 0..5 根节点：块 33..38（树 0 含 0x600 -> 根目录块 39） */
    for (uint32_t t = 0; t < RS_NTREES; t++) {
        uint32_t tb = RS_OTREE0_NO + t;
        rs_fill_mbh(rs_blk, tb, 1);
        rs_node_init(rs_loaded_node(rs_blk), RS_NHO_BLOCK, RS_TABLE_REL);
        if (t == 0) {
            uint8_t key[OT_KEY_SIZE];
            uint8_t val[OT_VAL_SIZE];
            rs_zero(key, OT_KEY_SIZE);
            wr64(key + 8, OBJ_ROOT);
            rs_zero(val, OT_VAL_SIZE);
            rs_fill_bref(val, RS_ROOTDIR0_NO);
            if (rs_node_add_rec(rs_loaded_node(rs_blk), RS_NODE_AREA,
                                RS_TABLE_REL, key, OT_KEY_SIZE, 0,
                                val, OT_VAL_SIZE) < 0) return -1;
        }
        if (rs_write_block(tb, rs_blk) != 0) return -1;
    }

    /* 根目录对象节点 @ 块 39（空叶） */
    rs_fill_mbh(rs_blk, RS_ROOTDIR0_NO, 1);
    rs_node_init(rs_loaded_node(rs_blk), RS_NHO_BLOCK, RS_TABLE_REL);
    if (rs_write_block(RS_ROOTDIR0_NO, rs_blk) != 0) return -1;

    /* checkpoint A/B：A 为活动（seq=1） */
    for (int c = 0; c < 2; c++) {
        uint32_t cb = (c == 0) ? RS_CPA_NO : RS_CPB_NO;
        uint64_t cseq = (c == 0) ? 1 : 0;
        rs_fill_mbh(rs_blk, cb, cseq);
        wr32(rs_blk + CP_BLK + 0, 0);
        wr16(rs_blk + CPH_MAJ, 1);
        wr16(rs_blk + CPH_MIN, 2);
        wr32(rs_blk + CPH_SELF_OFF, CP_SELF_BREF);
        wr32(rs_blk + CPH_SELF_SIZE, RS_BREF_SIZE);
        wr64(rs_blk + CPT_SEQ, cseq);
        wr32(rs_blk + CPT_SIZE, CP_TREE_BREF + RS_NTREES * RS_BREF_SIZE);
        wr32(rs_blk + CPT_UNK4, 32);
        wr32(rs_blk + CPT_NOFF, RS_NTREES);
        for (uint32_t t = 0; t < RS_NTREES; t++)
            wr32(rs_blk + CP_OFFARR + t * 4,
                 CP_TREE_BREF + t * RS_BREF_SIZE);
        rs_fill_bref(rs_blk + CP_SELF_BREF, cb);
        for (uint32_t t = 0; t < RS_NTREES; t++)
            rs_fill_bref(rs_blk + CP_TREE_BREF + t * RS_BREF_SIZE,
                         RS_OTREE0_NO + t);
        if (rs_write_block(cb, rs_blk) != 0) return -1;
    }

    /* 挂载验证（同时恢复运行期状态） */
    if (refs_mount(drive, part_start) != 0) return -1;
    return 0;
}
