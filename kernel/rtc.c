/*
 * rtc.c - CMOS 实时时钟（RTC）读取与走时
 *
 * 硬件接口（PC/AT 标准，所有 PC 兼容机一致）：
 *   0x70 地址端口（写时要屏蔽 NMI：bit7=1 会关掉 NMI，读完要放回来）
 *   0x71 数据端口
 *   寄存器：0x00 秒  0x02 分  0x04 时  0x06 星期  0x07 日
 *           0x08 月  0x09 年（两位）  0x32 世纪（可选，不是每台机器都有）
 *           0x0A 状态 A（bit7 UIP：更新进行中，此时读数可能是半新半旧）
 *           0x0B 状态 B（bit1 24 小时制 / bit2 二进制而非 BCD）
 *
 * 三个必须处理的坑：
 *  1. UIP：RTC 更新寄存器的瞬间读出来的值是撕裂的（秒在跳、分没跳）。
 *     读到 UIP=1 就等它过去（最多等几微秒），否则可能出现 23:59:00
 *     被读成 00:59:00 这种差一小时的结果。
 *  2. BCD 还是二进制：状态 B 的 bit2 说了算。以前 gfxwin 里那份代码
 *     无脑当 BCD 算，遇到二进制模式的机器（部分固件/虚拟机）会读出
 *     乱码时间。
 *  3. 12 小时制：状态 B 的 bit1=0 时，小时的 bit7 表示下午，需要换算。
 *
 * 走时：开机读一次就够了——CMOS 端口读一次要好几个微秒（0x70/0x71 是
 * 慢速 ISA 口），每帧读（GUI 时钟以前就这么干）纯粹浪费。平时用 1000Hz
 * 的 PIT tick 差值往前推，RTC 只作基准。
 */
#include "rtc.h"
#include "port.h"
#include "isr.h"

#define CMOS_ADDR   0x70u
#define CMOS_DATA   0x71u

#define REG_SEC     0x00u
#define REG_MIN     0x02u
#define REG_HOUR    0x04u
#define REG_DAY     0x07u
#define REG_MON     0x08u
#define REG_YEAR    0x09u
#define REG_CENT    0x32u
#define REG_STA     0x0Au
#define REG_STB     0x0Bu

#define STA_UIP     0x80u   /* 更新进行中 */
#define STB_24H     0x02u   /* 1 = 24 小时制 */
#define STB_BINARY  0x04u   /* 1 = 二进制，0 = BCD */

static uint8_t  g_ok;                 /* CMOS 可用且读数合法 */
static uint32_t g_base_tick;          /* 快照时刻的 g_pit_ticks */
static uint32_t g_base_sec;           /* 快照时刻的"当天已过秒数" */
static uint32_t g_base_day;           /* 快照时刻距 1970-01-01 的天数 */

/* ------------------------------------------------------------------
 * 原始寄存器读取
 * ------------------------------------------------------------------ */
static uint8_t cmos_read(uint8_t reg) {
    /* bit7=1 关 NMI：写地址端口时绝不能顺手把 NMI 打开/关闭状态改乱，
     * 所以这里读回原值、只改低 7 位（NMI 位保持不变）。 */
    uint8_t prev = inb(CMOS_ADDR);
    outb(CMOS_ADDR, (uint8_t)((prev & 0x80u) | (reg & 0x7Fu)));
    uint8_t v = inb(CMOS_DATA);
    return v;
}

/* 等更新窗口过去。返回 0 = 可以读了，1 = UIP 一直不松口（时钟坏了） */
static int cmos_wait_ready(void) {
    for (uint32_t i = 0; i < 100000u; i++) {
        if ((cmos_read(REG_STA) & STA_UIP) == 0) return 0;
    }
    return 1;
}

/* BCD/二进制归一 */
static uint32_t cmos_val(uint8_t reg, int binary) {
    uint8_t v = cmos_read(reg);
    return binary ? (uint32_t)v : (uint32_t)((v & 0x0Fu) + ((v >> 4) * 10u));
}

/* ------------------------------------------------------------------
 * 日历换算（只依赖纯整数运算，内核没有 libc）
 * ------------------------------------------------------------------ */
static int is_leap(uint32_t y) {
    return ((y % 4u == 0u) && (y % 100u != 0u)) || (y % 400u == 0u);
}

static uint32_t days_in_month(uint32_t y, uint32_t m) {
    static const uint8_t d[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (m == 2u && is_leap(y)) return 29u;
    return (m >= 1u && m <= 12u) ? (uint32_t)d[m - 1u] : 0u;
}

/* 年-月-日 -> 距 1970-01-01 的天数。用逐月累加而不是公式，避免 32 位
 * 乘法溢出（y*365 到 2026 年约 74 万，其实安全，但逐月更直观可查）。 */
static uint32_t ymd_to_days(uint32_t y, uint32_t m, uint32_t d) {
    if (y < 1970u || m < 1u || m > 12u || d < 1u) return 0;
    uint32_t days = 0;
    for (uint32_t yy = 1970u; yy < y; yy++) days += is_leap(yy) ? 366u : 365u;
    for (uint32_t mm = 1u; mm < m; mm++) days += days_in_month(y, mm);
    if (d > days_in_month(y, m)) return 0;          /* 非法日 */
    return days + (d - 1u);
}

static void days_to_ymd(uint32_t days, uint32_t *y, uint32_t *m, uint32_t *d) {
    uint32_t yy = 1970u;
    while (1) {
        uint32_t span = is_leap(yy) ? 366u : 365u;
        if (days < span) break;
        days -= span;
        yy++;
        if (yy > 2200u) { *y = 1970u; *m = 1u; *d = 1u; return; }  /* 上界防挂死 */
    }
    uint32_t mm = 1u;
    while (mm <= 12u) {
        uint32_t dim = days_in_month(yy, mm);
        if (days < dim) break;
        days -= dim;
        mm++;
    }
    *y = yy; *m = mm; *d = days + 1u;
}

/* ------------------------------------------------------------------
 * 对外接口
 * ------------------------------------------------------------------ */
void rtc_init(void) {
    g_ok = 0;
    g_base_tick = g_pit_ticks;
    g_base_day = 0;
    g_base_sec = 0;

    if (cmos_wait_ready() != 0) return;            /* UIP 卡住 = 时钟坏了 */

    uint8_t stb = cmos_read(REG_STB);
    int binary = (stb & STB_BINARY) ? 1 : 0;
    int h24    = (stb & STB_24H)    ? 1 : 0;

    uint32_t sec = cmos_val(REG_SEC, binary);
    uint32_t min = cmos_val(REG_MIN, binary);
    uint32_t hr  = cmos_val(REG_HOUR, binary);
    uint32_t day = cmos_val(REG_DAY, binary);
    uint32_t mon = cmos_val(REG_MON, binary);
    uint32_t yr  = cmos_val(REG_YEAR, binary);

    /* 12 小时制换算（bit7 = PM）。二进制模式同样适用。 */
    if (!h24) {
        uint8_t raw = cmos_read(REG_HOUR);
        uint32_t v = binary ? raw : (uint32_t)((raw & 0x0Fu) + ((raw >> 4) * 10u));
        if (raw & 0x80u) hr = (v & 0x7Fu) + 12u;    /* PM */
        else             hr = (v == 12u) ? 0u : v;  /* 12 AM -> 0 */
    }

    /* 世纪：0x32 有就用，没有就按"70-99 -> 19xx，00-69 -> 20xx"的传统
     * 约定猜（RTC 只有两位年份，这是 PC 兼容机的通行做法）。 */
    uint32_t year;
    uint8_t cent = cmos_read(REG_CENT);
    uint32_t c = binary ? (uint32_t)cent
                        : (uint32_t)((cent & 0x0Fu) + ((cent >> 4) * 10u));
    if (c >= 19u && c <= 21u) year = c * 100u + yr;
    else year = (yr >= 70u) ? (1900u + yr) : (2000u + yr);

    /* 合法性：任何一个字段越界就整体判不可用（宁可让调用方走退路，也不
     * 要拿一个显然错误的时间去写文件时间戳）。 */
    if (sec > 59u || min > 59u || hr > 23u) return;
    if (mon < 1u || mon > 12u || day < 1u) return;
    if (day > days_in_month(year, mon)) return;
    if (year < 1980u || year > 2200u) return;      /* FAT 时间戳下界是 1980 */

    /* UTC -> 本地时间（+RTC_TZ_HOURS 可能跨日，统一换算成"天数+当天秒数"） */
    uint32_t sod = hr * 3600u + min * 60u + sec;
    sod += (uint32_t)RTC_TZ_HOURS * 3600u;
    uint32_t d = ymd_to_days(year, mon, day);
    if (d == 0) return;                            /* 非法日期 */
    while (sod >= 86400u) { sod -= 86400u; d++; }

    g_base_day  = d;
    g_base_sec  = sod;
    g_base_tick = g_pit_ticks;
    g_ok = 1;
}

int rtc_valid(void) { return g_ok ? 1 : 0; }

int rtc_now(rtc_time_t *out) {
    if (!g_ok || out == 0) return -1;

    /* PIT 是 1000Hz，差值即毫秒。用减法而不是绝对值，天然过回绕。 */
    uint32_t ms = g_pit_ticks - g_base_tick;
    uint32_t total = g_base_sec + ms / 1000u;
    uint32_t days  = g_base_day + total / 86400u;
    uint32_t sod   = total % 86400u;

    uint32_t y = 1970u, m = 1u, d = 1u;
    days_to_ymd(days, &y, &m, &d);

    out->year   = (uint16_t)y;
    out->month  = (uint8_t)m;
    out->day    = (uint8_t)d;
    out->hour   = (uint8_t)(sod / 3600u);
    out->minute = (uint8_t)((sod % 3600u) / 60u);
    out->second = (uint8_t)(sod % 60u);
    return 0;
}

uint16_t rtc_fat_date(const rtc_time_t *t) {
    uint32_t y = (t->year >= 1980u) ? (uint32_t)t->year : 1980u;
    if (y > 2107u) y = 2107u;                       /* 7 位年份上限 */
    uint32_t m = (t->month >= 1u && t->month <= 12u) ? t->month : 1u;
    uint32_t d = (t->day >= 1u && t->day <= 31u) ? t->day : 1u;
    return (uint16_t)(((y - 1980u) << 9) | (m << 5) | d);
}

uint16_t rtc_fat_time(const rtc_time_t *t) {
    uint32_t h = (t->hour   <= 23u) ? t->hour   : 0u;
    uint32_t m = (t->minute <= 59u) ? t->minute : 0u;
    uint32_t s = (t->second <= 59u) ? t->second : 0u;
    return (uint16_t)((h << 11) | (m << 5) | (s / 2u));
}
