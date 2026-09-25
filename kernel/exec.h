/*
 * exec.h - 从文件系统装载并运行用户程序（步骤 5d）
 *
 * 这是"用户程序"与"内核内建机器码演示"的分界线：
 *   utest      走的是 usermode_run_demo()，程序是内核里的一个字节数组
 *   exec       走的是 exec_file()，程序是盘上的 ELF32 文件
 *
 * 当前语义（步骤 6 有调度器之前不会变）：
 *   - 同步执行：exec_file() 直到用户程序 SYS_EXIT 才返回
 *   - 执行完毕即回收用户映像与用户栈——现在没有进程表，留着也没意义
 *   - 失败一律返回负值，绝不"带着半截状态继续跑"
 */
#ifndef EXEC_H
#define EXEC_H

#include "types.h"

/* 单个 ELF 映像的最大字节数。受内核堆（384KB）限制：ELF 要先整块读进
 * 内核缓冲才能解析程序头。将来有了按段流式读取就不再需要这个上限。 */
#define EXEC_MAX_IMAGE   (64u * 1024u)

/* 命令行参数上限（不含 argv[0] 的终止 NULL） */
#define EXEC_MAX_ARGV    8

/*
 * 读取 -> 校验 -> 装载 -> 切入 ring3 -> 回收。
 *
 * name : 根目录下的文件名，如 "HELLO.ELF"
 * args : 命令行剩余部分（空格分隔），可为 NULL 或空串
 * why  : 失败原因（可为 NULL）
 *
 * 返回：>=0 为用户程序退出码；负值含义：
 *   -1 参数错误 / 文件系统未挂载
 *   -2 文件不存在或读取失败
 *   -3 文件过大或内核堆不足
 *   -4 ELF 校验/装载失败（非法 ELF、越界段、页不足）
 *   -5 用户栈建立失败（命令行过长或映射失败）
 */
int exec_file(const char *name, const char *args, const char **why);

/*
 * 后台启动：装载并切入 ring3，但**不等待**子进程结束就返回。
 * 成功返回 0 并把子进程 pid 写入 *out_pid；失败返回 exec_file 同款负值
 * （*out_pid 未定义）。调用方需自行（比如在每次提示符前用 WNOHANG）
 * 收割退出的子进程，否则其 ZOMBIE 会占着 MAX_TASKS 的槽位。
 */
int exec_file_bg(const char *name, const char *args, int *out_pid);

#endif
