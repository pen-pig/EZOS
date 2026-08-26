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

#endif
