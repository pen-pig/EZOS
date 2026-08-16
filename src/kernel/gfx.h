#if 0  // ===== gfx 图形模式已注释（保留原代码），菜单已合并到 gui 文本模式 =====
#ifndef GFX_H_INC
#define GFX_H_INC

#include "types.h"

// 运行时分辨率（由 gfx_init 根据 VBE 检测结果设置）
extern int GFX_W;
extern int GFX_H;
extern int GFX_BPP;            // 每像素位数：32/24/16/8
extern uint32_t GFX_FB;        // framebuffer 物理地址
extern int GFX_PITCH;          // 每行字节数

// VBE 结果结构（boot.asm 写入 0x5000 区域）
typedef struct {
    uint32_t fb_addr;          // framebuffer 物理地址
    uint16_t width;            // 屏幕宽度
    uint16_t height;           // 屏幕高度
    uint8_t  bpp;              // 每像素位数
    uint8_t  vbe_ok;           // 1=VBE 成功
    uint16_t pitch;            // 每行字节数
} vbe_info_t;

// 从 0x5000 读取 VBE 结果
void gfx_read_vbe_info(vbe_info_t *info);

void gfx_init(void);            // 初始化图形模式（VBE 优先，回退 VGA 0x13）
void gfx_restore_text(void);    // 恢复 80x25 文本模式
void gfx_set_palette(void);     // 设置 256 色调色板（仅 8bpp 模式）
void gfx_load_font(void);       // 加载 8x8 字体
void gfx_putpixel(int x, int y, uint32_t color);
void gfx_fill_rect(int x, int y, int w, int h, uint32_t color);
void gfx_draw_rect(int x, int y, int w, int h, uint32_t color);
void gfx_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg);
void gfx_draw_text(int x, int y, const char *s, uint32_t fg, uint32_t bg);
void gfx_clear(uint32_t color);

#endif
#endif  // ===== gfx 图形模式已注释 =====
