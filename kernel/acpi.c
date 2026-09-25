/*
 * acpi.c - 最小 ACPI 电源管理（RSDP/FADT/DSDT-_S5，只读解析）
 *
 * 解析链：
 *   1) RSDP：EBDA（0x40E 段址）与 0xE0000-0xFFFFF 扫 "RSD PTR " 签名。
 *      v1 校验 20 字节和；v2 追加 36 字节扩展校验。fail closed。
 *   2) 描述符表：v2+ 走 XSDT（64 位条目），否则 RSDT（32 位条目）。
 *      逐项校验签名与校验和，找 "FACP"（FADT）。
 *   3) FADT：PM1a_CNT_BLK(+64)，rev>=2 且 X_PM1a_CNT(+268) 非零则优先；
 *      DSDT 指针 +40，rev>=2 时 X_DSDT(+140) 非零则优先。
 *   4) DSDT：扫 "_S5_" 签名，其后是 AML 包长度编码，再取
 *      SLP_TYPa、SLP_TYPb。全程越界检查。
 *
 * 真机注意：部分 BIOS 需先向 FADT.SMI_CMD(+48) 写 ACPI_ENABLE(+52)
 * 才开放 PM 寄存器；此处实现该可选步骤（值非 0xFF 才发）。
 *
 * 已知简化：不解析 AML 完整命名空间、不处理 \_S5 双反斜杠路径之外
 * 的作用域嵌套——只认线性扫到的第一个 _S5 包。主流 BIOS 均可命中。
 */
#include "acpi.h"
#include "port.h"
#include "types.h"

typedef struct {
    int      ready;
    uint16_t pm1a_cnt;
    uint16_t pm1b_cnt;
    uint8_t  slp_typa;
    uint8_t  slp_typb;
    uint16_t smi_cmd;
    uint8_t  acpi_enable;
    int      smi_needed;
    char     status[96];
} acpi_state_t;

static acpi_state_t A;

/* 物理 IO 内存读：屏障阻止 GCC 把常量地址折叠成"零长数组越界"
 * （-Warray-bounds 对 0x40E 这类 BDA 低地址会误报，见 GCC array-bounds 建模） */
static inline volatile void *pa(uint32_t addr) {
    uint32_t p = addr;
    __asm__("" : "+r"(p));
    return (volatile void *)p;
}
static int ver_u8(uint32_t addr) { return *(volatile uint8_t *)pa(addr); }
static uint16_t ver_u16(uint32_t addr) {
    return *(volatile uint16_t *)pa(addr);
}
static uint32_t ver_u32(uint32_t addr) { return *(volatile uint32_t *)addr; }
static uint64_t ver_u64(uint32_t addr) {
    return ((uint64_t)ver_u32(addr + 4) << 32) | ver_u32(addr);
}

/* ACPI 表头公共字段：签名(4) 长度(4) 修订(1) 校验和(1) */
static int table_ok(uint32_t addr, const char sig[4]) {
    /* 只认 identity 映射（0-32MB）内的表：QEMU/真机常把 ACPI 表放
     * RAM 顶部（如 128MB 机器的 0x7FE22E0），越出 identity 读它 =
     * #PF panic。fail closed：按"表不可达"处理，走 legacy 回退。 */
    if (addr < 0x100000 || addr + 0x40000 > 0x02000000u) return 0;
    for (int i = 0; i < 4; i++) {
        if (ver_u8(addr + i) != (uint8_t)sig[i]) return 0;
    }
    uint32_t len = ver_u32(addr + 4);
    if (len < 36 || len > 0x40000) return 0;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum += ver_u8(addr + i);
    return sum == 0;
}

/* 在 [base, base+span) 里找 RSDP 签名，按步长 16 对齐扫描 */
static uint32_t find_rsdp_span(uint32_t base, uint32_t span) {
    for (uint32_t a = base; a + 36 <= base + span && a >= base; a += 16) {
        /* 签名 8 字节 "RSD PTR "：R,S,D,空格,P,T,R,空格（下标 0-7）。
         * 下标 8 起是校验和/OEMID。曾把 +8 也当空格校验（RSDP 永远扫
         * 不到），又曾把 +3 当 'T'（同样扫不到）——两次都被自检揪出。 */
        if (ver_u8(a) != 'R' || ver_u8(a + 1) != 'S') continue;
        if (ver_u8(a + 2) != 'D' || ver_u8(a + 3) != ' ') continue;
        if (ver_u8(a + 4) != 'P' || ver_u8(a + 5) != 'T') continue;
        if (ver_u8(a + 6) != 'R' || ver_u8(a + 7) != ' ') continue;
        /* v1 校验和：前 20 字节 */
        uint8_t sum = 0;
        for (int i = 0; i < 20; i++) sum += ver_u8(a + i);
        if (sum != 0) continue;
        /* v2+：36 字节扩展校验和（revision 在 +15） */
        if (ver_u8(a + 15) >= 2) {
            sum = 0;
            for (int i = 0; i < 36; i++) sum += ver_u8(a + i);
            if (sum != 0) continue;
        }
        return a;
    }
    return 0;
}

static uint32_t find_rsdp(void) {
    /* 1) EBDA：BDA 0x40E 是段址（字节地址 = 段<<4），区间按 1KB 假设 */
    uint16_t seg = ver_u16(0x40E);
    if (seg >= 0x8000 && seg < 0xA000) {
        uint32_t a = find_rsdp_span((uint32_t)seg << 4, 0x400);
        if (a) return a;
    }
    /* 2) BIOS ROM 区 */
    return find_rsdp_span(0xE0000, 0x20000);
}

/* 从描述符表（RSDT/XSDT）收集 FADT 地址，带每表校验和验证 */
static uint32_t find_fadt(uint32_t rsdp) {
    uint8_t revision = ver_u8(rsdp + 15);
    if (revision >= 2) {
        uint32_t xsdt = (uint32_t)ver_u64(rsdp + 24);
        if (table_ok(xsdt, "XSDT")) {
            uint32_t len = ver_u32(xsdt + 4);
            for (uint32_t off = 36; off + 8 <= len; off += 8) {
                uint32_t t = (uint32_t)ver_u64(xsdt + off);
                if (table_ok(t, "FACP")) return t;
            }
        }
    }
    /* 老机器/兼容：RSDT（36 号字段是 32 位 RSDT 地址） */
    uint32_t rsdt = ver_u32(rsdp + 16);
    if (rsdt && table_ok(rsdt, "RSDT")) {
        uint32_t len = ver_u32(rsdt + 4);
        for (uint32_t off = 36; off + 4 <= len; off += 4) {
            uint32_t t = ver_u32(rsdt + off);
            if (table_ok(t, "FACP")) return t;
        }
    }
    return 0;
}

/* DSDT 里找 _S5 睡眠包，返回 SLP_TYPa/b（越界/畸形一律失败） */
static int find_s5(uint32_t dsdt, uint8_t *typa, uint8_t *typb) {
    uint32_t len = ver_u32(dsdt + 4);
    if (len < 36 + 8 || len > 0x200000) return 0;
    for (uint32_t i = 36; i + 8 <= len; i++) {
        if (ver_u8(dsdt + i) != '_' || ver_u8(dsdt + i + 1) != 'S' ||
            ver_u8(dsdt + i + 2) != '5' || ver_u8(dsdt + i + 3) != '_') {
            continue;
        }
        uint32_t p = i + 4;
        /* 包长度：0x40-0x7F 单字节；0x80-0xBF 双字节；其余三字节。
         * 只需要知道长度编码占几个字节——值本身不用。 */
        uint8_t b0 = ver_u8(dsdt + p);
        uint32_t hdr = 1;
        if ((b0 & 0xC0) == 0x80) hdr = 2;
        else if ((b0 & 0xC0) == 0xC0) hdr = 3;
        else if ((b0 & 0xC0) != 0x40 && b0 != 0) {
            /* 0x00-0x3F 也是合法单字节长度 */
        }
        if (p + hdr + 3 > len) return 0;
        p += hdr;
        /* 常见形态：NameOp(0x08) 已在 _S5_ 前，包体内直接是
         * SLP_TYPa、SLP_TYPb、保留字节 */
        *typa = ver_u8(dsdt + p);
        *typb = ver_u8(dsdt + p + 1);
        if (*typa > 0x0F || *typb > 0x0F) return 0;   /* 非法 Sx 编码 */
        return 1;
    }
    return 0;
}

void acpi_init(void) {
    A.ready = 0;
    A.pm1b_cnt = 0;
    A.smi_needed = 0;
    for (int i = 0; i < (int)sizeof(A.status); i++) A.status[i] = 0;

    uint32_t rsdp = find_rsdp();
    if (!rsdp) {
        const char *s = "ACPI: RSDP not found - legacy QEMU power";
        for (int i = 0; s[i] && i < (int)sizeof(A.status) - 1; i++) A.status[i] = s[i];
        return;
    }

    uint32_t fadt = find_fadt(rsdp);
    if (!fadt) {
        const char *s = "ACPI: FADT not found - legacy QEMU power";
        for (int i = 0; s[i] && i < (int)sizeof(A.status) - 1; i++) A.status[i] = s[i];
        return;
    }

    uint8_t fadt_rev = ver_u8(fadt + 9);
    uint32_t pm1a = ver_u32(fadt + 64);
    if (fadt_rev >= 2) {
        /* X_PM1a_CNT_BLK 是 96 位 GAS（+268），简化读低 32 位地址段 */
        uint64_t x = ver_u64(fadt + 268);
        uint32_t xa = (uint32_t)(x & 0xFFFFFFFFu);
        if (xa != 0 && (x >> 32) == 0) pm1a = xa;   /* 仅接受 <=4G 的 IO/GAS */
    }
    uint32_t pm1b = ver_u32(fadt + 68);

    /* 真 BIOS 常见：ACPI 默认关闭，需 SMI 命令使能 PM 寄存器 */
    A.smi_cmd = (uint16_t)ver_u32(fadt + 48);
    A.acpi_enable = ver_u8(fadt + 52);
    if (A.smi_cmd != 0 && A.smi_cmd != 0xFFFF && A.acpi_enable != 0xFF) {
        A.smi_needed = 1;
    }

    /* DSDT：rev>=2 且 X_DSDT(+140) 非零则用 64 位地址 */
    uint32_t dsdt = ver_u32(fadt + 40);
    if (fadt_rev >= 2) {
        uint64_t xd = ver_u64(fadt + 140);
        uint32_t xda = (uint32_t)(xd & 0xFFFFFFFFu);
        if (xd != 0 && (xd >> 32) == 0 && table_ok(xda, "DSDT")) dsdt = xda;
    }
    if (!dsdt || !table_ok(dsdt, "DSDT")) {
        const char *s = "ACPI: DSDT invalid - legacy QEMU power";
        for (int i = 0; s[i] && i < (int)sizeof(A.status) - 1; i++) A.status[i] = s[i];
        return;
    }

    uint8_t typa = 0, typb = 0;
    if (!find_s5(dsdt, &typa, &typb)) {
        const char *s = "ACPI: _S5 not found - legacy QEMU power";
        for (int i = 0; s[i] && i < (int)sizeof(A.status) - 1; i++) A.status[i] = s[i];
        return;
    }

    if (pm1a == 0 || pm1a >= 0x10000) {
        const char *s = "ACPI: PM1a_CNT invalid - legacy QEMU power";
        for (int i = 0; s[i] && i < (int)sizeof(A.status) - 1; i++) A.status[i] = s[i];
        return;
    }

    A.ready = 1;
    A.pm1a_cnt = (uint16_t)pm1a;
    A.pm1b_cnt = (pm1b && pm1b < 0x10000) ? (uint16_t)pm1b : 0;
    A.slp_typa = typa;
    A.slp_typb = typb;

    /* 状态行：hex 打印手写（无 sprintf 依赖） */
    static const char h[] = "0123456789ABCDEF";
    const char *p = "ACPI: FADT rev ";
    int n = 0;
    for (; *p; p++) A.status[n++] = *p;
    A.status[n++] = (char)('0' + fadt_rev / 10);
    A.status[n++] = (char)('0' + fadt_rev % 10);
    p = " PM1a=0x";
    for (; *p; p++) A.status[n++] = *p;
    for (int sh = 12; sh >= 0; sh -= 4)
        A.status[n++] = h[(pm1a >> sh) & 0xF];
    p = " S5_TYP=0x";
    for (; *p; p++) A.status[n++] = *p;
    A.status[n++] = h[typa];
    A.status[n] = 0;
}

const char *acpi_status_line(void) {
    return A.status;
}

void acpi_shutdown(void) {
    if (A.ready) {
        /* 可选：SMI 使能 ACPI 模式（真 BIOS 需要；QEMU 的 SMI_CMD=0 跳过） */
        if (A.smi_needed) outb(A.smi_cmd, A.acpi_enable);

        uint16_t val = (uint16_t)((A.slp_typa << 10) | (1u << 13));  /* S5|SLP_EN */
        outw(A.pm1a_cnt, val);
        if (A.pm1b_cnt) outw(A.pm1b_cnt, val);
    } else {
        /* 回退：QEMU i440fx PIIX4 固定 PM1a_CNT=0x604，S5=0 */
        outw(0x604, 0x2000);
    }
    /* 机器应已断电；没断就停死在这，别返回到已半关的调用方 */
    for (;;) asm volatile("cli; hlt");
}
