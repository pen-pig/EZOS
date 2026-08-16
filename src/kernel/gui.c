#include "gui.h"
#include "tty.h"
#include "keyboard.h"
#include "mouse.h"
#include "exfat.h"
#include "types.h"

// 文本模式字符尺寸：80x25，每字符 8x16 像素
#define GUI_TXT_W 80
#define GUI_TXT_H 25
#define GUI_CHAR_W 8
#define GUI_CHAR_H 16

// 读取一行输入（Enter 结束；Esc 或 q 返回 -1；支持退格）
static int gui_read_line(char *buf, int maxlen) {
    int n = 0;
    while (1) {
        int c = keyboard_getchar();
        if (c == 0) continue;
        if (c == '\n') {
            buf[n] = '\0';
            return n;
        }
        if (c == 27 || c == 'q' || c == 'Q') {
            return -1;
        }
        if (c == '\b' || c == 8) {
            if (n > 0) {
                n--;
                terminal_putchar('\b');
                terminal_putchar(' ');
                terminal_putchar('\b');
            }
            continue;
        }
        if (n < maxlen - 1) {
            buf[n++] = (char)c;
            terminal_putchar((char)c);
        }
    }
}

// 等待任意键返回
static void gui_wait_key(void) {
    terminal_writestring("\n[Press any key to return to menu]");
    while (keyboard_getchar() == 0) {}
}

// 运行选中的应用；返回 1 表示退出 gui，0 表示返回菜单
static int gui_run_app(int sel) {
    if (sel == 8) return 1; // Exit

    terminal_initialize();
    terminal_setcolor(0x0F);

    if (sel == 0) { // Calculator
        terminal_writestring("Calculator: enter expression (e.g. 1+2*3), Enter to eval, q to quit.\n> ");
        char buf[40];
        while (1) {
            int n = gui_read_line(buf, sizeof(buf));
            if (n < 0) break;
            if (n > 0) {
                cmd_calc(buf);
                terminal_writestring("> ");
            }
        }
    } else if (sel == 1) { // Hex
        terminal_writestring("Hex: enter a number (dec or 0x..), Enter to convert, q to quit.\n> ");
        char buf[40];
        while (1) {
            int n = gui_read_line(buf, sizeof(buf));
            if (n < 0) break;
            if (n > 0) {
                cmd_hex(buf);
                terminal_writestring("> ");
            }
        }
    } else if (sel == 2) { // Rand
        terminal_writestring("Random number (0-99): ");
        cmd_rand("");
        gui_wait_key();
    } else if (sel == 3) { // Guess
        cmd_guess("");
        gui_wait_key();
    } else if (sel == 4) { // TicTacToe
        cmd_tictactoe("");
        gui_wait_key();
    } else if (sel == 5) { // Snake
        cmd_snake("");
        gui_wait_key();
    } else if (sel == 6) { // Viewer
        terminal_writestring("Viewer: README.TXT\n------------------\n");
        static uint8_t vbuf[512];
        int vn = exfat_read_file("README.TXT", vbuf, 512);
        if (vn > 0) {
            for (int i = 0; i < vn; i++) {
                terminal_putchar((char)vbuf[i]);
            }
        } else {
            terminal_writestring("(no README.TXT)\n");
        }
        gui_wait_key();
    } else if (sel == 7) { // About
        terminal_writestring("EZOS v0.3-gui\nA simple OS written from scratch.\n");
        terminal_writestring("Apps: Calculator, Hex, Rand, Guess, TicTacToe, Snake, Viewer\n");
        gui_wait_key();
    }
    return 0;
}

// 简单 GUI：全屏文本模式菜单，方向键或 W/S 移动，回车确认；支持鼠标移动/点击
void gui_start(void) {
    terminal_initialize();
    terminal_setcolor(0x0F); // 白字黑底

    const char *title = "EZOS GUI (Text Mode)";
    const char *menu[] = {
        "1. Calculator",
        "2. Hex",
        "3. Rand",
        "4. Guess",
        "5. TicTacToe",
        "6. Snake",
        "7. Viewer",
        "8. About",
        "9. Exit"
    };
    int menu_count = 9;
    int selected = 0;

    // 文本模式坐标边界（640x400），供鼠标坐标裁剪
    mouse_set_bounds(GUI_TXT_W * GUI_CHAR_W, GUI_TXT_H * GUI_CHAR_H);

    // 鼠标点击边沿检测
    int prev_buttons = 0;

    while (1) {
        terminal_initialize();
        terminal_set_cursor(0, 0);
        terminal_writestring(title);
        terminal_writestring("\n\n");

        // 读取鼠标状态，映射到文本行列
        int mx = mouse_get_x();
        int my = mouse_get_y();
        int mrow = my / GUI_CHAR_H;
        int mcol = mx / GUI_CHAR_W;
        int mbuttons = mouse_get_buttons();
        int mouse_click = (mbuttons & MOUSE_BTN_LEFT) && !(prev_buttons & MOUSE_BTN_LEFT);
        prev_buttons = mbuttons;

        // 鼠标悬停：若鼠标行落在菜单项区域，跟随高亮
        if (mrow >= 2 && mrow < 2 + menu_count) {
            selected = mrow - 2;
        }

        for (int i = 0; i < menu_count; i++) {
            if (i == selected) {
                terminal_setcolor(0x70); // 反显
                terminal_writestring("> ");
                terminal_writestring(menu[i]);
                terminal_setcolor(0x0F);
            } else {
                terminal_writestring("  ");
                terminal_writestring(menu[i]);
            }
            terminal_writestring("\n");
        }

        terminal_writestring("\nUse Up/Down (or W/S) to move, Enter to confirm.\n");
        terminal_writestring("Mouse: move to select, left-click to confirm.\n");

        // 绘制鼠标光标（反显块），不超出屏幕
        if (mouse_present()) {
            if (mrow >= 0 && mrow < GUI_TXT_H && mcol >= 0 && mcol < GUI_TXT_W) {
                terminal_set_cursor(mrow, mcol);
                terminal_setcolor(0x70);
                terminal_putchar(' ');
                terminal_setcolor(0x0F);
            }
        }

        // 鼠标点击菜单项 = 回车确认
        if (mouse_click && mrow >= 2 && mrow < 2 + menu_count) {
            selected = mrow - 2;
            if (gui_run_app(selected)) return;
            continue;
        }

        while (1) {
            int c = keyboard_getchar();
            if (c == 0) continue;

            if (c == 'w' || c == 'W' || c == KEY_UP) {
                if (selected > 0) selected--;
                break;
            } else if (c == 's' || c == 'S' || c == KEY_DOWN) {
                if (selected < menu_count - 1) selected++;
                break;
            } else if (c == '\n') {
                if (gui_run_app(selected)) return;
                break;
            }
        }
    }
}
