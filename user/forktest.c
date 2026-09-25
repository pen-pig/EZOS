/*
 * user/forktest.c - fork / waitpid 的用户态验证程序（用户态生态 ②）
 *
 * 为什么需要它：fork 的正确性没法在内核里自检——fork 要求父进程是
 * 真正的用户进程（is_user + 独立页目录），而 utest 那种内建路径跑在
 * task 0 上，没有独立地址空间，fork 只会返回 -1。所以必须从盘上 exec
 * 一个真 ELF 来验证。
 *
 * 验证点：
 *   1) 父进程拿到子 pid（>0），子进程拿到 0
 *   2) 子进程有自己独立的地址空间（改自己的全局变量，父进程看不到）
 *   3) waitpid 能阻塞等到子进程并取回退出码
 */
typedef unsigned int u32;

int write(int fd, const void *buf, u32 n);
void _exit(int code) __attribute__((noreturn));

#define SYS_FORK     43
#define SYS_WAITPID  44

static int ksyscall1(int num, int a1) {
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(num), "b"(a1) : "memory");
    return r;
}

static int kwaitpid(int pid, int *status) {
    int r;
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(SYS_WAITPID), "b"(pid), "c"(status), "d"(0)
                     : "memory");
    return r;
}

static u32 kstrlen(const char *s) {
    u32 n = 0;
    while (s[n]) n++;
    return n;
}

static void kputs(const char *s) { write(1, s, kstrlen(s)); }

static void kputdec(int v) {
    char b[12];
    int n = 0;
    unsigned u = (v < 0) ? (unsigned)(-v) : (unsigned)v;
    if (v < 0) kputs("-");
    if (u == 0) b[n++] = '0';
    while (u) { b[n++] = (char)('0' + (u % 10u)); u /= 10u; }
    while (n) { char c = b[--n]; write(1, &c, 1); }
}

/* 放在 .data：子进程写它，父进程读它。两边值不同才说明地址空间真的独立 */
static int g_shared_probe = 100;

int umain(int argc, char **argv) {
    (void)argc; (void)argv;
    kputs("forktest start\n");

    int pid = ksyscall1(SYS_FORK, 0);
    /* 两个进程都会走到这里：打印各自拿到的返回值，用来判定
     * "父得子 pid、子得 0" 这条契约是否成立。
     * 用互不相似的大写词——OCR 容易把相近的行读串。 */
    kputs("TAG RET ");
    kputdec(pid);
    kputs("\n");
    if (pid < 0) {
        kputs("TAG FORKFAILED\n");
        _exit(1);
    }
    if (pid == 0) {
        g_shared_probe = 555;
        kputs("TAG CHILD\n");
        _exit(7);
    }

    kputs("TAG PARENT PID ");
    kputdec(pid);
    kputs("\n");

    int status = -1;
    int rc = kwaitpid(pid, &status);
    kputs("TAG WAIT RC ");
    kputdec(rc);
    kputs(" ST ");
    kputdec(status);
    kputs("\n");

    /* 地址空间独立性的判据：父进程这里必须还是 100，不能被子进程改成 555 */
    kputs("forktest probe=");
    kputdec(g_shared_probe);
    kputs("\n");

    if (rc == pid && status == 7 && g_shared_probe == 100) {
        kputs("forktest ok\n");
        _exit(0);
    }
    kputs("forktest bad\n");
    _exit(2);
}
