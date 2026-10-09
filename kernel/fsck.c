/*
 * fsck.c - 卷一致性检查的分发层与报告输出（read-only）
 *
 * 各 FS 的扫描实现放在 fat.c / exfat.c 里（它们要复用那边的 static 助手：
 * FAT 项读取 + 扇区缓存、目录项枚举、簇链遍历、exFAT 的位图链）。这里只
 * 做三件事：位图小工具、按挂载类型分发、把报告打成稳定可断言的文本。
 *
 * 输出格式是**测试契约**：tests/test_fsck.py 靠
 *   "fsck: 0 problem(s) -- consistent"        （干净卷）
 *   "fsck: N problem(s) -- NOT consistent"    （有损伤）
 *   "fsck: orphan=N" 等字段行
 * 判定结果，改文案必须同步改测试。
 */
#include "fsck.h"
#include "fs.h"
#include "fat.h"
#include "exfat.h"
#include "tty.h"

/* ---- 极简输出：不碰 GUI，只走 tty（GUI 模式下由 gfx hook 接管） ---- */
static void fsck_puts(const char *s) { terminal_writestring(s); }

static void fsck_put_dec(uint32_t v) {
    char buf[16];
    int len = 0;
    if (v == 0) { terminal_putchar('0'); return; }
    while (v > 0) { buf[len++] = (char)('0' + (v % 10)); v /= 10; }
    while (len > 0) terminal_putchar(buf[--len]);
}

void fsck_bit_set(uint8_t *bm, uint32_t i) { bm[i >> 3] |= (uint8_t)(1u << (i & 7)); }

int fsck_bit_get(const uint8_t *bm, uint32_t i) {
    return (bm[i >> 3] >> (i & 7)) & 1;
}

/* 记下出问题的簇号，但只留前 FSCK_BAD_MAX 个——报告是给人看的摘要，
 * 不是完整清单；后面照样继续统计总数。 */
void fsck_record_bad(fsck_report_t *r, uint32_t cluster) {
    if (r->first_bad_n < FSCK_BAD_MAX) r->first_bad[r->first_bad_n++] = (uint32_t)cluster;
}

static uint32_t fsck_total(const fsck_report_t *r) {
    return r->bad_chain + r->loop + r->truncated + r->crosslink +
           r->orphan + r->free_but_used + r->bad_entry;
}

static void fsck_print_report(const fsck_report_t *r, const char *fsname) {
    fsck_puts("fsck: ");
    fsck_puts(fsname);
    fsck_puts(" clusters=");
    fsck_put_dec(r->clusters_total);
    fsck_puts("\n");

    fsck_puts("fsck: bad_chain=");
    fsck_put_dec(r->bad_chain);
    fsck_puts(" loop=");
    fsck_put_dec(r->loop);
    fsck_puts(" truncated=");
    fsck_put_dec(r->truncated);
    fsck_puts(" crosslink=");
    fsck_put_dec(r->crosslink);
    fsck_puts(" orphan=");
    fsck_put_dec(r->orphan);
    fsck_puts(" free_but_used=");
    fsck_put_dec(r->free_but_used);
    fsck_puts(" bad_entry=");
    fsck_put_dec(r->bad_entry);
    fsck_puts("\n");

    if (r->first_bad_n > 0) {
        fsck_puts("fsck: first bad clusters:");
        for (int i = 0; i < r->first_bad_n; i++) {
            terminal_putchar(' ');
            fsck_put_dec(r->first_bad[i]);
        }
        fsck_puts("\n");
    }

    uint32_t n = fsck_total(r);
    fsck_puts("fsck: ");
    fsck_put_dec(n);
    fsck_puts(" problem(s) -- ");
    fsck_puts(n == 0 ? "consistent" : "NOT consistent");
    fsck_puts("\n");
}

void cmd_fsck(const char *args) {
    (void)args;
    if (!fs_ready()) {
        fsck_puts("fsck: no volume mounted\n");
        return;
    }

    fsck_report_t r;
    for (int i = 0; i < FSCK_BAD_MAX; i++) r.first_bad[i] = 0;
    r.first_bad_n = 0;
    r.clusters_total = 0;
    r.bad_chain = r.loop = r.truncated = r.crosslink = 0;
    r.orphan = r.free_but_used = r.bad_entry = 0;

    int rc;
    const char *name;
    switch (fs_get_info()->type) {
    case FS_FAT12: rc = fat_fsck(&r);   name = "FAT12"; break;
    case FS_FAT16: rc = fat_fsck(&r);   name = "FAT16"; break;
    case FS_FAT32: rc = fat_fsck(&r);   name = "FAT32"; break;
    case FS_EXFAT: rc = exfat_fsck(&r); name = "exFAT"; break;
    default:
        fsck_puts("fsck: ");
        fsck_puts(fs_type_name());
        fsck_puts(" is read-only here, no consistency check implemented\n");
        return;
    }

    if (rc == -1) { fsck_puts("fsck: volume not mounted\n"); return; }
    if (rc == -2) { fsck_puts("fsck: out of memory (heap too small for this volume)\n"); return; }
    if (rc == -3) { fsck_puts("fsck: disk read error\n"); return; }
    if (rc != 0)  { fsck_puts("fsck: scan failed\n"); return; }

    fsck_print_report(&r, name);
}
