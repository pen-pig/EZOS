/*
 * elf.h - ELF32 加载器（步骤 5b）
 *
 * 安全边界（这是本模块最重要的部分）：
 * ELF 文件是**不可信输入**。一个恶意/损坏的 ELF 可以声明任意 p_vaddr，
 * 比如"把 .text 装到 0x00100000（内核 .bss.hi）"或"p_memsz = 4GB"。
 * 加载器必须在动页表之前把这些全部挡掉，否则等于给用户态一张
 * "往任意地址写任意内容"的通行证。校验失败一律拒绝并给出原因，
 * 绝不"尽力加载"。
 *
 * W^X：32-bit 非 PAE 没有 NX 位，无法禁止执行，但**可写性**可控——
 * 没有 PF_W 的段（典型是 .text）映射为只读，这样即便用户程序有 bug
 * 或被人利用，也没法就地改写自己的代码段。
 */
#ifndef ELF_H
#define ELF_H

#include "types.h"

/* ELF32 常量（System V gABI + i386 psABI） */
#define ELFCLASS32      1u
#define ELFDATA2LSB     1u
#define ET_EXEC         2u
#define EM_386          3u
#define EV_CURRENT      1u
#define PT_LOAD         1u
#define PF_X            0x1u
#define PF_W            0x2u
#define PF_R            0x4u

#define ELF_MAX_SEG     8u      /* 支持的 PT_LOAD 段数上限 */

/* 一次加载的结果：退出/换映像时要能完整回收 */
typedef struct {
    uint32_t entry;                     /* 入口虚拟地址（e_entry） */
    uint32_t brk;                       /* 映像结束地址（将来 sbrk 的起点） */
    struct {
        uint32_t va;                    /* 起始虚拟地址（页对齐） */
        uint32_t pages;                 /* 页数 */
        uint32_t phys;                  /* 首物理页（连续分配，便于回收） */
    } seg[ELF_MAX_SEG];
    uint32_t nseg;
} elf_image_t;

/* 只做头部/程序头表校验，不修改任何状态。合法返回 0，否则非 0，
 * 并通过 *why 给出人可读原因（直接打到 shell / dmesg）。 */
int elf_validate(const uint8_t *buf, uint32_t size, const char **why);

/* 完整加载：分配物理页、映射、拷文件段、清 bss、按 W^X 收紧权限。
 * img 出参记录回收所需信息。失败时保证"要么全成功，要么不残留"。 */
int elf_load(const uint8_t *buf, uint32_t size, elf_image_t *img, const char **why);

/* 回收 elf_load 分配的全部资源（页表映射 + 物理页） */
int elf_unload(const elf_image_t *img);

/* 自检：正常映像 + 各类畸形/越界映像都必须被正确处置 */
typedef void (*elf_puts_fn)(const char *s);
int elf_selftest(elf_puts_fn out);

#endif
