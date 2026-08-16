#include "gui.h"
#include "tty.h"
#include "keyboard.h"
#include "types.h"

// 简单 GUI：全屏文本模式菜单，方向键或 W/S 移动，回车确认
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

    while (1) {
        terminal_initialize();
        terminal_set_cursor(0, 0);
        terminal_writestring(title);
        terminal_writestring("\n\n");

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
