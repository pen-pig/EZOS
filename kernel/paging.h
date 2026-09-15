/*
 * paging.h - 32-bit 分页（步骤 3）
 *
 * 当前策略：identity mapping（虚拟地址 == 物理地址），覆盖
 *   0x00000000 - 0x02000000（32MB）：低 1MB（VGA/BIOS/实模式残区）、
 *   内核镜像 0x10000-0x70000、.bss.hi 1-2MB（堆 + dmesg）、
 *   GUI 背缓冲 16MB 起（gfxwin GW_BB_ADDR）、页表自身。
 *   另按 boot.asm 探测到的 VBE LFB（通常 0xE0000000）映射 8MB 窗口。
 *
 * 价值：#PF 语义落地（缺页地址/读写/权限可判），并为步骤 4 的 ring3
 * 用户页（PTE_US）与步骤 6 的按需换页铺路。identity mapping 让现有
 * 全部代码零改动继续运行。
 *
 * 页表物理位置：0x200000（2MB）起（低 1MB 被镜像/.bss/栈占满，
 * 2MB-16MB 是 .bss.hi 与 GUI 背缓冲之间的空闲 RAM）。
 */
#ifndef PAGING_H
#define PAGING_H

#include "types.h"

#define PAGE_SIZE        4096u

/* 页表项/页目录项标志（低 12 位，高位为物理页帧） */
#define PTE_P            0x001u     /* Present */
#define PTE_RW           0x002u     /* 可写（0 = 只读） */
#define PTE_US           0x004u     /* 用户态可访问（0 = 仅 ring0） */
#define PTE_PWT          0x008u     /* Write-through */
#define PTE_PCD          0x010u     /* Cache disable */
#define PTE_A            0x020u     /* Accessed（CPU 置位） */
#define PTE_D            0x040u     /* Dirty（CPU 置位） */
#define PTE_PS           0x080u     /* 仅 PDE：4MB 大页 */
#define PTE_G            0x100u     /* Global（CR4.PGE 时 TLB 不刷） */

/* 页结构物理位置：0x200000（2MB）起。
 * 低内存不可用：镜像固定占 0x10000-0x70000，.bss 实际延伸到 ~0x89F60
 * （idt_entries 就在 0x70C00），栈自 0x90000 向下增长，0xA0000 起是 VGA。
 * 2MB-16MB 是 .bss.hi 上界（linker.ld ASSERT <= 0x200000）与 GUI 背缓冲
 * （GW_BB_ADDR 0x1000000）之间唯一的大块连续空闲 RAM。 */
#define PAGING_PD_ADDR   0x00200000u
#define PAGING_PT_BASE   0x00201000u
#define PAGING_PT_MAX    16u                     /* 页表池容量（64KB） */

/* identity 映射上界（不含）：32MB */
#define PAGING_IDENTITY_END  0x02000000u

/* LFB 映射窗口大小：8MB（1280x1024x16bpp = 2.6MB，留足余量） */
#define PAGING_LFB_WINDOW    0x00800000u

/* 内核公共权限：ring0 读写。步骤 4 起用户页再加 PTE_US。 */
#define PAGING_KRN_FLAGS     (PTE_RW)

/* 用户页权限：ring3 可读写。步骤 4 起用户代码/栈用它。
 * 注意：x86 32-bit 非 PAE 模式下没有 NX 位，RW 即可执行；
 * 真正的"数据不可执行"要等 PAE/NX 或分段限长方案。 */
#define PAGING_USR_FLAGS     (PTE_RW | PTE_US)

/* MMIO（VBE LFB）权限：ring0 读写 + PCD/PWT 关闭缓存。
 * 帧缓冲是显存而非普通 RAM，按默认 WB 映射在 QEMU 下看似正常，
 * 但真机上写操作可能滞留在 cache 不落显存（表现为花屏/不刷新）。
 * 这里用最保守的 uncacheable；若将来引入 PAT 可改 write-combining。 */
#define PAGING_MMIO_FLAGS    (PTE_RW | PTE_PCD | PTE_PWT)
#define PAGING_RO_FLAGS      (0u)

/* 初始化：建 PD/PT、identity map、映射 LFB、置 CR3、开 CR0.PG。
 * 只能调用一次（重复调用直接返回）。返回 0 成功。 */
int paging_init(void);

/* CR0.PG 是否已置位 */
int paging_enabled(void);

/* 当前页目录物理地址（CR3 高 20 位） */
uint32_t paging_cr3(void);

/* 切换当前页目录（步骤 6c：每进程地址空间）。
 * mov cr3 会隐式全刷非全局 TLB——这正是任务切换时要的语义：换进程后
 * 上一个进程的用户区映射绝不能被残留 TLB 命中。
 * 内核区（identity + LFB）在**所有**进程页目录里都是同一批共享页表，
 * 所以换 CR3 后内核自己的代码/数据照常可访问。 */
void paging_switch_cr3(uint32_t pd_phys);

/* 映射单页。virt/phys 需 4KB 对齐；flags 取 PTE_* 低 12 位组合。
 * 已映射则覆盖（并 invlpg）。返回 0 成功，-1 页表池耗尽。 */
int paging_map(uint32_t virt, uint32_t phys, uint32_t flags);

/* 解除映射（置 PTE 为 0 + invlpg）。未映射则无操作。 */
void paging_unmap(uint32_t virt);

/* 页表查询：返回 0 成功（*out 得物理基址，*flags 得低 12 位属性），-1 未映射 */
int paging_query(uint32_t virt, uint32_t *out_phys, uint32_t *out_flags);

/* 统计：已映射页数 / 已用页表数 / 页目录物理地址 */
uint32_t paging_mapped_pages(void);
uint32_t paging_used_tables(void);

/* 自检（shell pagetest）：identity 一致性 + 映射/读写/解映射。
 * 逐行经 out 回调输出；返回 0 表示全部断言通过。 */
typedef void (*paging_puts_fn)(const char *s);
int paging_selftest(paging_puts_fn out);

#endif
