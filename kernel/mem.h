/*
 * mem.h - 内核 memcpy / memset / memcmp 声明。
 *
 * 签名与 libc 一致（n 是 uint32_t 而不是 size_t：内核是 32 位，
 * size_t 在这里就是 uint32_t，写 size_t 反而多一层 typedef 依赖）。
 *
 * 这些符号同时满足 Rust core 的链接需求，见 kernel/mem.c 的文件头注释。
 */
#ifndef EZ_MEM_H
#define EZ_MEM_H

#include "types.h"

void *memcpy(void *dst, const void *src, uint32_t n);
void *memset(void *dst, int v, uint32_t n);
int memcmp(const void *a, const void *b, uint32_t n);

#endif
