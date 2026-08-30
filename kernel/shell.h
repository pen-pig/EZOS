#ifndef SHELL_H
#define SHELL_H

#include "types.h"

void shell_run(void);
void shell_exit_clear(void);     // 清除 exit 标志（GUI 退出回 shell 前调用，防残留立即退出）
void system_reboot(void);        // 触发整机重启（8042 复位）
void system_shutdown(void);      // 触发整机关机（ACPI/QEMU）
void cmd_hex(const char *args);
void cmd_rand(const char *args);
void cmd_guess(const char *args);
void cmd_tictactoe(const char *args);
void cmd_snake(const char *args);

/* PC 蜂鸣器：freq Hz 鸣响 ms 毫秒（PIT ch2，供 shell/GUI/游戏音效共用） */
void beep(uint32_t freq, uint32_t ms);

/* 命令名前缀补全：唯一匹配写 out 返回 1；多个匹配填 matches[] 返回总数；
 * 无匹配返回 0。内核 shell 与 GUI 终端 Tab 补全共用 */
int shell_complete_command(const char *prefix, char *out, int outsz,
                           const char *matches[], int max_matches);

#endif
