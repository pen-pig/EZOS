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

/* rtl8139.c 收到完整以太帧（不含 CRC）后调入；返回后缓冲可复用 */
int net_input(const uint8_t *frame, uint32_t len);

/* sockcall 分发（args 已由 syscall.c 拷入内核；内含的用户指针未校验） */
int net_sockcall(uint32_t subcmd, const uint32_t a[5]);

/* 阻塞等待期间推进网络收包（轮询 RX，不依赖 IRQ） */
void net_poll(void);

/* 步骤 8a：rtl8139 在 rx_drain 排完一批包后调用——踢醒睡在 net 等待
 * 队列上的 recvfrom/accept/ARP 解析（task_wake_all，IRQ 上下文安全）。
 * 轮询路径（syscall 内 net_poll）同样会走到，重复唤醒无害（惊群自查）。 */
void net_rx_wake(void);

/* 统计（nic 命令 / E2E） */
uint32_t net_udp_rx(void);
uint32_t net_udp_tx(void);
uint32_t net_tcp_rx(void);
uint32_t net_tcp_tx(void);
uint32_t net_arp_replied(void);
uint32_t net_icmp_replied(void);
uint32_t net_arp_entries(void);
uint32_t net_sock_count(void);

#endif
