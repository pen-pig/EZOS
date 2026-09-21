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

    out(fail == 0 ? "  result: PASS\n" : "  result: FAIL\n");
    return fail;
}
