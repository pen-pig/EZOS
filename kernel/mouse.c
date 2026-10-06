/*
 * mouse.c - EZOS PS/2 鼠标驱动（v0.6 重构）
 * 职责：PS/2 位移累积、分辨率自适应坐标裁剪（320x200 / 640x480）
 *       mouse_get_x/y/buttons/wheel 查询接口；无鼠标时优雅降级
 */
#include "mouse.h"
#include "usbmouse.h"
#include "port.h"
#include "types.h"
#include "gfx.h"
#include "irqflags.h"

#define MOUSE_WHEEL_BUF_SIZE 64

static int wheel_buf[MOUSE_WHEEL_BUF_SIZE];
static int wheel_start = 0;
static int wheel_end = 0;

static int mouse_available = 0;
static int has_wheel = 0;
static int usb_present = 0;     /* H2-2e：USB boot 鼠标已认领（mouse_present 用） */

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

/*
 * 位移累积 + 分辨率自适应裁剪。
 * 参数统一为**屏幕坐标**：dx > 0 向右，dy > 0 向下。
 * PS/2 原生 Y 是"正=向上"，调用方负责取反（见 mouse_handler）；
 * USB boot 报告原生 Y 就是"正=向下"，usbmouse 侧直接传。
 */
static int mouse_speed = 256;   /* 灵敏度倍率 256=1.0x */

/* 设置鼠标灵敏度（256 = 原始速度；>256 加速，<256 减速） */
void mouse_set_sensitivity(int mul256) {
    if (mul256 < 32) mul256 = 32;
    if (mul256 > 2048) mul256 = 2048;
    mouse_speed = mul256;
}

int mouse_get_sensitivity(void) {
    return mouse_speed;
}

/* 滚轮环形缓冲：满了就丢弃最旧的事件（绝不覆盖未读数据，也绝不阻塞） */
static void wheel_push(int z) {
    int next = (wheel_end + 1) % MOUSE_WHEEL_BUF_SIZE;
    if (next == wheel_start) return;      /* 满：丢 */
    wheel_buf[wheel_end] = z;
    wheel_end = next;
}

static void mouse_accumulate(int dx, int dy) {
    dx = (int)((dx * mouse_speed) / 256);
    dy = (int)((dy * mouse_speed) / 256);
    mouse_x += dx;
    mouse_y += dy;
    int max_x = (GFX_W > 0) ? (GFX_W - 1) : 319;   /* 未初始化时按 320x200 兜底 */
    int max_y = (GFX_H > 0) ? (GFX_H - 1) : 199;
    if (mouse_x < 0) mouse_x = 0;
    else if (mouse_x > max_x) mouse_x = max_x;
    if (mouse_y < 0) mouse_y = 0;
    else if (mouse_y > max_y) mouse_y = max_y;
}

/* ==========================================================================
 * 纯函数：包解析
 *
 * 这两条分支以前各自散在 IRQ12 处理与 USB 轮询里，既不共享也没被任何测试
 * 覆盖——QEMU 没有触摸板、也不发滚轮事件，真机上一旦字节解析差一位就只能
 * 靠肉眼看指针飘。抽成纯函数（不碰全局、不碰端口）之后 selftest 可以直接
 * 喂合成包，把"只能在真机上试"的东西搬到开机自检里。
 *
 * Y 轴：PS/2 原生"正=向上"，USB boot 报告"正=向下"。统一取屏幕坐标
 * （dy>0 向下）后交给 mouse_accumulate，GUI 侧不必知道指针来自哪条总线。
 * ========================================================================== */

int mouse_decode_ps2(const uint8_t *pkt, uint32_t len, int wheel,
                     mouse_ev_t *ev) {
    if (!pkt || !ev || len < 3) return 1;          /* 太短：直接丢 */
    uint8_t b0 = pkt[0];
    if (!(b0 & 0x08)) return 1;                    /* bit3 同步位必须为 1 */
    /* 命令应答字节虽然 bit3=1，但不是包首字节；混进来会让整条包流错位 */
    if (b0 == 0xFA || b0 == 0xAA || b0 == 0xEE) return 1;
    /* bit7/bit6 = X/Y 溢出：位移数据无效，必须整包丢弃（否则指针乱跳） */
    if (b0 & 0xC0) return 1;

    ev->buttons = b0 & 0x07;
    ev->dx = (int)(int8_t)pkt[1];
    ev->dy = -(int)(int8_t)pkt[2];                 /* PS/2 正=向上 → 取反 */
    ev->dz = (wheel && len >= 4) ? (int)(int8_t)pkt[3] : 0;
    return 0;
}

int mouse_decode_usb(const uint8_t *rep, uint32_t len, mouse_ev_t *ev) {
    if (!rep || !ev || len < 3) return 1;          /* boot 报告至少 3 字节 */
    ev->buttons = rep[0] & 0x07;
    ev->dx = (int)(int8_t)rep[1];
    ev->dy = (int)(int8_t)rep[2];                  /* USB 正=向下，与屏幕同向 */
    ev->dz = (len >= 4) ? (int)(int8_t)rep[3] : 0; /* 第 4 字节是可选滚轮 */
    return 0;
}

void mouse_set_protocol(int four_byte) {
    uint32_t flags = irq_save_disable();
    has_wheel = four_byte ? 1 : 0;
    packet_len = has_wheel ? 4 : 3;
    packet_index = 0;      /* 切协议必须重置，否则半包与新长度错位 */
    irq_restore(flags);
}

int mouse_protocol(void) {
    return packet_len;
}

/* ---------- 自检：合成包向量 + 注入路径的端到端方向校验 ---------- */
int mouse_selftest(void (*out)(const char *line)) {
    int bad = 0;
    int asserts = 0;

    struct {
        uint8_t p[4];
        uint32_t len;
        int wheel;
        int valid;             /* 0 = 该包必须被丢弃 */
        int dx, dy, btn, dz;   /* valid=1 时的期望值 */
    } t[] = {
        /* --- PS/2 三字节 --- */
        { {0x08, 0x00, 0x00, 0}, 3, 0, 1,   0,   0, 0, 0 },  /* 静止 */
        { {0x09, 0x0A, 0x00, 0}, 3, 0, 1,  10,   0, 1, 0 },  /* 右键?左键 bit0 */
        { {0x08, 0xF6, 0x00, 0}, 3, 0, 1, -10,   0, 0, 0 },  /* 负位移符号扩展 */
        { {0x08, 0x00, 0x05, 0}, 3, 0, 1,   0,  -5, 0, 0 },  /* PS/2 上移 → dy<0 */
        { {0x08, 0x00, 0xFB, 0}, 3, 0, 1,   0,   5, 0, 0 },  /* PS/2 下移 → dy>0 */
        { {0x0C, 0x7F, 0x7F, 0}, 3, 0, 1, 127,-127, 4, 0 },  /* 中键 + 满量程 */
        /* --- 必须丢弃的包 --- */
        { {0x00, 0x00, 0x00, 0}, 3, 0, 0,   0,   0, 0, 0 },  /* 无同步位 */
        { {0xC8, 0x00, 0x00, 0}, 3, 0, 0,   0,   0, 0, 0 },  /* X 溢出 */
        { {0x48, 0x00, 0x00, 0}, 3, 0, 0,   0,   0, 0, 0 },  /* Y 溢出 */
        { {0xFA, 0x00, 0x00, 0}, 3, 0, 0,   0,   0, 0, 0 },  /* ACK */
        { {0xAA, 0x00, 0x00, 0}, 3, 0, 0,   0,   0, 0, 0 },  /* BAT */
        { {0x08, 0x00, 0x00, 0}, 2, 0, 0,   0,   0, 0, 0 },  /* 长度不足 */
        /* --- IntelliMouse 四字节 --- */
        { {0x08, 0x00, 0x00, 0x01}, 4, 1, 1,   0,   0, 0,  1 },
        { {0x08, 0x00, 0x00, 0xFF}, 4, 1, 1,   0,   0, 0, -1 },
        { {0x08, 0x00, 0x00, 0x01}, 4, 0, 1,   0,   0, 0,  0 }, /* 未开滚轮：dz 恒 0 */
        { {0x08, 0x00, 0x00, 0x01}, 3, 1, 1,   0,   0, 0,  0 }, /* 三字节包无 dz */
    };

    for (uint32_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        mouse_ev_t ev;
        ev.dx = ev.dy = ev.buttons = ev.dz = 0;
        asserts++;
        int r = mouse_decode_ps2(t[i].p, t[i].len, t[i].wheel, &ev);
        if (!t[i].valid) {
            if (r == 0) bad++;                       /* 该丢却收了 */
            continue;
        }
        if (r != 0) { bad++; continue; }
        if (ev.dx != t[i].dx || ev.dy != t[i].dy ||
            ev.buttons != t[i].btn || ev.dz != t[i].dz) bad++;
    }

    /* --- USB boot 报告：Y 轴方向与 PS/2 相反，必须逐条钉死 --- */
    struct {
        uint8_t r[4];
        uint32_t len;
        int valid;
        int dx, dy, btn, dz;
    } u[] = {
        { {0x01, 0x10, 0x20, 0}, 3, 1,  16,  32, 1, 0 },  /* USB 正 Y = 向下 */
        { {0x00, 0xF0, 0xF0, 0}, 3, 1, -16, -16, 0, 0 },
        { {0x02, 0x00, 0x00, 0}, 3, 1,   0,   0, 2, 0 },  /* 右键 */
        { {0x04, 0x00, 0x00, 0xFE}, 4, 1, 0, 0, 4, -2 },  /* 滚轮下滚 */
        { {0x01, 0x10, 0x20, 0}, 2, 0,   0,   0, 0, 0 },  /* 报告不完整 */
    };
    for (uint32_t i = 0; i < sizeof(u) / sizeof(u[0]); i++) {
        mouse_ev_t ev;
        ev.dx = ev.dy = ev.buttons = ev.dz = 0;
        asserts++;
        int r = mouse_decode_usb(u[i].r, u[i].len, &ev);
        if (!u[i].valid) {
            if (r == 0) bad++;
            continue;
        }
        if (r != 0) { bad++; continue; }
        if (ev.dx != u[i].dx || ev.dy != u[i].dy ||
            ev.buttons != u[i].btn || ev.dz != u[i].dz) bad++;
    }

    /* --- 端到端：注入 USB 事件，验证"屏幕坐标方向"与滚轮环形缓冲 ---
     * 这一段会真的动指针状态，所以先存后恢复，灵敏度也临时钉成 1.0x。 */
    {
        int sp = mouse_get_sensitivity();
        int sx = mouse_x, sy = mouse_y;
        mouse_set_sensitivity(256);
        mouse_warp(100, 100);

        mouse_inject_report(0, 10, 0, 0);            /* 向下 10 */
        asserts++;
        if (mouse_get_y() != 110) bad++;
        mouse_inject_report(10, 0, 0, 0);            /* 向右 10 */
        asserts++;
        if (mouse_get_x() != 110) bad++;

        /* 钳位：负方向越过 0 不能变负，也不能绕回屏幕另一侧 */
        mouse_inject_report(-500, -500, 0, 0);
        asserts++;
        if (mouse_get_x() != 0 || mouse_get_y() != 0) bad++;

        /* 滚轮环形缓冲：单事件能取回原值；灌爆后留下来的不超过容量 */
        while (mouse_get_wheel() != 0) { }           /* 排空（原本应为空） */
        mouse_inject_report(0, 0, 0, 5);
        asserts++;
        if (mouse_get_wheel() != 5) bad++;
        for (int i = 0; i < 200; i++) mouse_inject_report(0, 0, 0, 1);
        int got = 0;
        while (mouse_get_wheel() != 0 && got < 400) got++;
        asserts++;
        if (got > MOUSE_WHEEL_BUF_SIZE) bad++;       /* 溢出即状态破坏 */
        while (mouse_get_wheel() != 0) { }

        mouse_set_sensitivity(sp);
        mouse_warp(sx, sy);
    }

    /* 手写十进制（内核无 sprintf） */
    if (out) {
        char line[96];
        int p = 0;
        int lim = (int)sizeof(line) - 1;
        char tmp[12];
        int m;
        uint32_t v;

        const char *s = "  mouse: ";
        while (*s && p < lim) line[p++] = *s++;
        v = (uint32_t)asserts; m = 0;
        if (v == 0) tmp[m++] = '0';
        while (v) { tmp[m++] = (char)('0' + v % 10); v /= 10; }
        while (m && p < lim) line[p++] = tmp[--m];
        s = " asserts, proto=";
        while (*s && p < lim) line[p++] = *s++;
        v = (uint32_t)packet_len; m = 0;
        if (v == 0) tmp[m++] = '0';
        while (v) { tmp[m++] = (char)('0' + v % 10); v /= 10; }
        while (m && p < lim) line[p++] = tmp[--m];
        s = "B -> ";
        while (*s && p < lim) line[p++] = *s++;
        v = (uint32_t)bad; m = 0;
        if (v == 0) tmp[m++] = '0';
        while (v) { tmp[m++] = (char)('0' + v % 10); v /= 10; }
        while (m && p < lim) line[p++] = tmp[--m];
        s = bad ? " FAILED\n" : " ok\n";
        while (*s && p < lim) line[p++] = *s++;
        line[p] = '\0';
        out(line);
    }
    return bad;
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

    // 启用滚轮（IntelliMouse 协议）：采样率 200 -> 100 -> 80
    // [DEBUG] 强制 3 字节标准 PS/2 协议,避免与 QEMU 包长错位
    has_wheel = 0;
    packet_len = 3;
    packet_index = 0;
    /* 关键：必须在 mouse_available=1 之前使能数据报告并消费其 ACK(0xFA)。
     * 否则 IRQ12 处理程序会把 0xFA 当作包首字节（0xFA 的 bit3=1，能通过
     * 同步位检查），导致整个数据包流永久错位 —— 鼠标移动/点击全部错乱。 */
    mouse_cmd(0xF4);   // 使能数据报告

    mouse_available = 1;
    mouse_fail_stage = 0x55;   // [DEBUG] init 成功（0=未执行 1=A9失败 2=BAT失败 0xAA=未到末尾）

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
        /* 防御：命令应答字节（ACK=0xFA / BAT=0xAA / 回显=0xEE）虽然 bit3=1
         * 能通过同步检查，但不是数据包首字节。混入会导致包流错位。 */
        if (data == 0xFA || data == 0xAA || data == 0xEE) {
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

            mouse_ev_t ev;
            ev.dx = ev.dy = ev.buttons = ev.dz = 0;
            /* 解析与判废全在纯函数里（不同步 / 溢出 / 应答字节一律丢弃） */
            if (mouse_decode_ps2(packet, (uint32_t)packet_len, has_wheel, &ev) != 0) {
                outb(0xA0, 0x20);
                outb(0x20, 0x20);
                return;
            }
            pkt_cnt++;                    /* 只计真正生效的包 */

            mouse_buttons = ev.buttons;
            mouse_accumulate(ev.dx, ev.dy);
            if (ev.dz != 0) wheel_push(ev.dz);
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
    /* USB 鼠标是轮询驱动的（usbmouse_poll），这里顺带驱动一次，
     * 让 GUI/诊断在第一次查询前就能拿到最新的指针状态。 */
    usbmouse_poll();
    return (mouse_available || usb_present) ? 1 : 0;
}

int mouse_get_x(void) {
    /* 轮询钩子：GUI 每帧都会读坐标，USB 鼠标因此获得稳定的轮询机会
     *（内部 5ms 节流 + IF=0 直接返回，不会拖慢也不会挂死）。 */
    usbmouse_poll();
    return mouse_x;
}

/* ---- H2-2e：USB 鼠标注入（与 IRQ12 的 PS/2 路径共用状态） ----
 * 参数已由 usbmouse 转成屏幕坐标（dy>0 向下），这里只做灵敏度缩放、
 * 钳位与关中断保护。
 */
void mouse_inject_report(int dx, int dy, int buttons, int dz) {
    uint32_t flags = irq_save_disable();
    mouse_buttons = buttons & 0x07;
    mouse_accumulate(dx, dy);
    pkt_cnt++;
    if (dz != 0) wheel_push(dz);
    irq_restore(flags);
}

void mouse_usb_set_present(int present) {
    usb_present = present ? 1 : 0;
}

int mouse_usb_present(void) {
    return usb_present;
}

/* 直接设置指针位置（用于 GUI 启动时按实际分辨率居中） */
void mouse_warp(int x, int y) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (GFX_W > 0 && x > GFX_W - 1) x = GFX_W - 1;
    if (GFX_H > 0 && y > GFX_H - 1) y = GFX_H - 1;
    mouse_x = x;
    mouse_y = y;
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
