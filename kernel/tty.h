#ifndef TTY_H
#define TTY_H

#include "types.h"

void terminal_initialize(void);
void terminal_putchar(char c);
void terminal_write(const char* data, size_t size);
void terminal_writestring(const char* data);
void terminal_setcolor(uint8_t color);
void terminal_set_cursor(size_t row, size_t col);
void terminal_clear_line(size_t row);
size_t terminal_get_row(void);
size_t terminal_get_column(void);

/* 终端尺寸：shell 需要它算"输入行会折到第几行"。写死在 shell.c 里的话，
 * 改终端尺寸就会静默错位——这里是唯一事实来源。 */
#define TERM_WIDTH   80
#define TERM_HEIGHT  25
void terminal_scroll_up(void);
void terminal_scroll_down(void);
void terminal_scroll_reset(void);
int  terminal_in_scrollback(void);


/* 输出捕获模式：重定向/管道使用 */
void terminal_begin_capture(char *buf, int max);
int terminal_end_capture(void);

/* GUI 模式输出重定向：注册后 terminal_writestring 不再写 VGA 文本缓冲 */
void terminal_set_gfx_hook(void (*fn)(const char *));

#endif
