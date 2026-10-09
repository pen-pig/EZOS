/*
 * fsck.h - 卷一致性检查（read-only）统一报告结构
 *
 * 为什么要有这个东西：本项目所有可写 FS（FAT12/16/32、exFAT）的元数据更新
 * 都没有日志、没有两阶段提交。断电或崩溃发生在"写了 FAT 还没写位图"这种
 * 中间态时，卷会留下**内核自己看不出来**的损伤——因为 EZOS 读文件时只跟
 * 目录项里的首簇，从不校验 FAT/位图与目录树是否自洽。宿主一挂上去就是
 * chkdsk 报错（docs/EZOS_深度分析与功能建议.md 里列的是 P0）。
 *
 * fsck 不改任何东西（这一版是纯检测），只回答三个问题：
 *   1) 每条簇链走得通吗？（越界 / 成环 / 比 size 短）
 *   2) 有没有一个簇被两条链同时引用？（交叉链接 = 互相覆盖的数据丢失）
 *   3) 分配表里"已占用"的簇，是不是都有目录项指着它？（孤儿簇 = 泄漏）
 *
 * 做法是一张 reachable 位图：从根目录出发把每条链经过的簇置位，扫完再拿
 * 分配表（FAT12/16/32 用 FAT，exFAT 用 Allocation Bitmap）逐簇对拍。
 * 位图走 kmalloc，不放栈也不放 .bss.hi——后者只剩十几 KB，塞不下大卷。
 */
#ifndef FSCK_H
#define FSCK_H

#include "types.h"

/* 记录到的问题簇号上限（只在报告里列前几个，不做完整清单） */
#define FSCK_BAD_MAX 8

typedef struct {
    uint32_t clusters_total;    /* 数据簇总数 */
    uint32_t bad_chain;         /* 链上出现非法簇号（越界 / 坏簇标记） */
    uint32_t loop;              /* 链成环（步数超过簇总数仍未结束） */
    uint32_t truncated;         /* 链比目录项里的 size 短（文件被截短） */
    uint32_t crosslink;         /* 一个簇被两条链引用 */
    uint32_t orphan;            /* 分配表说占用，但没有任何目录项引用 */
    uint32_t free_but_used;     /* 目录树引用了分配表标为空闲的簇 */
    uint32_t bad_entry;         /* 目录项本身不合法（类型/长度越界） */
    uint32_t first_bad[FSCK_BAD_MAX];
    int      first_bad_n;
} fsck_report_t;

void fsck_bit_set(uint8_t *bm, uint32_t i);
int  fsck_bit_get(const uint8_t *bm, uint32_t i);
void fsck_record_bad(fsck_report_t *r, uint32_t cluster);

/* 各后端的扫描实现（定义在 fat.c / exfat.c，复用那里的 static 助手）：
 * 返回 0 = 扫完（问题数看报告），-1 = 未挂载，-2 = 内存不足，-3 = 读盘失败 */
int fat_fsck(fsck_report_t *r);
int exfat_fsck(fsck_report_t *r);

/* shell 命令入口 */
void cmd_fsck(const char *args);

#endif
