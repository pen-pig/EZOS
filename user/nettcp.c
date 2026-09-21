/*
 * user/nettcp.c - TCP echo 服务器用户态测试程序（步骤 7.3）
 *
 * 流程：socket(TCP) → bind(7001) → listen → accept(15s 超时) →
 *       recv(15s) → 载荷回显 → close（主动 FIN）→ close 监听 → 退出。
 *
 * E2E（temp/test_step7_sock.py）：Python 对端做完整三次握手 + 数据
 * 往返 + 四次挥手，断言 seq/ack 与校验和。
 */
typedef unsigned int u32;

int write(int fd, const void *buf, u32 n);
void _exit(int code) __attribute__((noreturn));
int sockcall(u32 subcmd, void *args);

/* 与 kernel/net.h 的 SC_* / SOCK_TYPE_* 对齐（用户态不引内核头） */
#define SOCK_TYPE_TCP 1u
#define SC_SOCKET    0u
#define SC_BIND      1u
#define SC_LISTEN    2u
#define SC_ACCEPT    3u
#define SC_SENDTO    4u
#define SC_RECVFROM  5u
#define SC_CLOSE     6u

static u32 kstrlen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }
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
    (void)argc; (void)argv;
    u32 a[5];
    static char buf[512];

    a[0] = SOCK_TYPE_TCP;
    int s = sockcall(SC_SOCKET, a);
    if (s < 0) { kputs("nettcp: socket failed\n"); _exit(1); }

    a[0] = (u32)s;
    a[1] = 7001;
    if (sockcall(SC_BIND, a) != 0) { kputs("nettcp: bind failed\n"); _exit(1); }

    a[0] = (u32)s;
    if (sockcall(SC_LISTEN, a) != 0) { kputs("nettcp: listen failed\n"); _exit(1); }

    kputs("nettcp: TCP echo listening on port 7001\n");

    a[0] = (u32)s;
    a[1] = 15000;                  /* accept 超时 ms */
    int c = sockcall(SC_ACCEPT, a);
    if (c < 0) { kputs("nettcp: accept timeout\n"); _exit(1); }
    kputs("nettcp: connected\n");

    a[0] = (u32)c;
    a[1] = (u32)buf;
    a[2] = sizeof(buf);
    a[3] = 15000;                  /* recv 超时 ms */
    a[4] = 0;                      /* TCP 无来源概念 */
    int n = sockcall(SC_RECVFROM, a);
    if (n <= 0) { kputs("nettcp: recv failed\n"); _exit(1); }

    kputs("nettcp: got ");
    kputdec((u32)n);
    kputs(" bytes\n");

    a[0] = (u32)c;
    a[1] = (u32)buf;
    a[2] = (u32)n;
    a[3] = 0;                      /* TCP 忽略目的地址 */
    a[4] = 0;
    if (sockcall(SC_SENDTO, a) < 0) { kputs("nettcp: send failed\n"); _exit(1); }

    a[0] = (u32)c;
    sockcall(SC_CLOSE, a);         /* 主动 FIN（FIN_WAIT_1） */
    a[0] = (u32)s;
    sockcall(SC_CLOSE, a);         /* 关监听 */
    kputs("nettcp: echoed, done\n");
    _exit(0);
}
