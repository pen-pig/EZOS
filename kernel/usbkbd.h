/*
 * usbkbd.h - USB HID 键盘（真机点亮 H2-2d）
 *
 * 在 H2-2c 枚举结果之上做三件事：
 *   1. 认领 HID boot 键盘（bInterfaceSubClass=1 / bInterfaceProtocol=1），
 *      用 SET_PROTOCOL(boot) + SET_IDLE(0) 把它切进 boot 报告格式；
 *   2. 轮询中断 IN 端点，解析 8 字节 boot 报告（修饰键 + 6KRO 键码数组）；
 *   3. 把新按下的键经 keyboard_inject() 送进与 PS/2 共用的键盘缓冲。
 *
 * 本模块是**轮询**的（不是中断驱动）：UHCI 与 RTL8139 共享 IRQ11，中断路径
 * 尚未打通，而在 IRQ 里做毫秒级轮询等待是不可接受的。轮询点挂在
 * keyboard_getchar() 上并做 5ms 节流——所有既有的取键循环（shell / desktop /
 * games）自动就获得了 USB 键盘输入，无需逐个改。
 */
#ifndef USBKBD_H
#define USBKBD_H

/* 枚举后调用：认领键盘设备并切 boot 协议。没有键盘时打印一行并静默返回。 */
void usbkbd_init(void);

/* 轮询一次中断端点（内部 5ms 节流）。可在任务上下文随时调用；无键盘时立即返回。 */
void usbkbd_poll(void);

/* 是否认领到了 USB HID boot 键盘 */
int usbkbd_present(void);

/* 是否已至少收到过一个合法报告（诊断/E2E 用：证明中断 IN 通道真的通了） */
int usbkbd_got_report(void);

#endif
