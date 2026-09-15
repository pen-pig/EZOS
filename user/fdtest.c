/*
 * user/fdtest.c - 文件描述符端到端测试程序（步骤 6b）
 *
 * 在 ring3 里走完整的 fd 语义：open/read/lseek/close，以及写模式建文件、
 * 落盘、读回验证，最后验证只读句柄拒绝写。所有结果打到 stdout。
 *
 * 与内核的边界：没有 libc，唯一内核入口是 int 0x80（crt0.asm 里包装成
 * write/read/open/close/lseek/_exit）。open 的文件名与缓冲都是用户指针，
 * 内核会逐一做 user_range_ok 校验——越界即返回 -1，不会碰内核内存。
 */
typedef unsigned int u32;
typedef int s32;

int open(const char *name, int flags);
int close(int fd);
int read(int fd, void *buf, u32 n);
int write(int fd, const void *buf, u32 n);
int lseek(int fd, int off, int whence);
void _exit(int code) __attribute__((noreturn));

#define O_RDONLY 0
#define O_WRONLY 1
#define SEEK_SET 0
#define SEEK_END 2

static u32 kstrlen(const char *s) {
    u32 n = 0;
    while (s[n]) n++;
    return n;
}

static void kputs(const char *s) { write(1, s, kstrlen(s)); }

static void kputdec(u32 v) {
    char b[12];
    int n = 0;
    if (v == 0) b[n++] = '0';
    while (v) { b[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (n) { write(1, &b[--n], 1); }
}

int umain(int argc, char **argv) {
    (void)argc; (void)argv;
    kputs("fd test: open/read/lseek/write/close\n");

    /* 1) 只读打开 README.TXT */
    int fd = open("README.TXT", O_RDONLY);
    if (fd < 0) { kputs("FAIL: open README.TXT\n"); return 1; }
    kputs("open README.TXT -> fd "); kputdec((u32)fd); kputs("\n");

    /* 2) 读出全部并回显 */
    char buf[128];
    int n = read(fd, buf, sizeof(buf));
    kputs("read -> "); kputdec((u32)n); kputs(" bytes: ");
    if (n > 0) write(1, buf, (u32)n);
    kputs("\n");

    /* 3) lseek SEEK_END 取大小，再回绕读前 8 字节 */
    int sz = lseek(fd, 0, SEEK_END);
    kputs("lseek END -> "); kputdec((u32)sz); kputs("\n");
    lseek(fd, 0, SEEK_SET);
    char b8[8];
    int n8 = read(fd, b8, 8);
    kputs("first 8 bytes: ");
    if (n8 > 0) write(1, b8, (u32)n8);
    kputs("\n");
    close(fd);

    /* 4) 写模式建文件 + 写内容 + close 落盘 */
    fd = open("FDTEST.TXT", O_WRONLY);
    if (fd < 0) { kputs("FAIL: open FDTEST.TXT (write)\n"); return 1; }
    const char *msg = "written from ring3 fdtest\n";
    int w = write(fd, msg, kstrlen(msg));
    kputs("write -> "); kputdec((u32)w); kputs(" bytes, closed\n");
    close(fd);

    /* 5) 重新只读打开，读回验证落盘结果 */
    fd = open("FDTEST.TXT", O_RDONLY);
    char rb[64];
    int rn = read(fd, rb, sizeof(rb));
    kputs("read back "); kputdec((u32)rn); kputs(" bytes: ");
    if (rn > 0) write(1, rb, (u32)rn);
    kputs("\n");
    close(fd);

    /* 6) 只读句柄写应被拒绝（返回 -1，映射为 0 打印） */
    fd = open("README.TXT", O_RDONLY);
    int wrc = write(fd, "x", 1);
    kputs("write to read-only fd rejected: ");
    kputs(wrc == -1 ? "yes\n" : "NO\n");
    close(fd);

    kputs("fd test done\n");
    return 0;
}
