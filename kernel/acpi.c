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
 * 范围：不解析完整 AML 命名空间（那是解释器的活）。\_S5 的取法是按
 * AML 字面解 NameOp + PackageOp + PkgLength + ByteConst 元素，并且
 * DSDT 与所有 SSDT 都扫；任何一步不合结构就换下一个候选，绝不猜。
 */
#include "acpi.h"
#include "port.h"
#include "types.h"
#include "paging.h"

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

/* 已经映射进窗口的表：直接按虚拟地址读（表头字段全是小端） */
static uint8_t  rd8 (uint32_t v) { return *(volatile uint8_t  *)v; }
static uint32_t rd32(uint32_t v) { return *(volatile uint32_t *)v; }
static uint64_t rd64(uint32_t v) { return ((uint64_t)rd32(v + 4) << 32) | rd32(v); }

/* ------------------------------------------------------------------
 * 临时映射窗口
 *
 * QEMU 与真机都把 ACPI 表放在 RAM 高处（128MB 机器的 RSDT/XSDT/FADT/
 * DSDT 在 0x7FE0000 附近 = 127MB），远超 identity 映射的 0-32MB——直接
 * 解引用物理地址就是 #PF panic。以前的处理是"越界就当没找到"，于是
 * ACPI 永远走不到标准路径，关机只能靠硬编码的 legacy 端口 0x604。
 *
 * 现在开一组临时映射槽：虚拟地址从 identity 上界（32MB，即 PDE[8]，
 * 当前空闲）起，每槽 64KB，共 4 槽。ACPI 表只读，所以映射成只读页。
 * 只在 acpi_init 期间使用：解析结果（PM1a_CNT/SLP_TYP）立刻缓存进 A，
 * 之后不再需要这些表。
 * ------------------------------------------------------------------ */
#define ACPI_WIN_VADDR   PAGING_IDENTITY_END          /* 0x02000000 */
#define ACPI_WIN_SLOTS   4u
#define ACPI_WIN_PAGES   16u                          /* 每槽 16 页 = 64KB */
#define ACPI_SLOT_BYTES  (ACPI_WIN_PAGES * 4096u)

static uint32_t g_slot_phys[ACPI_WIN_SLOTS];          /* 槽当前映射的物理页基址 */
static uint32_t g_slot_pages[ACPI_WIN_SLOTS];         /* 槽当前映射的页数（只记基址会踩坑：见下） */
static int      g_win_ok;                             /* 分页可用才开窗 */

/* 把 [pa, pa+len) 映射进槽 idx，返回窗口内虚拟地址；失败返回 0 */
static uint32_t acpi_slot_map(uint32_t idx, uint32_t pa, uint32_t len) {
    if (!g_win_ok || idx >= ACPI_WIN_SLOTS || pa == 0) return 0;
    if (len == 0 || len > ACPI_SLOT_BYTES) return 0;
    uint32_t base  = pa & 0xFFFFF000u;
    uint32_t off   = pa & 0x00000FFFu;
    uint32_t pages = (off + len + 4095u) / 4096u;
    if (pages > ACPI_WIN_PAGES) return 0;
    uint32_t vb = ACPI_WIN_VADDR + idx * ACPI_SLOT_BYTES;
    /* 命中缓存要**页数也够**：acpi_map_table 先用 64 字节（1 页）读表头，
     * 再按真实长度重映射；只比对物理基址的话第二次会命中只有 1 页的旧
     * 映射，读 DSDT（8KB+）就越出窗口末尾 → #PF。 */
    if (g_slot_phys[idx] == base && g_slot_pages[idx] >= pages) return vb + off;
    for (uint32_t i = 0; i < ACPI_WIN_PAGES; i++) paging_unmap(vb + i * 4096u);
    for (uint32_t i = 0; i < pages; i++) {
        if (paging_map(vb + i * 4096u, base + i * 4096u, PAGING_RO_FLAGS) != 0) {
            for (uint32_t j = 0; j < i; j++) paging_unmap(vb + j * 4096u);
            g_slot_phys[idx] = 0;
            g_slot_pages[idx] = 0;
            return 0;
        }
    }
    g_slot_phys[idx] = base;
    g_slot_pages[idx] = pages;
    return vb + off;
}

/* 映射一张表并校验签名 + 全表校验和，返回窗口内虚拟地址（0 = 不可信） */
static uint32_t acpi_map_table(uint32_t pa, const char sig[4], uint32_t slot) {
    uint32_t v = acpi_slot_map(slot, pa, 64);         /* 先够读表头 */
    if (!v) return 0;
    uint32_t len = rd32(v + 4);
    if (len < 36 || len > ACPI_SLOT_BYTES) return 0;
    v = acpi_slot_map(slot, pa, len);                 /* 按真实长度重映射 */
    if (!v) return 0;
    for (int i = 0; i < 4; i++)
        if (rd8(v + (uint32_t)i) != (uint8_t)sig[i]) return 0;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum += rd8(v + i);
    return (sum == 0) ? v : 0;                        /* 校验和不过 = 表不可信 */
}

/* 描述符表里最多收集几张表（XSDT/RSDT 条目） */
#define ACPI_MAX_TABLE  16u
static uint32_t g_tbl[ACPI_MAX_TABLE];
static uint32_t g_tbl_n;

/* 把 XSDT/RSDT 里的条目物理地址全收下来（签名校验留给 acpi_map_table） */
static void collect_tables(uint32_t rsdp) {
    g_tbl_n = 0;
    uint8_t revision = ver_u8(rsdp + 15);
    if (revision >= 2) {
        uint32_t xsdt = acpi_map_table((uint32_t)ver_u64(rsdp + 24), "XSDT", 0);
        if (xsdt) {
            uint32_t len = rd32(xsdt + 4);
            for (uint32_t off = 36;
                 off + 8 <= len && off + 8 <= ACPI_SLOT_BYTES && g_tbl_n < ACPI_MAX_TABLE;
                 off += 8) {
                g_tbl[g_tbl_n++] = (uint32_t)rd64(xsdt + off);
            }
            return;
        }
    }
    uint32_t rsdt = acpi_map_table(ver_u32(rsdp + 16), "RSDT", 0);
    if (!rsdt) return;
    uint32_t len = rd32(rsdt + 4);
    for (uint32_t off = 36;
         off + 4 <= len && off + 4 <= ACPI_SLOT_BYTES && g_tbl_n < ACPI_MAX_TABLE;
         off += 4) {
        g_tbl[g_tbl_n++] = rd32(rsdt + off);
    }
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

/* 描述符表里能同时出现几张 SSDT（\_S5 也可能挂在 SSDT 里） */
#define MAX_SSDT  8u
static uint32_t g_ssdt[MAX_SSDT];
static uint32_t g_ssdt_count;

/* 从收集到的条目里找指定签名的表，映射进槽 slot，返回窗口内虚拟地址。
 * FADT 常驻槽 1，SSDT 逐个用槽 3，互不覆盖。 */
static uint32_t find_table(const char sig[4], uint32_t slot) {
    for (uint32_t i = 0; i < g_tbl_n; i++) {
        uint32_t v = acpi_map_table(g_tbl[i], sig, slot);
        if (v) return v;
    }
    return 0;
}

/* 收集所有 SSDT 的**物理地址**（\_S5 也可能挂在 SSDT 里），校验延后 */
static void collect_ssdt(void) {
    g_ssdt_count = 0;
    for (uint32_t i = 0; i < g_tbl_n && g_ssdt_count < MAX_SSDT; i++) {
        uint32_t v = acpi_map_table(g_tbl[i], "SSDT", 3);
        if (v) g_ssdt[g_ssdt_count++] = g_tbl[i];
    }
}

/* ------------------------------------------------------------------
 * GAS（Generic Address Structure，ACPI 6.x 第 5.2.5.2 节）
 *
 *   偏移  0  AddressSpaceID   1 字节
 *   偏移 +1  RegisterBitWidth 1 字节
 *   偏移 +2  RegisterBitOffset 1 字节
 *   偏移 +3  AccessSize       1 字节
 *   偏移 +4  Address          8 字节（小端）
 *
 * 老代码把 +0 起的 8 字节当成一个 u64 地址、并要求高 4 字节为 0 —— 高
 * 4 字节装的是 AddressSpaceID/BitWidth/...，永远不为 0，于是所有 X_*
 * 分支都是死代码。现在按结构逐字段读，并且只接受 SystemIO（PM1a_CNT）
 * 或 SystemMemory（DSDT）。
 * ------------------------------------------------------------------ */
#define GAS_SPACE_MEMORY   0u
#define GAS_SPACE_IO       1u

typedef struct {
    uint8_t  space;
    uint8_t  bit_width;
    uint8_t  bit_offset;
    uint8_t  access_size;
    uint64_t address;
} gas_t;

static void gas_read(uint32_t addr, gas_t *g) {
    g->space       = (uint8_t)ver_u8(addr + 0);
    g->bit_width   = (uint8_t)ver_u8(addr + 1);
    g->bit_offset  = (uint8_t)ver_u8(addr + 2);
    g->access_size = (uint8_t)ver_u8(addr + 3);
    g->address     = ver_u64(addr + 4);
}

/* AML 包长度（ACPI 6.x 20.2.4 PkgLength）：
 *   lead byte bit7-6 = ByteCount(0..3)，表示 lead byte 之后还有几个字节。
 *   ByteCount==0 时长度 = lead byte 的 bit5-0；
 *   否则长度 = lead byte 的 bit3-0 | 后续每字节左移 (4 + 8*(i-1)) 位。
 * 返回 0 = 畸形；否则填充 *len（包总长，含长度编码自身）与 *hdr（编码占几字节）。 */
static int aml_pkg_len(uint32_t base, uint32_t off, uint32_t *len, uint32_t *hdr) {
    uint8_t b0 = (uint8_t)ver_u8(base + off);
    uint32_t bc = (uint32_t)((b0 >> 6) & 0x3u);
    uint32_t v;
    if (bc == 0) {
        v = (uint32_t)(b0 & 0x3Fu);
        *hdr = 1;
    } else {
        v = (uint32_t)(b0 & 0x0Fu);
        for (uint32_t i = 1; i <= bc; i++)
            v |= (uint32_t)ver_u8(base + off + i) << (4u + (i - 1u) * 8u);
        *hdr = 1u + bc;
    }
    if (v < *hdr) return 0;          /* 长度连自己的编码都盖不住 = 畸形 */
    *len = v;
    return 1;
}

/* 读一个 AML 整数元素（ByteConst 0x0A+byte / ZeroOp / OneOp / 裸小整数）。
 * 返回消耗的字节数，0 = 不接受。固件里 _S5 的元素必须是 0..7 的 Sx 编码。 */
static uint32_t aml_read_byte(uint32_t base, uint32_t off, uint32_t end, uint8_t *out) {
    if (off >= end) return 0;
    uint8_t b = (uint8_t)ver_u8(base + off);
    if (b == 0x0Au) {                     /* BytePrefix */
        if (off + 1 >= end) return 0;
        *out = (uint8_t)ver_u8(base + off + 1);
        return 2;
    }
    if (b == 0x00u) { *out = 0; return 1; }  /* ZeroOp */
    if (b == 0x01u) { *out = 1; return 1; }  /* OneOp */
    if (b < 0x0Au)  { *out = b; return 1; }  /* 裸小整数（部分固件省略 BytePrefix） */
    return 0;
}

/* 在一张 AML 表（DSDT/SSDT）里找 \_S5 包，返回 SLP_TYPa/b。
 *
 * 标准形态：NameOp(0x08) [RootChar 0x5C] "_S5_" PackageOp(0x12)
 *           PkgLength NumElements ByteConst(SLP_TYPa) ByteConst(SLP_TYPb)
 *
 * 老实现跳过长名字后**直接把 PkgLength 和 NumElements 当成 SLP_TYPa/b**
 * 读走——对 QEMU 的 `12 06 02 0A 05 0A 00` 会读出 0x06/0x02，而真正的
 * SLP_TYPa 是 0x05。写进 PM1a_CNT 的 SLP_TYP 字段是错的。现在按 AML 解。
 * 全程越界检查，畸形一律 fail closed。 */
static int find_s5(uint32_t dsdt, uint8_t *typa, uint8_t *typb) {
    uint32_t len = ver_u32(dsdt + 4);
    if (len < 36 + 16 || len > 0x200000u) return 0;
    for (uint32_t i = 36; i + 4 < len; i++) {
        if (rd8(dsdt + i + 0) != '_' || rd8(dsdt + i + 1) != 'S' ||
            rd8(dsdt + i + 2) != '5' || rd8(dsdt + i + 3) != '_') {
            continue;
        }
        /* 前面必须是 NameOp 0x08（中间允许一个 RootChar 0x5C）。
         * 否则这是别的名字里的 "_S5_" 子串，不能采信。 */
        uint8_t prev1 = (uint8_t)rd8(dsdt + i - 1);
        uint8_t prev2 = (uint8_t)rd8(dsdt + i - 2);
        if (prev1 != 0x08u && !(prev1 == 0x5Cu && prev2 == 0x08u)) continue;

        uint32_t p = i + 4;
        if (p >= len || rd8(dsdt + p) != 0x12u) continue;   /* 必须跟 PackageOp */
        uint32_t plen = 0, hdr = 0;
        if (!aml_pkg_len(dsdt, p + 1, &plen, &hdr)) continue;
        if ((uint64_t)p + 1u + plen > (uint64_t)len) continue; /* 包越出表尾 = 畸形 */
        uint32_t end = p + 1u + plen;                          /* 包内容终点（不含） */
        uint32_t q = p + 1u + hdr;                             /* PkgLength 之后 */

        uint8_t nel = 0;
        uint32_t adv = aml_read_byte(dsdt, q, end, &nel);
        if (!adv || nel < 1u) continue;
        q += adv;

        uint8_t a = 0, b = 0;
        adv = aml_read_byte(dsdt, q, end, &a);
        if (!adv) continue;
        q += adv;
        if (nel >= 2u) {
            adv = aml_read_byte(dsdt, q, end, &b);
            if (!adv) b = 0;
        }
        /* SLP_TYP 字段只有 3 位（PM1_CNT bit10-12），合法值 0..7 */
        if (a > 7u || b > 7u) continue;
        *typa = a;
        *typb = b;
        return 1;
    }
    return 0;
}

void acpi_init(void) {
    A.ready = 0;
    A.pm1b_cnt = 0;
    A.smi_needed = 0;
    for (int i = 0; i < (int)sizeof(A.status); i++) A.status[i] = 0;

    /* 开临时映射窗口：ACPI 表在 RAM 高处（常在 32MB 以上），越出 identity
     * 就得靠它才能读。分页没起来就整条链走不通，一律 legacy 回退。 */
    g_win_ok = paging_enabled() ? 1 : 0;
    for (uint32_t i = 0; i < ACPI_WIN_SLOTS; i++) g_slot_phys[i] = 0;

    uint32_t rsdp = find_rsdp();
    if (!rsdp) {
        const char *s = "ACPI: RSDP not found - legacy QEMU power";
        for (int i = 0; s[i] && i < (int)sizeof(A.status) - 1; i++) A.status[i] = s[i];
        return;
    }

    collect_tables(rsdp);
    uint32_t fadt = find_table("FACP", 1);       /* FADT 常驻槽 1 */
    if (!fadt) {
        const char *s = "ACPI: FADT not found - legacy QEMU power";
        for (int i = 0; s[i] && i < (int)sizeof(A.status) - 1; i++) A.status[i] = s[i];
        return;
    }

    uint32_t fadt_len = rd32(fadt + 4);
    /* ACPI 表头（ACPI 6.x 5.2.6）：Signature(0) Length(4) Revision(8)
     * Checksum(9) OEMID(10,6) OEMTableID(16,8) OEMRevision(24)
     * CreatorID(28) CreatorRevision(32) —— Revision 在 **+8**。
     * 以前读 +9（那是 Checksum），状态行于是打出 "rev H1"（0xF1/10='H'），
     * 而且拿校验和去比 ">= 2" 决定要不要用 X_* GAS 字段，纯属撞运气。 */
    uint8_t fadt_rev = rd8(fadt + 8);
    uint32_t pm1a = rd32(fadt + 64);
    if (fadt_rev >= 2 && fadt + 268 + 12 <= fadt + fadt_len) {
        /* X_PM1a_CNT_BLK(+268) 是 12 字节 GAS：地址在 +4 处，且必须是
         * SystemIO、位宽足够，才比 legacy 的 32 位 PM1a_CNT_BLK 可信。 */
        gas_t g;
        gas_read(fadt + 268, &g);
        if (g.space == GAS_SPACE_IO && g.bit_width >= 16u && g.bit_offset == 0 &&
            g.address != 0 && g.address < 0x10000u) {
            pm1a = (uint32_t)g.address;
        }
    }
    uint32_t pm1b = rd32(fadt + 68);

    /* 真 BIOS 常见：ACPI 默认关闭，需 SMI 命令使能 PM 寄存器 */
    A.smi_cmd = (uint16_t)rd32(fadt + 48);
    A.acpi_enable = rd8(fadt + 52);
    if (A.smi_cmd != 0 && A.smi_cmd != 0xFFFF && A.acpi_enable != 0xFF) {
        A.smi_needed = 1;
    }

    /* DSDT：rev>=2 且 X_DSDT(+140) 有效则用 64 位地址。
     * X_DSDT 同样是 12 字节 GAS，且必须是 SystemMemory。 */
    uint32_t dsdt_pa = rd32(fadt + 40);
    if (fadt_rev >= 2 && fadt + 140 + 12 <= fadt + fadt_len) {
        gas_t g;
        gas_read(fadt + 140, &g);
        if (g.space == GAS_SPACE_MEMORY && g.address != 0 &&
            (g.address >> 32) == 0) {
            dsdt_pa = (uint32_t)g.address;
        }
    }
    uint32_t dsdt = acpi_map_table(dsdt_pa, "DSDT", 2);
    if (!dsdt) {
        const char *s = "ACPI: DSDT invalid - legacy QEMU power";
        for (int i = 0; s[i] && i < (int)sizeof(A.status) - 1; i++) A.status[i] = s[i];
        return;
    }

    /* \_S5 先在 DSDT 找，找不到再遍历 SSDT（规范允许它挂在任一张表） */
    collect_ssdt();
    uint8_t typa = 0, typb = 0;
    int found = find_s5(dsdt, &typa, &typb);
    for (uint32_t k = 0; !found && k < g_ssdt_count; k++) {
        uint32_t sv = acpi_map_table(g_ssdt[k], "SSDT", 3);
        if (sv) found = find_s5(sv, &typa, &typb);
    }
    if (!found) {
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
