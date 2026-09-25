/*
 * user/hello.c - 第一个用户态 ELF 程序（步骤 5c）
 *
 * 与内核的边界：
 *   - 没有 libc，唯一的内核入口是 int 0x80（crt0.asm 里包装成 write/read/_exit）
 *   - 运行在 ring3：任何越权访问（内核地址、只读段写入）都会 #PF 并被
 *     panic 捕获，不会污染内核
 *   - 链接到 0x00400000（USER_IMAGE_BASE），由内核 exec 从磁盘加载
 *   - 复用 user/libc.h 提供的 puts/putdec/strlen（打印输出与重构前逐字节一致）
 */
#include "libc.h"

typedef unsigned int u32;     /* libc 用 unsigned int，这里保留别名仅用于调用点类型 */

int umain(int argc, char **argv) {
    puts("hello from ELF user program!\n");
    puts("argc = ");
    putdec((u32)argc);
    puts("\n");
    for (int i = 0; i < argc; i++) {
        puts("argv[");
        putdec((u32)i);
        puts("] = ");
        puts(argv[i] ? argv[i] : "(null)");
        puts("\n");
    }
    /* 越权写自检：写只读的 .text 段必须触发 #PF（有意为之则本行应注释掉） */
    puts("argv terminator ok: ");
    puts(argv[argc] == 0 ? "yes\n" : "NO\n");
    return 42;
}
