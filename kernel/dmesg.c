/*
 * dmesg.c - 内核日志环形缓冲实现（v2，语义清晰版）
 *
 * 模型：dm_buf 是 32KB 环；逻辑上永远存"最近 dm_lines_cnt 行"。
 * 每行记录 [start_seq, len)：start_seq 是"逻辑流偏移"（单调递增），
 * 物理位置 = start_seq % DM_BUF_SIZE。逻辑流不断前进，环自然覆盖旧数据。
 * 行索引 dm_line[] 按 FIFO 存 {start_seq, len}，满 256 行丢最旧。
 *
 * 特性刻意独立：不依赖 kmalloc/磁盘/中断，panic 路径可安全调用。
 */
#include "dmesg.h"

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

void dmesg_write(const char *line) {
    if (!line) return;
    for (uint32_t i = 0; line[i]; i++) {
        char c = line[i];
        if (c == '\n') { dm_push_line(); continue; }
        if (dm_cur_len < DM_MAX_LINE) dm_cur[dm_cur_len++] = c;
        /* 超长行静默截断 */
    }
}

static char dm_char(uint32_t idx, uint32_t k) {
    return dm_buf[(dm_line[idx].start + k) % DM_BUF_SIZE];
}

int dmesg_dump(void (*putchar_fn)(char), int n) {
    if (!putchar_fn || dm_cnt == 0) return 0;
    uint32_t start = 0;
    if (n > 0 && (uint32_t)n < dm_cnt) start = dm_cnt - n;
    int out = 0;
    for (uint32_t i = start; i < dm_cnt; i++) {
        uint32_t idx = (dm_first + i) % DM_MAX_LINES;
        for (uint32_t k = 0; k < dm_line[idx].len; k++)
            putchar_fn(dm_char(idx, k));
        out++;
    }
    return out;
}

int dmesg_tail(char *out, uint32_t outsz, int n) {
    if (!out || outsz == 0 || dm_cnt == 0) return 0;
    uint32_t start = 0;
    if (n > 0 && (uint32_t)n < dm_cnt) start = dm_cnt - n;
    uint32_t o = 0;
    for (uint32_t i = start; i < dm_cnt && o + 1 < outsz; i++) {
        uint32_t idx = (dm_first + i) % DM_MAX_LINES;
        for (uint32_t k = 0; k < dm_line[idx].len && o + 1 < outsz; k++)
            out[o++] = dm_char(idx, k);
    }
    out[o] = 0;
    return (int)o;
}

uint32_t dmesg_lines(void) { return dm_cnt; }
