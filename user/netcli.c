/*
 * user/netcli.c - TCP 主动连接客户端（步骤 7.4 验证程序）
 *
 * 流程：socket(TCP) → connect(10.0.2.2:7002) → 发 "REQ" → 收数据 →
 *       把收到的内容原样发回（对端据此判断乱序重组后交给应用的顺序是否
 *       正确）→ close。
 *
 * E2E（temp/test_step7_tcp.py）：Python 对端经 -netdev socket 直接收发
 * 以太帧（绕过 slirp，可任意编排丢包/乱序）：
 *   - 主动打开：断言收到 guest SYN，握手完成
 *   - 乱序重组：对端先发后一段（seq 靠后）再发前一段；guest 必须缓冲先到
 *     的乱序段，缺口补上后按序交给应用 —— 应用回显必须是 "HELLWORLD"
 *   - 重传：对端故意不 ACK guest 的回显段，断言同一 seq 的段出现 ≥2 次
 */
typedef unsigned int u32;

int write(int fd, const void *buf, u32 n);
void _exit(int code) __attribute__((noreturn));
int sockcall(u32 subcmd, void *args);

/* 与 kernel/net.h 的 SC_* / SOCK_TYPE_* 对齐（用户态不引内核头） */
#define SOCK_TYPE_TCP 1u
#define SC_SOCKET    0u
#define SC_CONNECT   7u
#define SC_SENDTO    4u
#define SC_RECVFROM  5u
#define SC_CLOSE     6u

/* 10.0.2.2 的大端打包（sockcall 的 dst_ip 约定为网络序 u32） */
#define PEER_IP_BE   ((10u << 24) | (0u << 16) | (2u << 8) | 2u)
#define PEER_PORT    7002u

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
    static char buf[256];

    a[0] = SOCK_TYPE_TCP;
    int s = sockcall(SC_SOCKET, a);
    if (s < 0) { kputs("netcli: socket failed\n"); _exit(1); }

    a[0] = (u32)s;
    a[1] = PEER_IP_BE;
    a[2] = PEER_PORT;
    a[3] = 15000;                  /* 握手超时 ms */
    if (sockcall(SC_CONNECT, a) != 0) { kputs("netcli: connect failed\n"); _exit(1); }
    kputs("netcli: connected\n");

    a[0] = (u32)s;
    a[1] = (u32)"REQ";
    a[2] = 3;
    a[3] = 0;
    a[4] = 0;
    if (sockcall(SC_SENDTO, a) < 0) { kputs("netcli: send failed\n"); _exit(1); }

    a[0] = (u32)s;
    a[1] = (u32)buf;
    a[2] = sizeof(buf);
    a[3] = 15000;                  /* recv 超时 ms */
    a[4] = 0;
    int n = sockcall(SC_RECVFROM, a);
    if (n <= 0) { kputs("netcli: recv failed\n"); _exit(1); }

    kputs("netcli: got ");
    kputdec((u32)n);
    kputs(" bytes\n");
    write(1, buf, (u32)n);
    write(1, "\n", 1);

    /* 回显：对端用它判断乱序重组的交付顺序 */
    a[0] = (u32)s;
    a[1] = (u32)buf;
    a[2] = (u32)n;
    a[3] = 0;
    a[4] = 0;
    if (sockcall(SC_SENDTO, a) < 0) { kputs("netcli: echo send failed\n"); _exit(1); }
    kputs("netcli: echoed\n");

    a[0] = (u32)s;
    sockcall(SC_CLOSE, a);
    kputs("netcli: done\n");
    _exit(0);
}
