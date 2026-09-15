/*
 * calc.c - shell 浮点计算器：递归下降解析 + x87 求值（步骤 5e）
 *
 * 设计要点：
 *   - 解析与求值一遍完成（AST 省掉：输入最长 SHELL 一行 128 字符）
 *   - 所有指针推进都有边界检查。输入是用户敲的，任何畸形串都只能得到
 *     一个错误消息，绝不能越界读
 *   - 除零/负数开方：x87 默认 CW 屏蔽异常，结果是 ±Inf/NaN；
 *     入口处统一做有限性检查并报错（打印 Inf/NaN 对终端没意义）
 *   - 无 FPU 的机器上直接拒绝运算（fpu_available），不装模作样
 *
 * 数学函数自备实现（i686-elf-gcc 的 libgcc 不含 math 库）：
 *   sqrt  牛顿迭代（初值直接取 x 本身——[0.5,2] 外会多迭代几轮，
 *         但迭代次数有上限 60，收敛即停，不会死循环）
 *   exp   先折半归约 |x|<1 再泰勒，平方还原
 *   ln    归约到 [0.5,1) 的 ln(1+u) 交错级数 + k*ln2
 *   sin   模 2π 象限归约 + 泰勒；cos = sin(x+π/2)；tan = sin/cos
 *   pow   e^(ln b * e)，负底数仅接受整数幂
 */
#include "calc.h"
#include "fpu.h"

/* ---------- 前置声明 ---------- */
typedef struct parser_s parser_t;
static double p_expr(parser_t *p);
static int str2(const char *s, char a, char b);
static int str3(const char *s, char a, char b, char c);
static int str4(const char *s, char a, char b, char c, char d);
static int str5(const char *s, char a, char b, char c, char d, char e);

/* ---------- 解析状态 ---------- */
struct parser_s {
    const char *s;      /* 当前读位置 */
    const char *err;    /* 错误消息（NULL=无错） */
};

/* ---------- 数学实现 ---------- */
static double c_fabs(double x) { return x < 0 ? -x : x; }

static double c_sqrt(double x) {
    if (x < 0) return 0.0 / 0.0;                       /* NaN */
    if (x == 0) return 0;
    double r = x, last;
    for (int i = 0; i < 60; i++) {
        last = r;
        r = 0.5 * (r + x / r);
        if (c_fabs(r - last) <= 1e-15 * c_fabs(r)) break;
    }
    return r;
}

static double c_exp(double x) {
    int n = 0;
    while (x > 1)  { x *= 0.5; n++; }
    while (x < -1) { x *= 0.5; n++; }
    double term = 1, sum = 1;
    for (int i = 1; i < 30; i++) { term *= x / (double)i; sum += term; }
    while (n-- > 0) sum *= sum;
    return sum;
}

static double c_ln(double x) {
    if (x <= 0) return 0.0 / 0.0;
    int k = 0;
    while (x >= 1)  { x *= 0.5; k++; }
    while (x < 0.5) { x *= 2;   k--; }
    double u = x - 1, term = u, sum = 0;
    for (int i = 1; i < 40; i++) {
        sum += term / (double)(i & 1 ? i : -i);
        term *= u;
    }
    return sum + 0.6931471805599453094 * (double)k;
}

static double c_log10(double x) { return c_ln(x) / 2.302585092994045684; }

static double c_sin(double x) {
    const double PI = 3.14159265358979323846;
    int sign = 1;
    x = x - (double)(long)(x / (2 * PI)) * (2 * PI);
    if (x < 0) x += 2 * PI;
    if (x > PI) { x -= PI; sign = -sign; }
    if (x > PI / 2) { x = PI - x; sign = -sign; }
    double term = x, sum = x;
    for (int i = 3; i < 15; i += 2) {
        term *= -x * x / (double)((i - 1) * i);
        sum += term;
    }
    return sign < 0 ? -sum : sum;
}

static double c_cos(double x) { return c_sin(x + 1.57079632679489661923); }

static double c_tan(double x) {
    double c = c_cos(x);
    if (c == 0) return 1e308 * 10;                     /* Inf */
    return c_sin(x) / c;
}

static double c_floor(double x) {
    long i = (long)x;
    if (x < 0 && (double)i != x) i--;
    return (double)i;
}
static double c_ceil(double x)  { return -c_floor(-x); }
static double c_round(double x) {
    return x < 0 ? -c_floor(-x + 0.5) : c_floor(x + 0.5);
}

static double c_pow(double b, double e) {
    if (b == 0) return e == 0 ? 1 : 0;
    if (b < 0) {
        long ie = (long)e;
        if ((double)ie == e) {
            double r = c_exp(c_ln(-b) * e);
            return (ie & 1) ? -r : r;
        }
        return 0.0 / 0.0;
    }
    return c_exp(c_ln(b) * e);
}

/* ---------- 字符分类 ---------- */
static int is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static int is_digit(char c) { return c >= '0' && c <= '9'; }

/* ---------- 解析器 ---------- */

static void skip_ws(parser_t *p) {
    while (p->s[0] == ' ' || p->s[0] == '\t') p->s++;
}

static double p_unary(parser_t *p);

static double p_primary(parser_t *p) {
    skip_ws(p);
    const char *s = p->s;

    if (s[0] == '(') {
        p->s = s + 1;
        double v = p_expr(p);
        skip_ws(p);
        if (p->err) return 0;
        if (p->s[0] != ')') { p->err = "expected ')'"; return 0; }
        p->s++;
        return v;
    }

    if (is_alpha(s[0])) {
        const char *start = s;
        while (is_alpha(s[0])) s++;
        uint32_t len = (uint32_t)(s - start);

        /* 常量 */
        if (len == 2 && str2(start, 'p','i')) { p->s = s; return 3.14159265358979323846; }
        if (len == 1 && start[0] == 'e')      { p->s = s; return 2.71828182845904523536; }

        /* 函数：fn ( expr ) */
        double (*fn)(double) = 0;
        if      (len == 4 && str4(start,'s','q','r','t'))                  fn = c_sqrt;
        else if (len == 3 && str3(start,'s','i','n'))                      fn = c_sin;
        else if (len == 3 && str3(start,'c','o','s'))                      fn = c_cos;
        else if (len == 3 && str3(start,'t','a','n'))                      fn = c_tan;
        else if (len == 3 && str3(start,'e','x','p'))                      fn = c_exp;
        else if (len == 3 && str3(start,'l','o','g'))                      fn = c_log10;
        else if (len == 2 && str2(start,'l','n'))                          fn = c_ln;
        else if (len == 3 && str3(start,'a','b','s'))                      fn = c_fabs;
        else if (len == 4 && str4(start,'f','a','b','s'))                  fn = c_fabs;
        else if (len == 5 && str5(start,'f','l','o','o','r'))              fn = c_floor;
        else if (len == 4 && str4(start,'c','e','i','l'))                  fn = c_ceil;
        else if (len == 5 && str5(start,'r','o','u','n','d'))             fn = c_round;
        else if (len == 3 && str3(start,'p','o','w')) { p->err = "pow(a,b) is written as a^b"; return 0; }
        else { p->err = "unknown function or constant"; return 0; }

        p->s = s;
        skip_ws(p);
        if (p->s[0] != '(') { p->err = "expected '(' after function name"; return 0; }
        p->s++;
        double v = p_expr(p);
        skip_ws(p);
        if (p->err) return 0;
        if (p->s[0] != ')') { p->err = "expected ')'"; return 0; }
        p->s++;
        return fn(v);
    }

    /* 数字：digits [ '.' digits ] [ ('e'|'E') ['+'|'-'] digits ] */
    {
        const char *d = s;
        uint32_t int_len = 0, frac_len = 0;
        while (is_digit(d[0])) { d++; int_len++; }
        if (d[0] == '.') {
            d++;
            while (is_digit(d[0])) { d++; frac_len++; }
        }
        if (int_len + frac_len == 0) { p->err = "expected a number"; return 0; }

        /* 科学计数（必须有指数数字才算） */
        int64_t expo = 0;
        if (d[0] == 'e' || d[0] == 'E') {
            const char *e = d + 1;
            int neg = 0;
            if (e[0] == '+' || e[0] == '-') { neg = (e[0] == '-'); e++; }
            if (is_digit(e[0])) {
                while (is_digit(e[0])) { expo = expo * 10 + (e[0] - '0'); e++; }
                if (neg) expo = -expo;
                d = e;
            }
        }

        /* 手工转 double（不依赖 sscanf） */
        double v = 0;
        {
            const char *q = s;
            while (is_digit(q[0])) { v = v * 10 + (double)(q[0] - '0'); q++; }
            if (q[0] == '.') {
                q++;
                double scale = 0.1;
                while (is_digit(q[0])) { v += (double)(q[0] - '0') * scale; scale *= 0.1; q++; }
            }
        }
        while (expo >  0) { v *= 10; expo--; }
        while (expo <  0) { v /= 10; expo++; }
        p->s = d;
        return v;
    }
}

static double p_unary(parser_t *p) {
    skip_ws(p);
    if (p->s[0] == '+') { p->s++; return p_unary(p); }
    if (p->s[0] == '-') { p->s++; return -p_unary(p); }
    return p_primary(p);
}

static double p_factor(parser_t *p) {
    double base = p_unary(p);
    if (p->err) return 0;
    skip_ws(p);
    if (p->s[0] == '^') {
        p->s++;
        double e = p_factor(p);                          /* 右结合 */
        if (p->err) return 0;
        return c_pow(base, e);
    }
    return base;
}

static double p_term(parser_t *p) {
    double v = p_factor(p);
    if (p->err) return 0;
    for (;;) {
        skip_ws(p);
        char op = p->s[0];
        if (op != '*' && op != '/' && op != '%') break;
        p->s++;
        double r = p_factor(p);
        if (p->err) return 0;
        if (op == '*') {
            v *= r;
        } else if (op == '/') {
            if (r == 0) { p->err = "division by zero"; return 0; }
            v /= r;
        } else {                                        /* 浮点模，符号随被除数 */
            if (r == 0) { p->err = "modulo by zero"; return 0; }
            v = v - c_floor(v / r) * r;
        }
    }
    return v;
}

static double p_expr(parser_t *p) {
    double v = p_term(p);
    if (p->err) return 0;
    for (;;) {
        skip_ws(p);
        char op = p->s[0];
        if (op != '+' && op != '-') break;
        p->s++;
        double r = p_term(p);
        if (p->err) return 0;
        if (op == '+') v += r; else v -= r;
    }
    return v;
}

/* ---------- 入口 ---------- */

int calc_eval(const char *expr, double *out, const char **err) {
    if (expr == 0 || out == 0) { if (err) *err = "null expression"; return -1; }
    if (!fpu_available())      { if (err) *err = "no FPU present";   return -2; }

    parser_t p;
    p.s = expr;
    p.err = 0;

    double v = p_expr(&p);
    skip_ws(&p);
    if (p.err == 0 && p.s[0] != '\0') p.err = "unexpected trailing characters";
    if (p.err) { if (err) *err = p.err; return -3; }

    if (v != v)     { if (err) *err = "result is NaN";      return -4; }
    if (v - v != 0) { if (err) *err = "result is infinite"; return -4; }

    *out = v;
    if (err) *err = 0;
    return 0;
}

/* ---------- 匹配辅助（前置声明在文件头） ---------- */
static int str2(const char *s, char a, char b) {
    return s[0] == a && s[1] == b;
}
static int str3(const char *s, char a, char b, char c) {
    return s[0] == a && s[1] == b && s[2] == c;
}
static int str4(const char *s, char a, char b, char c, char d) {
    return s[0] == a && s[1] == b && s[2] == c && s[3] == d;
}
static int str5(const char *s, char a, char b, char c, char d, char e) {
    return s[0] == a && s[1] == b && s[2] == c && s[3] == d && s[4] == e;
}
