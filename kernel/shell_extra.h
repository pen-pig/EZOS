#ifndef SHELL_EXTRA_H
#define SHELL_EXTRA_H

#include "types.h"      /* 下面 mem_range_mapped 的声明用到 uint32_t */

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
void cmd_uptime(const char *args);
void cmd_sleep(const char *args);
void cmd_mem(const char *args);
void cmd_dmesg(const char *args);
void cmd_kmtest(const char *args);
void cmd_pagetest(const char *args);
void cmd_utest(const char *args);
void cmd_pmmtest(const char *args);
void cmd_elftest(const char *args);
void cmd_exec(const char *args);
void cmd_calc(const char *args);
void cmd_selftest(const char *args);
void cmd_ktask(const char *args);
void cmd_ps(const char *args);
void cmd_pci(const char *args);
void cmd_nic(const char *args);

/* 内核解引用"用户给的地址"前的必查项：整段区间是否都已映射。
 * 未做校验时 `mem 0xFFFFFFFF` 会在 ring0 缺页，整个内核停机。
 * shell.c 的 hexdump 与 shell_extra.c 的 mem 共用这一份实现。 */
int mem_range_mapped(uint32_t addr, uint32_t len);

/* 开机一键自检（kernel_main 调用）：静默跑全部子系统断言，
 * 返回失败的子系统数（0=全部通过）。失败项可用 shell 的
 * selftest 命令看逐项详情。 */
int boot_selftest(void);

#endif
