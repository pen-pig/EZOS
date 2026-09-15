/*
 * syscall.c - 系统调用实现 + 用户态启动（步骤 4c）
 */
#include "syscall.h"
#include "idt.h"
#include "tty.h"
#include "keyboard.h"
#include "paging.h"
#include "panic.h"
#include "task.h"
#include "fd.h"

/* kernel_entry.asm */
extern void syscall_entry(void);
extern int  enter_usermode(uint32_t entry, uint32_t user_esp);
extern uint32_t g_user_exited;

static uint32_t g_syscalls;

/* ---------- 无 libc 的小工具 ---------- */
static uint32_t us_len(const char *s) {
    uint32_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static void us_copy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

/* ---------- 用户指针校验（安全边界） ---------- */
/*
 * 要求 [va, va+len) 范围内每一页都：
 *   1) 已映射（present）——否则内核解引用用户指针会 #PF
 *   2) 带 PTE_US——否则用户可借系统调用读写任意内核内存
 *   3) writable=1 时还须带 PTE_RW——内核会写这块内存。少了这条，
 *      用户只要传一个只读页（将来 .text 按 W^X 映射为 RO 就是），
 *      就能让内核在解引用时 #PF 崩溃，等于一条本地 DoS。
 * 长度 0 视为通过（无需解引用）。
 */
static int user_range_ok(uint32_t va, uint32_t len, int writable) {
    if (len == 0) return 1;
    if (va + len < va) return 0;                 /* 地址环绕 */
    uint32_t first = va & ~(uint32_t)0xFFF;
    uint32_t last  = (va + len - 1) & ~(uint32_t)0xFFF;
    for (uint32_t p = first; ; p += 0x1000u) {
        uint32_t phys = 0, flags = 0;
        if (paging_query(p, &phys, &flags) != 0) return 0;
        if (!(flags & PTE_US)) return 0;
        if (writable && !(flags & PTE_RW)) return 0;
        if (p == last) break;
    }
    return 1;
}

/* ---------- 系统调用分发 ---------- */
int syscall_handler(uint32_t num, uint32_t a1, uint32_t a2, uint32_t a3) {
    g_syscalls++;
    task_t *cur = task_current();

    switch (num) {
    case SYS_WRITE: {
        uint32_t fd = a1;
        const char *buf = (const char *)a2;
        uint32_t n = a3;
        if (!user_range_ok(a2, n, 0)) return -1;   /* 只读访问用户缓冲 */
        if (cur == 0) return -1;
        return fd_write(&cur->fds, (int)fd, (const uint8_t *)buf, n);
    }
    case SYS_READ: {
        /* 标准输入非阻塞：keyboard_getchar() 空缓冲返回 0（见 fd.c fd_read）。
         * 真正的阻塞读要等调度器把任务睡眠在键盘等待队列上（6c 之后）。 */
        uint32_t fd = a1;
        char *buf = (char *)a2;
        uint32_t n = a3;
        if (!user_range_ok(a2, n, 1)) return -1;   /* 内核要写，必须可写 */
        if (cur == 0) return -1;
        return fd_read(&cur->fds, (int)fd, (uint8_t *)buf, n);
    }
    case SYS_OPEN: {
        /* 文件名是用户指针：逐字节校验并拷进内核栈缓冲 */
        const char *u_name = (const char *)a1;
        int flags = (int)a2;
        char kname[FD_MAX_NAME];
        uint32_t i = 0;
        while (i < FD_MAX_NAME - 1) {
            if (!user_range_ok(a1 + i, 1, 0)) return -1;
            char c = u_name[i];
            if (c == '\0') break;
            kname[i] = c;
            i++;
        }
        if (i >= FD_MAX_NAME - 1) return -1;       /* 未遇 NUL：过长 */
        kname[i] = '\0';
        if (cur == 0) return -1;
        return fd_open(&cur->fds, kname, flags);
    }
    case SYS_CLOSE: {
        uint32_t fd = a1;
        if (cur == 0) return -1;
        return fd_close(&cur->fds, (int)fd);
    }
    case SYS_LSEEK: {
        uint32_t fd = a1;
        int32_t offset = (int32_t)a2;
        int whence = (int)a3;
        if (cur == 0) return -1;
        return fd_lseek(&cur->fds, (int)fd, offset, whence);
    }
    case SYS_EXIT:
        if (cur != 0 && cur->is_user) {
            /* 用户进程退出（步骤 6c）：拆地址空间、关 fd、记退出码并让出 CPU。
             * 它不会返回——本任务的 ring3 上下文就此作废。 */
            process_exit((int)a1);
        }
        /* utest 的内建用户程序：同步演示路径（task 0 上跑，没有独立地址空间），
         * 靠 g_user_exited 让 syscall_entry 直接回到 enter_usermode() 的调用者 */
        g_user_exited = 1;
        return (int)a1;
    default:
        return -1;
    }
}

uint32_t syscall_count(void) { return g_syscalls; }

void syscall_init(void) {
    /* 0xEE = 32-bit 中断门，DPL=3：允许 ring3 执行 int 0x80。
     * 用 0x8E（DPL=0）的话用户态 int 0x80 会直接 #GP。 */
    idt_set_gate(SYSCALL_VECTOR, (uint32_t)syscall_entry, 0x08, 0xEE);
}

/* ---------- 内建用户程序 ---------- */
/*
 * 32-bit 平面机器码（无重定位，装载到 USER_CODE_VA）：
 *
 *   mov eax, SYS_WRITE ; mov ebx, 1 ; mov ecx, msg1 ; mov edx, len1 ; int 0x80
 *   mov eax, SYS_WRITE ; mov ebx, 1 ; mov ecx, msg2 ; mov edx, len2 ; int 0x80
 *   mov eax, SYS_EXIT  ; mov ebx, 0 ; int 0x80
 *   msg1 / msg2（紧跟在代码后，地址在装载时回填）
 *
 * 两条 write 各往返一次 ring3->ring0->ring3，足以证明返回路径正确。
 * imm32 的偏移：ecx@11, edx@16（第一条）；ecx@33, edx@38（第二条）。
 */
static const uint8_t user_prog[] = {
    0xB8, 0x01, 0x00, 0x00, 0x00,       /* 00 mov eax,1        */
    0xBB, 0x01, 0x00, 0x00, 0x00,       /* 05 mov ebx,1        */
    0xB9, 0x00, 0x00, 0x00, 0x00,       /* 10 mov ecx,imm32    */
    0xBA, 0x00, 0x00, 0x00, 0x00,       /* 15 mov edx,imm32    */
    0xCD, 0x80,                         /* 20 int 0x80         */
    0xB8, 0x01, 0x00, 0x00, 0x00,       /* 22 mov eax,1        */
    0xBB, 0x01, 0x00, 0x00, 0x00,       /* 27 mov ebx,1        */
    0xB9, 0x00, 0x00, 0x00, 0x00,       /* 32 mov ecx,imm32    */
    0xBA, 0x00, 0x00, 0x00, 0x00,       /* 37 mov edx,imm32    */
    0xCD, 0x80,                         /* 42 int 0x80         */
    0xB8, 0x02, 0x00, 0x00, 0x00,       /* 44 mov eax,2        */
    0xBB, 0x00, 0x00, 0x00, 0x00,       /* 49 mov ebx,0        */
    0xCD, 0x80,                         /* 54 int 0x80         */
};
#define USER_PROG_SIZE   sizeof(user_prog)      /* 56 */

static const char user_msg1[] = "[ring3] hello from user mode (int 0x80 write)\n";
static const char user_msg2[] = "[ring3] second syscall returned to ring3 OK\n";

int usermode_run_demo(void) {
    /* 1) 用户页：代码页 + 栈页，都是 identity 物理页，权限加 PTE_US */
    uint32_t stack_page = USER_STACK_TOP - 0x1000u;
    if (paging_map(USER_CODE_VA, USER_CODE_VA, PAGING_USR_FLAGS) != 0) return -1;
    if (paging_map(stack_page, stack_page, PAGING_USR_FLAGS) != 0) return -1;

    /* 2) 装入程序 + 回填字符串地址 */
    uint8_t *code = (uint8_t *)USER_CODE_VA;
    us_copy(code, user_prog, USER_PROG_SIZE);

    char *m1 = (char *)(USER_CODE_VA + USER_PROG_SIZE);
    uint32_t l1 = us_len(user_msg1);
    us_copy(m1, user_msg1, l1 + 1);

    char *m2 = m1 + l1 + 1;
    uint32_t l2 = us_len(user_msg2);
    us_copy(m2, user_msg2, l2 + 1);

    *(uint32_t *)(code + 11) = (uint32_t)m1;
    *(uint32_t *)(code + 16) = l1;
    *(uint32_t *)(code + 33) = (uint32_t)m2;
    *(uint32_t *)(code + 38) = l2;

    /* 3) 用户态执行期间若崩，panic 屏能显示这条上下文 */
    panic_set_context("ring3 user program");

    /* 4) 切入 ring3。用户程序 SYS_EXIT 前不返回。 */
    int rc = enter_usermode(USER_CODE_VA, USER_STACK_TOP);

    panic_set_context("shell");
    return rc;
}

/* 供 shell/自检调用：确认安全边界确实拒绝非用户页（只读语义） */
int syscall_user_range_ok(uint32_t va, uint32_t len) {
    return user_range_ok(va, len, 0);
}

/* 自检用可写语义：拒绝"已映射用户页但只读"的情况 */
int syscall_user_range_rw(uint32_t va, uint32_t len) {
    return user_range_ok(va, len, 1);
}
