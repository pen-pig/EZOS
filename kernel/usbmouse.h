/*
 * usbmouse.h - USB HID 鼠标（真机点亮 H2-2e）
 *
 * 在 H2-2c 枚举结果之上做三件事：
 *   1. 认领 HID boot 鼠标（bInterfaceSubClass=1 / bInterfaceProtocol=2），
 *      用 SET_PROTOCOL(boot) + SET_IDLE(0) 把它切进 boot 报告格式；
 *   2. 轮询中断 IN 端点，解析 boot 报告（buttons / dx / dy / 可选 wheel）；
 *   3. 把位移与按键经 mouse_inject_report() 送进与 PS/2 共用的指针状态。
 *
 * 与键盘（usbkbd）一样是**轮询**的：UHCI 与 RTL8139 共享 IRQ11，中断路径未
 * 打通，而在 IRQ 里做毫秒级轮询等待是不可接受的。轮询点挂在
 * mouse_get_x()/mouse_present() 上并做 5ms 节流——所有既有的 GUI 取指针
 * 循环自动就获得了 USB 鼠标输入，无需逐个改。
 *
 * 坐标轴约定：boot 报告的 Y 正值是"向下"（与大多数 USB 鼠标固件一致，
 * 与 PS/2 的 "Y 正=向上" 相反），本模块统一转成屏幕坐标（dy>0 向下）。
 */
#ifndef USBMOUSE_H
#define USBMOUSE_H

/* 枚举后调用：认领鼠标设备并切 boot 协议。没有鼠标时打印一行并静默返回。 */
void usbmouse_init(void);

/* 轮询一次中断端点（内部 5ms 节流）。可在任务上下文随时调用；无鼠标时立即返回。 */
void usbmouse_poll(void);

/* 是否认领到了 USB HID boot 鼠标 */
int usbmouse_present(void);

/* 是否已至少收到过一个合法报告（诊断/E2E 用：证明中断 IN 通道真的通了） */
int usbmouse_got_report(void);

#endif
