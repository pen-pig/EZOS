/*
 * mem.c - 内核的 memcpy / memset / memcmp。
 *
 * **为什么需要这个文件**：Rust 的 core 在链接期会引用这三个符号（数组
 * 交换、切片拷贝、字符串比较都会生成它们），而内核是 -ffreestanding，
 * 没有 libc 可链。第一次接 Rust 模块时就踩过：编译能过，一链就报
 * "undefined reference to `memcpy'"，而且报错信息指向 core 内部某个
 * 浮点格式化函数，看不出是自己少提供了符号。
 *
 * 另一个动机是**去重**。在加这个文件之前，仓库里已经有 4 份同样的
 * 逐字节拷贝循环，各自 static 在 elf.c / exec.c / nvme.c / task.c 里，
 * 外加若干名字各异的 memset。新增第 5 份只会让"同一件事写了几遍"这个
 * 问题更严重。现在它们统一调这里的实现。
 *
 * **注意这不是可有可无的**。Rust 的 core 内部也用 memcpy/memset/memcmp
 * （比如切片拷贝），链接 libezos_rs.a 时必须有这些符号可解析。删掉本文件
 * 会在链接期报 undefined reference，报错信息指向 core 内部某个浮点格式化
 * 函数，完全看不出是自己少提供了符号（踩过）。
 *
 * 语义严格照 libc 写，别自作聪明：
 *   * memcpy 不保证 dst/src 不重叠（重叠是未定义行为，调用方自己负责）
 *   * memset 的 v 会先转成 unsigned char
 *   * memcmp 按 **unsigned char** 逐字节比，返回值只保证符号
 *     （不是"差值"——很多老代码依赖差值，那个习惯在 C 标准里是错的）
 */
#include "types.h"
#include "mem.h"

void *memcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    /* 逐字节：没有 libc 也没有对齐保证，而且内核里绝大多数拷贝都很短
     * （目录项、结构体）。真要搬几 MB 的数据，内核里也有 DMA 通道
     * 可用，不该指望这个。 */
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void *memset(void *dst, int v, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    uint8_t b = (uint8_t)v;              /* 必须截断到 8 位 */
    for (uint32_t i = 0; i < n; i++) d[i] = b;
    return dst;
}

int memcmp(const void *a, const void *b, uint32_t n) {
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    for (uint32_t i = 0; i < n; i++) {
        if (x[i] != y[i]) return (x[i] < y[i]) ? -1 : 1;
    }
    return 0;
}
