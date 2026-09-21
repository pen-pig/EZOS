/*
 * user/netecho.c - UDP echo 用户态测试程序（步骤 7.3）
 *
 * 流程：socket(UDP) → bind(7000) → 阻塞等一个数据报（15s 超时）→
 *       载荷原样发回发送方 → close → 退出。
 *
 * E2E（temp/test_step7_sock.py）：Python 对端经 -netdev socket 向
 * 10.0.2.15:7000 发 UDP，断言收到回显（含 IP/UDP 校验和验证）。
 */
typedef unsigned int u32;

int write(int fd, const void *buf, u32 n);
void _exit(int code) __attribute__((noreturn));
int sockcall(u32 subcmd, void *args);

/* 与 kernel/net.h 的 SC_* / SOCK_TYPE_* 对齐（用户态不引内核头） */
#define SOCK_TYPE_UDP 0u
#define SC_SOCKET    0u
#define SC_BIND      1u
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

static void kputip(u32 be) {
    kputdec((be >> 24) & 0xFF); kputs(".");
    kputdec((be >> 16) & 0xFF); kputs(".");
    kputdec((be >> 8) & 0xFF); kputs(".");
    kputdec(be & 0xFF);
}

int umain(int argc, char **argv) {
    (void)argc; (void)argv;
    u32 a[5];
    static char buf[512];
    static u32 src[2];

    a[0] = SOCK_TYPE_UDP;
    int s = sockcall(SC_SOCKET, a);
    if (s < 0) { kputs("netecho: socket failed\n"); _exit(1); }

    a[0] = (u32)s;
    a[1] = 7000;
    if (sockcall(SC_BIND, a) != 0) { kputs("netecho: bind failed\n"); _exit(1); }

    kputs("netecho: UDP echo listening on port 7000\n");

    a[0] = (u32)s;
    a[1] = (u32)buf;
    a[2] = sizeof(buf);
    a[3] = 15000;                 /* 超时 ms（0 = 缺省 10s） */
    a[4] = (u32)src;              /* 回填来源 ip/port，可为 0 */
    int n = sockcall(SC_RECVFROM, a);
    if (n < 0) { kputs("netecho: recv timeout\n"); _exit(1); }

    kputs("netecho: got ");
    kputdec((u32)n);
    kputs(" bytes from ");
    kputip(src[0]);
    kputs(":");
    kputdec(src[1]);
    kputs("\n");

    a[0] = (u32)s;
    a[1] = (u32)buf;
    a[2] = (u32)n;
    a[3] = src[0];                /* 大端打包的 IP */
    a[4] = src[1];                /* 端口（主机序） */
    if (sockcall(SC_SENDTO, a) < 0) { kputs("netecho: send failed\n"); _exit(1); }

    a[0] = (u32)s;
    sockcall(SC_CLOSE, a);
    kputs("netecho: echoed, done\n");
    _exit(0);
}
