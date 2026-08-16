#ifndef GUI_H
#define GUI_H

void gui_start(void);

/* 由 shell.c 提供的文本命令，供 gui 文本菜单调用（gfx 图形模式已注释，菜单合并到 gui） */
void cmd_calc(const char *args);
void cmd_hex(const char *args);
void cmd_rand(const char *args);
void cmd_guess(const char *args);
void cmd_tictactoe(const char *args);
void cmd_snake(const char *args);

#endif
