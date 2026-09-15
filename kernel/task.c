/*
 * task.c - 任务与抢占式调度器（步骤 6a）
 */
#include "task.h"
#include "gdt.h"
#include "isr.h"        /* g_pit_ticks */
#include "panic.h"
#include "fd.h"         /* task_reap_zombies / process_exit 里 fd_close */
#include "paging.h"     /* PAGING_PD_ADDR / paging_switch_cr3（6c 地址空间切换） */
#include "pmm.h"        /* 进程退出时按账回收物理页 */

/* ---------- 内核栈池 ----------
 * 每任务 16KB，位于 linker.ld 的 .bss.kstack（物理 0x904000 起）。
 * 为什么不放 .bss.hi：那里已用 ~1.97MB（上限 2MB），8 个 16KB 会顶穿。
 * 栈向低地址生长，kstack_top = kstack + TASK_KSIZE。 */
static uint8_t g_kstacks[MAX_TASKS][TASK_KSIZE]
    __attribute__((section(".bss.kstack"), aligned(16)));

static task_t   g_tasks[MAX_TASKS];
static uint32_t g_current;              /* 当前任务在表中的下标 */
static uint32_t g_next_pid = 1;
static uint32_t g_sched_count;
static uint32_t g_lock_depth;           /* 临界区嵌套计数 */
static int      g_ready;                /* task_init 是否已完成 */

extern void switch_to(uint32_t *old_esp, uint32_t new_esp);

/* ---------- 栈初始化 ---------- */
/*
 * 新任务的栈必须摆成**完整的 IRQ0 中断帧**，因为抢占切换发生在中断上下文里。
 *
 * switch_to 恢复序列：pop edi; pop esi; pop ebx; pop ebp; ret
 * ret 落到 task_irq_trampoline，它执行 popa; sti; iret（等价于 irq0 桩的
 * 后半段）——iret 弹出 EIP/CS/EFLAGS 进入 fn，EFLAGS 预置 0x202 使 IF=1。
 *
 *   esp+ 0  edi          <- switch_to 的 pop edi
 *   esp+ 4  esi
 *   esp+ 8  ebx
 *   esp+12  ebp
 *   esp+16  trampoline   <- switch_to 的 ret 目标
 *   esp+20  eax          <- popa 区（低地址是 eax：pusha 先压 eax）
 *   esp+24  ecx
 *   esp+28  edx
 *   esp+32  ebx
 *   esp+36  esp (dummy)
 *   esp+40  ebp
 *   esp+44  esi
 *   esp+48  edi
 *   esp+52  EIP = fn     <- iret 弹出（ring0 中断：无 ESP/SS）
 *   esp+56  CS  = 0x08
 *   esp+60  EFLAGS=0x202 (IF=1)
 *   esp+64  task_exit    <- fn 的返回地址
 *   esp+68  arg          <- fn 的参数
 *
 * 若改动 irq0 桩（boot/kernel_entry.asm）或 task_switch.asm 的 trampoline，
 * 本布局必须同步修改——三者是绑定的。
 */
extern void task_irq_trampoline(void);

static void stack_init(task_t *t, void (*fn)(void *), void *arg) {
    uint32_t *sp = (uint32_t *)t->kstack_top;
    *--sp = (uint32_t)arg;              /* +68 */
    *--sp = (uint32_t)task_exit;        /* +64 */
    *--sp = 0x202u;                     /* +60 EFLAGS: IF=1 */
    *--sp = 0x08u;                      /* +56 CS = 内核代码段 */
    *--sp = (uint32_t)fn;               /* +52 EIP */
    *--sp = 0u;                         /* +48 edi (popa) */
    *--sp = 0u;                         /* +44 esi */
    *--sp = 0u;                         /* +40 ebp */
    *--sp = 0u;                         /* +36 esp (dummy) */
    *--sp = 0u;                         /* +32 ebx */
    *--sp = 0u;                         /* +28 edx */
    *--sp = 0u;                         /* +24 ecx */
    *--sp = 0u;                         /* +20 eax */
    *--sp = (uint32_t)task_irq_trampoline;  /* +16 switch_to ret 目标 */
    *--sp = 0u;                         /* +12 ebp */
    *--sp = 0u;                         /* +8  ebx */
    *--sp = 0u;                         /* +4  esi */
    *--sp = 0u;                         /* +0  edi */
    t->esp = (uint32_t)sp;
}

/* ---------- 生命周期 ---------- */

void task_init(void) {
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        g_tasks[i].pid = 0;
        g_tasks[i].state = TASK_UNUSED;
        g_tasks[i].esp = 0;
        g_tasks[i].kstack = 0;
        g_tasks[i].kstack_top = 0;
        g_tasks[i].ticks = 0;
        g_tasks[i].switches = 0;
        g_tasks[i].name[0] = '\0';
        g_tasks[i].cr3 = 0;
        g_tasks[i].is_user = 0;
        g_tasks[i].exit_code = 0;
        g_tasks[i].user_stack_phys = 0;
        g_tasks[i].user_stack_pages = 0;
        g_tasks[i].pd_phys = 0;
        g_tasks[i].pt_phys = 0;
        g_tasks[i].img.nseg = 0;
        g_tasks[i].img.entry = 0;
        g_tasks[i].img.brk = 0;
    }

    /* 0 号任务 = 当前执行流（kernel_main → shell）。它用的还是启动栈，
     * 没有独立内核栈——切换时只需保存 esp，不需要 TSS.esp0（它永不在
     * ring3 被抢占，因此不存在"陷入栈"一说）。 */
    task_t *t = &g_tasks[0];
    t->pid = 0;
    t->state = TASK_RUNNING;
    t->ticks = TASK_TIMESLICE;
    t->switches = 1;
    t->kstack = 0;
    /* task 0 一直在启动栈（0x90000）上跑，没有本任务专属内核栈；但切回它时
     * 必须把 TSS.esp0 还原成公共陷入栈，否则会沿用上一个任务的栈顶——
     * 两个任务共用一个陷入栈会互相覆写现场。 */
    t->kstack_top = SYSCALL_TRAP_STACK_TOP;
    const char *n = "idle/shell";
    for (int i = 0; n[i]; i++) t->name[i] = n[i];
    fd_table_init(&t->fds);

    g_current = 0;
    g_sched_count = 0;
    g_lock_depth = 0;
    g_ready = 1;
}

int task_create(const char *name, void (*fn)(void *), void *arg) {
    if (!g_ready || fn == 0) return -1;

    /* 找空位 */
    int slot = -1;
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        if (g_tasks[i].state == TASK_UNUSED) { slot = (int)i; break; }
    }
    if (slot < 0) return -1;

    task_t *t = &g_tasks[slot];
    t->kstack     = (uint32_t)g_kstacks[slot];
    t->kstack_top = t->kstack + TASK_KSIZE;
    t->pid        = g_next_pid++;
    t->state      = TASK_READY;
    t->ticks      = TASK_TIMESLICE;
    t->switches   = 0;
    t->cr3        = 0;              /* 内核线程：用内核页目录 */
    t->is_user    = 0;
    t->exit_code  = 0;
    t->user_stack_phys = 0;
    t->user_stack_pages = 0;
    t->pd_phys    = 0;
    t->pt_phys    = 0;
    t->img.nseg   = 0;
    if (name) {
        int i = 0;
        for (; name[i] && i < 15; i++) t->name[i] = name[i];
        t->name[i] = '\0';
    } else {
        t->name[0] = '\0';
    }
    fd_table_init(&t->fds);
    stack_init(t, fn, arg);
    return (int)t->pid;
}

void task_exit(void) {
    if (!g_ready) return;
    g_tasks[g_current].state = TASK_ZOMBIE;
    /* 让出 CPU：ZOMBIE 不会被再选中，调度器自然切走 */
    task_yield();
    /* 不该走到这里 */
    for (;;) asm volatile("cli; hlt");
}

/* ---------- 用户进程（步骤 6c） ---------- */

task_t *task_find(int pid) {
    if (!g_ready || pid <= 0) return 0;
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        if (g_tasks[i].state != TASK_UNUSED && g_tasks[i].pid == (uint32_t)pid)
            return &g_tasks[i];
    }
    return 0;
}

int task_create_process(const char *name, uint32_t entry, uint32_t user_esp,
                        uint32_t pd_phys) {
    if (!g_ready || pd_phys == 0) return -1;

    int slot = -1;
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        if (g_tasks[i].state == TASK_UNUSED) { slot = (int)i; break; }
    }
    if (slot < 0) return -1;

    task_t *t = &g_tasks[slot];
    t->kstack     = (uint32_t)g_kstacks[slot];
    t->kstack_top = t->kstack + TASK_KSIZE;
    t->pid        = g_next_pid++;
    t->state      = TASK_READY;
    t->ticks      = TASK_TIMESLICE;
    t->switches   = 0;
    t->cr3        = pd_phys;
    t->is_user    = 1;
    t->exit_code  = 0;
    t->user_stack_phys = 0;
    t->user_stack_pages = 0;
    t->pd_phys    = pd_phys;
    t->pt_phys    = 0;
    t->img.nseg   = 0;
    if (name) {
        int i = 0;
        for (; name[i] && i < 15; i++) t->name[i] = name[i];
        t->name[i] = '\0';
    } else {
        t->name[0] = '\0';
    }
    fd_table_init(&t->fds);

    /*
     * 用户进程的首次出场与内核线程不同：iret 帧是 **ring3 帧**。
     *
     *   esp+ 0  edi          <- switch_to 的 pop edi
     *   esp+ 4  esi
     *   esp+ 8  ebx
     *   esp+12  ebp
     *   esp+16  trampoline   <- switch_to 的 ret 目标
     *   esp+20  popa 区（8 个哑元，保持与 irq0 桩一致的出栈序列）
     *   esp+52  EIP = entry  <- iret 弹出
     *   esp+56  CS  = 0x1B   <- RPL3，触发特权级切换
     *   esp+60  EFLAGS=0x202 (IF=1)
     *   esp+64  ESP = user_esp   <- 只有跨特权级的 iret 才弹这两个
     *   esp+68  SS  = 0x23
     *
     * 落到 ring3 后，用户程序从 entry 开始，栈顶就是 argc/argv（由 exec
     * 预先摆在其用户栈页上）。CR3 由 schedule() 在 switch_to 之前切好，
     * 所以 iret 取指时用的已经是进程自己的页目录。
     */
    uint32_t *sp = (uint32_t *)t->kstack_top;
    *--sp = 0x23u;                  /* SS */
    *--sp = user_esp;               /* ESP */
    *--sp = 0x202u;                 /* EFLAGS: IF=1 */
    *--sp = 0x1Bu;                  /* CS = 用户代码段 | RPL3 */
    *--sp = entry;                  /* EIP */
    for (int i = 0; i < 8; i++) *--sp = 0u;      /* popa 区 */
    *--sp = (uint32_t)task_irq_trampoline;
    *--sp = 0u;                     /* ebp */
    *--sp = 0u;                     /* ebx */
    *--sp = 0u;                     /* esi */
    *--sp = 0u;                     /* edi */
    t->esp = (uint32_t)sp;
    return (int)t->pid;
}

void process_exit(int code) {
    if (!g_ready) return;
    task_t *me = &g_tasks[g_current];
    if (!me->is_user) {
        /* 内核线程误用进程退出路径：退化成普通退出 */
        task_exit();
        return;
    }

    me->exit_code = code;
    me->state = TASK_ZOMBIE;

    /* 1) 先回内核地址空间——下面要释放的正是当前页目录所在的页 */
    paging_switch_cr3(PAGING_PD_ADDR);
    me->cr3 = 0;

    /* 2) 按账回收（谁分配谁记账，回查页表在这个时刻已经没有意义） */
    for (uint32_t s = 0; s < me->img.nseg; s++)
        pmm_free_pages(me->img.seg[s].phys, me->img.seg[s].pages);
    me->img.nseg = 0;

    for (uint32_t k = 0; k < me->user_stack_pages; k++)
        pmm_free_page(me->user_stack_phys + k * PMM_PAGE_SIZE);
    me->user_stack_phys = 0;
    me->user_stack_pages = 0;

    if (me->pt_phys) { pmm_free_page(me->pt_phys); me->pt_phys = 0; }
    if (me->pd_phys) { pmm_free_page(me->pd_phys); me->pd_phys = 0; }

    /* 3) 关掉它打开的文件（dirty 落盘）：进程退出的 fd 语义 */
    for (uint32_t f = 3; f < MAX_OPEN_FDS; f++) fd_close(&me->fds, (int)f);

    /* 4) 让出 CPU。本帧永不返回——槽位由 task_wait_pid 回收。 */
    schedule();
    for (;;) asm volatile("cli; hlt");
}

int task_wait_pid(int pid) {
    if (!g_ready || pid <= 0) return -1;
    if (task_find(pid) == 0) return -1;          /* 没有这个进程 */

    for (;;) {
        task_t *t = task_find(pid);
        if (t == 0) return -1;                   /* 已被别人收走 */
        if (t->state == TASK_ZOMBIE) {
            int code = t->exit_code;
            t->state = TASK_UNUSED;
            t->pid = 0;
            t->is_user = 0;
            t->cr3 = 0;
            t->name[0] = '\0';
            return code;
        }
        task_yield();                            /* 还活着：让出 CPU 等它 */
    }
}

void task_yield(void) {
    if (!g_ready) return;
    /* 强制让出：不清零时间片的话，schedule() 会因为"片还没用完"直接返回，
     * yield 就成了空操作——忙等让位的循环会变成死锁。 */
    g_tasks[g_current].ticks = 0;
    schedule();
}

/* ---------- 调度 ---------- */

/*
 * 收割 ZOMBIE：任务退出（task_exit 把自己置 ZOMBIE）后，它的任务槽、
 * fd 表、内核栈都可以回收了。由 schedule() 在每次切换**之后**统一做：
 *   - 只收割 ZOMBIE（退出语义已经完成，谁都不再引用它的状态）
 *   - 从不收割 RUNNING/READY（正在用的栈）
 * 谁分配谁记账的对称面：task_create 分配的 fd 表在这里关闭释放。
 */
static void task_reap_zombies(void) {
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        task_t *t = &g_tasks[i];
        if (t->state != TASK_ZOMBIE) continue;
        /* 用户进程的 ZOMBIE 要留到 task_wait_pid 取走退出码再回收——
         * 自动回收会让父进程永远读不到退出码。 */
        if (t->is_user) continue;
        /* 关掉它打开的所有文件句柄（dirty 的落盘），防止 fd/kmalloc 泄漏 */
        for (uint32_t f = 3; f < MAX_OPEN_FDS; f++)
            fd_close(&t->fds, (int)f);
        t->pid = 0;
        t->esp = 0;
        t->state = TASK_UNUSED;
        t->name[0] = '\0';
        /* kstack/kstack_top 保留为 0 即可（栈池按槽位静态分配，无需 free） */
    }
}

void schedule(void) {
    if (!g_ready || g_lock_depth != 0) return;   /* 临界区内不切换 */

    task_t *cur = &g_tasks[g_current];
    /* 先判断再递减：ticks 是无符号数，若为 0 时再 -- 会下溢成 0xFFFFFFFF，
     * 于是下面的 "> 0" 永远成立，任务再也不会被切走（task_yield 会失效）。 */
    if (cur->ticks > 0) cur->ticks--;
    if (cur->state == TASK_RUNNING && cur->ticks > 0) return;   /* 时间片未用完 */

    /* 轮转：从当前下一位开始找 RUNNABLE */
    uint32_t next = g_current;
    for (uint32_t i = 1; i <= MAX_TASKS; i++) {
        uint32_t cand = (g_current + i) % MAX_TASKS;
        if (g_tasks[cand].state == TASK_READY || g_tasks[cand].state == TASK_RUNNING) {
            next = cand;
            break;
        }
    }
    if (next == g_current) {
        /* 没有别的候选（当前是唯一可运行的），续一个时间片继续跑 */
        cur->ticks = TASK_TIMESLICE;
        return;
    }

    task_t *n = &g_tasks[next];
    if (cur->state == TASK_RUNNING) cur->state = TASK_READY;

    uint32_t old_idx = g_current;
    g_current = next;
    n->state = TASK_RUNNING;
    n->ticks = TASK_TIMESLICE;
    n->switches++;
    g_sched_count++;

    /* 内核栈同时充当该任务的 ring3 陷入栈 */
    if (n->kstack_top) tss_set_kernel_stack(n->kstack_top);

    /* 地址空间切换（步骤 6c）：用户进程带自己的页目录，内核任务用内核的。
     * 必须在 switch_to 之前——新任务可能首次出场，它的 iret 就要用新页目录
     * 取指；而本段代码所在的栈（旧任务内核栈）在两个页目录里都是
     * identity 映射的，换 CR3 不会让当前这条执行流失控。 */
    uint32_t cr3_cur = cur->cr3 ? cur->cr3 : PAGING_PD_ADDR;
    uint32_t cr3_new = n->cr3 ? n->cr3 : PAGING_PD_ADDR;
    if (cr3_new != cr3_cur) paging_switch_cr3(cr3_new);

    switch_to(&g_tasks[old_idx].esp, n->esp);

    /* 切换回来后收割 ZOMBIE 槽位（含 fd 表释放）。
     * 放在 switch_to 之后：此刻我们在新任务上下文里，旧任务已停走，
     * 它的 ZOMBIE 状态不会再变。 */
    task_reap_zombies();
}

/* ---------- 临界区 ---------- */

void task_lock(void) {
    asm volatile("cli");
    g_lock_depth++;
}

void task_unlock(void) {
    if (g_lock_depth) g_lock_depth--;
    if (g_lock_depth == 0) asm volatile("sti");
}

/* ---------- 查询 ---------- */

task_t *task_current(void) { return g_ready ? &g_tasks[g_current] : 0; }
const task_t *task_table(uint32_t *out_count) {
    if (out_count) *out_count = MAX_TASKS;
    return g_tasks;
}
uint32_t task_count_running(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < MAX_TASKS; i++)
        if (g_tasks[i].state == TASK_READY || g_tasks[i].state == TASK_RUNNING) n++;
    return n;
}
uint32_t schedule_count(void) { return g_sched_count; }

/* ---------- 自检 ---------- */
/*
 * 两个计数线程各自自增一段时间。真抢占的判据不是"计数都涨了"
 * （协作式也会涨），而是：
 *   1) 两个任务的 switches 都 > 1  -> 调度器确实来回切过
 *   2) 总计数量级相当                 -> 没有饿死
 *   3) 切换过程中没有崩溃（能打印就说明栈没错）
 */
static volatile uint32_t g_a, g_b;
static volatile int g_a_done, g_b_done;

static void tick_a(void *arg) {
    (void)arg;
    for (uint32_t i = 0; i < 200000u; i++) g_a++;
    g_a_done = 1;
}
static void tick_b(void *arg) {
    (void)arg;
    for (uint32_t i = 0; i < 200000u; i++) g_b++;
    g_b_done = 1;
}

int task_selftest(void (*out)(const char *)) {
    if (!out) return -1;
    int fail = 0;
    out("task scheduler self-test:\n");

    if (!g_ready) { out("  scheduler not initialized\n"); return 1; }

    g_a = g_b = 0;
    g_a_done = g_b_done = 0;
    int pa = task_create("selftest_a", tick_a, 0);
    int pb = task_create("selftest_b", tick_b, 0);
    if (pa < 0 || pb < 0) { out("  task_create failed\n"); return 1; }

    /* 自旋等待两者完成：期间 PIT 抢占应把它们来回切 */
    uint32_t guard = 0;
    while ((!g_a_done || !g_b_done) && guard++ < 0x2000000u) task_yield();

    /* 找这两个任务的最终状态 */
    task_t *ta = 0, *tb = 0;
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        if (g_tasks[i].pid == (uint32_t)pa) ta = &g_tasks[i];
        if (g_tasks[i].pid == (uint32_t)pb) tb = &g_tasks[i];
    }

    int ok_counts = (g_a == 200000u) && (g_b == 200000u);
    out("  both threads finished: ");
    out(ok_counts ? "yes [OK]\n" : "NO [FAIL]\n");
    if (!ok_counts) fail++;

    int ok_sw = (ta && tb && ta->switches > 1 && tb->switches > 1);
    out("  preempted more than once: ");
    out(ok_sw ? "yes [OK]\n" : "NO [FAIL]\n");
    if (!ok_sw) fail++;

    /* 两个任务被调度的次数不应差一个数量级（没饿死） */
    int ok_fair = 1;
    if (ta && tb && ta->switches && tb->switches) {
        uint32_t hi = ta->switches > tb->switches ? ta->switches : tb->switches;
        uint32_t lo = ta->switches > tb->switches ? tb->switches : ta->switches;
        if (lo == 0 || hi / lo > 10u) ok_fair = 0;
    }
    out("  no starvation: ");
    out(ok_fair ? "yes [OK]\n" : "NO [FAIL]\n");
    if (!ok_fair) fail++;

    if (ta) ta->state = TASK_UNUSED;
    if (tb) tb->state = TASK_UNUSED;

    out(fail == 0 ? "  result: PASS\n" : "  result: FAIL\n");
    return fail;
}
