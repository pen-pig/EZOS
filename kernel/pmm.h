/*
 * pmm.h - 物理页帧分配器（步骤 5a）
 *
 * 为什么需要：步骤 3 的分页只做 identity 映射 + 手工 map，没有"给我一页
 * 空闲物理内存"的能力。步骤 5 的 ELF 加载要为每个段分配物理页，步骤 6 的
 * 进程创建/退出还要能回收——所以先补上页帧管理。
 *
 * 设计：位图（1 bit / 4KB 页）。管理区 0x00A00000-0x01000000（10-16MB，
 * 6MB = 1536 页），位图本身 192 字节，放在 .bss（不占镜像）。
 *
 * 范围选择依据（与 gdt.h 的布局注释一致）：
 *   2MB 页表池 | 3MB pagetest scratch | 4-8MB 用户虚拟空间（映射到本池）
 *   | 9MB 陷入栈 | **10-16MB 本池** | 16MB GUI 背缓冲
 *
 * 刻意不做的：
 *   - 不做空闲链表/伙伴系统：单任务 + 页量小，位图线性扫描足够且零碎片风险
 *   - 不做换页（步骤 6 的事）
 */
#ifndef PMM_H
#define PMM_H

#include "types.h"

#define PMM_PAGE_SIZE   4096u

/* 管理区：物理 10MB - 16MB */
#define PMM_POOL_START  0x00A00000u
#define PMM_POOL_END    0x01000000u
#define PMM_POOL_PAGES  ((PMM_POOL_END - PMM_POOL_START) / PMM_PAGE_SIZE)

/* 初始化（kernel.c 在 paging_init 之后调用一次） */
void pmm_init(void);

/* 分配一页物理内存，返回物理地址；失败返回 0。
 * 返回的页内容**未清零**——调用方若要把页面暴露给用户态（否则会泄漏
 * 上一任使用者的内核数据，这是经典的内核信息泄漏漏洞），必须自己清零。 */
uint32_t pmm_alloc_page(void);

/* 分配 n 个**连续**物理页（ELF 段需要连续以便 memcpy），失败返回 0 */
uint32_t pmm_alloc_pages(uint32_t n);

/* 释放（n 必须与分配时一致；越界/重复释放会被拦下并返回 -1） */
int pmm_free_page(uint32_t phys);
int pmm_free_pages(uint32_t phys, uint32_t n);

/* 统计：总数 / 已用 / 空闲
 * 注意命名：free_count 是"空闲页数"统计，pmm_free_pages(phys,n) 是释放——
 * 两者不能同名（曾因同名导致 conflicting types）。 */
uint32_t pmm_total_pages(void);
uint32_t pmm_used_pages(void);
uint32_t pmm_free_count(void);

/* 自检（shell pmmtest）：分配-释放-回收一致性 + 零页泄漏检查 */
typedef void (*pmm_puts_fn)(const char *s);
int pmm_selftest(pmm_puts_fn out);

#endif
