/*
 * lockselftest.h - 内存分配器临界区保护自检（P1 无锁隐患的回归网）
 */
#ifndef LOCKSELFTEST_H
#define LOCKSELFTEST_H

/* 返回失败用例数，0 = 全部通过；每项结果逐行 klog（LOCKTEST: <name> ...） */
int lock_selftest(void);

#endif /* LOCKSELFTEST_H */
