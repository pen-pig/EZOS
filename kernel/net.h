/*
 * net.h - IPv4/UDP/TCP 协议栈 + socket 层（步骤 7.3）
 *
 * 定位：rtl8139.c 之下的协议处理全部上收到本模块——
 *   rtl8139.c 只管"帧进帧出"（RX 环/TX 描述符/IRQ），收到完整以太帧后
 *   调 net_input()；本模块做 ARP/ICMP/UDP/TCP 解析、socket 收发缓冲、
 *   系统调用实现。
 *
 * 阻塞模型（步骤 8a 起）：等待不再忙轮询——recvfrom/accept/ARP 解析
 * 睡在 net 等待队列上（task_sleep，全程 cli，丢失唤醒被结构性排除），
 * 由 rtl8139 rx_drain 的 net_rx_wake()（IRQ11 或轮询路径都会走）与
 * IRQ0 的 task_timer_tick（超时）唤醒。任务睡眠后会被切走或停机
 * （sti;hlt），中断照常投递——旧的"不能在系统调用里等 IRQ"约束随
 * 可睡眠阻塞一起消失。循环顶的 net_poll() 保留为兜底排水。
 *
 * socket 归属：每个 socket 记 owner pid（0 = 内核）。进程退出不挂接
 * process_exit（不动调度器），改为懒回收：socket() 发现表满时，把
 * owner pid 已不存在的条目收掉。教学取舍，注释在 sock_alloc。
 *
 * ABI：SYS_SOCKCALL(102)，ebx=子命令，ecx=用户态 u32 args[5]。
 * 参数里的用户指针由本模块在使用前经 syscall_user_range_ok/_rw 校验。
 */
#ifndef NET_H
#define NET_H

#include "types.h"

/* socket 类型（SC_SOCKET 的 a0） */
#define SOCK_TYPE_UDP 0u
#define SOCK_TYPE_TCP 1u

/* sockcall 子命令（参数为用户态 u32 args[5] 的槽位） */
#define SC_SOCKET   0u   /* a0=type                        -> sock id   */
#define SC_BIND     1u   /* a0=sock, a1=port               -> 0/-1      */
#define SC_LISTEN   2u   /* a0=sock(TCP)                   -> 0/-1      */
#define SC_ACCEPT   3u   /* a0=sock(监听), a1=timeout_ms    -> conn id   */
#define SC_SENDTO   4u   /* a0=sock, a1=buf, a2=len,
                          * a3=dst_ip(大端打包), a4=dst_port -> 发送字节数 */
#define SC_RECVFROM 5u   /* a0=sock, a1=buf, a2=len, a3=timeout_ms,
                          * a4=out 指针(u32[2]: src ip/port，可为 0)
                          *                               -> 收到字节数/0=EOF/-1 */
#define SC_CLOSE    6u   /* a0=sock                        -> 0/-1      */
#define SC_CONNECT  7u   /* a0=sock, a1=dst_ip(大端), a2=dst_port,
                          * a3=timeout_ms（步骤 7.4 主动打开）  -> 0/-1   */

/* rtl8139.c 收到完整以太帧（不含 CRC）后调入；返回后缓冲可复用 */
int net_input(const uint8_t *frame, uint32_t len);

/* sockcall 分发（args 已由 syscall.c 拷入内核；内含的用户指针未校验） */
int net_sockcall(uint32_t subcmd, const uint32_t a[5]);

/* 阻塞等待期间推进网络收包（轮询 RX，不依赖 IRQ） */
void net_poll(void);

/* 步骤 7.4：TCP 重传定时器——由 IRQ0（isr.c 的 irq0_handler）每 tick 调用，
 * 内部按 TCP_TICK_MS 节流；系统调用上下文由 net_poll 一并调用。IRQ 上下文
 * 安全（重传走 rtl8139_send，自带 IF 保护）。 */
void net_tick(void);

/* ---- 网络上层演示（shell 命令 ping / httpd 的后端） ---- */

/* 主动 ping：发 ICMP echo request 并睡等匹配 reply。
 * 返回 0 = 收到（*rtt_ms = 往返毫秒）、1 = 超时、-1 = 发送失败。
 * 只能在任务上下文调用（内部会 task_sleep）。 */
int net_ping(uint32_t dst_ip_be, uint32_t timeout_ms, uint32_t *rtt_ms);

/* 简易 HTTP 服务：监听 80，服务**一个** GET 连接（回固定 200 页面 + FIN）
 * 后返回。返回 0 = 已服务、1 = 30s 无连接、-1 = 端口占用/内存不足/发送失败。
 * 不做常驻监听：shell 无 Ctrl-C 中断机制，accept 常驻会让 shell 永久阻塞。 */
int net_httpd_once(void);

/* 步骤 8a：rtl8139 在 rx_drain 排完一批包后调用——踢醒睡在 net 等待
 * 队列上的 recvfrom/accept/ARP 解析（task_wake_all，IRQ 上下文安全）。
 * 轮询路径（syscall 内 net_poll）同样会走到，重复唤醒无害（惊群自查）。 */
void net_rx_wake(void);

/* 统计（nic 命令 / E2E） */
uint32_t net_udp_rx(void);
uint32_t net_udp_tx(void);
uint32_t net_tcp_rx(void);
uint32_t net_tcp_tx(void);
uint32_t net_tcp_rtx(void);   /* 步骤 7.4：重传次数 */
uint32_t net_arp_replied(void);
uint32_t net_icmp_replied(void);
uint32_t net_arp_entries(void);
uint32_t net_sock_count(void);
uint32_t net_ping_reqs(void);
uint32_t net_ping_matched(void);

#endif
