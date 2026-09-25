/*
 * user/spin.c - 忙等自旋的用户进程（步骤 6c 验证用）
 *
 * 作用：让一个用户进程在 ring3 跑好几秒，期间 shell 阻塞在 wait 上。
 * 能观察到"它还在跑、且每 tick 都在被 PIT 抢占后继续"，就说明
 * 用户进程是真被调度器管理的任务，而不是借 shell 栈同步执行的一段代码。
 *
 * 每轮打印一次 tick：E2E 在它跑到一半时截图，靠 tick 数证明它确实
 * 在推进（而不是卡死在某处）。
 */
#include "libc.h"

typedef unsigned int u32;

int umain(int argc, char **argv) {
    (void)argc; (void)argv;
    puts("spin: start\n");
    for (int t = 1; t <= 4; t++) {
        volatile u32 acc = 0;
        for (u32 i = 0; i < 120000000u; i++) acc += i;   /* volatile：防止被优化掉 */
        puts("spin: tick ");
        putdec((u32)t);
        puts(" (");
        putdec(acc & 0xFFFFu);
        puts(")\n");
    }
    puts("spin: done\n");
    return 7;
}
