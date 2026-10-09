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

/* 把内部行游标直接设到 row（并把列归零）。
 * shell 折行重绘时用它对齐"我要从这一行开始画"：滚屏只搬了显存内容，
 * 不动 terminal_row，不显式对齐的话下一次 putchar 会画到已经被滚走的行号上。 */
void terminal_set_row(size_t row);

/* 整体上滚 n 行（调 n 次 terminal_scroll）。
 * 输入折行折到屏底时需要它——真实终端就是这样：继续输入，屏幕上滚。 */
void terminal_scroll_n(uint32_t n);

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
