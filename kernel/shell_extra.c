/*
 * shell_extra.c - EZOS 命令行增强模块
 *
 * 借鉴来源：MikanOS (https://github.com/uchan-nos/mikanOS, Apache-2.0)
 *   - MakeArgVector(): 把整行命令按空白拆分为 argv/argc 的参数解析框架
 *   - ls 详细模式 / hexdump / mem / echo / help 等命令语义
 *
 * 适配层：ezos_console_* 把 EZOS tty 文本终端与键盘驱动桥接为
 * 轻量 console 接口，使上层命令实现与具体终端解耦（最小改动兼容）。
 *
 * 编译环境：i686-elf-gcc，-ffreestanding，无 libc。
 */

#include "shell_extra.h"
#include "tty.h"
#include "keyboard.h"
#include "types.h"
#include "port.h"
#include "exfat.h"

/* ==================================================================
 * 1. ezos_console 适配层：把 EZOS tty / 键盘桥接为轻量 console 接口
 * ================================================================== */

void ezos_console_putchar(char c) {
    terminal_putchar(c);
}

void ezos_console_write(const char *s) {
    terminal_writestring(s);
}

void ezos_console_print_dec(uint32_t num) {
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

void ezos_console_print_hex32(uint32_t val) {
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4) {
        terminal_putchar(hex[(val >> i) & 0xF]);
    }
}

void ezos_console_print_hex_byte(uint8_t val) {
    static const char hex[] = "0123456789ABCDEF";
    terminal_putchar(hex[val >> 4]);
    terminal_putchar(hex[val & 0x0F]);
}

/* 行编辑读取：回显字符、支持退格；返回长度（Esc 返回 -1） */
int ezos_console_readline(char *buf, int maxlen) {
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

/* ==================================================================
 * 2. 工具函数（freestanding，无 libc）
 * ================================================================== */

static int x_tolower(char c) {
    if (c >= 'A' && c <= 'Z') return c + ('a' - 'A');
    return c;
}

static int x_strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        int ca = x_tolower(*a), cb = x_tolower(*b);
        if (ca != cb) return ca - cb;
        a++;
        b++;
    }
    return *a - *b;
}

static int x_atoi(const char *s) {
    int result = 0;
    while (*s >= '0' && *s <= '9') {
        int d = *s - '0';
        if (result > (2147483647 - d) / 10) return 2147483647;  /* 溢出钳制 */
        result = result * 10 + d;
        s++;
    }
    return result;
}

static int x_hex_char_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static uint32_t x_htoi(const char *s) {
    uint32_t result = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    while (*s) {
        int v = x_hex_char_val(*s);
        if (v < 0) break;
        result = (result << 4) | v;
        s++;
    }
    return result;
}

/* ==================================================================
 * 3. argv 参数解析框架（借鉴 MikanOS MakeArgVector）
 *
 * 把一行输入按空白拆成 argv[0..argc-1]，token 拷贝到内部 argbuf。
 * MikanOS 原版使用 C++ lambda + Error 返回；此处以 C 语言最小化改写。
 * ================================================================== */

#define EZOS_MAX_ARGS 16
#define EZOS_ARGBUF_SIZE 256

typedef struct {
    int argc;
    char *argv[EZOS_MAX_ARGS];
    char argbuf[EZOS_ARGBUF_SIZE];
} ezos_args_t;

static void ezos_parse_args(const char *line, ezos_args_t *a) {
    int argc = 0;
    int argbuf_index = 0;

    a->argc = 0;
    while (*line == ' ') line++;
    if (*line == '\0') return;

    while (1) {
        if (argc >= EZOS_MAX_ARGS || argbuf_index >= EZOS_ARGBUF_SIZE) break;

        a->argv[argc] = &a->argbuf[argbuf_index];
        while (*line != '\0' && *line != ' ' && argbuf_index < EZOS_ARGBUF_SIZE - 1) {
            a->argbuf[argbuf_index++] = *line++;
        }
        a->argbuf[argbuf_index++] = '\0';
        argc++;

        while (*line == ' ') line++;
        if (*line == '\0') break;
    }
    a->argc = argc;
}

/* ==================================================================
 * 4. 命令别名表
 * ================================================================== */

#define MAX_ALIASES 16
#define ALIAS_NAME_MAX 24
#define ALIAS_CMD_MAX 96

static char alias_names[MAX_ALIASES][ALIAS_NAME_MAX];
static char alias_cmds[MAX_ALIASES][ALIAS_CMD_MAX];
static int alias_count = 0;

const char *shell_extra_lookup_alias(const char *name) {
    for (int i = 0; i < alias_count; i++) {
        if (x_strcasecmp(name, alias_names[i]) == 0) {
            return alias_cmds[i];
        }
    }
    return NULL;
}

/* ==================================================================
 * 5. 新命令实现
 * ================================================================== */

/* ver: 显示内核版本 */
void cmd_ver(const char *args) {
    (void)args;
    ezos_console_write("EZOS Kernel version 0.3-gui (i386)\n");
}

/* sysinfo: 汇总系统信息（CPU / 内存 / 时间 / 版本） */
static uint8_t x_cmos_read(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

static uint8_t x_bcd2dec(uint8_t v) {
    return (uint8_t)((v & 0x0F) + ((v >> 4) * 10));
}

static void x_print_cpu_vendor(void) {
    uint32_t eax, ebx, ecx, edx;
    char vendor[13];
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    ((uint32_t*)vendor)[0] = ebx;
    ((uint32_t*)vendor)[1] = edx;
    ((uint32_t*)vendor)[2] = ecx;
    vendor[12] = '\0';
    ezos_console_write(vendor);
}

void cmd_sysinfo(const char *args) {
    (void)args;

    ezos_console_write("EZOS System Information\n");
    ezos_console_write("------------------------\n");
    ezos_console_write("Version : 0.3-gui (i386)\n");
    ezos_console_write("CPU     : ");
    x_print_cpu_vendor();
    ezos_console_write("\n");

    uint16_t mem_kb = (uint16_t)((x_cmos_read(0x16) << 8) | x_cmos_read(0x15));
    uint16_t ext_kb = (uint16_t)((x_cmos_read(0x18) << 8) | x_cmos_read(0x17));
    ezos_console_write("Base mem: ");
    ezos_console_print_dec(mem_kb);
    ezos_console_write(" KB\n");
    ezos_console_write("Ext  mem: ");
    ezos_console_print_dec(ext_kb);
    ezos_console_write(" KB\n");
    ezos_console_write("Total   : ");
    ezos_console_print_dec(mem_kb + ext_kb);
    ezos_console_write(" KB\n");

    uint8_t hour = x_bcd2dec(x_cmos_read(0x04));
    uint8_t minute = x_bcd2dec(x_cmos_read(0x02));
    uint8_t second = x_bcd2dec(x_cmos_read(0x00));
    if (hour > 23) hour = 0;          /* clamp invalid CMOS values */
    if (minute > 59) minute = 0;
    if (second > 59) second = 0;
    ezos_console_write("Time    : ");
    if (hour < 10) ezos_console_putchar('0');
    ezos_console_print_dec(hour);
    ezos_console_putchar(':');
    if (minute < 10) ezos_console_putchar('0');
    ezos_console_print_dec(minute);
    ezos_console_putchar(':');
    if (second < 10) ezos_console_putchar('0');
    ezos_console_print_dec(second);
    ezos_console_write("\n");

    uint8_t year = x_bcd2dec(x_cmos_read(0x09));
    uint8_t month = x_bcd2dec(x_cmos_read(0x08));
    uint8_t day = x_bcd2dec(x_cmos_read(0x07));
    if (month < 1 || month > 12) month = 1;   /* clamp invalid CMOS values */
    if (day < 1 || day > 31) day = 1;
    ezos_console_write("Date    : ");
    ezos_console_print_dec(year + 2000);
    ezos_console_putchar('-');
    if (month < 10) ezos_console_putchar('0');
    ezos_console_print_dec(month);
    ezos_console_putchar('-');
    if (day < 10) ezos_console_putchar('0');
    ezos_console_print_dec(day);
    ezos_console_write("\n");
}

/* type / which: 查询命令类型（内建 / 别名 / 外部 / 未知） */
extern int shell_is_builtin(const char *name);

static void x_type_common(const char *args, int which_mode) {
    char name[64];
    int i = 0;
    while (*args == ' ') args++;
    while (*args && *args != ' ' && i < 63) name[i++] = *args++;
    name[i] = '\0';

    if (i == 0) {
        ezos_console_write(which_mode ? "Usage: which <command>\n" : "Usage: type <command>\n");
        return;
    }

    if (shell_is_builtin(name)) {
        ezos_console_write(name);
        ezos_console_write(which_mode ? ": built-in command\n" : " is a shell builtin\n");
        return;
    }
    if (shell_extra_lookup_alias(name)) {
        ezos_console_write(name);
        ezos_console_write(which_mode ? ": aliased command\n" : " is an alias\n");
        return;
    }
    if (exfat_init() == 0) {
        /* 检查当前目录是否存在同名文件 */
        char dummy[1];
        if (exfat_get_file_size(name) > 0) {
            ezos_console_write(name);
            ezos_console_write(which_mode ? ": external file\n" : " is a file in cwd\n");
            return;
        }
        (void)dummy;
    }
    ezos_console_write(name);
    ezos_console_write(which_mode ? ": not found\n" : ": not found\n");
}

void cmd_type(const char *args) {
    x_type_common(args, 0);
}

void cmd_which(const char *args) {
    x_type_common(args, 1);
}

/* alias: 列出 / 定义命令别名。语法 alias [name=value] [name...] */
void cmd_alias(const char *args) {
    if (*args == '\0') {
        if (alias_count == 0) {
            ezos_console_write("No aliases defined.\n");
            return;
        }
        for (int i = 0; i < alias_count; i++) {
            ezos_console_write("alias ");
            ezos_console_write(alias_names[i]);
            ezos_console_write("='");
            ezos_console_write(alias_cmds[i]);
            ezos_console_write("'\n");
        }
        return;
    }

    /* 首个 token 若含 '='，则视为 name=value（value 取到行尾，支持空格与引号） */
    const char *p = args;
    while (*p == ' ') p++;
    const char *name_start = p;
    while (*p && *p != '=' && *p != ' ') p++;
    if (*p == '=') {
        int nl = (int)(p - name_start);
        if (nl <= 0 || nl >= ALIAS_NAME_MAX) {
            ezos_console_write("alias: invalid name\n");
            return;
        }
        char name[ALIAS_NAME_MAX];
        for (int i = 0; i < nl; i++) name[i] = name_start[i];
        name[nl] = '\0';

        p++;
        const char *value = p;
        int vl = 0;
        while (value[vl] && value[vl] != '\n' && value[vl] != '\r') vl++;
        while (vl > 0 && (value[vl - 1] == ' ' || value[vl - 1] == '\t')) vl--;
        if (vl >= 2 && value[0] == '\'' && value[vl - 1] == '\'') {
            value++;
            vl -= 2;
        }
        if (vl <= 0 || vl >= ALIAS_CMD_MAX) {
            ezos_console_write("alias: invalid value\n");
            return;
        }

        int slot = alias_count;
        for (int i = 0; i < alias_count; i++) {
            if (x_strcasecmp(alias_names[i], name) == 0) {
                slot = i;
                break;
            }
        }
        if (slot >= MAX_ALIASES) {
            ezos_console_write("alias: table full\n");
            return;
        }
        for (int i = 0; i <= nl; i++) alias_names[slot][i] = name[i];
        for (int i = 0; i < vl; i++) alias_cmds[slot][i] = value[i];
        alias_cmds[slot][vl] = '\0';
        if (slot == alias_count) alias_count++;
        return;
    }

    /* 无 '='：全部作为别名名查询 */
    ezos_args_t a;
    ezos_parse_args(args, &a);
    for (int k = 0; k < a.argc; k++) {
        const char *v = shell_extra_lookup_alias(a.argv[k]);
        if (v) {
            ezos_console_write("alias ");
            ezos_console_write(a.argv[k]);
            ezos_console_write("='");
            ezos_console_write(v);
            ezos_console_write("'\n");
        } else {
            ezos_console_write(a.argv[k]);
            ezos_console_write(": alias not found\n");
        }
    }
}

/* unalias: 删除命令别名 */
void cmd_unalias(const char *args) {
    char name[ALIAS_NAME_MAX];
    int i = 0;
    while (*args == ' ') args++;
    while (*args && *args != ' ' && i < ALIAS_NAME_MAX - 1) name[i++] = *args++;
    name[i] = '\0';

    if (i == 0) {
        ezos_console_write("Usage: unalias <name>\n");
        return;
    }
    for (int k = 0; k < alias_count; k++) {
        if (x_strcasecmp(alias_names[k], name) == 0) {
            for (int m = k; m < alias_count - 1; m++) {
                for (int j = 0; j < ALIAS_NAME_MAX; j++) alias_names[m][j] = alias_names[m + 1][j];
                for (int j = 0; j < ALIAS_CMD_MAX; j++) alias_cmds[m][j] = alias_cmds[m + 1][j];
            }
            alias_count--;
            ezos_console_write("Unaliased: ");
            ezos_console_write(name);
            ezos_console_write("\n");
            return;
        }
    }
    ezos_console_write("unalias: ");
    ezos_console_write(name);
    ezos_console_write(": not found\n");
}

/* sleep: RDTSC 忙等延时（近似，QEMU 下按 ~1GHz TSC 估算） */
static void x_delay_ticks(uint64_t ticks) {
    uint64_t elapsed = 0;
    uint32_t start;
    __asm__ __volatile__("rdtsc" : "=a"(start) : : "edx");
    while (elapsed < ticks) {
        uint32_t now;
        __asm__ __volatile__("rdtsc" : "=a"(now) : : "edx");
        elapsed += (uint32_t)(now - start);
        start = now;
    }
}

void cmd_sleep(const char *args) {
    int ms = x_atoi(args);
    if (ms <= 0) {
        ezos_console_write("Usage: sleep <milliseconds>\n");
        return;
    }
    if (ms > 60000) ms = 60000;
    x_delay_ticks((uint64_t)ms * 1000000ull);  /* 约 1ms/1e6 ticks（QEMU 近似） */
    ezos_console_write("Done (");
    ezos_console_print_dec(ms);
    ezos_console_write(" ms).\n");
}

/* mem: 查看指定物理地址内存。mem <hexaddr> [len]（默认 128 字节） */
void cmd_mem(const char *args) {
    ezos_args_t a;
    ezos_parse_args(args, &a);
    if (a.argc < 1) {
        ezos_console_write("Usage: mem <hexaddr> [len]\n");
        return;
    }
    uint32_t addr = x_htoi(a.argv[0]);
    int len = 128;
    if (a.argc >= 2) {
        len = x_atoi(a.argv[1]);
        if (len <= 0) len = 128;
        if (len > 512) len = 512;
    }

    ezos_console_write("Memory dump at 0x");
    ezos_console_print_hex32(addr);
    ezos_console_write(" (");
    ezos_console_print_dec(len);
    ezos_console_write(" bytes):\n");
    for (int i = 0; i < len; i += 16) {
        ezos_console_print_hex32(addr + i);
        ezos_console_write("  ");
        for (int j = 0; j < 16; j++) {
            if (i + j < len) {
                ezos_console_print_hex_byte(*(volatile uint8_t *)(addr + i + j));
                ezos_console_putchar(' ');
            } else {
                ezos_console_write("   ");
            }
        }
        ezos_console_write(" ");
        for (int j = 0; j < 16 && i + j < len; j++) {
            char c = (char)*(volatile uint8_t *)(addr + i + j);
            ezos_console_putchar((c >= 32 && c <= 126) ? c : '.');
        }
        ezos_console_write("\n");
    }
}

/* ==================================================================
 * 6. 单命令详细帮助表
 * ================================================================== */

const char *shell_extra_help(const char *cmd) {
    if (x_strcasecmp(cmd, "ver") == 0) {
        return "ver - show kernel version\n  usage: ver";
    }
    if (x_strcasecmp(cmd, "sysinfo") == 0) {
        return "sysinfo - show system summary\n  usage: sysinfo\n  shows CPU vendor, memory, time and date.";
    }
    if (x_strcasecmp(cmd, "type") == 0 || x_strcasecmp(cmd, "which") == 0) {
        return "type|which <command> - show command type\n  usage: type <command>\n  reports builtin / alias / external file / not found.";
    }
    if (x_strcasecmp(cmd, "alias") == 0) {
        return "alias - list or define command aliases\n  usage: alias            (list all)\n         alias name=cmd   (define)\n         alias name       (query)";
    }
    if (x_strcasecmp(cmd, "unalias") == 0) {
        return "unalias <name> - remove a command alias\n  usage: unalias <name>";
    }
    if (x_strcasecmp(cmd, "sleep") == 0) {
        return "sleep <ms> - busy-wait delay (approx)\n  usage: sleep <milliseconds>";
    }
    if (x_strcasecmp(cmd, "mem") == 0) {
        return "mem <hexaddr> [len] - dump physical memory\n  usage: mem 0xB8000\n         mem 0x100000 64";
    }
    if (x_strcasecmp(cmd, "df") == 0) {
        return "df - show exFAT disk space usage\n  usage: df\n  shows total, used and free space of the current exFAT drive.";
    }
    if (x_strcasecmp(cmd, "du") == 0) {
        return "du [name] - show disk usage of current dir or file\n  usage: du\n         du subdir\n         du file.txt\n  without argument, reports usage of the current directory.";
    }
    if (x_strcasecmp(cmd, "ls") == 0) {
        return "ls [path] - list files and directories\n  usage: ls\n         ls subdir\n  lists the current/root directory or the given path on the exFAT drive.";
    }
    if (x_strcasecmp(cmd, "cd") == 0) {
        return "cd <dir> - change current directory\n  usage: cd subdir\n         cd ..\n         cd /";
    }
    if (x_strcasecmp(cmd, "pwd") == 0) {
        return "pwd - print working directory\n  usage: pwd";
    }
    if (x_strcasecmp(cmd, "cat") == 0) {
        return "cat <file> - print file content\n  usage: cat readme.txt";
    }
    if (x_strcasecmp(cmd, "clear") == 0 || x_strcasecmp(cmd, "cls") == 0) {
        return "clear|cls - clear the terminal screen\n  usage: clear";
    }
    if (x_strcasecmp(cmd, "echo") == 0) {
        return "echo [-n] <text> - print text\n  usage: echo hello\n         echo -n no-newline";
    }
    if (x_strcasecmp(cmd, "time") == 0) {
        return "time - show current time\n  usage: time";
    }
    if (x_strcasecmp(cmd, "date") == 0) {
        return "date - show current date\n  usage: date";
    }
    if (x_strcasecmp(cmd, "gui") == 0) {
        return "gui - launch the graphical desktop\n  usage: gui\n  starts the window manager; use 'Back to Terminal' in the start menu to return to shell.";
    }
    if (x_strcasecmp(cmd, "version") == 0) {
        return "version - show kernel version\n  usage: version";
    }
    if (x_strcasecmp(cmd, "uname") == 0) {
        return "uname - print system information\n  usage: uname";
    }
    if (x_strcasecmp(cmd, "meminfo") == 0) {
        return "meminfo - show memory info\n  usage: meminfo";
    }
    if (x_strcasecmp(cmd, "cpuid") == 0) {
        return "cpuid - show CPU vendor string\n  usage: cpuid";
    }
    if (x_strcasecmp(cmd, "readdisk") == 0) {
        return "readdisk [drive] <lba> - read and hexdump a disk sector\n  usage: readdisk 100\n         readdisk 1 100";
    }
    if (x_strcasecmp(cmd, "hexdump") == 0) {
        return "hexdump <hexaddr> - dump memory at address\n  usage: hexdump 0xB8000";
    }
    if (x_strcasecmp(cmd, "beep") == 0) {
        return "beep - make a beep sound\n  usage: beep";
    }
    if (x_strcasecmp(cmd, "about") == 0) {
        return "about - about this OS\n  usage: about";
    }
    if (x_strcasecmp(cmd, "history") == 0) {
        return "history - show command history\n  usage: history";
    }
    if (x_strcasecmp(cmd, "setcolor") == 0) {
        return "setcolor <fg> [bg] - set text color (0-15)\n  usage: setcolor 2\n         setcolor 7 1";
    }
    if (x_strcasecmp(cmd, "setdrive") == 0) {
        return "setdrive <0|1> - set exFAT drive\n  usage: setdrive 1";
    }
    if (x_strcasecmp(cmd, "write") == 0) {
        return "write <file> <content> - create a file with content\n  usage: write hello.txt Hello world";
    }
    if (x_strcasecmp(cmd, "rm") == 0) {
        return "rm <file> - delete a file\n  usage: rm temp.txt";
    }
    if (x_strcasecmp(cmd, "format") == 0) {
        return "format - format slave disk as exFAT\n  usage: format\n  WARNING: erases all data on the slave disk.";
    }
    if (x_strcasecmp(cmd, "grep") == 0) {
        return "grep <pattern> <file> - print lines containing pattern\n  usage: grep error log.txt";
    }
    if (x_strcasecmp(cmd, "wc") == 0) {
        return "wc <file> - count lines, words and characters\n  usage: wc note.txt";
    }
    if (x_strcasecmp(cmd, "head") == 0) {
        return "head <file> [n] - show first n lines (default 10)\n  usage: head log.txt\n         head log.txt 20";
    }
    if (x_strcasecmp(cmd, "tail") == 0) {
        return "tail <file> [n] - show last n lines (default 10)\n  usage: tail log.txt\n         tail log.txt 20";
    }
    if (x_strcasecmp(cmd, "touch") == 0) {
        return "touch <file> - create an empty file\n  usage: touch new.txt";
    }
    if (x_strcasecmp(cmd, "cp") == 0) {
        return "cp <src> <dst> - copy a file\n  usage: cp a.txt b.txt";
    }
    if (x_strcasecmp(cmd, "mv") == 0) {
        return "mv <src> <dst> - move or rename a file\n  usage: mv a.txt sub/b.txt";
    }
    if (x_strcasecmp(cmd, "mkdir") == 0) {
        return "mkdir <dir> - create a directory\n  usage: mkdir docs";
    }
    if (x_strcasecmp(cmd, "vi") == 0) {
        return "vi <file> - edit a text file\n  usage: vi note.txt";
    }
    if (x_strcasecmp(cmd, "calc") == 0) {
        return "calc <expr> - evaluate an expression (e.g. calc 1+2*3)\n  usage: calc (4+5)*6";
    }
    if (x_strcasecmp(cmd, "hex") == 0) {
        return "hex <num> - convert decimal <-> hex (0x.. for hex input)\n  usage: hex 255\n         hex 0xFF";
    }
    if (x_strcasecmp(cmd, "rand") == 0) {
        return "rand [max] - generate a random number (default 0-99)\n  usage: rand\n         rand 1000";
    }
    if (x_strcasecmp(cmd, "guess") == 0) {
        return "guess - guess-the-number game\n  usage: guess";
    }
    if (x_strcasecmp(cmd, "tictactoe") == 0) {
        return "tictactoe - play tic-tac-toe vs AI\n  usage: tictactoe";
    }
    if (x_strcasecmp(cmd, "snake") == 0) {
        return "snake - play snake game (arrows, P pause, Esc quit)\n  usage: snake";
    }
    if (x_strcasecmp(cmd, "help") == 0) {
        return "help [cmd] - show this help or detailed help for a command\n  usage: help\n         help df";
    }
    if (x_strcasecmp(cmd, "reboot") == 0) {
        return "reboot - reboot the system\n  usage: reboot";
    }
    if (x_strcasecmp(cmd, "shutdown") == 0) {
        return "shutdown - shutdown the system (QEMU exits)\n  usage: shutdown";
    }
    return NULL;
}
