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
void terminal_scroll_up(void);
void terminal_scroll_down(void);
void terminal_scroll_reset(void);
int  terminal_in_scrollback(void);

#endif
