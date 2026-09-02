#ifndef GFX_H_INC
#define GFX_H_INC

#include "types.h"

/* 运行时屏幕分辨率：VBE 多分辨率 LFB（boot.asm 探测存入 0x5000）或回退 VGA 0x13 320x200 */
extern int GFX_W;
extern int GFX_H;
/* 帧缓冲基址：VBE LFB（boot.asm 存入 0x5000 结构首字段）或 0xA0000 */
extern uint8_t *gfx_fb;
/* 每像素字节数：1=VGA 0x13 8bpp，2=VBE 16bpp RGB565 */
extern int gfx_bpp;
/* 颜色索引 -> RGB565 查找表（VBE 16bpp 模式使用；8bpp 走 DAC 调色板） */
extern uint16_t gfx_palette16[256];

/* boot.asm 在实模式 VBE 探测后写入 0x5000 的结构：
 *   0x5000: dword LFB 物理地址（0 = 无可用模式，内核回退 VGA 0x13）
 *   0x5004: word  XRES
 *   0x5006: word  YRES
 *   0x5008: byte  BPP（16 = 16bpp RGB565） */

void gfx_init(void);            // 初始化图形模式（优先 VBE 多分辨率 LFB，失败回退 VGA 0x13 320x200）
void gfx_restore_text(void);    // 恢复 80x25 文本模式
void gfx_text_font_init(void);  // 启动时把内置 8x16 字体写入 VGA plane 2，统一文本模式字体
void gfx_set_palette(void);     // 设置 256 色调色板
void gfx_load_font(void);       // 从 VGA 字体 ROM 读取 8x8 字体
void gfx_putpixel(int x, int y, uint8_t color);
void gfx_fill_rect(int x, int y, int w, int h, uint8_t color);
void gfx_draw_rect(int x, int y, int w, int h, uint8_t color);
void gfx_draw_char(int x, int y, char c, uint8_t fg, uint8_t bg);
void gfx_draw_text(int x, int y, const char *s, uint8_t fg, uint8_t bg);
void gfx_draw_text_scaled(int x, int y, const char *s, uint8_t fg, int bg, int scale);
void gfx_clear(uint8_t color);
void gfx_menu(void);          // 图形界面菜单（原 shell.c cmd_gfx迁移）
int gfx_eval(const char *s, int *ok); // 表达式求值器（图形计算器与 shell calc 共用）

#endif
