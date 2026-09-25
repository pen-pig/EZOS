/*
 * user/fdtest.c - 文件描述符端到端测试程序（步骤 6b）
 *
 * 在 ring3 里走完整的 fd 语义：open/read/lseek/close，以及写模式建文件、
 * 落盘、读回验证，最后验证只读句柄拒绝写。所有结果打到 stdout。
 *
 * 与内核的边界：没有 libc，唯一内核入口是 int 0x80（crt0.asm 里包装成
 * write/read/open/close/lseek/_exit）。open 的文件名与缓冲都是用户指针，
 * 内核会逐一做 user_range_ok 校验——越界即返回 -1，不会碰内核内存。
 *
 * 已改为复用 user/libc.h 的 puts/putdec/strlen，打印输出与重构前逐字节一致。
 */
#include "libc.h"

#define O_RDONLY 0
#define O_WRONLY 1
#define SEEK_SET 0
#define SEEK_END 2

int umain(int argc, char **argv) {
    (void)argc; (void)argv;
    puts("fd test: open/read/lseek/write/close\n");

    /* 1) 只读打开 README.TXT */
    int fd = open("README.TXT", O_RDONLY);
    if (fd < 0) { puts("FAIL: open README.TXT\n"); return 1; }
    puts("open README.TXT -> fd "); putdec((unsigned int)fd); puts("\n");

    /* 2) 读出全部并回显 */
    char buf[128];
    int n = read(fd, buf, sizeof(buf));
    puts("read -> "); putdec((unsigned int)n); puts(" bytes: ");
    if (n > 0) write(1, buf, (unsigned int)n);
    puts("\n");

    /* 3) lseek SEEK_END 取大小，再回绕读前 8 字节 */
    int sz = lseek(fd, 0, SEEK_END);
    puts("lseek END -> "); putdec((unsigned int)sz); puts("\n");
    lseek(fd, 0, SEEK_SET);
    char b8[8];
    int n8 = read(fd, b8, 8);
    puts("first 8 bytes: ");
    if (n8 > 0) write(1, b8, (unsigned int)n8);
    puts("\n");
    close(fd);

    /* 4) 写模式建文件 + 写内容 + close 落盘 */
    fd = open("FDTEST.TXT", O_WRONLY);
    if (fd < 0) { puts("FAIL: open FDTEST.TXT (write)\n"); return 1; }
    const char *msg = "written from ring3 fdtest\n";
    int w = write(fd, msg, strlen(msg));
    puts("write -> "); putdec((unsigned int)w); puts(" bytes, closed\n");
    close(fd);

    /* 5) 重新只读打开，读回验证落盘结果 */
    fd = open("FDTEST.TXT", O_RDONLY);
    char rb[64];
    int rn = read(fd, rb, sizeof(rb));
    puts("read back "); putdec((unsigned int)rn); puts(" bytes: ");
    if (rn > 0) write(1, rb, (unsigned int)rn);
    puts("\n");
    close(fd);

    /* 6) 只读句柄写应被拒绝（返回 -1，映射为 0 打印） */
    fd = open("README.TXT", O_RDONLY);
    int wrc = write(fd, "x", 1);
    puts("write to read-only fd rejected: ");
    puts(wrc == -1 ? "yes\n" : "NO\n");
    close(fd);

    puts("fd test done\n");
    return 0;
}
