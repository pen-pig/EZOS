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
static unsigned long pkt_cnt = 0;   /* [DEBUG] 收到的完整包计数 */
static unsigned char last_raw[2];   /* [DEBUG] 最近收到的 2 个原始字节 */
static int last_raw_n = 0;
static unsigned long raw_cnt = 0;   /* [DEBUG] 收到的原始字节总数 */

/* [DEBUG] mouse_init 失败阶段探针（.bss, volatile 防 DCE; QEMU 内存读可靠）:
   0x55=成功  1=A9 测试全失败  2=BAT 未收到 0xAA  0xAA=未到末尾  0=未执行 */
volatile unsigned char mouse_fail_stage;
volatile unsigned char mouse_a9val;   // [DEBUG] A9 测试实际返回字节

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
    int tries;

    mouse_fail_stage = 0xAA;  /* [DEBUG] 进入 init */

    // 使能辅助端口
    ps2_wait_write();
    outb(0x64, 0xA8);

    // 测试辅助端口是否有设备（返回 0x00 = 有设备）。
    // QEMU/部分固件应答较慢，单次读取可能碰上 0x60 未就绪读到 0xFF，
    // 这里做多次重试，避免误判“无鼠标”导致 GUI 鼠标整个不可用。
    for (tries = 0; tries < 5; tries++) {
        ps2_wait_write();
        outb(0x64, 0xA9);
        ps2_wait_read();
        mouse_a9val = inb(0x60);   /* [DEBUG] 记录 A9 返回值 */
        if (mouse_a9val == 0x00) break;
    }
    if (tries >= 5) {
        mouse_available = 0;
        mouse_fail_stage = 1;   // A9 测试全失败
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

    // 重置鼠标：BAT 自检应答为 0xAA（之前会先回 ACK 0xFA）。
    // 虚拟环境/慢固件可能让 ACK 或 BAT 晚到，这里不依赖“恰好下一字节就是 0xAA”，
    // 改为轮询有限个字节直到等来 0xAA，期间跳过无意义的 0xFA/0xFE。
    mouse_write(0xFF);
    {
        uint8_t bb = 0;
        int guard = 40;               /* 最多轮询 40 字节 */
        int bat_ok = 0;
        while (guard-- > 0) {
            ps2_wait_read();
            bb = inb(0x60);
            if (bb == 0xAA) { bat_ok = 1; mouse_fail_stage = 0xBB; break; }
            if (bb != 0xFA && bb != 0xFE) break;   /* 非预期应答，直接判失败 */
        }
        if (!bat_ok) {
            mouse_available = 0;
            mouse_fail_stage = 2;   // BAT 未收到 0xAA
            return;
        }
    }
    mouse_read();  // 设备 ID（通常 0x00）

    mouse_available = 1;
    mouse_fail_stage = 0x55;   // [DEBUG] init 成功（0=未执行 1=A9失败 2=BAT失败 0xAA=未到末尾）

    // 启用滚轮（IntelliMouse 协议）：采样率 200 -> 100 -> 80
    // [DEBUG] 强制 3 字节标准 PS/2 协议,避免与 QEMU 包长错位
    has_wheel = 0;
    packet_len = 3;
    mouse_cmd(0xF4);   // 使能数据报告

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
    last_raw[last_raw_n] = data;                    /* [DEBUG] 记录原始字节 */
    last_raw_n = (last_raw_n + 1) % 2;
    raw_cnt++;

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
            pkt_cnt++;

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

/* [DEBUG] 完整包计数（用于验证 PS/2 按钮/移动事件是否到达驱动） */
unsigned long mouse_packet_count(void) {
    return pkt_cnt;
}

/* [DEBUG] 最近 2 个原始字节（out[0]=较新, out[1]=较旧） + 原始字节总数 */
void mouse_raw_trace(unsigned char *out, unsigned long *cnt) {
    unsigned char s[2] = {0, 0};
    int n = last_raw_n;
    s[0] = last_raw[(n + 1) % 2];   /* 最近一个 */
    s[1] = last_raw[(n + 0) % 2];   /* 上一个 */
    out[0] = s[0]; out[1] = s[1];
    if (cnt) *cnt = raw_cnt;
}

void irq12_handler(void) {
    mouse_handler();
}
