/*
 * user/segprobe.c - sockcall 返回路径段寄存器破坏最小复现（7.3 调试用）
 *
 * 每个 sockcall 后 kputs：打印中断说明该系统调用的 push/pop 往返干净。
 * recvfrom 用 3s 超时且无人发包——只测"等待+超时"路径本身。
 */
typedef unsigned int u32;

int write(int fd, const void *buf, u32 n);
void _exit(int code) __attribute__((noreturn));
int sockcall(u32 subcmd, void *args);

#define SOCK_TYPE_UDP 0u
#define SC_SOCKET    0u
#define SC_BIND      1u
#define SC_RECVFROM  5u
#define SC_CLOSE     6u

static u32 kstrlen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }
static void kputs(const char *s) { write(1, s, kstrlen(s)); }

int umain(int argc, char **argv) {
    (void)argc; (void)argv;
    u32 a[5];
    static char buf[64];

    kputs("segprobe: start\n");

    a[0] = SOCK_TYPE_UDP;
    int s = sockcall(SC_SOCKET, a);
    kputs("segprobe: socket done\n");

    a[0] = (u32)s;
    a[1] = 7002;
    sockcall(SC_BIND, a);
    kputs("segprobe: bind done\n");

    a[0] = (u32)s;
    a[1] = (u32)buf;
    a[2] = sizeof(buf);
    a[3] = 3000;
    a[4] = 0;
    int n = sockcall(SC_RECVFROM, a);
    kputs("segprobe: recvfrom done\n");
    if (n < 0) kputs("segprobe: (timeout as expected)\n");

    a[0] = (u32)s;
    sockcall(SC_CLOSE, a);
    kputs("segprobe: close done\n");
    _exit(0);
}
