/*
 * erofs.c - EROFS（Enhanced ROM File System）只读驱动
 *
 * 支持范围（只读）：
 *   - 块大小 512..4096（blkszbits 9..12）
 *   - compact(32B) / extended(64B) inode
 *   - FLAT_PLAIN / FLAT_INLINE（尾部打包）/ CHUNK_BASED（4B 数组与 8B 索引）布局
 *   - 压缩文件：COMPRESSED_FULL（legacy 8B 索引）与 COMPRESSED_COMPACT
 *     （compacted 4B/2B 索引），LZ4 block 与 DEFLATE(raw inflate) 解压，
 *     支持 BIG_PCLUSTER（CBLKCNT）变体
 *   - 目录：dirent 数组 + 名字区（"." / ".." 跳过）
 *   - 不支持：fragment / ztailpacking / interlaced / LZMA / 多设备
 *
 * refs:
 *   - Linux fs/erofs/erofs_fs.h（GPL-2.0-only OR Apache-2.0）- 磁盘结构定义
 *   - Linux fs/erofs/{inode.c,data.c,zmap.c,namei.c,super.c} - 布局映射规则
 *   - U-Boot fs/erofs/zmap.c (GPL-2.0+) - 压缩索引解码参考实现
 *   - Mark Adler puff.c (zlib license) - DEFLATE inflate 参考实现
 *   - LZ4 Block Format spec - LZ4 块解压
 */
#include "erofs.h"
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

/* ---------- superblock 字段偏移（Linux erofs_fs.h struct erofs_super_block） ---------- */
#define SB_OFF_MAGIC          0     /* 0xE0F5E1E2 */
#define SB_OFF_CHECKSUM       4     /* crc32c(super_block[0:128], checksum 清零) */
#define SB_OFF_FEAT_COMPAT    8
#define SB_OFF_BLKSZBITS      12
#define SB_OFF_ROOT_NID       14
#define SB_OFF_INOS           16    /* LE64 */
#define SB_OFF_BLOCKS         36    /* 已用块数（statfs） */
#define SB_OFF_META_BLKADDR   40    /* 元数据区起始块 */
#define SB_OFF_FEAT_INCOMPAT  80
#define SB_OFF_EXTRA_DEVICES  86

#define EROFS_SUPER_OFFSET    1024
#define EROFS_MAGIC           0xE0F5E1E2u
#define EROFS_SB_COMPAT_CHKSUM   0x00000001u
/* 可安全忽略的 incompat 位：ZERO_PADDING(1) COMPR_CFGS/BIG_PCLUSTER(2)
 * CHUNKED_FILE(4) DEVICE_TABLE/COMPR_HEAD2(8，extra_devices==0 时无害) */
#define EROFS_SB_INCOMPAT_OK     0x0000000Fu

/* ---------- inode 字段偏移 ---------- */
#define INO_OFF_FORMAT        0     /* bit0=version bit1-3=datalayout */
#define INO_OFF_XATTR_ICOUNT  2
#define INO_OFF_MODE          4
#define INO_OFF_C_SIZE        8     /* compact i_size LE32 */
#define INO_OFF_C_IU          16
#define INO_OFF_E_SIZE        8     /* extended i_size LE64 */
#define INO_OFF_E_IU          16

#define EROFS_INODE_FLAT_PLAIN          0
#define EROFS_INODE_COMPRESSED_FULL     1
#define EROFS_INODE_FLAT_INLINE         2
#define EROFS_INODE_COMPRESSED_COMPACT  3
#define EROFS_INODE_CHUNK_BASED         4

#define EROFS_FT_REG_FILE  1
#define EROFS_FT_DIR       2

#define EROFS_NULL_ADDR    0xFFFFFFFFu

/* ---------- dirent（12 字节） ---------- */
#define DIRENT_SIZE        12
#define DIRENT_OFF_NID     0
#define DIRENT_OFF_NAMEOFF 8
#define DIRENT_OFF_TYPE    10

/* ---------- z_erofs 压缩索引（Linux erofs_fs.h / U-Boot zmap.c） ---------- */
#define Z_LTYPE_PLAIN   0
#define Z_LTYPE_HEAD1   1
#define Z_LTYPE_NONHEAD 2
#define Z_LTYPE_HEAD2   3

#define Z_D0_CBLKCNT            0x0800u   /* advise bit11 / compacted lo bit11 */
#define Z_ADVISE_COMPACTED_2B   0x0001u
#define Z_ADVISE_BIG_PCLUSTER_1 0x0002u
#define Z_ADVISE_BIG_PCLUSTER_2 0x0004u
#define Z_ADVISE_INLINE_PCLUSTER 0x0008u
#define Z_ADVISE_INTERLACED     0x0010u
#define Z_ADVISE_FRAGMENT_PCLUSTER 0x0020u

#define Z_COMPR_LZ4        0
#define Z_COMPR_LZMA       1
#define Z_COMPR_DEFLATE    2
#define Z_COMPR_SHIFTED    4

#define ZMAP_MAPPED        0x1

/* ---------- 挂载状态 ---------- */
static uint8_t  er_drive;
static uint8_t  er_mounted;
static uint32_t er_part_lba;
static uint32_t er_blkszbits;
static uint32_t er_blksz;
static uint32_t er_meta_blkaddr;
static uint64_t er_meta_off;              /* meta_blkaddr << blkszbits（卷内字节偏移） */
static uint32_t er_root_nid;
static uint64_t er_inos;
static uint32_t er_blocks;
static erofs_info_t er_info;

/* ---------- inode 载入结果 ---------- */
typedef struct {
    uint32_t nid;
    uint64_t iloc;            /* inode 卷内字节偏移 */
    uint16_t mode;
    uint64_t size;
    uint8_t  datalayout;
    uint32_t inode_isize;     /* 32 / 64 */
    uint32_t xattr_isize;     /* i_xattr_icount ? 12+(icount-1)*4 : 0 */
    uint32_t i_u;             /* raw_blkaddr 或 chunk_info */
    /* 压缩惰性字段（首次读取时填充） */
    uint16_t z_advise;
    uint8_t  z_alg[2];
    uint32_t z_lclusterbits;
    uint8_t  z_inited;
} er_inode_t;

/* ---------- crc32c（Castagnoli，SB 校验） ---------- */
static uint32_t er_crc32c(uint32_t crc, const uint8_t *p, uint32_t len) {
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0x82F63B78u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* ---------- 卷内字节偏移读取（任意长度，处理非对齐） ---------- */
static uint8_t er_secbuf[512];

static int er_read_bytes(uint64_t off, uint8_t *buf, uint32_t len) {
    uint32_t done = 0;
    while (done < len) {
        uint32_t lba = er_part_lba + (uint32_t)(off >> 9);
        uint32_t in = (uint32_t)(off & 511);
        uint32_t chunk = 512 - in;
        if (chunk > len - done) chunk = len - done;
        if (ata_read_sector(er_drive, lba, er_secbuf) != 0) return -1;
        for (uint32_t i = 0; i < chunk; i++) buf[done + i] = er_secbuf[in + i];
        off += chunk;
        done += chunk;
    }
    return 0;
}

static int er_read_blk(uint32_t blk, uint8_t *buf) {
    return er_read_bytes((uint64_t)blk << er_blkszbits, buf, er_blksz);
}

/* ---------- inode 载入 ---------- */
static int er_load_inode(uint32_t nid, er_inode_t *ino) {
    ino->nid = nid;
    ino->z_inited = 0;
    ino->iloc = er_meta_off + ((uint64_t)nid << 5);
    uint8_t hdr[32];
    if (er_read_bytes(ino->iloc, hdr, 32) != 0) return -1;
    uint16_t fmt = rd16(hdr);
    ino->datalayout = (uint8_t)((fmt >> 1) & 7);
    uint16_t icnt = rd16(hdr + 2);
    ino->xattr_isize = icnt ? 12 + (uint32_t)(icnt - 1) * 4 : 0;
    ino->mode = rd16(hdr + 4);
    if (fmt & 1) {                    /* extended：64 字节 */
        ino->inode_isize = 64;
        uint8_t ext[64];
        if (er_read_bytes(ino->iloc, ext, 64) != 0) return -1;
        ino->size = rd64(ext + 8);
        ino->i_u = rd32(ext + 16);
    } else {                          /* compact：32 字节 */
        ino->inode_isize = 32;
        ino->size = rd32(hdr + 8);
        ino->i_u = rd32(hdr + 16);
    }
    if (ino->size > 0x40000000u) return -1;    /* 1GB 保护上限 */
    return 0;
}

/* ============ DEFLATE inflate（raw stream，puff.c 风格） ============ */

typedef struct {
    uint16_t count[16];
    uint16_t symbol[288];
} er_huff_t;

static int er_huff_build(er_huff_t *h, const uint8_t *lengths, int n) {
    for (int i = 0; i < 16; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) h->count[lengths[i]]++;
    if (h->count[0] == n) return -1;
    int left = 1;
    for (int len = 1; len <= 15; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;
    }
    uint16_t offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + h->count[len];
    for (int i = 0; i < n; i++)
        if (lengths[i]) h->symbol[offs[lengths[i]]++] = (uint16_t)i;
    return 0;
}

static int er_huff_decode(const er_huff_t *h, const uint8_t *src,
                          uint32_t srclen, uint32_t *bitpos) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        int bit = 0;
        if (*bitpos < srclen * 8)
            bit = (src[*bitpos >> 3] >> (*bitpos & 7)) & 1;
        (*bitpos)++;
        code |= bit;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const uint16_t er_lbase[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,
    67,83,99,115,131,163,195,227,258};
static const uint8_t er_lext[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const uint16_t er_dbase[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
    1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const uint8_t er_dext[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

/* LSB-first 读 n 位（越界按 0 填充，由上层长度校验兜底） */
static uint32_t er_bits(const uint8_t *src, uint32_t srclen,
                        uint32_t *bitpos, uint32_t n) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t bit = 0;
        if (*bitpos < srclen * 8)
            bit = (src[*bitpos >> 3] >> (*bitpos & 7)) & 1u;
        (*bitpos)++;
        v |= bit << i;
    }
    return v;
}

static int er_inflate(const uint8_t *src, uint32_t srclen,
                      uint8_t *dst, uint32_t dstcap, uint32_t *outlen) {
    static er_huff_t lit_h, dist_h, clen_h;
    static uint8_t lens[320];
    static const uint8_t clorder[19] =
        {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    uint32_t bitpos = 0, dp = 0;
    for (;;) {
        uint32_t last = er_bits(src, srclen, &bitpos, 1);
        uint32_t type = er_bits(src, srclen, &bitpos, 2);
        if (type == 0) {                       /* stored */
            bitpos = (bitpos + 7) & ~7u;
            if (bitpos / 8 + 4 > srclen) return -1;
            uint32_t len = src[bitpos / 8] | ((uint32_t)src[bitpos / 8 + 1] << 8);
            bitpos += 32;
            if (bitpos / 8 + len > srclen || dp + len > dstcap) return -1;
            for (uint32_t i = 0; i < len; i++) dst[dp++] = src[bitpos / 8 + i];
            bitpos += len * 8;
        } else {
            if (type == 1) {                   /* fixed huffman */
                static int fixed_inited = 0;
                if (!fixed_inited) {
                    static uint8_t fl[288], fd[30];
                    for (int i = 0; i < 144; i++) fl[i] = 8;
                    for (int i = 144; i < 256; i++) fl[i] = 9;
                    for (int i = 256; i < 280; i++) fl[i] = 7;
                    for (int i = 280; i < 288; i++) fl[i] = 8;
                    for (int i = 0; i < 30; i++) fd[i] = 5;
                    if (er_huff_build(&lit_h, fl, 288) != 0) return -1;
                    if (er_huff_build(&dist_h, fd, 30) != 0) return -1;
                    fixed_inited = 1;
                }
            } else if (type == 2) {            /* dynamic huffman */
                uint32_t hlit = er_bits(src, srclen, &bitpos, 5) + 257;
                uint32_t hdist = er_bits(src, srclen, &bitpos, 5) + 1;
                uint32_t hclen = er_bits(src, srclen, &bitpos, 4) + 4;
                uint8_t cl[19];
                for (uint32_t i = 0; i < 19; i++) cl[i] = 0;
                for (uint32_t i = 0; i < hclen; i++)
                    cl[clorder[i]] = (uint8_t)er_bits(src, srclen, &bitpos, 3);
                if (er_huff_build(&clen_h, cl, 19) != 0) return -1;
                uint32_t n = hlit + hdist, i = 0;
                while (i < n) {
                    int sym = er_huff_decode(&clen_h, src, srclen, &bitpos);
                    if (sym < 0) return -1;
                    if (sym < 16) {
                        lens[i++] = (uint8_t)sym;
                    } else {
                        uint32_t rep, val;
                        if (sym == 16) {
                            if (i == 0) return -1;
                            val = lens[i - 1];
                            rep = 3 + er_bits(src, srclen, &bitpos, 2);
                        } else if (sym == 17) {
                            val = 0;
                            rep = 3 + er_bits(src, srclen, &bitpos, 3);
                        } else {
                            val = 0;
                            rep = 11 + er_bits(src, srclen, &bitpos, 7);
                        }
                        if (i + rep > n) return -1;
                        while (rep--) lens[i++] = (uint8_t)val;
                    }
                }
                if (er_huff_build(&lit_h, lens, hlit) != 0) return -1;
                if (er_huff_build(&dist_h, lens + hlit, hdist) != 0) return -1;
            } else {
                return -1;
            }
            for (;;) {
                int sym = er_huff_decode(&lit_h, src, srclen, &bitpos);
                if (sym < 0) return -1;
                if (sym < 256) {
                    if (dp >= dstcap) return -1;
                    dst[dp++] = (uint8_t)sym;
                } else if (sym == 256) {
                    break;
                } else {
                    sym -= 257;
                    if (sym >= 29) return -1;
                    uint32_t len = er_lbase[sym] +
                                   er_bits(src, srclen, &bitpos, er_lext[sym]);
                    int dsym = er_huff_decode(&dist_h, src, srclen, &bitpos);
                    if (dsym < 0 || dsym >= 30) return -1;
                    uint32_t dist = er_dbase[dsym] +
                                    er_bits(src, srclen, &bitpos, er_dext[dsym]);
                    if (dist == 0 || dist > dp || dp + len > dstcap) return -1;
                    uint32_t s = dp - dist;
                    for (uint32_t k = 0; k < len; k++) dst[dp++] = dst[s + k];
                }
            }
        }
        if (last) break;
    }
    *outlen = dp;
    return 0;
}

/* ============ LZ4 block 解压（LZ4 Block Format spec） ============ */

static int er_lz4_decompress(const uint8_t *src, uint32_t srclen,
                             uint8_t *dst, uint32_t dstcap, uint32_t *outlen) {
    uint32_t sp = 0, dp = 0;
    for (;;) {
        if (sp >= srclen) return -1;
        uint32_t token = src[sp++];
        uint32_t ll = token >> 4;
        if (ll == 15) {
            uint8_t b;
            do {
                if (sp >= srclen) return -1;
                b = src[sp++];
                ll += b;
            } while (b == 255);
        }
        if (dp + ll > dstcap || sp + ll > srclen) return -1;
        for (uint32_t i = 0; i < ll; i++) dst[dp++] = src[sp++];
        if (sp == srclen) break;              /* 末尾 literals 结束 */
        if (sp + 2 > srclen) return -1;
        uint32_t off = src[sp] | ((uint32_t)src[sp + 1] << 8);
        sp += 2;
        if (off == 0 || off > dp) return -1;
        uint32_t ml = token & 15;
        if (ml == 15) {
            uint8_t b;
            do {
                if (sp >= srclen) return -1;
                b = src[sp++];
                ml += b;
            } while (b == 255);
        }
        ml += 4;
        if (dp + ml > dstcap) return -1;
        uint32_t s = dp - off;
        for (uint32_t i = 0; i < ml; i++) dst[dp++] = dst[s + i];
    }
    *outlen = dp;
    return 0;
}

/* ============ 压缩索引映射（U-Boot fs/erofs/zmap.c 移植精简版） ============ */

typedef struct {
    uint64_t m_la, m_pa, m_llen, m_plen;
    uint32_t m_flags;
    uint8_t  m_algfmt;
    uint32_t index;                 /* 已载入索引块号；0xFFFFFFFF = 无 */
    uint8_t  mpage[4096];           /* compacted 索引块缓存 */
} er_zmap_t;

typedef struct {
    uint32_t lcn;
    uint8_t  type, headtype;
    uint16_t clusterofs;
    uint16_t delta[2];
    uint32_t pblk, compressedblks;
} er_zrec_t;

static int er_z_reload(er_zmap_t *map, uint32_t eblk) {
    if (map->index == eblk) return 0;
    if (er_read_blk(eblk, map->mpage) != 0) return -1;
    map->index = eblk;
    return 0;
}

/* 读取惰性压缩头：map header 位于 inode+xattr 后按 8 字节对齐处 */
static int er_z_fill_lazy(er_inode_t *vi) {
    if (vi->z_inited) return 0;
    uint64_t pos = (vi->iloc + vi->inode_isize + vi->xattr_isize + 7) &
                   ~(uint64_t)7;
    uint8_t h[8];
    if (er_read_bytes(pos, h, 8) != 0) return -1;
    if (h[7] & 0x80) return -1;               /* fragment inode：整文件在 packed inode */
    vi->z_advise = rd16(h + 4);
    vi->z_alg[0] = (uint8_t)(h[6] & 15);
    vi->z_alg[1] = (uint8_t)(h[6] >> 4);
    /* 只支持 LZ4(0) / DEFLATE(2)；LZMA(1) 与未知算法拒绝 */
    if (vi->z_alg[0] == Z_COMPR_LZMA || vi->z_alg[0] > Z_COMPR_DEFLATE) return -1;
    if (vi->z_alg[1] == Z_COMPR_LZMA || vi->z_alg[1] > Z_COMPR_DEFLATE) return -1;
    if (vi->z_advise & (Z_ADVISE_INLINE_PCLUSTER | Z_ADVISE_FRAGMENT_PCLUSTER |
                        Z_ADVISE_INTERLACED))
        return -1;
    vi->z_lclusterbits = er_blkszbits + (h[7] & 7);
    vi->z_inited = 1;
    return 0;
}

/* legacy COMPRESSED_FULL：8 字节索引，直接按字节读 */
static int er_z_load_legacy(er_inode_t *vi, er_zrec_t *m, er_zmap_t *map,
                            uint32_t lcn) {
    (void)map;
    uint64_t pos = ((vi->iloc + vi->inode_isize + vi->xattr_isize + 7) &
                    ~(uint64_t)7) + 16 + (uint64_t)lcn * 8;
    uint8_t d[8];
    if (er_read_bytes(pos, d, 8) != 0) return -1;
    m->lcn = lcn;
    uint8_t type = (uint8_t)(rd16(d) & 3);
    switch (type) {
    case Z_LTYPE_NONHEAD:
        m->clusterofs = (uint16_t)(1u << vi->z_lclusterbits);
        m->delta[0] = rd16(d + 4);
        if (m->delta[0] & Z_D0_CBLKCNT) {
            if (!(vi->z_advise & Z_ADVISE_BIG_PCLUSTER_1)) return -1;
            m->compressedblks = m->delta[0] & 0x7FF;
            m->delta[0] = 1;
        }
        m->delta[1] = rd16(d + 6);
        break;
    case Z_LTYPE_PLAIN:
    case Z_LTYPE_HEAD1:
        m->clusterofs = rd16(d + 2);
        m->pblk = rd32(d + 4);
        break;
    default:
        return -1;                             /* HEAD2 legacy 不存在 */
    }
    m->type = type;
    return 0;
}

/* compacted 索引位解码（U-Boot decode_compactedbits） */
static uint32_t er_z_decode_bits(uint32_t lobits, uint32_t lomask,
                                 const uint8_t *in, uint32_t pos, uint8_t *type) {
    uint32_t v = rd32(in + (pos >> 3)) >> (pos & 7);
    uint32_t lo = v & lomask;
    *type = (uint8_t)((v >> lobits) & 3);
    return lo;
}

/* compacted 索引解包（U-Boot unpack_compacted_index，lookahead 恒 false） */
static int er_z_unpack_compacted(er_inode_t *vi, er_zrec_t *m, er_zmap_t *map,
                                 uint32_t amortizedshift, uint64_t pos) {
    uint32_t lclusterbits = vi->z_lclusterbits;
    uint32_t lomask = (1u << lclusterbits) - 1;
    uint32_t vcnt, base, lo, encodebits, nblk, eofs;
    const uint8_t *in;
    uint8_t type;
    int i;

    if ((1u << amortizedshift) == 4) vcnt = 2;
    else if ((1u << amortizedshift) == 2 && lclusterbits == 12) vcnt = 16;
    else return -1;

    int big_pcluster = (vi->z_advise & Z_ADVISE_BIG_PCLUSTER_1) != 0;
    encodebits = ((vcnt << amortizedshift) - 4) * 8 / vcnt;
    eofs = (uint32_t)(pos & (er_blksz - 1));
    base = eofs & ~((vcnt << amortizedshift) - 1);
    in = map->mpage + base;

    i = (int)((eofs - base) >> amortizedshift);

    lo = er_z_decode_bits(lclusterbits, lomask, in, encodebits * (uint32_t)i, &type);
    m->type = type;
    if (type == Z_LTYPE_NONHEAD) {
        m->clusterofs = (uint16_t)(1u << lclusterbits);
        if (lo & Z_D0_CBLKCNT) {
            if (!big_pcluster) return -1;
            m->compressedblks = lo & 0x7FF;
            m->delta[0] = 1;
            return 0;
        } else if (i + 1 != (int)vcnt) {
            m->delta[0] = (uint16_t)lo;
            return 0;
        }
        /* 组尾 NONHEAD：delta[0] 由前一个条目间接推导 */
        lo = er_z_decode_bits(lclusterbits, lomask, in,
                              encodebits * (uint32_t)(i - 1), &type);
        if (type != Z_LTYPE_NONHEAD) lo = 0;
        else if (lo & Z_D0_CBLKCNT) lo = 1;
        m->delta[0] = (uint16_t)(lo + 1);
        return 0;
    }
    m->clusterofs = (uint16_t)lo;
    m->delta[0] = 0;
    /* 回推本组首个 head 的基准块号 */
    if (!big_pcluster) {
        nblk = 1;
        while (i > 0) {
            --i;
            lo = er_z_decode_bits(lclusterbits, lomask, in,
                                  encodebits * (uint32_t)i, &type);
            if (type == Z_LTYPE_NONHEAD) i -= (int)lo;
            if (i >= 0) ++nblk;
        }
    } else {
        nblk = 0;
        while (i > 0) {
            --i;
            lo = er_z_decode_bits(lclusterbits, lomask, in,
                                  encodebits * (uint32_t)i, &type);
            if (type == Z_LTYPE_NONHEAD) {
                if (lo & Z_D0_CBLKCNT) {
                    --i;
                    nblk += lo & 0x7FF;
                    continue;
                }
                if (lo <= 1) return -1;
                i -= (int)lo - 2;
                continue;
            }
            ++nblk;
        }
    }
    in += (vcnt << amortizedshift) - 4;
    m->pblk = rd32(in) + nblk;
    return 0;
}

/* COMPRESSED_COMPACT：compacted 4B 初始段 + 2B 段 + 4B 尾段（U-Boot 布局） */
static int er_z_load_compacted(er_inode_t *vi, er_zrec_t *m, er_zmap_t *map,
                               uint32_t lcn) {
    uint32_t lclusterbits = vi->z_lclusterbits;
    uint64_t ebase = ((vi->iloc + vi->inode_isize + vi->xattr_isize + 7) &
                      ~(uint64_t)7) + 8;                 /* map header 之后 */
    uint32_t totalidx = (uint32_t)((vi->size + er_blksz - 1) >> er_blkszbits);
    uint32_t compacted_4b_initial, compacted_2b, amortizedshift;
    uint64_t pos;

    if (lclusterbits != 12) return -1;
    if (lcn >= totalidx) return -1;
    m->lcn = lcn;
    compacted_4b_initial = (32 - (uint32_t)(ebase % 32)) / 4;
    if (compacted_4b_initial == 32 / 4) compacted_4b_initial = 0;
    if ((vi->z_advise & Z_ADVISE_COMPACTED_2B) && compacted_4b_initial < totalidx)
        compacted_2b = (totalidx - compacted_4b_initial) & ~15u;
    else
        compacted_2b = 0;

    pos = ebase;
    uint32_t l = lcn;
    if (l < compacted_4b_initial) {
        amortizedshift = 2;
        goto out;
    }
    pos += (uint64_t)compacted_4b_initial * 4;
    l -= compacted_4b_initial;
    if (l < compacted_2b) {
        amortizedshift = 1;
        goto out;
    }
    pos += (uint64_t)compacted_2b * 2;
    l -= compacted_2b;
    amortizedshift = 2;
out:
    pos += (uint64_t)l << amortizedshift;
    if (er_z_reload(map, (uint32_t)(pos >> er_blkszbits)) != 0) return -1;
    return er_z_unpack_compacted(vi, m, map, amortizedshift, pos);
}

static int er_z_load_cluster(er_inode_t *vi, er_zrec_t *m, er_zmap_t *map,
                             uint32_t lcn) {
    if (vi->datalayout == EROFS_INODE_COMPRESSED_FULL)
        return er_z_load_legacy(vi, m, map, lcn);
    if (vi->datalayout == EROFS_INODE_COMPRESSED_COMPACT)
        return er_z_load_compacted(vi, m, map, lcn);
    return -1;
}

/* NONHEAD 回溯到所属 HEAD（U-Boot z_erofs_extent_lookback） */
static int er_z_lookback(er_inode_t *vi, er_zrec_t *m, er_zmap_t *map,
                         uint32_t dist, int depth) {
    uint32_t lclusterbits = vi->z_lclusterbits;
    if (depth > 64) return -1;
    uint32_t lcn = m->lcn;
    if (lcn < dist) return -1;
    lcn -= dist;
    if (er_z_load_cluster(vi, m, map, lcn) != 0) return -1;
    switch (m->type) {
    case Z_LTYPE_NONHEAD:
        if (!m->delta[0]) return -1;
        return er_z_lookback(vi, m, map, m->delta[0], depth + 1);
    case Z_LTYPE_PLAIN:
    case Z_LTYPE_HEAD1:
        m->headtype = m->type;
        map->m_la = ((uint64_t)lcn << lclusterbits) | m->clusterofs;
        return 0;
    default:
        return -1;
    }
}

/* 压缩数据长度（U-Boot z_erofs_get_extent_compressedlen） */
static int er_z_complen(er_inode_t *vi, er_zrec_t *m, er_zmap_t *map) {
    uint32_t lclusterbits = vi->z_lclusterbits;
    if (m->headtype == Z_LTYPE_PLAIN ||
        !(vi->z_advise & Z_ADVISE_BIG_PCLUSTER_1)) {
        map->m_plen = 1u << lclusterbits;
        return 0;
    }
    uint32_t lcn = m->lcn + 1;
    if (m->compressedblks) {
        map->m_plen = (uint64_t)m->compressedblks << er_blkszbits;
        return 0;
    }
    if (er_z_load_cluster(vi, m, map, lcn) != 0) return -1;
    switch (m->type) {
    case Z_LTYPE_PLAIN:
    case Z_LTYPE_HEAD1:
        m->compressedblks = 1u << (lclusterbits - er_blkszbits);
        break;
    case Z_LTYPE_NONHEAD:
        if (m->delta[0] != 1 || !m->compressedblks) return -1;
        break;
    default:
        return -1;
    }
    map->m_plen = (uint64_t)m->compressedblks << er_blkszbits;
    return 0;
}

/* 映射一个 lcluster（U-Boot z_erofs_do_map_blocks 精简） */
static int er_z_do_map(er_inode_t *vi, er_zmap_t *map) {
    er_zrec_t m;
    uint32_t lclusterbits = vi->z_lclusterbits;
    uint64_t ofs = map->m_la;
    uint32_t initial_lcn = (uint32_t)(ofs >> lclusterbits);
    uint32_t endoff = (uint32_t)(ofs & ((1u << lclusterbits) - 1));
    uint64_t end;

    m.lcn = 0; m.type = 0; m.headtype = 0; m.clusterofs = 0;
    m.delta[0] = 0; m.delta[1] = 0; m.pblk = 0; m.compressedblks = 0;

    if (er_z_load_cluster(vi, &m, map, initial_lcn) != 0) return -1;

    map->m_flags = ZMAP_MAPPED;
    end = ((uint64_t)m.lcn + 1) << lclusterbits;
    switch (m.type) {
    case Z_LTYPE_PLAIN:
    case Z_LTYPE_HEAD1:
        if (endoff >= m.clusterofs) {
            m.headtype = m.type;
            map->m_la = ((uint64_t)m.lcn << lclusterbits) | m.clusterofs;
            break;
        }
        if (!m.lcn) return -1;
        end = ((uint64_t)m.lcn << lclusterbits) | m.clusterofs;
        m.delta[0] = 1;
        /* fallthrough */
    case Z_LTYPE_NONHEAD:
        if (er_z_lookback(vi, &m, map, m.delta[0], 0) != 0) return -1;
        break;
    default:
        return -1;
    }
    map->m_llen = end - map->m_la;
    map->m_pa = (uint64_t)m.pblk << er_blkszbits;
    if (er_z_complen(vi, &m, map) != 0) return -1;
    if (m.headtype == Z_LTYPE_PLAIN) {
        if (vi->z_advise & Z_ADVISE_INTERLACED) return -1;
        map->m_algfmt = Z_COMPR_SHIFTED;
    } else {
        map->m_algfmt = (m.headtype == Z_LTYPE_HEAD2) ? vi->z_alg[1]
                                                      : vi->z_alg[0];
    }
    return 0;
}

/* ---------- 压缩文件读取 ---------- */
/* 大缓冲放高内存段 .bss.hi（1MB+，见 linker.ld）：低 640KB 区留给栈/小数据 */
#define ER_HIBUF __attribute__((section(".bss.hi")))
#define ER_ZBUF 65536
static uint8_t er_cbuf[ER_ZBUF] ER_HIBUF;
static uint8_t er_dbuf[ER_ZBUF] ER_HIBUF;

static int er_read_z(er_inode_t *vi, uint8_t *buf, uint32_t max) {
    if (er_z_fill_lazy(vi) != 0) return -1;
    uint64_t la = 0;
    while (la < vi->size && la < max) {
        er_zmap_t map;
        map.m_la = la;
        map.m_flags = 0;
        map.index = 0xFFFFFFFFu;
        if (er_z_do_map(vi, &map) != 0) return -1;
        if (map.m_llen == 0) return -1;
        if (!(map.m_flags & ZMAP_MAPPED)) {
            uint32_t z = (map.m_llen > max - la) ? max - (uint32_t)la
                                                 : (uint32_t)map.m_llen;
            for (uint32_t k = 0; k < z; k++) buf[la + k] = 0;
        } else {
            if (map.m_plen == 0 || map.m_plen > ER_ZBUF) return -1;
            if (er_read_bytes(map.m_pa, er_cbuf, (uint32_t)map.m_plen) != 0)
                return -1;
            uint32_t copy = (uint32_t)map.m_llen;
            if (copy > vi->size - map.m_la) copy = (uint32_t)(vi->size - map.m_la);
            if (copy > max - la) copy = max - (uint32_t)la;
            if (map.m_algfmt == Z_COMPR_SHIFTED) {
                uint32_t n = copy < (uint32_t)map.m_plen
                           ? copy : (uint32_t)map.m_plen;
                for (uint32_t k = 0; k < n; k++) buf[la + k] = er_cbuf[k];
                for (uint32_t k = n; k < copy; k++) buf[la + k] = 0;
            } else {
                uint32_t outlen = 0;
                int r = -1;
                if (map.m_algfmt == Z_COMPR_LZ4)
                    r = er_lz4_decompress(er_cbuf, (uint32_t)map.m_plen,
                                          er_dbuf, ER_ZBUF, &outlen);
                else if (map.m_algfmt == Z_COMPR_DEFLATE)
                    r = er_inflate(er_cbuf, (uint32_t)map.m_plen,
                                   er_dbuf, ER_ZBUF, &outlen);
                if (r != 0 || outlen < copy) return -1;
                for (uint32_t k = 0; k < copy; k++) buf[la + k] = er_dbuf[k];
            }
        }
        la = map.m_la + map.m_llen;
    }
    return (int)((vi->size < max) ? vi->size : max);
}

/* ---------- FLAT 布局读取（PLAIN / INLINE 尾打包） ---------- */
static int er_read_flat(er_inode_t *vi, uint8_t *buf, uint32_t max) {
    uint64_t size = vi->size < max ? vi->size : max;
    uint32_t raw = vi->i_u;
    uint32_t nblocks = (uint32_t)((vi->size + er_blksz - 1) / er_blksz);
    uint64_t done = 0;
    for (uint32_t b = 0; b + 1 < nblocks && done < size; b++) {
        uint32_t chunk = er_blksz;
        if (chunk > size - done) chunk = (uint32_t)(size - done);
        if (er_read_bytes((uint64_t)(raw + b) << er_blkszbits,
                          buf + done, chunk) != 0) return -1;
        done += chunk;
    }
    if (nblocks != 0 && done < size) {
        uint32_t tail = (uint32_t)(size - done);
        uint64_t off;
        if (vi->datalayout == EROFS_INODE_FLAT_INLINE)
            off = vi->iloc + vi->inode_isize + vi->xattr_isize;
        else
            off = (uint64_t)(raw + nblocks - 1) << er_blkszbits;
        if (er_read_bytes(off, buf + done, tail) != 0) return -1;
        done += tail;
    }
    return (int)done;
}

/* ---------- CHUNK 布局读取（4B 数组 / 8B chunk index） ---------- */
static int er_read_chunk(er_inode_t *vi, uint8_t *buf, uint32_t max) {
    uint32_t fmt = vi->i_u & 0xFFFF;           /* i_u 低 16 位 chunk format */
    uint32_t chunkbits = (fmt & 0x1F) + er_blkszbits;
    int has_idx = (fmt & 0x20) != 0;
    uint64_t chunksize = 1ULL << chunkbits;
    uint64_t table = vi->iloc + vi->inode_isize + vi->xattr_isize;
    uint32_t nchunks = (uint32_t)((vi->size + chunksize - 1) / chunksize);
    uint64_t size = vi->size < max ? vi->size : max;
    uint64_t done = 0;
    for (uint32_t c = 0; c < nchunks && done < size; c++) {
        uint8_t ent[8];
        uint32_t esz = has_idx ? 8 : 4;
        if (er_read_bytes(table + (uint64_t)c * esz, ent, esz) != 0) return -1;
        uint32_t blkaddr = rd32(ent + (has_idx ? 4 : 0));
        uint32_t chunk = (uint32_t)((chunksize < size - done)
                                    ? chunksize : size - done);
        if (blkaddr == EROFS_NULL_ADDR) {
            for (uint32_t k = 0; k < chunk; k++) buf[done + k] = 0;
        } else {
            if (er_read_bytes((uint64_t)blkaddr << er_blkszbits,
                              buf + done, chunk) != 0) return -1;
        }
        done += chunk;
    }
    return (int)done;
}

/* ---------- 统一数据读取入口 ---------- */
static int er_read_file_data(er_inode_t *vi, uint8_t *buf, uint32_t max) {
    switch (vi->datalayout) {
    case EROFS_INODE_FLAT_PLAIN:
    case EROFS_INODE_FLAT_INLINE:
        return er_read_flat(vi, buf, max);
    case EROFS_INODE_CHUNK_BASED:
        return er_read_chunk(vi, buf, max);
    case EROFS_INODE_COMPRESSED_FULL:
    case EROFS_INODE_COMPRESSED_COMPACT:
        return er_read_z(vi, buf, max);
    default:
        return -1;
    }
}

/* ---------- 目录遍历 ---------- */
static uint8_t er_dirbuf[32768] ER_HIBUF;

typedef int (*er_dir_cb)(const char *name, uint32_t nid, int is_dir, void *ctx);

static int er_dir_walk(uint32_t dir_nid, er_dir_cb cb, void *ctx) {
    er_inode_t ino;
    if (er_load_inode(dir_nid, &ino) != 0) return -1;
    if ((ino.mode & 0xF000) != 0x4000) return -1;
    if (ino.size == 0) return 0;
    uint32_t dlen = ino.size > sizeof(er_dirbuf)
                  ? (uint32_t)sizeof(er_dirbuf) : (uint32_t)ino.size;
    int r = er_read_file_data(&ino, er_dirbuf, dlen);
    if (r < 0 || (uint32_t)r < dlen) return -1;
    if (dlen < DIRENT_SIZE) return 0;

    uint32_t ndirents = rd16(er_dirbuf + DIRENT_OFF_NAMEOFF) / DIRENT_SIZE;
    if (ndirents > dlen / DIRENT_SIZE) return -1;
    for (uint32_t i = 0; i < ndirents; i++) {
        const uint8_t *de = er_dirbuf + i * DIRENT_SIZE;
        uint32_t nid = rd32(de + DIRENT_OFF_NID);       /* nid 低 32 位 */
        uint32_t nameoff = rd16(de + DIRENT_OFF_NAMEOFF);
        uint32_t nextoff = (i + 1 < ndirents)
            ? rd16(er_dirbuf + (i + 1) * DIRENT_SIZE + DIRENT_OFF_NAMEOFF)
            : dlen;
        if (nameoff < ndirents * DIRENT_SIZE || nameoff >= dlen) continue;
        if (nextoff > dlen || nextoff <= nameoff) continue;
        uint32_t nlen = nextoff - nameoff;
        if (nlen > 255) continue;
        char name[256];
        for (uint32_t k = 0; k < nlen; k++) name[k] = (char)er_dirbuf[nameoff + k];
        name[nlen] = 0;
        if (nlen == 1 && name[0] == '.') continue;              /* "." */
        if (nlen == 2 && name[0] == '.' && name[1] == '.') continue;   /* ".." */
        int is_dir = de[DIRENT_OFF_TYPE] == EROFS_FT_DIR;
        if (cb(name, nid, is_dir, ctx) != 0) return -1;
    }
    return 0;
}

/* ---------- 路径解析 ---------- */
typedef struct {
    const char *name;
    uint32_t name_len;
    int found;
    uint32_t nid;
    int is_dir;
} er_lookup_ctx;

static char er_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* 本 OS 约定文件名 ASCII 大小写不敏感（与 exFAT/FAT 一致；区别于 Linux 原生 EROFS） */
static int er_lookup_cb(const char *name, uint32_t nid, int is_dir, void *ctx) {
    er_lookup_ctx *c = (er_lookup_ctx *)ctx;
    uint32_t n = 0;
    while (name[n]) n++;
    if (n != c->name_len) return 0;
    for (uint32_t i = 0; i < n; i++)
        if (er_lower(name[i]) != er_lower(c->name[i])) return 0;
    c->found = 1;
    c->nid = nid;
    c->is_dir = is_dir;
    return 0;
}

static uint32_t er_resolve(const char *path, er_inode_t *ino_out,
                           int *is_dir_out, uint64_t *size_out) {
    if (!er_mounted) return 0;
    er_inode_t ino;
    uint32_t cur = er_root_nid;
    if (er_load_inode(cur, &ino) != 0) return 0;
    const char *p = path;
    while (*p == '/') p++;
    while (*p) {
        const char *seg = p;
        uint32_t slen = 0;
        while (p[slen] && p[slen] != '/') slen++;
        if (slen == 0 || slen > 255) return 0;
        if ((ino.mode & 0xF000) != 0x4000) return 0;
        er_lookup_ctx ctx;
        ctx.name = seg;
        ctx.name_len = slen;
        ctx.found = 0;
        if (er_dir_walk(cur, er_lookup_cb, &ctx) != 0) return 0;
        if (!ctx.found) return 0;
        cur = ctx.nid;
        if (er_load_inode(cur, &ino) != 0) return 0;
        p += slen;
        while (*p == '/') p++;
    }
    if (is_dir_out) *is_dir_out = (ino.mode & 0xF000) == 0x4000;
    if (size_out) *size_out = ino.size;
    if (ino_out) *ino_out = ino;
    return cur;
}

/* ---------- 对外 API ---------- */
int erofs_is_dir(const char *path) {
    int is_dir;
    if (er_resolve(path, 0, &is_dir, 0) == 0) return -1;
    return is_dir;
}

uint32_t erofs_get_file_size(const char *path) {
    uint64_t size;
    if (er_resolve(path, 0, 0, &size) == 0) return 0;
    return (size > 0xFFFFFFFFu) ? 0xFFFFFFFFu : (uint32_t)size;
}

int erofs_read_file(const char *path, uint8_t *buffer, uint32_t max_size) {
    er_inode_t ino;
    if (er_resolve(path, &ino, 0, 0) == 0) return -1;
    if ((ino.mode & 0xF000) != 0x8000) return -1;    /* 非常规文件 */
    return er_read_file_data(&ino, buffer, max_size);
}

typedef struct {
    fs_dir_entry_t *entries;
    int max, n;
} er_fill_ctx;

static int er_fill_cb(const char *name, uint32_t nid, int is_dir, void *ctx) {
    er_fill_ctx *c = (er_fill_ctx *)ctx;
    if (c->n >= c->max) return 1;
    fs_dir_entry_t *e = &c->entries[c->n++];
    uint32_t i = 0;
    while (name[i] && i < 255) { e->name[i] = name[i]; i++; }
    e->name[i] = 0;
    e->is_dir = (uint8_t)is_dir;
    e->size = 0;
    if (!is_dir) {
        er_inode_t ci;
        if (er_load_inode(nid, &ci) == 0) e->size = (uint32_t)ci.size;
    }
    return 0;
}

int erofs_read_dir(const char *path, fs_dir_entry_t *entries, int max_entries) {
    int is_dir;
    uint32_t dir = er_resolve(path, 0, &is_dir, 0);
    if (dir == 0 || !is_dir) return -1;
    er_fill_ctx ctx;
    ctx.entries = entries;
    ctx.max = max_entries;
    ctx.n = 0;
    if (er_dir_walk(dir, er_fill_cb, &ctx) != 0) return -1;
    return ctx.n;
}

uint32_t erofs_get_file_clusters(const char *path) {
    er_inode_t ino;
    if (er_resolve(path, &ino, 0, 0) == 0) return 0;
    /* EROFS 无显式块计数字段：按文件大小向上取整到块（只读统计口径） */
    return (uint32_t)((ino.size + er_blksz - 1) / er_blksz);
}

const erofs_info_t *erofs_get_info(void) { return &er_info; }

/* 从 MBR 分区表取卷长度（superblock 无卷大小字段；未知返回 0） */
static uint32_t er_part_len(uint8_t drive, uint32_t part_start) {
    uint8_t sec[512];
    if (ata_read_sector(drive, 0, sec) != 0) return 0;
    if (sec[510] != 0x55 || sec[511] != 0xAA) return 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = sec + 446 + i * 16;
        if (e[4] == 0) continue;
        if (rd32(e + 8) == part_start) return rd32(e + 12);
    }
    return 0;
}

int erofs_mount(uint8_t drive, uint32_t part_start) {
    uint8_t sec[512];
    if (ata_read_sector(drive, part_start + 2, sec) != 0) return -1;
    const uint8_t *sb = sec;                     /* 1024 偏移 = 第 2 扇区起 */
    if (rd32(sb + SB_OFF_MAGIC) != EROFS_MAGIC) return -1;
    uint32_t blkszbits = sb[SB_OFF_BLKSZBITS];
    if (blkszbits < 9 || blkszbits > 12) return -1;
    uint32_t feat_compat = rd32(sb + SB_OFF_FEAT_COMPAT);
    uint32_t feat_incompat = rd32(sb + SB_OFF_FEAT_INCOMPAT);
    if (feat_incompat & ~EROFS_SB_INCOMPAT_OK) return -1;
    if (rd16(sb + SB_OFF_EXTRA_DEVICES) > 0) return -1;    /* 多设备不支持 */
    if (feat_compat & EROFS_SB_COMPAT_CHKSUM) {
        uint8_t tmp[128];
        for (int i = 0; i < 128; i++) tmp[i] = sb[i];
        tmp[4] = tmp[5] = tmp[6] = tmp[7] = 0;
        if (er_crc32c(0, tmp, 128) != rd32(sb + SB_OFF_CHECKSUM)) return -1;
    }

    er_drive = drive;
    er_part_lba = part_start;
    er_blkszbits = blkszbits;
    er_blksz = 1u << blkszbits;
    er_meta_blkaddr = rd32(sb + SB_OFF_META_BLKADDR);
    er_meta_off = (uint64_t)er_meta_blkaddr << blkszbits;
    er_root_nid = rd16(sb + SB_OFF_ROOT_NID);
    er_inos = rd64(sb + SB_OFF_INOS);
    er_blocks = rd32(sb + SB_OFF_BLOCKS);
    er_mounted = 1;

    er_inode_t root;
    if (er_root_nid >= er_inos ||
        er_load_inode(er_root_nid, &root) != 0 ||
        (root.mode & 0xF000) != 0x4000) {
        er_mounted = 0;
        return -1;
    }

    er_info.part_start = part_start;
    er_info.bytes_per_sector = 512;
    er_info.sectors_per_cluster = (uint8_t)(er_blksz / 512);
    er_info.cluster_count = er_blocks;      /* 只读 FS：全部为已用块 */
    er_info.used_clusters = er_blocks;
    er_info.volume_sectors = er_part_len(drive, part_start);
    return 0;
}
