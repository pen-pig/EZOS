/*
 * pmm.c - 物理页帧分配器（步骤 5a）
 *
 * 位图语义：bit=1 表示"已分配"，bit=0 表示"空闲"。
 * 位图放 .bss（内核启动时随 .bss 一并清零 = 全部空闲）。
 */
#include "pmm.h"
#include "irqflags.h"

#define BITS_PER_WORD   32u
#define BITMAP_WORDS    ((PMM_POOL_PAGES + BITS_PER_WORD - 1u) / BITS_PER_WORD)

static uint32_t g_bitmap[BITMAP_WORDS];
static uint32_t g_used;          /* 已分配页数（统计，避免每次遍历位图） */
static int      g_ready;

/* ---------- 位图原语 ---------- */

static int bit_get(uint32_t i) {
    return (g_bitmap[i / BITS_PER_WORD] >> (i % BITS_PER_WORD)) & 1u;
}

static void bit_set(uint32_t i) {
    g_bitmap[i / BITS_PER_WORD] |= (1u << (i % BITS_PER_WORD));
}

static void bit_clear(uint32_t i) {
    g_bitmap[i / BITS_PER_WORD] &= ~(1u << (i % BITS_PER_WORD));
}

static int in_pool(uint32_t phys, uint32_t n) {
    if (phys == 0 || (phys & (PMM_PAGE_SIZE - 1u)) != 0) return 0;
    if (phys < PMM_POOL_START) return 0;
    if (n == 0) return 0;
    /* 上界检查用减法避免 phys + n*PAGE 的溢出 */
    if (phys > PMM_POOL_END) return 0;
    if (PMM_POOL_END - phys < n * PMM_PAGE_SIZE) return 0;
    return 1;
}

static uint32_t idx_of(uint32_t phys) {
    return (phys - PMM_POOL_START) / PMM_PAGE_SIZE;
}

/* ---------- 对外 API ---------- */

void pmm_init(void) {
    for (uint32_t i = 0; i < BITMAP_WORDS; i++) g_bitmap[i] = 0;
    g_used = 0;
    g_ready = 1;
}

uint32_t pmm_alloc_page(void) {
    return pmm_alloc_pages(1);
}

uint32_t pmm_alloc_pages(uint32_t n) {
    if (!g_ready || n == 0 || n > PMM_POOL_PAGES) return 0;

    /* 「找到连续空位」与「置位占用」之间不能断开：否则两次并发分配会选中
     * 同一段物理页——典型的 double allocation，同一块内存同时给两个所有者，
     * 比泄漏难查得多。约束见 irqflags.h。 */
    uint32_t f = irq_save_disable();
    uint32_t res = 0;

    /* 首次适配：找 n 个连续空闲位。位图小（48 字），线性扫描足够。 */
    uint32_t run = 0;
    for (uint32_t i = 0; i < PMM_POOL_PAGES; i++) {
        if (!bit_get(i)) {
            run++;
            if (run == n) {
                uint32_t first = i + 1u - n;
                for (uint32_t k = 0; k < n; k++) bit_set(first + k);
                g_used += n;
                res = PMM_POOL_START + first * PMM_PAGE_SIZE;
                break;
            }
        } else {
            run = 0;
        }
    }
    irq_restore(f);
    return res;                         /* 0 = 池耗尽 */
}

int pmm_free_page(uint32_t phys) {
    return pmm_free_pages(phys, 1);
}

int pmm_free_pages(uint32_t phys, uint32_t n) {
    if (!g_ready || !in_pool(phys, n)) return -1;

    uint32_t first = idx_of(phys);
    if (first + n > PMM_POOL_PAGES) return -1;

    /* 「检测是否已空闲」与「清位」必须一气呵成：否则同一区间的两次并发
     * 释放都会通过重复释放检测（两者都读到 bit=1），各自清位后 g_used
     * 被多减，真正的 double free 反而被漏掉。 */
    uint32_t f = irq_save_disable();
    int res = 0;

    /* 重复释放检测：只要有一位已经是 0（空闲），说明调用方给错了范围 */
    for (uint32_t k = 0; k < n; k++) {
        if (!bit_get(first + k)) { res = -1; break; }
    }
    if (res == 0) {
        for (uint32_t k = 0; k < n; k++) bit_clear(first + k);
        g_used -= n;
    }
    irq_restore(f);
    return res;
}

uint32_t pmm_total_pages(void) { return PMM_POOL_PAGES; }
uint32_t pmm_used_pages(void)  { return g_used; }
uint32_t pmm_free_count(void) { return PMM_POOL_PAGES - g_used; }

/* ---------- 自检 ---------- */

static void phex(pmm_puts_fn out, uint32_t v) {
    static const char h[] = "0123456789ABCDEF";
    char s[12];
    int n = 0;
    do { s[n++] = h[v & 0xFu]; v >>= 4; } while (v);
    while (n < 8) s[n++] = '0';
    while (n) { char c = s[--n]; char t[2] = { c, 0 }; out(t); }
}

static void pdec(pmm_puts_fn out, uint32_t v) {
    char s[16];
    int n = 0;
    if (v == 0) s[n++] = '0';
    while (v) { s[n++] = (char)('0' + v % 10u); v /= 10u; }
    while (n) { char c = s[--n]; char t[2] = { c, 0 }; out(t); }
}

static void pmm_null_puts(const char *s) { (void)s; }

int pmm_selftest(pmm_puts_fn out) {
    /* out==NULL：静默模式（开机自检用），只跑断言不打印 */
    if (!out) { out = pmm_null_puts; }
    int fail = 0;

    out("pmm self-test:\n");
    out("  pool 0x"); phex(out, PMM_POOL_START);
    out("-0x");        phex(out, PMM_POOL_END);
    out(" ("); pdec(out, PMM_POOL_PAGES); out(" pages)\n");

    /* 1) 单页分配/回收：地址必须落在池内且 4KB 对齐 */
    uint32_t a = pmm_alloc_page();
    int ok = (a != 0) && in_pool(a, 1);
    out("  alloc page -> 0x"); phex(out, a);
    out(ok ? " [OK]\n" : " [FAIL]\n");
    if (!ok) fail++;

    /* 2) 两次分配不能给同一页 */
    uint32_t b = pmm_alloc_page();
    ok = (b != 0) && (b != a);
    out("  distinct pages: ");
    out(ok ? "yes [OK]\n" : "NO [FAIL]\n");
    if (!ok) fail++;

    /* 3) 释放后计数回退；重复释放必须被拒 */
    uint32_t before = pmm_used_pages();
    ok = (pmm_free_page(a) == 0) && (pmm_used_pages() == before - 1u);
    out("  free + count: ");
    out(ok ? "[OK]\n" : "[FAIL]\n");
    if (!ok) fail++;

    ok = (pmm_free_page(a) != 0);          /* 重复释放 */
    out("  double free rejected: ");
    out(ok ? "yes [OK]\n" : "NO [FAIL]\n");
    if (!ok) fail++;

    /* 4) 越界地址必须被拒（池外的地址不能误释放） */
    ok = (pmm_free_page(0x00400000u) != 0) && (pmm_free_page(0) != 0);
    out("  out-of-range free rejected: ");
    out(ok ? "yes [OK]\n" : "NO [FAIL]\n");
    if (!ok) fail++;

    /* 5) 连续多页分配（ELF 段需要） */
    uint32_t c = pmm_alloc_pages(4);
    ok = (c != 0) && in_pool(c, 4);
    out("  alloc 4 contiguous -> 0x"); phex(out, c);
    out(ok ? " [OK]\n" : " [FAIL]\n");
    if (!ok) fail++;

    if (c) pmm_free_pages(c, 4);
    if (b) pmm_free_page(b);

    /* 6) 统计收尾：全部归还后 used 应为 0（自检自身不泄漏页） */
    out("  used after test: "); pdec(out, pmm_used_pages());
    out(" / "); pdec(out, pmm_total_pages()); out("\n");
    if (pmm_used_pages() != 0) {
        out("  [FAIL] selftest leaked pages\n");
        fail++;
    }

    out(fail == 0 ? "  result: PASS\n" : "  result: FAIL\n");
    return fail;
}
