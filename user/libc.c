/*
 * user/libc.c - 用户态极简 libc 实现（用户态生态 ③）
 *
 * freestanding：不链接任何标准库，只用编译器内建类型 + crt0.asm 提供的
 * write/read/open/close/lseek/_exit/sockcall。本文件实现：
 *   - 通用陷入 syscall() 与 fork/waitpid/pipe/getpid/exit 薄封装
 *   - 字符串/内存小工具：strlen/strcmp/strncmp/memcpy/memset/itoa
 *   - 终端打印辅助：puts / putdec
 *
 * 所有函数保持最小且跨程序行为一致，方便把 helloworld 类程序里手抄的
 * kstrlen/kputs/kputdec 收拢到这里而不改变任何一行屏幕输出。
 */
#include "libc.h"

/* ---- 系统调用薄封装 ---- */

int syscall(int num, int a, int b, int c) {
    int r;
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(num), "b"(a), "c"(b), "d"(c)
                     : "memory");
    return r;
}

void exit(int code) {
    _exit(code);          /* crt0.asm 的 _exit 走 SYS_EXIT，永不返回 */
}

int fork(void) {
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(SYS_FORK) : "memory");
    return r;
}

int waitpid(int pid, int *status, int options) {
    int r;
    /* ebx=pid, ecx=status 用户指针, edx=options。
     * 内核返回：>=0 退出码（已回收，*status 写入退出码，返回被等 pid）；
     *           0 表示 WNOHANG 且子进程仍在跑（未回收）；
     *           -1 无效 pid / 已被收走。
     * 这里原样透传，调用方按"成功返回被等 pid"判定即可（与旧 kwaitpid 一致）。 */
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(SYS_WAITPID), "b"(pid), "c"(status), "d"(options)
                     : "memory");
    return r;
}

int pipe(int fds[2]) {
    int r;
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(SYS_PIPE), "b"(fds)
                     : "memory");
    return r;
}

int getpid(void) {
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(SYS_GETPID) : "memory");
    return r;
}

/* ---- 字符串 / 内存 ---- */

unsigned int strlen(const char *s) {
    unsigned int n = 0;
    while (s && s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    if (a == 0 && b == 0) return 0;
    if (a == 0) return -1;
    if (b == 0) return 1;
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, unsigned int n) {
    if (n == 0) return 0;
    while (n-- && *a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

void *memcpy(void *dst, const void *src, unsigned int n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (unsigned int i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void *memset(void *dst, int v, unsigned int n) {
    unsigned char *d = (unsigned char *)dst;
    unsigned char c = (unsigned char)v;
    for (unsigned int i = 0; i < n; i++) d[i] = c;
    return dst;
}

void itoa(unsigned int v, char *out) {
    char b[12];
    int n = 0;
    if (v == 0) b[n++] = '0';
    while (v) { b[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    int m = 0;
    while (n) out[m++] = b[--n];
    out[m] = '\0';
}

/* ---- 终端打印辅助 ---- */

void puts(const char *s) {
    write(1, s, strlen(s));
}

/* 十进制打印（正数/0/负数都支持，负数带 '-'），逐字符写出，
 * 与旧 kputdec 输出完全一致。 */
void putdec(int v) {
    unsigned u = (v < 0) ? (unsigned)(-v) : (unsigned)v;
    char b[12];
    int n = 0;
    if (v < 0) write(1, "-", 1);
    if (u == 0) b[n++] = '0';
    while (u) { b[n++] = (char)('0' + (u % 10u)); u /= 10u; }
    while (n) { char c = b[--n]; write(1, &c, 1); }
}
