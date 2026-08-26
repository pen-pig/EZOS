#include "gfx.h"
#include "port.h"
#include "types.h"
#include "tty.h"
#include "keyboard.h"
#include "exfat.h"
#include "gfxwin.h"
#include "shell.h"
#include "vga_font.h"

/* runtime resolution & framebuffer (declared in gfx.h; boot.asm stores VBE LFB at 0x5000) */
int GFX_W = 320;
int GFX_H = 200;
uint8_t *gfx_fb = (uint8_t*)0xA0000;
int gfx_bpp = 1;                      /* 1=VGA 0x13, 2=VBE 16bpp */
uint16_t gfx_palette16[256];          /* ï¿½ï¿½É«ï¿½ï¿½ï¿½ï¿½ -> RGB565ï¿½ï¿½VBE 16bpp ï¿½Ã£ï¿½ */

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static inline void io_wait(void) {
    outb(0x80, 0);
}

/* VGA 8x16 ÎÄ±¾×ÖÌåµÄ±£´æ/»Ö¸´£¨plane 2£©£¬½â¾ö GUI¡úÎÄ±¾Ä£Ê½ÇĞ»»ºó×ÖÌå±»ÆÆ»µ */
static void gfx_save_font(void);
static void gfx_restore_font(void);

static uint8_t font8x8[256][8];
// ï¿½ï¿½ï¿½ï¿½ 8x8 ï¿½ï¿½ï¿½å£¨ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½Õ¸ï¿½ï¿½ï¿½ï¿½Ö¡ï¿½ï¿½ï¿½Ğ´ï¿½ï¿½Ä¸ï¿½ï¿½ï¿½ï¿½ï¿½Ã·ï¿½ï¿½Å£ï¿½
static const uint8_t builtin_font[][8] = {
    [0x20] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // space
    [0x21] = {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, // !
    [0x22] = {0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00}, // "
    [0x23] = {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00}, // #
    [0x24] = {0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00}, // $
    [0x25] = {0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00}, // %
    [0x26] = {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00}, // &
    [0x27] = {0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00}, // '
    [0x28] = {0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00}, // (
    [0x29] = {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00}, // )
    [0x2A] = {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // *
    [0x2B] = {0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00}, // +
    [0x2C] = {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06}, // ,
    [0x2D] = {0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00}, // -
    [0x2E] = {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00}, // .
    [0x2F] = {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00}, // /
    [0x30] = {0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00}, // 0
    [0x31] = {0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00}, // 1
    [0x32] = {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00}, // 2
    [0x33] = {0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00}, // 3
    [0x34] = {0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00}, // 4
    [0x35] = {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00}, // 5
    [0x36] = {0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00}, // 6
    [0x37] = {0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00}, // 7
    [0x38] = {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00}, // 8
    [0x39] = {0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00}, // 9
    [0x3A] = {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00}, // :
    [0x3B] = {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06}, // ;
    [0x3C] = {0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00}, // <
    [0x3D] = {0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00}, // =
    [0x3E] = {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00}, // >
    [0x3F] = {0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00}, // ?
    [0x40] = {0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00}, // @
    [0x41] = {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00}, // A
    [0x42] = {0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00}, // B
    [0x43] = {0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00}, // C
    [0x44] = {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00}, // D
    [0x45] = {0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00}, // E
    [0x46] = {0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00}, // F
    [0x47] = {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00}, // G
    [0x48] = {0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00}, // H
    [0x49] = {0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // I
    [0x4A] = {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00}, // J
    [0x4B] = {0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00}, // K
    [0x4C] = {0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00}, // L
    [0x4D] = {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00}, // M
    [0x4E] = {0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00}, // N
    [0x4F] = {0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00}, // O
    [0x50] = {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00}, // P
    [0x51] = {0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00}, // Q
    [0x52] = {0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00}, // R
    [0x53] = {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00}, // S
    [0x54] = {0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // T
    [0x55] = {0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00}, // U
    [0x56] = {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00}, // V
    [0x57] = {0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00}, // W
    [0x58] = {0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00}, // X
    [0x59] = {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00}, // Y
    [0x5A] = {0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00}, // Z
    [0x5B] = {0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00}, // [
    [0x5C] = {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00}, // backslash
    [0x5D] = {0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00}, // ]
    [0x5E] = {0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00}, // ^
    [0x5F] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, // _
    [0x60] = {0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00}, // `
    [0x61] = {0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00}, // a
    [0x62] = {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00}, // b
    [0x63] = {0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00}, // c
    [0x64] = {0x38,0x30,0x30,0x3E,0x33,0x33,0x6E,0x00}, // d
    [0x65] = {0x00,0x00,0x1E,0x33,0x3F,0x03,0x1E,0x00}, // e
    [0x66] = {0x1C,0x36,0x06,0x0F,0x06,0x06,0x0F,0x00}, // f
    [0x67] = {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F}, // g
    [0x68] = {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00}, // h
    [0x69] = {0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00}, // i
    [0x6A] = {0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E}, // j
    [0x6B] = {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00}, // k
    [0x6C] = {0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00}, // l
    [0x6D] = {0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00}, // m
    [0x6E] = {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00}, // n
    [0x6F] = {0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00}, // o
    [0x70] = {0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F}, // p
    [0x71] = {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78}, // q
    [0x72] = {0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00}, // r
    [0x73] = {0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00}, // s
    [0x74] = {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00}, // t
    [0x75] = {0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00}, // u
    [0x76] = {0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00}, // v
    [0x77] = {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00}, // w
    [0x78] = {0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00}, // x
    [0x79] = {0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F}, // y
    [0x7A] = {0x00,0x00,0x3F,0x18,0x0C,0x06,0x3F,0x00}, // z
    [0x7B] = {0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00}, // {
    [0x7C] = {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, // |
    [0x7D] = {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00}, // }
    [0x7E] = {0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00}, // ~
};// åˆ‡æ¢ï¿?? 320x200x256 å›¾å½¢æ¨¡å¼ï¼ˆVGA æ¨¡å¼ 0x13ï¿??
void gfx_init(void) {
    gfx_save_font();   /* VBE ÇĞ»»Ç°±£´æÎÄ±¾Ä£Ê½ 8x16 ×ÖÌå£¨plane2£©£¬¹© gfx_restore_text »¹Ô­ */
    /* boot.asm ï¿½ï¿½ï¿½ï¿½ÊµÄ£Ê½ï¿½ï¿½ï¿? VBE ï¿½ï¿½Ö±ï¿½ï¿½ï¿½Ì½ï¿½â²¢Ğ´ï¿½ï¿? 0x5000 ï¿½á¹¹ï¿½ï¿½
     *   0x5000: dword LFB ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½Ö·ï¿½ï¿½0 = ï¿½Ş¿ï¿½ï¿½ï¿½Ä£Ê½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ VGA 0x13ï¿½ï¿½
     *   0x5004: word  XRES
     *   0x5006: word  YRES
     *   0x5008: byte  BPPï¿½ï¿½16 = 16bpp RGB565ï¿½ï¿½
     * ï¿½Ë´ï¿½ï¿½ï¿½ï¿½Ù°ï¿½ hypervisor Ç¿ï¿½ï¿½ï¿½ï¿½ï¿½ã£ºQEMU TCG/Microsoft Hv ï¿½È»ï¿½ï¿½ï¿½ï¿½ï¿½
     * -vga std Êµï¿½ï¿½Ö§ï¿½ï¿½ Bochs VBEï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ boot.asm ï¿½ï¿½Ì½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½É¡ï¿½
     * ï¿½ï¿½Êµï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½Â·ï¿? VBE_DISPI ID Ì½ï¿½ï¿½Ğµï¿½ï¿½ï¿½ID ï¿½ï¿½ 0xB0C0 ï¿½Å»ï¿½ï¿½ï¿½ VGA 0x13ï¿½ï¿½ï¿½ï¿½ */
    uint32_t lfb  = *(volatile uint32_t*)0x5000;
    uint16_t vxr  = *(volatile uint16_t*)0x5004;
    uint16_t vyr  = *(volatile uint16_t*)0x5006;
    uint8_t  vbpp = *(volatile uint8_t*)0x5008;

    /* Probe VBE_DISPI ID (bochs-vbe only). If absent (e.g. real HW without
     * Bochs VBE), force VGA 0x13 fallback - writing to the LFB would be
     * invisible. */
    outw(0x1CE, 0x00);                  /* VBE_DISPI_INDEX_ID */
    {
        uint16_t _vbepid = inw(0x1CF);
        if ((_vbepid & 0xFFF0u) == 0xB0C0u
            && lfb >= 0x00100000u && lfb < 0xFFF00000u
            && vxr >= 320 && vyr >= 200 && vbpp == 16) {
            /* VBE LFB: use boot-probed resolution, 16bpp RGB565 (gfx_bpp=2).
             * QEMU std (Bochs VBE) shows vertical-stripe corruption in 8bpp
             * indexed mode, but 16bpp is stable. gfx_putpixel maps the 8-bit
             * logical color through gfx_palette16 (set by gfx_set_palette). */
            GFX_W = vxr;
            GFX_H = vyr;
            gfx_fb = (uint8_t*)(uint32_t)lfb;
            gfx_bpp = 2;
            /* activate via VBE_DISPI registers (bochs-vbe/QEMU).
             * Index map: 0=ID 1=XRES 2=YRES 3=BPP 4=ENABLE. Sequence: disable,
             * program XRES/YRES/BPP, then enable. */
            outw(0x1CE, 0x04); outw(0x1CF, 0x00);  /* VBE_DISPI_ENABLE = 0 */
            outw(0x1CE, 0x01); outw(0x1CF, vxr);   /* XRES */
            outw(0x1CE, 0x02); outw(0x1CF, vyr);   /* YRES */
            outw(0x1CE, 0x03); outw(0x1CF, 16);    /* BPP = 16 (RGB565) */
            outw(0x1CE, 0x04); outw(0x1CF, 0x01);  /* VBE_DISPI_ENABLE = 1 */
            gfx_set_palette();                     /* build RGB565 lookup table */
            gfx_load_font();
            return;
        }
    }
    /* Fallback: VGA mode 0x13 320x200x256 */
    GFX_W = 320;
    GFX_H = 200;
    gfx_fb = (uint8_t*)0xA0000;
    gfx_bpp = 1;
    outb(0x3C2, 0x63);

    // Sequencer
    outb(0x3C4, 0x00); outb(0x3C5, 0x01);
    outb(0x3C4, 0x01); outb(0x3C5, 0x00);
    outb(0x3C4, 0x02); outb(0x3C5, 0x0F);
    outb(0x3C4, 0x03); outb(0x3C5, 0x00);
    outb(0x3C4, 0x04); outb(0x3C5, 0x0E);
    io_wait();

    // CRTC
    outb(0x3D4, 0x11); outb(0x3D5, 0x0C);
    outb(0x3D4, 0x00); outb(0x3D5, 0x5F);
    outb(0x3D4, 0x01); outb(0x3D5, 0x4F);
    outb(0x3D4, 0x02); outb(0x3D5, 0x50);
    outb(0x3D4, 0x03); outb(0x3D5, 0x82);
    outb(0x3D4, 0x04); outb(0x3D5, 0x54);
    outb(0x3D4, 0x05); outb(0x3D5, 0x80);
    outb(0x3D4, 0x06); outb(0x3D5, 0x0B);
    outb(0x3D4, 0x07); outb(0x3D5, 0x3E);
    outb(0x3D4, 0x08); outb(0x3D5, 0x00);
    outb(0x3D4, 0x09); outb(0x3D5, 0x40);
    outb(0x3D4, 0x0A); outb(0x3D5, 0x00);
    outb(0x3D4, 0x0B); outb(0x3D5, 0x00);
    outb(0x3D4, 0x0C); outb(0x3D5, 0x00);
    outb(0x3D4, 0x0D); outb(0x3D5, 0x00);
    outb(0x3D4, 0x0E); outb(0x3D5, 0x00);
    outb(0x3D4, 0x0F); outb(0x3D5, 0x00);
    outb(0x3D4, 0x10); outb(0x3D5, 0xEA);
    outb(0x3D4, 0x12); outb(0x3D5, 0xDF);
    outb(0x3D4, 0x13); outb(0x3D5, 0x28);
    outb(0x3D4, 0x14); outb(0x3D5, 0x00);
    outb(0x3D4, 0x15); outb(0x3D5, 0xE7);
    outb(0x3D4, 0x16); outb(0x3D5, 0x04);
    outb(0x3D4, 0x17); outb(0x3D5, 0xE3);
    outb(0x3D4, 0x11); outb(0x3D5, 0xAC);
    io_wait();

    // Graphics Controller
    outb(0x3CE, 0x00); outb(0x3CF, 0x00);
    outb(0x3CE, 0x01); outb(0x3CF, 0x00);
    outb(0x3CE, 0x02); outb(0x3CF, 0x00);
    outb(0x3CE, 0x03); outb(0x3CF, 0x00);
    outb(0x3CE, 0x04); outb(0x3CF, 0x00);
    outb(0x3CE, 0x05); outb(0x3CF, 0x40);
    outb(0x3CE, 0x06); outb(0x3CF, 0x05);
    outb(0x3CE, 0x07); outb(0x3CF, 0x0F);
    outb(0x3CE, 0x08); outb(0x3CF, 0xFF);

    // Attribute Controller
    inb(0x3DA);
    outb(0x3C0, 0x30); outb(0x3C0, 0x41);
    outb(0x3C0, 0x31); outb(0x3C0, 0x00);
    outb(0x3C0, 0x32); outb(0x3C0, 0x0F);
    outb(0x3C0, 0x33); outb(0x3C0, 0x00);
    outb(0x3C0, 0x34); outb(0x3C0, 0x00);
    outb(0x3C0, 0x35); outb(0x3C0, 0x00);
    outb(0x3C0, 0x36); outb(0x3C0, 0x00);
    outb(0x3C0, 0x37); outb(0x3C0, 0x00);
    outb(0x3C0, 0x38); outb(0x3C0, 0x00);
    outb(0x3C0, 0x39); outb(0x3C0, 0x00);
    outb(0x3C0, 0x3A); outb(0x3C0, 0x00);
    outb(0x3C0, 0x3B); outb(0x3C0, 0x00);
    outb(0x3C0, 0x3C); outb(0x3C0, 0x00);
    outb(0x3C0, 0x3D); outb(0x3C0, 0x00);
    outb(0x3C0, 0x3E); outb(0x3C0, 0x00);
    outb(0x3C0, 0x3F); outb(0x3C0, 0x00);
    outb(0x3C0, 0x20);
    outb(0x3C4, 0x00); outb(0x3C5, 0x03);  // resume sequencer (clear sync reset)
    /* é‡ç½® VGA è°ƒè‰²æ¿ä¸ºé»˜è?¤æ–‡æœ?è‰²ï¼Œé˜²æ?¢å›¾å½¢æ¨¡å¼è°ƒè‰²æ¿æ±¡æŸ“æ–‡æœ¬æ˜¾ç¤º */
    outb(0x3C8, 0x00);
    for (int _i = 0; _i < 16; _i++) {
        static const uint8_t _pr[16] = {0,0,170,170,0,0,170,170,85,85,255,255,85,85,255,255};
        static const uint8_t _pg[16] = {0,170,0,170,0,170,0,170,85,255,85,255,85,255,85,255};
        static const uint8_t _pb[16] = {0,0,0,0,170,170,170,170,85,85,85,85,255,255,255,255};
        outb(0x3C9, _pr[_i]); outb(0x3C9, _pg[_i]); outb(0x3C9, _pb[_i]);
    }
    /* å…‰æ ‡å½¢çŠ¶æ¢å?ä¸ºæ–‡æœ¬æ¨¡å¼ */
    outb(0x3D4, 0x0A); outb(0x3D5, 0x0E);
    outb(0x3D4, 0x0B); outb(0x3D5, 0x0F);

}

// æ?ï¿??? 80x25 æ–‡æœ¬æ¨¡å¼
void gfx_dump_regs_dbg_line(int line)
{
    volatile uint8_t* vm = (volatile uint8_t*)(0xB8000 + (line & 1) * 160);
    static const char hexd[] = "0123456789ABCDEF";
    uint8_t v[48];
    int i, k = 0;
    for (i = 0; i <= 0x18; i++) { outb(0x3D4, i); v[k++] = inb(0x3D5); }
    v[k++] = inb(0x3CC);
    for (i = 0; i <= 0x04; i++) { outb(0x3C4, i); v[k++] = inb(0x3C5); }
    for (i = 0; i <= 0x08; i++) { outb(0x3CE, i); v[k++] = inb(0x3CF); }
    inb(0x3DA);
    outb(0x3C0, 0x10 | 0x20); v[k++] = inb(0x3C1);
    outb(0x3C0, 0x12 | 0x20); v[k++] = inb(0x3C1);
    outb(0x3C0, 0x20);
    for (i = 0; i < 40 && i * 4 + 3 < 160; i++) {
        vm[i*4+0] = hexd[v[i] >> 4];
        vm[i*4+1] = 0x07;
        vm[i*4+2] = hexd[v[i] & 0x0F];
        vm[i*4+3] = 0x07;
    }
}

void gfx_dump_regs_dbg(void)
{
    gfx_dump_regs_dbg_line(0);
}

/* ½øÈëÍ¼ĞÎÄ£Ê½Ç°µ÷ÓÃ£º°Ñ VGA 8x16 ÎÄ±¾×ÖÌå£¨plane 2£¬SeaBIOS ÒÑ¼ÓÔØ£©¶ÁÈëÄÚ´æ¡£
 * GUI ÆÚ¼ä 0xA0000 Æ½Ãæ±» LFB/bank Ó³Éä¸²¸Ç£¬×ÖÌåËæÖ®±»ÆÆ»µ£¬·µ»ØÎÄ±¾Ä£Ê½Ğë»¹Ô­¡£ */
static uint8_t g_vga_font[256 * 16];
static int g_vga_font_saved = 0;

static void gfx_save_font(void) {
    if (g_vga_font_saved) return;
    outb(0x3CE, 0x06); outb(0x3CF, 0x05);  /* GC6 misc: odd/even off -> ÏßĞÔ¶ÁÆ½Ãæ */
    outb(0x3CE, 0x05); outb(0x3CF, 0x00);  /* GC5: read mode 0 */
    outb(0x3CE, 0x04); outb(0x3CF, 0x02);  /* GC4: read map plane 2 */
    {
        volatile uint8_t *fp = (volatile uint8_t*)0xA0000;
        int n, j;
        for (n = 0; n < 256; n++)
            for (j = 0; j < 16; j++)
                g_vga_font[n * 16 + j] = fp[n * 32 + j];
    }
    outb(0x3CE, 0x04); outb(0x3CF, 0x00);  /* read map plane 0 */
    outb(0x3CE, 0x06); outb(0x3CF, 0x0E);  /* GC6 misc: »ØÎÄ±¾Ä£Ê½ */
    outb(0x3CE, 0x05); outb(0x3CF, 0x10);  /* GC5: read mode 1 */
    g_vga_font_saved = 1;
}

/* ÎÄ±¾Ä£Ê½»Ö¸´Ê±µ÷ÓÃ£º°Ñ±£´æµÄ 8x16 ×ÖÌåĞ´»Ø plane 2£¬ÇåµôÍ¼ĞÎ½×¶ÎĞ´ÈëµÄ²ĞÁô×ÖĞÎ¡£ */
static void gfx_restore_font(void) {
    if (!g_vga_font_saved) return;
    outb(0x3C4, 0x04); outb(0x3C5, 0x04);  /* SC4: odd/even off */
    outb(0x3C4, 0x02); outb(0x3C5, 0x04);  /* SC2: map mask plane 2 */
    outb(0x3CE, 0x05); outb(0x3CF, 0x00);  /* GC5: write mode 0 */
    outb(0x3CE, 0x06); outb(0x3CF, 0x04);  /* GC6: Í¼ĞÎÆ½ÃæĞ´Ä£Ê½ */
    {
        volatile uint8_t *fp = (volatile uint8_t*)0xA0000;
        int n, j;
        for (n = 0; n < 256; n++)
            for (j = 0; j < 16; j++)
                fp[n * 32 + j] = g_vga_font[n * 16 + j];
    }
    outb(0x3CE, 0x04); outb(0x3CF, 0x02);  /* read map plane 2 */
    outb(0x3CE, 0x05); outb(0x3CF, 0x10);  /* GC5: read mode 1 */
    outb(0x3CE, 0x06); outb(0x3CF, 0x0E);  /* GC6: ÎÄ±¾Ä£Ê½ */
    outb(0x3C4, 0x02); outb(0x3C5, 0x0F);  /* SC2: map mask È«²¿Æ½Ãæ */
    outb(0x3C4, 0x04); outb(0x3C5, 0x03);  /* SC4: odd/even£¨ÎÄ±¾Ä£Ê½£© */
}

void gfx_restore_text(void) {
    /* disable VBE (bochs-vbe) first so VGA register sequence restores text mode */
    outw(0x1CE, 0x04); outw(0x1CF, 0x00);  /* VBE_DISPI_ENABLE = 0 */
    // Misc Output: enable color, 25.175MHz, 400-line
    outb(0x3C2, 0x67);

    // Sequencer: sync reset first, then text values, then resume
    outb(0x3C4, 0x00); outb(0x3C5, 0x01);
    outb(0x3C4, 0x01); outb(0x3C5, 0x00);
    outb(0x3C4, 0x02); outb(0x3C5, 0x03);
    outb(0x3C4, 0x03); outb(0x3C5, 0x00);
    outb(0x3C4, 0x04); outb(0x3C5, 0x03);
    outb(0x3C4, 0x00); outb(0x3C5, 0x03);

    // CRTC (mode 03h full table, SeaBIOS values)
    outb(0x3D4, 0x11); outb(0x3D5, 0x0C);  // clear CRTC write-protect
    outb(0x3D4, 0x00); outb(0x3D5, 0x5F);
    outb(0x3D4, 0x01); outb(0x3D5, 0x4F);
    outb(0x3D4, 0x02); outb(0x3D5, 0x50);
    outb(0x3D4, 0x03); outb(0x3D5, 0x82);
    outb(0x3D4, 0x04); outb(0x3D5, 0x55);
    outb(0x3D4, 0x05); outb(0x3D5, 0x81);
    outb(0x3D4, 0x06); outb(0x3D5, 0xBF);   /* V Total = 191 (400 ï¿½ï¿½ï¿½Ä±ï¿½) */
    outb(0x3D4, 0x07); outb(0x3D5, 0x1F);   /* Overflow (ï¿½Ä±ï¿½Ä£Ê½) */
    outb(0x3D4, 0x08); outb(0x3D5, 0x00);
    outb(0x3D4, 0x09); outb(0x3D5, 0x4F);
    outb(0x3D4, 0x0A); outb(0x3D5, 0x0D);
    outb(0x3D4, 0x0B); outb(0x3D5, 0x0E);
    outb(0x3D4, 0x0C); outb(0x3D5, 0xC0);   /* start_addr ¸ßÎ» = 0xC0 */
    outb(0x3D4, 0x0D); outb(0x3D5, 0x00);   /* start_addr = 0xC000£¬¶ÔÆë B8000 ¶Î */
    outb(0x3D4, 0x0E); outb(0x3D5, 0x00);
    outb(0x3D4, 0x0F); outb(0x3D5, 0x00);
    outb(0x3D4, 0x10); outb(0x3D5, 0x9C);
    outb(0x3D4, 0x11); outb(0x3D5, 0x8E);   /* V Retrace End (ï¿½Ä±ï¿½Ä£Ê½) */
    outb(0x3D4, 0x12); outb(0x3D5, 0x8F);
    outb(0x3D4, 0x13); outb(0x3D5, 0x28);
    outb(0x3D4, 0x14); outb(0x3D5, 0x00);
    outb(0x3D4, 0x15); outb(0x3D5, 0x96);
    outb(0x3D4, 0x16); outb(0x3D5, 0xB9);
    outb(0x3D4, 0x17); outb(0x3D5, 0xA3);   /* Mode Control (ï¿½Ä±ï¿½Ä£Ê½) */

    // Graphics Controller (mode 03h)
    outb(0x3CE, 0x00); outb(0x3CF, 0x00);
    outb(0x3CE, 0x01); outb(0x3CF, 0x00);
    outb(0x3CE, 0x02); outb(0x3CF, 0x00);
    outb(0x3CE, 0x03); outb(0x3CF, 0x00);
    outb(0x3CE, 0x04); outb(0x3CF, 0x00);
    outb(0x3CE, 0x05); outb(0x3CF, 0x10);
    outb(0x3CE, 0x06); outb(0x3CF, 0x0E);
    outb(0x3CE, 0x07); outb(0x3CF, 0x0F);
    outb(0x3CE, 0x08); outb(0x3CF, 0xFF);

    // Attribute Controller: standard index write (no 0x20 bit)
    inb(0x3DA);
    outb(0x3C0, 0x00); outb(0x3C0, 0x00);
    outb(0x3C0, 0x01); outb(0x3C0, 0x01);
    outb(0x3C0, 0x02); outb(0x3C0, 0x02);
    outb(0x3C0, 0x03); outb(0x3C0, 0x03);
    outb(0x3C0, 0x04); outb(0x3C0, 0x04);
    outb(0x3C0, 0x05); outb(0x3C0, 0x05);
    outb(0x3C0, 0x06); outb(0x3C0, 0x06);
    outb(0x3C0, 0x07); outb(0x3C0, 0x07);
    outb(0x3C0, 0x08); outb(0x3C0, 0x08);
    outb(0x3C0, 0x09); outb(0x3C0, 0x09);
    outb(0x3C0, 0x0A); outb(0x3C0, 0x0A);
    outb(0x3C0, 0x0B); outb(0x3C0, 0x0B);
    outb(0x3C0, 0x0C); outb(0x3C0, 0x0C);
    outb(0x3C0, 0x0D); outb(0x3C0, 0x0D);
    outb(0x3C0, 0x0E); outb(0x3C0, 0x0E);
    outb(0x3C0, 0x0F); outb(0x3C0, 0x0F);
    outb(0x3C0, 0x10); outb(0x3C0, 0x08);  // Mode Control: text mode (AG=0)
    outb(0x3C0, 0x11); outb(0x3C0, 0x00);  // Overscan: black
    outb(0x3C0, 0x12); outb(0x3C0, 0x0F);  // Color Plane Enable
    outb(0x3C0, 0x13); outb(0x3C0, 0x00);  // PEL Panning
    outb(0x3C0, 0x14); outb(0x3C0, 0x00);  // Color Select
    outb(0x3C0, 0x20);                      // back to index mode

    // clear text screen (80x25) to avoid stale framebuffer
    {
        volatile uint16_t* vm = (volatile uint16_t*)0xB8000;
        int i;
        for (i = 0; i < 80 * 25; i++) vm[i] = 0x0720;
    }

    // restore DAC palette to VGA standard 16 colors (GUI may have
    // remapped 0xF0-0xFF entries via gw_win10_palette / gfx_set_palette)
    {
        static const uint8_t std_r[16] = {0,0,0,0,170,170,170,170,85,85,85,85,255,255,255,255};
        static const uint8_t std_g[16] = {0,0,170,170,0,0,170,170,85,85,255,255,85,85,255,255};
        static const uint8_t std_b[16] = {0,170,0,170,0,170,0,170,85,255,85,255,85,255,85,255};
        outb(0x3C8, 0);
        for (int j = 0; j < 256; j++) {
            uint8_t idx = (uint8_t)(j & 0x0F);
            outb(0x3C9, std_r[idx]); outb(0x3C9, std_g[idx]); outb(0x3C9, std_b[idx]);
        }
    }

    /* Reload saved VGA 8x16 font into plane 2 so text mode shows clean glyphs
     * after VBE LFB mapped over the 0xA0000 planes (removes the EXPERIMENT5
     * 0x20=0xFF debug fill that produced colored blocks). */
    gfx_restore_font();
}

void gfx_set_palette(void) {
    static const uint8_t std_r[16] = {0,0,0,0,170,170,170,170,85,85,85,85,255,255,255,255};
    static const uint8_t std_g[16] = {0,0,170,170,0,0,170,170,85,85,255,255,85,85,255,255};
    static const uint8_t std_b[16] = {0,170,0,170,0,170,0,170,85,255,85,255,85,255,85,255};
    if (gfx_bpp == 2) {
        /* VBE 16bpp: ï¿½ï¿½ RGB565 ï¿½ï¿½ï¿½Ò±ï¿½ï¿½ï¿½ï¿½ï¿½É«ï¿½ï¿½ï¿½ï¿½ -> ï¿½ï¿½ï¿? */
        for (int i = 0; i < 256; i++) {
            uint8_t r, g, b;
            if (i < 16) {
                r = std_r[i]; g = std_g[i]; b = std_b[i];
            } else if (i >= 0xF0 && i <= 0xF7) {
                /* Win10 palette (gfxwin Ô¼ï¿½ï¿½) */
                static const uint8_t wr[8]  = {0x00,0x00,0x3C,0xF3,0xE1,0xCD,0x99,0xE8};
                static const uint8_t wg[8]  = {0x78,0x5A,0x9B,0xF3,0xE1,0xCD,0x99,0x11};
                static const uint8_t wb[8]  = {0xD7,0x9E,0xE8,0xF3,0xE1,0xCD,0x99,0x23};
                r = wr[i-0xF0]; g = wg[i-0xF0]; b = wb[i-0xF0];
            } else if (i == 0xF8) { r=0x20; g=0x20; b=0x20; }  /* taskbar */
            else if (i == 0xF9) { r=0x40; g=0x40; b=0x40; }  /* taskbar hover */
            else {
                r = (uint8_t)((i * 3) & 0x3F);
                g = (uint8_t)((i * 5) & 0x3F);
                b = (uint8_t)((i * 7) & 0x3F);
            }
            gfx_palette16[i] = rgb565(r, g, b);
        }
        return;
    }
    outb(0x3C8, 0);
    for (int i = 0; i < 256; i++) {
        uint8_t r, g, b;
        if (i < 16) {
            r = std_r[i]; g = std_g[i]; b = std_b[i];
        } else if (i >= 0xF0 && i <= 0xF7) {
            /* Win10 palette (gfxwin Ô¼ï¿½ï¿½) */
            static const uint8_t wr[8]  = {0x00,0x00,0x3C,0xF3,0xE1,0xCD,0x99,0xE8};
            static const uint8_t wg[8]  = {0x78,0x5A,0x9B,0xF3,0xE1,0xCD,0x99,0x11};
            static const uint8_t wb[8]  = {0xD7,0x9E,0xE8,0xF3,0xE1,0xCD,0x99,0x23};
            r = wr[i-0xF0]; g = wg[i-0xF0]; b = wb[i-0xF0];
        } else if (i == 0xF8) { r=0x20; g=0x20; b=0x20; }  /* taskbar */
        else if (i == 0xF9) { r=0x40; g=0x40; b=0x40; }  /* taskbar hover */
        else {
            r = (uint8_t)((i * 3) & 0x3F);
            g = (uint8_t)((i * 5) & 0x3F);
            b = (uint8_t)((i * 7) & 0x3F);
        }
        outb(0x3C9, r); outb(0x3C9, g); outb(0x3C9, b);
    }
}

//// ï¿?? VGA å­—ä½“ ROM è¯»å– 8x8 å­—ä½“
//void gfx_load_font(void) {
//    outb(0x3CE, 0x04); outb(0x3CF, 0x02);
//    outb(0x3CE, 0x05); outb(0x3CF, 0x00);
//    outb(0x3CE, 0x06); outb(0x3CF, 0x04);
//    uint8_t *font = (uint8_t*)0xC0000 + 0x1F * 8192;
//    for (int i = 0; i < 256 * 8; i++) {
//        font8x8[i / 8][i % 8] = font[i];
//    }
//    outb(0x3CE, 0x04); outb(0x3CF, 0x00);
//    outb(0x3CE, 0x05); outb(0x3CF, 0x10);
//    outb(0x3CE, 0x06); outb(0x3CF, 0x0E);
//}
void gfx_load_font(void) {
        // Use built-in font (space, digits 0-9, A-Z, a-z, . - = *); no ROM read
    int n = (int)(sizeof(builtin_font) / sizeof(builtin_font[0]));
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < 8; j++) {
            font8x8[i][j] = builtin_font[i][j];
        }
    }
    // ï¿½ï¿½ï¿½ï¿½ï¿½Ö·ï¿½ï¿½ï¿½ï¿½Î?ï¿½ï¿½
    for (int i = 128; i < 256; i++) {
        for (int j = 0; j < 8; j++) {
            font8x8[i][j] = 0;
        }
    }
}
void gfx_putpixel(int x, int y, uint8_t color) {
    if (x < 0 || x >= GFX_W || y < 0 || y >= GFX_H) return;
    if (gfx_bpp == 2) {
        ((uint16_t*)gfx_fb)[y * GFX_W + x] = gfx_palette16[color];
    } else {
        gfx_fb[y * GFX_W + x] = color;
    }
}

void gfx_fill_rect(int x, int y, int w, int h, uint8_t color) {
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            gfx_putpixel(x + i, y + j, color);
        }
    }
}

void gfx_draw_rect(int x, int y, int w, int h, uint8_t color) {
    for (int i = 0; i < w; i++) {
        gfx_putpixel(x + i, y, color);
        gfx_putpixel(x + i, y + h - 1, color);
    }
    for (int j = 0; j < h; j++) {
        gfx_putpixel(x, y + j, color);
        gfx_putpixel(x + w - 1, y + j, color);
    }
}

void gfx_draw_char(int x, int y, char c, uint8_t fg, uint8_t bg) {
    uint8_t ch = (uint8_t)c;
    for (int row = 0; row < 8; row++) {
        uint8_t line = font8x8[ch][row];
        for (int col = 0; col < 8; col++) {
            gfx_putpixel(x + col, y + row, (line & (0x01 << col)) ? fg : bg);
        }
    }
}

void gfx_draw_text(int x, int y, const char *s, uint8_t fg, uint8_t bg) {
    while (*s) {
        gfx_draw_char(x, y, *s, fg, bg);
        x += 8;
        s++;
    }
}

/* ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½Å´ï¿½ï¿½ï¿½ï¿½Ö£ï¿½Ã¿ï¿½ï¿½ 8x8 ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½Ø»ï¿½ï¿½ï¿½ scale x scale ï¿½ï¿½ï¿½Ø¿é£¨scale>=1ï¿½ï¿½ï¿½ï¿½
 * ï¿½ï¿½ï¿½ÚµÍ·Ö±ï¿½ï¿½Ê£ï¿½320x200ï¿½ï¿½ï¿½ÂµÄ´ï¿½ï¿½ï¿½ï¿?/Í¼ï¿½ï¿½ï¿½ï¿½ï¿½Æ£ï¿½2x ï¿½ï¿½ 16px ï¿½ß¡ï¿½ */
void gfx_draw_text_scaled(int x, int y, const char *s, uint8_t fg, int bg, int scale) {
    if (scale < 1) scale = 1;
    while (*s) {
        uint8_t ch = (uint8_t)*s;
        for (int row = 0; row < 8; row++) {
            uint8_t line = font8x8[ch][row];
            for (int col = 0; col < 8; col++) {
                if (line & (0x01 << col)) {
                    /* Ç°ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ */
                    for (int dy = 0; dy < scale; dy++) {
                        for (int dx = 0; dx < scale; dx++) {
                            gfx_putpixel(x + col * scale + dx, y + row * scale + dy, fg);
                        }
                    }
                } else if (bg >= 0) {
                    /* ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½Ø£ï¿½bg < 0 ï¿½ï¿½Ê¾Í¸ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½Ç°ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½É?/ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½É«ï¿½ï¿½Ò»ï¿½Â²ï¿½ï¿½ï¿½ï¿½Ó±ï¿½ */
                    for (int dy = 0; dy < scale; dy++) {
                        for (int dx = 0; dx < scale; dx++) {
                            gfx_putpixel(x + col * scale + dx, y + row * scale + dy, (uint8_t)bg);
                        }
                    }
                }
            }
        }
        x += 8 * scale;
        s++;
    }
}

void gfx_clear(uint8_t color) {
    gfx_fill_rect(0, 0, GFX_W, GFX_H, color);
}
// ---- ï¿½ï¿½Í¼ï¿½ï¿½ UIï¿½ï¿½VGA 320x200x256ï¿½ï¿½----

// ï¿½ï¿½ï¿½ï¿½Ê½ï¿½ï¿½Öµï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½Ö§ï¿½ï¿½ + - * / % ï¿½ï¿½ï¿½ï¿½ï¿½Å£ï¿½
static int gfx_expr_pos;
static int gfx_expr_err;
static const char *gfx_expr_str;

static int gfx_expr_parse_expr(void);

static int gfx_expr_parse_factor(void) {
    int v;
    if (gfx_expr_str[gfx_expr_pos] == '(') {
        gfx_expr_pos++;
        v = gfx_expr_parse_expr();
        if (gfx_expr_str[gfx_expr_pos] == ')') gfx_expr_pos++;
        return v;
    }
    v = 0;
    while (gfx_expr_str[gfx_expr_pos] >= '0' && gfx_expr_str[gfx_expr_pos] <= '9') {
        v = v * 10 + (gfx_expr_str[gfx_expr_pos] - '0');
        gfx_expr_pos++;
    }
    return v;
}

static int gfx_expr_parse_term(void) {
    int v = gfx_expr_parse_factor();
    while (1) {
        char c = gfx_expr_str[gfx_expr_pos];
        if (c == '*') { gfx_expr_pos++; v *= gfx_expr_parse_factor(); }
        else if (c == '/') { gfx_expr_pos++; int d = gfx_expr_parse_factor(); if (d != 0) v /= d; else gfx_expr_err = 1; }
        else if (c == '%') { gfx_expr_pos++; int d = gfx_expr_parse_factor(); if (d != 0) v %= d; else gfx_expr_err = 1; }
        else break;
    }
    return v;
}

static int gfx_expr_parse_expr(void) {
    int v = gfx_expr_parse_term();
    while (1) {
        char c = gfx_expr_str[gfx_expr_pos];
        if (c == '+') { gfx_expr_pos++; v += gfx_expr_parse_term(); }
        else if (c == '-') { gfx_expr_pos++; v -= gfx_expr_parse_term(); }
        else break;
    }
    return v;
}

int gfx_eval(const char *s, int *ok) {
    gfx_expr_pos = 0;
    gfx_expr_err = 0;
    gfx_expr_str = s;
    int v = gfx_expr_parse_expr();
    while (gfx_expr_str[gfx_expr_pos] == ' ') gfx_expr_pos++;
    *ok = (gfx_expr_str[gfx_expr_pos] == '\0') && !gfx_expr_err;
    return v;
}

void gfx_menu(void) {
    gfx_dump_regs_dbg();   // DBG: dump BIOS text-mode registers to B8000 before switching
    gfx_init();
    gfx_set_palette();
    gfx_load_font();

    static const char *keys[] = {
        "7","8","9","/",
        "4","5","6","*",
        "1","2","3","-",
        "0","(",")","+",
        "C","=","%"," "
    };

    int running = 1;
    int page = 0;        // 0=menu 1=calc 2=viewer 3=about
    int menu_sel = 0;
    int calc_buf[32];
    int calc_len = 0;
    int calc_sel = 0;
    int calc_result = 0;
    int calc_has_result = 0;
    int calc_err = 0;

    while (running) {
        gfx_clear(0x00);

        if (page == 0) {
            gfx_draw_text(20, 8, "EZOS GUI", 0x0F, 0x00);
            gfx_draw_text(20, 20, "v0.3-gui", 0x0A, 0x00);
            gfx_draw_text(20, 36, "Select an app:", 0x07, 0x00);
            const char *items[] = { "Calculator", "Hex", "Rand", "Guess", "TicTacToe", "Snake", "Windows", "Viewer", "About", "Exit" };
            for (int i = 0; i < 10; i++) {
                int my = 48 + i * ((GFX_H >= 400) ? 22 : 15);
                if (i == menu_sel) {
                    gfx_fill_rect(20, my, (GFX_W >= 600) ? 300 : 200, 13, 0x1F);
                    gfx_draw_text(24, my + 2, items[i], 0x0F, 0x1F);
                } else {
                    gfx_draw_text(24, my + 2, items[i], 0x07, 0x00);
                }
            }
            gfx_draw_text(20, GFX_H - 10, "[W/S] move [Enter] open [Esc] quit", 0x08, 0x00);
        } else if (page == 1) {
            gfx_draw_text(20, 16, "Calculator", 0x0F, 0x00);
            gfx_draw_text(20, 32, "expr: 1+2*3  ( ) + - * / %", 0x07, 0x00);
            gfx_fill_rect(20, 48, 280, 18, 0x1F);
            gfx_draw_text(24, 50, "> ", 0x0F, 0x1F);
            for (int i = 0; i < calc_len; i++) {
                char ch[2] = { (char)calc_buf[i], '\0' };
                gfx_draw_text(24 + (i % 30) * 8, 50, ch, 0x0F, 0x1F);
            }
            if (calc_has_result) {
                char res[40];
                int rl = 0;
                int rv = calc_result;
                if (rv == 0) { res[rl++] = '0'; }
                else {
                    char tmp[16];
                    int tl = 0;
                    int neg = 0;
                    if (rv < 0) { neg = 1; rv = -rv; }
                    while (rv > 0) { tmp[tl++] = '0' + (rv % 10); rv /= 10; }
                    if (neg) res[rl++] = '-';
                    while (tl > 0) res[rl++] = tmp[--tl];
                }
                res[rl] = '\0';
                gfx_draw_text(24, 74, "= ", 0x0A, 0x00);
                gfx_draw_text(40, 74, res, 0x0A, 0x00);
            } else if (calc_err) {
                gfx_draw_text(24, 74, "= ERROR", 0x0C, 0x00);
            }
            for (int i = 0; i < 20; i++) {
                int bx = 20 + (i % 4) * 72;
                int by = 96 + (i / 4) * 18;
                if (i == calc_sel) {
                    gfx_fill_rect(bx, by, 68, 16, 0x1F);
                    gfx_draw_text(bx + 4, by + 2, keys[i], 0x0F, 0x1F);
                } else {
                    gfx_draw_rect(bx, by, 68, 16, 0x07);
                    gfx_draw_text(bx + 4, by + 2, keys[i], 0x07, 0x00);
                }
            }
            gfx_draw_text(20, GFX_H - 12, "[arrows] move  [Enter] press  [Esc] back", 0x08, 0x00);
        } else if (page == 2) {
            gfx_draw_text(20, 16, "Viewer", 0x0F, 0x00);
            gfx_draw_text(20, 32, "Shows README.TXT content:", 0x07, 0x00);
            static uint8_t vbuf[512];
            int vn = exfat_read_file("README.TXT", vbuf, 512);
            if (vn > 0) {
                int line = 0;
                int col = 0;
                for (int i = 0; i < vn && line < 8; i++) {
                    if (vbuf[i] == '\n') { line++; col = 0; }
                    else if (col < ((GFX_W >= 600) ? 60 : 30)) {
                        char ch[2] = { (char)vbuf[i], '\0' };
                        gfx_draw_text(20 + col * 8, 56 + line * 12, ch, 0x07, 0x00);
                        col++;
                    }
                }
            } else {
                gfx_draw_text(20, 56, "(no README.TXT)", 0x08, 0x00);
            }
            gfx_draw_text(20, GFX_H - 12, "[Esc] back", 0x08, 0x00);
        } else if (page == 3) {
            gfx_draw_text(20, 16, "EZOS GUI", 0x0F, 0x00);
            gfx_draw_text(20, 32, "v0.3-gui", 0x0A, 0x00);
            gfx_draw_text(20, 56, "A tiny graphical UI", 0x07, 0x00);
            gfx_draw_text(20, 72, "for EZOS kernel.", 0x07, 0x00);
            gfx_draw_text(20, 96, "Apps: Calculator, Viewer", 0x07, 0x00);
            gfx_draw_text(20, GFX_H - 12, "[Esc] back", 0x08, 0x00);
        }

        int c = 0;
        while (c == 0) c = keyboard_getchar();

        if (page == 0) {
            if (c == 'w' || c == KEY_UP) { if (menu_sel > 0) menu_sel--; }
            else if (c == 's' || c == KEY_DOWN) { if (menu_sel < 9) menu_sel++; }
            else if (c == '\n') {
                if (menu_sel == 0) { page = 1; calc_len = 0; calc_sel = 0; calc_has_result = 0; calc_err = 0; }
                else if (menu_sel == 1) { gfx_restore_text(); terminal_initialize(); cmd_hex("255"); goto gfx_back; }
                else if (menu_sel == 2) { gfx_restore_text(); terminal_initialize(); cmd_rand(""); goto gfx_back; }
                else if (menu_sel == 3) { gfx_restore_text(); terminal_initialize(); cmd_guess(""); goto gfx_back; }
                else if (menu_sel == 4) { gfx_restore_text(); terminal_initialize(); cmd_tictactoe(""); goto gfx_back; }
                else if (menu_sel == 5) { gfx_restore_text(); terminal_initialize(); cmd_snake(""); goto gfx_back; }
                else if (menu_sel == 6) { gw_demo(); goto gfx_back; }
                else if (menu_sel == 7) { page = 2; }
                else if (menu_sel == 8) { page = 3; }
                else { running = 0; }
            }
            else if (c == 27) { running = 0; }
        } else if (page == 1) {
            if (c == 27) { page = 0; }
            else if (c == KEY_UP) { if (calc_sel >= 4) calc_sel -= 4; }
            else if (c == KEY_DOWN) { if (calc_sel < 16) calc_sel += 4; }
            else if (c == KEY_LEFT) { if (calc_sel % 4 > 0) calc_sel--; }
            else if (c == KEY_RIGHT) { if (calc_sel % 4 < 3) calc_sel++; }
            else if (c == '\n') {
                const char *k = keys[calc_sel];
                if (k[0] == 'C') {
                    calc_len = 0; calc_has_result = 0; calc_err = 0;
                } else if (k[0] == '=') {
                    if (calc_len > 0) {
                        char expr[40];
                        for (int i = 0; i < calc_len; i++) expr[i] = (char)calc_buf[i];
                        expr[calc_len] = '\0';
                        int ok = 0;
                        calc_result = gfx_eval(expr, &ok);
                        calc_has_result = ok;
                        calc_err = !ok;
                    }
                } else if (k[0] != ' ') {
                    if (calc_len < 32) {
                        calc_buf[calc_len++] = k[0];
                        calc_has_result = 0;
                        calc_err = 0;
                    }
                }
            }
        } else if (page == 2) {
            if (c == 27) { page = 0; }
        } else if (page == 3) {
            if (c == 27) { page = 0; }
        }

        continue;
    gfx_back:
        terminal_writestring("\n[Press any key to return to GUI]");
        while (keyboard_getchar() == 0) {}
        gfx_init();
        gfx_set_palette();
        gfx_load_font();
    }

    gfx_restore_text();
    terminal_initialize();   // ï¿½Ş¸ï¿½ï¿½ï¿½ï¿½Ë³ï¿½Í¼ï¿½ï¿½Ä£Ê½ï¿½ï¿½ï¿½ï¿½ï¿½ï¿½Ø½ï¿½ï¿½Ä±ï¿½ï¿½Õ¶Ë£ï¿½ï¿½ï¿½Ç°ï¿½ï¿? TEMP-DBG ×¢ï¿½Íµï¿½ï¿½Âºï¿½ï¿½ï¿½ï¿½ï¿½
}

