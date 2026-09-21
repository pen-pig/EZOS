/*
 * syscall.h - 系统调用 + 用户态（步骤 4）
 *
 * 当前形态（步骤 5 有 ELF 加载器后会演进）：
 *   - 陷入方式：int 0x80，IDT 门 DPL=3，是用户态进入内核的唯一合法入口
 *   - 参数：eax=调用号，ebx/ecx/edx=参数；返回值在 eax
 *   - 用户空间：固定的两页（代码 + 栈），用户程序是内核内建的机器码
 *     （还没有 ELF 加载器，也没有 fork/调度，故采用"同步切入、exit 返回"模型：
 *      enter_usermode() 不返回直到用户程序 SYS_EXIT）
 *
 * 安全边界：所有用户传入的指针必须经 user_range_ok() 校验——要求范围内
 * 每一页都已映射且带 PTE_US。缺了这一步，用户态就能借系统调用读写内核内存。
 */
#ifndef SYSCALL_H
#define SYSCALL_H

#include "types.h"

/* 调用号（沿用 Linux i386 的编号，便于将来对齐） */
#define SYS_READ   0u
#define SYS_WRITE  1u
#define SYS_EXIT   2u
#define SYS_OPEN   5u
#define SYS_CLOSE  6u
#define SYS_LSEEK  19u
/* socketcall 复用器（沿用 Linux i386 的 102 号）：
 * ebx=子命令（net.h SC_*），ecx=用户态 u32 args[5]。
 * 3 参数 ABI 不变，子命令的 5 个槽位经一个结构体传入。 */
#define SYS_SOCKCALL 102u

#define SYSCALL_VECTOR  0x80u

/* fd 表由 fd.c / task.c 提供（每任务一张，见 task.h 的 task_t.fds）。
 * 标准流 0/1/2 的定义见 fd.h（FD_STDIN/FD_STDOUT/FD_STDERR）。 */

/* 用户空间布局（步骤 5 起）：
 *   0x00400000 - 0x00800000  用户映像区（ELF 段 + 将来的堆）
 *   0x00800000               用户栈顶（向下增长）
 *
 * 注意：这是**虚拟**地址。4MB-8MB 在 identity 映射下原本指向同名物理地址，
 * 但 ELF/栈的页会由 pmm 重新映射到物理页池（10-16MB），因此用户虚拟地址
 * 与物理地址不再相等——凡是要访问用户内存的代码都必须走它的虚拟地址。
 */
#define USER_CODE_VA    0x00400000u              /* ELF 映像装载基准 */
#define USER_IMAGE_BASE 0x00400000u
#define USER_IMAGE_END  0x00800000u              /* 映像区上界（不含） */
#define USER_STACK_TOP  0x00800000u              /* 栈顶，向下增长 */
#define USER_STACK_PAGES 4u                      /* 16KB 用户栈 */

void syscall_init(void);

/* 汇编 syscall_entry 调入；应用代码不直接调用 */
int syscall_handler(uint32_t num, uint32_t a1, uint32_t a2, uint32_t a3);

/* 演示：映射用户空间、装入内建用户程序、切入 ring3 执行、exit 后返回。
 * 返回用户程序的退出码；返回 -1 表示未能进入用户态（如用户页映射失败）。 */
int usermode_run_demo(void);

uint32_t syscall_count(void);

/* 自检用：校验"用户指针范围"是否通过安全边界（供 shell 断言非用户页被拒）
 * _ok = 只读访问语义；_rw = 内核可写语义（额外要求 PTE_RW） */
int syscall_user_range_ok(uint32_t va, uint32_t len);
int syscall_user_range_rw(uint32_t va, uint32_t len);

#endif
