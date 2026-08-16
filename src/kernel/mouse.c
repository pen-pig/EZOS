#include "mouse.h"
#include "port.h"
#include "types.h"
#include "tty.h"
// #include "gfx.h"  // gfx 图形模式已注释

#define MOUSE_WHEEL_BUF_SIZE 64

static int wheel_buf[MOUSE_WHEEL_BUF_SIZE];
static int wheel_start = 0;
static int wheel_end = 0;

static int mouse_available = 0;
static int has_wheel = 0;
static int mouse_irq_count = 0;  // 调试：IRQ12 触发次数

static uint8_t packet[4];
static int packet_index = 0;
static int packet_len = 3;

// 鼠标坐标与按钮状态
static int mouse_x = 160;
static int mouse_y = 100;
static int mouse_buttons = 0;

// 坐标边界（默认 320x200，可由 gfx_init / gui 按当前显示模式调整）
static int mouse_bounds_w = 320;
static int mouse_bounds_h = 200;

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

void mouse_init(void) {
    uint8_t status;

    terminal_writestring("mouse_init start\n");

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

    terminal_writestring("mouse_init end, irq=");
    // 简单十进制输出
    {
        int v = mouse_irq_count;
        char buf[12]; int i = 0;
        if (v == 0) { terminal_putchar('0'); }
        else { while (v > 0 && i < 11) { buf[i++] = (char)('0' + v % 10); v /= 10; } while (i > 0) terminal_putchar(buf[--i]); }
    }
    terminal_writestring("\n");
}

void mouse_handler(void) {
    mouse_irq_count++;
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

            // 解析按钮状态（byte0 低 3 位）
            mouse_buttons = packet[0] & 0x07;

            // 解析 X/Y 位移（9 位有符号）
            int dx = (int)(int8_t)packet[1];
            int dy = (int)(int8_t)packet[2];
            if (packet[0] & 0x10) dx |= 0xFFFFFF00;  // X 溢出/符号扩展
            if (packet[0] & 0x20) dy |= 0xFFFFFF00;  // Y 溢出/符号扩展

            // 更新坐标（屏幕 Y 轴向下为正，PS/2 Y 正方向向上，故减去 dy）
            mouse_x += dx;
            mouse_y -= dy;
            if (mouse_x < 0) mouse_x = 0;
            if (mouse_y < 0) mouse_y = 0;
            if (mouse_x >= mouse_bounds_w) mouse_x = mouse_bounds_w - 1;
            if (mouse_y >= mouse_bounds_h) mouse_y = mouse_bounds_h - 1;

            // 滚轮
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

int mouse_get_irq_count(void) {
    return mouse_irq_count;
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

// 设置坐标边界（按当前显示模式调整，如文本模式 640x400、图形模式 GFX_W/GFX_H）
void mouse_set_bounds(int w, int h) {
    if (w > 0) mouse_bounds_w = w;
    if (h > 0) mouse_bounds_h = h;
    if (mouse_x >= mouse_bounds_w) mouse_x = mouse_bounds_w - 1;
    if (mouse_y >= mouse_bounds_h) mouse_y = mouse_bounds_h - 1;
}

void irq12_handler(void) {
    mouse_handler();
}
