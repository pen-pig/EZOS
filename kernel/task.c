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
#include "syscall.h"    /* USER_STACK_TOP / USER_STACK_PAGES：task_fork 复制用户栈用 */

/* ---------- 内核栈池 ----------
 * 每任务 16KB，位于 linker.ld 的 .bss.kstack（物理 0x904000 起）。
 * 为什么不放 .bss.hi：那里已用 ~1.97MB（上限 2MB），8 个 16KB 会顶穿。
 * 栈向低地址生长，kstack_top = kstack + TASK_KSIZE。 */
static uint8_t g_kstacks[MAX_TASKS][TASK_KSIZE]
    __attribute__((section(".bss.kstack"), aligned(16)));

/* g_tasks 迁 .bss.hi（步骤 8a）：低 .bss 在 0x90000 栈区断言前只剩
 * ~160B，等待队列新字段（wait_next/wake_tick/wait_q）加上必然顶爆
 * ASSERT。高段（1-2MB）identity 映射且所有进程 PD 共享，IRQ/系统调用
 * 上下文访问无碍（同 net.c 的 socket 表/ARP 缓存的取舍）。 */
static task_t   g_tasks[MAX_TASKS] __attribute__((section(".bss.hi")));
static uint32_t g_current;              /* 当前任务在表中的下标 */
static uint32_t g_next_pid = 1;
static uint32_t g_sched_count;
static uint32_t g_lock_depth;           /* 临界区嵌套计数 */
static int      g_ready;                /* task_init 是否已完成 */

/* ZOMBIE 收割等待队列（步骤 8a）：task_wait_pid 睡在这上面，
 * 子进程 process_exit 时唤醒——取代旧的"轮询 + task_yield"忙等。
 * {-1}：head 哨兵必须是 -1（0 是合法任务下标，静态零初始化会把
 * shell 误挂进空队列）。所有静态等待队列同理。 */
static wait_queue_t g_wq_reap = {-1};

extern void switch_to(uint32_t *old_esp, uint32_t new_esp);

/* ---------- 无 libc 小工具（fork 复制地址空间用） ---------- */
static void t_memcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

/*
 * 在子进程私有用户页表里设一个 PTE（pt_phys 是子进程页表物理页，identity
 * 映射下可直接当指针用）。flags 直接沿用父进程 PTE 的低 12 位属性位。
 */
static void child_pt_set(uint32_t pt_phys, uint32_t va, uint32_t phys, uint32_t flags) {
    uint32_t *pt = (uint32_t *)pt_phys;
    uint32_t pti = (va >> 12) & 0x3FFu;
    pt[pti] = (phys & 0xFFFFF000u) | (flags & 0xFFFu) | PTE_P;
}

/* ---------- 栈初始化 ---------- */
/*
 * 新任务的栈必须摆成**完整的 IRQ0 中断帧**，因为抢占切换发生在中断上下文里。
 *
 * switch_to 恢复序列（步骤 8a 起）：pop edi; pop esi; pop ebx; pop ebp;
 * popfd; ret。ret 落到 task_irq_trampoline，它执行 popa; sti; iret（等价于
 * irq0 桩的后半段）——iret 弹出 EIP/CS/EFLAGS 进入 fn，EFLAGS 预置 0x202
 * 使 IF=1。popfd 槽预置 0x0002（IF=0）：首次出场由 trampoline 的 sti
 * 再开中断，与切换点 IF=0 的约定一致（见 task_switch.asm 的注释）。
 *
 *   esp+ 0  edi          <- switch_to 的 pop edi
 *   esp+ 4  esi
 *   esp+ 8  ebx
 *   esp+12  ebp
 *   esp+16  EFLAGS=0x0002 <- switch_to 的 popfd（IF=0）
 *   esp+20  trampoline   <- switch_to 的 ret 目标
 *   esp+24  eax          <- popa 区（低地址是 eax：pusha 先压 eax）
 *   esp+28  ecx
 *   esp+32  edx
 *   esp+36  ebx
 *   esp+40  esp (dummy)
 *   esp+44  ebp
 *   esp+48  esi
 *   esp+52  edi
 *   esp+56  EIP = fn     <- iret 弹出（ring0 中断：无 ESP/SS）
 *   esp+60  CS  = 0x08
 *   esp+64  EFLAGS=0x202 (IF=1)
 *   esp+68  task_exit    <- fn 的返回地址
 *   esp+72  arg          <- fn 的参数
 *
 * 若改动 irq0 桩（boot/kernel_entry.asm）或 task_switch.asm 的 trampoline/
 * popfd 序列，本布局必须同步修改——三者是绑定的。
 */
extern void task_irq_trampoline(void);

static void stack_init(task_t *t, void (*fn)(void *), void *arg) {
    uint32_t *sp = (uint32_t *)t->kstack_top;
    *--sp = (uint32_t)arg;              /* +72 */
    *--sp = (uint32_t)task_exit;        /* +68 */
    *--sp = 0x202u;                     /* +64 EFLAGS: IF=1 */
    *--sp = 0x08u;                      /* +60 CS = 内核代码段 */
    *--sp = (uint32_t)fn;               /* +56 EIP */
    *--sp = 0u;                         /* +52 edi (popa) */
    *--sp = 0u;                         /* +48 esi */
    *--sp = 0u;                         /* +44 ebp */
    *--sp = 0u;                         /* +40 esp (dummy) */
    *--sp = 0u;                         /* +36 ebx */
    *--sp = 0u;                         /* +32 edx */
    *--sp = 0u;                         /* +28 ecx */
    *--sp = 0u;                         /* +24 eax */
    *--sp = (uint32_t)task_irq_trampoline;  /* +20 switch_to ret 目标 */
    *--sp = 0x0002u;                    /* +16 EFLAGS for popfd（IF=0，bit1 恒 1） */
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
        g_tasks[i].wait_next = -1;
        g_tasks[i].wake_tick = 0;
        g_tasks[i].wait_q = 0;
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
    t->wait_next  = -1;             /* 槽位复用：等待字段必须清——上个任务
                                       的残留链指针会挂出幽灵队列 */
    t->wake_tick  = 0;
    t->wait_q     = 0;
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
    /* 不该走到这里（没有别的可运行任务时 schedule 直接返回）。
     * 停车必须 sti 而不是 cli：僵尸占着 CPU 只是暂态，等待者可能睡在
     * 定时唤醒上——关中断会让 PIT 停摆，全系统的超时一起冻死。 */
    for (;;) asm volatile("sti; hlt");
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
    t->wait_next  = -1;             /* 槽位复用：同 task_create */
    t->wake_tick  = 0;
    t->wait_q     = 0;
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
    *--sp = 0x0002u;                /* EFLAGS for popfd（IF=0，bit1 恒 1） */
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

    /* 0) 唤醒所有等待者（步骤 8a）：
     *    - 睡在收割队列上的父进程（wait_pid）——它等的就是这一刻的 ZOMBIE；
     *    - 万一还有谁直接睡在本任务身上（me->wait_q，预留语义）。
     * 必须在拆页目录之前做：waiter 醒来可能引用本进程的用户内存。 */
    task_wake_all(&g_wq_reap);
    if (me->wait_q != 0) task_wake_all(me->wait_q);

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

    /* 4) 让出 CPU。本帧永不返回——槽位由 task_wait_pid 回收。
     * 兜底停车同样 sti;hlt（理由同 task_exit：别关死中断）。 */
    schedule();
    for (;;) asm volatile("sti; hlt");
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
        /* 步骤 8a：真睡眠等子进程 ZOMBIE（process_exit 会 wake_all）。
         * 无超时——wait 语义就是无限等；对方消失由循环顶的 task_find 判定。 */
        task_sleep(&g_wq_reap, 0);
    }
}

/* ---------- fork（步骤 9：用户态生态第二步） ----------
 *
 * 复制父进程（当前正在执行 fork 系统调用的用户进程）的用户地址空间与 fd 表，
 * 建立一个可运行的子任务。设计要点：
 *
 * 1) 子进程的"出场帧"完全照搬父进程此刻的系统调用现场（syscall_entry 在
 *    syscall_handler 入口压下的那一帧）。frame 是 syscall_handler 入口的 esp，
 *    其下方布局由 kernel_entry.asm 固定：
 *       idx  内容
 *        5   edi (pusha)        6  esi       7  ebp
 *        9   ebx (pusha)       10  edx      11  ecx      12  eax (pusha)
 *       17   SS (iret 帧)      18  ESP      19  EFLAGS   20  CS   21  EIP
 *    子进程栈上摆成与 task_create_process 同构的 ring3 帧，只是寄存器换成父进程
 *    的真实值、且 eax 槽置 0（子进程 fork 返回值）。子进程被调度后沿
 *    switch_to → trampoline → popa → iret 回到 ring3，EIP/CS/EFLAGS/ESP 与父
 *    进程从同一 int 0x80 返回时完全一致——于是父子"同时"从 fork 调用点之后继续，
 *    子进程 eax=0，父进程 eax=子 pid（由 syscall_handler 的返回值给出）。
 *
 * 2) 地址空间逐页深拷贝：新建页目录 + 用户区页表，内核区 PDE 与父/内核共享
 *    （同一批内核页表）。父进程的 ELF 各段、用户栈在 4-8MB 区间，逐页分配新
 *    物理页并 memcpy，按"谁分配谁记账"记到子进程 img/user_stack 上——这样
 *    process_exit 的既有回收路径（连续块 pmm_free_pages）原样可用，无需改。
 *    复制中途任何一步失败都按自己记的账对称回滚（已分配的子进程页/页表/页目录
 *    全部归还），绝不回查页表。
 *
 * 3) 全程 task_lock()：关中断 + 禁调度，建子进程期间不被抢占（共享 g_tasks 与
 *    页表）。fd 复制的 pipe 引用计数递增也依赖这个 cli 窗口。
 */
int task_fork(void *syscall_frame) {
    if (!g_ready || syscall_frame == 0) return -1;
    task_t *parent = &g_tasks[g_current];
    if (!parent->is_user || parent->cr3 == 0) return -1;   /* 只有用户进程能 fork */

    uint32_t *fr = (uint32_t *)syscall_frame;

    task_lock();   /* cli + 禁调度，覆盖槽位分配 / 页表 / fd 复制 */

    /* 找一个空闲任务槽（先于资源分配，避免占了内存却没地方挂） */
    int slot = -1;
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        if (g_tasks[i].state == TASK_UNUSED) { slot = (int)i; break; }
    }
    if (slot < 0) { task_unlock(); return -1; }

    /* 1) 子进程页目录 + 用户区页表 */
    uint32_t pd_phys = pmm_alloc_page();
    uint32_t pt_phys = pmm_alloc_page();
    if (pd_phys == 0 || pt_phys == 0) {
        if (pd_phys) pmm_free_page(pd_phys);
        if (pt_phys) pmm_free_page(pt_phys);
        task_unlock();
        return -1;
    }
    {
        uint32_t *cpd = (uint32_t *)pd_phys;
        uint32_t *ppd = (uint32_t *)parent->cr3;
        for (uint32_t i = 0; i < 1024u; i++) {
            if (i == 1u) continue;
            cpd[i] = ppd[i];                          /* 内核区 PDE 共享 */
        }
        cpd[1] = pt_phys | PTE_P | PTE_RW | PTE_US;
        uint32_t *cpt = (uint32_t *)pt_phys;
        for (uint32_t i = 0; i < 1024u; i++) cpt[i] = 0;
    }

    /* 2) 复制 ELF 各段（连续块分配，逐页拷贝 + 记子进程 PTE） */
    uint32_t child_seg[ELF_MAX_SEG];
    for (uint32_t s = 0; s < ELF_MAX_SEG; s++) child_seg[s] = 0;
    uint32_t nseg = parent->img.nseg;
    int fail = 0;
    for (uint32_t s = 0; s < nseg; s++) {
        uint32_t pages = parent->img.seg[s].pages;
        uint32_t cphys = pmm_alloc_pages(pages);
        if (cphys == 0) { fail = 1; break; }
        child_seg[s] = cphys;
        uint32_t va = parent->img.seg[s].va;
        for (uint32_t k = 0; k < pages; k++) {
            uint32_t p = va + k * PMM_PAGE_SIZE;
            uint32_t phys = 0, flags = 0;
            if (paging_query(p, &phys, &flags) != 0) {       /* 父页必在，失败即异常 */
                pmm_free_pages(cphys, pages);
                child_seg[s] = 0;
                fail = 1;
                break;
            }
            /* 源：父进程虚拟地址（当前 CR3=父，可直接读）；
             * 目的：子进程新物理页（identity 映射，可直接写） */
            t_memcpy((void *)(cphys + k * PMM_PAGE_SIZE), (const void *)p, PMM_PAGE_SIZE);
            child_pt_set(pt_phys, p, cphys + k * PMM_PAGE_SIZE, flags);
        }
        if (fail) break;
    }

    /* 3) 复制用户栈（连续块） */
    uint32_t child_stack = 0;
    if (!fail) {
        uint32_t pages = parent->user_stack_pages;
        child_stack = pmm_alloc_pages(pages);
        if (child_stack == 0) {
            fail = 1;
        } else {
            uint32_t sva = USER_STACK_TOP - pages * PMM_PAGE_SIZE;
            for (uint32_t k = 0; k < pages; k++) {
                uint32_t p = sva + k * PMM_PAGE_SIZE;
                uint32_t phys = 0, flags = 0;
                if (paging_query(p, &phys, &flags) != 0) {
                    pmm_free_pages(child_stack, pages);
                    child_stack = 0;
                    fail = 1;
                    break;
                }
                t_memcpy((void *)(child_stack + k * PMM_PAGE_SIZE),
                         (const void *)p, PMM_PAGE_SIZE);
                child_pt_set(pt_phys, p, child_stack + k * PMM_PAGE_SIZE, flags);
            }
        }
    }

    /* 失败：按自己记的账对称回滚，绝不回查页表 */
    if (fail) {
        for (uint32_t s = 0; s < nseg; s++)
            if (child_seg[s]) pmm_free_pages(child_seg[s], parent->img.seg[s].pages);
        if (child_stack) pmm_free_pages(child_stack, parent->user_stack_pages);
        pmm_free_page(pt_phys);
        pmm_free_page(pd_phys);
        task_unlock();
        return -1;
    }

    /* 4) 登记子任务，复制 fd 表（pipe 递增引用计数在 fd_table_dup 内完成） */
    task_t *c = &g_tasks[slot];
    c->kstack     = (uint32_t)g_kstacks[slot];
    c->kstack_top = c->kstack + TASK_KSIZE;
    c->pid        = g_next_pid++;
    c->state      = TASK_READY;
    c->ticks      = TASK_TIMESLICE;
    c->switches   = 0;
    c->cr3        = pd_phys;
    c->is_user    = 1;
    c->exit_code  = 0;
    c->user_stack_phys   = child_stack;
    c->user_stack_pages  = parent->user_stack_pages;
    c->pd_phys    = pd_phys;
    c->pt_phys    = pt_phys;
    c->img        = parent->img;                 /* 段布局/虚拟地址/页数一致 */
    for (uint32_t s = 0; s < nseg; s++)
        c->img.seg[s].phys = child_seg[s];        /* 物理页换成子进程自己的 */
    c->wait_next  = -1;
    c->wake_tick  = 0;
    c->wait_q     = 0;
    {
        const char *n = parent->name;
        int i = 0;
        for (; n[i] && i < 15; i++) c->name[i] = n[i];
        c->name[i] = '\0';
    }
    fd_table_dup(&c->fds, &parent->fds);

    /* 5) 摆子进程内核栈：ring3 出场帧，寄存器照搬父进程，eax 槽置 0 */
    /* 帧布局见 syscall.c 顶部注释（由 syscall_entry 汇编给出基址，不依赖 C prologue）：
     *   16 edi  17 esi  18 ebp  19 esp  20 ebx  21 edx  22 ecx  23 eax（pusha 区，/4 后）
     *   64/4=16 ... ; iret 帧：EIP=64 CS=68 EFLAGS=72 ESP=76 SS=80 -> 下标 16..20
     * 注意 pusha 区起始是 +16 字节 = 下标 4，别把两套下标混了。 */
    {
        uint32_t *sp = (uint32_t *)c->kstack_top;
        *--sp = fr[20];                  /* SS   （+80） */
        *--sp = fr[19];                  /* ESP  （+76） */
        *--sp = fr[18];                  /* EFLAGS（+72） */
        *--sp = fr[17];                  /* CS   （+68） */
        *--sp = fr[16];                  /* EIP  （+64） */
        *--sp = 0u;                      /* popa: eax（子进程返回 0） */
        *--sp = fr[10];                  /* ecx  （+40） */
        *--sp = fr[9];                   /* edx  （+36） */
        *--sp = fr[8];                   /* ebx  （+32） */
        *--sp = 0u;                      /* esp 占位（+28，popa 会弹掉，值无意义） */
        *--sp = fr[6];                   /* ebp  （+24） */
        *--sp = fr[5];                   /* esi  （+20） */
        *--sp = fr[4];                   /* edi  （+16） */
        *--sp = (uint32_t)task_irq_trampoline;   /* switch_to ret 目标 */
        *--sp = 0x0002u;                /* popfd 用 EFLAGS（IF=0） */
        *--sp = 0u;                     /* ebp  */
        *--sp = 0u;                     /* ebx  */
        *--sp = 0u;                     /* esi  */
        *--sp = 0u;                     /* edi  */
        c->esp = (uint32_t)sp;
    }

    int child_pid = (int)c->pid;
    task_unlock();                /* 此时子进程已完整（含栈帧），可被调度 */
    return child_pid;             /* 父进程拿到子 pid；子进程经栈帧返回 0 */
}

/* ---------- 等待队列与可睡眠阻塞（步骤 8a） ----------
 *
 * 正确性核心：丢失唤醒在结构上不可能。
 *   task_sleep 全程 IF=0——"查条件→入队→置 BLOCKED→切换"这条路径上
 *   任何唤醒源（IRQ1 键盘 / IRQ11 网卡 / IRQ0 定时）都无法插进来：
 *   唤醒者要么整体先跑（条件已真，等待者根本不入队），要么整体后跑
 *   （必然看得到入队与 BLOCKED 置位）。单核上，cli 窗口就是原子性。
 *
 * 超时用 g_pit_ticks（1ms/格，uint32 约 49.7 天回绕）：deadline 比较用
 * 回绕安全的有符号差。注意"超时打断后再次 task_sleep"会重新计满整个
 * timeout——本原语的调用方（wait_cond 风格包装）都持自己的绝对
 * deadline，不依赖 task_sleep 保留剩余预算。
 *
 * schedule() 发现无人可切而直接返回时，睡眠者"顶着 BLOCKED 状态"继续
 * 持有 CPU（状态对调度器的谎言）：此时它留在队列上，由 task_sleep 收尾
 * 摘链并 sti;hlt 停机等唤醒源。停机只能发生在任务上下文——schedule 若
 * 从 IRQ0 进来，被中断任务的 state 是 RUNNING，走不到这个分支。
 */

static void wq_push(wait_queue_t *wq, int idx) {
    g_tasks[idx].wait_next = wq->head;
    g_tasks[idx].wait_q = wq;
    wq->head = idx;
}

static void wq_remove(wait_queue_t *wq, int idx) {
    int *p = &wq->head;
    while (*p != -1) {
        if (*p == idx) {
            *p = g_tasks[idx].wait_next;
            g_tasks[idx].wait_next = -1;
            g_tasks[idx].wait_q = 0;
            return;
        }
        p = &g_tasks[*p].wait_next;
    }
}

void task_timer_tick(void) {
    if (!g_ready) return;
    uint32_t now = g_pit_ticks;
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        task_t *t = &g_tasks[i];
        if (t->state != TASK_BLOCKED || t->wake_tick == 0) continue;
        if ((int32_t)(now - t->wake_tick) < 0) continue;   /* 未到期 */
        if (t->wait_q != 0) wq_remove(t->wait_q, (int)i);
        t->wake_tick = 0;
        t->state = TASK_READY;     /* 真唤醒还是超时，由等待循环自查条件 */
    }
}

void task_wake_all(wait_queue_t *wq) {
    if (!g_ready || wq == 0) return;
    int i = wq->head;
    while (i != -1) {
        int nx = g_tasks[i].wait_next;
        g_tasks[i].wait_next = -1;
        g_tasks[i].wait_q = 0;
        g_tasks[i].wake_tick = 0;
        /* 只动真正睡着的。顶 BLOCKED 状态持 CPU 的睡眠者（见文件头）
         * 还没被切走，wake_all 若把它置 READY 会让 schedule 多派一次——
         * 但它随后的收尾会把状态归位，这里保持只字面唤醒。 */
        if (g_tasks[i].state == TASK_BLOCKED) g_tasks[i].state = TASK_READY;
        i = nx;
    }
    wq->head = -1;
}

void task_sleep(wait_queue_t *wq, uint32_t timeout_ms) {
    if (!g_ready || wq == 0) return;
    if (g_lock_depth != 0) return;     /* 临界区内不许睡：schedule 也不切 */

    int me = (int)g_current;
    task_t *t = &g_tasks[me];

    asm volatile("cli");
    t->wake_tick = timeout_ms ? (g_pit_ticks + timeout_ms) : 0;
    t->state = TASK_BLOCKED;
    wq_push(wq, me);
    schedule();                        /* 全程 IF=0：路径不可分割 */

    /* 回到这里两种可能：
     *   a) 被切走又切回——唤醒/超时方已摘链，调度器派发时置了 RUNNING；
     *   b) 无人可切，schedule 未切换直接返回——本任务"顶着 BLOCKED"
     *      继续持 CPU，且仍在队列上（这正是要的：wake_all 与 timer_tick
     *      都还能找到它。若在此前摘链/清 deadline，停机后就再无人能唤醒
     *      ——PIT 到点无人可标 READY，等于自锁）。 */
    if (t->state == TASK_BLOCKED) {
        /* b)：保持登记，开中断停机。从 hlt 醒来即某个唤醒源已跑完：
         * 可能是真唤醒、超时、或无关中断（假唤醒——调用方循环重查条件）。
         * 无论哪种，这里归一化状态后返回，由调用方判定下一步。 */
        asm volatile("sti; hlt");
        if (t->wait_q == wq) wq_remove(wq, me);
        t->wake_tick = 0;
        t->state = TASK_RUNNING;       /* 此刻 CPU 就在它手里，如实登记 */
        return;
    }
    t->wake_tick = 0;                  /* a)：清残留 deadline 即可 */
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
        /* BLOCKED（步骤 8a）：睡在等待队列上，只等唤醒，不参与轮转 */
        if (g_tasks[cand].state == TASK_READY || g_tasks[cand].state == TASK_RUNNING) {
            next = cand;
            break;
        }
    }
    if (next == g_current) {
        /* 没有别的候选：睡眠者的等待循环自己负责停机等待（task_sleep
         * 里的 sti;hlt——只能任务上下文做，中断上下文 sti 会引 PIT 嵌套），
         * 这里直接返回，让调用方（很可能正睡在循环里）继续。 */
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
static wait_queue_t g_wq_self = {-1};   /* 自检：定时睡眠用（哨兵 -1，同上） */

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

    /* 4) 定时睡眠（步骤 8a）：此刻没有别的可运行任务——task_sleep 会
     * 走"无人可切 → 保持入队登记 → sti;hlt 停机"路径，30ms 后由 IRQ0
     * 的 task_timer_tick 标 READY 并把 CPU 从 hlt 拉起来。睡眠期间
     * tick 必须前进（ woke = PIT 没停摆），且偏差在教学容差内。 */
    {
        uint32_t t0 = g_pit_ticks;
        task_sleep(&g_wq_self, 30);
        uint32_t dt = g_pit_ticks - t0;
        int ok_sleep = (dt >= 25u && dt <= 500u);
        out("  timed sleep 30ms woke via PIT: ");
        out(ok_sleep ? "yes [OK]\n" : "NO [FAIL]\n");
        if (!ok_sleep) fail++;
    }

    out(fail == 0 ? "  result: PASS\n" : "  result: FAIL\n");
    return fail;
}
