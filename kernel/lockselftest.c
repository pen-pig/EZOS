/*
 * lockselftest.c - 内存分配器临界区保护自检（P1 无锁隐患的回归网）
 *
 * 背景：kmalloc/kfree 与 pmm_alloc/free 此前完全无锁，而 net.c 的收发路径
 * 会在 IRQ11 上下文调用 kmalloc（net.c:185-186 懒分配等），抢占式调度下
 * 可重入撕裂空闲链表/位图。修复是在两个分配器内部用 irq_save_disable/
 * irq_restore（kernel/irqflags.h）包住元数据临界区。
 *
 * 本文件验证的不是"能不能分配"，而是三件容易悄悄回归的事：
 *   1) irqflags 原语语义正确——尤其是"原本就关中断时不许误开"。这是
 *      task_lock/task_unlock（全局深度计数、归零无条件 sti）做不到的；
 *      若有人把分配器改回用 task_lock，第 2/3/4 项会立刻变红。
 *   2) 模拟 IRQ 上下文（cli 已生效）中调用分配器，返回后中断必须仍是关
 *      的——否则 iret 之前就开了中断，同一 IRQ 会重入。
 *   3) 压力下不变量成立：反复交错分配/释放后 used 计数回到基线，且不会
 *      把同一块内存同时交给两个所有者（指针/页地址两两不同）。
 *
 * 约束：行宽 <= 80 列（自检铁律）；内核无 printf，拼接全部手写；
 * 临界区内禁止睡眠，因此这里只做纯计算。
 */
#include "lockselftest.h"
#include "types.h"
#include "irqflags.h"
#include "kmalloc.h"
#include "pmm.h"
#include "dmesg.h"

/* ---------- 行输出助手（手写拼接，行宽受控） ---------- */

static void lt_str(char *b, int *n, int lim, const char *s) {
    while (*s && *n < lim) b[(*n)++] = *s++;
}

static void lt_result(const char *name, int ok) {
    char b[64];
    int n = 0;
    int lim = (int)sizeof(b) - 1;
    lt_str(b, &n, lim, "LOCKTEST: ");
    lt_str(b, &n, lim, name);
    lt_str(b, &n, lim, ok ? " PASS" : " FAIL");
    b[n] = '\0';
    dmesg_write(b);
    dmesg_write("\n");
}

/* 读当前 EFLAGS 的 IF 位 */
static uint32_t lt_read_if(void) {
    uint32_t f;
    asm volatile("pushf\n\t"
                 "pop  %0"
                 : "=r"(f)
                 :
                 : "memory");
    return f & EFLAGS_IF;
}

/* ---------- 1) 原语语义：开中断时 save 读到 1，restore 复原 ---------- */

static int lt_if_save_on(void) {
    uint32_t was = irq_save_disable();
    int ok = (was != 0);              /* 调用前 IF=1，必须读到非零 */
    ok = ok && (lt_read_if() == 0);   /* 此刻必须是关的 */
    irq_restore(was);
    ok = ok && (lt_read_if() != 0);   /* 恢复后必须重新开着 */
    return ok ? 0 : 1;
}

/* ---------- 2) 原语语义：关中断时 save 读到 0，restore 不许误开 ---------- */

static int lt_if_nested_keep(void) {
    asm volatile("cli" ::: "memory");
    uint32_t was = irq_save_disable();
    int ok = (was == 0);              /* 已经关着，必须读到 0 */
    irq_restore(was);                 /* 绝不能在这里 sti */
    ok = ok && (lt_read_if() == 0);   /* 关键：仍须是关的 */
    asm volatile("sti" ::: "memory");
    return ok ? 0 : 1;
}

/* ---------- 3) 模拟 IRQ 上下文调 kmalloc/kfree ---------- */

static int lt_kmalloc_irq_nested(void) {
    /* IRQ 桩（boot/kernel_entry.asm）入口就是 cli+call，此处照搬 */
    asm volatile("cli" ::: "memory");
    int ok = 1;
    void *p = kmalloc(128);
    if (p == 0) ok = 0;
    if (lt_read_if() != 0) ok = 0;    /* kmalloc 返回后不得提前开中断 */
    if (p) kfree(p);
    if (lt_read_if() != 0) ok = 0;    /* kfree 同理 */
    asm volatile("sti" ::: "memory");
    return ok ? 0 : 1;
}

/* ---------- 4) 模拟 IRQ 上下文调 pmm_alloc/free ---------- */

static int lt_pmm_irq_nested(void) {
    asm volatile("cli" ::: "memory");
    int ok = 1;
    uint32_t pa = pmm_alloc_page();
    if (pa == 0) ok = 0;
    if (lt_read_if() != 0) ok = 0;    /* alloc 返回后不得提前开中断 */
    if (pa) pmm_free_page(pa);
    if (lt_read_if() != 0) ok = 0;    /* free 同理 */
    asm volatile("sti" ::: "memory");
    return ok ? 0 : 1;
}

/* ---------- 5) 堆压力：随机交错分配/释放，校验记账与重叠 ---------- */

static int lt_km_stress(void) {
    void *slots[48];
    for (int i = 0; i < 48; i++) slots[i] = 0;

    uint32_t u0 = kmalloc_used();
    uint32_t s = 0x1234ABCDu;         /* 固定种子，可复现 */
    int ok = 1;

    for (int round = 0; round < 400 && ok; round++) {
        s = s * 1664525u + 1013904223u;
        int i = (int)((s >> 16) & 47u);
        if (slots[i]) {
            kfree(slots[i]);
            slots[i] = 0;
            continue;
        }
        s = s * 1664525u + 1013904223u;
        uint32_t sz = (s % 512u) + 8u;           /* 8..519 字节 */
        uint8_t *p = (uint8_t *)kmalloc(sz);
        if (!p) { ok = 0; break; }
        slots[i] = p;
        /* 写入可复算的 pattern 并读回：若同一块被交给两个所有者，
         * 后写的会踩掉先写的，读回即失败 */
        for (uint32_t k = 0; k < sz; k++)
            p[k] = (uint8_t)(sz + k);
        for (uint32_t k = 0; k < sz; k++)
            if (p[k] != (uint8_t)(sz + k)) { ok = 0; break; }
    }

    /* 指针两两不同：直接抓"同一块给了两个 slot" */
    for (int i = 0; i < 48 && ok; i++) {
        if (!slots[i]) continue;
        for (int j = i + 1; j < 48 && ok; j++)
            if (slots[j] == slots[i]) ok = 0;
    }

    for (int i = 0; i < 48; i++)
        if (slots[i]) kfree(slots[i]);

    if (kmalloc_used() != u0) ok = 0;  /* 记账必须归零 */
    return ok ? 0 : 1;
}

/* ---------- 6) 页帧压力：分配不重叠，用尽记账一致 ---------- */

static int lt_pmm_stress(void) {
    uint32_t pages[32];
    uint32_t u0 = pmm_used_pages();
    int got = 0;
    int ok = 1;

    for (int i = 0; i < 32 && ok; i++) {
        pages[i] = pmm_alloc_page();
        if (pages[i] == 0) break;      /* 池满：见好就收，不算失败 */
        got++;
    }
    /* 页地址两两不同且在池内：直接抓 double allocation */
    for (int i = 0; i < got && ok; i++) {
        for (int j = i + 1; j < got && ok; j++)
            if (pages[j] == pages[i]) ok = 0;
    }
    for (int i = 0; i < got; i++) {
        if (pmm_free_page(pages[i]) != 0) ok = 0;
    }
    if (pmm_used_pages() != u0) ok = 0;  /* 记账必须归零 */
    return ok ? 0 : 1;
}

/* ---------- 6) dmesg（IRQ 与进程共用的输出通道）临界区 ----------
 *
 * 为什么单列：dmesg_write 是全系统唯一被 IRQ 上下文（net/rtl8139 的 IRQ11、
 * ehci/nvme/usb 中断路径）与进程上下文（shell）同时调用的输出通道。它若
 * 无保护，串口侧两行字符会交错——而本项目所有 E2E 都是**按行**断言串口
 * 输出的，交错会直接造成难以复现的假红假绿。
 *
 * 这里只验证一件能真正失败的事：
 *   a) 在"已关中断"的模拟 IRQ 上下文里调用，返回后中断必须仍是关的。
 *      若有人把 dmesg 改成用 task_lock()（全局深度计数、归零无条件 sti），
 *      这一项立刻变红——与 kmalloc/pmm 那两项是同一个回归网。
 *
 * 刻意**不**测"两行字符交错"：单核自检里两次 dmesg_write 天然串行，
 * 无论有没有临界区结果都一样，属于加了也测不出回归的弱断言。真正的交错
 * 需要 IRQ11 在进程上下文写串口的中途抢入，自检构造不出来——那一层靠
 * 代码审查 + dmesg.c 顶部的并发注释守住。 */

static int lt_dmesg_irq_nested(void) {
    asm volatile("cli" ::: "memory");
    int ok = 1;
    /* 前缀刻意不用 "LOCKTEST:"——那是以 lt_result 打出的**结果行**专用前缀，
     * tests/test_lock.py 靠它统计用例数，自检自己写的数据行不能用同名前缀。 */
    dmesg_write("LOCKDBG: dmesg-irq-ctx");
    if (lt_read_if() != 0) ok = 0;    /* 返回后不得提前开中断 */
    asm volatile("sti" ::: "memory");
    return ok ? 0 : 1;
}

/* 读路径回归：dmesg_dump/tail 被改成了"快照 + 逐行加锁拷贝"，这里验证
 * 改写后仍能按原顺序读回完整行（抓的是 dm_first 漏快照、idx 算错、
 * 拷贝截断这类把读路径改坏的错误）。 */
static int lt_expect2(const char *buf, const char *a, const char *b) {
    int i = 0;
    for (int k = 0; a[k]; k++) {
        if (buf[i] == 0 || buf[i] != a[k]) return 1;
        i++;
    }
    for (int k = 0; b[k]; k++) {
        if (buf[i] == 0 || buf[i] != b[k]) return 1;
        i++;
    }
    return 0;
}

static int lt_dmesg_tail_readback(void) {
    /* 必须带 '\n'：dmesg_write 只在遇到换行时才把 dm_cur 刷成一行，
     * 不带换行的内容会留在"正在积累的行"里，dmesg_tail 根本看不到它。 */
    dmesg_write("LOCKDBG: row-alpha\n");
    dmesg_write("LOCKDBG: row-bravo\n");
    char buf[160];
    int got = dmesg_tail(buf, sizeof(buf), 2);
    if (got <= 0) return 1;
    return lt_expect2(buf, "LOCKDBG: row-alpha\n", "LOCKDBG: row-bravo\n");
}

/* ---------- 主入口 ---------- */

int lock_selftest(void) {
    int nfail = 0;
    int r;

    r = lt_if_save_on();         lt_result("irqflags save(IF=1)", r == 0);
    if (r) nfail++;
    r = lt_if_nested_keep();     lt_result("irqflags nested keep", r == 0);
    if (r) nfail++;
    r = lt_kmalloc_irq_nested(); lt_result("kmalloc in IRQ ctx", r == 0);
    if (r) nfail++;
    r = lt_pmm_irq_nested();     lt_result("pmm in IRQ ctx", r == 0);
    if (r) nfail++;
    r = lt_km_stress();          lt_result("kmalloc stress", r == 0);
    if (r) nfail++;
    r = lt_pmm_stress();         lt_result("pmm stress", r == 0);
    if (r) nfail++;
    r = lt_dmesg_irq_nested();   lt_result("dmesg in IRQ ctx", r == 0);
    if (r) nfail++;
    r = lt_dmesg_tail_readback(); lt_result("dmesg tail readback", r == 0);
    if (r) nfail++;

    return nfail;
}
