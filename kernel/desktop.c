/*
 * desktop.c - EZOS 桌面文件管理器（Win10 风格字符界面 v0.5）
 * 整块重构：蓝色顶栏 + 路径栏 + 列表/滚动 + 底部状态栏 + 查看文件页
 * 功能不变：列表 / 进入目录 / 查看文件 / 返回
 */
#include "desktop.h"
#include "tty.h"
#include "keyboard.h"
#include "types.h"
#include "exfat.h"

#define MAX_ENTRIES 64
#define MAX_VIEW_SIZE 4096
#define LIST_ROWS 20

static exfat_dir_entry_t entries[MAX_ENTRIES];
static int entry_count = 0;
static int selected = 0;
static int scroll = 0;

/* CP437 字符 */
#define BOX_H  "\xC4"   /* ─ */

/* Win10 配色（VGA 文本属性：前景 | 背景<<4） */
#define C_TITLE  0x1F  /* 蓝底白字：顶栏/选中行 */
#define C_BAR    0x70  /* 灰底黑字：路径/状态栏 */
#define C_NORMAL 0x07  /* 灰字黑底：文件 */
#define C_TEXT   0x0F  /* 白字黑底：正文 */
#define C_DIR    0x09  /* 亮蓝字：目录 */
#define C_DIM    0x08  /* 深灰字：辅助 */

static int dstrlen(const char *s)
{
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void puts_at(int row, int col, uint8_t attr, const char *s)
{
    /* 限宽写入，避免超出 80 列换行错乱 */
    int max = 80 - col;
    if (max < 0) return;
    terminal_set_cursor(row, col);
    terminal_setcolor(attr);
    for (int i = 0; s[i] && i < max; i++)
        terminal_putchar(s[i]);
}

static void fill_range(int row, int col, int len, uint8_t attr, char c)
{
    terminal_set_cursor(row, col);
    terminal_setcolor(attr);
    for (int i = 0; i < len; i++)
        terminal_putchar(c);
}

static void print_dec(uint32_t num)
{
    char tmp[12];
    int i = 0;
    if (num == 0) {
        terminal_putchar('0');
        return;
    }
    while (num > 0) {
        tmp[i++] = '0' + (num % 10);
        num /= 10;
    }
    while (i > 0)
        terminal_putchar(tmp[--i]);
}

/* 十进制转字符串，返回长度 */
static int u32_to_str(uint32_t num, char *buf)
{
    char tmp[12];
    int i = 0;
    if (num == 0) {
        buf[0] = '0'; buf[1] = 0;
        return 1;
    }
    while (num > 0) {
        tmp[i++] = '0' + (num % 10);
        num /= 10;
    }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = 0;
    return j;
}

/* 查看文件内容页 */
static void desktop_view_file(const char *name)
{
    static uint8_t buf[MAX_VIEW_SIZE];
    int n = exfat_read_file(name, buf, MAX_VIEW_SIZE - 1);
    terminal_initialize();

    /* 顶部标题栏（Win10 蓝底白字） */
    fill_range(0, 0, 80, C_TITLE, ' ');
    puts_at(0, 2, C_TITLE, " View: ");
    puts_at(0, 9, C_TITLE, name);
    /* 分隔线 */
    fill_range(1, 0, 80, C_DIM, BOX_H[0]);

    if (n < 0) {
        puts_at(3, 2, C_TEXT, "Failed to read file.");
    } else {
        buf[n] = '\0';
        terminal_setcolor(C_NORMAL);
        terminal_writestring((const char *)buf);
        if (n >= MAX_VIEW_SIZE - 1)
            terminal_writestring("\n... (truncated)\n");
    }
    terminal_writestring("\n--- Press any key to return ---\n");
    while (keyboard_getchar() == 0) {}
}

/* 绘制 Win10 风格桌面 */
static void desktop_render(void)
{
    terminal_initialize();

    /* 顶部标题栏 */
    fill_range(0, 0, 80, C_TITLE, ' ');
    puts_at(0, 2, C_TITLE, " EZOS Desktop - File Manager");

    /* 路径栏（浅灰底黑字） */
    fill_range(1, 0, 80, C_BAR, ' ');
    puts_at(1, 1, C_BAR, "Path: ");
    puts_at(1, 7, C_BAR, exfat_cwd_path());

    /* 分隔线 */
    fill_range(2, 0, 80, C_DIM, BOX_H[0]);

    entry_count = exfat_read_dir(entries, MAX_ENTRIES);
    if (entry_count < 0) entry_count = 0;

    /* 列表区 */
    for (int i = 0; i < LIST_ROWS && (scroll + i) < entry_count; i++) {
        int idx = scroll + i;
        int row = 3 + i;
        uint8_t a = (idx == selected) ? C_TITLE : (entries[idx].is_dir ? C_DIR : C_NORMAL);

        fill_range(row, 0, 80, a, ' ');
        puts_at(row, 1, a, "> ");

        if (entries[idx].is_dir) {
            puts_at(row, 4, a, "[D] ");
            puts_at(row, 8, a, entries[idx].name);
            puts_at(row, 8 + dstrlen(entries[idx].name), a, "/");
        } else {
            char sz[32];
            int szlen;
            sz[0] = '(';
            szlen = 1 + u32_to_str(entries[idx].size, sz + 1);
            sz[szlen++] = ' ';
            sz[szlen++] = 'b'; sz[szlen++] = 'y';
            sz[szlen++] = 't'; sz[szlen++] = 'e';
            sz[szlen++] = 's'; sz[szlen++] = ')';
            sz[szlen] = 0;
            int scol = 80 - 1 - szlen;
            if (scol < 8) scol = 8;
            puts_at(row, 4, a, "[F] ");
            puts_at(row, 8, a, entries[idx].name);
            puts_at(row, scol, a, sz);
        }
    }

    /* 空目录提示 */
    if (entry_count == 0) {
        fill_range(3, 0, 80, C_NORMAL, ' ');
        puts_at(3, 4, C_DIM, "(empty directory)");
    }

    /* 清理剩余行残留 */
    int shown = entry_count - scroll;
    if (shown > LIST_ROWS) shown = LIST_ROWS;
    if (shown < 0) shown = 0;
    for (int i = shown; i < LIST_ROWS; i++)
        fill_range(3 + i, 0, 80, C_NORMAL, ' ');

    /* 分隔线 + 底部状态栏 */
    fill_range(23, 0, 80, C_DIM, BOX_H[0]);
    fill_range(24, 0, 80, C_BAR, ' ');
    puts_at(24, 2, C_BAR, "UP/DOWN select   Enter open   Backspace up   Q quit");

    /* 右侧计数 [n/m] */
    terminal_set_cursor(24, 66);
    terminal_setcolor(C_BAR);
    terminal_writestring("[");
    print_dec(entry_count == 0 ? 0 : selected + 1);
    terminal_writestring("/");
    print_dec(entry_count);
    terminal_writestring("]");
}

// Win10 风格文本文件管理器：列表 / 进入目录 / 查看文件 / 返回
void desktop_run(void) {
    terminal_initialize();
    selected = 0;
    scroll = 0;
    desktop_render();

    while (1) {
        int c = keyboard_getchar();
        if (c == 0) continue;

        if (c == KEY_UP) {
            if (selected > 0) {
                selected--;
                if (selected < scroll) scroll = selected;
                desktop_render();
            }
        } else if (c == KEY_DOWN) {
            if (selected < entry_count - 1) {
                selected++;
                if (selected >= scroll + LIST_ROWS) scroll = selected - LIST_ROWS + 1;
                desktop_render();
            }
        } else if (c == '\n') {
            if (entry_count > 0 && selected < entry_count) {
                if (entries[selected].is_dir) {
                    if (exfat_change_dir(entries[selected].name) == 0) {
                        selected = 0;
                        scroll = 0;
                        desktop_render();
                    }
                } else {
                    desktop_view_file(entries[selected].name);
                    desktop_render();
                }
            }
        } else if (c == '\b') {
            if (exfat_change_dir("..") == 0) {
                selected = 0;
                scroll = 0;
                desktop_render();
            }
        } else if (c == 'q' || c == 'Q') {
            terminal_initialize();
            terminal_writestring("Exiting desktop, back to kernel shell.\n");
            return;
        }
    }
}
