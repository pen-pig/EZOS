#ifndef STACK_H
#define STACK_H

/* stack.h - 主栈水位。
 *
 * 内核栈是**没有 MMU 保护**的稀缺资源：
 *     主栈     = [0x100000, 0x200000) 1MB（linker.ld 里
 *                __stack_bottom / __stack_top 两个符号给出边界）
 *     任务内核栈 = TASK_KSIZE = 16KB（kernel/task.h）
 *     ring0 IRQ 不切栈，中断还会往当前栈上再压一帧
 *
 * 栈越界在这套系统里**不会立刻崩**，而是静默写穿紧邻的 .data。历史上
 * shell.c cmd_ls 在栈上放了 fs_dir_entry_t entries[64]（16.9KB），一进 ls
 * 就写穿并覆盖 fs.c 的 ro_cwd[256] —— 症状是提示符变 [A.TXT]、cat/write
 * 全部失败，看起来完全是文件系统坏了，其实盘是好的（宿主 ref_ext4 读得出）。
 *
 * 所以有两道防线：
 *   1. 静态：tools/check_stack.py（单帧 > 12KB 直接 FAIL）—— 防住"单帧过大"
 *   2. 运行时：stack_high_water() —— 防住"调用链叠加起来过大"
 * 两道都不能省：静态看不见链的深度，运行时看不见没被踩到的风险。
 */
#include "types.h"

/* 启动时把空闲栈涂成 0xA5。必须在 kernel_main 最开头调用。 */
void     stack_paint(void);
/* 历史栈峰值（字节）。 */
uint32_t stack_high_water(void);
/* 栈总容量（字节）。 */
uint32_t stack_total(void);

#endif /* STACK_H */
