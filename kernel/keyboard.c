#include "keyboard.h"
#include "tty.h"
#include "port.h"
#include "types.h"
#include "task.h"
#include "usbkbd.h"
#include "usbmouse.h"
#include "irqflags.h"
#include "isr.h"

#define KEYBOARD_BUFFER_SIZE 256
static int keyboard_buffer[KEYBOARD_BUFFER_SIZE];  // 键存 int，支持扩展键码
static int buffer_start = 0;
static int buffer_end = 0;

/* 键盘等待队列（步骤 8a）：阻塞读睡在这上面，IRQ1 塞入按键后唤醒。
 * 静态即可——全系统只有一个键盘。{-1}：head 哨兵（0 是任务下标）。 */
static wait_queue_t kb_wq = {-1};

static int left_shift = 0;
static int right_shift = 0;
static int alt_down = 0;      /* 左/右 Alt 按下状态（Alt+Tab 窗口切换用） */

/* �ȴ� 8042 ���뻺���д��IBF ��գ� */
static void kb_wait_write(void) {
    int timeout = 100000;
    while (--timeout > 0 && (inb(0x64) & 0x02)) ;
}

/* �ȴ� 8042 �������ɶ���OBF ��λ�� */
static void kb_wait_read(void) {
    int timeout = 100000;
    while (--timeout > 0 && !(inb(0x64) & 0x01)) ;
}

/* ���� 8042 ���� IRQ�������ֽ� bit0=1������������� scancode */
void keyboard_init(void) {
    uint8_t status;

    /* Read the current controller command byte. */
    kb_wait_write();
    outb(0x64, 0x20);
    kb_wait_read();
    status = inb(0x60);

    /* HOT-REBOOT FIX (must not be a plain "|=" on bit0 only):
     *
     * system_reboot() pulses CPU reset through the 8042 (outb(0x64,0xFE)).
     * That resets the CPU but NOT the keyboard controller, and a warm BIOS
     * boot does not always reprogram the KBC. The translation bit (bit6,
     * set-2 -> set-1) can therefore be LOST across reboot, leaving the
     * keyboard emitting native set-2 bytes while every table in this driver
     * is built for set 1.
     *
     * Measured symptom: after `reboot`, typing "ver" shows "JJXX" on screen
     * and the command never runs -- set-2 'v' is 0x2A, which this driver
     * reads as "left shift pressed", so everything after it is misdecoded.
     * Before reboot the same keystrokes arrive as 2F AF 12 92 13 93 1C 9C
     * (correct set 1).
     *
     * So: force translation ON and the keyboard interface ENABLED instead of
     * preserving whatever the BIOS left behind.
     *   bit0 (INT)  = 1 : keyboard IRQ1 enabled
     *   bit4 (KDIS) = 0 : keyboard interface enabled (1 would disable it)
     *   bit6 (XLATE)= 1 : translate scan code set 2 -> set 1
     * bit5 (AUX/mouse interface) is deliberately left untouched. */
    status |= 0x01;         /* IRQ1 enable */
    status &= (uint8_t)~0x10;   /* keyboard interface enabled */
    status |= 0x40;         /* translate to scan code set 1 */
    kb_wait_write();
    outb(0x64, 0x60);
    kb_wait_write();
    outb(0x60, status);

    /* ������� scancode����ֹ��ʼ���ڼ��ѹ�İ�����Ⱦ�������� */
    while ((inb(0x64) & 0x01)) {
        inb(0x60);
    }

    /* ȷ�����̽ӿ����� */
    kb_wait_write();
    outb(0x64, 0xAE);
}

/* ��ϣ������жϵ��ü�����data ��ȫ�֣��� QEMU monitor ���ڴ���֤�� */
volatile uint32_t kb_irq_count = 0;
volatile uint32_t kb_last_scancode = 0;

static const char scancode_to_ascii_base[] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,  'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,  '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' ', 0,
    /* 0x3B..0x44: F1..F10（由 switch 映射为 KEY_F1..KEY_F10，表置 0） */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 0x45 NumLock, 0x46 ScrollLock */
    0, 0,
    /* 0x47..0x53: 小键盘 7 - 5 + 1 0 .（方向/PgUp/PgDn 由 switch 处理） */
    '7', 0, 0, '-', 0, '5', 0, '+', '1', 0, 0, '0', '.',
    /* 0x54..0x56: 未用 */
    0, 0, 0,
    /* 0x57 F11, 0x58 F12（由 switch 映射） */
    0, 0,
};

static const char scancode_to_ascii_shift[] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,  'A','S','D','F','G','H','J','K','L',':','"','~',
    0,  '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0, ' ', 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0,
    '7', 0, 0, '-', 0, '5', 0, '+', '1', 0, 0, '0', '.',
    0, 0, 0,
    0, 0,
};

void keyboard_handler(void) {
    kb_irq_count++;
    uint8_t scancode = inb(0x60);
    kb_last_scancode = scancode;

    if (scancode == 0x2A || scancode == 0x36) {       // Shift ����
        left_shift = (scancode == 0x2A) ? 1 : left_shift;
        right_shift = (scancode == 0x36) ? 1 : right_shift;
        outb(0x20, 0x20);
        return;
    } else if (scancode == 0xAA || scancode == 0xB6) { // Shift �ͷ�
        left_shift = (scancode == 0xAA) ? 0 : left_shift;
        right_shift = (scancode == 0xB6) ? 0 : right_shift;
        outb(0x20, 0x20);
        return;
    } else if (scancode == 0x38) {   // Alt ����
        alt_down = 1;
        outb(0x20, 0x20);
        return;
    } else if (scancode == 0xB8) {   // Alt �ͷ�
        alt_down = 0;
        outb(0x20, 0x20);
        return;
    }

    if (!(scancode & 0x80)) { // �����¼�
        int key = 0;
        int shifted = (left_shift || right_shift);
        switch (scancode) {
            case 0x0F: key = alt_down ? KEY_ALT_TAB : '\t'; break;   /* Tab：Alt 按下时为窗口切换 */
            case 0x48: key = shifted ? KEY_PGUP : KEY_UP; break;
            case 0x50: key = shifted ? KEY_PGDN : KEY_DOWN; break;
            case 0x4B: key = KEY_LEFT; break;
            case 0x4D: key = KEY_RIGHT; break;
            case 0x49: key = KEY_PGUP; break;
            case 0x51: key = KEY_PGDN; break;
            case 0x3B: key = KEY_F1; break;
            case 0x3C: key = KEY_F2; break;
            case 0x3D: key = KEY_F3; break;
            case 0x3E: key = KEY_F4; break;
            case 0x3F: key = KEY_F5; break;
            case 0x40: key = KEY_F6; break;
            case 0x41: key = KEY_F7; break;
            case 0x42: key = KEY_F8; break;
            case 0x43: key = KEY_F9; break;
            case 0x44: key = KEY_F10; break;
            case 0x57: key = KEY_F11; break;
            case 0x58: key = KEY_F12; break;
            default:
                if (scancode < sizeof(scancode_to_ascii_base)) {
                    if (shifted) {
                        key = (unsigned char)scancode_to_ascii_shift[scancode];
                    } else {
                        key = (unsigned char)scancode_to_ascii_base[scancode];
                    }
                }
                break;
        }

        if (key != 0) {
            int next = (buffer_end + 1) % KEYBOARD_BUFFER_SIZE;
            if (next != buffer_start) {
                keyboard_buffer[buffer_end] = key;
                buffer_end = next;
                task_wake_all(&kb_wq);    /* 有新数据：踢醒阻塞读（IRQ 上下文安全） */
            }
        }
    }

    outb(0x20, 0x20);
}

/*
 * USB HID 键盘路径（H2-2d）的注入口。与 IRQ1 共用同一个环形缓冲，所以必须
 * 用 irq_save_disable 保护索引（IRQ1 可能在任意时刻插入）。
 * 缓冲满时丢弃（与 IRQ1 行为一致），绝不覆盖未读数据。
 */
void keyboard_inject(int key) {
    if (key == 0) return;
    uint32_t f = irq_save_disable();
    int next = (buffer_end + 1) % KEYBOARD_BUFFER_SIZE;
    if (next != buffer_start) {
        keyboard_buffer[buffer_end] = key;
        buffer_end = next;
    }
    irq_restore(f);
    task_wake_all(&kb_wq);    /* 有新数据：踢醒阻塞读 */
}

int keyboard_getchar(void) {
    /* H2-2d: poll the USB HID keyboard before handing out buffered keys.
     * All existing get-and-loop callers (shell / desktop / games) pick up
     * USB keystrokes through this one hook; throttling lives in usbkbd_poll. */
    usbkbd_poll();
    /* H2-2e: the USB mouse is polled too, for the same reason as the keyboard
     * - a text-mode shell never calls mouse_get_x(), so this busy-wait hook is
     * the only place the pointer gets serviced. Throttling/IF guard live in
     * usbmouse_poll. */
    usbmouse_poll();
    if (buffer_start == buffer_end) return 0;
    int c = keyboard_buffer[buffer_start];
    buffer_start = (buffer_start + 1) % KEYBOARD_BUFFER_SIZE;
    return c;
}

/* 步骤 8a：任务上下文的阻塞等待——睡到下一个按键（IRQ1 唤醒）或
 * 30s 超时。超时兜底是给"永远没人打字"的调用方（如用户进程阻塞
 * read(stdin)）留一条退出路径，避免任务永久滞留 BLOCKED。
 * 不可在 IRQ / task_lock 临界区调用（task_sleep 会拒绝并立即返回）。
 *
 * H2-2d：USB 键盘是**轮询**的，一次睡满 30s 期间它的按键根本进不来。
 * 改成 50ms 短睡循环，每轮醒来做一次 usbkbd_poll()；PS/2（IRQ1）仍然
 * 立即唤醒，行为不变。退化保护：task_sleep 在 IRQ / task_lock 里会立即
 * 返回，此时 tick 不推进，连试几次就直接返回，避免退化成几百次空转。 */
void keyboard_block(void) {
    uint32_t t0 = g_pit_ticks;
    int spins = 0;
    while ((uint32_t)(g_pit_ticks - t0) < 30000u) {
        usbkbd_poll();
        if (buffer_start != buffer_end) return;
        uint32_t before = g_pit_ticks;
        task_sleep(&kb_wq, 50);
        if (g_pit_ticks == before && ++spins > 4) return;   /* 根本没睡着 */
    }
}

void irq1_handler(void) {
    keyboard_handler();
}
