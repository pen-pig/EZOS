/*
 * games.c - EZOS 游戏合集（自 sinwindows games.cpp 移植，C++ -> 纯 C）
 *
 * 来源：temp/sindows/sindows/src/games.cpp（741 行，7 个游戏）：
 *   1. 猜数字 2. 井字棋AI 3. 贪吃蛇 4. 2048 5. 扫雷 6. 石头剪刀布 7. 记忆翻牌
 *
 * 移植原则：游戏逻辑保持原样，仅 I/O 适配 EZOS 接口：
 *   - 文本输出  ：ezos_console_write / ezos_console_print_dec / ezos_console_putchar
 *   - 行输入    ：ezos_console_readline（Esc 按空串处理）
 *   - 键盘      ：keyboard_getchar()（非阻塞，0 表示无键，取代 sinwindows poll_key）
 *   - 清屏/光标 ：terminal_initialize / terminal_set_cursor / terminal_clear_line
 *   - 滚动      ：terminal_scroll_up / terminal_scroll_down（EZOS 方向键为 KEY_UP/KEY_DOWN，
 *                 Shift+方向键映射 KEY_PGUP/KEY_PGDN，取代 sinwindows handle_scroll_key）
 *   - 随机数    ：本地 LCG + RDTSC 播种（EZOS 无全局 rand/seed）
 *   - 计时      ：RDTSC（QEMU 下约 1GHz，180e6 ticks ≈ 0.18s，取代 PIT tick）
 *
 * 编译：i686-elf-gcc -ffreestanding -O2 -Wall -Wextra -Ikernel
 */

#include "games.h"
#include "types.h"
#include "tty.h"
#include "keyboard.h"

/* ---- ezos_console 适配层（定义于 shell_extra.c） ---- */
extern void ezos_console_putchar(char c);
extern void ezos_console_write(const char *s);
extern void ezos_console_print_dec(uint32_t num);
extern int  ezos_console_readline(char *buf, int maxlen);

/* ==================================================================
 * GUI 桌面模式适配层（gfxwin.c 桌面图标启动游戏）
 * ------------------------------------------------------------------
 * 内核 shell 模式（g_gfx=0）：全部走原有 ezos_console / terminal 直写。
 * GUI 模式（g_gfx=1）：文本输出重定向到 24x80 缓冲，由终端窗口重绘；
 *   阻塞输入改为轮询 keyboard_getchar + 回调 yield（驱动重绘），
 *   避免 GUI 主循环被游戏独占导致画面冻结。
 * ================================================================== */
static int g_gfx = 0;
static int g_gfx_kind = 0;          /* 当前 GUI 运行的游戏类型 1..7（games_play 设置） */
static void (*g_yield)(void) = NULL;
static char g_gbuf[24 * 80];
static int g_gpos = 0;

/* 保存真实接口（供重定向宏内部使用，避免宏自递归） */
static void (*real_putchar)(char) = ezos_console_putchar;
static void (*real_write)(const char *) = ezos_console_write;
static void (*real_print_dec)(uint32_t) = ezos_console_print_dec;
static int  (*real_readline)(char *, int) = ezos_console_readline;
static void (*real_term_init)(void) = terminal_initialize;
static void (*real_term_set_cursor)(size_t, size_t) = terminal_set_cursor;
static void (*real_term_clear_line)(size_t) = terminal_clear_line;
static size_t (*real_term_get_row)(void) = terminal_get_row;

void games_set_gfx(int on, void (*yield_fn)(void)) {
    g_gfx = on;
    g_yield = yield_fn;
    g_gpos = 0;
    for (int i = 0; i < 24 * 80; i++) g_gbuf[i] = ' ';
}
char *games_gfx_buf(void) { return g_gbuf; }
int games_gfx_pos(void) { return g_gpos; }

/* GUI 鼠标点击队列：gfxwin 点击沿写入，游戏轮询消费 */
static int g_mouse_a = -1, g_mouse_b = -1, g_mouse_btn = 0;
void games_set_mouse(int a, int b, int btn)
{
    g_mouse_a = a;
    g_mouse_b = b;
    if (btn) g_mouse_btn = btn;   /* 1=左键 2=右键 */
}
int games_mouse_poll(int *a, int *b)
{
    int btn = g_mouse_btn;
    if (btn == 0) return 0;
    g_mouse_btn = 0;              /* 一次消费 */
    *a = g_mouse_a;
    *b = g_mouse_b;
    return btn;
}

static void games_yield(void) { if (g_yield) g_yield(); }

static void games_gfx_scroll(void) {
    for (int i = 0; i < 23 * 80; i++) g_gbuf[i] = g_gbuf[i + 80];
    for (int i = 23 * 80; i < 24 * 80; i++) g_gbuf[i] = ' ';
    g_gpos = 23 * 80;
}

static void games_putc(char c) {
    if (!g_gfx) { real_putchar(c); return; }
    if (c == '\n') {
        g_gpos = (g_gpos / 80 + 1) * 80;
    } else {
        g_gbuf[g_gpos] = c;
        g_gpos++;
    }
    if (g_gpos >= 24 * 80) games_gfx_scroll();
}

static void games_write(const char *s) {
    if (!g_gfx) { real_write(s); return; }
    while (*s) games_putc(*s++);
}

static void games_print_dec(uint32_t num) {
    if (!g_gfx) { real_print_dec(num); return; }
    char tmp[12];
    int n = 0;
    if (num == 0) tmp[n++] = '0';
    while (num > 0) { tmp[n++] = (char)('0' + (num % 10)); num /= 10; }
    while (n > 0) games_putc(tmp[--n]);
}

static void games_clear_gfx(void) {
    if (!g_gfx) { real_term_init(); return; }
    g_gpos = 0;
    for (int i = 0; i < 24 * 80; i++) g_gbuf[i] = ' ';
}

static void games_set_cursor_gfx(size_t r, size_t c) {
    if (!g_gfx) { real_term_set_cursor(r, c); return; }
    g_gpos = (int)(r * 80 + c);
    if (g_gpos > 24 * 80) g_gpos = 24 * 80 - 1;
}

static void games_clear_line_gfx(size_t r) {
    if (!g_gfx) { real_term_clear_line(r); return; }
    for (int c = 0; c < 80; c++) g_gbuf[(int)r * 80 + c] = ' ';
}

static size_t games_get_row_gfx(void) {
    if (!g_gfx) return real_term_get_row();
    return (size_t)(g_gpos / 80);
}

/* GUI 下行输入：轮询键盘 + yield 驱动重绘；Esc -> 空串（返回 -1） */
static int games_readline_gfx(char *buf, int maxlen) {
    if (!g_gfx) {
        buf[0] = '\0';
        return real_readline(buf, maxlen);
    }
    int n = 0;
    for (;;) {
        int c = keyboard_getchar();
        if (c == 0) { games_yield(); continue; }
        if (c == 0x1B) { buf[0] = '\0'; games_yield(); return -1; }
        if (c == '\n' || c == '\r') {
            buf[n] = '\0';
            games_putc('\n');
            games_yield();
            return 0;
        }
        if (c == 0x08 && n > 0) {
            n--;
            if (g_gpos > 0) {
                g_gpos--;
                g_gbuf[g_gpos] = ' ';
            }
        } else if (c >= 0x20 && c < 0x7F && n < maxlen - 1) {
            buf[n++] = (char)c;
            games_putc((char)c);
        }
        games_yield();
    }
}

/* 游戏对 tty / shell_extra 的调用在 GUI 模式下重定向到缓冲 */
#define ezos_console_putchar games_putc
#define ezos_console_write games_write
#define ezos_console_print_dec games_print_dec
#define ezos_console_readline games_readline_gfx
#define terminal_initialize games_clear_gfx
#define terminal_set_cursor games_set_cursor_gfx
#define terminal_clear_line games_clear_line_gfx
#define terminal_get_row games_get_row_gfx

/* ==================================================================
 * 小型工具（freestanding，无 libc；EZOS 内核不自带全局 rand/atoi 等）
 * ================================================================== */

static uint32_t games_rng_state = 0x9E3779B9u;

static void games_rand_seed(uint32_t s) {
    games_rng_state = s ? s : 1;
}

static uint32_t games_rand_next(void) {
    games_rng_state = games_rng_state * 1664525u + 1013904223u;
    return games_rng_state;
}

/* RDTSC 低 32 位时间源（EZOS 无 PIT tick 计数，参照 shell.c delay_ticks） */
static uint32_t games_rdtsc(void) {
    uint32_t lo;
    __asm__ __volatile__("rdtsc" : "=a"(lo) : : "edx");
    return lo;
}

/* 数字前缀解析（等价 sinwindows strtoi 在游戏中的用法） */
static int games_atoi(const char *s) {
    int result = 0;
    while (*s >= '0' && *s <= '9') {
        result = result * 10 + (*s - '0');
        s++;
    }
    return result;
}

static const char *games_strchr(const char *s, int ch) {
    while (*s) {
        if (*s == (char)ch) return s;
        s++;
    }
    return NULL;
}

/* 字符串相等比较（等价 sinwindows command_is 的菜单匹配） */
static int games_streq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

/* 行输入封装：Esc 时按空串处理（sinwindows readline 无返回值） */
static void games_readline(char *buf, int maxlen) {
    buf[0] = '\0';
    if (ezos_console_readline(buf, maxlen) < 0) buf[0] = '\0';
}

/* 清屏（tty 无独立清屏 API；terminal_initialize 重置行列并清显存） */
static void games_clear(void) {
    terminal_initialize();
}

/* ---------------- 游戏 1：猜数字 ---------------- */
static void game_guess(void) {
    games_rand_seed(games_rdtsc() + 1);
    int secret = (int)(games_rand_next() % 100) + 1;
    games_clear();
    ezos_console_write("=== Guess the Number ===\n");
    ezos_console_write("I picked a number 1-100. You have 10 tries.\n");
    char buf[16];
    int tries = 0;
    while (tries < 10) {
        ezos_console_print_dec((uint32_t)(tries + 1));
        ezos_console_write("> ");
        games_readline(buf, sizeof(buf));
        int guess = games_atoi(buf);
        if (guess < 1 || guess > 100) {
            ezos_console_write("Enter a number 1-100.\n");
            continue;
        }
        tries++;
        if (guess == secret) {
            ezos_console_write("Correct! You win in ");
            ezos_console_print_dec((uint32_t)tries);
            ezos_console_write(" tries.\n");
            break;
        } else if (guess < secret) {
            ezos_console_write("Too low.\n");
        } else {
            ezos_console_write("Too high.\n");
        }
    }
    if (tries >= 10) {
        ezos_console_write("Out of tries. The number was ");
        ezos_console_print_dec((uint32_t)secret);
        ezos_console_write(".\n");
    }
    ezos_console_write("Press Enter to return...\n");
    games_readline(buf, sizeof(buf));
}

/* ---------------- 游戏 2：井字棋 ----------------
 * 原 sinwindows 直接写 VGA 显存做局部刷新；EZOS tty 封装显存，
 * 改为 terminal_set_cursor + putchar 单格定位更新，行为等价：
 * 首次 tic_render 整盘绘制，之后落子只调 tic_update_cell 更新单格。 */
static char tic_board[9];
static uint16_t tic_cell_off[9];   // 每个格子字符的屏幕偏移 (row*80+col)
static int tic_board_row = 0;      // 棋盘首行的屏幕行号

static void tic_status(const char *s);

static void tic_render(void) {
    if (g_gfx) { games_yield(); return; }   /* GUI 图形渲染由桌面窗口完成 */
    games_clear();
    ezos_console_write("=== Tic-Tac-Toe ===\n");
    ezos_console_write("You are X, AI is O. Position map:\n");
    ezos_console_write(" 1 | 2 | 3\n--+---+--\n 4 | 5 | 6\n--+---+--\n 7 | 8 | 9\n");
    ezos_console_write("\n");
    tic_board_row = (int)terminal_get_row();
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) {
            int idx = r * 3 + c;
            tic_cell_off[idx] = (uint16_t)((tic_board_row + r * 2) * 80 + c * 4);
            ezos_console_putchar(tic_board[idx]);
            if (c < 2) ezos_console_write(" | ");
        }
        ezos_console_putchar('\n');
        if (r < 2) ezos_console_write("--+---+--\n");
    }
    // 棋盘占 5 行：board_row..board_row+4；下方空一行留给提示行
    tic_status("You are X, AI is O. Enter 1-9 to move.");
}

/* 只更新单个格子的字符（空格 -> X/O），用光标定位覆盖写 */
static void tic_update_cell(int i) {
    if (g_gfx) { games_yield(); return; }
    int off = tic_cell_off[i];
    terminal_set_cursor((size_t)(off / 80), (size_t)(off % 80));
    ezos_console_putchar(tic_board[i]);
}

/* 覆盖写固定提示行（清行后重写，不追加、不滚动、不堆积） */
static void tic_status(const char *s) {
    if (g_gfx) { games_yield(); return; }
    terminal_clear_line((size_t)(tic_board_row + 6));
    terminal_set_cursor((size_t)(tic_board_row + 6), 0);
    ezos_console_write(s);
}

/* 清空并定位输入提示行（棋盘下方固定位置） */
static void tic_prompt_line(void) {
    if (g_gfx) return;
    terminal_clear_line((size_t)(tic_board_row + 7));
    terminal_set_cursor((size_t)(tic_board_row + 7), 0);
}

static int tic_winner(void) {
    for (int r = 0; r < 3; r++) {
        if (tic_board[r * 3] != ' ' && tic_board[r * 3] == tic_board[r * 3 + 1] &&
            tic_board[r * 3 + 1] == tic_board[r * 3 + 2])
            return tic_board[r * 3] == 'X' ? 1 : 2;
    }
    for (int c = 0; c < 3; c++) {
        if (tic_board[c] != ' ' && tic_board[c] == tic_board[3 + c] &&
            tic_board[3 + c] == tic_board[6 + c])
            return tic_board[c] == 'X' ? 1 : 2;
    }
    if (tic_board[0] != ' ' && tic_board[0] == tic_board[4] && tic_board[4] == tic_board[8])
        return tic_board[0] == 'X' ? 1 : 2;
    if (tic_board[2] != ' ' && tic_board[2] == tic_board[4] && tic_board[4] == tic_board[6])
        return tic_board[2] == 'X' ? 1 : 2;
    return 0;
}

static int tic_full(void) {
    for (int i = 0; i < 9; i++) if (tic_board[i] == ' ') return 0;
    return 1;
}

static int tic_ai_move(void) {
    // 1) 自己能赢就走
    for (int i = 0; i < 9; i++) {
        if (tic_board[i] == ' ') {
            tic_board[i] = 'O';
            if (tic_winner() == 2) { tic_board[i] = ' '; return i; }
            tic_board[i] = ' ';
        }
    }
    // 2) 堵玩家
    for (int i = 0; i < 9; i++) {
        if (tic_board[i] == ' ') {
            tic_board[i] = 'X';
            if (tic_winner() == 1) { tic_board[i] = ' '; return i; }
            tic_board[i] = ' ';
        }
    }
    // 3) 中心 / 角落 / 边
    if (tic_board[4] == ' ') return 4;
    int corners[4] = { 0, 2, 6, 8 };
    for (int i = 0; i < 4; i++) if (tic_board[corners[i]] == ' ') return corners[i];
    int edges[4] = { 1, 3, 5, 7 };
    for (int i = 0; i < 4; i++) if (tic_board[edges[i]] == ' ') return edges[i];
    return -1;
}

static void game_tic(void) {
    for (int i = 0; i < 9; i++) tic_board[i] = ' ';
    tic_render();   // 首次整盘绘制（内部清屏），之后落子只更新单格
    char buf[8];
    if (g_gfx) {
        /* GUI 图形模式：1-9 或鼠标点击落子，AI 立即回应；界面由桌面窗口绘制 */
        games_yield();
        for (;;) {
            int pos = -1;
            int ma, mb;
            if (games_mouse_poll(&ma, &mb)) {
                if (ma >= 0 && ma < 3 && mb >= 0 && mb < 3) pos = ma * 3 + mb;
                else { games_yield(); continue; }
            } else {
                int c = keyboard_getchar();
                if (c == 0) { games_yield(); continue; }
                if (c < '1' || c > '9') continue;
                pos = c - '1';
            }
            if (tic_board[pos] != ' ') { games_yield(); continue; }
            tic_board[pos] = 'X';
            games_yield();
            if (tic_winner() == 1) break;
            if (tic_full()) break;
            int ai = tic_ai_move();
            if (ai < 0 || ai > 8 || tic_board[ai] != ' ') break;
            tic_board[ai] = 'O';
            games_yield();
            if (tic_winner() == 2) break;
            if (tic_full()) break;
        }
        games_yield();   /* 终局画面 */
        while (keyboard_getchar() != 0) { }
        while (keyboard_getchar() == 0) games_yield();
        return;
    }
    for (;;) {
        tic_prompt_line();
        ezos_console_write("Your move (1-9): ");
        games_readline(buf, sizeof(buf));
        int pos = games_atoi(buf) - 1;
        if (pos < 0 || pos > 8) {
            tic_status("Invalid move. Enter a number 1-9.");
            continue;
        }
        if (tic_board[pos] != ' ') {
            tic_status("Position occupied, choose another.");
            continue;
        }
        tic_board[pos] = 'X';
        tic_update_cell(pos);
        if (tic_winner() == 1) { tic_status("You win!"); break; }
        if (tic_full()) { tic_status("Draw!"); break; }
        int ai = tic_ai_move();
        if (ai < 0 || ai > 8 || tic_board[ai] != ' ') break;
        tic_board[ai] = 'O';
        tic_update_cell(ai);
        if (tic_winner() == 2) { tic_status("AI wins!"); break; }
        if (tic_full()) { tic_status("Draw!"); break; }
    }
    tic_prompt_line();
    ezos_console_write("Press Enter to return...");
    games_readline(buf, sizeof(buf));
}

/* ---------------- 游戏 3：贪吃蛇 ---------------- */
#define SNAKE_W 20
#define SNAKE_H 10
#define SNAKE_MAX 200

static int snake_body[SNAKE_MAX][2];
static int snake_len;
static int snake_dir; // 0 up 1 down 2 left 3 right
static int food_x, food_y;
static int snake_alive = 1;   /* 0=结束/退出（供 GUI 渲染） */

static void snake_render(void) {
    if (g_gfx) { games_yield(); return; }   /* GUI 图形渲染由桌面窗口完成 */
    games_clear();
    ezos_console_write("=== Snake (WASD move, Q quit) ===\n");
    for (int y = 0; y < SNAKE_H; y++) {
        for (int x = 0; x < SNAKE_W; x++) {
            if (x == food_x && y == food_y) { ezos_console_putchar('*'); continue; }
            int isSnake = 0;
            for (int i = 0; i < snake_len; i++) {
                if (snake_body[i][0] == x && snake_body[i][1] == y) { isSnake = 1; break; }
            }
            ezos_console_putchar((uint8_t)(isSnake ? '#' : '.'));
        }
        ezos_console_write("\n");
    }
    ezos_console_write("Score: ");
    ezos_console_print_dec((uint32_t)(snake_len - 3));
    ezos_console_write("\n");
}

static void snake_spawn_food(void) {
    for (;;) {
        food_x = (int)(games_rand_next() % SNAKE_W);
        food_y = (int)(games_rand_next() % SNAKE_H);
        int onSnake = 0;
        for (int i = 0; i < snake_len; i++) {
            if (snake_body[i][0] == food_x && snake_body[i][1] == food_y) { onSnake = 1; break; }
        }
        if (!onSnake) return;
    }
}

static void game_snake(void) {
    games_rand_seed(games_rdtsc() + 7);
    games_clear();
    ezos_console_write("=== Snake ===\n");
    ezos_console_write("WASD to steer, Q to quit. Eat * to grow.\n");
    snake_len = 3;
    snake_dir = 3; // 初始向右
    snake_alive = 1;
    snake_body[0][0] = 4; snake_body[0][1] = 5;
    snake_body[1][0] = 3; snake_body[1][1] = 5;
    snake_body[2][0] = 2; snake_body[2][1] = 5;
    snake_spawn_food();
    snake_render();

    // RDTSC 每约 0.18s（QEMU ~1GHz）自动前进一格，不依赖按键
    uint32_t last_move = games_rdtsc();
    const uint32_t move_interval = 180000000u;
    int alive = 1;
    int quit = 0;
    while (alive && !quit) {
        // 非阻塞处理按键：W/A/S/D 只改变方向，Q 退出；
        // Shift+Up/Shift+Down（EZOS 映射 KEY_PGUP/KEY_PGDN）滚动屏幕
        int c;
        /* 鼠标点击窗口四区改方向（0上 1下 2左 3右） */
        {
            int ma, mb;
            if (games_mouse_poll(&ma, &mb)) {
                if (ma == 0) { if (snake_dir != 1) snake_dir = 0; }
                else if (ma == 1) { if (snake_dir != 0) snake_dir = 1; }
                else if (ma == 2) { if (snake_dir != 3) snake_dir = 2; }
                else if (ma == 3) { if (snake_dir != 2) snake_dir = 3; }
            }
        }
        while ((c = keyboard_getchar()) != 0) {
            if (c == KEY_UP || c == KEY_DOWN) continue;   // 普通方向键在贪吃蛇中无意义
            if (c == KEY_PGUP) { terminal_scroll_up(); continue; }
            if (c == KEY_PGDN) { terminal_scroll_down(); continue; }
            if (c == 'w' || c == 'W') { if (snake_dir != 1) snake_dir = 0; }
            else if (c == 's' || c == 'S') { if (snake_dir != 0) snake_dir = 1; }
            else if (c == 'a' || c == 'A') { if (snake_dir != 3) snake_dir = 2; }
            else if (c == 'd' || c == 'D') { if (snake_dir != 2) snake_dir = 3; }
            else if (c == 'q' || c == 'Q') { quit = 1; }
        }
        if (quit) break;

        // 定时移动
        uint32_t now = games_rdtsc();
        if ((uint32_t)(now - last_move) < move_interval) continue;
        last_move = now;

        int nx = snake_body[0][0], ny = snake_body[0][1];
        if (snake_dir == 0) ny--;
        else if (snake_dir == 1) ny++;
        else if (snake_dir == 2) nx--;
        else nx++;
        // 碰到边界：输
        if (nx < 0 || nx >= SNAKE_W || ny < 0 || ny >= SNAKE_H) { alive = 0; break; }
        // 碰到自身（尾部即将移开，不判死；排除最后一节）
        for (int i = 0; i < snake_len - 1; i++) {
            if (snake_body[i][0] == nx && snake_body[i][1] == ny) { alive = 0; break; }
        }
        if (!alive) break;
        for (int i = snake_len - 1; i > 0; i--) {
            snake_body[i][0] = snake_body[i - 1][0];
            snake_body[i][1] = snake_body[i - 1][1];
        }
        snake_body[0][0] = nx; snake_body[0][1] = ny;
        if (nx == food_x && ny == food_y) {
            snake_body[snake_len][0] = snake_body[snake_len - 1][0];
            snake_body[snake_len][1] = snake_body[snake_len - 1][1];
            snake_len++;
            if (snake_len >= SNAKE_MAX) { alive = 0; break; }
            snake_spawn_food();
        }
        snake_render();
    }
    if (!alive) {
        snake_alive = 0;
        ezos_console_write("Game Over! Score: ");
        ezos_console_print_dec((uint32_t)(snake_len - 3));
        ezos_console_write("\n");
    } else {
        snake_alive = 0;
        ezos_console_write("Quit.\n");
    }
    ezos_console_write("Press any key to return to menu...\n");
    // 清空按键缓冲后等待任意键返回
    while (keyboard_getchar() != 0) { }
    while (keyboard_getchar() == 0) games_yield();
}

/* ---------------- 游戏 4：2048 ---------------- */
static uint32_t g2048[16];
static uint32_t g2048_score;
static int      g2048_moved;
static int      g2048_over;   /* 0=进行中 1=结束（供 GUI 渲染） */

static void g2048_spawn(void) {
    int empty[16], n = 0;
    for (int i = 0; i < 16; i++) if (g2048[i] == 0) empty[n++] = i;
    if (n == 0) return;
    int idx = empty[games_rand_next() % (uint32_t)n];
    g2048[idx] = (games_rand_next() % 10 == 0) ? 4 : 2;
}

/* 一行 4 格左滑：压缩 -> 合并 -> 再压缩；返回该行是否发生变化 */
static int g2048_slide_left(uint32_t *row) {
    uint32_t orig[4];
    for (int i = 0; i < 4; i++) orig[i] = row[i];
    int oi = 0;
    for (int i = 0; i < 4; i++) if (row[i]) row[oi++] = row[i];
    for (int i = oi; i < 4; i++) row[i] = 0;
    for (int i = 0; i < 3; i++) {
        if (row[i] && row[i] == row[i + 1]) {
            row[i] *= 2;
            g2048_score += row[i];
            for (int j = i + 1; j < 3; j++) row[j] = row[j + 1];
            row[3] = 0;
        }
    }
    for (int i = 0; i < 4; i++) if (row[i] != orig[i]) return 1;
    return 0;
}

static void g2048_get_row(uint32_t *row, int r) {
    for (int c = 0; c < 4; c++) row[c] = g2048[r * 4 + c];
}

static void g2048_set_row(const uint32_t *row, int r) {
    for (int c = 0; c < 4; c++) g2048[r * 4 + c] = row[c];
}

static void g2048_get_col(uint32_t *col, int c) {
    for (int r = 0; r < 4; r++) col[r] = g2048[r * 4 + c];
}

static void g2048_set_col(const uint32_t *col, int c) {
    for (int r = 0; r < 4; r++) g2048[r * 4 + c] = col[r];
}

static void g2048_move(int dir) {
    // 0 up 1 down 2 left 3 right
    g2048_moved = 0;
    uint32_t line[4], rev[4];
    for (int i = 0; i < 4; i++) {
        if (dir == 2 || dir == 3) {
            g2048_get_row(line, i);
            if (dir == 3) {
                for (int k = 0; k < 4; k++) rev[k] = line[3 - k];
                int m = g2048_slide_left(rev);
                for (int k = 0; k < 4; k++) line[k] = rev[3 - k];
                g2048_set_row(line, i);
                g2048_moved = g2048_moved || m;
            } else {
                g2048_moved = g2048_slide_left(line) || g2048_moved;
                g2048_set_row(line, i);
            }
        } else {
            g2048_get_col(line, i);
            if (dir == 1) {
                for (int k = 0; k < 4; k++) rev[k] = line[3 - k];
                int m = g2048_slide_left(rev);
                for (int k = 0; k < 4; k++) line[k] = rev[3 - k];
                g2048_set_col(line, i);
                g2048_moved = g2048_moved || m;
            } else {
                g2048_moved = g2048_slide_left(line) || g2048_moved;
                g2048_set_col(line, i);
            }
        }
    }
}

static int g2048_can_move(void) {
    for (int i = 0; i < 16; i++) if (g2048[i] == 0) return 1;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) {
            if (c < 3 && g2048[r * 4 + c] == g2048[r * 4 + c + 1]) return 1;
            if (r < 3 && g2048[r * 4 + c] == g2048[(r + 1) * 4 + c]) return 1;
        }
    return 0;
}

static void g2048_render(void) {
    if (g_gfx) { games_yield(); return; }   /* GUI 图形渲染由桌面窗口完成 */
    games_clear();
    ezos_console_write("=== 2048 ===\n");
    ezos_console_write("WASD to move, Q to quit. Score: ");
    ezos_console_print_dec(g2048_score);
    ezos_console_write("\n");
    for (int r = 0; r < 4; r++) {
        ezos_console_write("+----+----+----+----+\n");
        ezos_console_write("|");
        for (int c = 0; c < 4; c++) {
            uint32_t v = g2048[r * 4 + c];
            if (v == 0) {
                ezos_console_write("    ");
            } else if (v < 10) {
                ezos_console_write("   ");
                ezos_console_print_dec(v);
            } else if (v < 100) {
                ezos_console_write("  ");
                ezos_console_print_dec(v);
            } else if (v < 1000) {
                ezos_console_write(" ");
                ezos_console_print_dec(v);
            } else {
                ezos_console_print_dec(v);
            }
            ezos_console_putchar('|');
        }
        ezos_console_write("\n");
    }
    ezos_console_write("+----+----+----+----+\n");
}

static void game_2048(void) {
    games_rand_seed(games_rdtsc() + 13);
    for (int i = 0; i < 16; i++) g2048[i] = 0;
    g2048_score = 0;
    g2048_over = 0;
    g2048_spawn();
    g2048_spawn();
    g2048_render();
    int quit = 0;
    while (!quit) {
        int c;
        /* 阻塞等待一输入：键盘任意键或鼠标点击（方向 0..3 由窗口重心换算） */
        for (;;) {
            int ma, mb;
            if (games_mouse_poll(&ma, &mb)) {
                c = (ma == 0) ? 'w' : (ma == 1) ? 's' : (ma == 2) ? 'a' : 'd';
                break;
            }
            c = keyboard_getchar();
            if (c != 0) break;
            games_yield();
        }
        if (c == 'w' || c == 'W') g2048_move(0);
        else if (c == 's' || c == 'S') g2048_move(1);
        else if (c == 'a' || c == 'A') g2048_move(2);
        else if (c == 'd' || c == 'D') g2048_move(3);
        else if (c == 'q' || c == 'Q') { quit = 1; break; }
        else continue;
        if (g2048_moved) g2048_spawn();
        g2048_render();
        if (!g2048_can_move()) {
            g2048_over = 1;
            ezos_console_write("Game Over!\n");
            break;
        }
    }
    ezos_console_write("Press Enter to return...\n");
    char buf[8];
    games_readline(buf, sizeof(buf));
}

/* ---------------- 游戏 5：扫雷 ---------------- */
#define MS_W 9
#define MS_H 9
static uint8_t  ms_board[MS_W * MS_H];   // 0-8 周围雷数，9=雷
static int      ms_revealed[MS_W * MS_H];
static int      ms_flagged[MS_W * MS_H];
static int      ms_ready;
static int      ms_left;                 // 剩余未翻开非雷格数
static int      ms_lose;
static int      ms_cx = 4, ms_cy = 4;    // GUI 模式光标（方向键导航）

static void ms_place_mines(int safe_idx) {
    for (int i = 0; i < MS_W * MS_H; i++) {
        ms_board[i] = 0;
        ms_revealed[i] = 0;
        ms_flagged[i] = 0;
    }
    int placed = 0;
    while (placed < 10) {
        int idx = (int)(games_rand_next() % (MS_W * MS_H));
        if (idx == safe_idx || ms_board[idx] == 9) continue;
        ms_board[idx] = 9;
        placed++;
    }
    for (int r = 0; r < MS_H; r++) {
        for (int c = 0; c < MS_W; c++) {
            if (ms_board[r * MS_W + c] == 9) continue;
            int n = 0;
            for (int dr = -1; dr <= 1; dr++)
                for (int dc = -1; dc <= 1; dc++) {
                    int nr = r + dr, nc = c + dc;
                    if (nr < 0 || nr >= MS_H || nc < 0 || nc >= MS_W) continue;
                    if (ms_board[nr * MS_W + nc] == 9) n++;
                }
            ms_board[r * MS_W + c] = (uint8_t)n;
        }
    }
}

static void ms_flood(int r, int c) {
    if (r < 0 || r >= MS_H || c < 0 || c >= MS_W) return;
    int idx = r * MS_W + c;
    if (ms_revealed[idx] || ms_flagged[idx] || ms_board[idx] == 9) return;
    ms_revealed[idx] = 1;
    ms_left--;
    if (ms_board[idx] != 0) return;
    for (int dr = -1; dr <= 1; dr++)
        for (int dc = -1; dc <= 1; dc++)
            if (dr || dc) ms_flood(r + dr, c + dc);
}

static void ms_render(void) {
    if (g_gfx) { games_yield(); return; }   /* GUI 图形渲染由桌面窗口完成 */
    games_clear();
    ezos_console_write("=== Minesweeper ===\n");
    ezos_console_write("Input: 'r c' reveal, 'f r c' flag, 'q' quit\n");
    ezos_console_write("   1 2 3 4 5 6 7 8 9\n");
    for (int r = 0; r < MS_H; r++) {
        ezos_console_putchar((uint8_t)('1' + r));
        ezos_console_putchar(' ');
        for (int c = 0; c < MS_W; c++) {
            int idx = r * MS_W + c;
            if (ms_flagged[idx]) {
                ezos_console_putchar('F');
            } else if (!ms_revealed[idx]) {
                ezos_console_putchar('.');
            } else if (ms_board[idx] == 9) {
                ezos_console_putchar('*');
            } else if (ms_board[idx] == 0) {
                ezos_console_putchar(' ');
            } else {
                ezos_console_putchar((uint8_t)('0' + ms_board[idx]));
            }
            ezos_console_putchar(' ');
        }
        ezos_console_write("\n");
    }
}

/* GUI 模式按键轮询：方向键移动光标，Enter/Space 翻开，F 插旗，Q 退出。
 * 返回 0=无输入 1=移动 2=翻开 3=插旗 4=退出 */
static int ms_poll_key_gfx(void)
{
    for (;;) {
        int ma, mb;
        int mbtn = games_mouse_poll(&ma, &mb);
        if (mbtn) {
            if (ma < 0 || ma >= MS_H || mb < 0 || mb >= MS_W) return 1;
            ms_cx = mb; ms_cy = ma;
            return (mbtn == 2) ? 3 : 2;   /* 右键=插旗 左键=翻开 */
        }
        int c = keyboard_getchar();
        if (c == 0) { games_yield(); return 0; }
        if (c == KEY_UP) { if (ms_cy > 0) ms_cy--; return 1; }
        if (c == KEY_DOWN) { if (ms_cy < MS_H - 1) ms_cy++; return 1; }
        if (c == KEY_LEFT) { if (ms_cx > 0) ms_cx--; return 1; }
        if (c == KEY_RIGHT) { if (ms_cx < MS_W - 1) ms_cx++; return 1; }
        if (c == '\n' || c == '\r' || c == ' ') return 2;
        if (c == 'f' || c == 'F') return 3;
        if (c == 'q' || c == 'Q') return 4;
        games_yield();
    }
}

static void game_minesweeper(void) {
    games_rand_seed(games_rdtsc() + 29);
    ms_ready = 0;
    ms_lose = 0;
    ms_left = MS_W * MS_H - 10;
    ms_cx = 4; ms_cy = 4;
    if (g_gfx) {
        /* GUI 图形模式：方向键导航 + Enter 翻开 + F 插旗 + Q 退出 */
        games_yield();   /* 首绘 */
        for (;;) {
            int act = ms_poll_key_gfx();
            if (act == 0 || act == 1) continue;      /* 移动由 yield 重绘 */
            if (act == 4) break;                     /* 退出 */
            int idx = ms_cy * MS_W + ms_cx;
            if (act == 3) {
                if (!ms_revealed[idx]) ms_flagged[idx] = !ms_flagged[idx];
                games_yield();
                continue;
            }
            /* act == 2 翻开 */
            if (ms_flagged[idx] || ms_revealed[idx]) { games_yield(); continue; }
            if (!ms_ready) { ms_place_mines(idx); ms_ready = 1; }
            if (ms_board[idx] == 9) {
                ms_revealed[idx] = 1;
                ms_lose = 1;
                games_yield();
                break;
            }
            ms_flood(ms_cy, ms_cx);
            games_yield();
            if (ms_left == 0) break;
        }
        games_yield();   /* 终局画面（雷/胜利） */
        while (keyboard_getchar() != 0) { }
        while (keyboard_getchar() == 0) games_yield();
        return;
    }
    ms_render();
    char buf[16];
    for (;;) {
        ezos_console_write(">> ");
        games_readline(buf, sizeof(buf));
        if (buf[0] == 'q' || buf[0] == 'Q') break;
        int flag = 0;
        const char *p = buf;
        if (p[0] == 'f' || p[0] == 'F') { flag = 1; p++; }
        while (*p == ' ') p++;
        int r = games_atoi(p) - 1;
        const char *sp = games_strchr(p, ' ');
        int c = sp ? games_atoi(sp + 1) - 1 : -1;
        if (r < 0 || r >= MS_H || c < 0 || c >= MS_W) {
            ezos_console_write("Invalid\n");
            continue;
        }
        int idx = r * MS_W + c;
        if (!ms_ready) {
            ms_place_mines(idx);
            ms_ready = 1;
        }
        if (flag) {
            if (!ms_revealed[idx]) ms_flagged[idx] = !ms_flagged[idx];
            ms_render();
            continue;
        }
        if (ms_flagged[idx] || ms_revealed[idx]) {
            ezos_console_write("Already open/flagged\n");
            continue;
        }
        if (ms_board[idx] == 9) {
            ms_revealed[idx] = 1;
            ms_lose = 1;
            ms_render();
            ezos_console_write("BOOM! You hit a mine.\n");
            break;
        }
        ms_flood(r, c);
        ms_render();
        if (ms_left == 0) {
            ezos_console_write("You win!\n");
            break;
        }
    }
    ezos_console_write("Press Enter to return...\n");
    games_readline(buf, sizeof(buf));
}

/* ---------------- 游戏 6：石头剪刀布 ---------------- */
static int rps_pwins = 0, rps_cwins = 0, rps_draws = 0;
static int rps_last_p = 0, rps_last_c = 0, rps_last_d = -1;   /* d: 0=平 1=胜 2=负 */

static void game_rps(void) {
    games_clear();
    ezos_console_write("=== Rock Paper Scissors ===\n");
    ezos_console_write("1=Rock 2=Paper 3=Scissors 0=Quit\n");
    rps_pwins = 0; rps_cwins = 0; rps_draws = 0;
    rps_last_p = 0; rps_last_c = 0; rps_last_d = -1;
    static const char *names[4] = { "", "Rock", "Paper", "Scissors" };
    char buf[16];
    if (g_gfx) {
        /* GUI 图形模式：1/2/3 直接出拳，0/Q 退出，界面由桌面窗口绘制 */
        games_yield();
        for (;;) {
            int c;
            int ma, mb;
            if (games_mouse_poll(&ma, &mb)) {
                if (ma >= 1 && ma <= 3) c = '0' + ma;
                else { games_yield(); continue; }
            } else {
                c = keyboard_getchar();
                if (c == 0) { games_yield(); continue; }
            }
            if (c == '1') rps_last_p = 1;
            else if (c == '2') rps_last_p = 2;
            else if (c == '3') rps_last_p = 3;
            else if (c == '0' || c == 'q' || c == 'Q') break;
            else { games_yield(); continue; }
            rps_last_c = (int)(games_rand_next() % 3) + 1;
            int d = (rps_last_p - rps_last_c + 3) % 3;   /* 1: p 胜 */
            if (d == 0) { rps_draws++; rps_last_d = 0; }
            else if (d == 1) { rps_pwins++; rps_last_d = 1; }
            else { rps_cwins++; rps_last_d = 2; }
            games_yield();
        }
        while (keyboard_getchar() != 0) { }
        while (keyboard_getchar() == 0) games_yield();
        return;
    }
    for (;;) {
        ezos_console_print_dec((uint32_t)rps_pwins);
        ezos_console_write("-");
        ezos_console_print_dec((uint32_t)rps_cwins);
        ezos_console_write("-");
        ezos_console_print_dec((uint32_t)rps_draws);
        ezos_console_write("  Choose: ");
        games_readline(buf, sizeof(buf));
        int p = games_atoi(buf);
        if (p == 0) break;
        if (p < 1 || p > 3) {
            ezos_console_write("Enter 1/2/3\n");
            continue;
        }
        int c = (int)(games_rand_next() % 3) + 1;
        int d = (p - c + 3) % 3;   // 1: p 胜
        ezos_console_write("You: ");
        ezos_console_write(names[p]);
        ezos_console_write("  CPU: ");
        ezos_console_write(names[c]);
        ezos_console_write("  ");
        if (d == 0) { ezos_console_write("Draw\n"); rps_draws++; }
        else if (d == 1) { ezos_console_write("You win\n"); rps_pwins++; }
        else { ezos_console_write("CPU wins\n"); rps_cwins++; }
    }
}

/* ---------------- 游戏 7：记忆翻牌 ---------------- */
#define MEM_ROWS 4
#define MEM_COLS 4
static char  mem_cards[16];
static int   mem_face[16];   // 已配对
static char  mem_show[16];   // 当前翻开的临时显示
static int   mem_flips;
static int   mem_cx = 0, mem_cy = 0;   // GUI 模式光标

static void mem_render(void) {
    if (g_gfx) { games_yield(); return; }   /* GUI 图形渲染由桌面窗口完成 */
    games_clear();
    ezos_console_write("=== Memory Match ===\n");
    ezos_console_write("Flip count: ");
    ezos_console_print_dec((uint32_t)mem_flips);
    ezos_console_write("   Rows/Cols: 1..4\n");
    for (int r = 0; r < MEM_ROWS; r++) {
        for (int c = 0; c < MEM_COLS; c++) {
            int idx = r * MEM_COLS + c;
            ezos_console_putchar(' ');
            if (mem_face[idx]) ezos_console_putchar((uint8_t)mem_cards[idx]);
            else if (mem_show[idx]) ezos_console_putchar((uint8_t)mem_show[idx]);
            else ezos_console_putchar('.');
            ezos_console_putchar(' ');
        }
        ezos_console_write("\n");
    }
}

/* GUI 模式按键轮询：方向键移动光标，Enter/Space 翻牌，Q 退出。
 * 返回 0=无输入 1=移动 2=翻牌 3=退出 */
static int mem_poll_key_gfx(void)
{
    for (;;) {
        int ma, mb;
        if (games_mouse_poll(&ma, &mb)) {
            if (ma < 0 || ma >= MEM_ROWS || mb < 0 || mb >= MEM_COLS) return 1;
            mem_cx = mb; mem_cy = ma;
            return 2;   /* 点击翻牌 */
        }
        int c = keyboard_getchar();
        if (c == 0) { games_yield(); return 0; }
        if (c == KEY_UP) { if (mem_cy > 0) mem_cy--; return 1; }
        if (c == KEY_DOWN) { if (mem_cy < MEM_ROWS - 1) mem_cy++; return 1; }
        if (c == KEY_LEFT) { if (mem_cx > 0) mem_cx--; return 1; }
        if (c == KEY_RIGHT) { if (mem_cx < MEM_COLS - 1) mem_cx++; return 1; }
        if (c == '\n' || c == '\r' || c == ' ') return 2;
        if (c == 'q' || c == 'Q') return 3;
        games_yield();
    }
}

static void game_memory(void) {
    games_rand_seed(games_rdtsc() + 37);
    for (int i = 0; i < 8; i++) {
        mem_cards[i * 2] = (char)('A' + i);
        mem_cards[i * 2 + 1] = (char)('A' + i);
    }
    // Fisher-Yates 洗牌
    for (int i = 15; i > 0; i--) {
        int j = (int)(games_rand_next() % (uint32_t)(i + 1));
        char t = mem_cards[i]; mem_cards[i] = mem_cards[j]; mem_cards[j] = t;
    }
    for (int i = 0; i < 16; i++) { mem_face[i] = 0; mem_show[i] = 0; }
    mem_flips = 0;
    mem_cx = 0; mem_cy = 0;
    int matched = 0;
    char buf[16];
    if (g_gfx) {
        /* GUI 图形模式：方向键导航 + Enter 翻牌，不匹配自动翻回 */
        int first = -1;
        games_yield();   /* 首绘 */
        while (matched < 8) {
            int act = mem_poll_key_gfx();
            if (act == 0 || act == 1) continue;      /* 移动由 yield 重绘 */
            if (act == 3) break;                     /* 退出 */
            int idx = mem_cy * MEM_COLS + mem_cx;
            if (mem_face[idx] || mem_show[idx]) { games_yield(); continue; }
            mem_flips++;
            mem_show[idx] = mem_cards[idx];
            if (first < 0) { first = idx; games_yield(); continue; }
            if (first == idx) { mem_show[idx] = 0; first = -1; games_yield(); continue; }
            games_yield();   /* 显示第二张 */
            if (mem_cards[first] == mem_cards[idx]) {
                mem_face[first] = 1; mem_face[idx] = 1;
                mem_show[first] = 0; mem_show[idx] = 0;
                matched++;
                first = -1;
            } else {
                /* 不匹配：停留约 1.2s 后自动翻回（期间忽略输入） */
                uint32_t t0 = games_rdtsc();
                while ((uint32_t)(games_rdtsc() - t0) < 120000000u) {
                    while (keyboard_getchar() != 0) { }
                    games_yield();
                }
                mem_show[first] = 0;
                mem_show[idx] = 0;
                first = -1;
            }
        }
        games_yield();
        while (keyboard_getchar() != 0) { }
        while (keyboard_getchar() == 0) games_yield();
        return;
    }
    while (matched < 8) {
        mem_render();
        int first = -1;
        for (int i = 0; i < 16; i++) if (mem_show[i]) { first = i; break; }
        ezos_console_write("Flip (r c): ");
        games_readline(buf, sizeof(buf));
        if (buf[0] == 'q' || buf[0] == 'Q') break;
        int r = games_atoi(buf) - 1;
        const char *sp = games_strchr(buf, ' ');
        int c = sp ? games_atoi(sp + 1) - 1 : -1;
        if (r < 0 || r >= MEM_ROWS || c < 0 || c >= MEM_COLS) {
            ezos_console_write("Invalid\n");
            continue;
        }
        int idx = r * MEM_COLS + c;
        if (mem_face[idx] || mem_show[idx]) {
            ezos_console_write("Already open\n");
            continue;
        }
        mem_flips++;
        mem_show[idx] = mem_cards[idx];
        if (first < 0) continue;
        if (first == idx) { mem_show[idx] = 0; continue; }
        mem_render();
        if (mem_cards[first] == mem_cards[idx]) {
            mem_face[first] = 1;
            mem_face[idx] = 1;
            mem_show[first] = 0;
            mem_show[idx] = 0;
            matched++;
            ezos_console_write("Match!\n");
        } else {
            ezos_console_write("No match. Press Enter...\n");
            games_readline(buf, sizeof(buf));
            mem_show[first] = 0;
            mem_show[idx] = 0;
        }
    }
    mem_render();
    ezos_console_write("You win in ");
    ezos_console_print_dec((uint32_t)mem_flips);
    ezos_console_write(" flips!\n");
    ezos_console_write("Press Enter to return...\n");
    games_readline(buf, sizeof(buf));
}

/* ---------------- 游戏菜单 ---------------- */
static int games_menu_select(void) {
    games_clear();
    for (;;) {
        ezos_console_write("=== EZOS Games ===\n");
        ezos_console_write("1. Guess the Number\n");
        ezos_console_write("2. Tic-Tac-Toe\n");
        ezos_console_write("3. Snake\n");
        ezos_console_write("4. 2048\n");
        ezos_console_write("5. Minesweeper\n");
        ezos_console_write("6. Rock Paper Scissors\n");
        ezos_console_write("7. Memory Match\n");
        ezos_console_write("0. Back to shell\n");
        ezos_console_write("Choose: ");
        char buf[8];
        games_readline(buf, sizeof(buf));
        if (games_streq(buf, "1")) return 1;
        else if (games_streq(buf, "2")) return 2;
        else if (games_streq(buf, "3")) return 3;
        else if (games_streq(buf, "4")) return 4;
        else if (games_streq(buf, "5")) return 5;
        else if (games_streq(buf, "6")) return 6;
        else if (games_streq(buf, "7")) return 7;
        else if (games_streq(buf, "0") || games_streq(buf, "q")) return 0;
        else { ezos_console_write("Invalid choice.\n"); games_yield(); }
    }
}

/* GUI 桌面模式：直接运行指定游戏（1..7），游戏结束后返回 */
void games_play(int choice) {
    g_gfx_kind = choice;
    switch (choice) {
        case 1: game_guess(); break;
        case 2: game_tic(); break;
        case 3: game_snake(); break;
        case 4: game_2048(); break;
        case 5: game_minesweeper(); break;
        case 6: game_rps(); break;
        case 7: game_memory(); break;
        default: break;
    }
}

/* ---------------- GUI 渲染状态导出（供 gfxwin.c 图形化绘制） ---------------- */
int  games_gfx_kind(void)  { return g_gfx_kind; }

int  games_tic_board(int i)          { return tic_board[i]; }
int  games_tic_winner(void)          { return tic_winner(); }

int  games_snake_len(void)           { return snake_len; }
int  games_snake_alive(void)         { return snake_alive; }
int  games_snake_body_x(int i)       { return snake_body[i][0]; }
int  games_snake_body_y(int i)       { return snake_body[i][1]; }
int  games_snake_food_x(void)        { return food_x; }
int  games_snake_food_y(void)        { return food_y; }

int  games_2048_cell(int i)          { return (int)g2048[i]; }
int  games_2048_score(void)          { return (int)g2048_score; }
int  games_2048_over(void)           { return g2048_over; }

int  games_ms_cell(int r, int c)     { return ms_board[r * MS_W + c]; }
int  games_ms_revealed(int r, int c) { return ms_revealed[r * MS_W + c]; }
int  games_ms_flagged(int r, int c)  { return ms_flagged[r * MS_W + c]; }
int  games_ms_cursor_x(void)         { return ms_cx; }
int  games_ms_cursor_y(void)         { return ms_cy; }
int  games_ms_lose(void)             { return ms_lose; }
int  games_ms_left(void)             { return ms_left; }

int  games_mem_cell(int i)           { return (int)mem_cards[i]; }
int  games_mem_face(int i)           { return mem_face[i]; }
int  games_mem_show(int i)           { return (int)mem_show[i]; }
int  games_mem_flips(void)           { return mem_flips; }
int  games_mem_cursor_x(void)        { return mem_cx; }
int  games_mem_cursor_y(void)        { return mem_cy; }

int  games_rps_pwins(void)           { return rps_pwins; }
int  games_rps_cwins(void)           { return rps_cwins; }
int  games_rps_draws(void)           { return rps_draws; }
int  games_rps_last_p(void)          { return rps_last_p; }
int  games_rps_last_c(void)          { return rps_last_c; }
int  games_rps_last_d(void)          { return rps_last_d; }

/* ---------------- shell 命令入口：games ---------------- */
/* GUI 宿主启动器（gfxwin.c 注册）：fn(0)=图形菜单，fn(1..7)=直接运行游戏。
 * 非空说明处于图形桌面；内核 shell 阶段为 NULL，命令走文本菜单。 */
static int (*g_gui_launch)(int idx) = NULL;

void games_set_gui_launcher(int (*fn)(int)) { g_gui_launch = fn; }

int games_gui_launch(int idx) { return g_gui_launch ? g_gui_launch(idx) : 0; }

void cmd_games(const char *args) {
    (void)args;
    if (games_gui_launch(0)) return;   /* 图形桌面：打开图形游戏菜单 */
    for (;;) {
        int sel = games_menu_select();
        if (sel == 0) { ezos_console_write("Back to shell.\n"); break; }
        games_play(sel);
    }
}
