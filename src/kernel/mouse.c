#include "mouse.h"
#include "port.h"
#include "types.h"

#define MOUSE_WHEEL_BUF_SIZE 64

static int wheel_buf[MOUSE_WHEEL_BUF_SIZE];
static int wheel_start = 0;
static int wheel_end = 0;

static int mouse_available = 0;
static int has_wheel = 0;

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

void irq12_handler(void) {
    mouse_handler();
}
