/*
 * net.c - IPv4/UDP/TCP 协议栈 + socket 层（步骤 7.3）
 *
 * 分层：
 *   net_input(以太帧) ─┬─ ARP   → 应答 request / 学习 sender 进缓存
 *                      ├─ ICMP  → echo reply（7.2 从 rtl8139.c 迁入）
 *                      ├─ UDP   → 按目的端口投递到 socket 接收环
 *                      └─ TCP   → 极简服务端状态机（见下）
 *   发送路径：net_udp_send / tcp_send_seg → ipv4_send →（查 ARP 缓存，
 *   缺则发 request 并轮询等待）→ rtl8139_send
 *
 * TCP 范围（教学取舍，明示不做）：
 *   - 只做被动打开（listen/accept），无 connect() 主动连接
 *   - 无重传定时器（对端可靠时够用；QEMU socket netdev 是本机回环）
 *   - 无乱序重组：seq != rcv_nxt 的数据整段丢弃并回 dup-ACK
 *   - 固定通告窗口 4096，不跟踪对端窗口（回显数据 <= 单段）
 *   - 无 TIME_WAIT：LAST_ACK 收最终 ACK / FIN_WAIT_2 收 FIN 即回收
 *
 * 并发模型：本模块代码只运行在两类上下文——IRQ11（net_input）与
 * 系统调用（net_sockcall，IF=0）。二者互不可重入（单核 + 中断门清 IF），
 * 故 socket 表/ARP 缓存/环形缓冲无需加锁。
 *
 * 阻塞模型（步骤 8a 起为真睡眠）：系统调用门 0xEE 进入后 IF=0，
 * IRQ11 不会再触发——但等待不再忙轮询：recvfrom/accept/ARP 解析睡在
 * g_netrx_wq 上（task_sleep 全程 cli，丢失唤醒被结构性排除），由
 * rtl8139 rx_drain 的 net_rx_wake()（IRQ11 或轮询路径）与 IRQ0 的
 * task_timer_tick（超时）唤醒。醒来先 net_poll() 排一次环再查条件，
 * 覆盖"包在睡眠前已到"与"唤醒源是无关中断"两种情形。
 *
 * 校验和注意（真 bug 修复）：反码和不满足 ~a+~b == ~(a+b)——伪首部与
 * 段必须分别求 RAW 和再相加、最后统一取反一次（cksum_raw），对两段
 * 各自取反相加会算出错误校验和，slirp 等真协议栈直接丢包。
 */
#include "net.h"
#include "rtl8139.h"
#include "kmalloc.h"
#include "syscall.h"
#include "task.h"
#include "isr.h"        /* g_pit_ticks：睡眠超时的 tick 源（步骤 8a） */
#include "dmesg.h"

#define ETH_HDR_LEN   14u
#define IP_HDR_MIN    20u
#define UDP_HDR_LEN   8u
#define TCP_HDR_MIN   20u
#define NET_BUILD_MAX 1792u          /* 协议构造缓冲（最大以太帧 + 余量） */
#define NET_DEFAULT_WAIT_MS 10000u   /* timeout 参数为 0 时的缺省等待 */

/* TCP 标志位 */
#define TCP_FIN 0x01u
#define TCP_SYN 0x02u
#define TCP_RST 0x04u
#define TCP_PSH 0x08u
#define TCP_ACK 0x10u

/* socket 表 */
#define NET_MAX_SOCKS 8u
#define NET_SOCK_BUF  4096u          /* 每 socket 接收环 */

/* TCP 状态 */
enum {
    TS_UNUSED = 0,
    TS_LISTEN,
    TS_SYN_RCVD,
    TS_ESTABLISHED,
    TS_CLOSE_WAIT,      /* 对端已 FIN */
    TS_LAST_ACK,        /* 我方 FIN 已发，等最终 ACK */
    TS_FIN_WAIT_1,      /* 我方先关，等对端 ACK */
    TS_FIN_WAIT_2       /* 我方 FIN 已被 ACK，等对端 FIN */
};

typedef struct {
    uint8_t  used;
    uint8_t  type;                  /* SOCK_TYPE_* */
    uint8_t  state;                 /* TS_*（TCP） */
    uint8_t  got_fin;               /* TCP：对端 FIN 已收（recv 0 语义） */
    uint16_t local_port;            /* 主机序（数值）；线上转换只在 put16/be16 边界做 */
    uint16_t peer_port;             /* 主机序（TCP conn） */
    uint32_t peer_ip;               /* 大端打包（TCP conn） */
    uint32_t owner_pid;             /* 懒回收：0 = 内核 */
    int      parent;                /* TCP：所属监听 socket 下标，-1 = 独立 */
    uint8_t *rx;                    /* 接收环（kmalloc，槽位复用） */
    uint32_t rx_r, rx_w;            /* 环读写指针（字节，模 NET_SOCK_BUF） */
    uint32_t snd_nxt, rcv_nxt;      /* TCP 序号 */
} sock_t;

/* socket 表与 ARP 缓存放 .bss.hi（1-2MB 高内存段）：低 .bss 预算紧
 * （linker.ld 断言 __bss_end <= 0x90000，栈区之前只剩几百字节），
 * 高段 identity 映射且所有进程 PD 共享，IRQ/系统调用上下文访问无碍。 */
#define NET_HIBUF __attribute__((section(".bss.hi")))

static sock_t g_socks[NET_MAX_SOCKS] NET_HIBUF;
static uint8_t *g_build;            /* 协议载荷构造缓冲（懒分配） */
static uint8_t *g_frame;            /* 最终帧组装缓冲（与 g_build 分离：
                                      * ipv4_send 的 eth+ip 头写在 g_frame，
                                      * payload 从 g_build 拷入，避免同缓冲
                                      * 前后段重叠拷贝互相踩） */

/* 统计 */
static uint32_t g_udp_rx, g_udp_tx, g_tcp_rx, g_tcp_tx;
static uint32_t g_arp_replied, g_icmp_replied;

/* IP 标识计数器 */
static uint16_t g_ip_id = 0x1000;

/* ---------- 小工具 ---------- */

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}
static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v;
}
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static void wr16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

/* Internet 校验和（RFC 1071）：16 位反码和。cksum_raw 返回不取反的和
 * （多段求和时逐段累加，最后统一取反一次——对两段分别取反再相加是
 * 错的：反码和不满足 ~a+~b == ~(a+b)，slirp 等真协议栈会丢弃坏包）。 */
static uint32_t cksum_raw(const uint8_t *p, uint32_t n) {
    uint32_t s = 0;
    while (n >= 2) { s += ((uint32_t)p[0] << 8) | p[1]; p += 2; n -= 2; }
    if (n) s += (uint32_t)p[0] << 8;
    while (s >> 16) s = (s & 0xFFFFu) + (s >> 16);
    return s;
}
static uint16_t cksum(const uint8_t *p, uint32_t n) {
    return (uint16_t)(~cksum_raw(p, n));
}

static uint32_t my_ip_be(void) {
    return be32(rtl8139_ip());
}

/* net 等待队列（步骤 8a）：recvfrom/accept/ARP 解析共用一条——
 * 唤醒源都是"RX 环进了新包"（rtl8139 rx_drain → net_rx_wake）。
 * 惊群（多个等待者同醒）由各自的条件复查消化，任务数 ≤ MAX_TASKS。
 * {-1}：head 哨兵（0 是任务下标）。 */
static wait_queue_t g_netrx_wq = {-1};

void net_rx_wake(void) {
    task_wake_all(&g_netrx_wq);
}

/* 构造缓冲懒分配（net_input 可能在 IRQ 上下文首次进入；kmalloc 在
 * IF=0 下调用是安全的——见文件头并发模型） */
static int build_ready(void) {
    if (g_build && g_frame) return 1;
    if (!g_build)   g_build = kmalloc(NET_BUILD_MAX + 16);
    if (!g_frame)   g_frame = kmalloc(NET_BUILD_MAX + 16);
    if (!g_build || !g_frame) {
        dmesg_write("NET: kmalloc failed for build buffers");
        return 0;
    }
    return 1;
}

/* ---------- ARP 缓存 ---------- */

#define ARP_CACHE 4
static struct {
    uint32_t ip;                    /* 大端打包；0 = 空槽 */
    uint8_t  mac[6];
} g_arp[ARP_CACHE] NET_HIBUF;

static void arp_learn(uint32_t ip, const uint8_t *mac) {
    if (ip == 0) return;
    for (int i = 0; i < ARP_CACHE; i++) {
        if (g_arp[i].ip == ip) {
            for (int k = 0; k < 6; k++) g_arp[i].mac[k] = mac[k];
            return;
        }
    }
    int slot = 0;
    for (int i = 1; i < ARP_CACHE; i++)
        if (g_arp[i].ip == 0) { slot = i; break; }
    g_arp[slot].ip = ip;
    for (int k = 0; k < 6; k++) g_arp[slot].mac[k] = mac[k];
}

static const uint8_t *arp_lookup(uint32_t ip) {
    for (int i = 0; i < ARP_CACHE; i++)
        if (g_arp[i].ip == ip) return g_arp[i].mac;
    return 0;
}

static void arp_request(uint32_t ip) {
    uint8_t *r = g_build;
    for (int i = 0; i < 6; i++) r[i] = 0xFF;
    const uint8_t *my = rtl8139_mac();
    for (int i = 0; i < 6; i++) r[6 + i] = my[i];
    r[12] = 0x08; r[13] = 0x06;
    uint8_t *p = r + ETH_HDR_LEN;
    put16(p + 0, 1); put16(p + 2, 0x0800); p[4] = 6; p[5] = 4;
    put16(p + 6, 1);                              /* request */
    for (int i = 0; i < 6; i++) p[8 + i] = my[i];
    put32(p + 14, my_ip_be());
    for (int i = 0; i < 6; i++) p[18 + i] = 0;
    put32(p + 24, ip);
    rtl8139_send(r, ETH_HDR_LEN + 28);
}

static const uint8_t *arp_resolve(uint32_t ip, uint32_t timeout_ms) {
    const uint8_t *m = arp_lookup(ip);
    if (m) return m;
    arp_request(ip);
    /* 步骤 8a：睡等 ARP reply（rx_drain → net_rx_wake 唤醒），
     * deadline 用 PIT tick（回绕安全的有符号比较）。 */
    uint32_t deadline = g_pit_ticks + timeout_ms;
    for (;;) {
        net_poll();                                /* 先排环：reply 可能已到 */
        m = arp_lookup(ip);
        if (m) return m;
        uint32_t now = g_pit_ticks;
        if ((int32_t)(now - deadline) >= 0) return 0;
        task_sleep(&g_netrx_wq, deadline - now);
    }
}

/* ---------- IPv4 发送 ---------- */

/* 构造并发送一个 IPv4 包（payload 为完整传输层报文，通常在 g_build）。
 * 顺序（防别名/防重入）：先把 payload 拷进 g_frame 尾部，再做可能阻塞
 * 轮询的 ARP 解析（期间 net_input 重入只会动 g_build，不碰 g_frame），
 * 最后写 eth+ip 头并发送。
 * 返回 0 = 已提交网卡；-1 = ARP 解析失败/参数非法。 */
static int ipv4_send(uint32_t dst_ip, uint8_t proto,
                     const uint8_t *payload, uint32_t len) {
    if (len == 0 || ETH_HDR_LEN + IP_HDR_MIN + len > NET_BUILD_MAX) return -1;

    uint8_t *ip = g_frame + ETH_HDR_LEN;
    for (uint32_t i = 0; i < len; i++) ip[IP_HDR_MIN + i] = payload[i];

    const uint8_t *dst_mac = arp_resolve(dst_ip, 3000);
    if (!dst_mac) return -1;

    uint8_t *r = g_frame;
    for (int i = 0; i < 6; i++) r[i] = dst_mac[i];
    const uint8_t *my = rtl8139_mac();
    for (int i = 0; i < 6; i++) r[6 + i] = my[i];
    r[12] = 0x08; r[13] = 0x00;

    ip[0] = 0x45; ip[1] = 0;
    put16(ip + 2, (uint16_t)(IP_HDR_MIN + len));
    put16(ip + 4, ++g_ip_id);
    put16(ip + 6, 0);                              /* 无分片 */
    ip[8] = 64; ip[9] = proto;
    put16(ip + 10, 0);
    put32(ip + 12, my_ip_be());
    put32(ip + 16, dst_ip);
    put16(ip + 10, cksum(ip, IP_HDR_MIN));

    return rtl8139_send(r, ETH_HDR_LEN + IP_HDR_MIN + len);
}

/* ---------- ICMP（echo reply） ---------- */

static void icmp_input(const uint8_t *ip, uint32_t ihl) {
    const uint8_t *ic = ip + ihl;
    if (ic[0] != 8 || ic[1] != 0) return;          /* 只回 echo request */

    uint32_t totlen = be16(ip + 2);
    uint32_t iclen = totlen - ihl;
    if (iclen < 8 || iclen > NET_BUILD_MAX) return;

    uint8_t *p = g_build;
    p[0] = 0;                                      /* echo reply（type 0） */
    p[1] = 0;
    put16(p + 2, 0);
    for (uint32_t i = 4; i < iclen; i++) p[i] = ic[i];
    put16(p + 2, cksum(p, iclen));
    if (ipv4_send(be32(ip + 12), 1, p, iclen) == 0) g_icmp_replied++;
}

/* ---------- UDP ---------- */

/* 接收环里的 UDP 记录格式：[u32 src_ip 大端][u16 src_port 主机序][u16 len] + 数据 */
static void udp_deliver(uint32_t src_ip, uint16_t src_port, uint16_t dst_port,
                        const uint8_t *data, uint32_t n) {
    for (uint32_t i = 0; i < NET_MAX_SOCKS; i++) {
        sock_t *s = &g_socks[i];
        if (!s->used || s->type != SOCK_TYPE_UDP) continue;
        if (s->local_port != dst_port) continue;

        uint32_t total = 8 + n;
        uint32_t space = (NET_SOCK_BUF - 1 + s->rx_r - s->rx_w) % NET_SOCK_BUF;
        if (space < total) return;                 /* 环满：丢弃 */

        uint8_t hdr[8];
        put32(hdr, src_ip);
        wr16le(hdr + 4, src_port);
        wr16le(hdr + 6, (uint16_t)n);
        for (uint32_t k = 0; k < 8; k++) {
            s->rx[s->rx_w] = hdr[k];
            s->rx_w = (s->rx_w + 1) % NET_SOCK_BUF;
        }
        for (uint32_t k = 0; k < n; k++) {
            s->rx[s->rx_w] = data[k];
            s->rx_w = (s->rx_w + 1) % NET_SOCK_BUF;
        }
        return;                                    /* 只投递第一个匹配 */
    }
    /* 无监听端口：静默丢弃（不做 ICMP port-unreachable） */
}

static void udp_input(const uint8_t *ip, uint32_t ihl) {
    uint32_t totlen = be16(ip + 2);
    if (totlen < ihl + UDP_HDR_LEN) return;
    const uint8_t *u = ip + ihl;
    uint32_t ulen = be16(u + 4);
    if (ulen < UDP_HDR_LEN) return;
    uint32_t dlen = ulen - UDP_HDR_LEN;
    if (dlen > totlen - ihl - UDP_HDR_LEN) return;

    g_udp_rx++;
    udp_deliver(be32(ip + 12), be16(u + 0), be16(u + 2),
                u + UDP_HDR_LEN, dlen);
}

static int net_udp_send(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
                        const uint8_t *data, uint32_t n) {
    if (n > NET_BUILD_MAX - IP_HDR_MIN - UDP_HDR_LEN) return -1;
    uint8_t *p = g_build;
    put16(p + 0, src_port);
    put16(p + 2, dst_port);
    put16(p + 4, (uint16_t)(UDP_HDR_LEN + n));
    put16(p + 6, 0);
    for (uint32_t i = 0; i < n; i++) p[UDP_HDR_LEN + i] = data[i];

    /* UDP 校验和含伪首部（IPv4 可选，算上更正规，对端可验证）。
     * 伪首部与段分别求 RAW 和再相加，最后统一取反。 */
    uint32_t need = UDP_HDR_LEN + n;
    if (need & 1) p[need] = 0;
    uint8_t ps[12];
    put32(ps + 0, my_ip_be());
    put32(ps + 4, dst_ip);
    ps[8] = 0; ps[9] = 17;
    put16(ps + 10, (uint16_t)need);
    uint32_t s = cksum_raw(ps, 12) + cksum_raw(p, need + (need & 1));
    while (s >> 16) s = (s & 0xFFFFu) + (s >> 16);
    uint16_t c = (uint16_t)(~s);
    if (c == 0) c = 0xFFFF;
    put16(p + 6, c);

    if (ipv4_send(dst_ip, 17, p, need) != 0) return -1;
    g_udp_tx++;
    return (int)n;
}

/* ---------- TCP ---------- */

static int tcp_send_seg(sock_t *s, uint8_t flags, const uint8_t *data, uint32_t n) {
    if (n > NET_BUILD_MAX - IP_HDR_MIN - TCP_HDR_MIN) return -1;
    const uint8_t *dst_mac = arp_lookup(s->peer_ip);
    if (!dst_mac) return -1;

    uint8_t *r = g_build;
    for (int i = 0; i < 6; i++) r[i] = dst_mac[i];
    const uint8_t *my = rtl8139_mac();
    for (int i = 0; i < 6; i++) r[6 + i] = my[i];
    r[12] = 0x08; r[13] = 0x00;

    uint8_t *ip = r + ETH_HDR_LEN;
    uint32_t iplen = IP_HDR_MIN + TCP_HDR_MIN + n;
    ip[0] = 0x45; ip[1] = 0;
    put16(ip + 2, (uint16_t)iplen);
    put16(ip + 4, ++g_ip_id);
    put16(ip + 6, 0);
    ip[8] = 64; ip[9] = 6;
    put16(ip + 10, 0);
    put32(ip + 12, my_ip_be());
    put32(ip + 16, s->peer_ip);

    uint8_t *t = ip + IP_HDR_MIN;
    put16(t + 0, s->local_port);
    put16(t + 2, s->peer_port);
    put32(t + 4, s->snd_nxt);
    put32(t + 8, s->rcv_nxt);
    t[12] = (uint8_t)(TCP_HDR_MIN << 2);
    t[13] = flags;
    put16(t + 14, 4096);                           /* 固定通告窗口 */
    put16(t + 16, 0); put16(t + 18, 0);
    for (uint32_t i = 0; i < n; i++) t[TCP_HDR_MIN + i] = data[i];

    /* 校验和：伪首部 + 段（含补零）。分段求 RAW 和，统一取反一次。 */
    uint32_t tlen = TCP_HDR_MIN + n;
    if (tlen & 1) t[tlen] = 0;
    uint8_t ps[12];
    put32(ps + 0, my_ip_be());
    put32(ps + 4, s->peer_ip);
    ps[8] = 0; ps[9] = 6;
    put16(ps + 10, (uint16_t)tlen);
    uint32_t sum = cksum_raw(ps, 12) + cksum_raw(t, tlen + (tlen & 1));
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    put16(t + 16, (uint16_t)(~sum));

    put16(ip + 10, cksum(ip, IP_HDR_MIN));

    if (rtl8139_send(r, ETH_HDR_LEN + iplen) != 0) return -1;
    g_tcp_tx++;
    return 0;
}

static void sock_free(sock_t *s) {
    s->used = 0;
    s->state = TS_UNUSED;
    /* rx 缓冲保留复用（挂在槽上），不 kfree */
}

static void tcp_input(const uint8_t *ip, uint32_t ihl) {
    uint32_t totlen = be16(ip + 2);
    if (totlen < ihl + TCP_HDR_MIN) return;
    const uint8_t *t = ip + ihl;
    uint32_t doff = (uint32_t)(t[12] >> 4) * 4u;
    if (doff < TCP_HDR_MIN || ihl + doff > totlen) return;
    uint8_t  flags = t[13];
    uint32_t seq = be32(t + 4);
    uint32_t ack = be32(t + 8);
    uint32_t dlen = totlen - ihl - doff;
    const uint8_t *data = t + doff;
    uint16_t dport = be16(t + 2);
    uint16_t sport = be16(t + 0);
    uint32_t srcip = be32(ip + 12);

    sock_t *conn = 0, *listener = 0;
    for (uint32_t i = 0; i < NET_MAX_SOCKS; i++) {
        sock_t *s = &g_socks[i];
        if (!s->used || s->type != SOCK_TYPE_TCP) continue;
        if (s->local_port != dport) continue;
        if (s->state == TS_LISTEN) { listener = s; continue; }
        if (s->peer_port == sport && s->peer_ip == srcip) { conn = s; break; }
    }

    if (!conn) {
        if (!listener || !(flags & TCP_SYN) || (flags & TCP_ACK)) return;
        for (uint32_t i = 0; i < NET_MAX_SOCKS; i++) {
            sock_t *s = &g_socks[i];
            if (s->used) continue;
            s->used = 1;
            s->type = SOCK_TYPE_TCP;
            s->state = TS_SYN_RCVD;
            s->local_port = dport;
            s->peer_port = sport;
            s->peer_ip = srcip;
            s->got_fin = 0;
            s->owner_pid = listener->owner_pid;
            s->parent = (int)(listener - g_socks);
            s->rx_r = s->rx_w = 0;
            /* ISN 由收发计数器拼成，可预测——教学取舍；真实环境必须随机化
             * （RFC 6528），否则序列号可被猜出，连接可被注入/劫持。 */
            s->snd_nxt = 0x1000 + g_tcp_tx * 64000u + g_udp_rx;
            s->rcv_nxt = seq + 1;
            tcp_send_seg(s, TCP_SYN | TCP_ACK, 0, 0);
            s->snd_nxt++;
            return;
        }
        return;                                    /* 连接表满：丢弃 */
    }

    if (flags & TCP_RST) { sock_free(conn); return; }

    switch (conn->state) {
    case TS_SYN_RCVD:
        if (!(flags & TCP_ACK) || ack != conn->snd_nxt) return;
        conn->state = TS_ESTABLISHED;
        if (dlen == 0 && !(flags & TCP_FIN)) return;  /* 纯握手 ACK：无需回包 */
        /* 握手第三次 ACK 捎带的首段数据/FIN：落入 ESTABLISHED 一并处理。
         * 原实现直接 return 丢掉——要等对端超时重传才收得到（多一个 RTT，
         * 不可靠对端则数据彻底丢失）。 */
        /* FALLTHROUGH */
    case TS_ESTABLISHED:
    case TS_CLOSE_WAIT:
        if (dlen > 0 && seq == conn->rcv_nxt) {
            uint32_t space = (NET_SOCK_BUF - 1 + conn->rx_r - conn->rx_w)
                             % NET_SOCK_BUF;
            if (space >= dlen) {
                for (uint32_t k = 0; k < dlen; k++) {
                    conn->rx[conn->rx_w] = data[k];
                    conn->rx_w = (conn->rx_w + 1) % NET_SOCK_BUF;
                }
                conn->rcv_nxt += dlen;
                g_tcp_rx += dlen;
            }
            /* 环满：不推进 rcv_nxt —— 对端超时重传即恢复 */
        }
        if (flags & TCP_FIN) {
            if (seq + dlen == conn->rcv_nxt) {
                conn->rcv_nxt++;
                conn->got_fin = 1;
                if (conn->state == TS_ESTABLISHED) conn->state = TS_CLOSE_WAIT;
            }
        }
        tcp_send_seg(conn, TCP_ACK, 0, 0);         /* 数据/FIN/乱序均即时 ACK */
        return;
    case TS_LAST_ACK:
        if ((flags & TCP_ACK) && ack == conn->snd_nxt) sock_free(conn);
        return;
    case TS_FIN_WAIT_1:
        if ((flags & TCP_ACK) && ack == conn->snd_nxt) conn->state = TS_FIN_WAIT_2;
        if (flags & TCP_FIN) {
            if (seq + dlen == conn->rcv_nxt) {
                conn->rcv_nxt++;               /* FIN 占一个序号：ACK 必须是 FINseq+1，
                                                * 否则对端视为未确认而重传 FIN */
                conn->got_fin = 1;
            }
            tcp_send_seg(conn, TCP_ACK, 0, 0);
            sock_free(conn);
        }
        return;
    case TS_FIN_WAIT_2:
        if (flags & TCP_FIN) {
            if (seq + dlen == conn->rcv_nxt) {
                conn->rcv_nxt++;               /* 同上：先推进序号再 ACK */
                conn->got_fin = 1;
            }
            tcp_send_seg(conn, TCP_ACK, 0, 0);
            sock_free(conn);
        }
        return;
    default:
        return;
    }
}

/* ---------- 帧分发入口 ---------- */

int net_input(const uint8_t *f, uint32_t len) {
    if (!build_ready()) return -1;
    if (len < ETH_HDR_LEN) return -1;
    uint16_t et = be16(f + 12);

    if (et == 0x0806) {                            /* ARP */
        if (len < ETH_HDR_LEN + 28) return -1;
        const uint8_t *a = f + ETH_HDR_LEN;
        if (be16(a + 0) != 1 || be16(a + 2) != 0x0800 ||
            a[4] != 6 || a[5] != 4) return -1;

        uint32_t spa = be32(a + 14);
        arp_learn(spa, a + 8);                     /* request/reply 都学习 */

        if (be16(a + 6) == 1 && be32(a + 24) == my_ip_be()) {
            uint8_t *r = g_build;
            for (int i = 0; i < 6; i++) r[i] = a[8 + i];
            const uint8_t *my = rtl8139_mac();
            for (int i = 0; i < 6; i++) r[6 + i] = my[i];
            r[12] = 0x08; r[13] = 0x06;
            uint8_t *p = r + ETH_HDR_LEN;
            put16(p + 0, 1); put16(p + 2, 0x0800); p[4] = 6; p[5] = 4;
            put16(p + 6, 2);
            for (int i = 0; i < 6; i++) p[8 + i] = my[i];
            put32(p + 14, my_ip_be());
            for (int i = 0; i < 6; i++) p[18 + i] = a[8 + i];
            put32(p + 24, spa);
            if (rtl8139_send(r, ETH_HDR_LEN + 28) == 0) g_arp_replied++;
        }
        return 0;
    }
    if (et != 0x0800) return -1;
    if (len < ETH_HDR_LEN + IP_HDR_MIN) return -1;
    const uint8_t *ip = f + ETH_HDR_LEN;
    if ((ip[0] >> 4) != 4) return -1;
    uint32_t ihl = (uint32_t)(ip[0] & 0x0F) * 4u;
    if (ihl < IP_HDR_MIN || ETH_HDR_LEN + ihl > len) return -1;
    uint32_t totlen = be16(ip + 2);
    if (totlen < ihl || totlen > 1500 || ETH_HDR_LEN + totlen > len) return -1;
    /* 统一 ARP 学习：任何发到本机的 IPv4 帧都记录 (源 IP, 源 MAC)。
     * 同网段直连成立（教学环境无路由）；回包不再需要 ARP 解析。 */
    arp_learn(be32(ip + 12), f + 6);
    if (be32(ip + 16) != my_ip_be()) return -1;

    if (ip[9] == 1)      icmp_input(ip, ihl);
    else if (ip[9] == 17) udp_input(ip, ihl);
    else if (ip[9] == 6)  tcp_input(ip, ihl);
    return 0;
}

/* ---------- socket 层 ---------- */

static sock_t *sock_get(uint32_t idx) {
    if (idx >= NET_MAX_SOCKS) return 0;
    if (!g_socks[idx].used) return 0;
    return &g_socks[idx];
}

static sock_t *sock_alloc(uint8_t type, uint32_t owner_pid) {
    /* 懒回收：表满时清掉 owner 进程已不存在的条目（ZOMBIE 未收割仍算
     * 存在——task_find 找得到，不误杀）。pid 0 = 内核，永不回收。 */
    for (uint32_t i = 0; i < NET_MAX_SOCKS; i++) {
        sock_t *s = &g_socks[i];
        if (s->used && s->owner_pid != 0 && task_find((int)s->owner_pid) == 0)
            sock_free(s);
    }
    for (uint32_t i = 0; i < NET_MAX_SOCKS; i++) {
        sock_t *s = &g_socks[i];
        if (s->used) continue;
        s->used = 1;
        s->type = type;
        s->state = TS_UNUSED;
        s->got_fin = 0;
        s->local_port = 0;
        s->peer_port = 0;
        s->peer_ip = 0;
        s->owner_pid = owner_pid;
        s->parent = -1;
        s->rx_r = s->rx_w = 0;
        s->snd_nxt = s->rcv_nxt = 0;
        if (!s->rx) {
            s->rx = kmalloc(NET_SOCK_BUF);
            if (!s->rx) { s->used = 0; return 0; }
        }
        return s;
    }
    return 0;
}

/* 步骤 8a：真睡眠等待条件。循环顶先 net_poll() 再查条件（覆盖"包在
 * 睡眠窗口内已到/唤醒源是无关中断"）；然后睡到 net_rx_wake 或超时。 */
static int wait_cond(int (*cond)(void *), void *ctx, uint32_t timeout_ms) {
    if (timeout_ms == 0) timeout_ms = NET_DEFAULT_WAIT_MS;
    uint32_t deadline = g_pit_ticks + timeout_ms;
    for (;;) {
        net_poll();
        if (cond(ctx)) return 1;
        uint32_t now = g_pit_ticks;
        if ((int32_t)(now - deadline) >= 0) return 0;
        task_sleep(&g_netrx_wq, deadline - now);
    }
}

static int cond_udp(void *p) {
    sock_t *s = (sock_t *)p;
    return s->rx_r != s->rx_w;
}
static int cond_tcp(void *p) {
    sock_t *s = (sock_t *)p;
    return s->rx_r != s->rx_w || s->got_fin;
}
static int cond_accept(void *p) {
    sock_t *l = (sock_t *)p;
    for (uint32_t i = 0; i < NET_MAX_SOCKS; i++)
        if (g_socks[i].used && g_socks[i].parent == (int)(l - g_socks) &&
            g_socks[i].state == TS_ESTABLISHED)
            return 1;
    return 0;
}

static int uptr_ok(uint32_t va, uint32_t len, int rw) {
    return rw ? syscall_user_range_rw(va, len) : syscall_user_range_ok(va, len);
}

int net_sockcall(uint32_t subcmd, const uint32_t a[5]) {
    task_t *cur = task_current();
    uint32_t pid = cur ? cur->pid : 0;
    if (!build_ready()) return -1;

    switch (subcmd) {
    case SC_SOCKET: {
        if (a[0] != SOCK_TYPE_UDP && a[0] != SOCK_TYPE_TCP) return -1;
        sock_t *s = sock_alloc(a[0], pid);
        if (!s) return -1;
        return (int)(s - g_socks);
    }
    case SC_BIND: {
        sock_t *s = sock_get(a[0]);
        if (!s || a[1] == 0 || a[1] > 0xFFFF) return -1;
        if (s->type == SOCK_TYPE_TCP && s->state != TS_UNUSED) return -1;
        /* 端口一律存数值（host 序）；线上转换只在 put16/be16 边界做 */
        for (uint32_t i = 0; i < NET_MAX_SOCKS; i++) {
            sock_t *o = &g_socks[i];
            if (o->used && o != s && o->local_port == (uint16_t)a[1])
                return -1;                         /* 端口已占用 */
        }
        s->local_port = (uint16_t)a[1];
        return 0;
    }
    case SC_LISTEN: {
        sock_t *s = sock_get(a[0]);
        if (!s || s->type != SOCK_TYPE_TCP || s->local_port == 0) return -1;
        s->state = TS_LISTEN;
        return 0;
    }
    case SC_ACCEPT: {
        sock_t *l = sock_get(a[0]);
        if (!l || l->type != SOCK_TYPE_TCP || l->state != TS_LISTEN) return -1;
        if (!wait_cond(cond_accept, l, a[1])) return -1;
        for (uint32_t i = 0; i < NET_MAX_SOCKS; i++) {
            sock_t *c = &g_socks[i];
            if (c->used && c->parent == (int)(l - g_socks) &&
                c->state == TS_ESTABLISHED) {
                c->parent = -1;
                return (int)i;
            }
        }
        return -1;
    }
    case SC_SENDTO: {
        sock_t *s = sock_get(a[0]);
        if (!s || a[2] == 0) return -1;
        if (!uptr_ok(a[1], a[2], 0)) return -1;
        const uint8_t *buf = (const uint8_t *)a[1];
        if (s->type == SOCK_TYPE_UDP) {
            if (s->local_port == 0 || a[4] == 0 || a[4] > 0xFFFF) return -1;
            return net_udp_send(a[3], (uint16_t)a[4],
                                s->local_port, buf, a[2]);
        }
        if (s->state != TS_ESTABLISHED && s->state != TS_CLOSE_WAIT) return -1;
        if (tcp_send_seg(s, TCP_PSH | TCP_ACK, buf, a[2]) != 0) return -1;
        s->snd_nxt += a[2];
        return (int)a[2];
    }
    case SC_RECVFROM: {
        sock_t *s = sock_get(a[0]);
        if (!s || a[2] == 0) return -1;
        if (!uptr_ok(a[1], a[2], 1)) return -1;
        uint8_t *buf = (uint8_t *)a[1];

        int (*cond)(void *) = (s->type == SOCK_TYPE_UDP) ? cond_udp : cond_tcp;
        if (!cond(s) && !wait_cond(cond, s, a[3])) return -1;

        if (s->type == SOCK_TYPE_UDP) {
            uint8_t hdr[8];
            for (int k = 0; k < 8; k++) {
                hdr[k] = s->rx[s->rx_r];
                s->rx_r = (s->rx_r + 1) % NET_SOCK_BUF;
            }
            uint32_t dn = hdr[6] | ((uint32_t)hdr[7] << 8);
            uint32_t n = (dn > a[2]) ? a[2] : dn;
            for (uint32_t k = 0; k < n; k++) {
                buf[k] = s->rx[s->rx_r];
                s->rx_r = (s->rx_r + 1) % NET_SOCK_BUF;
            }
            for (uint32_t k = n; k < dn; k++)      /* 截断残余弹掉，保边界 */
                s->rx_r = (s->rx_r + 1) % NET_SOCK_BUF;
            if (a[4] && uptr_ok(a[4], 8, 1)) {
                uint32_t *out = (uint32_t *)a[4];
                out[0] = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
                         ((uint32_t)hdr[2] << 8) | hdr[3];
                out[1] = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8);
            }
            return (int)n;
        }
        /* TCP：字节流；对端 FIN 且读空 → 0（EOF） */
        uint32_t n = 0;
        while (n < a[2] && s->rx_r != s->rx_w) {
            buf[n++] = s->rx[s->rx_r];
            s->rx_r = (s->rx_r + 1) % NET_SOCK_BUF;
        }
        return (int)n;
    }
    case SC_CLOSE: {
        sock_t *s = sock_get(a[0]);
        if (!s) return -1;
        if (s->type == SOCK_TYPE_TCP) {
            if (s->state == TS_ESTABLISHED || s->state == TS_CLOSE_WAIT) {
                if (tcp_send_seg(s, TCP_FIN | TCP_ACK, 0, 0) != 0) return -1;
                s->snd_nxt++;
                s->state = (s->state == TS_ESTABLISHED) ? TS_FIN_WAIT_1
                                                        : TS_LAST_ACK;
                return 0;
            }
            if (s->state == TS_LISTEN) {
                /* 关监听：未 accept 的子连接一并回收。留着它们的话，
                 * parent 记录的下标在本槽位被新 socket 复用后会遭误认领
                 * （cond_accept/accept 只比对下标，不比对身份）。 */
                for (uint32_t i = 0; i < NET_MAX_SOCKS; i++) {
                    if (g_socks[i].used && g_socks[i].parent == (int)(s - g_socks))
                        sock_free(&g_socks[i]);
                }
            }
        }
        sock_free(s);
        return 0;
    }
    default:
        return -1;
    }
}

/* ---------- 轮询 / 统计 ---------- */

void net_poll(void) {
    rtl8139_poll();
}

uint32_t net_udp_rx(void)      { return g_udp_rx; }
uint32_t net_udp_tx(void)      { return g_udp_tx; }
uint32_t net_tcp_rx(void)      { return g_tcp_rx; }
uint32_t net_tcp_tx(void)      { return g_tcp_tx; }
uint32_t net_arp_replied(void) { return g_arp_replied; }
uint32_t net_icmp_replied(void) { return g_icmp_replied; }
uint32_t net_arp_entries(void) {
    uint32_t n = 0;
    for (int i = 0; i < ARP_CACHE; i++) if (g_arp[i].ip != 0) n++;
    return n;
}
uint32_t net_sock_count(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < NET_MAX_SOCKS; i++) if (g_socks[i].used) n++;
    return n;
}
