#include "keyboard.h"
#include "tty.h"
#include "port.h"
#include "types.h"

#define KEYBOARD_BUFFER_SIZE 256
static int keyboard_buffer[KEYBOARD_BUFFER_SIZE];  // �洢 int��֧���������
static int buffer_start = 0;
static int buffer_end = 0;

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

    /* ����ǰ�����ֽ� */
    kb_wait_write();
    outb(0x64, 0x20);
    kb_wait_read();
    status = inb(0x60);

    /* ��λ bit0�������ж�ʹ�ܣ���-��-д��������� bit1 ������λ�� */
    status |= 0x01;
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
            }
        }
    }

    outb(0x20, 0x20);
}

int keyboard_getchar(void) {
    if (buffer_start == buffer_end) return 0;
    int c = keyboard_buffer[buffer_start];
    buffer_start = (buffer_start + 1) % KEYBOARD_BUFFER_SIZE;
    return c;
}

void irq1_handler(void) {
    keyboard_handler();
}
