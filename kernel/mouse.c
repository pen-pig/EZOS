/*
 * mouse.c - EZOS PS/2 鼠标驱动（v0.6 重构）
 * 职责：PS/2 位移累积、分辨率自适应坐标裁剪（320x200 / 640x480）
 *       mouse_get_x/y/buttons/wheel 查询接口；无鼠标时优雅降级
 */
#include "mouse.h"
#include "port.h"
#include "types.h"
#include "gfx.h"

#define MOUSE_WHEEL_BUF_SIZE 64

static int wheel_buf[MOUSE_WHEEL_BUF_SIZE];
static int wheel_start = 0;
static int wheel_end = 0;

static int mouse_available = 0;
static int has_wheel = 0;

/* 当前指针坐标（范围随 GFX_W/GFX_H 自适应，初始化时置屏幕中心） */
static int mouse_x = 0;
static int mouse_y = 0;
static int mouse_buttons = 0;   // bit0=左, bit1=右, bit2=中

static uint8_t packet[4];
static int packet_index = 0;
static int packet_len = 3;

// 等待 PS/2 输出缓冲可读
static void ps2_wait_read(void) {
    int timeout = 100000;
    while (--timeout > 0 && !(inb(0x64) & 1)) ;
}

// 等待 PS/2 输入缓冲可写
static void ps2_wait_write(void) {
    int timeout = 100000;
    while (--timeout > 0 && (inb(0x64) & 2)) ;
}

// 向鼠标发送命令（先写 0xD4 到 0x64，再写命令到 0x60）
static void mouse_write(uint8_t cmd) {
    ps2_wait_write();
    outb(0x64, 0xD4);
    ps2_wait_write();
    outb(0x60, cmd);
}

static uint8_t mouse_read(void) {
    ps2_wait_read();
    return inb(0x60);
}

// 发送命令并丢弃 ACK
static void mouse_cmd(uint8_t cmd) {
    mouse_write(cmd);
    mouse_read();
}

/* 位移累积 + 分辨率自适应裁剪：
 * PS/2 X 向右为正、Y 向上为正；屏幕 Y 向下为正，故 dy 取反。
 * 钳位范围随 GFX_W/GFX_H 动态变化：320x200 或 640x480 均正确。
 */
static int mouse_speed = 256;   /* 灵敏度倍率 256=1.0x */

/* 设置鼠标灵敏度（256 = 原始速度；>256 加速，<256 减速） */
void mouse_set_sensitivity(int mul256) {
    if (mul256 < 32) mul256 = 32;
    if (mul256 > 2048) mul256 = 2048;
    mouse_speed = mul256;
}

static void mouse_accumulate(int dx, int dy) {
    dx = (int)((dx * mouse_speed) / 256);
    dy = (int)((dy * mouse_speed) / 256);
    mouse_x += dx;
    mouse_y -= dy;
    if (mouse_x < 0) mouse_x = 0;
    else if (mouse_x > GFX_W - 1) mouse_x = GFX_W - 1;
    if (mouse_y < 0) mouse_y = 0;
    else if (mouse_y > GFX_H - 1) mouse_y = GFX_H - 1;
}

void mouse_init(void) {
    uint8_t status;

    // 使能辅助端口
    ps2_wait_write();
    outb(0x64, 0xA8);

    // 测试辅助端口是否有设备（返回 0x00 = 有设备）
    ps2_wait_write();
    outb(0x64, 0xA9);
    ps2_wait_read();
    if (inb(0x60) != 0x00) {
        mouse_available = 0;
        return;   // 无鼠标 / 无触摸板，优雅降级
    }

    // 使能 IRQ12，清除第二个端口时钟禁用
    ps2_wait_write();
    outb(0x64, 0x20);
    ps2_wait_read();
    status = inb(0x60);
    status |= 0x02;
    status &= ~0x20;
    ps2_wait_write();
    outb(0x64, 0x60);
    ps2_wait_write();
    outb(0x60, status);

    // 重置鼠标
    mouse_cmd(0xFF);
    if (mouse_read() != 0xAA) {   // BAT 自检结果应为 0xAA
        mouse_available = 0;
        return;
    }
    mouse_read();  // 设备 ID（通常 0x00）

    mouse_available = 1;

    // 启用滚轮（IntelliMouse 协议）：采样率 200 -> 100 -> 80
    mouse_cmd(0xF3); mouse_cmd(200);
    mouse_cmd(0xF3); mouse_cmd(100);
    mouse_cmd(0xF3); mouse_cmd(80);

    // 读设备 ID：0x03 = IntelliMouse（4 字节包，带滚轮）
    mouse_cmd(0xF2);
    uint8_t id = mouse_read();
    if (id == 0x03) {
        has_wheel = 1;
        packet_len = 4;
    } else {
        has_wheel = 0;
        packet_len = 3;
    }

    // 使能数据报告
    mouse_cmd(0xF4);

    // 分辨率自适应：指针初始位置 = 屏幕中心（320x200 -> 160,100；640x480 -> 320,240）
    if (GFX_W > 0 && GFX_H > 0) {
        mouse_x = GFX_W / 2;
        mouse_y = GFX_H / 2;
    } else {
        mouse_x = 160;   // gfx 未初始化时的安全兜底
        mouse_y = 100;
    }
}

void mouse_handler(void) {
    if (!mouse_available) {
        outb(0xA0, 0x20);
        outb(0x20, 0x20);
        return;
    }
    uint8_t data = inb(0x60);

    if (packet_index == 0) {
        if (!(data & 0x08)) {   // 同步位必须为 1
            outb(0xA0, 0x20);
            outb(0x20, 0x20);
            return;
        }
        packet[0] = data;
        packet_index = 1;
    } else {
        packet[packet_index] = data;
        packet_index++;
        if (packet_index >= packet_len) {
            packet_index = 0;

            // PS/2 位移累积（带符号），并按当前分辨率裁剪
            int dx = (int)(int8_t)packet[1];
            int dy = (int)(int8_t)packet[2];
            mouse_accumulate(dx, dy);
            mouse_buttons = packet[0] & 0x07;

            if (has_wheel) {
                int8_t z = (int8_t)packet[3];
                if (z != 0) {
                    int next = (wheel_end + 1) % MOUSE_WHEEL_BUF_SIZE;
                    if (next != wheel_start) {
                        wheel_buf[wheel_end] = (int)z;
                        wheel_end = next;
                    }
                }
            }
        }
    }
    outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

int mouse_get_wheel(void) {
    if (wheel_start == wheel_end) return 0;
    int w = wheel_buf[wheel_start];
    wheel_start = (wheel_start + 1) % MOUSE_WHEEL_BUF_SIZE;
    return w;
}

int mouse_present(void) {
    return mouse_available;
}

int mouse_get_x(void) {
    return mouse_x;
}

int mouse_get_y(void) {
    return mouse_y;
}

int mouse_get_buttons(void) {
    return mouse_buttons;
}

void irq12_handler(void) {
    mouse_handler();
}
