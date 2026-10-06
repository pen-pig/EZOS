/*
 * sysinfo.c - 日用工具：md5 / crc32 / crc16 / crc32c
 *
 * 为什么要有这些：文件系统对不对，"自己能算一遍校验和"是最硬的证据。
 * 宿主机只能对照别人的输出看，而我们要能对**盘上原始字节**直接求值——
 * 包括 exFAT 目录项的 SetChecksum、卷引导区的 Boot Checksum。
 *
 * 全部无分配、无 libc。crc32 用 zlib 多项式（与 Python zlib.crc32 一致），
 * crc32c 用 Castagnoli（与 EROFS 引导校验和一致），crc16 用 Modbus。
 */
#include "types.h"
#include "fs.h"
#include "fd.h"
#include "task.h"

/* ================= MD5 (RFC 1321) ================= */
typedef struct {
    uint32_t a, b, c, d;
    uint64_t bits;
    uint8_t buf[64];
    uint32_t buflen;
} md5_ctx;

static const uint32_t md5_K[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu,
    0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
    0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu,
    0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
    0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
    0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
    0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
    0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
    0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu,
    0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
    0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u,
    0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
    0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u,
    0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
    0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u
};

static const uint8_t md5_R[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20, 5,  9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
};

static uint32_t rol32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static void md5_block(md5_ctx *c, const uint8_t *p) {
    uint32_t m[16];
    for (int i = 0; i < 16; i++)
        m[i] = (uint32_t)p[i*4] | ((uint32_t)p[i*4+1] << 8) |
               ((uint32_t)p[i*4+2] << 16) | ((uint32_t)p[i*4+3] << 24);
    uint32_t a = c->a, b = c->b, cc = c->c, d = c->d;
    for (int i = 0; i < 64; i++) {
        uint32_t f; int g;
        if (i < 16)      { f = (b & cc) | (~b & d);  g = i; }
        else if (i < 32) { f = (d & b) | (~d & cc);  g = (5*i + 1) & 15; }
        else if (i < 48) { f = b ^ cc ^ d;           g = (3*i + 5) & 15; }
        else             { f = cc ^ (b | ~d);        g = (7*i) & 15; }
        uint32_t tmp = d;
        d = cc; cc = b;
        b = b + rol32(a + f + md5_K[i] + m[g], md5_R[i]);
        a = tmp;
    }
    c->a += a; c->b += b; c->c += cc; c->d += d;
}

static void md5_init(md5_ctx *c) {
    c->a = 0x67452301u; c->b = 0xefcdab89u;
    c->c = 0x98badcfeu; c->d = 0x10325476u;
    c->bits = 0; c->buflen = 0;
}

static void md5_update(md5_ctx *c, const uint8_t *p, uint32_t len) {
    c->bits += (uint64_t)len * 8;
    while (len) {
        uint32_t n = 64 - c->buflen;
        if (n > len) n = len;
        for (uint32_t i = 0; i < n; i++) c->buf[c->buflen + i] = p[i];
        c->buflen += n; p += n; len -= n;
        if (c->buflen == 64) { md5_block(c, c->buf); c->buflen = 0; }
    }
}

static void md5_final(md5_ctx *c, uint8_t out[16]) {
    uint64_t bits = c->bits;
    uint8_t one = 0x80, zero = 0;
    md5_update(c, &one, 1);
    while (c->buflen != 56) md5_update(c, &zero, 1);
    for (int i = 0; i < 8; i++) {
        uint8_t b = (uint8_t)(bits >> (8 * i));
        md5_update(c, &b, 1);
    }
    for (int i = 0; i < 4; i++) {
        out[i]      = (uint8_t)(c->a >> (8 * i));
        out[4 + i]  = (uint8_t)(c->b >> (8 * i));
        out[8 + i]  = (uint8_t)(c->c >> (8 * i));
        out[12 + i] = (uint8_t)(c->d >> (8 * i));
    }
}

/* ================= CRC ================= */
uint32_t ezos_crc32(uint32_t crc, const uint8_t *p, uint32_t len) {
    crc = ~crc;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

uint32_t ezos_crc32c(uint32_t crc, const uint8_t *p, uint32_t len) {
    crc = ~crc;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0x82F63B78u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

uint16_t ezos_crc16(const uint8_t *p, uint32_t len) {
    uint16_t crc = 0xFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint16_t)p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001)
                            : (uint16_t)(crc >> 1);
    }
    return crc;
}

/* ================= 对外纯 API（shell 包装在 shell_extra.c） ================= */

/* 分块读完整文件并算校验和。
 * 注意：fs_read_file() 每次都从文件开头读（没有偏移参数），拿它写循环会把
 * 同一段反复喂进校验和——必须走 fd 才有连续的文件位置。
 * 返回读到的字节数，<=0 表示打不开。 */
static int digest_fd(const char *path, uint8_t *buf, uint32_t bufsz,
                     int which, uint32_t *crc32_out, uint16_t *crc16_out,
                     uint32_t *total) {
    task_t *cur = task_current();
    if (cur == 0) return -1;
    fd_table_t *t = &cur->fds;
    int fd = fd_open(t, path, O_RDONLY);
    if (fd < 0) return -1;
    uint32_t c32 = 0, sum = 0;
    uint16_t c16 = 0xFFFF;
    int n;
    while ((n = fd_read(t, fd, buf, bufsz)) > 0) {
        if (which == 0)      c32 = ezos_crc32(c32, buf, (uint32_t)n);
        else if (which == 2) c32 = ezos_crc32c(c32, buf, (uint32_t)n);
        else if (which == 1) {
            for (int i = 0; i < n; i++) {
                c16 ^= (uint16_t)buf[i];
                for (int k = 0; k < 8; k++)
                    c16 = (c16 & 1) ? (uint16_t)((c16 >> 1) ^ 0xA001)
                                    : (uint16_t)(c16 >> 1);
            }
        }
        sum += (uint32_t)n;
    }
    fd_close(t, fd);
    if (crc32_out) *crc32_out = c32;
    if (crc16_out) *crc16_out = c16;
    if (total) *total = sum;
    return (int)sum;
}

static void hex_of(char *out, uint32_t v, int digits, const char *hexl) {
    for (int i = 0; i < digits; i++)
        out[i] = hexl[(v >> ((digits - 1 - i) * 4)) & 15];
    out[digits] = 0;
}

static const char HEXL[] = "0123456789abcdef";
static const char HEXU[] = "0123456789ABCDEF";

/* 成功返回 0；失败返回 -1。hex 缓冲区至少 33 字节。 */
int sysinfo_md5(const char *path, char *hex33, uint32_t *size_out) {
    static uint8_t buf[1024];
    task_t *cur = task_current();
    if (cur == 0) return -1;
    fd_table_t *t = &cur->fds;
    int fd = fd_open(t, path, O_RDONLY);
    if (fd < 0) return -1;
    md5_ctx c;
    md5_init(&c);
    uint32_t total = 0;
    int n;
    while ((n = fd_read(t, fd, buf, sizeof(buf))) > 0) {
        md5_update(&c, buf, (uint32_t)n);
        total += (uint32_t)n;
    }
    fd_close(t, fd);
    if (total == 0) return -1;
    uint8_t dig[16];
    md5_final(&c, dig);
    for (int i = 0; i < 16; i++) {
        hex33[i*2]   = HEXL[dig[i] >> 4];
        hex33[i*2+1] = HEXL[dig[i] & 15];
    }
    hex33[32] = 0;
    if (size_out) *size_out = total;
    return 0;
}

/* which: 0=crc32 2=crc32c 1=crc16。hex 缓冲区至少 17 字节。 */
int sysinfo_crc(const char *path, int which, char *hex_out) {
    static uint8_t buf[1024];
    uint32_t c32 = 0, total = 0;
    uint16_t c16 = 0;
    if (digest_fd(path, buf, sizeof(buf), which, &c32, &c16, &total) <= 0)
        return -1;
    if (which == 1) hex_of(hex_out, c16, 4, HEXU);
    else            hex_of(hex_out, c32, 8, HEXL);
    return 0;
}

/* 供 selftest 用：纯内存校验和（不碰文件系统） */
int sysinfo_selftest(void) {
    int bad = 0;
    /* RFC 1321 官方测试向量 */
    static const char *msg = "abc";
    uint8_t buf[8];
    char hex[33];
    for (uint32_t i = 0; i < 3; i++) buf[i] = (uint8_t)msg[i];
    md5_ctx c; uint8_t dig[16];
    md5_init(&c); md5_update(&c, buf, 3); md5_final(&c, dig);
    for (int i = 0; i < 16; i++) {
        hex[i*2]   = HEXL[dig[i] >> 4];
        hex[i*2+1] = HEXL[dig[i] & 15];
    }
    hex[32] = 0;
    /* 900150983cd24fb0d6963f7d28e17f72 */
    static const char *want = "900150983cd24fb0d6963f7d28e17f72";
    for (int i = 0; i < 32; i++) if (hex[i] != want[i]) { bad++; break; }

    /* 空串的 MD5 = d41d8cd98f00b204e9800998ecf8427e */
    md5_init(&c); md5_final(&c, dig);
    for (int i = 0; i < 16; i++) {
        hex[i*2]   = HEXL[dig[i] >> 4];
        hex[i*2+1] = HEXL[dig[i] & 15];
    }
    hex[32] = 0;
    {
        const char *w2 = "d41d8cd98f00b204e9800998ecf8427e";
        for (int i = 0; i < 32; i++) if (hex[i] != w2[i]) { bad++; break; }
    }

    /* crc32("123456789") = 0xCBF43926（zlib 标准向量） */
    static const uint8_t v9[10] = "123456789";  /* 含结尾 NUL */
    if (ezos_crc32(0, v9, 9) != 0xCBF43926u) bad++;
    /* crc32c("123456789") = 0xE3069283（Castagnoli 标准向量） */
    if (ezos_crc32c(0, v9, 9) != 0xE3069283u) bad++;
    /* crc16(Modbus)("123456789") = 0x4B37 */
    if (ezos_crc16(v9, 9) != 0x4B37u) bad++;
    return bad;
}
