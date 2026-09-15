/*
 * elf.c - ELF32 加载器（步骤 5b）
 *
 * 只支持静态链接的 ET_EXEC（i386，小端）。动态链接要等有 ld.so 和
 * 用户态运行时之后再说——在那之前不实现，也不假装实现。
 */
#include "elf.h"
#include "paging.h"
#include "pmm.h"
#include "syscall.h"      /* USER_IMAGE_BASE / USER_IMAGE_END */

/* ELF32 结构（packed：按 gABI 布局，不能让编译器插填充） */
typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) elf32_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_offset;
    uint32_t p_vaddr;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
    uint32_t p_align;
} __attribute__((packed)) elf32_phdr_t;

/* ---------- 小工具（不依赖 libc） ---------- */

static void e_memset(void *dst, uint8_t v, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    for (uint32_t i = 0; i < n; i++) d[i] = v;
}

static void e_memcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

static uint32_t align_up(uint32_t v, uint32_t a) {
    return (v + a - 1u) & ~(a - 1u);
}

/* 检查 [start, start+len) 是否完全落在 [lo, hi) 内（防溢出写法） */
static int range_in(uint32_t start, uint32_t len, uint32_t lo, uint32_t hi) {
    if (len == 0) return (start >= lo) && (start < hi);
    if (start < lo) return 0;
    if (hi <= start) return 0;
    if (hi - start < len) return 0;
    return 1;
}

/* ---------- 校验 ---------- */

int elf_validate(const uint8_t *buf, uint32_t size, const char **why) {
    #define REJ(m) do { if (why) *why = (m); return -1; } while (0)

    if (buf == 0) REJ("null buffer");
    if (size < sizeof(elf32_ehdr_t)) REJ("file smaller than ELF header");

    const elf32_ehdr_t *eh = (const elf32_ehdr_t *)buf;

    /* magic: 0x7F 'E' 'L' 'F' */
    if (!(eh->e_ident[0] == 0x7Fu && eh->e_ident[1] == 'E' &&
          eh->e_ident[2] == 'L' && eh->e_ident[3] == 'F'))
        REJ("bad ELF magic");
    if (eh->e_ident[4] != ELFCLASS32) REJ("not ELFCLASS32");
    if (eh->e_ident[5] != ELFDATA2LSB) REJ("not little-endian");
    if (eh->e_ident[6] != EV_CURRENT) REJ("bad ELF version");
    if (eh->e_type != ET_EXEC) REJ("not ET_EXEC (static executable required)");
    if (eh->e_machine != EM_386) REJ("not EM_386 (i386)");

    /* 入口必须落在用户映像区内——否则等于跳进内核执行 */
    if (!range_in(eh->e_entry, 1u, USER_IMAGE_BASE, USER_IMAGE_END))
        REJ("entry point outside user image area");

    if (eh->e_phentsize != sizeof(elf32_phdr_t)) REJ("bad e_phentsize");
    if (eh->e_phnum == 0) REJ("no program headers");
    if (eh->e_phnum > ELF_MAX_SEG) REJ("too many program headers");

    /* 程序头表本身必须完整落在文件内 */
    uint32_t pht_bytes = (uint32_t)eh->e_phnum * sizeof(elf32_phdr_t);
    if (!range_in(eh->e_phoff, pht_bytes, 0u, size))
        REJ("program header table outside file");

    /* 逐个 PT_LOAD 做越界/合法性检查（此时还不碰页表） */
    const elf32_phdr_t *ph = (const elf32_phdr_t *)(buf + eh->e_phoff);
    uint32_t nload = 0;
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        nload++;
        if (ph[i].p_memsz == 0) continue;            /* 空段：跳过 */

        if (ph[i].p_filesz > ph[i].p_memsz) REJ("p_filesz > p_memsz");
        if (!range_in(ph[i].p_vaddr, ph[i].p_memsz, USER_IMAGE_BASE, USER_IMAGE_END))
            REJ("segment outside user image area");
        if (!range_in(ph[i].p_offset, ph[i].p_filesz, 0u, size))
            REJ("segment file range outside file");
        if (nload > ELF_MAX_SEG) REJ("too many PT_LOAD segments");
    }
    if (nload == 0) REJ("no PT_LOAD segments");
    return 0;
    #undef REJ
}

/* ---------- 加载 ---------- */

/* 回滚：把已经映射并分配的前 n 个段全部释放，保证失败时不留残骸 */
static void unwind(elf_image_t *img) {
    for (uint32_t s = 0; s < img->nseg; s++) {
        uint32_t va = img->seg[s].va;
        for (uint32_t k = 0; k < img->seg[s].pages; k++) {
            uint32_t phys = 0;
            if (paging_query(va, &phys, 0) == 0) pmm_free_page(phys);
            paging_unmap(va);
            va += PMM_PAGE_SIZE;
        }
    }
    img->nseg = 0;
}

int elf_load(const uint8_t *buf, uint32_t size, elf_image_t *img, const char **why) {
    #define FAIL(m) do { if (why) *why = (m); unwind(img); return -1; } while (0)

    if (img == 0) { if (why) *why = "null image"; return -1; }
    img->nseg = 0;
    img->entry = 0;
    img->brk = 0;

    if (elf_validate(buf, size, why) != 0) return -1;

    const elf32_ehdr_t *eh = (const elf32_ehdr_t *)buf;
    const elf32_phdr_t *ph = (const elf32_phdr_t *)(buf + eh->e_phoff);

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_memsz == 0) continue;

        uint32_t va_start = ph[i].p_vaddr & ~(PMM_PAGE_SIZE - 1u);
        uint32_t va_end   = align_up(ph[i].p_vaddr + ph[i].p_memsz, PMM_PAGE_SIZE);
        uint32_t pages    = (va_end - va_start) / PMM_PAGE_SIZE;

        if (img->nseg >= ELF_MAX_SEG) FAIL("segment table overflow");

        /* 连续物理页：便于整段 memcpy，也便于一次性回收 */
        uint32_t phys = pmm_alloc_pages(pages);
        if (phys == 0) FAIL("out of physical pages");

        /* 先以 RW 映射（要写内容），最后再按段的 PF_W 收紧 */
        for (uint32_t k = 0; k < pages; k++) {
            if (paging_map(va_start + k * PMM_PAGE_SIZE,
                           phys + k * PMM_PAGE_SIZE,
                           PAGING_USR_FLAGS) != 0) {
                pmm_free_pages(phys, pages);
                FAIL("paging_map failed");
            }
        }

        img->seg[img->nseg].va    = va_start;
        img->seg[img->nseg].pages = pages;
        img->seg[img->nseg].phys  = phys;
        img->nseg++;

        /* 整段清零：既保证 bss 为零，也避免把上一任使用者的内核数据
         * 泄漏给用户态（页刚从 pmm 出来，内容未知）。 */
        e_memset((void *)va_start, 0, pages * PMM_PAGE_SIZE);

        /* 文件部分 */
        if (ph[i].p_filesz != 0)
            e_memcpy((void *)ph[i].p_vaddr, buf + ph[i].p_offset, ph[i].p_filesz);

        /* W^X：不可写段改为只读（无 NX，执行不可禁，但至少不能就地改码） */
        if ((ph[i].p_flags & PF_W) == 0u) {
            for (uint32_t k = 0; k < pages; k++) {
                paging_map(va_start + k * PMM_PAGE_SIZE,
                           phys + k * PMM_PAGE_SIZE,
                           PTE_US);
            }
        }

        uint32_t seg_end = ph[i].p_vaddr + ph[i].p_memsz;
        if (seg_end > img->brk) img->brk = align_up(seg_end, PMM_PAGE_SIZE);
    }

    img->entry = eh->e_entry;
    return 0;
    #undef FAIL
}

int elf_unload(const elf_image_t *img) {
    if (img == 0) return -1;
    for (uint32_t s = 0; s < img->nseg; s++) {
        uint32_t va = img->seg[s].va;
        /* 用装载时记录的物理地址归还，不回查页表：
         * 页表是"共享可变状态"，回查会把回收的正确性建立在
         * "这期间没人动过 PTE"这个假设上。谁分配谁记账，回收只看账。 */
        uint32_t phys = img->seg[s].phys;
        for (uint32_t k = 0; k < img->seg[s].pages; k++) {
            pmm_free_page(phys + k * PMM_PAGE_SIZE);
            paging_unmap(va);
            va += PMM_PAGE_SIZE;
        }
    }
    return 0;
}

/* ---------- 自检 ---------- */

static void eputs_num(elf_puts_fn out, uint32_t v) {
    char s[12];
    int n = 0;
    if (v == 0) s[n++] = '0';
    while (v) { s[n++] = (char)('0' + v % 10u); v /= 10u; }
    while (n) { char c = s[--n]; char t[2] = { c, 0 }; out(t); }
}

/*
 * 自检要覆盖的是"拒绝"路径：畸形 ELF 必须在 elf_validate 阶段就被挡下，
 * 且不能碰页表（用 pmm_used_pages 前后一致来证明没有页泄漏）。
 */
/* 静默输出：开机自检路径用，丢弃所有文案 */
static void elf_null_puts(const char *s) { (void)s; }

int elf_selftest(elf_puts_fn out) {
    /* out==NULL：静默模式（开机自检用），只跑断言不打印 */
    if (!out) { out = elf_null_puts; }
    int fail = 0;

    out("elf self-test:\n");

    /* 构造一个最小合法 ELF：1 个 PT_LOAD，代码段在 0x400000 */
    static uint8_t good[sizeof(elf32_ehdr_t) + sizeof(elf32_phdr_t) + 16];
    e_memset(good, 0, sizeof(good));
    elf32_ehdr_t *eh = (elf32_ehdr_t *)good;
    good[0] = 0x7F; good[1] = 'E'; good[2] = 'L'; good[3] = 'F';
    good[4] = ELFCLASS32; good[5] = ELFDATA2LSB; good[6] = EV_CURRENT;
    eh->e_type = ET_EXEC;
    eh->e_machine = EM_386;
    eh->e_version = EV_CURRENT;
    eh->e_entry = USER_IMAGE_BASE;
    eh->e_phoff = sizeof(elf32_ehdr_t);
    eh->e_phentsize = sizeof(elf32_phdr_t);
    eh->e_phnum = 1;
    elf32_phdr_t *ph = (elf32_phdr_t *)(good + sizeof(elf32_ehdr_t));
    ph->p_type = PT_LOAD;
    ph->p_offset = sizeof(elf32_ehdr_t) + sizeof(elf32_phdr_t);
    ph->p_vaddr = USER_IMAGE_BASE;
    ph->p_filesz = 4;
    ph->p_memsz = 4;
    ph->p_flags = PF_R | PF_X;

    const char *why = 0;
    int ok = (elf_validate(good, sizeof(good), &why) == 0);
    out("  valid ELF accepted: ");
    out(ok ? "yes [OK]\n" : "NO [FAIL] ");
    if (!ok) { out(why ? why : "?"); out("\n"); fail++; }

    /* --- 以下都必须是"拒绝"，且不得泄漏页 --- */
    struct { const char *name; uint32_t off; uint8_t val; } bad[] = {
        { "bad magic",        0, 0x00 },
        { "bad class",        4, 2    },
        { "bad endianness",   5, 2    },
    };
    for (uint32_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        uint8_t old = good[bad[i].off];
        good[bad[i].off] = bad[i].val;
        uint32_t used_before = pmm_used_pages();
        ok = (elf_validate(good, sizeof(good), &why) != 0);
        ok = ok && (pmm_used_pages() == used_before);   /* 校验阶段不得动页 */
        out("  reject "); out(bad[i].name); out(": ");
        out(ok ? "yes [OK]\n" : "NO [FAIL]\n");
        if (!ok) fail++;
        good[bad[i].off] = old;
    }

    /* 入口指向内核空间（0x00100000 = .bss.hi）必须拒绝 */
    {
        uint32_t old_entry = eh->e_entry;
        eh->e_entry = 0x00100000u;
        ok = (elf_validate(good, sizeof(good), &why) != 0);
        out("  reject entry into kernel: ");
        out(ok ? "yes [OK]\n" : "NO [FAIL]\n");
        if (!ok) fail++;
        eh->e_entry = old_entry;
    }

    /* 段伸到用户区之外必须拒绝 */
    {
        uint32_t old_memsz = ph->p_memsz;
        ph->p_memsz = 0x10000000u;                     /* 256MB，远超映像区 */
        ok = (elf_validate(good, sizeof(good), &why) != 0);
        out("  reject oversized segment: ");
        out(ok ? "yes [OK]\n" : "NO [FAIL]\n");
        if (!ok) fail++;
        ph->p_memsz = old_memsz;
    }

    /* 文件范围越界（p_offset+filesz 超出文件大小）必须拒绝 */
    {
        uint32_t old_off = ph->p_offset, old_fsz = ph->p_filesz;
        ph->p_offset = 0xFFFFF000u;
        ph->p_filesz = 0x2000u;
        ok = (elf_validate(good, sizeof(good), &why) != 0);
        out("  reject segment past EOF: ");
        out(ok ? "yes [OK]\n" : "NO [FAIL]\n");
        if (!ok) fail++;
        ph->p_offset = old_off; ph->p_filesz = old_fsz;
    }

    /* 真加载一次：成功 + 内容正确 + 卸载后页全部归还 */
    {
        uint32_t used_before = pmm_used_pages();
        elf_image_t img;
        /* 把 4 字节 payload 写成一个可识别的立即数（0x90 0x90 0xEB 0xFE = nop;nop;jmp .） */
        uint8_t *payload = good + sizeof(elf32_ehdr_t) + sizeof(elf32_phdr_t);
        payload[0] = 0x90; payload[1] = 0x90; payload[2] = 0xEB; payload[3] = 0xFE;
        ok = (elf_load(good, sizeof(good), &img, &why) == 0);
        out("  load: ");
        out(ok ? "[OK]\n" : "[FAIL] ");
        if (!ok) { out(why ? why : "?"); out("\n"); fail++; }
        if (ok) {
            /* 段只读（PF_W 未置）-> W^X：读可以，写应被拒 */
            volatile uint8_t *code = (volatile uint8_t *)USER_IMAGE_BASE;
            out("  payload at entry: ");
            out((code[0] == 0x90 && code[2] == 0xEB) ? "readback OK\n" : "readback FAIL\n");
            if (!(code[0] == 0x90 && code[2] == 0xEB)) fail++;

            uint32_t phys = 0, flags = 0;
            int q = paging_query(USER_IMAGE_BASE, &phys, &flags);
            int ro = (q == 0) && ((flags & PTE_RW) == 0u) && ((flags & PTE_US) != 0u);
            out("  W^X (text not writable): ");
            out(ro ? "yes [OK]\n" : "NO [FAIL]\n");
            if (!ro) fail++;

            elf_unload(&img);
            out("  unload returns pages: ");
            out((pmm_used_pages() == used_before) ? "yes [OK]\n" : "NO [FAIL]\n");
            if (pmm_used_pages() != used_before) {
                out("  leaked "); eputs_num(out, pmm_used_pages() - used_before);
                out(" page(s)\n");
                fail++;
            }
        }
    }

    out(fail == 0 ? "  result: PASS\n" : "  result: FAIL\n");
    return fail;
}
