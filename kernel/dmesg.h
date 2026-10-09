/*
 * dmesg.h - 内核日志环形缓冲（步骤 2b）
 *
 * klog 系（kernel.c）的输出镜像进 32KB 环形缓冲，GUI/重启后历史不再丢。
 * 行结构：完整行入缓（超长截断 240B），行数计数，满则丢最旧行。
 *
 * panic 屏（panic.c）附带最近 4 行；shell dmesg 命令全量回看。
 */
#ifndef DMESG_H
#define DMESG_H

#include "types.h"

/* 一行写入环形缓冲 **并** 镜像到串口（只记 dmesg、不上屏的调用方用） */
void dmesg_write(const char *line);

/* 一行只写入环形缓冲，**不**碰串口（已经上过屏的调用方用：klog / st_report）。
 * 两者都写会让串口日志每一行出现两次。 */
void dmesg_record(const char *line);

/* 回看：把最后 n 行（<=0 = 全部）逐行经 putc 输出；返回行数 */
int dmesg_dump(void (*putchar_fn)(char), int n);

/* 最近 min(n, 总行数) 行拷入 out（每行以 \n 结尾）；返回拷贝字节数 */
int dmesg_tail(char *out, uint32_t outsz, int n);

/* 总行数 */
uint32_t dmesg_lines(void);

#endif
