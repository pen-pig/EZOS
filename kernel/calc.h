/*
 * calc.h - shell 浮点计算器（步骤 5e）
 *
 * 递归下降表达式解析器，语法：
 *
 *   expr   := term (('+'|'-') term)*
 *   term   := factor (('*'|'/'|'%') factor)*
 *   factor := unary ('^' factor)?            右结合
 *   unary  := ('+'|'-')* primary
 *   primary:= number | '(' expr ')' | func '(' expr ')' | const
 *   func   := sqrt | sin | cos | tan | exp | log | ln | abs
 *             fabs | floor | ceil | round
 *   const  := pi | e
 *   number := digits [ '.' digits ] [ ('e'|'E') ['+'|'-'] digits ]
 *
 * 支持 x87（fpu_init 探测）；无 FPU 时返回错误而不是崩溃。
 */
#ifndef CALC_H
#define CALC_H

#include "types.h"

/*
 * 求值一行表达式。成功返回 0 且 *out 为结果；失败返回非 0，
 * err（可为 NULL）指向静态错误消息（不动态分配，不释放）。
 */
int calc_eval(const char *expr, double *out, const char **err);

#endif
