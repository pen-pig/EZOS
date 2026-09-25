/*
 * user/forktest.c - fork / waitpid 的用户态验证程序（用户态生态 ②）
 *
 * 为什么需要它：fork 的正确性没法在内核里自检——fork 要求父进程是
 * 真正的用户进程（is_user + 独立页目录），而 utest 那种内建路径跑在
 * task 0 上，没有独立地址空间，fork 只会返回 -1。所以必须从盘上 exec
 * 一个真 ELF 来验证。
 *
 * 验证点：
 *   1) 父进程拿到子 pid（>0），子进程拿到 0
 *   2) 子进程有自己独立的地址空间（改自己的全局变量，父进程看不到）
 *   3) waitpid 能阻塞等到子进程并取回退出码
 *
 * 已改为复用 user/libc.h 的 fork/waitpid/puts/putdec/strlen，输出不变。
 */
#include "libc.h"

/* 放在 .data：子进程写它，父进程读它。两边值不同才说明地址空间真的独立 */
static int g_shared_probe = 100;

int umain(int argc, char **argv) {
    (void)argc; (void)argv;
    puts("forktest start\n");

    int pid = fork();
    /* 两个进程都会走到这里：打印各自拿到的返回值，用来判定
     * "父得子 pid、子得 0" 这条契约是否成立。
     * 用互不相似的大写词——OCR 容易把相近的行读串。 */
    puts("TAG RET ");
    putdec(pid);
    puts("\n");
    if (pid < 0) {
        puts("TAG FORKFAILED\n");
        _exit(1);
    }
    if (pid == 0) {
        g_shared_probe = 555;
        puts("TAG CHILD\n");
        _exit(7);
    }

    puts("TAG PARENT PID ");
    putdec(pid);
    puts("\n");

    int status = -1;
    int rc = waitpid(pid, &status, 0);
    puts("TAG WAIT RC ");
    putdec(rc);
    puts(" ST ");
    putdec(status);
    puts("\n");

    /* 地址空间独立性的判据：父进程这里必须还是 100，不能被子进程改成 555 */
    puts("forktest probe=");
    putdec(g_shared_probe);
    puts("\n");

    if (rc == pid && status == 7 && g_shared_probe == 100) {
        puts("forktest ok\n");
        _exit(0);
    }
    puts("forktest bad\n");
    _exit(2);
}
