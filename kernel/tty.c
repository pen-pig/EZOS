#include "tty.h"
#include "port.h"        // 必须包含，提供 outb/inb

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

static uint16_t* const VGA_MEMORY = (uint16_t*) 0xB8000;

static size_t terminal_row;
static size_t terminal_column;
static uint8_t terminal_color;
static char *g_capture_buf = NULL;
static int g_capture_len = 0;
static int g_capture_max = 0;
static void (*g_gfx_hook)(const char *) = NULL;

void terminal_set_gfx_hook(void (*fn)(const char *)) {
    g_gfx_hook = fn;
}

void terminal_begin_capture(char *buf, int max) {
    g_capture_buf = buf;
    g_capture_len = 0;
    g_capture_max = max;
}

int terminal_end_capture(void) {
    int n = g_capture_len;
    g_capture_buf = NULL;
    g_capture_max = 0;
    return n;
}
static uint16_t* terminal_buffer;

#define SCROLLBACK_LINES 100
static uint16_t scrollback[SCROLLBACK_LINES][VGA_WIDTH];
static int sb_start = 0;
static int sb_count = 0;
static int view_offset = 0;
static uint16_t screen_backup[VGA_HEIGHT][VGA_WIDTH];
static int backup_valid = 0;

enum vga_color {
    VGA_COLOR_BLACK = 0,
    VGA_COLOR_BLUE = 1,
    VGA_COLOR_GREEN = 2,
    VGA_COLOR_CYAN = 3,
    VGA_COLOR_RED = 4,
    VGA_COLOR_MAGENTA = 5,
    VGA_COLOR_BROWN = 6,
    VGA_COLOR_LIGHT_GREY = 7,
    VGA_COLOR_DARK_GREY = 8,
    VGA_COLOR_LIGHT_BLUE = 9,
    VGA_COLOR_LIGHT_GREEN = 10,
    VGA_COLOR_LIGHT_CYAN = 11,
    VGA_COLOR_LIGHT_RED = 12,
    VGA_COLOR_LIGHT_MAGENTA = 13,
    VGA_COLOR_LIGHT_BROWN = 14,
    VGA_COLOR_WHITE = 15,
};

static inline uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg) {
    return (uint8_t)(fg | (bg << 4));
}

static inline uint16_t vga_entry(unsigned char uc, uint8_t color) {
    return (uint16_t)uc | ((uint16_t)color << 8);
}

void terminal_initialize(void) {
    terminal_row = 0;
    terminal_column = 0;
    terminal_color = vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    terminal_buffer = VGA_MEMORY;
    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            const size_t index = y * VGA_WIDTH + x;
            terminal_buffer[index] = vga_entry(' ', terminal_color);
        }
    }
    // 初始化光标位置
    terminal_set_cursor(0, 0);
}

void terminal_setcolor(uint8_t color) {
    terminal_color = color;
}

void terminal_putentryat(char c, uint8_t color, size_t x, size_t y) {
    const size_t index = y * VGA_WIDTH + x;
    terminal_buffer[index] = vga_entry(c, color);
}

void terminal_scroll(void) {
    // 将滚出屏幕的顶行保存到 scrollback
    for (size_t x = 0; x < VGA_WIDTH; x++) {
        scrollback[sb_start][x] = terminal_buffer[x];
    }
    sb_start = (sb_start + 1) % SCROLLBACK_LINES;
    if (sb_count < SCROLLBACK_LINES) sb_count++;

    for (size_t y = 0; y < VGA_HEIGHT - 1; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            terminal_buffer[y * VGA_WIDTH + x] = terminal_buffer[(y + 1) * VGA_WIDTH + x];
        }
    }
    for (size_t x = 0; x < VGA_WIDTH; x++) {
        terminal_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = vga_entry(' ', terminal_color);
    }
}

void terminal_putchar(char c) {
    /* 捕获模式：输出写入缓冲，不写屏幕 */
    if (g_capture_buf) {
        if (g_capture_len < g_capture_max - 1) {
            g_capture_buf[g_capture_len++] = c;
        }
        /* 捕获模式下仍更新行列状态，保证命令执行后光标位置正确 */
        if (c == '\n') {
            terminal_column = 0;
            if (terminal_row < VGA_HEIGHT - 1) terminal_row++;
        } else if (c == '\t') {
            terminal_column = (terminal_column + 4) & ~3;
            if (terminal_column >= VGA_WIDTH) { terminal_column = 0; if (terminal_row < VGA_HEIGHT - 1) terminal_row++; }
        } else {
            if (++terminal_column == VGA_WIDTH) { terminal_column = 0; if (terminal_row < VGA_HEIGHT - 1) terminal_row++; }
        }
        return;
    }
    if (c == '\n') {
        terminal_column = 0;
        if (++terminal_row == VGA_HEIGHT) {
            terminal_scroll();
            terminal_row = VGA_HEIGHT - 1;
        }
        terminal_set_cursor(terminal_row, terminal_column);
        return;
    }
    if (c == '\t') {
        terminal_column = (terminal_column + 4) & ~3;
        if (terminal_column >= VGA_WIDTH) {
            terminal_column = 0;
            if (++terminal_row == VGA_HEIGHT) {
                terminal_scroll();
                terminal_row = VGA_HEIGHT - 1;
            }
        }
        terminal_set_cursor(terminal_row, terminal_column);
        return;
    }
    if (c == '\b') {
        if (terminal_column > 0) {
            terminal_column--;
            terminal_putentryat(' ', terminal_color, terminal_column, terminal_row);
            terminal_set_cursor(terminal_row, terminal_column);
        }
        return;
    }

    terminal_putentryat(c, terminal_color, terminal_column, terminal_row);
    if (++terminal_column == VGA_WIDTH) {
        terminal_column = 0;
        if (++terminal_row == VGA_HEIGHT) {
            terminal_scroll();
            terminal_row = VGA_HEIGHT - 1;
        }
    }
    terminal_set_cursor(terminal_row, terminal_column);
}

void terminal_write(const char* data, size_t size) {
    for (size_t i = 0; i < size; i++)
        terminal_putchar(data[i]);
}

void terminal_writestring(const char* data) {
    if (g_gfx_hook) {
        g_gfx_hook(data);
        return;
    }
    for (size_t i = 0; data[i] != '\0'; i++) {
        terminal_putchar(data[i]);
    }
}

void terminal_set_cursor(size_t row, size_t col) {
    uint16_t pos = (uint16_t)(row * VGA_WIDTH + col);
    outb(0x3D4, 0x0F);
    outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
    // 更新内部变量
    terminal_row = row;
    terminal_column = col;
}

void terminal_clear_line(size_t row) {
    for (size_t col = 0; col < VGA_WIDTH; col++) {
        terminal_putentryat(' ', terminal_color, col, row);
    }
    terminal_set_cursor(row, 0);
}

size_t terminal_get_row(void) {
    return terminal_row;
}

// 将 scrollback 中「向上滚动 k 行」后的一屏内容渲染到屏幕
static void render_scrollback(int k) {
    for (int y = 0; y < VGA_HEIGHT; y++) {
        int seq = sb_count - k + y;
        if (seq < sb_count) {
            int idx = (sb_start - sb_count + seq) % SCROLLBACK_LINES;
            if (idx < 0) idx += SCROLLBACK_LINES;
            for (int x = 0; x < VGA_WIDTH; x++) {
                terminal_buffer[y * VGA_WIDTH + x] = scrollback[idx][x];
            }
        } else {
            int src_y = y - k;
            if (src_y >= 0 && src_y < VGA_HEIGHT && backup_valid) {
                for (int x = 0; x < VGA_WIDTH; x++) {
                    terminal_buffer[y * VGA_WIDTH + x] = screen_backup[src_y][x];
                }
            } else {
                for (int x = 0; x < VGA_WIDTH; x++) {
                    terminal_buffer[y * VGA_WIDTH + x] = vga_entry(' ', terminal_color);
                }
            }
        }
    }
}

void terminal_scroll_reset(void) {
    if (backup_valid) {
        for (int y = 0; y < VGA_HEIGHT; y++) {
            for (int x = 0; x < VGA_WIDTH; x++) {
                terminal_buffer[y * VGA_WIDTH + x] = screen_backup[y][x];
            }
        }
        backup_valid = 0;
    }
    view_offset = 0;
}

void terminal_scroll_up(void) {
    if (sb_count == 0) return;
    if (view_offset == 0) {
        for (int y = 0; y < VGA_HEIGHT; y++) {
            for (int x = 0; x < VGA_WIDTH; x++) {
                screen_backup[y][x] = terminal_buffer[y * VGA_WIDTH + x];
            }
        }
        backup_valid = 1;
    }
    if (view_offset < sb_count) {
        view_offset += 1;
    }
    render_scrollback(view_offset);
}

void terminal_scroll_down(void) {
    if (view_offset > 0) {
        view_offset -= 1;
        if (view_offset == 0) {
            terminal_scroll_reset();
        } else {
            render_scrollback(view_offset);
        }
    } else {
        terminal_scroll_reset();
    }
}

int terminal_in_scrollback(void) {
    return view_offset > 0;
}
