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
#include "fs.h"
#include "isr.h"
#include "dmesg.h"
#include "kmalloc.h"
#include "paging.h"
#include "syscall.h"
#include "pmm.h"
#include "elf.h"
#include "exec.h"
#include "fpu.h"
#include "calc.h"
#include "task.h"
#include "fd.h"

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
    if (fs_init() == 0) {
        /* 检查当前目录是否存在同名文件 */
        char dummy[1];
        if (fs_get_file_size(name) > 0) {
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

/* uptime: 开机时长（PIT 1000Hz tick 计） */
void cmd_uptime(const char *args) {
    (void)args;
    uint32_t ticks = g_pit_ticks;
    /* 顺带锁存 ch0 counter（诊断时钟用：counter 按需读，不走 IRQ） */
    outb(0x43, 0x00);
    uint16_t cnt = (uint16_t)(inb(0x40) | (inb(0x40) << 8));
    ezos_console_write("up ");
    ezos_console_print_dec(ticks / 1000);
    ezos_console_write("s (ticks ");
    ezos_console_print_dec(ticks);
    ezos_console_write(" c ");
    ezos_console_print_dec(cnt);
    ezos_console_write(")\n");
}

/* dmesg [n] - 回看内核环形日志（默认全部；n = 最近 n 行） */
void cmd_dmesg(const char *args) {
    ezos_args_t a;
    ezos_parse_args(args, &a);
    int n = 0;                             /* 0 = 全部 */
    if (a.argc >= 1) {
        n = x_atoi(a.argv[0]);
        if (n < 0) n = 0;
    }
    ezos_console_write("dmesg: last ");
    ezos_console_print_dec(dmesg_lines());
    ezos_console_write(" line(s) buffered\n");
    int dumped = dmesg_dump(ezos_console_putchar, n);
    if (dumped == 0) ezos_console_write("(empty)\n");
}

/* kmtest - 堆分配器自检：分配/释放/合并/越界检测演练 */
/* utest：ring3 + 系统调用自检。
 *   1) 内核侧断言：非用户页（.bss.hi 的 0x00100000）必须被 user_range_ok 拒绝
 *   2) 映射用户页 -> 装入内建用户程序 -> enter_usermode() 切入 ring3
 *   3) 用户程序两次 SYS_WRITE（各往返一次 ring3->ring0->ring3）后 SYS_EXIT
 *   4) 校验退出码与系统调用计数
 * 用户程序的输出直接由 SYS_WRITE 打到终端，穿插在本函数输出之间。 */
void cmd_utest(const char *args) {
    (void)args;
    ezos_console_write("usermode demo (step 4):\n");

    /* 1) 安全边界"拒绝"侧（此刻用户页尚未映射）
     *    只测拒绝不够：校验逻辑若写反到"一律拒绝"，测试同样会显示通过，
     *    而真实的用户 write 会全部失败——必须正反两侧都验。 */
    int rej_kernel = (syscall_user_range_ok(0x00100000u, 4) == 0);  /* 已映射但无 PTE_US */
    int rej_unmap  = (syscall_user_range_ok(0x05000000u, 4) == 0);  /* 完全未映射 */
    ezos_console_write("  kernel page rejected: ");
    ezos_console_write(rej_kernel ? "yes [OK]\n" : "NO [FAIL]\n");
    ezos_console_write("  unmapped page rejected: ");
    ezos_console_write(rej_unmap ? "yes [OK]\n" : "NO [FAIL]\n");

    /* 2) ring3 实跑（用户输出在此之间直接打印） */
    uint32_t before = syscall_count();
    ezos_console_write("  entering ring 3 at 0x00400000...\n");

    int rc = usermode_run_demo();

    ezos_console_write("  back in ring 0, exit code ");
    ezos_console_print_dec((uint32_t)rc);
    ezos_console_write(", syscalls ");
    ezos_console_print_dec(syscall_count() - before);
    ezos_console_write("\n");

    /* 3) 安全边界"放行"侧：此时用户代码页已映射且带 US|RW */
    int ok_user = (syscall_user_range_ok(0x00400000u, 4) == 1);
    int ok_rw   = (syscall_user_range_rw(0x00400000u, 4) == 1);
    ezos_console_write("  user page accepted: ");
    ezos_console_write(ok_user ? "yes [OK]\n" : "NO [FAIL]\n");
    ezos_console_write("  user page writable: ");
    ezos_console_write(ok_rw ? "yes [OK]\n" : "NO [FAIL]\n");

    int ok = rej_kernel && rej_unmap && ok_user && ok_rw &&
             (rc == 0) && (syscall_count() - before == 3);
    ezos_console_write(ok ? "  result: PASS\n" : "  result: FAIL\n");
}

/* pagetest：分页自检（identity 一致性 + 动态映射/读写/解映射）。
 * 不主动触发 #PF——破坏性缺页测试用 `crash pf`。 */
static void pg_puts(const char *s) { ezos_console_write(s); }

/* elftest：ELF 加载器自检（步骤 5b） */
void cmd_elftest(const char *args) {
    (void)args;
    int fail = elf_selftest(pg_puts);
    if (fail != 0) {
        ezos_console_write("  (");
        ezos_console_print_dec((uint32_t)fail);
        ezos_console_write(" assertion(s) failed)\n");
    } else {
        ezos_console_write("  result: PASS\n");
    }
}

/* exec：从盘上装载 ELF32 用户程序并在 ring3 执行（步骤 5d） */
void cmd_exec(const char *args) {
    /* 第一个空白之前是文件名，其余原样作为用户程序的命令行参数 */
    char name[64];
    int i = 0;
    while (args[i] != '\0' && args[i] != ' ' && i < 63) { name[i] = args[i]; i++; }
    name[i] = '\0';
    const char *rest = args + i;
    while (*rest == ' ') rest++;

    if (name[0] == '\0') {
        ezos_console_write("exec: usage: exec <file> [args...]\n");
        return;
    }

    ezos_console_write("exec: loading ");
    ezos_console_write(name);
    ezos_console_write(" ...\n");

    const char *why = 0;
    int rc = exec_file(name, rest, &why);
    if (rc < 0) {
        ezos_console_write("exec: ");
        ezos_console_write(name);
        ezos_console_write(": ");
        ezos_console_write(why ? why : "failed");
        ezos_console_write("\n");
        return;
    }
    ezos_console_write("exec: ");
    ezos_console_write(name);
    ezos_console_write(" exited with code ");
    ezos_console_print_dec((uint32_t)rc);
    ezos_console_write("\n");
}

/* calc：浮点计算器（步骤 5e）。
 * 递归下降解析 + x87 求值，语法见 calc.h。
 * 结果打印：整数不带小数点，其余固定 6 位小数（四舍五入）。 */
void cmd_calc(const char *args) {
    if (args == 0 || args[0] == '\0') {
        ezos_console_write("calc <expr> - floating point calculator\n");
        ezos_console_write("  ops: + - * / % ^ ( )\n");
        ezos_console_write("  funcs: sqrt sin cos tan exp ln log abs floor ceil round\n");
        ezos_console_write("  consts: pi e\n");
        ezos_console_write("  examples:\n");
        ezos_console_write("    calc 1.5*2+1\n");
        ezos_console_write("    calc sqrt(2)\n");
        ezos_console_write("    calc sin(pi/6)*100\n");
        return;
    }

    if (!fpu_available()) {
        ezos_console_write("calc: no FPU present on this machine\n");
        return;
    }

    double v;
    const char *err = 0;
    if (calc_eval(args, &v, &err) != 0) {
        ezos_console_write("calc: ");
        ezos_console_write(err ? err : "error");
        ezos_console_write("\n");
        return;
    }

    ezos_console_write("= ");
    /* 打印：四舍五入到 6 位小数；round(x*1e6)/1e6 会引入双重舍入误差，
     * 但显示 6 位小数时不可见，且实现只需一次 x87 调整。 */
    double r = v;
    if (r < 0) { ezos_console_write("-"); r = -r; }
    double scaled = r * 1000000.0 + 0.5;      /* 舍入 */
    uint32_t whole = (uint32_t)(scaled / 1000000.0);
    uint32_t frac  = (uint32_t)(scaled - (double)whole * 1000000.0);
    /* 舍入可能进位到 1e6（如 0.9999996） */
    if (frac >= 1000000u) { whole += 1; frac -= 1000000u; }
    ezos_console_print_dec(whole);
    if (frac != 0) {
        ezos_console_write(".");
        char digits[7];
        for (int i = 5; i >= 0; i--) { digits[i] = (char)('0' + frac % 10u); frac /= 10u; }
        digits[6] = '\0';
        /* 去掉尾部的 0（0.5 而不是 0.500000） */
        int end = 6;
        while (end > 0 && digits[end - 1] == '0') end--;
        digits[end] = '\0';
        ezos_console_write(digits);
    }
    ezos_console_write("\n");
}

/* selftest：一键运行所有子系统自检（步骤 5e） */
void cmd_selftest(const char *args) {
    (void)args;
    ezos_console_write("EZOS full self-test:\n");
    struct { const char *name; int run; } r[8];
    int n = 0, failed = 0;

    /* kmalloc：静默判定（分配/写入/回读/释放后 used 归零）
     * ——详细数字留给 kmtest，这里只看健康与否 */
    {
        uint32_t u0 = kmalloc_used();
        void *p1 = kmalloc(1000), *p2 = kmalloc(4000), *p3 = kmalloc(200);
        int ok = (p1 != 0 && p2 != 0 && p3 != 0);
        if (ok) {
            for (int i = 0; i < 1000; i++) ((uint8_t *)p1)[i] = (uint8_t)i;
            for (int i = 0; i < 1000; i++)
                if (((uint8_t *)p1)[i] != (uint8_t)i) { ok = 0; break; }
            kfree(p3); kfree(p2); kfree(p1);
            if (kmalloc_used() != u0) ok = 0;            /* 合并后必须回到基线 */
        }
        r[n].name = "kmalloc  kernel heap";
        r[n].run = ok ? 0 : 1;
        n++;
    }

    /* paging：官方自检（无输出参数时静默跑断言） */
    r[n].name = "paging   identity + map/unmap";
    r[n].run = paging_selftest(0);
    n++;

    /* pmm / elf：自检自带逐条输出，直接跑 */
    r[n].name = "pmm      page frame allocator";
    r[n].run = pmm_selftest(pg_puts);
    n++;

    r[n].name = "elf      ELF32 loader";
    r[n].run = elf_selftest(pg_puts);
    n++;

    /* fpu：x87 探测 + 算术 */
    r[n].name = "fpu      x87 arithmetic";
    r[n].run = fpu_selftest(pg_puts);
    n++;

    /* calc：表达式引擎（静默：一组已知答案的算式） */
    {
        static const struct { const char *e; double want; } t[] = {
            { "1.5*2+1",        4 },
            { "10/4",           2.5 },
            { "sqrt(2)*sqrt(2)",2 },
            { "2^10",           1024 },
            { "sin(0)",         0 },
            { "1+2*3",          7 },
            { "(1+2)*3",        9 },
            { "-4+10",          6 },
        };
        int bad = 0;
        for (uint32_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
            double v; const char *err = 0;
            if (calc_eval(t[i].e, &v, &err) != 0) { bad++; continue; }
            /* 显示 6 位小数内相等即可（1/3 类循环小数不算失败） */
            double d = v - t[i].want;
            if (d < 0) d = -d;
            if (d > 5e-7) bad++;
        }
        /* 错误路径：这些表达式必须被拒绝 */
        static const char *badexpr[] = { "1+", "sin", "1/0", "0/0", "abc", "1 2" };
        for (uint32_t i = 0; i < sizeof(badexpr) / sizeof(badexpr[0]); i++) {
            double v; const char *err = 0;
            if (calc_eval(badexpr[i], &v, &err) == 0) bad++;      /* 不该成功 */
        }
        r[n].name = "calc     expression engine";
        r[n].run = bad;
        n++;
    }

    /* exec：真跑盘上的 HELLO.ELF，退出码 42 = 全链路健康
     * （FS->ELF->ring3->syscall->exit->页回收） */
    {
        const char *why = 0;
        int rc = exec_file("HELLO.ELF", 0, &why);
        r[n].name = "exec     ring3 ELF run";
        r[n].run = (rc == 42) ? 0 : 1;
        n++;
    }

    /* fd：open/read/lseek/write/close 全语义（真实文件系统走一遍） */
    r[n].name = "fd       file descriptors";
    r[n].run = fd_selftest(pg_puts);
    n++;

    /* 汇总 */
    ezos_console_write("----------\n");
    for (int i = 0; i < n; i++) {
        ezos_console_write("  ");
        ezos_console_write(r[i].name);
        ezos_console_write(": ");
        ezos_console_write(r[i].run == 0 ? "PASS\n" : "FAIL\n");
        if (r[i].run != 0) failed++;
    }
    ezos_console_write("----------\n");
    ezos_console_write("  ");
    ezos_console_print_dec((uint32_t)n);
    ezos_console_write(" tests, ");
    ezos_console_print_dec((uint32_t)failed);
    ezos_console_write(" failed -> ");
    ezos_console_write(failed == 0 ? "ALL PASS\n" : "SYSTEM UNSTABLE\n");
}

/* boot_selftest：开机自检（kernel_main 调用）。
 * 与 cmd_selftest 同一套判定逻辑，但全程静默，只返回失败子系统数。
 * 0 = 全部通过。 */
int boot_selftest(void) {
    struct { int run; } r[8];
    int n = 0;

    {   /* kmalloc */
        uint32_t u0 = kmalloc_used();
        void *p1 = kmalloc(1000), *p2 = kmalloc(4000), *p3 = kmalloc(200);
        int ok = (p1 != 0 && p2 != 0 && p3 != 0);
        if (ok) {
            for (int i = 0; i < 1000; i++) ((uint8_t *)p1)[i] = (uint8_t)i;
            for (int i = 0; i < 1000; i++)
                if (((uint8_t *)p1)[i] != (uint8_t)i) { ok = 0; break; }
            kfree(p3); kfree(p2); kfree(p1);
            if (kmalloc_used() != u0) ok = 0;
        }
        r[n++].run = ok ? 0 : 1;
    }
    r[n++].run = paging_selftest(0);                 /* NULL = 静默 */
    r[n++].run = pmm_selftest(0);
    r[n++].run = elf_selftest(0);
    r[n++].run = fpu_selftest(0);
    {   /* calc：已知答案 + 必须拒绝的畸形 */
        static const struct { const char *e; double want; } t[] = {
            { "1.5*2+1", 4 }, { "10/4", 2.5 }, { "sqrt(2)*sqrt(2)", 2 },
            { "2^10", 1024 }, { "sin(0)", 0 }, { "1+2*3", 7 },
        };
        int bad = 0;
        for (uint32_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
            double v; const char *err = 0;
            if (calc_eval(t[i].e, &v, &err) != 0) { bad++; continue; }
            double d = v - t[i].want;
            if (d < 0) d = -d;
            if (d > 5e-7) bad++;
        }
        static const char *badexpr[] = { "1+", "sin", "1/0", "abc" };
        for (uint32_t i = 0; i < sizeof(badexpr) / sizeof(badexpr[0]); i++) {
            double v; const char *err = 0;
            if (calc_eval(badexpr[i], &v, &err) == 0) bad++;
        }
        r[n++].run = bad;
    }
    {   /* exec：HELLO.ELF 退出码 42 */
        const char *why = 0;
        r[n++].run = (exec_file("HELLO.ELF", 0, &why) == 42) ? 0 : 1;
    }
    r[n++].run = fd_selftest(0);        /* NULL = 静默 */

    int failed = 0;
    for (int i = 0; i < n; i++)
        if (r[i].run != 0) failed++;
    return failed;
}


/* ktask：起两个计数内核线程，验证抢占式调度真的在工作（步骤 6a） */
static volatile uint32_t kt_a, kt_b;
static volatile int kt_a_quit, kt_b_quit;

static void kt_worker(void *arg) {
    volatile uint32_t *ctr = (arg == 0) ? &kt_a : &kt_b;
    volatile int *quit = (arg == 0) ? &kt_a_quit : &kt_b_quit;
    while (!*quit) (*ctr)++;
}

void cmd_ktask(const char *args) {
    (void)args;
    kt_a = kt_b = 0;
    kt_a_quit = kt_b_quit = 0;

    int p1 = task_create("kt_a", kt_worker, 0);
    int p2 = task_create("kt_b", kt_worker, (void *)1);
    if (p1 < 0 || p2 < 0) {
        ezos_console_write("ktask: task_create failed\n");
        return;
    }
    ezos_console_write("ktask: started 2 kernel threads, running 2s...\n");

    /* 忙等约 2 秒（用 PIT tick，1000Hz） */
    uint32_t start = g_pit_ticks;
    while (g_pit_ticks - start < 2000u) task_yield();

    kt_a_quit = 1;
    kt_b_quit = 1;
    /* 再让一轮，确保两个线程都退出 */
    start = g_pit_ticks;
    while (g_pit_ticks - start < 200u) task_yield();

    ezos_console_write("  thread a iterations: ");
    ezos_console_print_dec(kt_a);
    ezos_console_write("\n  thread b iterations: ");
    ezos_console_print_dec(kt_b);
    ezos_console_write("\n  scheduler switches: ");
    ezos_console_print_dec(schedule_count());
    ezos_console_write("\n");

    uint32_t sw = schedule_count();
    int ok = (kt_a > 0) && (kt_b > 0) && (sw > 2);
    ezos_console_write("  result: ");
    ezos_console_write(ok ? "PASS\n" : "FAIL\n");
}

/* ps：列出任务表 */
void cmd_ps(const char *args) {
    (void)args;
    static const char *st[] = { "unused", "ready", "running", "blocked", "zombie" };
    uint32_t n = 0;
    const task_t *tab = task_table(&n);
    ezos_console_write("  PID  STATE    SWITCH  NAME\n");
    for (uint32_t i = 0; i < n; i++) {
        if (tab[i].state == TASK_UNUSED) continue;
        ezos_console_write("  ");
        ezos_console_print_dec(tab[i].pid);
        ezos_console_write("   ");
        ezos_console_write(st[(uint32_t)tab[i].state]);
        ezos_console_write("   ");
        ezos_console_print_dec(tab[i].switches);
        ezos_console_write("     ");
        ezos_console_write(tab[i].name[0] ? tab[i].name : "(unnamed)");
        ezos_console_write("\n");
    }
    ezos_console_write("  scheduler switches: ");
    ezos_console_print_dec(schedule_count());
    ezos_console_write("\n");
}

/* pmmtest：物理页帧分配器自检（步骤 5a） */
void cmd_pmmtest(const char *args) {
    (void)args;
    int fail = pmm_selftest(pg_puts);
    if (fail != 0) {
        ezos_console_write("  (");
        ezos_console_print_dec((uint32_t)fail);
        ezos_console_write(" assertion(s) failed)\n");
    }
}

void cmd_pagetest(const char *args) {
    (void)args;
    if (!paging_enabled()) {
        ezos_console_write("paging: CR0.PG=0 (paging not enabled)\n");
        return;
    }
    int fail = paging_selftest(pg_puts);
    if (fail != 0) {
        ezos_console_write("  (");
        ezos_console_print_dec((uint32_t)fail);
        ezos_console_write(" assertion(s) failed)\n");
    }
}

void cmd_kmtest(const char *args) {
    (void)args;
    ezos_console_write("kmalloc self-test:\n");
    uint32_t t0 = kmalloc_total(), u0 = kmalloc_used(), f0 = kmalloc_largest_free();
    ezos_console_write("  before: total ");
    ezos_console_print_dec(t0 / 1024);
    ezos_console_write("KB used ");
    ezos_console_print_dec(u0);
    ezos_console_write("B largest ");
    ezos_console_print_dec(f0 / 1024);
    ezos_console_write("KB\n");

    void *p1 = kmalloc(1000);
    void *p2 = kmalloc(4000);
    void *p3 = kmalloc(200);
    if (!p1 || !p2 || !p3) {
        ezos_console_write("  FAIL: alloc returned NULL\n");
        return;
    }
    /* 填充并校验 */
    for (int i = 0; i < 1000; i++) ((uint8_t *)p1)[i] = (uint8_t)i;
    for (int i = 0; i < 1000; i++)
        if (((uint8_t *)p1)[i] != (uint8_t)i) {
            ezos_console_write("  FAIL: data corrupted at ");
            ezos_console_print_dec(i);
            ezos_console_write("\n");
            return;
        }
    ezos_console_write("  alloc 1000+4000+200B ... write/readback OK\n");

    uint32_t u1 = kmalloc_used();
    kfree(p2);
    kfree(p1);
    kfree(p3);
    uint32_t u2 = kmalloc_used();
    ezos_console_write("  used ");
    ezos_console_print_dec(u1);
    ezos_console_write("B -> ");
    ezos_console_print_dec(u2);
    ezos_console_write("B after free (merge ");
    ezos_console_write(u2 == u0 ? "OK" : "FAIL");
    ezos_console_write(")\n");

    uint32_t f1 = kmalloc_largest_free();
    ezos_console_write("  largest free after: ");
    ezos_console_print_dec(f1 / 1024);
    ezos_console_write("KB (before ");
    ezos_console_print_dec(f0 / 1024);
    ezos_console_write("KB)\n");
    ezos_console_write("  result: ");
    ezos_console_write(u2 == u0 ? "PASS" : "FAIL");
    ezos_console_write("\n");
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
    if (x_strcasecmp(cmd, "dmesg") == 0) {
        return "dmesg [n] - show kernel ring log (all or last n lines)\n  usage: dmesg\n         dmesg 10";
    }
    if (x_strcasecmp(cmd, "kmtest") == 0) {
        return "kmtest - kernel heap allocator self-test (alloc/free/merge/guard)";
    }
    if (x_strcasecmp(cmd, "pagetest") == 0) {
        return "pagetest - paging self-test (identity consistency + map/rw/unmap)";
    }
    if (x_strcasecmp(cmd, "utest") == 0) {
        return "utest - ring3 user-mode + int 0x80 syscall self-test\n"
               "  usage: utest\n"
               "  runs a builtin machine-code program in ring 3 (2 writes + exit),\n"
               "  and checks the user-pointer safety boundary on both sides.";
    }
    if (x_strcasecmp(cmd, "pmmtest") == 0) {
        return "pmmtest - physical page frame allocator self-test\n"
               "  usage: pmmtest\n"
               "  checks alloc/free accounting, double-free rejection and\n"
               "  out-of-range rejection.";
    }
    if (x_strcasecmp(cmd, "elftest") == 0) {
        return "elftest - ELF32 loader self-test\n"
               "  usage: elftest\n"
               "  validates a well-formed ELF is accepted and 6 malformed ones\n"
               "  are rejected, then loads, reads back, checks W^X and unloads.";
    }
    if (x_strcasecmp(cmd, "exec") == 0) {
        return "exec <file> [args] - load and run an ELF32 user program in ring 3\n"
               "  usage: exec HELLO.ELF\n"
               "         exec HELLO.ELF arg1 arg2\n"
               "  reads the file from the mounted filesystem, loads it at\n"
               "  0x00400000, passes argc/argv on the user stack, and runs it\n"
               "  until SYS_EXIT. Pages are fully reclaimed afterwards.\n"
               "  A malformed ELF is rejected before a single page is mapped.";
    }
    if (x_strcasecmp(cmd, "calc") == 0) {
        return "calc <expr> - floating point calculator\n"
               "  usage: calc 1.5*2+1\n"
               "         calc sqrt(2)\n"
               "         calc sin(pi/6)\n"
               "  operators: + - * / % ^ ( )\n"
               "  functions: sqrt sin cos tan exp ln log abs floor ceil round\n"
               "  constants: pi e";
    }
    if (x_strcasecmp(cmd, "selftest") == 0) {
        return "selftest - run all subsystem self-tests at once\n"
               "  usage: selftest\n"
               "  covers: kmalloc, paging, pmm, elf loader, fpu, calc, exec\n"
               "  a summary line reports PASS/FAIL per subsystem and overall.";
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
