/*
 * fpu.h - x87 FPU 初始化与自检（步骤 5e）
 *
 * EZOS 是 i686 内核。i686 起 CR0.EM 必须为 0（EM=1 在 i686 上对 SIMD
 * 指令有未定义行为），所以"FPU 不可用"不能靠 EM 模拟，只能：
 *   - CR0.ES/MP 保持复位值
 *   - 探测：fninit 后 fnstenv 读状态字，若 FPU 存在状态字为 0
 *   - 不存在就保持 x87 禁用状态，calc 命令据此报错而不是 #NM 崩溃
 *
 * float ABI：i686-elf-gcc 默认 387 协处理器（-mfpmath=387），
 * 调用约定是 st(0) 返回（SSE 未启用，无 XMM 参数）。
 */
#ifndef FPU_H
#define FPU_H

#include "types.h"

/* 探测并初始化 x87。返回：
 *   0  存在且已 fninit（控制字 = 0x037F：无穷大/舍入默认，所有异常屏蔽）
 *   -1 不存在（无 FPU 的 386/486SX，calc 不可用但不影响其余功能） */
int fpu_init(void);

/* fpu_init() 之后是否可用 */
int fpu_available(void);

/* 自检：基本四则 + 比较精度。out==NULL 时静默。返回失败的断言数（0=全过） */
int fpu_selftest(void (*out)(const char *));

#endif
