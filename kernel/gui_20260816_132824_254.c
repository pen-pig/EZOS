#include "gui.h"
#include "tty.h"
#include "keyboard.h"
#include "mouse.h"
#include "types.h"

// 文本模式字符尺寸：80x25，每字符 8x16 像素
#define GUI_TXT_W 80
#define GUI_TXT_H 25
#define GUI_CHAR_W 8
#define GUI_CHAR_H 16

// 简单 GUI：全屏文本模式菜单，方向键或 W/S 移动，回车确认；支持鼠标移动/点击
void gui_start(void) {
    terminal_initialize();
    terminal_setcolor(0x0F); // 白字黑底

    const char *title = "EZOS Graphical Interface (Text Mode)";
    const char *menu[] = {
        "1. About EZOS",
        "2. Show Time",
        "3. Exit to Shell"
    };
    int menu_count = 3;
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
            if (selected == 0) {
                terminal_initialize();
                terminal_writestring("EZOS v0.3-gui\nA simple OS written from scratch.\nPress Enter to return.\n");
                while (1) {
                    int c2 = keyboard_getchar();
                    if (c2 == '\n') break;
                }
            } else if (selected == 1) {
                terminal_initialize();
                terminal_writestring("Time: 12:00:00\nPress Enter to return.\n");
                while (1) {
                    int c2 = keyboard_getchar();
                    if (c2 == '\n') break;
                }
            } else if (selected == 2) {
                return;
            }
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
                if (selected == 0) {
                    terminal_initialize();
                    terminal_writestring("EZOS v0.3-gui\nA simple OS written from scratch.\nPress Enter to return.\n");
                    while (1) {
                        int c2 = keyboard_getchar();
                        if (c2 == '\n') break;
                    }
                } else if (selected == 1) {
                    terminal_initialize();
                    terminal_writestring("Time: 12:00:00\nPress Enter to return.\n");
                    while (1) {
                        int c2 = keyboard_getchar();
                        if (c2 == '\n') break;
                    }
                } else if (selected == 2) {
                    return;
                }
                break;
            }
        }
    }
}
