/*
 * kmalloc.c - 内核堆分配器实现（.bss.hi 静态池 + first-fit free-list）
 *
 * 布局（16B 对齐）：
 *   已分配块：[头 8B: size|1][魔数 4B][用户数据 ... ][填充到 16B 对齐]
 *   空闲块：  [头 8B: size|0][next 4B][剩余空间]
 * 魔数紧跟块头（固定偏移 8..11），不与用户区重叠——用户区从 +12 起，
 * 返回指针按 16 对齐意味着头+魔数共 12B + 4B pad = 16B 前缀。
 * 简化：返回指针 = 块 + 16（头 8 + 魔数 4 + pad 4），始终 16B 对齐。
 *
 * 分裂阈值：尾部剩余 >= 32B（头 8 + 魔数 4 + 最小用户 16，对齐后 32）。
 * 合并：kfree 立即前后合并（地址相邻判定）。
 */
#include "kmalloc.h"
#include "panic.h"

#define KM_POOL_SIZE   (384 * 1024)     /* 384KB 池 */
#define KM_ALIGN        16u
#define KM_HDR          16u             /* 头 8 + 魔数 4 + pad 4 */
#define KM_MIN_SPLIT    32u
#define KM_MAGIC        0x4B4D4150u      /* "KMAP" */
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

void kmalloc_init(void) {
    km_block_t *b = (km_block_t *)km_pool;
    b->size = KM_POOL_SIZE;
    b->next = NULL;
    km_head = b;
    km_used_bytes = 0;
}

void *kmalloc(uint32_t size) {
    if (size == 0) return NULL;
    uint32_t need = round_up(size + KM_HDR, KM_ALIGN);

    km_block_t *prev = NULL;
    for (km_block_t *b = km_head; b; prev = b, b = b->next) {
        if (!blk_is_free(b)) continue;
        uint32_t bs = blk_size(b);
        if (bs < need) continue;

        if (bs - need >= KM_MIN_SPLIT) {
            km_block_t *rest = (km_block_t *)((uint8_t *)b + need);
            rest->size = bs - need;
            rest->next = b->next;
            b->size = need | KM_ALLOCATED;
            if (prev) prev->next = rest;
            else km_head = rest;
            *km_magic_of(b) = KM_MAGIC;
            km_used_bytes += need;
            return (uint8_t *)b + KM_HDR;
        }

        b->size |= KM_ALLOCATED;
        if (prev) prev->next = b->next;
        else km_head = b->next;
        *km_magic_of(b) = KM_MAGIC;
        km_used_bytes += bs;
        return (uint8_t *)b + KM_HDR;
    }
    return NULL;
}

void kfree(void *ptr) {
    if (!ptr) return;
    km_block_t *b = (km_block_t *)((uint8_t *)ptr - KM_HDR);

    if ((uint8_t *)b < km_pool || (uint8_t *)b >= km_pool + KM_POOL_SIZE) {
        panic_set_context("kfree: pointer outside heap");
        asm volatile("ud2");
    }
    if (blk_is_free(b)) {
        panic_set_context("kfree: double free");
        asm volatile("ud2");
    }
    if (*km_magic_of(b) != KM_MAGIC) {
        panic_set_context("kfree: heap corruption (header smashed)");
        asm volatile("ud2");
    }
    uint32_t bs = blk_size(b);
    /* 尺寸上界校验：头部若被踩坏，bs 可能是荒谬值，后续的相邻判定与
     * 合并会算出越界地址。必须在合并之前拦下。 */
    if (bs < KM_HDR || bs > KM_POOL_SIZE ||
        (uint8_t *)b + bs > km_pool + KM_POOL_SIZE) {
        panic_set_context("kfree: corrupt block size");
        asm volatile("ud2");
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
}

uint32_t kmalloc_total(void)    { return KM_POOL_SIZE; }
uint32_t kmalloc_used(void)     { return km_used_bytes; }

uint32_t kmalloc_largest_free(void) {
    uint32_t max = 0;
    for (km_block_t *b = km_head; b; b = b->next)
        if (blk_is_free(b) && blk_size(b) > max) max = blk_size(b);
    return max;
}

