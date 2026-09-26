/*
 * irqflags.h - 单核抢占式内核的临界区原语（保存/恢复中断状态）
 *
 * 为什么存在这个文件
 * ------------------
 * 本内核是单核 + 抢占式（IRQ0 触发轮转调度）。对单核而言，关中断同时也
 * 关掉了抢占（抢占靠 IRQ0 进入），所以「关中断」就是最底层也最有效的
 * 临界区手段，足以保护 kmalloc/kfree 与 pmm 的元数据。
 *
 * 为什么不用 task_lock()/task_unlock()
 * ------------------------------------
 * task_lock() = cli + 全局深度计数 g_lock_depth++；task_unlock() 在计数
 * 归零时**无条件 sti**。而 IRQ 桩（boot/kernel_entry.asm 的 irq0/irq1/
 * irq11/irq12/...）在 `call xxx_handler` 之前已经手工 cli，此时 g_lock_depth
 * 仍然是 0。于是：
 *
 *     中断处理函数里 kmalloc()
 *       -> task_lock()    depth: 0 -> 1
 *       -> task_unlock()  depth: 1 -> 0  ==> sti   <== 灾难
 *
 * 这在 iret 之前就把中断打开了，会造成同一 IRQ 的重入（处理函数自己打断
 * 自己），轻则栈溢出、重则再次进入 kmalloc 破坏空闲链表。
 *
 * net.c 就有这样的调用路径：net_input() 会在 IRQ11 上下文首次进入并调用
 * kmalloc 懒分配收发缓冲（net.c:185-186），TCP 路径还有 net.c:558/562/889。
 *
 * irq_save_disable()/irq_restore() 不依赖任何全局计数，只依赖调用点自身
 * 的 IF 状态，因此在「裸任务上下文」「已持 task_lock 的上下文」「IRQ 处理
 * 函数内部」这三种嵌套情形下都是局部正确的。
 *
 * 为什么 irq_restore 只恢复 IF 而不是整个 EFLAGS
 * ----------------------------------------------
 * 恢复整个 EFLAGS 会把方向标志 DF 等一并写回，覆盖掉编译器在临界区里刚
 * 设置的状态；而且 pushf/cli/popf 的字节序列本身也不是原子的。这里只取
 * IF 位（0x200）判断，做法与 Linux 的 local_irq_save/local_irq_restore 一致。
 *
 * 不可睡眠约束
 * ------------
 * 临界区内（irq_restore 之前）**禁止调用任何可能阻塞/让出的函数**
 * （task_sleep/task_yield/等待队列等）。关中断期间不会被抢占，若此时主动
 * 让出，调度器将永远不会回来——等于死锁。当前 kmalloc/kfree/pmm_* 内部
 * 只做纯计算与 panic 校验，满足此约束。
 */
#ifndef IRQFLAGS_H
#define IRQFLAGS_H

#include "types.h"

/* EFLAGS 第 9 位：中断允许标志 IF */
#define EFLAGS_IF 0x200u

/*
 * 关中断，返回调用前 IF 是否置位（0 = 原本已关，非 0 = 原本开着）。
 * "memory" 屏障：禁止编译器把临界区内的读写重排到加锁之前或解锁之后。
 */
static inline uint32_t irq_save_disable(void) {
    uint32_t f;
    asm volatile("pushf\n\t"
                 "pop  %0\n\t"
                 "cli"
                 : "=r"(f)
                 :
                 : "memory");
    return f & EFLAGS_IF;
}

/*
 * 配对恢复：只有当调用 irq_save_disable() 之前中断是开着的，才重新打开。
 * 传入的一定是 irq_save_disable() 的返回值。
 */
static inline void irq_restore(uint32_t flags) {
    if (flags) asm volatile("sti" ::: "memory");
}

#endif /* IRQFLAGS_H */
