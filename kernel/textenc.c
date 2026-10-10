/*
 * textenc.c - 文件名编解码与文本算法的 **C 参考实现**。
 *
 * 这个文件里的函数**不是生产路径**——生产路径走 rust_bridge.h 里
 * EZ_TEXT_IMPL 选中的那一份（默认 Rust）。它们存在的理由有两条：
 *
 *   1. `rstest` 的对拍基线。三个语言各写一遍同一条算法，任何一份的边界
 *      处理漂移（把落单代理项原样透传、overlong 没拒、装不下时截断而不是
 *      报错）都会在 rstest 变红。单看一份实现永远发现不了这类错。
 *   2. EZ_TEXT_IMPL=0 时的回退目标。Rust/Zig 没装（EZOS_SKIP_RUSTZIG=1）
 *      时内核照样编得过、跑得起来。
 *
 * 所以这里的代码要**故意写得直白**，不要为了省几条指令去玩位技巧——
 * 一旦它和生产实现出现"同样的聪明错误"，对拍就失去意义了。
 *
 * 契约（与 rust/ezos_rs/src/lib.rs、rust/ezos_zig/ezos_zig.zig 逐位一致）：
 *   * 落单代理项（高代理无低代理 / 低代理无高代理）-> U+FFFD，前进 1 个码元
 *   * 非法 UTF-8 -> U+FFFD，前进 1 字节
 *   * 输出装不下 -> 返回 -1，**绝不返回半截结果**
 */
#include "types.h"
#include "rust_bridge.h"

/* ---------- UTF-16LE -> UTF-8 ---------- */

/* 把一个码点按 UTF-8 写进 dst（最多 4 字节）。room 是可用字节数。
 * 返回写入字节数；装不下返回 -1。 */
static int32_t put_utf8(uint32_t cp, uint8_t *dst, uint32_t room) {
    uint32_t need;
    if (cp < 0x80) {
        need = 1;
    } else if (cp < 0x800) {
        need = 2;
    } else if (cp < 0x10000) {
        need = 3;
    } else {
        need = 4;
    }
    if (room < need) return -1;
    if (need == 1) {
        dst[0] = (uint8_t)cp;
    } else if (need == 2) {
        dst[0] = (uint8_t)(0xC0 | (cp >> 6));
        dst[1] = (uint8_t)(0x80 | (cp & 0x3F));
    } else if (need == 3) {
        dst[0] = (uint8_t)(0xE0 | (cp >> 12));
        dst[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        dst[2] = (uint8_t)(0x80 | (cp & 0x3F));
    } else {
        dst[0] = (uint8_t)(0xF0 | (cp >> 18));
        dst[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
        dst[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
        dst[3] = (uint8_t)(0x80 | (cp & 0x3F));
    }
    return (int32_t)need;
}

/* UTF-16LE 码元数组 -> UTF-8（C 的 uint16_t[] 在 i686 上就是 UTF-16LE）。
 * units = 码元个数；cap = dst 字节容量（含结尾 NUL）。
 * 返回写入字节数（不含 NUL）；容量不足或 units==0 返回 -1。 */
int32_t utf16_to_utf8_c(const uint16_t *src, uint32_t units,
                        char *dst, uint32_t cap) {
    uint8_t *out = (uint8_t *)dst;
    uint32_t used = 0, i = 0;
    if (units == 0 || cap == 0) return -1;
    while (i < units) {
        uint32_t c = src[i];
        i++;
        uint32_t cp;
        if (c >= 0xD800 && c < 0xDC00) {
            /* 高代理：必须紧跟低代理，否则替换成 U+FFFD */
            if (i < units) {
                uint32_t lo = src[i];
                if (lo >= 0xDC00 && lo < 0xE000) {
                    i++;
                    cp = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                } else {
                    cp = 0xFFFD;
                }
            } else {
                cp = 0xFFFD;
            }
        } else if (c >= 0xDC00 && c < 0xE000) {
            cp = 0xFFFD;                  /* 落单低代理 */
        } else {
            cp = c;
        }
        /* room 要给结尾 NUL 留一个字节 */
        if (cap - 1 <= used) return -1;
        int32_t n = put_utf8(cp, out + used, cap - 1 - used);
        if (n < 0) return -1;
        used += (uint32_t)n;
    }
    out[used] = 0;
    return (int32_t)used;
}

/* ---------- UTF-8 -> UTF-16LE ---------- */

/* 取一个续字节并校验它在 0x80..0xBF。合法返回其低 6 位，非法返回 -1。 */
static int32_t cont(const uint8_t *p) {
    if ((p[0] & 0xC0) != 0x80) return -1;
    return (int32_t)(p[0] & 0x3F);
}

/* UTF-8（C 风格 NUL 终止）-> UTF-16LE 码元数组（不补 NUL 码元）。
 * cap = dst 能容纳的码元个数。返回写入码元数；装不下返回 -1。 */
int32_t utf8_to_utf16_c(const char *src, uint16_t *dst, uint32_t cap) {
    const uint8_t *s = (const uint8_t *)src;
    uint32_t used = 0, i = 0;
    if (cap == 0) return -1;
    for (;;) {
        uint32_t b = s[i];
        uint32_t cp = 0, adv = 1;
        if (b == 0) break;
        if (b < 0x80) {
            cp = b;
        } else if (b < 0xE0) {
            /* 110xxxxx。必须拒 overlong（C0/C1 编码的 <0x80 值），
             * 否则 "\xC0\x80" 会被当成 U+0000 —— 那是 NUL，文件名到
             * 这里就断了，后面全是垃圾。 */
            if ((b & 0x1E) == 0) {
                cp = 0xFFFD;
            } else {
                int32_t c1 = cont(s + i + 1);
                if (c1 < 0) {
                    cp = 0xFFFD;
                } else {
                    cp = ((b & 0x1F) << 6) | (uint32_t)c1;
                    adv = 2;
                }
            }
        } else if (b < 0xF0) {
            /* 1110xxxx。必须拒 surrogate 区（U+D800..DFFF）——UTF-8 里
             * 根本不允许编码代理项，放进去就是给对端埋一个落单代理。 */
            if ((b & 0x0F) == 0 && b < 0xE0) {
                cp = 0xFFFD;
            } else {
                int32_t c1 = cont(s + i + 1);
                if (c1 < 0) {
                    cp = 0xFFFD;
                } else {
                    int32_t c2 = cont(s + i + 2);
                    if (c2 < 0) {
                        cp = 0xFFFD;
                    } else {
                        uint32_t v = ((b & 0x0F) << 12) |
                                     ((uint32_t)c1 << 6) | (uint32_t)c2;
                        if ((v >= 0xD800 && v < 0xE000) || v < 0x800) {
                            cp = 0xFFFD;
                        } else {
                            cp = v;
                            adv = 3;
                        }
                    }
                }
            }
        } else if (b < 0xF8) {
            /* 11110xxx。码点必须落在 0x10000..0x10FFFF */
            int32_t c1 = cont(s + i + 1);
            if (c1 < 0) {
                cp = 0xFFFD;
            } else {
                int32_t c2 = cont(s + i + 2);
                if (c2 < 0) {
                    cp = 0xFFFD;
                } else {
                    int32_t c3 = cont(s + i + 3);
                    if (c3 < 0) {
                        cp = 0xFFFD;
                    } else {
                        uint32_t v = ((b & 0x07) << 18) |
                                     ((uint32_t)c1 << 12) |
                                     ((uint32_t)c2 << 6) | (uint32_t)c3;
                        if (v < 0x10000 || v > 0x10FFFF) {
                            cp = 0xFFFD;
                        } else {
                            cp = v;
                            adv = 4;
                        }
                    }
                }
            }
        } else {
            cp = 0xFFFD;                   /* 0xF8..0xFF：UTF-8 里不存在 */
        }
        i += adv;
        if (cp >= 0x10000) {
            /* BMP 之外 -> 高+低代理对。这是写 NTFS/exFAT/ReFS 目录项
             * 必须的：把 UTF-8 每字节当 Latin-1 塞进去会让"中"变成
             * U+00E4 U+00B8 两个字符，Windows 读出来是乱码。 */
            if (cap - used < 2) return -1;
            uint32_t v = cp - 0x10000;
            dst[used] = (uint16_t)(0xD800 + (v >> 10));
            dst[used + 1] = (uint16_t)(0xDC00 + (v & 0x3FF));
            used += 2;
        } else {
            if (cap - used < 1) return -1;
            dst[used] = (uint16_t)cp;
            used += 1;
        }
    }
    return (int32_t)used;
}

/* ---------- Levenshtein 编辑距离 ---------- */

/* 两行滚动的 DP。数组只有 2*(limit+1) 个 uint16_t，limit 上限 24 时是
 * 100 字节，远低于内核栈预算——注意「大缓冲必须 static」那条红线
 * （tools/check_stack.py 守着）说的是 KB 级的东西，这里离得远。
 *
 * 用"一块缓冲 + 奇偶下标翻转"而不是两行数组整体交换：后者在优化器
 * 眼里是可向量化的定长拷贝，Rust 侧实测踩过这个坑（把 300KB 的浮点
 * 运行时拖进内核）。这里保持三份实现同一形状，对拍时也更容易看出差异。 */
#define LEV_MAX 24
#define LEV_ROW  (LEV_MAX + 1)

int32_t levenshtein_buffer_c(uint32_t *buf, const char *a, const char *b,
                             uint32_t limit);

int32_t levenshtein_buffer_c(uint32_t *buf, const char *a, const char *b,
                             uint32_t limit) {
    uint32_t alen = 0, blen = 0;
    while (alen < 64 && a[alen]) alen++;
    while (blen < 64 && b[blen]) blen++;
    if (alen > limit || blen > limit) return (int32_t)(limit + 1);
    if (alen == 0) return (int32_t)blen;
    if (blen == 0) return (int32_t)alen;
    uint16_t *b16 = (uint16_t *)buf;      /* 两行：b16[0..25] / b16[25..50] */
    for (uint32_t j = 0; j <= blen; j++) b16[j] = (uint16_t)j;
    for (uint32_t i = 1; i <= alen; i++) {
        uint32_t cur = (i % 2) * LEV_ROW;
        uint32_t prev = cur ^ LEV_ROW;
        b16[cur] = (uint16_t)i;
        char ca = a[i - 1];
        for (uint32_t j = 1; j <= blen; j++) {
            uint16_t cost = (ca == b[j - 1]) ? 0 : 1;
            uint16_t del = b16[prev + j] + 1;
            uint16_t ins = b16[cur + j - 1] + 1;
            uint16_t sub = b16[prev + j - 1] + cost;
            uint16_t m = del < ins ? del : ins;
            b16[cur + j] = m < sub ? m : sub;
        }
    }
    return (int32_t)b16[(alen % 2) * LEV_ROW + blen];
}

/* 对外包装：栈上开 2*LEV_ROW 个 uint16_t。
 * 这一层存在的意义是让 levenshtein_c 的签名与 _rs/_zig 一致（都自己分配），
 * 免得调用点出现"C 版要传 buffer、Rust 版不用"的差异。 */
uint32_t levenshtein_c(const char *a, const char *b, uint32_t limit) {
    uint16_t buf[2 * LEV_ROW];
    int32_t r = levenshtein_buffer_c((uint32_t *)(void *)buf, a, b, limit);
    return (r < 0) ? limit + 1 : (uint32_t)r;
}
