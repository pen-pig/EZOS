#ifndef GFXWIN_H
#define GFXWIN_H

#include "types.h"

/*
 * gfxwin.h - EZOS 窗口框架
 * 移植自 RetrOS-32 (joexbayer/RetrOS-32, MIT) 的 gfxlib/window/component/theme
 * 上层 GUI 逻辑，适配到 EZOS 现有 gfx.h/gfx.c 底层接口
 * (VBE 640x480x256 LFB / VGA mode 0x13 320x200x256)
 *
 * v0.5 Win10 主题版（UI 抄 RetrOS-32 成品窗口系统，配色改 Win10）：
 *   - 主题全部改为 Win10 风格（扁平化、标题栏 #0078D7、圆角按钮、深浅配色）
 *   - 新增 gw_win10_palette()：进入 GUI 时把 0xF0-0xF7 重映射为 Win10 色
 *   - 按钮改为扁平圆角（1px 深灰边框，按下加深 + 文字偏移）
 *   - 滚动条/窗口边框/标题栏按钮全部扁平化
 *   - 窗口移动 ghost 框改为蓝色
 *   - 演示窗口布局按 640x480 重排
 * v0.6 Win10 桌面层（参考 sinwindows UI 框架思路，整体 Win10 化）：
 *   - 桌面墙纸：渐变蓝底 + 居中四窗格徽标（640x480 / 320x200 自适应）
 *   - 任务栏：深色 #202020 底、开始按钮、任务按钮（切换/恢复/最小化）、右侧时钟
 *   - 开始菜单：左侧应用列表点击启动窗口，Esc/点击外部关闭
 *   - 鼠标完整交互：悬停高亮、任务栏切换、拖动、窗口点击
 *   - 布局全部按 GFX_W/GFX_H 动态计算，320x200 回退不破版
 * 功能保持不变：窗口创建/关闭/置顶/拖动、按钮、滚动条、鼠标/键盘事件、
 * About/Counter/List/Clock 演示、gw_start
 */

/* Win10 专用调色板索引（由 gw_win10_palette() 重映射 DAC 后使用） */
#define GW_C_BLUE        0xF0  /* #0078D7 标题栏蓝（活动） */
#define GW_C_BLUE_DARK   0xF1  /* #005A9E 深蓝（桌面/按下态） */
#define GW_C_BLUE_LIGHT  0xF2  /* #3C9BE8 亮蓝（强调/ghost） */
#define GW_C_GRAY_BG     0xF3  /* #F3F3F3 浅灰（浅色内容底） */
#define GW_C_GRAY_BORDER 0xF4  /* #E1E1E1 窗口边框/滚动条轨道 */
#define GW_C_GRAY_SLIDER 0xF5  /* #CDCDCD 滑块/次按钮面 */
#define GW_C_GRAY_TITLE  0xF6  /* #999999 非活动标题栏 */
#define GW_C_CLOSE_RED   0xF7  /* #E81123 关闭按钮红 */
#define GW_C_TASKBAR     0xF8  /* #202020 任务栏/开始菜单底 */
#define GW_C_TASKBAR_HL  0xF9  /* #404040 任务按钮悬停/激活 */

#define GW_MAX_WINDOWS 8
#define GW_TITLE_MAX   20
#define GW_TITLE_H     12
#define GW_BORDER      8

#define GW_F_MOVABLE   0x01   /* 标题栏可拖动 */
#define GW_F_NO_CLOSE  0x02   /* 无关闭按钮 */
#define GW_F_NO_MIN    0x04   /* 无最小化按钮 */

typedef struct gw_window gw_window_t;

typedef void (*gw_draw_fn)(gw_window_t *w);          /* 内容区绘制回调回调 */
typedef void (*gw_key_fn)(gw_window_t *w, int key);  /* 键盘回调 */
typedef void (*gw_click_fn)(gw_window_t *w, int lx, int ly); /* 内容区点击回调（逻辑坐标） */
typedef void (*gw_mouse_fn)(gw_window_t *w, int lx, int ly); /* 内容区按下回调（逻辑坐标） */

struct gw_window {
    int used;
    char title[GW_TITLE_MAX];
    int x, y;               /* 外框左上角 */
    int w, h;               /* 外框尺寸 */
    int inner_w, inner_h;   /* 内容区尺寸 */
    int focused;
    int hidden;
    int flags;
    int moving;             /* 拖动中 */
    int move_dx, move_dy;   /* 拖动时鼠标偏移 */
    int ghost_x, ghost_y;   /* 移动动画 ghost 虚线框位置 */
    int minimized;          /* 最小化（隐藏）标记，点击桌面空白恢复 */
    gw_draw_fn draw;
    gw_key_fn key;
    gw_click_fn click;
    gw_mouse_fn mousedown;
    void *user;
};

/* 初始化 */
void gw_init(void);
void gw_win10_palette(void);   /* 重映射 0xF0-0xF7 为 Win10 色（进入 GUI 前调用） */
void gw_set_theme(int index);
int gw_theme_count(void);
const char *gw_theme_name(int index);

/* 窗口管理 */
gw_window_t *gw_create(const char *title, int x, int y, int w, int h, int flags);
void gw_close(gw_window_t *w);
void gw_minimize(gw_window_t *w);
void gw_restore(gw_window_t *w);
gw_window_t *gw_focused(void);

/* 内容区原点（屏幕坐标） */
int gw_ox(gw_window_t *w);
int gw_oy(gw_window_t *w);

/* 每帧绘制与输入 */
void gw_draw_all(void);
void gw_handle_mouse(int mx, int my, int buttons);
void gw_handle_key(int key);

/* 控件 */
void gw_button(int x, int y, int w, int h, const char *label, uint8_t color);
void gw_button_ex(int x, int y, int w, int h, const char *label, uint8_t color, int pressed);
void gw_scrollbar(int x, int y, int h, int thumb_y, int thumb_h, uint8_t color);

/* 演示程序（shell 调用） */
void gw_demo(void);
void gw_start(void);
int  gw_launch_gui_game(int idx); /* shell command GUI entry: 0=menu, 1..7=run */           /* 初始化图形模式并进入窗口 GUI （shell wins 命令调用） */

/* Win10 桌面层 */
void gw_draw_desktop(void);            /* 墙纸：渐变蓝底 + 居中徽标 */
void gw_draw_taskbar(void);            /* 任务栏：开始按钮 + 任务按钮 + 时钟 */
void gw_draw_start_menu(void);         /* 开始菜单：应用列表 */
int  gw_handle_desktop_mouse(int mx, int my, int buttons); /* 桌面层鼠标，返回 1=已处理 */
int  gw_start_menu_open(void);         /* 开始菜单是否打开 */
void gw_launch_app(int idx);           /* 启动/恢复应用窗口 */

#endif
