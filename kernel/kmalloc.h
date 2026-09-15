/*
 * kmalloc.h - 内核堆分配器（步骤 2）
 *
 * 设计：.bss.hi 高内存静态池 + 空闲块链表（first-fit + 分裂）
 *   - 池 384KB @ .bss.hi，16 字节对齐
 *   - 块头 8B：{ size(含头,低位标志) } + { next }（size&1=1 表示已分配）
 *   - 分配：first-fit，尾部剩余 >=24B（头+最小块）则分裂
 *   - 释放：魔数校验 + 相邻空闲块合并（立即合并，无延迟）
 *   - 越界检测：块头尾各 4B 魔数，kfree 时校验，坏即 panic 上下文标注
 *
 * 非目标（现阶段）：多池、per-CPU、SLAB、缺页增长。够用就好。
 */
#ifndef KMALLOC_H
#define KMALLOC_H

#include "types.h"

void kmalloc_init(void);

/* 返回 16 字节对齐指针；失败返回 NULL（大小为 0 也返回 NULL） */
void *kmalloc(uint32_t size);

/* 释放 kmalloc 返回的指针；空指针静默；魔数不符进 panic 上下文停机 */
void kfree(void *ptr);

/* 调试：总池 / 已用 / 最大连续空闲（shell mem 命令用） */
uint32_t kmalloc_total(void);
uint32_t kmalloc_used(void);
uint32_t kmalloc_largest_free(void);

#endif
