/*
 * rtc.h - CMOS 实时时钟（RTC）读取与走时
 *
 * 为什么单独一个模块：以前只有 GUI 的桌面时钟读 CMOS（gfxwin.c 里自己
 * 写了一份 gw_cmos_read），而文件系统写入的创建/修改时间戳是**写死的
 * 2026-01-01**（exfat.c 的 0x5C21、fat.c 的 0x5C21）。也就是说同一个
 * 系统里"屏幕上的时间"和"文件上的时间"来自两个互不相干的来源，后者
 * 还是个常量——这就是占位实现：文件拷到别的机器上看不出先后。
 *
 * 本模块提供唯一的时间来源：
 *   rtc_init()  开机读一次 CMOS 快照（并记下当时的 g_pit_ticks）
 *   rtc_now()   快照 + PIT 走过的毫秒 -> 当前本地时间
 * CMOS 读不到（无 RTC / 校验失败）时 rtc_valid() 返回 0，调用方必须
 * 自己决定退路（本项目的退路是沿用那个基准日期，而不是假装成功）。
 *
 * 时区：BIOS RTC 存的是 UTC（QEMU 与多数服务器如此），显示与 FAT/exFAT
 * 时间戳都按 RTC_TZ_HOURS 折算成本地时间。改这一处即可，不要在各调用点
 * 各加一次偏移（gfxwin 以前是自己在读的时候 +8）。
 */
#ifndef RTC_H
#define RTC_H

#include "types.h"

/* CMOS 存 UTC，本地时间 = UTC + RTC_TZ_HOURS（+8 = 中国标准时间） */
#define RTC_TZ_HOURS   8

typedef struct {
    uint16_t year;      /* 完整年份，如 2026 */
    uint8_t  month;     /* 1-12 */
    uint8_t  day;       /* 1-31 */
    uint8_t  hour;      /* 0-23（本地时间） */
    uint8_t  minute;    /* 0-59 */
    uint8_t  second;    /* 0-59 */
} rtc_time_t;

/* 开机快照。失败时 rtc_valid() 为 0，rtc_now() 返回 -1。 */
void rtc_init(void);

/* CMOS 是否可用且读数合法 */
int rtc_valid(void);

/* 当前本地时间。返回 0 = 成功，-1 = RTC 不可用（out 不被修改）。 */
int rtc_now(rtc_time_t *out);

/* 当前时间 -> DOS/FAT/exFAT 的时间戳字段编码（三者同一套打包格式）：
 *   date: (year-1980)<<9 | month<<5 | day
 *   time: hour<<11 | minute<<5 | (second/2)
 * 越界字段一律夹紧，绝不产出非法日期（FAT 里 year<1980 会被驱动当成
 * 空条目跳过，等于文件凭空消失）。 */
uint16_t rtc_fat_date(const rtc_time_t *t);
uint16_t rtc_fat_time(const rtc_time_t *t);

/* RTC 不可用时的退路：2026-01-01 00:00（与以前写死的值一致） */
#define RTC_FALLBACK_DATE   0x5C21u
#define RTC_FALLBACK_TIME   0x0000u

#endif /* RTC_H */
