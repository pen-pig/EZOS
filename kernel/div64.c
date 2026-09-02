/*
 * div64.c - 64 位除法的 libgcc 辅助函数（freestanding 实现）
 *
 * i686 无原生 64 位除法指令，gcc 对 unsigned long long 的 / 和 %
 * 会生成对 __udivdi3/__umoddi3 的调用；内核用裸 ld 链接（无 libgcc），
 * 故自带移位减法实现（二进制长除法）。
 *
 * refs:
 *   - GCC libgcc2.c __udivmoddi4（算法等价）
 */

typedef unsigned long long u64;

static u64 udivmod64(u64 num, u64 den, u64 *rem) {
    u64 quot = 0;
    int shift = 0;

    if (den == 0) {                 /* 除零：返回 0，内核侧各驱动已有边界检查 */
        if (rem) *rem = 0;
        return 0;
    }
    if (num < den) {
        if (rem) *rem = num;
        return 0;
    }

    /* 除数左移至不超过被除数的最大 2^k 倍（防溢出：最高位已置位时停） */
    while (shift < 63 && (den & 0x8000000000000000ull) == 0 &&
           (den << 1) <= num) {
        den <<= 1;
        shift++;
    }
    /* 逐位恢复：从最高有效位到第 0 位 */
    for (; shift >= 0; shift--) {
        quot <<= 1;
        if (num >= den) {
            num -= den;
            quot |= 1;
        }
        den >>= 1;
    }
    if (rem) *rem = num;
    return quot;
}

u64 __udivdi3(u64 num, u64 den) {
    u64 r;
    return udivmod64(num, den, &r);
}

u64 __umoddi3(u64 num, u64 den) {
    u64 r;
    udivmod64(num, den, &r);
    return r;
}

u64 __udivmoddi4(u64 num, u64 den, u64 *rem) {
    return udivmod64(num, den, rem);
}
