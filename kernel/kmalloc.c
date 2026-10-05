/*
 * kmalloc.c - 内核堆分配器实现（.bss.hi 静态池 + first-fit free-list）
 *
 * 布局（16B 对齐）：
 *   已分配块：[头 4B: size|1][请求大小 4B][魔数 4B][pad 4B][用户数据][尾部 canary 4B][pad]
 *   空闲块：  [头 4B: size|0][next 4B][剩余空间]
 * 返回指针 = 块 + 16（始终 16B 对齐）。已分配块不在空闲链表里，所以 next 槽
 * 空闲——用来存"用户请求的字节数"，kfree 才能算出尾部 canary 的位置。
 *
 * 两个魔数分工：
 *   头魔数（+8）  拦 UAF / 重复释放 / 指针不是 kmalloc 返回的
 *   尾 canary     拦越界写，紧贴用户区末尾（按 4 对齐），写完 1 字节就踩中
 * 两者都要有：只有头魔数时，越界写会一路踩进下一个块的头或别人的数据，
 * 要等到受害者被 kfree 才暴露，现场早就凉了。
 *
 * 分裂阈值：尾部剩余 >= 32B（头 8 + 魔数 4 + 最小用户 16，对齐后 32）。
 * 合并：kfree 立即前后合并（地址相邻判定）。
 *
 * 块大小恒为 16 的倍数（初始池、need、分裂剩余都是），kmalloc_audit 才能
 * 按 size 从头走到尾做全池体检。
 */
#include "kmalloc.h"
#include "panic.h"
#include "irqflags.h"

#define KM_POOL_SIZE   (384 * 1024)     /* 384KB 池 */
#define KM_ALIGN        16u
#define KM_HDR          16u             /* 头 4 + 请求大小 4 + 魔数 4 + pad 4 */
#define KM_TAIL         4u              /* 尾部 canary */
#define KM_MIN_SPLIT    32u
#define KM_MAGIC        0x4B4D4150u      /* "KMAP" 头魔数 */
#define KM_TAIL_MAGIC   0x5A5A5A5Au      /* 尾部 canary（越界写第一字节就踩中） */
#define KM_ALLOCATED    1u

typedef struct km_block {
    uint32_t size;                      /* 含头与魔数区，低位=分配标志 */
    struct km_block *next;               /* 空闲块：链表 */
} km_block_t;

#define KM_HIBUF __attribute__((section(".bss.hi")))
static uint8_t km_pool[KM_POOL_SIZE] KM_HIBUF;

static km_block_t *km_head;             /* 空闲链表头（地址升序） */
static uint32_t km_used_bytes;

static uint32_t blk_size(const km_block_t *b) { return b->size & ~KM_ALLOCATED; }
static int blk_is_free(const km_block_t *b)   { return (b->size & KM_ALLOCATED) == 0; }
static uint32_t round_up(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

/* 魔数固定在块头之后 4B（偏移 8..11），返回给用户的指针 = 块 + 16 */
static uint32_t *km_magic_of(km_block_t *b) {
    return (uint32_t *)((uint8_t *)b + 8);
}

/* 已分配块的 next 槽（偏移 4..7）改存"用户请求的字节数"——空闲链表不再
 * 需要它，而 kfree 只有指针、没有大小，算不出 canary 在哪。 */
static uint32_t *km_req_of(km_block_t *b) {
    return (uint32_t *)((uint8_t *)b + 4);
}

/* 尾部 canary：紧贴用户区末尾（4 对齐），越界写 1 字节即踩中 */
static uint32_t *km_tail_of(void *user, uint32_t req) {
    return (uint32_t *)((uint8_t *)user + round_up(req, 4u));
}

static void km_arm(km_block_t *b, uint32_t req, uint32_t total) {
    *km_req_of(b) = req;
    *km_magic_of(b) = KM_MAGIC;
    *km_tail_of((uint8_t *)b + KM_HDR, req) = KM_TAIL_MAGIC;
    b->size = total | KM_ALLOCATED;
}

void kmalloc_init(void) {
    km_block_t *b = (km_block_t *)km_pool;
    b->size = KM_POOL_SIZE;
    b->next = NULL;
    km_head = b;
    km_used_bytes = 0;
}

void *kmalloc(uint32_t size) {
    if (size == 0) return NULL;
    /* +KM_TAIL：尾部 canary 也要占位，否则用户区末尾正好顶到下一个块的头 */
    uint32_t need = round_up(size + KM_HDR + KM_TAIL, KM_ALIGN);

    /* 空闲链表的遍历、分割、摘除必须是原子的。net.c 的收发路径会在
     * IRQ11 上下文进入这里（net.c:185-186 的懒分配），若允许嵌套会撕裂
     * 链表——典型症状是同一块被分配两次。约束见 irqflags.h。 */
    uint32_t f = irq_save_disable();
    void *res = NULL;

    km_block_t *prev = NULL;
    for (km_block_t *b = km_head; b; prev = b, b = b->next) {
        if (!blk_is_free(b)) continue;
        uint32_t bs = blk_size(b);
        if (bs < need) continue;

        if (bs - need >= KM_MIN_SPLIT) {
            km_block_t *rest = (km_block_t *)((uint8_t *)b + need);
            rest->size = bs - need;
            rest->next = b->next;      /* 先接好尾部：km_arm 会占用 b 的 next 槽 */
            km_arm(b, size, need);
            if (prev) prev->next = rest;
            else km_head = rest;
            km_used_bytes += need;
            res = (uint8_t *)b + KM_HDR;
            break;
        }

        /* 不分裂：整体交出去。同样必须先摘链——km_arm 之后 b->next 槽里
         * 已经是请求大小了，那时再读会把链表接歪。 */
        km_block_t *nx = b->next;
        km_arm(b, size, bs);
        if (prev) prev->next = nx;
        else km_head = nx;
        km_used_bytes += bs;
        res = (uint8_t *)b + KM_HDR;
        break;
    }
    irq_restore(f);
    return res;
}

void kfree(void *ptr) {
    if (!ptr) return;
    km_block_t *b = (km_block_t *)((uint8_t *)ptr - KM_HDR);

    /* 下面的校验、插回、合并共用一段临界区：任何一步被 IRQ 上下文的另一次
     * kfree/kmalloc 打断，都会让空闲链表暂时处于不一致的中间态并被对方
     * 观察到（例如刚插回还没合并的块被当成两块分别分配出去）。
     * 各 panic 分支在赴死前先把中断状态恢复，不影响 panic 屏输出。 */
    uint32_t f = irq_save_disable();

    if ((uint8_t *)b < km_pool || (uint8_t *)b >= km_pool + KM_POOL_SIZE) {
        irq_restore(f);
        panic_set_context("kfree: pointer outside heap");
        asm volatile("ud2");
    }
    if (blk_is_free(b)) {
        irq_restore(f);
        panic_set_context("kfree: double free");
        asm volatile("ud2");
    }
    if (*km_magic_of(b) != KM_MAGIC) {
        irq_restore(f);
        panic_set_context("kfree: heap corruption (header smashed)");
        asm volatile("ud2");
    }
    uint32_t bs = blk_size(b);
    /* 尺寸上界校验：头部若被踩坏，bs 可能是荒谬值，后续的相邻判定与
     * 合并会算出越界地址。必须在合并之前拦下。 */
    if (bs < KM_HDR || bs > KM_POOL_SIZE ||
        (uint8_t *)b + bs > km_pool + KM_POOL_SIZE) {
        irq_restore(f);
        panic_set_context("kfree: corrupt block size");
        asm volatile("ud2");
    }
    /* 尾部 canary：用户区写穿时这里最先被改。比头魔数更早发现，且不依赖
     * 受害者什么时候才被释放。 */
    {
        uint32_t req = *km_req_of(b);
        if (req == 0 || req > KM_POOL_SIZE) {
            irq_restore(f);
            panic_set_context("kfree: corrupt request size");
            asm volatile("ud2");
        }
        uint32_t *tail = km_tail_of((uint8_t *)b + KM_HDR, req);
        if ((uint8_t *)tail + KM_TAIL > (uint8_t *)b + bs) {
            irq_restore(f);
            panic_set_context("kfree: canary outside block");
            asm volatile("ud2");
        }
        if (*tail != KM_TAIL_MAGIC) {
            irq_restore(f);
            panic_set_context("kfree: heap overflow (tail canary smashed)");
            asm volatile("ud2");
        }
    }
    km_used_bytes -= bs;

    /* 按地址序插回链表 */
    km_block_t *prev = NULL, *cur = km_head;
    while (cur && (uint8_t *)cur < (uint8_t *)b) {
        prev = cur;
        cur = cur->next;
    }
    b->size = bs;
    b->next = cur;
    if (prev) prev->next = b;
    else km_head = b;

    /* 前向合并 */
    if (prev && (uint8_t *)prev + blk_size(prev) == (uint8_t *)b) {
        prev->size = blk_size(prev) + bs;
        prev->next = b->next;
        b = prev;
    }
    /* 后向合并 */
    if (b->next && (uint8_t *)b + blk_size(b) == (uint8_t *)b->next) {
        b->size = blk_size(b) + blk_size(b->next);
        b->next = b->next->next;
    }

    irq_restore(f);
}

uint32_t kmalloc_total(void)    { return KM_POOL_SIZE; }
uint32_t kmalloc_used(void)     { return km_used_bytes; }

/* 全池体检：从头按 size 走遍每一个块，逐个校验已分配块的头魔数与尾部 canary。
 * 返回损坏块数（0 = 堆健康）。不修改任何东西，所以能在"怀疑有人越界但还
 * 不想停机"的时候随时调——kmtest 的越界演练靠它取证。 */
uint32_t kmalloc_audit(void) {
    uint32_t f = irq_save_disable();
    uint32_t bad = 0;
    uint8_t *p = km_pool;
    while (p + KM_HDR <= km_pool + KM_POOL_SIZE) {
        km_block_t *b = (km_block_t *)p;
        uint32_t bs = blk_size(b);
        /* 尺寸本身不可信（可能被踩坏）：立刻停，否则会顺着假 size 走出池外 */
        if (bs < KM_HDR || bs > KM_POOL_SIZE ||
            p + bs > km_pool + KM_POOL_SIZE) {
            bad++;
            break;
        }
        if (!blk_is_free(b)) {
            uint32_t req = *km_req_of(b);
            uint32_t *tail = km_tail_of(p + KM_HDR, req);
            if (*km_magic_of(b) != KM_MAGIC) bad++;
            else if (req == 0 || req > KM_POOL_SIZE) bad++;
            else if ((uint8_t *)tail + KM_TAIL > p + bs) bad++;
            else if (*tail != KM_TAIL_MAGIC) bad++;
        }
        p += bs;
    }
    irq_restore(f);
    return bad;
}

/* 仅供自检：越界演练踩坏 canary 之后把它装回去，好让 kfree 走正常路径。
 * 生产代码不该调这个——发现 canary 坏了的正确处理是修 bug，不是抹平现场。 */
void kmalloc_repair_tail(void *ptr) {
    if (!ptr) return;
    km_block_t *b = (km_block_t *)((uint8_t *)ptr - KM_HDR);
    uint32_t f = irq_save_disable();
    if (*km_magic_of(b) == KM_MAGIC) {
        uint32_t req = *km_req_of(b);
        *km_tail_of(ptr, req) = KM_TAIL_MAGIC;
    }
    irq_restore(f);
}

uint32_t kmalloc_largest_free(void) {
    /* 遍历空闲链表也要在临界区内：IRQ 上下文的 kmalloc/kfree 可能正改到
     * 一半（例如 prev->size 已合并、prev->next 还没摘），此时读到的最大
     * 空闲块是错的，shell `mem` 会报出与真实堆不一致的数字。 */
    uint32_t f = irq_save_disable();
    uint32_t max = 0;
    for (km_block_t *b = km_head; b; b = b->next)
        if (blk_is_free(b) && blk_size(b) > max) max = blk_size(b);
    irq_restore(f);
    return max;
}

