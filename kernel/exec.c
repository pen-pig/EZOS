/*
 * exec.c - 从文件系统装载并运行用户程序（步骤 5d 装载 / 步骤 6c 进程化）
 *
 * 步骤 6c 起，exec 不再是"借 shell 的栈和地址空间同步跑一段"：
 *   1) 读文件 + ELF 校验（不可信输入，全部在装载前拦下）
 *   2) 为进程建独立页目录：内核区（identity + LFB）共享内核页表，
 *      用户区 4-8MB 用**本进程私有**的页表
 *   3) 装载 ELF、分配并清零用户栈、按 System V i386 压 argc/argv
 *   4) task_create_process()：登记成一个可抢占的用户进程（ring3 出场帧）
 *   5) exec_file() 阻塞等待它结束（wait 语义），返回退出码
 *
 * 为什么第 2 步要用"临时借用内核 PD[1]"的小技巧：
 *   elf_load()/paging_map() 都是对**当前 CR3** 的页表操作，而装载必须在
 *   进程的私有页表里落 PTE。做法是把 4-8MB 的 PDE 临时指向子进程页表，
 *   装载完再还原 + 刷 TLB。整个过程只有一个可运行任务（shell 自己），
 *   不会被抢占打断——这是本实现成立的前提。
 *
 * 安全要点：
 *   - ELF 是**不可信输入**。所有越界检查都在 elf.c，这里不重复信任它。
 *   - 用户栈整块清零后才交出去：页刚从 pmm 出来，内容是上一个使用者的
 *     残留（可能是内核数据），直接给用户等于信息泄漏。
 *   - argv 字符串与指针数组都放在用户栈上，且必须落在已映射的栈页内。
 *     用户程序传进来的指针由内核解引用前还要再过一次 user_range_ok。
 */
#include "exec.h"
#include "elf.h"
#include "fs.h"
#include "paging.h"
#include "pmm.h"
#include "syscall.h"
#include "task.h"
#include "panic.h"
#include "kmalloc.h"

/* ---------- 小工具（不依赖 libc） ---------- */
static void x_memset(void *dst, uint8_t v, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < n; i++) d[i] = v;
}

static uint32_t x_strlen(const char *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

/*
 * 把一段字符串压到用户栈上（向低地址生长），返回新的栈顶。
 * 返回 0 表示会越过栈底 floor —— 调用方必须据此放弃并回滚：
 * 越过 floor 就是写到未映射的地址，内核当场 #PF。
 */
static uint32_t ustack_push_str(uint32_t sp, uint32_t floor,
                                const char *s, uint32_t len) {
    if (len + 1u > sp - floor) return 0;
    sp -= len + 1u;
    char *d = (char *)sp;
    for (uint32_t i = 0; i < len; i++) d[i] = s[i];
    d[len] = '\0';
    return sp;
}

/* ---------- 主体 ---------- */

/*
 * 回收 spawn 过程中已分配的一切。
 * 必须在"内核 PD[1] 仍指向子进程页表"时调用——这样 elf_unload 的
 * paging_unmap 正好落在私有页表上；调用方随后还原 PDE 并刷 TLB。
 */
static void spawn_teardown(elf_image_t *img, uint32_t stack_base,
                           uint32_t sphys, uint32_t pages,
                           uint32_t pd_phys, uint32_t pt_phys) {
    if (img) elf_unload(img);
    if (sphys) {
        for (uint32_t k = 0; k < pages; k++) {
            paging_unmap(stack_base + k * PMM_PAGE_SIZE);
            pmm_free_page(sphys + k * PMM_PAGE_SIZE);
        }
    }
    if (pt_phys) pmm_free_page(pt_phys);
    if (pd_phys) pmm_free_page(pd_phys);
}

/*
 * 归还"临时借用的内核 PD[1]"并解除抢占禁令。
 *
 * 借用期间必须关中断：PD[1] 被改成了子进程的私有页表，此时若有别的任务
 * 被调度进来，它看到的 4-8MB 是别人的用户空间——即便本实现里 exec 期间
 * 只有 shell 一个可运行任务（exec 阻塞在 wait 上），这个前提一旦被后续
 * 步骤（后台任务 / 网络）打破就是静默的内存破坏。关中断把它变成不变式。
 *
 * 还原后必须重载 CR3：借用期间在 4-8MB 上建立的 TLB 缓存全部作废。
 */
static void restore_borrow(uint32_t save_pde1) {
    uint32_t *kpd = (uint32_t *)PAGING_PD_ADDR;
    kpd[1] = save_pde1;
    paging_switch_cr3(PAGING_PD_ADDR);
    task_unlock();
}

int exec_file(const char *name, const char *args, const char **why) {
    #define FAIL(v, m) do { if (why) *why = (m); return (v); } while (0)

    if (name == 0 || name[0] == '\0') FAIL(-1, "missing file name");
    if (!fs_ready())                  FAIL(-1, "no filesystem mounted");

    uint32_t size = fs_get_file_size(name);
    if (size == 0)                    FAIL(-2, "file not found or empty");
    if (size > EXEC_MAX_IMAGE)        FAIL(-3, "file too large");

    uint8_t *buf = (uint8_t *)kmalloc(size);
    if (buf == 0)                     FAIL(-3, "out of kernel heap");
    /* fs_read_file 成功时返回读取字节数（>0），失败返回 -1：
     * 必须核对字节数而不是判 0——判 0 会把成功当失败。 */
    if (fs_read_file(name, buf, size) != (int)size) {
        kfree(buf);
        FAIL(-2, "read failed");
    }

    const char *ewhy = 0;
    if (elf_validate(buf, size, &ewhy) != 0) {
        kfree(buf);
        if (why) *why = (ewhy ? ewhy : "not a valid ELF32 image");
        return -4;
    }

    /* ---- 1) 进程地址空间：页目录 + 用户区页表 ---- */
    uint32_t *kpd = (uint32_t *)PAGING_PD_ADDR;      /* 内核页目录（identity） */
    uint32_t pd_phys = pmm_alloc_page();
    uint32_t pt_phys = pmm_alloc_page();
    if (pd_phys == 0 || pt_phys == 0) {
        if (pd_phys) pmm_free_page(pd_phys);
        if (pt_phys) pmm_free_page(pt_phys);
        kfree(buf);
        FAIL(-4, "out of pages for process address space");
    }
    /* pmm 契约：返回的页内容未定义，交出去前必须自己清零 */
    x_memset((void *)pd_phys, 0, PMM_PAGE_SIZE);
    x_memset((void *)pt_phys, 0, PMM_PAGE_SIZE);

    /* 内核区在**所有**进程页目录里共享同一批页表（identity + LFB），
     * 用户区（4-8MB，PDE 索引 1）换成本进程私有页表。 */
    uint32_t *upd = (uint32_t *)pd_phys;
    for (uint32_t i = 0; i < 1024u; i++) {
        if (i == 1u) continue;
        if (kpd[i] & PTE_P) upd[i] = kpd[i];
    }
    upd[1] = pt_phys | PTE_P | PTE_RW | PTE_US;

    /* 临时借用：让 4-8MB 的映射操作落进子进程页表。
     * 改完 PDE 必须立刻刷 TLB——否则装载会写进物理 4-8MB 的旧 identity 页。 */
    task_lock();                      /* 借用期间禁止抢占（见 restore_borrow） */
    uint32_t save_pde1 = kpd[1];
    kpd[1] = pt_phys | PTE_P | PTE_RW;
    paging_switch_cr3(PAGING_PD_ADDR);

    /* ---- 2) 装载 ELF（映射 + 拷贝 + 清 bss + W^X） ---- */
    elf_image_t img;
    ewhy = 0;
    if (elf_load(buf, size, &img, &ewhy) != 0) {
        spawn_teardown(0, 0, 0, 0, pd_phys, pt_phys);
        restore_borrow(save_pde1);
        kfree(buf);
        if (why) *why = (ewhy ? ewhy : "ELF load failed");
        return -4;
    }

    /* ---- 3) 用户栈：连续物理页，整块清零后才交出去 ---- */
    const uint32_t pages = USER_STACK_PAGES;
    const uint32_t stack_base = USER_STACK_TOP - pages * PMM_PAGE_SIZE;

    uint32_t sphys = pmm_alloc_pages(pages);
    if (sphys == 0) {
        spawn_teardown(&img, stack_base, 0, 0, pd_phys, pt_phys);
        restore_borrow(save_pde1);
        kfree(buf);
        FAIL(-5, "out of physical pages for user stack");
    }
    for (uint32_t k = 0; k < pages; k++) {
        if (paging_map(stack_base + k * PMM_PAGE_SIZE,
                       sphys + k * PMM_PAGE_SIZE, PAGING_USR_FLAGS) != 0) {
            /* 已映射的前 k 页逐页还，剩余 k..pages-1 整块还（对称回滚） */
            for (uint32_t j = 0; j < k; j++) {
                paging_unmap(stack_base + j * PMM_PAGE_SIZE);
                pmm_free_page(sphys + j * PMM_PAGE_SIZE);
            }
            pmm_free_pages(sphys + k * PMM_PAGE_SIZE, pages - k);
            spawn_teardown(&img, 0, 0, 0, pd_phys, pt_phys);
            restore_borrow(save_pde1);
            kfree(buf);
            FAIL(-5, "user stack mapping failed");
        }
    }
    x_memset((void *)stack_base, 0, pages * PMM_PAGE_SIZE);

    /* ---- 4) argc / argv：argv[0] 是程序名，其后是命令行参数 ---- */
    uint32_t sp = USER_STACK_TOP;
    uint32_t uargv[EXEC_MAX_ARGV];
    int argc = 0;
    const char *fail_argv = 0;

    sp = ustack_push_str(sp, stack_base, name, x_strlen(name));
    if (sp == 0) fail_argv = "argument list does not fit in user stack";
    else uargv[argc++] = sp;

    if (fail_argv == 0 && args != 0) {
        const char *p = args;
        while (*p != '\0' && argc < EXEC_MAX_ARGV) {
            while (*p == ' ') p++;
            if (*p == '\0') break;
            const char *tok = p;
            while (*p != '\0' && *p != ' ') p++;
            sp = ustack_push_str(sp, stack_base, tok, (uint32_t)(p - tok));
            if (sp == 0) {
                fail_argv = "argument list does not fit in user stack";
                break;
            }
            uargv[argc++] = sp;
        }
    }

    if (fail_argv == 0) {
        sp &= ~3u;                                    /* 指针数组 4 字节对齐 */
        /* argc 本身还要写在 argv_base-4 处，故下界要多留 4 字节，
         * 否则 argc 会写到栈底以下的未映射页，内核当场 #PF。 */
        uint32_t argv_base = sp - (uint32_t)(argc + 1) * 4u;
        if (argv_base < stack_base + 4u || argv_base > USER_STACK_TOP)
            fail_argv = "argument list does not fit in user stack";
        else {
            for (int i = 0; i < argc; i++)
                *(uint32_t *)(argv_base + (uint32_t)i * 4u) = uargv[i];
            *(uint32_t *)(argv_base + (uint32_t)argc * 4u) = 0u;  /* argv[argc] */
            sp = argv_base - 4u;
            *(uint32_t *)sp = (uint32_t)argc;                     /* esp -> argc */
        }
    }

    if (fail_argv != 0) {
        spawn_teardown(&img, stack_base, sphys, pages, pd_phys, pt_phys);
        restore_borrow(save_pde1);
        kfree(buf);
        FAIL(-5, fail_argv);
    }

    /* ---- 5) 登记成用户进程，把回收账目交给它 ---- */
    int pid = task_create_process(name, img.entry, sp, pd_phys);
    if (pid < 0) {
        spawn_teardown(&img, stack_base, sphys, pages, pd_phys, pt_phys);
        restore_borrow(save_pde1);
        kfree(buf);
        FAIL(-4, "out of process slots");
    }
    task_t *tp = task_find(pid);
    if (tp != 0) {
        tp->img = img;                    /* 段物理页记账：退出时按账回收 */
        tp->user_stack_phys = sphys;
        tp->user_stack_pages = pages;
        tp->pt_phys = pt_phys;
    }

    /* ---- 6) 还原内核 PDE[1] 并刷掉装载期间建立的用户映射缓存 ---- */
    restore_borrow(save_pde1);
    kfree(buf);                            /* 映像已进进程页表，缓冲可以还回去 */

    /* ---- 7) 阻塞等待子进程结束（wait 语义），返回它的退出码 ---- */
    panic_set_context(name);
    int rc = task_wait_pid(pid);
    panic_set_context("shell");
    return rc;

    #undef FAIL
}
