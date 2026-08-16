#include "keyboard.h"
#include "tty.h"
#include "port.h"
#include "types.h"

#define KEYBOARD_BUFFER_SIZE 256
static int keyboard_buffer[KEYBOARD_BUFFER_SIZE];  // 存储 int，支持虚拟键码
static int buffer_start = 0;
static int buffer_end = 0;

static int shift_pressed = 0;

static const char scancode_to_ascii_base[] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,  'a','s','d','f','g','h','j','k','l',';','\'','`',
    0,  '\\','z','x','c','v','b','n','m',',','.','/', 0,
    '*', 0, ' ', 0,
};

static const char scancode_to_ascii_shift[] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,  'A','S','D','F','G','H','J','K','L',':','"','~',
    0,  '|','Z','X','C','V','B','N','M','<','>','?', 0,
    '*', 0, ' ', 0,
};

void keyboard_handler(void) {
    uint8_t scancode = inb(0x60);

    if (scancode == 0x2A || scancode == 0x36) {       // Shift 按下
        shift_pressed = 1;
        outb(0x20, 0x20);
        return;
    } else if (scancode == 0xAA || scancode == 0xB6) { // Shift 释放
        shift_pressed = 0;
        outb(0x20, 0x20);
        return;
    }

    if (!(scancode & 0x80)) { // 按下事件
        int key = 0;
        switch (scancode) {
            case 0x48: key = shift_pressed ? KEY_PGUP : KEY_UP; break;
            case 0x50: key = shift_pressed ? KEY_PGDN : KEY_DOWN; break;
            case 0x4B: key = KEY_LEFT; break;
            case 0x4D: key = KEY_RIGHT; break;
            case 0x49: key = KEY_PGUP; break;
            case 0x51: key = KEY_PGDN; break;
            default:
                if (scancode < sizeof(scancode_to_ascii_base)) {
                    if (shift_pressed) {
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

void keyboard_init(void) {
    // 无需操作
}

void irq1_handler(void) {
    keyboard_handler();
}
