/*
 * user/fdleak.c - 故意"忘关"文件句柄的用户进程（步骤 6c 验证用）
 *
 * 打开 FDLEAK.TXT 写入，**不调用 close** 就直接 _exit。
 * 内核的 process_exit() 必须替它关掉所有句柄——dirty 的还要落盘，
 * 否则这既丢数据又泄漏内核堆（整文件句柄缓冲）。
 *
 * E2E 判据：进程退出后 `cat FDLEAK.TXT` 能看到内容。
 */
#include "libc.h"

#define O_WRONLY 1

int umain(int argc, char **argv) {
    (void)argc; (void)argv;
    int fd = open("FDLEAK.TXT", O_WRONLY);
    if (fd < 0) {
        puts("fdleak: open failed\n");
        return 1;
    }
    const char *msg = "leak test from ring3\n";
    if (write(fd, msg, strlen(msg)) != (int)strlen(msg)) {
        puts("fdleak: write failed\n");
        return 2;
    }
    puts("fdleak: wrote without close\n");
    return 5;                       /* 故意不 close：交给内核代关 */
}
