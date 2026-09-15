/*
 * user/hello.c - 第一个用户态 ELF 程序（步骤 5c）
 *
 * 与内核的边界：
 *   - 没有 libc，唯一的内核入口是 int 0x80（crt0.asm 里包装成 write/read/_exit）
 *   - 运行在 ring3：任何越权访问（内核地址、只读段写入）都会 #PF 并被
 *     panic 捕获，不会污染内核
 *   - 链接到 0x00400000（USER_IMAGE_BASE），由内核 exec 从磁盘加载
 */
typedef unsigned int u32;

int write(int fd, const void *buf, u32 n);
int read(int fd, void *buf, u32 n);
void _exit(int code) __attribute__((noreturn));

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
    char out[13];
    int m = 0;
    while (n) out[m++] = b[--n];
    out[m] = 0;
    kputs(out);
}

int umain(int argc, char **argv) {
    kputs("hello from ELF user program!\n");
    kputs("argc = ");
    kputdec((u32)argc);
    kputs("\n");
    for (int i = 0; i < argc; i++) {
        kputs("argv[");
        kputdec((u32)i);
        kputs("] = ");
        kputs(argv[i] ? argv[i] : "(null)");
        kputs("\n");
    }
    /* 越权写自检：写只读的 .text 段必须触发 #PF（有意为之则本行应注释掉） */
    kputs("argv terminator ok: ");
    kputs(argv[argc] == 0 ? "yes\n" : "NO\n");
    return 42;
}
