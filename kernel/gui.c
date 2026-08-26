/*
 * gui.c - EZOS 文本界面（Win10 风格字符界面 v0.5）
 * 整块重构：窗口边框 + 蓝色标题栏 + 反显菜单 + 实时时钟
 * 功能不变：About / Show Time / Exit 三项文本菜单
 */
#include "gui.h"
#include "tty.h"
#include "keyboard.h"
#include "port.h"
#include "types.h"

/* CP437 框线字符（VGA 文本模式单字节） */
#define BOX_TL "\xDA"   /* ┌ */
#define BOX_TR "\xBF"   /* ┐ */
#define BOX_BL "\xC0"   /* └ */
#define BOX_BR "\xD9"   /* ┘ */
#define BOX_H  "\xC4"   /* ─ */
#define BOX_V  "\xB3"   /* │ */

/* Win10 配色（VGA 文本属性：前景 | 背景<<4） */
#define C_TITLE  0x1F  /* 蓝底白字：标题栏/选中项（#0078D7 近似） */
#define C_ACCENT 0x09  /* 亮蓝字：强调文字 */
#define C_BAR    0x70  /* 灰底黑字：浅灰栏 */
#define C_NORMAL 0x07  /* 灰字黑底：正文 */
#define C_TEXT   0x0F  /* 白字黑底：标题文字 */
#define C_DIM    0x08  /* 深灰字：辅助提示 */

static void put_at(int row, int col, uint8_t attr, char c)
{
    terminal_set_cursor(row, col);
    terminal_setcolor(attr);
    terminal_putchar(c);
}

static void puts_at(int row, int col, uint8_t attr, const char *s)
{
    terminal_set_cursor(row, col);
    terminal_setcolor(attr);
    terminal_writestring(s);
}

static void fill_range(int row, int col, int len, uint8_t attr, char c)
{
    terminal_set_cursor(row, col);
    terminal_setcolor(attr);
    for (int i = 0; i < len; i++)
        terminal_putchar(c);
}

static void draw_box(int x, int y, int w, int h)
{
    puts_at(y, x, C_NORMAL, BOX_TL);
    fill_range(y, x + 1, w - 2, C_NORMAL, BOX_H[0]);
    put_at(y, x + w - 1, C_NORMAL, BOX_TR[0]);
    for (int r = 1; r < h - 1; r++) {
        put_at(y + r, x, C_NORMAL, BOX_V[0]);
        put_at(y + r, x + w - 1, C_NORMAL, BOX_V[0]);
    }
    puts_at(y + h - 1, x, C_NORMAL, BOX_BL);
    fill_range(y + h - 1, x + 1, w - 2, C_NORMAL, BOX_H[0]);
    put_at(y + h - 1, x + w - 1, C_NORMAL, BOX_BR[0]);
}

/* 等待回车 */
static void wait_enter(void)
{
    while (1) {
        int c = keyboard_getchar();
        if (c == '\n') break;
    }
}

/* CMOS 实时时钟（QEMU 支持） */
static uint8_t cmos_read(uint8_t reg)
{
    outb(0x70, reg);
    return inb(0x71);
}
static int bcd2dec(uint8_t v)
{
    return (v & 0x0F) + ((v >> 4) * 10);
}

/* About 子页 */
static void gui_about(void)
{
    const int wx = 15, wy = 4, ww = 50, wh = 10;
    terminal_initialize();
    draw_box(wx, wy, ww, wh);
    fill_range(wy + 1, wx + 1, ww - 2, C_TITLE, ' ');
    puts_at(wy + 1, wx + 3, C_TITLE, "About EZOS");
    fill_range(wy + 2, wx + 1, ww - 2, C_ACCENT, BOX_H[0]);
    puts_at(wy + 4, wx + 5, C_TEXT, "EZOS v0.3-gui");
    puts_at(wy + 5, wx + 5, C_NORMAL, "A simple OS written from scratch.");
    puts_at(wy + 7, wx + 5, C_DIM, "Press Enter to return.");
    wait_enter();
}

/* Show Time 子页（实时 CMOS 时钟） */
static void gui_time(void)
{
    const int wx = 15, wy = 4, ww = 50, wh = 10;
    uint8_t hour = bcd2dec(cmos_read(0x04));
    uint8_t minute = bcd2dec(cmos_read(0x02));
    uint8_t second = bcd2dec(cmos_read(0x00));

    /* 无效 CMOS（如全 0xFF）回退固定时间 */
    if (hour > 23 || minute > 59 || second > 59) {
        hour = 12; minute = 0; second = 0;
    }

    char buf[16];
    buf[0] = '0' + hour / 10; buf[1] = '0' + hour % 10; buf[2] = ':';
    buf[3] = '0' + minute / 10; buf[4] = '0' + minute % 10; buf[5] = ':';
    buf[6] = '0' + second / 10; buf[7] = '0' + second % 10; buf[8] = 0;

    terminal_initialize();
    draw_box(wx, wy, ww, wh);
    fill_range(wy + 1, wx + 1, ww - 2, C_TITLE, ' ');
    puts_at(wy + 1, wx + 3, C_TITLE, "Show Time");
    fill_range(wy + 2, wx + 1, ww - 2, C_ACCENT, BOX_H[0]);
    puts_at(wy + 4, wx + 5, C_TEXT, "Time: ");
    puts_at(wy + 4, wx + 11, C_ACCENT, buf);
    puts_at(wy + 7, wx + 5, C_DIM, "Press Enter to return.");
    wait_enter();
}

// Win10 风格文本菜单：方向键或 W/S 移动，回车确认
void gui_start(void) {
    terminal_initialize();
    terminal_setcolor(C_NORMAL);

    const char *title = "EZOS GUI  (Text Mode)";
    const char *menu[] = {
        "1. About EZOS",
        "2. Show Time",
        "3. Exit to Shell"
    };
    int menu_count = 3;
    int selected = 0;

    while (1) {
        const int wx = 15, wy = 3, ww = 50, wh = 11;
        terminal_initialize();
        draw_box(wx, wy, ww, wh);

        /* 蓝色标题栏 */
        fill_range(wy + 1, wx + 1, ww - 2, C_TITLE, ' ');
        puts_at(wy + 1, wx + 3, C_TITLE, title);
        /* 强调分隔线 */
        fill_range(wy + 2, wx + 1, ww - 2, C_ACCENT, BOX_H[0]);
        /* 菜单前留白 */
        fill_range(wy + 3, wx + 1, ww - 2, C_NORMAL, ' ');

        /* 菜单项（选中反显蓝底白字） */
        for (int i = 0; i < menu_count; i++) {
            int row = wy + 4 + i;
            if (i == selected) {
                fill_range(row, wx + 1, ww - 2, C_TITLE, ' ');
                puts_at(row, wx + 3, C_TITLE, "> ");
                puts_at(row, wx + 6, C_TITLE, menu[i]);
            } else {
                fill_range(row, wx + 1, ww - 2, C_NORMAL, ' ');
                puts_at(row, wx + 3, C_NORMAL, "  ");
                puts_at(row, wx + 6, C_NORMAL, menu[i]);
            }
        }
        /* 底部操作提示 */
        puts_at(wy + 9, wx + 3, C_DIM, "Up/Down (or W/S) move, Enter confirm");

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
                if (selected == 0) gui_about();
                else if (selected == 1) gui_time();
                else if (selected == 2) return;
                break;
            }
        }
    }
}
