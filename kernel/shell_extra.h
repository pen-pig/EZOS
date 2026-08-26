#ifndef SHELL_EXTRA_H
#define SHELL_EXTRA_H

/*
 * shell_extra.h - EZOS 命令行增强模块（借鉴 MikanOS 参数解析与命令语义）
 *
 * 由 shell.c 命令表注册的新命令实现放在 shell_extra.c，
 * 通过本头文件暴露给 shell.c 与外部调用方。
 */

/* 单命令详细帮助：cmd 匹配时返回帮助文本，否则返回 NULL */
const char *shell_extra_help(const char *cmd);

/* 命令别名查询：name 是已注册别名时返回展开串，否则返回 NULL */
const char *shell_extra_lookup_alias(const char *name);

/* 新增命令（shell.c 命令表 extern 引用） */
void cmd_ver(const char *args);
void cmd_sysinfo(const char *args);
void cmd_type(const char *args);
void cmd_which(const char *args);
void cmd_alias(const char *args);
void cmd_unalias(const char *args);
void cmd_sleep(const char *args);
void cmd_mem(const char *args);

#endif
