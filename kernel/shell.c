#include "shell.h"
#include "shell_extra.h"
#include "tty.h"
#include "keyboard.h"
#include "types.h"
#include "port.h"
#include "ata.h"
#include "exfat.h"
#include "gui.h"
#include "mouse.h"
#include "gfx.h"
#include "gfxwin.h"

#define CMD_BUFFER_SIZE 128
#define HISTORY_SIZE 8

static char cmd_buffer[CMD_BUFFER_SIZE];
static int cmd_pos = 0;
static char history[HISTORY_SIZE][CMD_BUFFER_SIZE];
static int history_count = 0;

static int history_index = -1;      // 当前浏览的历史位置，-1 表示新输入行
static size_t cursor = 0;           // 光标在当前输入行的位置
static size_t current_row = 0;      // 当前提示符所在行号
static int shell_exit_flag = 0;     // exit 命令置位，shell_run 据此返回
/* 诊断：shell 循环每轮采样 EFLAGS（data 段全局，供 QEMU monitor 读内存验证） */
volatile uint32_t dbg_shell_eflags = 0;

// 字符串长度
static size_t my_strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

// 忽略大小写比较
static int my_tolower(char c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static int my_strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        char ca = my_tolower(*a);
        char cb = my_tolower(*b);
        if (ca != cb) return ca - cb;
        a++;
        b++;
    }
    return *a - *b;
}

// 简单 atoi
static int my_atoi(const char *s) {
    int result = 0;
    while (*s >= '0' && *s <= '9') {
        result = result * 10 + (*s - '0');
        s++;
    }
    return result;
}

// 十六进制字符转值
static int hex_char_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 解析十六进制字符串为数字
static uint32_t my_htoi(const char *s) {
    uint32_t result = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    while (*s) {
        int v = hex_char_val(*s);
        if (v < 0) break;
        result = (result << 4) | v;
        s++;
    }
    return result;
}

// 64 位 / 32 位无符号除法（避免依赖 libgcc 的 __udivdi3）
// 入参 hi:lo 组成 64 位被除数，d 为除数，返回商
static uint32_t udiv64_32(uint32_t hi, uint32_t lo, uint32_t d) {
    if (d == 0) return 0;
    if (hi >= d) return 0xFFFFFFFF;   // 商溢出保护
    uint64_t rem = hi;
    uint32_t q = 0;
    for (int i = 31; i >= 0; i--) {
        rem = (rem << 1) | ((lo >> i) & 1);
        if (rem >= d) {
            rem -= d;
            q |= (1u << i);
        }
    }
    return q;
}

// 打印十进制
static void print_dec(uint32_t num) {
    char buf[16];
    int len = 0;
    if (num == 0) {
        terminal_putchar('0');
        return;
    }
    while (num > 0) {
        buf[len++] = '0' + (num % 10);
        num /= 10;
    }
    while (len > 0) terminal_putchar(buf[--len]);
}

// 打印十六进制字节
static void print_hex_byte(uint8_t val) {
    char hex[] = "0123456789ABCDEF";
    terminal_putchar(hex[val >> 4]);
    terminal_putchar(hex[val & 0x0F]);
}

// 打印十六进制32位
static void print_hex32(uint32_t val) {
    print_hex_byte((val >> 24) & 0xFF);
    print_hex_byte((val >> 16) & 0xFF);
    print_hex_byte((val >> 8) & 0xFF);
    print_hex_byte(val & 0xFF);
}

// CMOS 读一个字节
static uint8_t cmos_read(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

/* 命令实现 */
static void cmd_help(const char *args);
static void cmd_clear(const char *args);
static void cmd_exit(const char *args);
static void cmd_echo(const char *args);
static void cmd_version(const char *args);
static void cmd_time(const char *args);
static void cmd_date(const char *args);
static void cmd_reboot(const char *args);
static void cmd_shutdown(const char *args);
static void cmd_meminfo(const char *args);
static void cmd_cpuid(const char *args);
static void cmd_readdisk(const char *args);
static void cmd_hexdump(const char *args);
static void cmd_beep(const char *args);
static void cmd_about(const char *args);
static void cmd_history(const char *args);
static void cmd_setcolor(const char *args);
static void cmd_ls(const char *args);
static void cmd_cat(const char *args);
static void cmd_write(const char *args);
static void cmd_rm(const char *args);
static void cmd_format(const char *args);
static void cmd_setdrive(const char *args);
static void cmd_grep(const char *args);
static void cmd_wc(const char *args);
static void cmd_head(const char *args);
static void cmd_tail(const char *args);
static void cmd_touch(const char *args);
static void cmd_cp(const char *args);
static void cmd_mv(const char *args);
static void cmd_cd(const char *args);
static void cmd_mkdir(const char *args);
static void cmd_pwd(const char *args);
static void cmd_gui(const char *args);
static void cmd_desktop(const char *args);
static void cmd_uname(const char *args);
static void cmd_vi(const char *args);
static void cmd_df(const char *args);
static void cmd_du(const char *args);
static void cmd_calc(const char *args);
void cmd_hex(const char *args);
void cmd_rand(const char *args);
void cmd_guess(const char *args);
void cmd_tictactoe(const char *args);
void cmd_snake(const char *args);
void cmd_games(const char *args);

typedef struct {
    const char *name;
    void (*func)(const char *args);
} command_t;

static const command_t commands[] = {
    {"help",     cmd_help},
    {"exit",     cmd_exit},
    {"clear",    cmd_clear},
    {"cls",      cmd_clear},
    {"echo",     cmd_echo},
    {"version",  cmd_version},
    {"time",     cmd_time},
    {"date",     cmd_date},
    {"reboot",   cmd_reboot},
    {"shutdown", cmd_shutdown},
    {"meminfo",  cmd_meminfo},
    {"cpuid",    cmd_cpuid},
    {"readdisk", cmd_readdisk},
    {"hexdump",  cmd_hexdump},
    {"beep",     cmd_beep},
    {"about",    cmd_about},
    {"history",  cmd_history},
    {"setcolor", cmd_setcolor},
    {"ls",       cmd_ls},
    {"cat",      cmd_cat},
    {"write",    cmd_write},
    {"rm",       cmd_rm},
    {"format",   cmd_format},
    {"setdrive", cmd_setdrive},
    {"grep",     cmd_grep},
    {"wc",       cmd_wc},
    {"head",     cmd_head},
    {"tail",     cmd_tail},
    {"touch",    cmd_touch},
    {"cp",       cmd_cp},
    {"mv",       cmd_mv},
    {"cd",       cmd_cd},
    {"mkdir",    cmd_mkdir},
    {"pwd",      cmd_pwd},
    {"gui",      cmd_gui},
    {"desktop",  cmd_desktop},
    {"uname",    cmd_uname},
    {"vi",       cmd_vi},
    {"df",       cmd_df},
    {"du",       cmd_du},
    {"calc",     cmd_calc},
    {"hex",      cmd_hex},
    {"rand",     cmd_rand},
    {"guess",    cmd_guess},
    {"tictactoe",cmd_tictactoe},
    {"snake",    cmd_snake},
    {"games",    cmd_games},
    /* 新增：shell_extra.c 增强命令（借鉴 MikanOS 参数解析与语义） */
    {"ver",      cmd_ver},
    {"sysinfo",  cmd_sysinfo},
    {"type",     cmd_type},
    {"which",    cmd_which},
    {"alias",    cmd_alias},
    {"unalias",  cmd_unalias},
    {"sleep",    cmd_sleep},
    {"mem",      cmd_mem},
    {0, 0}
};

/* ===== 管道与重定向支持 ===== */
#define PIPE_BUF_SIZE 4096
static char pipe_buffer[PIPE_BUF_SIZE];
static int pipe_len = 0;

/* ===== 终端颜色辅助（tty 的 vga_color 枚举在 tty.c 内 static，shell.c 直接调 terminal_setcolor）===== */
#define CLR_WHITE     15
#define CLR_LIGHT_GREY  7
#define CLR_DARK_GREY   8
#define CLR_LIGHT_BLUE  9
#define CLR_LIGHT_GREEN 10
#define CLR_LIGHT_CYAN  11
#define CLR_LIGHT_RED   12
#define CLR_BLACK       0

static void shell_fg(uint8_t fg) { terminal_setcolor((uint8_t)(fg | (0 << 4))); }
static void shell_color_default(void) { shell_fg(CLR_LIGHT_GREY); }

/* 检查扩展名是否为可执行 */
static int is_exe_name(const char *name) {
    int len = 0; while (name[len]) len++;
    if (len < 4) return 0;
    return (name[len-4]=='.' && (name[len-3]=='e'||name[len-3]=='E') && (name[len-2]=='x'||name[len-2]=='X') && (name[len-1]=='e'||name[len-1]=='E'))
        || (name[len-4]=='.' && (name[len-3]=='c'||name[len-3]=='C') && (name[len-2]=='o'||name[len-2]=='O') && (name[len-1]=='m'||name[len-1]=='M'))
        || (name[len-4]=='.' && (name[len-3]=='b'||name[len-3]=='B') && (name[len-2]=='i'||name[len-2]=='I') && (name[len-1]=='n'||name[len-1]=='N'));
}

/* 执行原始命令（不含重定向/管道预处理） */
static void shell_execute_raw(char *cmd);

/* shell 模式：1=用户 shell（GUI Terminal / User Shell），0=内核 shell */
static int shell_user_mode = 0;
void shell_set_user_mode(int u) { shell_user_mode = u ? 1 : 0; }

/* 执行带重定向/管道的命令 */
static void shell_execute(char *cmd) {
    if (history_count < HISTORY_SIZE) {
        for (int i = 0; i < CMD_BUFFER_SIZE; i++) {
            history[history_count][i] = cmd[i];
            if (cmd[i] == '\0') break;
        }
        history_count++;
    }
    while (*cmd == ' ') cmd++;
    if (*cmd == '\0') return;

    /* 查找重定向 >、>> 和管道 |（首字符非命令符号，忽略引号内） */
    int in_quote = 0;
    char *gt = NULL, *pipe = NULL;
    for (char *p = cmd; *p; p++) {
        if (*p == '"') in_quote = !in_quote;
        if (in_quote) continue;
        if (*p == '|') { pipe = p; break; }
        if (*p == '>') { gt = p; break; }
    }

    if (gt) {
        /* cmd > file 或 cmd >> file */
        int append = (gt[0] != '\0' && gt[1] == '>');
        *gt = '\0';
        char *fname = gt + 1 + (append ? 1 : 0);
        while (*fname == ' ') fname++;
        if (append) {
            /* >> 追加：先读旧文件再合并 */
            static uint8_t old[4096];
            static uint8_t merged[8192];
            int mn = 0;
            if (exfat_init() == 0) {
                int on = exfat_read_file(fname, old, 4096);
                if (on > 0) for (int i = 0; i < on && mn < 8191; i++) merged[mn++] = old[i];
            }
            static char cap[PIPE_BUF_SIZE];
            terminal_begin_capture(cap, PIPE_BUF_SIZE);
            shell_execute_raw(cmd);
            int n = terminal_end_capture();
            for (int i = 0; i < n && mn < 8191; i++) merged[mn++] = (uint8_t)cap[i];
            if (mn > 0 && exfat_init() == 0) {
                exfat_create_file(fname, merged, mn);
            }
        } else {
            static char cap[PIPE_BUF_SIZE];
            terminal_begin_capture(cap, PIPE_BUF_SIZE);
            shell_execute_raw(cmd);
            int n = terminal_end_capture();
            if (n > 0 && exfat_init() == 0) {
                exfat_create_file(fname, (const uint8_t*)cap, n);
            }
        }
        return;
    }

    if (pipe) {
        /* cmd1 | cmd2 */
        *pipe = '\0';
        char *left = cmd;
        char *right = pipe + 1;
        while (*right == ' ') right++;
        static char pbuf[PIPE_BUF_SIZE];
        terminal_begin_capture(pbuf, PIPE_BUF_SIZE);
        shell_execute_raw(left);
        pipe_len = terminal_end_capture();
        if (pipe_len > PIPE_BUF_SIZE) pipe_len = PIPE_BUF_SIZE - 1;
        for (int i = 0; i < pipe_len; i++) pipe_buffer[i] = pbuf[i];
        shell_execute_raw(right);
        pipe_len = 0;
        return;
    }

    shell_execute_raw(cmd);
}

static void shell_execute_raw(char *cmd) {
    if (history_count < HISTORY_SIZE) {
        for (int i = 0; i < CMD_BUFFER_SIZE; i++) {
            history[history_count][i] = cmd[i];
            if (cmd[i] == '\0') break;
        }
        history_count++;
    }

    while (*cmd == ' ') cmd++;
    if (*cmd == '\0') return;

    char *space = cmd;
    while (*space && *space != ' ') space++;
    if (*space == ' ') {
        *space = '\0';
        space++;
    } else {
        space = cmd + my_strlen(cmd);
    }

    /* 内核 shell 不提供用户态命令（gui/desktop/games 等），
     * User Shell（shell_user_mode=1）才可访问 */
    if (!shell_user_mode) {
        static const char *const user_only_cmds[] = {
            "gui", "desktop", "games", "guess", "tictactoe", "snake"
        };
        for (size_t ui = 0; ui < sizeof(user_only_cmds) / sizeof(user_only_cmds[0]); ui++) {
            if (my_strcasecmp(cmd, user_only_cmds[ui]) == 0) {
                terminal_writestring("Unknown command: ");
                terminal_writestring(cmd);
                terminal_writestring("\n");
                return;
            }
        }
    }

    for (int i = 0; commands[i].name != 0; i++) {
        if (my_strcasecmp(cmd, commands[i].name) == 0) {
            commands[i].func(space);
            return;
        }
    }

    /* 别名展开（借鉴 POSIX shell 语义）：alias name=cmd 定义的别名在此生效 */
    const char *alias_exp = shell_extra_lookup_alias(cmd);
    if (alias_exp) {
        static int alias_depth = 0;
        if (alias_depth < 4) {
            char expanded[CMD_BUFFER_SIZE];
            int n = 0;
            while (alias_exp[n] && n < CMD_BUFFER_SIZE - 1) {
                expanded[n] = alias_exp[n];
                n++;
            }
            expanded[n] = '\0';
            alias_depth++;
            shell_execute(expanded);
            alias_depth--;
            return;
        }
        terminal_writestring("alias: recursion too deep: ");
        terminal_writestring(cmd);
        terminal_writestring("\n");
        return;
    }

    terminal_writestring("Unknown command: ");
    terminal_writestring(cmd);
    terminal_writestring("\n");
}

/* 供 shell_extra.c 的 type/which 查询命令是否为内建命令 */
int shell_is_builtin(const char *name) {
    for (int i = 0; commands[i].name != 0; i++) {
        if (my_strcasecmp(name, commands[i].name) == 0) return 1;
    }
    return 0;
}

// 重绘当前输入行
static void shell_redraw_line(void) {
    terminal_clear_line(current_row);
    terminal_writestring("> ");
    for (int i = 0; i < cmd_pos; i++) {
        terminal_putchar(cmd_buffer[i]);
    }
    size_t col = 2 + cursor;
    terminal_set_cursor(current_row, col);
}

void shell_run(void) {
    if (shell_user_mode)
        terminal_writestring("EZOS User Shell - Type 'help' for commands.\n");
    else
        terminal_writestring("EZOS Kernel Shell - Type 'help' for commands,'exit' to continue boot.\n");
    terminal_writestring("> ");

    cmd_pos = 0;
    cursor = 0;
    history_index = -1;
    current_row = terminal_get_row();

    while (1) {
        asm volatile("sti; nop; nop; nop; nop; nop");   /* 确保中断开启（跨过 STI 抑制窗口） */
        { uint32_t fl; asm volatile("pushfl; popl %0" : "=r"(fl)); dbg_shell_eflags = fl; }
        if (shell_exit_flag) {
            shell_exit_flag = 0;
            return;
        }
        int c = keyboard_getchar();
        if (c == 0) continue;

        if (c == KEY_PGUP) {
            terminal_scroll_up();
            continue;
        }
        if (c == KEY_PGDN) {
            terminal_scroll_down();
            continue;
        }
        if (terminal_in_scrollback()) {
            terminal_scroll_reset();
            shell_redraw_line();
        }

        if (c == '\n') {
            terminal_putchar('\n');
            cmd_buffer[cmd_pos] = '\0';
            shell_execute(cmd_buffer);
            cmd_pos = 0;
            cursor = 0;
            history_index = -1;
            terminal_writestring("> ");
            current_row = terminal_get_row();
        } else if (c == '\b') {
            if (cursor > 0) {
                // 删除光标前字符
                for (int i = cursor - 1; i < cmd_pos - 1; i++) {
                    cmd_buffer[i] = cmd_buffer[i + 1];
                }
                cmd_pos--;
                cursor--;
                shell_redraw_line();
            }
        } else if (c == KEY_LEFT) {
            if (cursor > 0) {
                cursor--;
                shell_redraw_line();
            }
        } else if (c == KEY_RIGHT) {
            if (cursor < (size_t)cmd_pos) {
                cursor++;
                shell_redraw_line();
            }
        } else if (c == KEY_UP) {
            if (history_count > 0 && history_index < history_count - 1) {
                history_index++;
                int hist_pos = history_count - 1 - history_index;
                for (int i = 0; i < CMD_BUFFER_SIZE; i++) {
                    cmd_buffer[i] = history[hist_pos][i];
                    if (cmd_buffer[i] == '\0') break;
                }
                cmd_pos = my_strlen(cmd_buffer);
                cursor = cmd_pos;
                shell_redraw_line();
            }
        } else if (c == KEY_DOWN) {
            if (history_index > 0) {
                history_index--;
                int hist_pos = history_count - 1 - history_index;
                for (int i = 0; i < CMD_BUFFER_SIZE; i++) {
                    cmd_buffer[i] = history[hist_pos][i];
                    if (cmd_buffer[i] == '\0') break;
                }
                cmd_pos = my_strlen(cmd_buffer);
                cursor = cmd_pos;
                shell_redraw_line();
            } else if (history_index == 0) {
                history_index = -1;
                cmd_pos = 0;
                cursor = 0;
                shell_redraw_line();
            }
        } else if (c >= 32 && c <= 126) {
            if (cmd_pos < CMD_BUFFER_SIZE - 1) {
                // 在 cursor 位置插入字符
                for (int i = cmd_pos; i > (int)cursor; i--) {
                    cmd_buffer[i] = cmd_buffer[i - 1];
                }
                cmd_buffer[cursor] = (char)c;
                cmd_pos++;
                cursor++;
                shell_redraw_line();
            }
        }
    }
}

/* 命令具体实现 */
static void cmd_help(const char *args) {
    if (*args != '\0') {
        const char *detail = shell_extra_help(args);
        if (detail) {
            terminal_writestring(detail);
            terminal_putchar('\n');
            return;
        }
        terminal_writestring("No detailed help for '");
        terminal_writestring(args);
        terminal_writestring("'. Use 'help' for the full command list.\n");
        return;
    }
    terminal_writestring("Available commands:\n");
    terminal_writestring("  help       - show this help\n");
    terminal_writestring("  clear/cls  - clear screen\n");
    terminal_writestring("  echo <txt> - print text\n");
    terminal_writestring("  version    - show version\n");
    terminal_writestring("  time       - show current time\n");
    terminal_writestring("  date       - show current date\n");
    terminal_writestring("  reboot     - reboot system\n");
    terminal_writestring("  shutdown   - shutdown (QEMU exits)\n");
    terminal_writestring("  meminfo    - show memory info\n");
    terminal_writestring("  cpuid      - show CPU vendor\n");
    terminal_writestring("  readdisk [drive] <lba> - read and hexdump disk sector\n");
    terminal_writestring("  hexdump <hexaddr> - dump memory\n");
    terminal_writestring("  beep       - make a beep sound\n");
    terminal_writestring("  about      - about this OS\n");
    terminal_writestring("  history    - show command history\n");
    terminal_writestring("  setcolor <fg> [bg] - set text color (0-15)\n");
    terminal_writestring("  setdrive <0|1> - set exFAT drive\n");
    terminal_writestring("  ls         - list root directory (exFAT)\n");
    terminal_writestring("  cat <file> - read file content (exFAT)\n");
    terminal_writestring("  write <file> <content> - create file with content\n");
    terminal_writestring("  rm <file>  - delete file\n");
    terminal_writestring("  format     - format slave disk as exFAT\n");
    terminal_writestring("  grep <pattern> <file> - print lines containing pattern\n");
    terminal_writestring("  wc <file>  - count lines/words/characters\n");
    terminal_writestring("  head <file> [n] - show first n lines (default 10)\n");
    terminal_writestring("  tail <file> [n] - show last n lines (default 10)\n");
    terminal_writestring("  touch <file> - create empty file\n");
    terminal_writestring("  cp <src> <dst> - copy file\n");
    terminal_writestring("  mv <src> <dst> - move/rename file\n");
    terminal_writestring("  cd <dir>   - change directory\n");
    terminal_writestring("  mkdir <dir> - create directory\n");
    terminal_writestring("  pwd        - print working directory\n");
    if (shell_user_mode) {
        terminal_writestring("  desktop    - launch graphical desktop\n");
        terminal_writestring("  gui        - launch text-mode GUI\n");
    }
    terminal_writestring("  uname      - print system information\n");
    terminal_writestring("  vi <file>  - edit a text file\n");
    terminal_writestring("  df         - show exFAT disk space usage\n");
    terminal_writestring("  du [name]  - show disk usage of current dir or file\n");
    terminal_writestring("  calc <expr> - evaluate expression (e.g. calc 1+2*3)\n");
    terminal_writestring("  hex <num>  - convert decimal <-> hex (0x.. for hex input)\n");
    terminal_writestring("  rand [max] - generate a random number (default 0-99)\n");
    if (shell_user_mode) {
        terminal_writestring("  guess      - guess-the-number game\n");
        terminal_writestring("  tictactoe  - play tic-tac-toe vs AI\n");
        terminal_writestring("  snake      - play snake game (arrows, P pause, Esc quit)\n");
        terminal_writestring("  games      - list/launch games\n");
    }
    terminal_writestring("  ver        - show kernel version\n");
    terminal_writestring("  sysinfo    - show system summary (CPU/memory/time)\n");
    terminal_writestring("  type <cmd> - show command type (builtin/alias/file)\n");
    terminal_writestring("  which <cmd> - locate a command\n");
    terminal_writestring("  alias [name=cmd] - list or define command aliases\n");
    terminal_writestring("  unalias <name> - remove a command alias\n");
    terminal_writestring("  sleep <ms> - busy-wait delay (approx)\n");
    terminal_writestring("  mem <hexaddr> [len] - dump physical memory\n");
    terminal_writestring("  help <cmd> - detailed help for a command\n");
}

static void cmd_clear(const char *args) {
    (void)args;
    terminal_initialize();
}

static void cmd_exit(const char *args) {
    (void)args;
    shell_exit_flag = 1;
}

static void cmd_echo(const char *args) {
    int no_newline = 0;
    if (args[0] == '-' && args[1] == 'n' && (args[2] == ' ' || args[2] == '\0')) {
        no_newline = 1;
        args += 2;
        while (*args == ' ') args++;
    }
    while (*args) {
        if (*args == '\\' && args[1]) {
            args++;
            if (*args == 'n') {
                terminal_putchar('\n');
            } else if (*args == 't') {
                terminal_putchar('\t');
            } else if (*args == '\\') {
                terminal_putchar('\\');
            } else {
                terminal_putchar('\\');
                terminal_putchar(*args);
            }
            args++;
        } else {
            terminal_putchar(*args);
            args++;
        }
    }
    if (!no_newline) terminal_putchar('\n');
}

static void cmd_version(const char *args) {
    (void)args;
    terminal_writestring("EZOS v0.3-gui\n");
}

static void cmd_time(const char *args) {
    (void)args;
    uint8_t hour = cmos_read(0x04);
    uint8_t minute = cmos_read(0x02);
    uint8_t second = cmos_read(0x00);
    hour = ((hour & 0x0F) + ((hour >> 4) * 10));
    minute = ((minute & 0x0F) + ((minute >> 4) * 10));
    second = ((second & 0x0F) + ((second >> 4) * 10));
    if (hour > 23) hour = 0;          /* clamp invalid CMOS values */
    if (minute > 59) minute = 0;
    if (second > 59) second = 0;
    terminal_writestring("Current time: ");
    print_dec(hour);
    terminal_putchar(':');
    if (minute < 10) terminal_putchar('0');
    print_dec(minute);
    terminal_putchar(':');
    if (second < 10) terminal_putchar('0');
    print_dec(second);
    terminal_putchar('\n');
}

static void cmd_date(const char *args) {
    (void)args;
    uint8_t year = cmos_read(0x09);
    uint8_t month = cmos_read(0x08);
    uint8_t day = cmos_read(0x07);
    year = ((year & 0x0F) + ((year >> 4) * 10));
    month = ((month & 0x0F) + ((month >> 4) * 10));
    day = ((day & 0x0F) + ((day >> 4) * 10));
    if (month < 1 || month > 12) month = 1;   /* clamp invalid CMOS values */
    if (day < 1 || day > 31) day = 1;
    terminal_writestring("Current date: ");
    print_dec(year + 2000);
    terminal_putchar('-');
    if (month < 10) terminal_putchar('0');
    print_dec(month);
    terminal_putchar('-');
    if (day < 10) terminal_putchar('0');
    print_dec(day);
    terminal_putchar('\n');
}

static void cmd_reboot(const char *args) {
    (void)args;
    system_reboot();
}

void system_reboot(void) {
    terminal_writestring("Rebooting...\n");
    for (int i = 0; i < 100000; i++) asm volatile("nop");
    outb(0x64, 0xFE);
    asm volatile("int3");
}

void system_shutdown(void) {
    terminal_writestring("Shutting down...\n");
    outw(0x604, 0x2000);
    asm volatile("cli; hlt");
}

static void cmd_shutdown(const char *args) {
    (void)args;
    system_shutdown();
}

static void cmd_meminfo(const char *args) {
    (void)args;
    uint8_t low = cmos_read(0x15);
    uint8_t high = cmos_read(0x16);
    uint16_t mem_kb = (uint16_t)((high << 8) | low);
    terminal_writestring("Base memory: ");
    print_dec(mem_kb);
    terminal_writestring(" KB\n");

    uint8_t ext_low = cmos_read(0x17);
    uint8_t ext_high = cmos_read(0x18);
    uint16_t ext_kb = (uint16_t)((ext_high << 8) | ext_low);
    terminal_writestring("Extended memory: ");
    print_dec(ext_kb);
    terminal_writestring(" KB\n");
    terminal_writestring("Total memory: ");
    print_dec(mem_kb + ext_kb);
    terminal_writestring(" KB\n");
}

static void cmd_cpuid(const char *args) {
    (void)args;
    uint32_t eax, ebx, ecx, edx;
    char vendor[13];
    asm volatile("cpuid"
                 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(0));
    ((uint32_t*)vendor)[0] = ebx;
    ((uint32_t*)vendor)[1] = edx;
    ((uint32_t*)vendor)[2] = ecx;
    vendor[12] = '\0';
    terminal_writestring("CPU Vendor: ");
    terminal_writestring(vendor);
    terminal_putchar('\n');
}

static void cmd_readdisk(const char *args) {
    int drive = 0;
    int lba = 0;

    int first = my_atoi(args);
    const char *p = args;
    while (*p >= '0' && *p <= '9') p++;
    while (*p == ' ') p++;

    if (*p == '\0') {
        lba = first;
    } else {
        drive = first;
        lba = my_atoi(p);
    }

    uint8_t buffer[512];
    if (ata_read_sector((uint8_t)drive, (uint32_t)lba, buffer) != 0) {
        terminal_writestring("Disk read error!\n");
        return;
    }
    terminal_writestring("Sector ");
    print_dec(lba);
    terminal_writestring(" (512 bytes, drive ");
    print_dec(drive);
    terminal_writestring("):\n");
    for (int i = 0; i < 512; i += 16) {
        print_hex32(i);
        terminal_writestring("  ");
        for (int j = 0; j < 16; j++) {
            print_hex_byte(buffer[i + j]);
            terminal_putchar(' ');
        }
        terminal_putchar(' ');
        for (int j = 0; j < 16; j++) {
            char c = buffer[i + j];
            if (c >= 32 && c <= 126) terminal_putchar(c);
            else terminal_putchar('.');
        }
        terminal_putchar('\n');
    }
}

static void cmd_hexdump(const char *args) {
    uint32_t addr = my_htoi(args);
    int len = 64;
    /* 可选长度参数：hexdump <addr> [len]，最大 1024 */
    const char *p = args;
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    if (*p) {
        len = my_atoi(p);
        if (len <= 0) len = 64;
        if (len > 1024) len = 1024;
    }
    terminal_writestring("Hexdump at 0x");
    print_hex32(addr);
    terminal_writestring(":\n");
    for (int i = 0; i < len; i++) {
        if (i % 16 == 0) {
            print_hex32(addr + i);
            terminal_writestring("  ");
        }
        uint8_t val = *(volatile uint8_t *)(addr + i);
        print_hex_byte(val);
        terminal_putchar(' ');
        if (i % 16 == 15) terminal_putchar('\n');
    }
    if (len % 16 != 0) terminal_putchar('\n');
}

static void cmd_beep(const char *args) {
    (void)args;
    outb(0x43, 0xB6);
    uint16_t div = 1193180 / 1000;
    outb(0x42, (uint8_t)(div & 0xFF));
    outb(0x42, (uint8_t)(div >> 8));
    uint8_t tmp = inb(0x61);
    if (tmp != (tmp | 3)) {
        outb(0x61, tmp | 3);
    }
    for (volatile int i = 0; i < 1000000; i++);
    tmp = inb(0x61) & 0xFC;
    outb(0x61, tmp);
}

static void cmd_about(const char *args) {
    (void)args;
    terminal_writestring("EZOS v0.3-gui - A hobby operating system\n");
    terminal_writestring("Written in C and Assembly\n");
}

static void cmd_history(const char *args) {
    if (args[0] == '-' && args[1] == 'c') {
        history_count = 0;
        terminal_writestring("History cleared.\n");
        return;
    }
    for (int i = 0; i < history_count; i++) {
        print_dec(i + 1);
        terminal_writestring(": ");
        terminal_writestring(history[i]);
        terminal_putchar('\n');
    }
}

static void cmd_setcolor(const char *args) {
    int fg = -1, bg = -1;
    if (*args) {
        fg = my_atoi(args);
        while (*args && *args != ' ') args++;
        if (*args == ' ') {
            args++;
            bg = my_atoi(args);
        }
    }
    if (fg < 0 || fg > 15 || bg < 0 || bg > 15) {
        terminal_writestring("Usage: setcolor <fg 0-15> [bg 0-15]\n");
        return;
    }
    terminal_setcolor((uint8_t)(fg | (bg << 4)));
    terminal_writestring("Color set.\n");
}

static void cmd_ls(const char *args) {
    int verbose = 0;
    if (args[0] == '-' && args[1] == 'l') verbose = 1;
    if (exfat_init() != 0) {
        terminal_writestring("exFAT init failed. Is disk formatted?\n");
        return;
    }
    terminal_writestring(exfat_cwd_path());
    terminal_writestring(":\n");
    exfat_dir_entry_t entries[64];
    int n = exfat_read_dir(entries, 64);
    if (n < 0) {
        terminal_writestring("read dir failed\n");
        return;
    }
    if (verbose) {
        /* -l 详细模式：类型 + 右对齐大小 + 名称（借鉴 MikanOS ListAllEntries） */
        for (int i = 0; i < n; i++) {
            if (entries[i].is_dir) shell_fg(CLR_LIGHT_BLUE);
            else if (is_exe_name(entries[i].name)) shell_fg(CLR_LIGHT_GREEN);
            else shell_fg(CLR_LIGHT_GREY);
            terminal_putchar(entries[i].is_dir ? 'd' : '-');
            terminal_putchar(' ');
            uint32_t sz = entries[i].size;
            char buf[16];
            int blen = 0;
            if (sz == 0) buf[blen++] = '0';
            while (sz > 0) { buf[blen++] = '0' + (sz % 10); sz /= 10; }
            shell_fg(CLR_LIGHT_GREY);
            for (int pad = blen; pad < 10; pad++) terminal_putchar(' ');
            while (blen > 0) terminal_putchar(buf[--blen]);
            terminal_putchar(' ');
            if (entries[i].is_dir) shell_fg(CLR_LIGHT_BLUE);
            else if (is_exe_name(entries[i].name)) shell_fg(CLR_LIGHT_GREEN);
            else shell_fg(CLR_LIGHT_GREY);
            terminal_writestring(entries[i].name);
            if (entries[i].is_dir) terminal_putchar('/');
            shell_color_default();
            terminal_putchar('\n');
        }
    } else {
        /* eza 风格彩色网格 */
        int maxlen = 4;
        for (int i = 0; i < n; i++) {
            int l = 0; while (entries[i].name[l]) l++;
            if (l > maxlen) maxlen = l;
        }
        int cols = 80 / (maxlen + 3);
        if (cols < 1) cols = 1;
        int rows = (n + cols - 1) / cols;
        if (rows < 1) rows = 1;
        char namebuf[64];
        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < cols; c++) {
                int idx = c * rows + r;
                if (idx >= n) break;
                if (entries[idx].is_dir) shell_fg(CLR_LIGHT_BLUE);
                else if (is_exe_name(entries[idx].name)) shell_fg(CLR_LIGHT_GREEN);
                else shell_fg(CLR_LIGHT_GREY);
                int l = 0; while (entries[idx].name[l] && l < 60) { namebuf[l] = entries[idx].name[l]; l++; }
                namebuf[l] = '\0';
                for (int k = 0; k < l; k++) terminal_putchar(namebuf[k]);
                if (entries[idx].is_dir) terminal_putchar('/');
                shell_color_default();
                /* 填充到列宽 */
                int pad = l + (entries[idx].is_dir ? 1 : 0);
                for (int fill = pad; fill < maxlen + 3; fill++) terminal_putchar(' ');
            }
            terminal_putchar('\n');
        }
    }
}

static void cmd_cat(const char *args) {
    if (exfat_init() != 0) {
        terminal_writestring("exFAT init failed.\n");
        return;
    }
    char filename[128];
    int i = 0;
    while (*args && *args != ' ' && i < 127) {
        filename[i++] = *args++;
    }
    filename[i] = '\0';
    if (i == 0) {
        if (pipe_len > 0) {
            for (int j = 0; j < pipe_len; j++) terminal_putchar(pipe_buffer[j]);
            terminal_putchar('\n');
            return;
        }
        terminal_writestring("Usage: cat <filename>\n");
        return;
    }
    static uint8_t file_buffer[4096];
    int bytes = exfat_read_file(filename, file_buffer, 4096);
    if (bytes < 0) {
        terminal_writestring("File not found or read error.\n");
    } else {
        for (int j = 0; j < bytes; j++) {
            char c = (char)file_buffer[j];
            if (c == '\n' || c == '\t' || c == '\r' || (c >= 32 && c <= 126)) {
                terminal_putchar(c);
            } else {
                terminal_putchar('.');
            }
        }
        terminal_putchar('\n');
    }
}

static void cmd_write(const char *args) {
    char filename[128];
    int i = 0;
    while (*args && *args != ' ' && i < 127) {
        filename[i++] = *args++;
    }
    filename[i] = '\0';
    if (*args == ' ') args++;
    if (i == 0 || *args == '\0') {
        terminal_writestring("Usage: write <filename> <content>\n");
        return;
    }

    const char *content = args;
    uint32_t len = 0;
    while (content[len]) len++;
    if (len > 512) len = 512;

    if (exfat_create_file(filename, (const uint8_t*)content, len) != 0) {
        terminal_writestring("Failed to write file.\n");
    } else {
        terminal_writestring("File written successfully.\n");
    }
}

static void cmd_rm(const char *args) {
    if (exfat_delete_file(args) != 0) {
        terminal_writestring("Failed to delete file.\n");
    } else {
        terminal_writestring("File deleted.\n");
    }
}

static void cmd_format(const char *args) {
    (void)args;
    if (exfat_format() != 0) {
        terminal_writestring("Format failed.\n");
    } else {
        terminal_writestring("Disk formatted as exFAT.\n");
        // exfat_format 成功时已设置 exfat_ready = 1，无需重复设置
    }
}

static void cmd_setdrive(const char *args) {
    int drive = my_atoi(args);
    if (drive < 0 || drive > 1) {
        terminal_writestring("Usage: setdrive <0|1>\n");
        return;
    }
    exfat_set_drive((uint8_t)drive);
    terminal_writestring("exFAT drive set to ");
    print_dec(drive);
    terminal_writestring("\n");
}

/* ============ 新增 Linux 风格命令（自实现） ============ */

// 解析下一个空格分隔的 token，写入 out，返回剩余参数指针
static const char *parse_token(const char *args, char *out, int max) {
    int i = 0;
    while (*args == ' ') args++;
    while (*args && *args != ' ' && i < max - 1) out[i++] = *args++;
    out[i] = '\0';
    while (*args == ' ') args++;
    return args;
}

static void cmd_grep(const char *args) {
    char pattern[128];
    char filename[128];
    const char *p = parse_token(args, pattern, 128);
    parse_token(p, filename, 128);
    if (pattern[0] == '\0') {
        terminal_writestring("Usage: grep <pattern> [file]\n");
        return;
    }
    if (filename[0] == '\0' && pipe_len > 0) {
        /* 从管道输入搜索 */
        int plen = my_strlen(pattern);
        int line_start = 0, line_end = 0;
        while (line_end < pipe_len) {
            while (line_end < pipe_len && pipe_buffer[line_end] != '\n') line_end++;
            int found = 0;
            for (int s = line_start; s + plen <= line_end; s++) {
                int k = 0;
                while (k < plen && pipe_buffer[s + k] == pattern[k]) k++;
                if (k == plen) { found = 1; break; }
            }
            if (found) {
                for (int j = line_start; j < line_end; j++) terminal_putchar(pipe_buffer[j]);
                terminal_putchar('\n');
            }
            line_end++;
            line_start = line_end;
        }
        return;
    }
    static uint8_t file_buffer[4096];
    int bytes = exfat_read_file(filename, file_buffer, 4096);
    if (bytes < 0) {
        terminal_writestring("File not found or read error.\n");
        return;
    }
    int plen = my_strlen(pattern);
    int line_start = 0, line_end = 0;
    while (line_end < bytes) {
        while (line_end < bytes && file_buffer[line_end] != '\n') line_end++;
        int found = 0;
        for (int s = line_start; s + plen <= line_end; s++) {
            int k = 0;
            while (k < plen && file_buffer[s + k] == pattern[k]) k++;
            if (k == plen) { found = 1; break; }
        }
        if (found) {
            for (int j = line_start; j < line_end; j++) terminal_putchar((char)file_buffer[j]);
            terminal_putchar('\n');
        }
        line_end++;
        line_start = line_end;
    }
}

static void cmd_wc(const char *args) {
    char filename[128];
    parse_token(args, filename, 128);
    if (filename[0] == '\0') {
        if (pipe_len > 0) {
            int lines = 0, words = 0, chars = 0;
            int in_word = 0;
            for (int i = 0; i < pipe_len; i++) {
                char c = pipe_buffer[i];
                chars++;
                if (c == '\n') { lines++; in_word = 0; }
                else if (c == ' ' || c == '\t' || c == '\r') { in_word = 0; }
                else if (!in_word) { words++; in_word = 1; }
            }
            print_dec(lines); terminal_putchar(' ');
            print_dec(words); terminal_putchar(' ');
            print_dec(chars); terminal_putchar(' ');
            terminal_writestring("(pipe)\n");
            return;
        }
        terminal_writestring("Usage: wc <file>\n");
        return;
    }
    static uint8_t file_buffer[4096];
    int bytes = exfat_read_file(filename, file_buffer, 4096);
    if (bytes < 0) {
        terminal_writestring("File not found or read error.\n");
        return;
    }
    int lines = 0, words = 0, chars = 0;
    int in_word = 0;
    for (int i = 0; i < bytes; i++) {
        char c = (char)file_buffer[i];
        chars++;
        if (c == '\n') { lines++; in_word = 0; }
        else if (c == ' ' || c == '\t' || c == '\r') { in_word = 0; }
        else if (!in_word) { words++; in_word = 1; }
    }
    print_dec(lines);
    terminal_putchar(' ');
    print_dec(words);
    terminal_putchar(' ');
    print_dec(chars);
    terminal_putchar(' ');
    terminal_writestring(filename);
    terminal_putchar('\n');
}

static void cmd_head(const char *args) {
    char filename[128];
    char numstr[32];
    const char *p = parse_token(args, filename, 128);
    parse_token(p, numstr, 32);
    if (filename[0] == '\0') {
        terminal_writestring("Usage: head <file> [n]\n");
        return;
    }
    int n = numstr[0] ? my_atoi(numstr) : 10;
    static uint8_t file_buffer[4096];
    int bytes = exfat_read_file(filename, file_buffer, 4096);
    if (bytes < 0) {
        terminal_writestring("File not found or read error.\n");
        return;
    }
    int shown = 0, line_start = 0, line_end = 0;
    while (line_end < bytes && shown < n) {
        while (line_end < bytes && file_buffer[line_end] != '\n') line_end++;
        for (int j = line_start; j < line_end; j++) terminal_putchar((char)file_buffer[j]);
        terminal_putchar('\n');
        shown++;
        line_end++;
        line_start = line_end;
    }
}

static void cmd_tail(const char *args) {
    char filename[128];
    char numstr[32];
    const char *p = parse_token(args, filename, 128);
    parse_token(p, numstr, 32);
    if (filename[0] == '\0') {
        terminal_writestring("Usage: tail <file> [n]\n");
        return;
    }
    int n = numstr[0] ? my_atoi(numstr) : 10;
    static uint8_t file_buffer[4096];
    int bytes = exfat_read_file(filename, file_buffer, 4096);
    if (bytes < 0) {
        terminal_writestring("File not found or read error.\n");
        return;
    }
    int total_lines = 0;
    for (int i = 0; i < bytes; i++) if (file_buffer[i] == '\n') total_lines++;
    int start_line = total_lines > n ? total_lines - n : 0;
    int cur_line = 0, line_start = 0;
    for (int pos = 0; pos <= bytes; pos++) {
        if (pos == bytes || file_buffer[pos] == '\n') {
            if (cur_line >= start_line) {
                for (int j = line_start; j < pos; j++) terminal_putchar((char)file_buffer[j]);
                terminal_putchar('\n');
            }
            cur_line++;
            line_start = pos + 1;
        }
    }
}

static void cmd_touch(const char *args) {
    char filename[128];
    parse_token(args, filename, 128);
    if (filename[0] == '\0') {
        terminal_writestring("Usage: touch <file>\n");
        return;
    }
    if (exfat_create_file(filename, 0, 0) != 0) {
        terminal_writestring("Failed to create file.\n");
    } else {
        terminal_writestring("File created.\n");
    }
}

static void cmd_cp(const char *args) {
    char src[128], dst[128];
    const char *p = parse_token(args, src, 128);
    parse_token(p, dst, 128);
    if (src[0] == '\0' || dst[0] == '\0') {
        terminal_writestring("Usage: cp <src> <dst>\n");
        return;
    }
    static uint8_t file_buffer[4096];
    int bytes = exfat_read_file(src, file_buffer, 4096);
    if (bytes < 0) {
        terminal_writestring("Source file not found.\n");
        return;
    }
    if (exfat_create_file(dst, file_buffer, (uint32_t)bytes) != 0) {
        terminal_writestring("Copy failed.\n");
    } else {
        terminal_writestring("Copied.\n");
    }
}

static void cmd_mv(const char *args) {
    char src[128], dst[128];
    const char *p = parse_token(args, src, 128);
    parse_token(p, dst, 128);
    if (src[0] == '\0' || dst[0] == '\0') {
        terminal_writestring("Usage: mv <src> <dst>\n");
        return;
    }
    static uint8_t file_buffer[4096];
    int bytes = exfat_read_file(src, file_buffer, 4096);
    if (bytes < 0) {
        terminal_writestring("Source file not found.\n");
        return;
    }
    if (exfat_create_file(dst, file_buffer, (uint32_t)bytes) != 0) {
        terminal_writestring("Move failed.\n");
        return;
    }
    if (exfat_delete_file(src) != 0) {
        terminal_writestring("Moved, but failed to delete source.\n");
    } else {
        terminal_writestring("Moved.\n");
    }
}

static void cmd_cd(const char *args) {
    char dir[128];
    parse_token(args, dir, 128);
    if (dir[0] == '\0') {
        terminal_writestring("Usage: cd <dir>\n");
        return;
    }
    if (exfat_init() != 0) {
        terminal_writestring("exFAT init failed. Is disk formatted?\n");
        return;
    }
    if (exfat_change_dir(dir) != 0) {
        terminal_writestring("cd: no such directory\n");
    }
}

static void cmd_mkdir(const char *args) {
    char dir[128];
    parse_token(args, dir, 128);
    if (dir[0] == '\0') {
        terminal_writestring("Usage: mkdir <dir>\n");
        return;
    }
    if (exfat_init() != 0) {
        terminal_writestring("exFAT init failed. Is disk formatted?\n");
        return;
    }
    if (exfat_mkdir(dir) != 0) {
        terminal_writestring("mkdir: failed\n");
    }
}

static void cmd_pwd(const char *args) {
    (void)args;
    terminal_writestring(exfat_cwd_path());
    terminal_putchar('\n');
}

static void cmd_gui(const char *args) {
    (void)args;
    gui_start();
    terminal_initialize();
}

/* User Shell 的 desktop 命令：进入图形桌面（与 kernel_main 用户态阶段一致） */
static void cmd_desktop(const char *args) {
    (void)args;
    terminal_initialize();
    gw_start();
    terminal_initialize();
}


#define VI_MAX_LINES 100
#define VI_MAX_LINE 96
#define VI_SCREEN_ROWS 24
#define VI_SCREEN_COLS 80

static char vi_lines[VI_MAX_LINES][VI_MAX_LINE];
static int vi_len[VI_MAX_LINES];
static int vi_count = 0;
static int vi_row = 0;
static int vi_col = 0;
static int vi_top = 0;
static int vi_left = 0;
static char vi_filename[128];
static int vi_modified = 0;
static int vi_mode = 0;
static const char *vi_msg = NULL;

// 确保光标在合法范围
static void vi_clamp_cursor(void) {
    if (vi_count == 0) { vi_row = 0; vi_col = 0; return; }
    if (vi_row < 0) vi_row = 0;
    if (vi_row >= vi_count) vi_row = vi_count - 1;
    if (vi_col < 0) vi_col = 0;
    if (vi_col > vi_len[vi_row]) vi_col = vi_len[vi_row];
}

// 调整滚动偏移，确保光标可见
static void vi_scroll_into_view(void) {
    if (vi_row < vi_top) vi_top = vi_row;
    if (vi_row >= vi_top + VI_SCREEN_ROWS) vi_top = vi_row - VI_SCREEN_ROWS + 1;
    if (vi_col < vi_left) vi_left = vi_col;
    if (vi_col >= vi_left + VI_SCREEN_COLS - 2) vi_left = vi_col - (VI_SCREEN_COLS - 2);
    if (vi_left < 0) vi_left = 0;
    if (vi_top < 0) vi_top = 0;
}

// 渲染界面
static void vi_render(void) {
    terminal_initialize();
    for (int i = 0; i < VI_SCREEN_ROWS; i++) {
        int line = vi_top + i;
        if (line < vi_count) {
            terminal_set_cursor((size_t)i, 0);
            int L = vi_len[line];
            for (int x = 0; x < VI_SCREEN_COLS - 1; x++) {
                int src = vi_left + x;
                terminal_putchar(src < L ? vi_lines[line][src] : ' ');
            }
        }
    }
    terminal_set_cursor(VI_SCREEN_ROWS, 0);
    terminal_setcolor(0x70);
    if (vi_msg != NULL) {
        terminal_writestring(vi_msg);
        vi_msg = NULL;
    } else {
        for (int k = 0; k < 24 && vi_filename[k]; k++) terminal_putchar(vi_filename[k]);
        terminal_putchar(' ');
        if (vi_mode == 1) terminal_writestring("-- INSERT --");
        else terminal_writestring("-- NORMAL --");
        terminal_putchar(' ');
        print_dec((uint32_t)(vi_row + 1));
        terminal_putchar(',');
        print_dec((uint32_t)(vi_col + 1));
        if (vi_modified) terminal_writestring(" [+]");
    }
    terminal_setcolor(0x07);
    terminal_set_cursor((size_t)(vi_row - vi_top), (size_t)(vi_col - vi_left));
}

// 在光标处插入字符
static void vi_insert_char(char c) {
    if (vi_len[vi_row] >= VI_MAX_LINE - 1) return;
    for (int i = vi_len[vi_row]; i > vi_col; i--) {
        vi_lines[vi_row][i] = vi_lines[vi_row][i - 1];
    }
    vi_lines[vi_row][vi_col] = c;
    vi_len[vi_row]++;
    vi_col++;
    vi_modified = 1;
}

// 删除光标处字符
static void vi_delete_char(void) {
    if (vi_col >= vi_len[vi_row]) return;
    for (int i = vi_col; i < vi_len[vi_row] - 1; i++) {
        vi_lines[vi_row][i] = vi_lines[vi_row][i + 1];
    }
    vi_len[vi_row]--;
    vi_modified = 1;
}

// 删除光标前字符（行首则合并到上一行）
static void vi_backspace(void) {
    if (vi_col > 0) {
        vi_col--;
        vi_delete_char();
    } else if (vi_row > 0) {
        int prev = vi_len[vi_row - 1];
        if (prev + vi_len[vi_row] < VI_MAX_LINE) {
            for (int i = 0; i < vi_len[vi_row]; i++) {
                vi_lines[vi_row - 1][prev + i] = vi_lines[vi_row][i];
            }
            vi_len[vi_row - 1] = prev + vi_len[vi_row];
            for (int r = vi_row; r < vi_count - 1; r++) {
                vi_len[r] = vi_len[r + 1];
                for (int m = 0; m < VI_MAX_LINE; m++) vi_lines[r][m] = vi_lines[r + 1][m];
            }
            vi_count--;
            vi_row--;
            vi_col = prev;
            vi_modified = 1;
        }
    }
}

// 在光标处拆行
static void vi_insert_newline(void) {
    if (vi_count >= VI_MAX_LINES) return;
    int tail = vi_len[vi_row] - vi_col;
    for (int r = vi_count; r > vi_row + 1; r--) {
        vi_len[r] = vi_len[r - 1];
        for (int m = 0; m < VI_MAX_LINE; m++) vi_lines[r][m] = vi_lines[r - 1][m];
    }
    vi_count++;
    for (int m = 0; m < tail; m++) {
        vi_lines[vi_row + 1][m] = vi_lines[vi_row][vi_col + m];
    }
    vi_len[vi_row + 1] = tail;
    vi_lines[vi_row][vi_col] = '\0';
    vi_len[vi_row] = vi_col;
    vi_row++;
    vi_col = 0;
    vi_modified = 1;
}

// 删除当前行
static void vi_delete_line(void) {
    if (vi_count == 0) return;
    for (int r = vi_row; r < vi_count - 1; r++) {
        vi_len[r] = vi_len[r + 1];
        for (int m = 0; m < VI_MAX_LINE; m++) vi_lines[r][m] = vi_lines[r + 1][m];
    }
    vi_count--;
    if (vi_count == 0) {
        vi_count = 1;
        vi_len[0] = 0;
        vi_lines[0][0] = '\0';
        vi_row = 0;
        vi_col = 0;
    } else if (vi_row >= vi_count) {
        vi_row = vi_count - 1;
    }
    vi_modified = 1;
}

// 保存到文件
static void vi_save(void) {
    static uint8_t out_buf[4096];
    int o = 0;
    for (int k = 0; k < vi_count && o < 4095; k++) {
        int L = vi_len[k];
        for (int m = 0; m < L && o < 4095; m++) out_buf[o++] = (uint8_t)vi_lines[k][m];
        if (o < 4095) out_buf[o++] = '\n';
    }
    if (exfat_create_file(vi_filename, out_buf, (uint32_t)o) == 0) {
        vi_msg = "saved";
        vi_modified = 0;
    } else {
        vi_msg = "save failed";
    }
}

// 在状态栏读取命令
static int vi_read_command(char *buf, int max) {
    terminal_set_cursor(VI_SCREEN_ROWS, 0);
    terminal_setcolor(0x70);
    for (int x = 0; x < 40; x++) terminal_putchar(' ');
    terminal_set_cursor(VI_SCREEN_ROWS, 0);
    terminal_putchar(':');
    terminal_setcolor(0x07);
    int pos = 0;
    while (1) {
        int c = keyboard_getchar();
        if (c == 0) continue;
        if (c == '\n') { buf[pos] = '\0'; return 1; }
        if (c == 27) { buf[0] = '\0'; return 0; }
        if (c == '\b') {
            if (pos > 0) pos--;
        } else if (c >= 32 && c < 127 && pos < max - 1) {
            buf[pos++] = (char)c;
        }
        terminal_set_cursor(VI_SCREEN_ROWS, 1);
        for (int x = 0; x < 40; x++) terminal_putchar(' ');
        terminal_set_cursor(VI_SCREEN_ROWS, 1);
        for (int x = 0; x < pos; x++) terminal_putchar(buf[x]);
        terminal_set_cursor(VI_SCREEN_ROWS, 1 + pos);
    }
}

static void cmd_uname(const char *args) {
    if (args[0] == '-') {
        terminal_writestring("EZOS localhost 0.3-gui i686\n");
    } else {
        terminal_writestring("EZOS\n");
    }
}

static void cmd_df(const char *args) {
    (void)args;
    if (exfat_init() != 0) {
        terminal_writestring("df: exFAT not available\n");
        return;
    }
    const exfat_info_t *info = exfat_get_info();
    if (!info || info->cluster_count == 0) {
        terminal_writestring("df: no volume info\n");
        return;
    }
    uint32_t cluster_size = (uint32_t)info->bytes_per_sector * info->sectors_per_cluster;
    uint32_t used_clusters = exfat_count_used_clusters();
    /* 用 64 位乘法 + 右移换算 KB，避免 libgcc 的 64 位除法依赖 */
    uint64_t total_b = (uint64_t)info->cluster_count * cluster_size;
    uint64_t used_b = (uint64_t)used_clusters * cluster_size;
    uint32_t total_kb = (uint32_t)(total_b >> 10);
    uint32_t used_kb = (uint32_t)(used_b >> 10);
    uint32_t free_kb = (used_kb >= total_kb) ? 0 : total_kb - used_kb;
    uint64_t pct_num = (uint64_t)used_clusters * 100;
    uint32_t pct = udiv64_32((uint32_t)(pct_num >> 32), (uint32_t)pct_num,
                             info->cluster_count);

    terminal_writestring("Filesystem 1K-blocks     Used    Avail Use% Mounted on\n");
    terminal_writestring("exFAT       ");
    print_dec(total_kb);
    terminal_writestring(" ");
    print_dec(used_kb);
    terminal_writestring(" ");
    print_dec(free_kb);
    terminal_writestring(" ");
    print_dec(pct);
    terminal_writestring("% / (");
    print_dec((uint32_t)info->cluster_count);
    terminal_writestring(" clusters, ");
    print_dec(cluster_size);
    terminal_writestring(" B/cluster)\n");
}

static void cmd_du(const char *args) {
    char name[128];
    parse_token(args, name, 128);
    if (exfat_init() != 0) {
        terminal_writestring("du: exFAT not available\n");
        return;
    }
    const exfat_info_t *info = exfat_get_info();
    if (!info) {
        terminal_writestring("du: no volume info\n");
        return;
    }
    uint32_t cluster_size = (uint32_t)info->bytes_per_sector * info->sectors_per_cluster;

    /* du <file>：显示指定文件/目录的簇占用（KB） */
    if (name[0] != '\0') {
        uint32_t clusters = exfat_get_file_clusters(name);
        uint32_t kb = (uint32_t)(((uint64_t)clusters * cluster_size) >> 10);
        print_dec(kb);
        terminal_writestring("\t");
        terminal_writestring(name);
        terminal_putchar('\n');
        return;
    }

    /* du：列出当前目录全部条目占用（KB）与总计 */
    exfat_dir_entry_t entries[64];
    int n = exfat_read_dir(entries, 64);
    if (n < 0) {
        terminal_writestring("du: cannot read directory\n");
        return;
    }
    uint32_t total = 0;
    for (int i = 0; i < n; i++) {
        uint32_t clusters = exfat_get_file_clusters(entries[i].name);
        uint32_t kb = (uint32_t)(((uint64_t)clusters * cluster_size) >> 10);
        total += kb;
        print_dec(kb);
        terminal_writestring("\t");
        terminal_writestring(entries[i].name);
        if (entries[i].is_dir) terminal_putchar('/');
        terminal_putchar('\n');
    }
    terminal_writestring("total\t");
    print_dec(total);
    terminal_putchar('\n');
}

static void cmd_vi(const char *args) {
    char filename[128];
    parse_token(args, filename, 128);
    if (filename[0] == '\0') {
        terminal_writestring("Usage: vi <file>\n");
        return;
    }
    for (int i = 0; i < 128; i++) { vi_filename[i] = filename[i]; if (filename[i] == '\0') break; }

    static uint8_t file_buffer[4096];
    vi_count = 0;
    int bytes = exfat_read_file(filename, file_buffer, 4096);
    if (bytes > 0) {
        int i = 0;
        while (i < bytes && vi_count < VI_MAX_LINES) {
            int j = 0;
            while (i < bytes && file_buffer[i] != '\n' && j < VI_MAX_LINE - 1) {
                vi_lines[vi_count][j++] = (char)file_buffer[i++];
            }
            vi_lines[vi_count][j] = '\0';
            vi_len[vi_count] = j;
            vi_count++;
            if (i < bytes && file_buffer[i] == '\n') i++;
        }
    }
    if (vi_count == 0) {
        vi_count = 1;
        vi_len[0] = 0;
        vi_lines[0][0] = '\0';
    }
    vi_row = 0;
    vi_col = 0;
    vi_top = 0;
    vi_left = 0;
    vi_modified = 0;
    vi_mode = 0;
    vi_msg = NULL;

    int running = 1;
    while (running) {
        vi_clamp_cursor();
        vi_scroll_into_view();
        vi_render();
        int c = 0;
        while (c == 0) {
            c = keyboard_getchar();
            int w = mouse_get_wheel();
            if (w != 0) {
                if (w > 0) {
                    if (vi_row > 0) vi_row--;
                } else {
                    if (vi_row < vi_count - 1) vi_row++;
                }
                vi_clamp_cursor();
                vi_scroll_into_view();
                vi_render();
            }
        }

        if (vi_mode == 1) {
            if (c == 27) {
                vi_mode = 0;
                if (vi_col > 0) vi_col--;
            } else if (c == '\n') {
                vi_insert_newline();
            } else if (c == '\b') {
                vi_backspace();
            } else if (c == KEY_LEFT) {
                if (vi_col > 0) vi_col--;
            } else if (c == KEY_RIGHT) {
                if (vi_col < vi_len[vi_row]) vi_col++;
            } else if (c == KEY_UP) {
                if (vi_row > 0) { vi_row--; vi_clamp_cursor(); }
            } else if (c == KEY_DOWN) {
                if (vi_row < vi_count - 1) { vi_row++; vi_clamp_cursor(); }
            } else if (c >= 32 && c < 127) {
                vi_insert_char((char)c);
            }
        } else {
            if (c == 'h' || c == KEY_LEFT) {
                if (vi_col > 0) vi_col--;
            } else if (c == 'l' || c == KEY_RIGHT) {
                if (vi_col < vi_len[vi_row]) vi_col++;
            } else if (c == 'k' || c == KEY_UP) {
                if (vi_row > 0) vi_row--;
            } else if (c == 'j' || c == KEY_DOWN) {
                if (vi_row < vi_count - 1) vi_row++;
            } else if (c == 'i') {
                vi_mode = 1;
            } else if (c == 'a') {
                if (vi_col < vi_len[vi_row]) vi_col++;
                vi_mode = 1;
            } else if (c == 'x') {
                vi_delete_char();
            } else if (c == '0') {
                vi_col = 0;
            } else if (c == '$') {
                vi_col = vi_len[vi_row];
            } else if (c == 'd') {
                int c2 = 0;
                while (c2 == 0) c2 = keyboard_getchar();
                if (c2 == 'd') vi_delete_line();
            } else if (c == 'g') {
                int c2 = 0;
                while (c2 == 0) c2 = keyboard_getchar();
                if (c2 == 'g') vi_row = 0;
            } else if (c == 'G') {
                if (vi_count > 0) vi_row = vi_count - 1;
            } else if (c == ':') {
                char cmd[16];
                if (vi_read_command(cmd, 16)) {
                    if (my_strcasecmp(cmd, "w") == 0) {
                        vi_save();
                    } else if (my_strcasecmp(cmd, "wq") == 0) {
                        vi_save();
                        running = 0;
                    } else if (my_strcasecmp(cmd, "q") == 0) {
                        if (vi_modified) {
                            vi_msg = "E37: no write (use :q! or :wq)";
                        } else {
                            running = 0;
                        }
                    } else if (my_strcasecmp(cmd, "q!") == 0) {
                        running = 0;
                    } else {
                        vi_msg = "bad command";
                    }
                }
            }
        }
        vi_clamp_cursor();
    }
    terminal_initialize();
}



/* ================= 实用工具 ================= */

// 打印有符号整数
static void print_int(int num) {
    if (num < 0) {
        terminal_putchar('-');
        num = -num;
    }
    print_dec((uint32_t)num);
}

// calc: 表达式计算器（复用 gfx_eval）
static void cmd_calc(const char *args) {
    if (*args == '\0') {
        terminal_writestring("Usage: calc <expr>   e.g. calc 1+2*3\n");
        return;
    }
    int ok = 0;
    int v = gfx_eval(args, &ok);
    if (!ok) {
        terminal_writestring("Error: bad expression\n");
        return;
    }
    terminal_writestring("= ");
    print_int(v);
    terminal_writestring("\n");
}

// hex: 十进制/十六进制互转
void cmd_hex(const char *args) {
    if (*args == '\0') {
        terminal_writestring("Usage: hex <num>   dec->hex, or 0x<hex> -> dec\n");
        return;
    }
    if (args[0] == '0' && (args[1] == 'x' || args[1] == 'X')) {
        uint32_t v = my_htoi(args + 2);
        terminal_writestring("dec: ");
        print_dec(v);
        terminal_writestring("\n");
    } else {
        uint32_t v = (uint32_t)my_atoi(args);
        terminal_writestring("hex: 0x");
        print_hex32(v);
        terminal_writestring("\n");
    }
}

// rand: 伪随机数（LCG）
static uint32_t rand_state = 0x9E3779B9u;
static uint32_t my_rand(void) {
    rand_state = rand_state * 1664525u + 1013904223u;
    return rand_state;
}

void cmd_rand(const char *args) {
    int max = 100;
    if (*args != '\0') max = my_atoi(args);
    if (max <= 0) max = 1;
    print_dec(my_rand() % (uint32_t)max);
    terminal_writestring("\n");
}

/* ================= 游戏 ================= */

// 读取一行输入（回显），返回长度；Esc 返回 -1
static int read_line(char *buf, int maxlen) {
    int n = 0;
    while (1) {
        int c = keyboard_getchar();
        if (c == 0) continue;
        if (c == 27) return -1;
        if (c == '\n') {
            terminal_putchar('\n');
            buf[n] = '\0';
            return n;
        }
        if (c == '\b') {
            if (n > 0) {
                n--;
                terminal_putchar('\b');
                terminal_putchar(' ');
                terminal_putchar('\b');
            }
            continue;
        }
        if (n < maxlen - 1) {
            buf[n++] = (char)c;
            terminal_putchar((char)c);
        }
    }
}

// guess: 猜数字游戏
void cmd_guess(const char *args) {
    (void)args;
    int target = (int)(my_rand() % 100) + 1;
    terminal_writestring("I picked a number 1-100. Guess it! (0 to quit)\n");
    char line[16];
    while (1) {
        terminal_writestring("> ");
        int n = read_line(line, 16);
        if (n < 0) { terminal_writestring("Quit.\n"); return; }
        int g = my_atoi(line);
        if (g == 0) {
            terminal_writestring("Quit. (answer was ");
            print_dec(target);
            terminal_writestring(")\n");
            return;
        }
        if (g < target) terminal_writestring("Too low.\n");
        else if (g > target) terminal_writestring("Too high.\n");
        else {
            terminal_writestring("Correct! The number was ");
            print_dec(target);
            terminal_writestring(".\n");
            return;
        }
    }
}

// tictactoe: 井字棋（玩家 X vs AI O）
static int tt_win(char b[9], char p) {
    static const int lines[8][3] = {
        {0,1,2},{3,4,5},{6,7,8},
        {0,3,6},{1,4,7},{2,5,8},
        {0,4,8},{2,4,6}
    };
    for (int i = 0; i < 8; i++) {
        if (b[lines[i][0]] == p && b[lines[i][1]] == p && b[lines[i][2]] == p)
            return 1;
    }
    return 0;
}

static int tt_full(char b[9]) {
    for (int i = 0; i < 9; i++) if (b[i] == 0) return 0;
    return 1;
}

static void tt_draw(char b[9]) {
    terminal_writestring("\n");
    for (int r = 0; r < 3; r++) {
        terminal_writestring("  ");
        for (int c = 0; c < 3; c++) {
            char ch = b[r*3+c] ? b[r*3+c] : ('1' + r*3+c);
            terminal_putchar(ch);
            if (c < 2) terminal_writestring(" | ");
        }
        terminal_writestring("\n");
        if (r < 2) terminal_writestring("  ---------\n");
    }
    terminal_writestring("\n");
}

void cmd_tictactoe(const char *args) {
    (void)args;
    char b[9] = {0};
    terminal_writestring("Tic-Tac-Toe: you are X, AI is O. Enter 1-9.\n");
    while (1) {
        tt_draw(b);
        char line[8];
        int mv;
        while (1) {
            terminal_writestring("Your move (1-9): ");
            int n = read_line(line, 8);
            if (n < 0) { terminal_writestring("Quit.\n"); return; }
            mv = my_atoi(line);
            if (mv >= 1 && mv <= 9 && b[mv-1] == 0) break;
            terminal_writestring("Invalid. Try again.\n");
        }
        b[mv-1] = 'X';
        if (tt_win(b, 'X')) { tt_draw(b); terminal_writestring("You win!\n"); return; }
        if (tt_full(b)) { tt_draw(b); terminal_writestring("Draw.\n"); return; }
        // AI 落子
        int best = -1;
        for (int i = 0; i < 9 && best < 0; i++) {   // 1) 能赢就赢
            if (b[i] == 0) {
                b[i] = 'O';
                if (tt_win(b, 'O')) best = i;
                b[i] = 0;
            }
        }
        for (int i = 0; i < 9 && best < 0; i++) {   // 2) 堵玩家
            if (b[i] == 0) {
                b[i] = 'X';
                if (tt_win(b, 'X')) best = i;
                b[i] = 0;
            }
        }
        if (best < 0 && b[4] == 0) best = 4;        // 3) 中心
        if (best < 0) {                             // 4) 角落
            static const int corners[4] = {0,2,6,8};
            for (int i = 0; i < 4; i++) {
                if (b[corners[i]] == 0) { best = corners[i]; break; }
            }
        }
        if (best < 0) {                             // 5) 随机
            int n = (int)(my_rand() % 9);
            for (int i = 0; i < 9; i++) {
                int idx = (n + i) % 9;
                if (b[idx] == 0) { best = idx; break; }
            }
        }
        b[best] = 'O';
        terminal_writestring("AI plays ");
        print_dec(best + 1);
        terminal_writestring(".\n");
        if (tt_win(b, 'O')) { tt_draw(b); terminal_writestring("AI wins!\n"); return; }
        if (tt_full(b)) { tt_draw(b); terminal_writestring("Draw.\n"); return; }
    }
}

// snake: 文本贪吃蛇
#define SNAKE_W 40
#define SNAKE_H 20
#define SNAKE_MAX (SNAKE_W * SNAKE_H)

// RDTSC 忙等延时（约 ticks 个 TSC 周期）
static void delay_ticks(uint32_t ticks) {
    uint32_t start;
    __asm__ __volatile__("rdtsc" : "=a"(start) : : "edx");
    while (1) {
        uint32_t now;
        __asm__ __volatile__("rdtsc" : "=a"(now) : : "edx");
        if (now - start >= ticks) break;
    }
}

void cmd_snake(const char *args) {
    (void)args;
    int sx[SNAKE_MAX], sy[SNAKE_MAX];
    int len = 3;
    int dir = KEY_RIGHT;
    int next_dir = KEY_RIGHT;
    int fx = 15, fy = 10;
    int score = 0;
    int paused = 0;
    int over = 0;

    sx[0] = 10; sy[0] = 10;
    sx[1] = 9;  sy[1] = 10;
    sx[2] = 8;  sy[2] = 10;

    terminal_initialize();
    terminal_writestring("SNAKE  [arrows] move  [P] pause  [Esc] quit\n");

    while (1) {
        int c = keyboard_getchar();
        if (c == 27) break;
        if (c == 'p' || c == 'P') paused = !paused;
        else if (c == KEY_UP || c == KEY_DOWN || c == KEY_LEFT || c == KEY_RIGHT) {
            if (!((c == KEY_UP && dir == KEY_DOWN) ||
                  (c == KEY_DOWN && dir == KEY_UP) ||
                  (c == KEY_LEFT && dir == KEY_RIGHT) ||
                  (c == KEY_RIGHT && dir == KEY_LEFT))) {
                next_dir = c;
            }
        }

        if (!paused && !over) {
            dir = next_dir;
            int nx = sx[0], ny = sy[0];
            if (dir == KEY_UP) ny--;
            else if (dir == KEY_DOWN) ny++;
            else if (dir == KEY_LEFT) nx--;
            else if (dir == KEY_RIGHT) nx++;

            if (nx < 0 || nx >= SNAKE_W || ny < 0 || ny >= SNAKE_H) over = 1;
            for (int i = 0; i < len && !over; i++) {
                if (sx[i] == nx && sy[i] == ny) over = 1;
            }
            if (over) break;

            for (int i = len; i > 0; i--) { sx[i] = sx[i-1]; sy[i] = sy[i-1]; }
            sx[0] = nx; sy[0] = ny;

            if (nx == fx && ny == fy) {
                len++;
                score++;
                if (len >= SNAKE_MAX) break;   /* full board: stop before sx[len] overflows */
                int ok = 0;
                for (int tries = 0; tries < 200 && !ok; tries++) {
                    fx = (int)(my_rand() % SNAKE_W);
                    fy = (int)(my_rand() % SNAKE_H);
                    ok = 1;
                    for (int i = 0; i < len; i++) {
                        if (sx[i] == fx && sy[i] == fy) { ok = 0; break; }
                    }
                }
            }
        }

        terminal_initialize();
        terminal_writestring("SNAKE  score: ");
        print_dec(score);
        if (paused) terminal_writestring("  [PAUSED]");
        terminal_writestring("\n");
        for (int x = 0; x < SNAKE_W + 2; x++) terminal_putchar('#');
        terminal_putchar('\n');
        for (int y = 0; y < SNAKE_H; y++) {
            terminal_putchar('#');
            for (int x = 0; x < SNAKE_W; x++) {
                char ch = ' ';
                if (x == fx && y == fy) ch = '*';
                for (int i = 0; i < len; i++) {
                    if (sx[i] == x && sy[i] == y) { ch = (i == 0) ? 'O' : 'o'; break; }
                }
                terminal_putchar(ch);
            }
            terminal_putchar('#');
            terminal_putchar('\n');
        }
        for (int x = 0; x < SNAKE_W + 2; x++) terminal_putchar('#');
        terminal_putchar('\n');
        if (over) {
            terminal_writestring("GAME OVER! score: ");
            print_dec(score);
            terminal_writestring("\n");
            break;
        }

        delay_ticks(30000000);
    }
    terminal_initialize();
}


/* GUI 终端等外部模块注入一行命令 */
int shell_exec_line(const char *line) {
    char buf[CMD_BUFFER_SIZE];
    int i = 0;
    if (!line) return -1;
    while (line[i] && i < CMD_BUFFER_SIZE - 1) { buf[i] = line[i]; i++; }
    buf[i] = '\0';
    shell_execute(buf);
    return 0;
}
