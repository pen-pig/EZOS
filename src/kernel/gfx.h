#ifndef GFX_H_INC
#define GFX_H_INC

#include "types.h"

#define GFX_W 320
#define GFX_H 200

void gfx_init(void);            // 切换到 320x200x256 图形模式
void gfx_restore_text(void);    // 恢复 80x25 文本模式
void gfx_set_palette(void);     // 设置 256 色调色板
void gfx_load_font(void);       // 从 VGA 字体 ROM 读取 8x8 字体
void gfx_putpixel(int x, int y, uint8_t color);
void gfx_fill_rect(int x, int y, int w, int h, uint8_t color);
void gfx_draw_rect(int x, int y, int w, int h, uint8_t color);
void gfx_draw_char(int x, int y, char c, uint8_t fg, uint8_t bg);
void gfx_draw_text(int x, int y, const char *s, uint8_t fg, uint8_t bg);
void gfx_clear(uint8_t color);

#endif
