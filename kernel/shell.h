#ifndef SHELL_H
#define SHELL_H

void shell_run(void);
void shell_set_user_mode(int u); // 1=用户 shell（GUI Terminal），0=内核 shell
void system_reboot(void);        // 触发整机重启（8042 复位）
void system_shutdown(void);      // 触发整机关机（ACPI/QEMU）
void cmd_hex(const char *args);
void cmd_rand(const char *args);
void cmd_guess(const char *args);
void cmd_tictactoe(const char *args);
void cmd_snake(const char *args);


#endif
