#ifndef GAMES_H
#define GAMES_H

/*
 * games.h - EZOS 游戏合集（自 sinwindows games.cpp 移植，纯 C 版）
 *
 * 入口：shell 命令 "games" 打开游戏菜单（7 个游戏）；
 *       GUI 桌面模式经 games_set_gfx + games_play 在终端窗口中运行。
 * 实现见 games.c，编译进内核（build.ninja / build_and_test.bat 已含 games.o）。
 */

void cmd_games(const char *args);

/* GUI 桌面模式适配（gfxwin.c 调用）：
 * on=1 时文本输出重定向到 24x80 缓冲，yield_fn 在阻塞输入时周期回调
 * （一般传 gw_term_yield 以驱动终端窗口重绘），on=0 恢复内核 shell 直写。 */
void games_set_gfx(int on, void (*yield_fn)(void));
char *games_gfx_buf(void);   /* GUI 模式下 24x80 文本缓冲首地址 */
int   games_gfx_pos(void);   /* GUI 模式下当前光标在缓冲中的偏移 */
void  games_play(int choice);/* 直接运行 1..7 号游戏（不经过菜单） */

/* GUI 宿主启动器（gfxwin.c 注册）：fn(0)=图形菜单，fn(1..7)=直接运行游戏。
 * games_gui_launch 供 shell 命令在图形桌面下自动走图形入口。 */
void games_set_gui_launcher(int (*fn)(int));
int  games_gui_launch(int idx);

/* GUI 图形渲染接口（gfxwin.c 调用）：返回当前游戏类型与状态，
 * 供桌面窗口把游戏画成图形界面（不再渲染字符网格）。 */
int   games_gfx_kind(void);          /* 当前游戏 1..7，未运行返回 0 */
int   games_tic_board(int i);        /* 井字棋 9 格（i=0..8） */
int   games_tic_winner(void);        /* 0=无 1/2=胜者 */
int   games_snake_len(void);
int   games_snake_alive(void);       /* 0=已结束/退出 */
int   games_snake_body_x(int i);     /* 身体第 i 节 x */
int   games_snake_body_y(int i);     /* 身体第 i 节 y */
int   games_snake_food_x(void);
int   games_snake_food_y(void);
int   games_2048_cell(int i);        /* 4x4（i=0..15） */
int   games_2048_score(void);
int   games_2048_over(void);
int   games_ms_cell(int r, int c);   /* 9x9 */
int   games_ms_revealed(int r, int c);
int   games_ms_flagged(int r, int c);
int   games_ms_cursor_x(void);
int   games_ms_cursor_y(void);
int   games_ms_lose(void);
int   games_ms_left(void);           /* 剩余未翻开非雷格数 */
int   games_mem_cell(int i);         /* 16 张牌面 */
int   games_mem_face(int i);         /* 已配对 */
int   games_mem_show(int i);         /* 当前翻开 */
int   games_mem_flips(void);
int   games_mem_cursor_x(void);
int   games_mem_cursor_y(void);
int   games_rps_pwins(void);
int   games_rps_cwins(void);
int   games_rps_draws(void);
int   games_rps_last_p(void);        /* 0=无 */
int   games_rps_last_c(void);
int   games_rps_last_d(void);        /* 0=平 1=胜 2=负 */

/* GUI 鼠标点击接口（gfxwin.c 写入，games.c 轮询消耗）：
 * games_set_mouse(a, b, btn)：btn 1=左键按下沿 2=右键按下沿 0=无，
 *   a/b 为该游戏语义坐标（棋盘行列 / 方向 0..3 / 出拳 1..3）
 * games_mouse_poll(a, b)：返回按下沿(1/2)并输出坐标，一次消费；0=无 */
void games_set_mouse(int a, int b, int btn);
int  games_mouse_poll(int *a, int *b);

#endif
