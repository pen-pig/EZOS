/*
 * paging.c - 32-bit 分页实现（identity mapping，步骤 3）
 *
 * 两级页表（4KB 页）：PD 在 0x70000，页表池 0x71000 起最多 12 张（48KB）。
 * identity 覆盖 0-32MB，另映射 VBE LFB 8MB 窗口。全部页面 PTE_US=0
 * （仅 ring0），步骤 4 起再为用户映射打 PTE_US。
 *
 * 关键约束：
 *  - 页结构必须放在"保证空闲"的物理内存：低 1MB 已被镜像（0x10000-0x70000）、
 *    .bss（延伸至 ~0x89F60，含 idt_entries @ 0x70C00）、栈（自 0x90000 向下）
 *    与 VGA（0xA0000+）占满；.bss.hi 1-2MB 是堆/dmesg/FS 缓冲，仅剩几十 KB。
 *    故固定取 2MB（0x200000）：linker.ld 断言 __hbss_end <= 0x200000，
 *    而下一个使用者（GUI 背缓冲）在 16MB，中间 14MB 全空闲。
 *  - identity mapping 下虚拟地址 == 物理地址，所以页表指针可直接当物理地址
 *    填进 PDE——这是本阶段刻意保持的简化，步骤 6 换页时会改成显式 phys。
 *  - 修改页表后必须 invlpg；换 CR3 由硬件隐式全刷 TLB。
 *  - 本模块不依赖 kmalloc（页表池静态），可在 kmalloc 之前/崩溃路径安全调用。
 */
#include "paging.h"

#define PD  ((volatile uint32_t *)PAGING_PD_ADDR)

static uint32_t g_pt_used;          /* 已分配的页表数 */
static uint32_t g_mapped;           /* 已映射页数（统计用） */
static int      g_ready;            /* paging_init 是否已成功执行 */

/* ---------- 硬件原语 ---------- */

static uint32_t read_cr0(void) {
    uint32_t v;
    asm volatile("mov %%cr0, %0" : "=r"(v));
    return v;
}

static void write_cr0(uint32_t v) {
    asm volatile("mov %0, %%cr0" :: "r"(v) : "memory");
}

static void write_cr3(uint32_t v) {
    asm volatile("mov %0, %%cr3" :: "r"(v) : "memory");
}

static uint32_t read_cr3(void) {
    uint32_t v;
    asm volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

static void invlpg(uint32_t virt) {
    asm volatile("invlpg (%0)" :: "r"(virt) : "memory");
}

/* ---------- 页表池 ---------- */

/*
 * 当前活动页目录的（虚拟）地址。
 *
 * 步骤 6c 起用户进程有自己的页目录，页表 walk **必须跟着 CR3 走**：
 * 在进程上下文里若还在查固定内核页目录，用户页在那里要么是内核 identity
 * 映射（无 PTE_US），要么根本没映射——user_range_ok 会因此把合法的用户
 * 指针判成非法，所有 read/write 静默返回 -1（这个坑的表现是"用户程序
 * 退出码正常但一个字都打不出来"）。
 *
 * identity 映射下 CR3 里的物理地址可以直接当虚拟地址用。
 * 初始化阶段（CR3 还没装载）退回固定地址。
 */
static volatile uint32_t *pd_active(void) {
    if (!g_ready) return (volatile uint32_t *)PAGING_PD_ADDR;
    return (volatile uint32_t *)(read_cr3() & 0xFFFFF000u);
}

/* 取虚拟地址所属页表；create=1 时按需从池中分配一张（清零） */
static volatile uint32_t *pt_for(uint32_t virt, int create) {
    uint32_t pdi = virt >> 22;
    volatile uint32_t *pd = pd_active();
    if (pd[pdi] & PTE_P)
        /* identity mapping：PDE 里的物理地址可直接当虚拟地址用。
         * 注意必须用 **活动** 页目录的 PDE 取页表——拿固定内核页目录的
         * PDE 会在进程上下文里返回内核那张 4-8MB 表（用户页无 US 位），
         * user_range_ok 于是把合法用户指针判非法，write/read 静默 -1。 */
        return (volatile uint32_t *)(pd[pdi] & 0xFFFFF000u);

    if (!create) return NULL;
    if (g_pt_used >= PAGING_PT_MAX) return NULL;

    /* 页表池是**内核**的静态资源，只在内核页目录下分配。
     * 用户进程的用户区页表由 exec 单独从 pmm 分配并写进它自己的 PDE——
     * 若在进程页目录里往共享池里塞表，会污染所有进程的页目录。 */
    if (pd != (volatile uint32_t *)PAGING_PD_ADDR) return NULL;

    volatile uint32_t *pt =
        (volatile uint32_t *)(PAGING_PT_BASE + g_pt_used * PAGE_SIZE);
    g_pt_used++;
    for (uint32_t i = 0; i < 1024u; i++) pt[i] = 0;
    pd[pdi] = (uint32_t)pt | PTE_P | PTE_RW;
    return pt;
}

/* ---------- 对外 API ---------- */

int paging_map(uint32_t virt, uint32_t phys, uint32_t flags) {
    if (virt & 0xFFFu || phys & 0xFFFu) return -1;      /* 必须页对齐 */
    volatile uint32_t *pt = pt_for(virt, 1);
    if (!pt) return -1;

    uint32_t pti = (virt >> 12) & 0x3FFu;
    if (!(pt[pti] & PTE_P)) g_mapped++;
    pt[pti] = (phys & 0xFFFFF000u) | (flags & 0xFFFu) | PTE_P;

    /* 用户标志必须同步到 PDE：两级 walk 中 PDE.US=0 时 ring3 访问
     * 直接 #PF（err=P|U 保护违例），即便 PTE.US=1 也不会走到 PTE。
     * PDE 放行只是必要条件，每页真实权限仍由各 PTE 决定。 */
    if (flags & PTE_US) pd_active()[virt >> 22] |= PTE_US | PTE_RW;

    invlpg(virt);
    return 0;
}

void paging_unmap(uint32_t virt) {
    volatile uint32_t *pt = pt_for(virt, 0);
    if (!pt) return;
    uint32_t pti = (virt >> 12) & 0x3FFu;
    if (pt[pti] & PTE_P) g_mapped--;
    pt[pti] = 0;
    invlpg(virt);
}

int paging_query(uint32_t virt, uint32_t *out_phys, uint32_t *out_flags) {
    volatile uint32_t *pt = pt_for(virt, 0);
    if (!pt) return -1;
    uint32_t pte = pt[(virt >> 12) & 0x3FFu];
    if (!(pte & PTE_P)) return -1;
    if (out_phys)  *out_phys  = pte & 0xFFFFF000u;
    if (out_flags) *out_flags = pte & 0xFFFu;
    return 0;
}

int paging_enabled(void) { return (read_cr0() & 0x80000000u) != 0; }

uint32_t paging_cr3(void) {
    uint32_t v;
    asm volatile("mov %%cr3, %0" : "=r"(v));
    return v & 0xFFFFF000u;
}

void paging_switch_cr3(uint32_t pd_phys) {
    /* 页目录基址必须 4KB 对齐，低 12 位是 PCD/PWT 标志位（本 OS 不用） */
    write_cr3(pd_phys & 0xFFFFF000u);
}

uint32_t paging_mapped_pages(void) { return g_mapped; }
uint32_t paging_used_tables(void)  { return g_pt_used; }

/* ---------- 初始化 ---------- */

/* identity 映射 [0, end) */
static int identity_map(uint32_t end) {
    for (uint32_t v = 0; v < end; v += PAGE_SIZE)
        if (paging_map(v, v, PAGING_KRN_FLAGS) != 0) return -1;
    return 0;
}

/* 映射 VBE LFB（boot.asm 实模式探测结果存放在 0x5000）：
 *   0x5000: dword LFB 物理地址（0 = 无 LFB 模式，走 VGA 0x13 回退）
 *   0x5004/0x5006: 宽/高   0x5008: bpp
 * LFB 通常在 0xE0000000（QEMU std / Bochs VBE），不在 identity 区内，
 * 必须显式映射，否则首次进 GUI 写 LFB 会 #PF。
 */
static int map_lfb(void) {
    uint32_t lfb  = *(volatile uint32_t *)0x5000u;
    uint16_t vxr  = *(volatile uint16_t *)0x5004u;
    uint16_t vyr  = *(volatile uint16_t *)0x5006u;
    uint8_t  vbpp = *(volatile uint8_t  *)0x5008u;

    if (lfb < 0x00100000u || lfb >= 0xFFF00000u) return -1;   /* 无 LFB 模式 */
    if (vxr < 320u || vyr < 200u || vbpp != 16u) return -1;   /* 非 16bpp 线性模式 */

    /* 按需计算窗口：ceil(w*h*2, 4KB)，再取 8MB 上限内的较大者 */
    uint32_t need = (uint32_t)vxr * (uint32_t)vyr * 2u;
    need = (need + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
    if (need < PAGING_LFB_WINDOW) need = PAGING_LFB_WINDOW;

    uint32_t base = lfb & 0xFFFFF000u;
    for (uint32_t off = 0; off < need; off += PAGE_SIZE)
        if (paging_map(base + off, base + off, PAGING_MMIO_FLAGS) != 0) return -1;
    return 0;
}

int paging_init(void) {
    if (g_ready) return 0;

    /* 0) 页结构区可写性校验：该处若无物理 RAM（或被其他使用者占用），
     *    后续置位 CR0.PG 会立刻三重故障且无输出，必须提前拦下。 */
    PD[0] = 0xA5A5A5A5u;
    PD[1023] = 0x5A5A5A5Au;
    if (PD[0] != 0xA5A5A5A5u || PD[1023] != 0x5A5A5A5Au) return -1;

    /* 1) 页目录清零（PDE 全 0 = 全线性空间未映射） */
    for (uint32_t i = 0; i < 1024u; i++) PD[i] = 0;
    g_pt_used = 0;
    g_mapped  = 0;

    /* 2) identity 映射 0-32MB（含内核、堆、背缓冲、VGA、页表自身） */
    if (identity_map(PAGING_IDENTITY_END) != 0) return -1;

    /* 3) VBE LFB 映射（失败不代表错误：无 LFB 时走 VGA 0x13） */
    map_lfb();

    /* 4) 装载 CR3 并置位 CR0.PG
     *    顺序不可换：先 CR3 再 PG。mov cr3 会隐式全刷 TLB。
     *    identity mapping 保证下一条取指地址不变，无需跳转同步。 */
    write_cr3(PAGING_PD_ADDR);
    write_cr0(read_cr0() | 0x80000000u);

    g_ready = 1;
    return 0;
}

/* ---------- 自检 ---------- */

static void hex8(paging_puts_fn out, uint32_t v) {
    static const char h[] = "0123456789ABCDEF";
    char b[16];
    int n = 0;
    do { b[n++] = h[v & 0xFu]; v >>= 4; } while (v);
    while (n < 8) b[n++] = '0';
    while (n) { char c = b[--n]; char s[2] = { c, 0 }; out(s); }
}

/* 静默输出：开机自检路径用，丢弃所有文案 */
static void pg_null_puts(const char *s) { (void)s; }

int paging_selftest(paging_puts_fn out) {
    /* out==NULL：静默模式（开机自检用），只跑断言不打印 */
    if (!out) { out = pg_null_puts; }

    int fail = 0;

    out("paging self-test:\n");

    /* 1) CR0.PG */
    out("  CR0.PG=");
    out(paging_enabled() ? "1" : "0");
    out(" CR3=0x");
    hex8(out, paging_cr3());
    out("\n");
    if (!paging_enabled()) fail++;

    /* 2) identity 一致性：内核 / VGA / 高内存 BSS 三点抽样 */
    static const uint32_t probes[] = { 0x00010000u, 0x000B8000u, 0x00100000u, 0x01000000u };
    for (uint32_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        uint32_t phys = 0, fl = 0;
        int ok = (paging_query(probes[i], &phys, &fl) == 0) && (phys == probes[i]);
        out("  identity 0x"); hex8(out, probes[i]);
        out(" -> 0x");        hex8(out, phys);
        out(ok ? " [OK]\n" : " [FAIL]\n");
        if (!ok) fail++;
    }

    /* 3) 动态映射 + 读写回环：虚拟 0x04000000 -> 物理 0x00300000
     *    （3MB 处空闲：页结构占 2MB-0x210000，GUI 背缓冲在 16MB） */
    const uint32_t SCRATCH_VA = 0x04000000u;
    const uint32_t SCRATCH_PA = 0x00300000u;
    int rc = paging_map(SCRATCH_VA, SCRATCH_PA, PAGING_KRN_FLAGS);
    uint32_t phys = 0;
    int mapped_ok = (rc == 0) && (paging_query(SCRATCH_VA, &phys, 0) == 0) && (phys == SCRATCH_PA);

    /* 只有映射成功才解引用：映射失败时写 SCRATCH_VA 会立刻 #PF，
     * 把"页表池耗尽"这个真实原因掩盖成一次看似无关的缺页崩溃。 */
    int rw_ok = 0;
    if (mapped_ok) {
        volatile uint32_t *scratch = (volatile uint32_t *)SCRATCH_VA;
        uint32_t pattern = 0xC0FFEE00u;
        *scratch = pattern;
        rw_ok = (*scratch == pattern);
    }

    out("  map 0x"); hex8(out, SCRATCH_VA);
    out(" -> 0x");   hex8(out, SCRATCH_PA);
    out(mapped_ok ? " [OK]" : " [FAIL]");
    out(rw_ok ? " write/readback [OK]\n" : " write/readback [FAIL]\n");
    if (!mapped_ok || !rw_ok) fail++;

    /* 4) 解映射：PTE 应立刻变 non-present（不解引用，避免主动 #PF） */
    paging_unmap(SCRATCH_VA);
    int unmapped_ok = (paging_query(SCRATCH_VA, &phys, 0) != 0);
    out("  unmap 0x"); hex8(out, SCRATCH_VA);
    out(unmapped_ok ? " PTE cleared [OK]\n" : " still present [FAIL]\n");
    if (!unmapped_ok) fail++;

    /* 5) 统计 */
    out("  pages mapped: ");
    {
        uint32_t n = paging_mapped_pages();
        char buf[16];
        int i = 0;
        if (n == 0) buf[i++] = '0';
        while (n) { buf[i++] = (char)('0' + (n % 10u)); n /= 10u; }
        while (i) { char c = buf[--i]; char s[2] = { c, 0 }; out(s); }
    }
    out(" (page tables: ");
    {
        uint32_t n = paging_used_tables();
        char buf[16];
        int i = 0;
        if (n == 0) buf[i++] = '0';
        while (n) { buf[i++] = (char)('0' + (n % 10u)); n /= 10u; }
        while (i) { char c = buf[--i]; char s[2] = { c, 0 }; out(s); }
    }
    out("/");
    {
        uint32_t n = (uint32_t)PAGING_PT_MAX;
        char buf[16];
        int i = 0;
        if (n == 0) buf[i++] = '0';
        while (n) { buf[i++] = (char)('0' + (n % 10u)); n /= 10u; }
        while (i) { char c = buf[--i]; char s[2] = { c, 0 }; out(s); }
    }
    out(")\n");

    out(fail == 0 ? "  result: PASS\n" : "  result: FAIL\n");
    return fail;
}
