/*
 * fd.c - 文件描述符实现（步骤 6b）
 *
 * 句柄模型：整文件读入内核堆内存（open 时），read/write 只在内存里移动
 * offset 与拷贝字节，close 时若 dirty 则用 fs_create_file 整文件落盘。
 * 之所以不实现"按扇区流式读"，是因为 fs 层现阶段只有整读/整写两个原语；
 * 文件都很小，这个模型足够且实现简单、不易错。
 *
 * 安全要点：
 *   - 用户指针的合法性（user_range_ok）由 syscall.c 在进本层之前校验，
 *     本层不再信任也不重复检查用户地址；
 *   - write 触发扩容时，新增长的字节必须清零——否则 close 落盘会把
 *     内核堆里上一个使用者的残留数据写进用户文件（信息泄漏）。
 */
#include "fd.h"
#include "fs.h"
#include "kmalloc.h"
#include "keyboard.h"
#include "tty.h"
#include "task.h"          /* task_sleep / task_wake_all：pipe 阻塞读写的唯一睡眠路径 */

/* ---------- pipe（匿名管道，步骤 6c 用户态生态第一步） ----------
 * 静态对象池、无动态分配：用尽则 fail closed 返回 -1。每个 pipe 一个 4096
 * 字节环形缓冲，读/写端各有独立引用计数；两端计数都为 0 时释放回池。
 * 阻塞/唤醒走 task_sleep / task_wake_all，且"条件判定 → 入队"全程 cli，
 * 单核下 cli 即原子，丢失唤醒在结构上不可能（项目在网络部分真实踩过这个坑）。
 *
 * fd_entry 复用约定（与标准流一致，避免同名被宏替换）：
 *   type   = FD_TYPE_PIPE
 *   writable= 1 表示这是"写端"，0 表示"读端"（FILE 类型里 writable 是写权限，
 *             此处语义不同但仅在 FD_TYPE_PIPE 分支内解释）
 *   data   = 指向 pipe_t 对象（共享，跨两个 fd 端） */
#define PIPE_MAX   16u             /* 池中 pipe 对象上限 */
#define PIPE_BUF   4096u           /* 每 pipe 环形缓冲字节数 */

typedef struct {
    uint8_t   used;               /* 1 = 已分配；0 = 空闲（释放回池） */
    uint16_t  ref_r;              /* 读端打开数 */
    uint16_t  ref_w;              /* 写端打开数 */
    uint32_t  head;               /* 写位置（环形，[0,PIPE_BUF)） */
    uint32_t  tail;               /* 读位置（环形，[0,PIPE_BUF)） */
    uint32_t  count;              /* 缓冲中可用字节数（区分"满/空"靠它，不靠 head==tail） */
    uint8_t  *buf;                /* 环形缓冲：**运行时 kmalloc**，不放静态 */
    wait_queue_t wq;              /* 读/写端共用一队列：唤醒后各自重查自身条件 */
} pipe_t;

/* BSS 零初始化：used=0（全部空闲）；wq.head 仅在 fd_pipe 分配时置 -1，
 * 空闲对象不会被引用，故初值 0 无碍。
 *
 * 环形缓冲为什么不静态：16 个 × 4096B = 64KB，落进低 .bss 会顶穿
 * linker.ld:41 的 `__bss_end <= 0x90000`（全量链接已实测失败一次）；
 * 挪去 .bss.hi 也不行——那个段只剩 ~21KB 余量。所以元数据静态、
 * 缓冲运行时从 kmalloc 申请，失败即 fail closed。 */
static pipe_t g_pipes[PIPE_MAX];

/* 从池里取一个空闲 pipe 对象（含缓冲），成功返回下标，
 * 池耗尽或 kmalloc 失败返回 -1。任何失败路径都不残留半成品（used 不许被
 * 置 1 后又不回收）。 */
static int pipe_alloc(void) {
    for (uint32_t i = 0; i < PIPE_MAX; i++) {
        if (g_pipes[i].used == 0) {
            uint8_t *buf = (uint8_t *)kmalloc(PIPE_BUF);
            if (buf == 0) return -1;            /* 没内存就别占槽 */
            for (uint32_t k = 0; k < PIPE_BUF; k++) buf[k] = 0;
            g_pipes[i].used = 1;
            g_pipes[i].buf = buf;
            g_pipes[i].ref_r = 0;
            g_pipes[i].ref_w = 0;
            g_pipes[i].head = 0;
            g_pipes[i].tail = 0;
            g_pipes[i].count = 0;
            g_pipes[i].wq.head = 0;
            return (int)i;
        }
    }
    return -1;
}

/* 释放 pipe 对象：两端引用都归零时调用。
 * 顺序很重要：先摘掉 used 标记并让 wq 失活，再 kfree —— 单核但有抢占和
 * IRQ，先释放内存的话，被抢占到别的路径上时它可能拿到一个已回池的对象。 */
static void pipe_free(pipe_t *p) {
    if (p->buf != 0) kfree(p->buf);
    p->buf = 0;
    p->head = 0;
    p->tail = 0;
    p->count = 0;
    p->ref_r = 0;
    p->ref_w = 0;
    p->used = 0;
}

/* ---------- 小工具 ---------- */
static uint32_t fd_strlen(const char *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

static void fd_copy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

/* 把文件句柄扩到 newsize（> size）字节。新增长区域清零。
 * 成功返回 1，失败（OOM / 超上限）返回 0 且句柄保持原状。 */
static int fd_grow(fd_entry_t *e, uint32_t newsize) {
    if (newsize > MAX_FD_FILE) return 0;
    uint8_t *nd = (uint8_t *)kmalloc(newsize);
    if (nd == 0) return 0;
    if (e->data != 0 && e->size > 0)
        fd_copy(nd, e->data, e->size);
    for (uint32_t i = e->size; i < newsize; i++) nd[i] = 0;   /* 防泄漏 */
    if (e->data != 0) kfree(e->data);
    e->data = nd;
    e->size = newsize;
    return 1;
}

/* ---------- 生命周期 ---------- */

void fd_table_init(fd_table_t *t) {
    if (t == 0) return;
    for (uint32_t i = 0; i < MAX_OPEN_FDS; i++) {
        t->fds[i].type = FD_TYPE_FREE;
        t->fds[i].writable = 0;
        t->fds[i].dirty = 0;
        t->fds[i].size = 0;
        t->fds[i].offset = 0;
        t->fds[i].data = 0;
        t->fds[i].name[0] = '\0';
    }
    t->fds[FD_STDIN].type = FD_TYPE_STDIN;
    t->fds[FD_STDOUT].type = FD_TYPE_STDOUT;
    t->fds[FD_STDERR].type = FD_TYPE_STDERR;
}

int fd_open(fd_table_t *t, const char *name, int flags) {
    if (t == 0 || name == 0 || name[0] == '\0') return -1;
    if (flags != O_RDONLY && flags != O_WRONLY && flags != O_RDWR) return -1;

    uint32_t nlen = fd_strlen(name);
    if (nlen >= FD_MAX_NAME) return -1;               /* 文件名过长 */

    uint32_t size = fs_get_file_size(name);
    /* fs_get_file_size 找不到返回 0，与"空文件"无法区分。读模式下
     * size==0 统一按"不存在或空"拒绝（本 OS 无空文件场景）。写模式下
     * size==0 视为"不存在或空"，从空开始建。 */
    if (size == 0 && flags == O_RDONLY) return -1;
    if (size > MAX_FD_FILE) return -1;
    /* O_WRONLY 是"创建或截断覆盖"语义：不读入旧内容，从空开始写。
     * 否则打开一个残留的大文件后 write 少量字节，close 落盘会把旧尾巴
     * 一并写回（文件比预期长）。 */
    if (flags == O_WRONLY) size = 0;

    /* 从 3 开始找空位（0/1/2 是标准流，永远占住） */
    int slot = -1;
    for (uint32_t i = 3; i < MAX_OPEN_FDS; i++) {
        if (t->fds[i].type == FD_TYPE_FREE) { slot = (int)i; break; }
    }
    if (slot < 0) return -1;

    uint8_t *data = 0;
    if (size > 0) {
        data = (uint8_t *)kmalloc(size);
        if (data == 0) return -1;
        if (fs_read_file(name, data, size) != (int)size) {
            kfree(data);
            return -1;
        }
    }

    fd_entry_t *e = &t->fds[slot];
    e->type = FD_TYPE_FILE;
    e->writable = (flags != O_RDONLY) ? 1 : 0;
    e->dirty = 0;
    e->size = size;
    e->offset = 0;
    e->data = data;
    fd_copy(e->name, name, nlen);
    e->name[nlen] = '\0';
    return slot;
}

int fd_close(fd_table_t *t, int fd) {
    if (t == 0 || fd < 0 || (uint32_t)fd >= MAX_OPEN_FDS) return -1;
    fd_entry_t *e = &t->fds[fd];
    if (e->type == FD_TYPE_FREE) return -1;

    /* pipe 端关闭：递减对应引用计数，两端皆 0 时释放回池，并唤醒对端
     * 重判 EOF（读端全关）/ 无读者（写端全关）。整段关中断避免与睡眠方
     * 的"判定→入队"窗口交错（丢失唤醒）。 */
    if (e->type == FD_TYPE_PIPE) {
        pipe_t *p = (pipe_t *)e->data;
        if (p != 0) {
            asm volatile("cli" ::: "memory");
            if (e->writable) { if (p->ref_w > 0) p->ref_w--; }
            else            { if (p->ref_r > 0) p->ref_r--; }
            if (p->ref_r == 0 && p->ref_w == 0)
                pipe_free(p);                      /* 两端皆关：释放回池 + 还缓冲 */
            task_wake_all(&p->wq);                 /* 唤醒对端重判条件 */
            asm volatile("sti" ::: "memory");
        }
        e->type    = FD_TYPE_FREE;
        e->writable= 0;
        e->dirty   = 0;
        e->size    = 0;
        e->offset  = 0;
        e->data    = 0;
        e->name[0] = '\0';
        return 0;
    }

    if (e->type != FD_TYPE_FILE) return -1;   /* 标准流不参与 close */

    if (e->dirty && e->name[0] != '\0') {
        /* 落盘：写回失败不阻塞释放内存（避免泄漏）；失败在返回值无感，
         * 因为 close 的 POSIX 语义不保证 fsync 成功。 */
        fs_create_file(e->name, e->data ? e->data : (const uint8_t *)"", e->size);
    }
    if (e->data != 0) kfree(e->data);

    e->type = FD_TYPE_FREE;
    e->writable = 0;
    e->dirty = 0;
    e->size = 0;
    e->offset = 0;
    e->data = 0;
    e->name[0] = '\0';
    return 0;
}

/* ---------- pipe 创建 ---------- */

int fd_pipe(fd_table_t *t, int *u_fds) {
    if (t == 0 || u_fds == 0) return -1;

    /* 1) 从静态池取一个 pipe 对象；池耗尽直接 fail closed。 */
    int pi = pipe_alloc();
    if (pi < 0) return -1;
    pipe_t *p = &g_pipes[pi];

    /* 2) 在本任务 fd 表找两个空槽（≥3，0/1/2 永远留给标准流）。
     *    若找不到两个空槽，须把刚取的池对象还回去，避免泄漏。 */
    int rfd = -1, wfd = -1;
    for (uint32_t i = 3; i < MAX_OPEN_FDS; i++) {
        if (t->fds[i].type == FD_TYPE_FREE) {
            if (rfd < 0) rfd = (int)i;
            else { wfd = (int)i; break; }
        }
    }
    if (rfd < 0 || wfd < 0) {
        pipe_free(p);             /* 释放回池并把已申请的缓冲还回去 */
        return -1;
    }

    /* 3) 初始化 pipe 对象：环形缓冲空，读/写端各 1 个引用。 */
    p->used  = 1;
    p->ref_r = 1;
    p->ref_w = 1;
    p->head  = 0;
    p->tail  = 0;
    p->count = 0;
    p->wq.head = -1;

    /* 4) 登记两端 fd（writable 复用为"是否写端"）。 */
    t->fds[rfd].type    = FD_TYPE_PIPE;
    t->fds[rfd].writable= 0;
    t->fds[rfd].dirty   = 0;
    t->fds[rfd].size    = 0;
    t->fds[rfd].offset  = 0;
    t->fds[rfd].data    = (uint8_t *)p;
    t->fds[rfd].name[0] = '\0';

    t->fds[wfd].type    = FD_TYPE_PIPE;
    t->fds[wfd].writable= 1;
    t->fds[wfd].dirty   = 0;
    t->fds[wfd].size    = 0;
    t->fds[wfd].offset  = 0;
    t->fds[wfd].data    = (uint8_t *)p;
    t->fds[wfd].name[0] = '\0';

    /* 5) 写回用户（u_fds 已由 syscall.c 经 user_range_ok(...,1) 校验可写）。 */
    u_fds[0] = rfd;
    u_fds[1] = wfd;
    return 0;
}

/* ---------- 读写 ---------- */

int fd_read(fd_table_t *t, int fd, uint8_t *buf, uint32_t n) {
    if (t == 0 || fd < 0 || (uint32_t)fd >= MAX_OPEN_FDS) return -1;
    fd_entry_t *e = &t->fds[fd];

    if (e->type == FD_TYPE_STDIN) {
        /* 步骤 8a：阻塞读键盘。缓冲空 → keyboard_block() 睡到 IRQ1
         * 塞入按键或 30s 超时（超时按 EOF 语义返回已读字节，可能为 0）。
         * 已读到部分数据时缓冲取空即返回——保持"有数据就不等满"的
         * 旧语义，readline 类调用方不会被卡在半个行缓冲上。 */
        uint32_t i = 0;
        while (i < n) {
            int c = keyboard_getchar();
            if (c == 0) {
                if (i > 0) break;
                keyboard_block();
                continue;
            }
            buf[i++] = (uint8_t)c;
        }
        return (int)i;
    }

    /* pipe 读端：读空且有写端 → 睡等数据；写端全关 → EOF(0)；
     * 有数据则读最多 min(n,count) 字节（部分读允许，不阻塞读满）。 */
    if (e->type == FD_TYPE_PIPE) {
        pipe_t *p = (pipe_t *)e->data;
        if (p == 0) return -1;
        if (e->writable) return -1;                 /* 写端不可读 */
        if (n == 0) return 0;
        for (;;) {
            asm volatile("cli" ::: "memory");       /* 条件判定与入队原子 */
            if (p->count > 0) {
                uint32_t k = (n < p->count) ? n : p->count;
                for (uint32_t i = 0; i < k; i++)
                    buf[i] = p->buf[(p->tail + i) % PIPE_BUF];
                p->tail  = (p->tail + k) % PIPE_BUF;
                p->count -= k;
                task_wake_all(&p->wq);              /* 腾出空间，唤醒写端 */
                asm volatile("sti" ::: "memory");
                return (int)k;
            }
            if (p->ref_w == 0) {                    /* 全部写端关闭 → EOF */
                asm volatile("sti" ::: "memory");
                return 0;
            }
            task_sleep(&p->wq, 0);                  /* 有写端，睡等数据 */
            /* 回到这里循环重查（task_sleep 内已平衡 cli/sti；下一轮再 cli） */
        }
    }

    if (e->type != FD_TYPE_FILE) return -1;               /* stdout/stderr 不可读 */
    if (e->offset >= e->size) return 0;              /* EOF */
    uint32_t avail = e->size - e->offset;
    uint32_t k = n < avail ? n : avail;
    for (uint32_t i = 0; i < k; i++) buf[i] = e->data[e->offset + i];
    e->offset += k;
    return (int)k;
}

int fd_write(fd_table_t *t, int fd, const uint8_t *buf, uint32_t n) {
    if (t == 0 || fd < 0 || (uint32_t)fd >= MAX_OPEN_FDS) return -1;
    fd_entry_t *e = &t->fds[fd];

    if (e->type == FD_TYPE_STDOUT || e->type == FD_TYPE_STDERR) {
        terminal_write((const char *)buf, n);
        return (int)n;
    }

    /* pipe 写端：无读者 → 返回 -1（SIGPIPE 等价，不杀进程）；缓冲满且有读者
     * → 睡等空间；有空间则写最多 min(n,空闲) 字节（部分写允许）。 */
    if (e->type == FD_TYPE_PIPE) {
        pipe_t *p = (pipe_t *)e->data;
        if (p == 0) return -1;
        if (!e->writable) return -1;                /* 读端不可写 */
        if (n == 0) return 0;
        for (;;) {
            asm volatile("cli" ::: "memory");       /* 条件判定与入队原子 */
            if (p->ref_r == 0) {                    /* 无读者 → SIGPIPE 等价 */
                asm volatile("sti" ::: "memory");
                return -1;
            }
            if (p->count < PIPE_BUF) {
                uint32_t free = PIPE_BUF - p->count;
                uint32_t k = (n < free) ? n : free;
                for (uint32_t i = 0; i < k; i++)
                    p->buf[(p->head + i) % PIPE_BUF] = buf[i];
                p->head  = (p->head + k) % PIPE_BUF;
                p->count += k;
                task_wake_all(&p->wq);              /* 有数据，唤醒读端 */
                asm volatile("sti" ::: "memory");
                return (int)k;
            }
            task_sleep(&p->wq, 0);                  /* 满且有读者，睡等空间 */
        }
    }

    if (e->type != FD_TYPE_FILE) return -1;
    if (!e->writable) return -1;
    if (n == 0) return 0;

    if (e->offset + n > e->size) {
        if (!fd_grow(e, e->offset + n)) return -1;
    }
    for (uint32_t i = 0; i < n; i++) e->data[e->offset + i] = buf[i];
    e->offset += n;
    e->dirty = 1;
    return (int)n;
}

int fd_lseek(fd_table_t *t, int fd, int32_t offset, int whence) {
    if (t == 0 || fd < 0 || (uint32_t)fd >= MAX_OPEN_FDS) return -1;
    fd_entry_t *e = &t->fds[fd];
    if (e->type != FD_TYPE_FILE) return -1;

    int32_t base;
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = (int32_t)e->offset; break;
    case SEEK_END: base = (int32_t)e->size; break;
    default: return -1;
    }
    int32_t newoff = base + offset;
    if (newoff < 0) return -1;
    if ((uint32_t)newoff > MAX_FD_FILE) return -1;
    e->offset = (uint32_t)newoff;               /* 允许越过 size（写时填洞） */
    return (int)newoff;
}

/* ---------- 自检 ---------- */

static void fd_null_puts(const char *s) { (void)s; }

int fd_selftest(void (*out)(const char *)) {
    if (!out) out = fd_null_puts;      /* NULL = 静默跑，仍返回失败数 */
    int fail = 0;
    out("fd self-test:\n");

    if (!fs_ready()) {
        out("  filesystem not mounted [FAIL]\n");
        return 1;
    }

    fd_table_t t;
    fd_table_init(&t);

    /* 清理可能残留的测试文件（上次自检中断/用户程序写过） */
    fs_delete_file("FDTEST.TXT");

    /* 1) 打开 README.TXT，读，验证内容 */
    int fd = fd_open(&t, "README.TXT", O_RDONLY);
    if (fd < 0) {
        out("  open README.TXT [FAIL]\n");
        return 1;
    }
    uint8_t buf[64];
    int n = fd_read(&t, fd, buf, sizeof(buf));
    int ok1 = (n == 46) && buf[0] == 'W' && buf[7] == ' ';   /* "Welcome " 第 8 字节 */
    out("  open+read README.TXT (46B): ");
    out(ok1 ? "yes [OK]\n" : "NO [FAIL]\n");
    if (!ok1) fail++;

    /* 2) lseek SEEK_SET 0 回绕重读，首字节一致 */
    if (fd_lseek(&t, fd, 0, SEEK_SET) != 0) fail++;
    uint8_t b2[8];
    int n2 = fd_read(&t, fd, b2, 8);
    int ok2 = (n2 == 8) && b2[0] == 'W' && b2[7] == ' ';
    out("  lseek SET 0 + reread 8B: ");
    out(ok2 ? "yes [OK]\n" : "NO [FAIL]\n");
    if (!ok2) fail++;

    /* 3) lseek SEEK_END 返回文件大小 */
    int pos = fd_lseek(&t, fd, 0, SEEK_END);
    int ok3 = (pos == 46);
    out("  lseek END == size (46): ");
    out(ok3 ? "yes [OK]\n" : "NO [FAIL]\n");
    if (!ok3) fail++;
    fd_close(&t, fd);

    /* 4) 写模式建文件，write 内容，close 落盘 */
    fd = fd_open(&t, "FDTEST.TXT", O_WRONLY);
    int ok4 = (fd >= 0);
    out("  open FDTEST.TXT (write/create): ");
    out(ok4 ? "yes [OK]\n" : "NO [FAIL]\n");
    if (!ok4) { fail++; }
    else {
        const char *msg = "fd write test\n";
        uint32_t ml = fd_strlen(msg);
        if (fd_write(&t, fd, (const uint8_t *)msg, ml) != (int)ml) { ok4 = 0; fail++; }
        fd_close(&t, fd);

        /* 5) 重新只读打开，读回验证一致 */
        fd = fd_open(&t, "FDTEST.TXT", O_RDONLY);
        uint8_t rb[32];
        int rn = fd_read(&t, fd, rb, sizeof(rb));
        int ok5 = (fd >= 0) && (rn == (int)ml) && rb[0] == 'f' && rb[ml - 1] == '\n';
        out("  read back written file: ");
        out(ok5 ? "yes [OK]\n" : "NO [FAIL]\n");
        if (!ok5) fail++;
        if (fd >= 0) fd_close(&t, fd);
        fs_delete_file("FDTEST.TXT");
    }

    /* 6) 只读句柄拒绝写 */
    fd = fd_open(&t, "README.TXT", O_RDONLY);
    int ok6 = (fd >= 0) && (fd_write(&t, fd, (const uint8_t *)"x", 1) == -1);
    out("  read-only fd rejects write: ");
    out(ok6 ? "yes [OK]\n" : "NO [FAIL]\n");
    if (!ok6) fail++;
    if (fd >= 0) fd_close(&t, fd);

    /* ===== 7..10) pipe =====
     * 注意：这些用例必须保证不会真的睡下去——写端关掉后读才返回 0(EOF)，
     * 有数据可读时读不会阻塞。一旦顺序写错，自检会把整机挂在 task_sleep 上，
     * 而 selftest 是在 shell 之前的开机自检里跑的，卡住就再也进不了 shell。 */
    int pfd[2];
    int ok7 = (fd_pipe(&t, pfd) == 0);
    out("  pipe() creates r/w pair: ");
    out(ok7 ? "yes [OK]\n" : "NO [FAIL]\n");
    if (!ok7) {
        fail++;
    } else {
        const char *msg = "pipe payload";
        int ml = (int)fd_strlen(msg);

        /* 8) 写 -> 读回：内容一致，且读端不可写 / 写端不可读 */
        int w = fd_write(&t, pfd[1], (const uint8_t *)msg, (uint32_t)ml);
        uint8_t pb[32];
        int r = fd_read(&t, pfd[0], pb, sizeof(pb));
        int ok8 = (w == ml) && (r == ml) && (pb[0] == 'p') && (pb[ml - 1] == 'd')
                  && (fd_write(&t, pfd[0], (const uint8_t *)"x", 1) == -1)
                  && (fd_read(&t, pfd[1], pb, 1) == -1);
        out("  pipe write->read roundtrip: ");
        out(ok8 ? "yes [OK]\n" : "NO [FAIL]\n");
        if (!ok8) fail++;

        /* 9) 写端关掉后，读端必须读到 EOF(0) 而不是一直睡 */
        fd_close(&t, pfd[1]);
        int r2 = fd_read(&t, pfd[0], pb, sizeof(pb));
        int ok9 = (r2 == 0);
        out("  pipe read returns EOF(0) after write end closed: ");
        out(ok9 ? "yes [OK]\n" : "NO [FAIL]\n");
        if (!ok9) {
            fail++;
            /* 读到 0 之外的结果说明状态机坏了：直接把读端也关掉止损，
             * 别让后面的用例继续引用这个 pipe。 */
        }
        fd_close(&t, pfd[0]);

        /* 10) 读端关掉后，写必须失败（SIGPIPE 的等价语义：返回 -1，不杀进程） */
        int ok10 = 0;
        if (fd_pipe(&t, pfd) == 0) {
            fd_close(&t, pfd[0]);
            ok10 = (fd_write(&t, pfd[1], (const uint8_t *)"x", 1) == -1);
            fd_close(&t, pfd[1]);
        }
        out("  pipe write fails after read end closed: ");
        out(ok10 ? "yes [OK]\n" : "NO [FAIL]\n");
        if (!ok10) fail++;
    }

    out(fail == 0 ? "  result: PASS\n" : "  result: FAIL\n");
    return fail;
}
