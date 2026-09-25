/*
 * serial.h - COM1 串口调试输出（真机点亮的最基本手段）
 *
 * 真机上没有 QMP/screendump，串口是唯一的"开机即用"诊断通道：
 * BIOS 装载完内核的第一毫秒起 0x3F8 就可用，不依赖显存模式/分页。
 * 所有 klog/dmesg 日志与 panic 现场都镜像到这里（见 dmesg.c/panic.c）。
 */
#ifndef SERIAL_H
#define SERIAL_H

/* 初始化 COM1：115200 8N1 + FIFO。做一次环回自检（MODEM 控制器回环），
 * 失败（无串口硬件）则保持 serial_ready=0，后续输出静默跳过。 */
void serial_init(void);

/* 单字符/字符串输出。未初始化或自检失败时为无害 no-op。
 * '\n' 自动展开为 '\r\n'（真机终端协议）。 */
void serial_putc(char c);
void serial_write(const char *s);

/* 串口是否可用（环回自检通过） */
int serial_ready(void);

#endif
