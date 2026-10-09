/*
 * dmesg.c - 内核日志环形缓冲实现（v2，语义清晰版）
 *
 * 模型：dm_buf 是 32KB 环；逻辑上永远存"最近 dm_lines_cnt 行"。
 * 每行记录 [start_seq, len)：start_seq 是"逻辑流偏移"（单调递增），
 * 物理位置 = start_seq % DM_BUF_SIZE。逻辑流不断前进，环自然覆盖旧数据。
 * 行索引 dm_line[] 按 FIFO 存 {start_seq, len}，满 256 行丢最旧。
 *
 * 特性刻意独立：不依赖 kmalloc/磁盘，panic 路径可安全调用。
 *
 * 并发（P1 补漏）
 * --------------
 * dmesg_write() 是唯一被 **IRQ 上下文** 与 **进程上下文** 同时调用的输出
 * 通道：net/rtl8139（IRQ11）、ehci/nvme/usb 的中断路径与 shell 都往这打。
 * 原先完全无锁，后果有两层：
 *   1) 串口侧两行字符交错 -> 按行断言的 E2E（本项目的验证回路全靠它）
 *      会读到半截行/混合行，表现为难以复现的假红假绿；
 *   2) 环形缓冲的 dm_seq / dm_line[] / dm_cur 是"先改游标再改数据"的多步
 *      更新，被打断会让行索引记住已被环覆盖的旧偏移，dmesg 回看吐垃圾。
 * 单核下关中断同时关掉抢占，足够保护；irq_save_disable/irq_restore 可安全
 * 嵌套（panic 路径、已持锁路径都能调）。serial_write 内部的 lsr_wait 有
 * 轮询上限（serial.c:35），关中断期间不会死等。
 *
 * 读路径（dump/tail）刻意**不**整段关中断：整屏输出要写几万次 VGA，
 * 关中断几十毫秒会丢中断。改成"逐行加锁拷快照、锁外输出"。
 */
#include "dmesg.h"
#include "serial.h"
#include "irqflags.h"

#define DM_BUF_SIZE  32768u
#define DM_MAX_LINES 256u
#define DM_MAX_LINE  240u        /* 单行截断上限（不含 \n） */

/* 32KB 环 + 4KB 索引放高内存段（低 640KB 区 BSS 已紧张，见 linker.ld ASSERT） */
#define DM_HIBUF __attribute__((section(".bss.hi")))
static char dm_buf[DM_BUF_SIZE] DM_HIBUF;
static uint64_t dm_seq;                      /* 逻辑流游标：下一字节的位置 */

typedef struct { uint64_t start; uint32_t len; } dm_line_t;
static dm_line_t dm_line[DM_MAX_LINES] DM_HIBUF;  /* FIFO 行索引 */
static uint32_t dm_first;                    /* 最旧行下标 */
static uint32_t dm_cnt;
static uint32_t dm_bytes;                    /* 索引行占用总字节（<= DM_BUF_SIZE） */

/* 正在积累的行 */
static char dm_cur[DM_MAX_LINE];
static uint32_t dm_cur_len;

static void dm_push_line(void) {
    uint32_t need = dm_cur_len + 1;          /* 含 \n */
    if (need > DM_BUF_SIZE) { dm_cur_len = 0; return; }

    /* 关键：先按"字节容量"淘汰，再按"行数上限"淘汰。
     *
     * 只按行数淘汰是不够的——256 行 × 最长 240B = 61,440B 远超 32KB 缓冲，
     * 平均行长超过 127B 时索引会记住"数据已被环覆盖"的旧行，dmesg 回看
     * 就会吐出垃圾。所以必须维护 dm_bytes，保证索引行的字节和 <= 缓冲。 */
    while (dm_cnt > 0 && dm_bytes + need > DM_BUF_SIZE) {
        dm_bytes -= dm_line[dm_first].len;
        dm_first = (dm_first + 1) % DM_MAX_LINES;
        dm_cnt--;
    }
    /* 行索引满：丢最旧（行长很短时先撞上这个上限） */
    if (dm_cnt == DM_MAX_LINES) {
        dm_bytes -= dm_line[dm_first].len;
        dm_first = (dm_first + 1) % DM_MAX_LINES;
        dm_cnt--;
    }

    uint32_t slot = (dm_first + dm_cnt) % DM_MAX_LINES;
    dm_line[slot].start = dm_seq;
    dm_line[slot].len = need;

    for (uint32_t k = 0; k < dm_cur_len; k++)
        dm_buf[(dm_seq + k) % DM_BUF_SIZE] = dm_cur[k];
    dm_buf[(dm_seq + dm_cur_len) % DM_BUF_SIZE] = '\n';

    dm_seq += need;
    dm_cnt++;
    dm_bytes += need;
    dm_cur_len = 0;
}

static void dmesg_push(const char *line, int echo_serial) {
    if (!line) return;
    /* 串口镜像（真机诊断通道）：serial 未 init/自检失败时内部 no-op。
     * 放在环形缓冲之前——环形缓冲裁剪不影响串口侧拿到完整行。
     * 与缓冲更新同处一个临界区：否则串口侧的两行字符会交错。 */
    uint32_t f = irq_save_disable();
    if (echo_serial) {
        serial_write(line);
        /* 只在调用方**没带**换行时补一个。klog 的 dm_mirror 和 st_report
         * 传进来的行都以 
 结尾，以前无条件再补一个 —— 串口日志里每行
         * 后面跟一个空行，看着就像输出被打散了。 */
        uint32_t n = 0;
        while (line[n]) n++;
        if (n == 0 || line[n - 1] != '\n') serial_putc('\n');
    }
    for (uint32_t i = 0; line[i]; i++) {
        char c = line[i];
        if (c == '\n') { dm_push_line(); continue; }
        if (dm_cur_len < DM_MAX_LINE) dm_cur[dm_cur_len++] = c;
        /* 超长行静默截断 */
    }
    irq_restore(f);
}

void dmesg_write(const char *line) {
    dmesg_push(line, 1);
}

/* 只进环形缓冲、**不**写串口 —— 给"已经上过屏"的调用方用。
 *
 * terminal_putchar 早已把控制台输出逐字符镜像到 COM1（H1a，真机无屏时的
 * 诊断通道，也是 E2E 唯一的取结果通道）。klog / st_report 都是"先上屏、
 * 再记 dmesg"，这里再往串口写一遍，串口日志上每一行都会出现两次
 * （实测开机日志：24 条自检 + 每条的 klog 汇总，全是双份，两份的时间戳
 * 还不一样，因为 dm_mirror 是**事后**重新取的时间）。
 * dmesg 命令回看不受影响 —— 走的还是同一个环形缓冲。 */
void dmesg_record(const char *line) {
    dmesg_push(line, 0);
}

static char dm_char(uint32_t idx, uint32_t k) {
    return dm_buf[(dm_line[idx].start + k) % DM_BUF_SIZE];
}

/* 在临界区内把第 idx 行拷进调用方缓冲，返回拷贝长度。
 * 读路径靠它做到"关中断只在单行拷贝期间"，避免整屏输出长时间关中断。 */
static uint32_t dm_copy_line(uint32_t idx, char *out, uint32_t cap) {
    uint32_t f = irq_save_disable();
    uint32_t len = dm_line[idx].len;
    if (len > cap) len = cap;
    for (uint32_t k = 0; k < len; k++) out[k] = dm_char(idx, k);
    irq_restore(f);
    return len;
}

/* 快照当前行范围（写路径随时可能追加新行/淘汰旧行，这里只负责"此刻"的行）。
 * dm_first 必须一起快照：否则遍历期间它被推进，idx 会算到错误的表项。 */
static void dm_snapshot(uint32_t *first_out, uint32_t *start_out,
                        uint32_t *cnt_out, int n) {
    uint32_t f = irq_save_disable();
    uint32_t c = dm_cnt;
    uint32_t start = 0;
    if (n > 0 && (uint32_t)n < c) start = c - (uint32_t)n;
    *first_out = dm_first;
    *start_out = start;
    *cnt_out = c;
    irq_restore(f);
}

int dmesg_dump(void (*putchar_fn)(char), int n) {
    if (!putchar_fn) return 0;
    uint32_t first, start, cnt;
    dm_snapshot(&first, &start, &cnt, n);
    if (cnt == 0) return 0;
    char row[DM_MAX_LINE + 2];
    int out = 0;
    for (uint32_t i = start; i < cnt; i++) {
        uint32_t idx = (first + i) % DM_MAX_LINES;
        uint32_t len = dm_copy_line(idx, row, DM_MAX_LINE + 1);
        for (uint32_t k = 0; k < len; k++) putchar_fn(row[k]);
        out++;
    }
    return out;
}

int dmesg_tail(char *out, uint32_t outsz, int n) {
    if (!out || outsz == 0) return 0;
    uint32_t first, start, cnt;
    dm_snapshot(&first, &start, &cnt, n);
    if (cnt == 0) return 0;
    char row[DM_MAX_LINE + 2];
    uint32_t o = 0;
    for (uint32_t i = start; i < cnt && o + 1 < outsz; i++) {
        uint32_t idx = (first + i) % DM_MAX_LINES;
        uint32_t len = dm_copy_line(idx, row, DM_MAX_LINE + 1);
        for (uint32_t k = 0; k < len && o + 1 < outsz; k++) out[o++] = row[k];
    }
    out[o] = 0;
    return (int)o;
}

uint32_t dmesg_lines(void) { return dm_cnt; }
