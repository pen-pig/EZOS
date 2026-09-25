/*
 * task.h - 任务（内核线程）与抢占式调度器（步骤 6a）
 *
 * 当前形态与边界（明确的"还没做什么"，避免看起来比实际更完整）：
 *   - 支持内核线程（ring0），抢占式时间片轮转
 *   - 每个任务有独立内核栈，切换时更新 TSS.esp0（ring3 陷入栈）
 *   - 单地址空间：所有任务共用内核页目录，不做 per-process CR3 切换
 *     （步骤 6c 的多用户进程才会引入）
 *   - 没有优先级、没有 SMP；阻塞睡眠用静态侵入式等待队列（步骤 8a：
 *     task_sleep/task_wake_all——条件检查与入队之间关中断，
 *     丢失唤醒窗口被结构性排除，超时由 PIT tick 驱动）
 *
 * 切换机制（为什么必须是这样）：
 *   抢占发生在 IRQ0（PIT）上下文里。被中断任务的全部寄存器由 irq0 桩的
 *   pusha 保存；switch_to 只需保存 C 调用约定里的 callee-saved 寄存器
 *   （ebp/ebx/esi/edi）+ 返回地址。切到新任务时，新任务的栈顶正是它当初
 *   被切走时在 switch_to 里留下的那一帧，pop 完 ret 回去，就沿着它自己的
 *   调用链返回到 irq0 桩，再 iret。
 *
 *   因此**新任务的栈必须预先摆成"被 switch_to 切走过"的样子**——
 *   见 task.c 的 stack_init。
 */
#ifndef TASK_H
#define TASK_H

#include "types.h"
#include "fd.h"
#include "elf.h"

#define MAX_TASKS      8
#define TASK_KSIZE     0x4000u          /* 每任务内核栈 16KB */
#define TASK_TIMESLICE 10               /* 时间片：10 个 PIT tick = 10ms */

/* 等待队列（步骤 8a）：静态侵入式单链——链节点就是任务表下标本身，
 * 无动态内存。head 为 -1 表示空队列。定义须在 task_t 之前（字段引用）。 */
typedef struct wait_queue {
    int head;                           /* 队首任务下标；-1 = 空 */
} wait_queue_t;

typedef enum {
    TASK_UNUSED  = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_ZOMBIE
} task_state_t;

typedef struct {
    uint32_t     esp;            /* 切换点栈指针；任务不在运行时有效 */
    uint32_t     kstack;         /* 内核栈底 */
    uint32_t     kstack_top;     /* 内核栈顶（也是 TSS.esp0 的值） */
    uint32_t     pid;
    task_state_t state;
    uint32_t     ticks;          /* 剩余时间片 */
    uint32_t     switches;       /* 被调度到的次数（观测调度器是否真在工作） */
    char         name[16];
    fd_table_t   fds;            /* 本任务的文件描述符表（步骤 6b） */

    /* ---- 步骤 6c：用户进程（is_user=1）才有意义 ---- */
    uint32_t     cr3;               /* 进程页目录物理地址；0 = 用内核页目录 */
    uint8_t      is_user;           /* 1 = 用户进程：有独立地址空间，ring3 执行 */
    int          exit_code;         /* 退出码（ZOMBIE 时有效） */
    uint32_t     user_stack_phys;   /* 用户栈首物理页（连续 user_stack_pages 页） */
    uint32_t     user_stack_pages;
    uint32_t     pd_phys;           /* 进程页目录物理页（回收用记账） */
    uint32_t     pt_phys;           /* 用户区页表物理页（4-8MB，回收用记账） */
    elf_image_t  img;               /* ELF 段记账：退出时按账回收物理页 */

    /* ---- 步骤 8a：可睡眠的阻塞原语（等待队列） ---- */
    int          wait_next;         /* 所在等待队列链的 next（任务下标），-1 = 不在队 */
    uint32_t     wake_tick;         /* 定时睡眠的唤醒截止（g_pit_ticks），0 = 非定时 */
    wait_queue_t *wait_q;           /* 正在排队的队列（唤醒/超时摘链用） */
} task_t;

/* 初始化任务表，并把当前执行流（kernel_main）登记为 0 号任务。
 * 必须在 pit_init 之后、任何 task_create 之前调用。 */
void task_init(void);

/* 创建内核线程。fn 返回后任务自动退出。返回 pid，失败返回 -1。 */
int task_create(const char *name, void (*fn)(void *), void *arg);

/*
 * 创建**用户进程**任务（步骤 6c）。与内核线程的区别：
 *   - 首次被调度到时经 task_irq_trampoline 的 iret 落到 ring3（CS=0x1B），
 *     而不是进 ring0 的 C 函数；
 *   - 有自己的页目录 pd_phys，调度到它时会切 CR3；
 *   - 退出走 process_exit()（拆地址空间 + 记退出码），不是 task_exit()。
 * 返回 pid，失败返回 -1。地址空间的回收账目由调用方（exec）填进 task_t。
 */
int task_create_process(const char *name, uint32_t entry, uint32_t user_esp,
                        uint32_t pd_phys);

/* 按 pid 找任务（找不到返回 0） */
task_t *task_find(int pid);

/*
 * 用户进程退出（SYS_EXIT 调用，永不返回）：
 * 切回内核地址空间 → 按账回收 ELF 段/用户栈/页表/页目录 → 关掉它的 fd
 * （dirty 落盘）→ 记退出码 → 置 ZOMBIE → 让出 CPU。
 * 用户进程的 ZOMBIE 不会被 task_reap_zombies 自动回收，要等 task_wait_pid
 * 取走退出码（wait 语义）。
 */
void process_exit(int code);

/* 等待指定 pid 结束：轮询 + 让出 CPU，结束后回收任务槽并返回退出码。
 * pid 不存在返回 -1。 */
int task_wait_pid(int pid);

/*
 * task_wait_pid 的可选项变体（后台任务收割用）。
 *   options & WNOHANG：子进程仍在运行时不阻塞，返回 -2（不收割）；
 *                       子进程已退出则照常回收并返回其退出码（>=0）。
 *   options = 0        ：等价于 task_wait_pid(pid)（阻塞直到退出或被其它 waiter 收走）。
 * 返回：>=0 退出码；-1 无效/已不存在的 pid；-2 WNOHANG 且仍在运行。
 */
int task_wait_pid_opt(int pid, int options);

/*
 * fork 当前用户进程（SYS_FORK 实现）：复制父进程的用户地址空间（4-8MB 区间，
 * 逐页深拷贝、每页独立记账）与 fd 表（普通文件 data 深拷贝、pipe 递增引用计数），
 * 新建一个可运行子任务。父进程返回子 pid，子进程返回 0，失败返回 -1。
 * syscall_frame 必须是 syscall_handler 入口处的 esp（用于复制父进程的用户寄存器
 * 帧，使子进程从 fork 调用点之后原样继续）。复制中途失败会对称回滚已分配资源。
 * 调用者须为 is_user 的用户进程（内核线程无独立地址空间，fork 返回 -1）。
 */
int task_fork(void *syscall_frame);

/* 当前任务主动放弃 CPU（协作式让位，抢占之外的补充） */
void task_yield(void);

/*
 * 睡到被唤醒或超时（步骤 8a 的阻塞原语）。
 *   - timeout_ms=0 表示无限等；否则按 PIT tick（1ms/格）定时到点唤醒
 *   - 只能在任务上下文（系统调用/内核线程）调用，不得在 task_lock
 *     临界区内（那里 schedule 拒绝切换，本函数退化为立即返回）
 *   - 唤醒方（IRQ 处理/资源释放方）调 task_wake_all；惊群语义：
 *     醒来的等待者各自重查条件，不满足就再睡（≤ MAX_TASKS，无负担）
 */
void task_sleep(wait_queue_t *wq, uint32_t timeout_ms);

/* 唤醒整条等待队列（IRQ 处理 / 资源就绪方调用；IF=0 上下文安全） */
void task_wake_all(wait_queue_t *wq);

/* PIT 每 tick 调用（IRQ0 上下文）：到点的定时睡眠者回 READY */
void task_timer_tick(void);

/* 当前任务退出（fn 返回时自动调用；也可显式调用） */
void task_exit(void);

/* 调度器：选下一个 RUNNABLE 任务并切换。由 IRQ0 与 task_yield 调用。 */
void schedule(void);

/* 查询接口（shell 的 ps 命令 / 自检用） */
task_t *task_current(void);
const task_t *task_table(uint32_t *out_count);
uint32_t task_count_running(void);

/* 统计：调度总次数 */
uint32_t schedule_count(void);

/*
 * 临界区：关调度（不是关中断）。
 * 用于保护"被多个任务共享但还不是可重入的"内核设施——现阶段是终端输出
 * 和 ATA 读盘：它们内部有静态缓冲，两个任务同时进去会互相踩。
 * 实现为禁止调度 + 关中断（单核下等价且更安全：IRQ0 不会再来抢）。
 */
void task_lock(void);
void task_unlock(void);

/* 自检：调度器是否真的在轮转（跑两个计数线程一段时间后比对）
 * 返回失败的断言数（0=全过）。 */
int task_selftest(void (*out)(const char *));

#endif
