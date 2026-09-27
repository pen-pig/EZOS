/*
 * sysvol.c - 内核内置只读系统卷（挂载点 /system 与 /bin）
 *
 * 内容来自 kernel/sysvol_data.c（tools/make_sysvol.py 生成），在 .rodata 里，
 * 因此 `format` 任何数据盘都擦不掉它 —— 这是它存在的核心理由。
 *
 * 本模块只做三件事：按路径查找、读、列目录。写操作一律不在本模块实现，
 * 由 fs.c 在路由处直接拒绝（返回 -1）。
 */
#include "sysvol.h"
#include "dmesg.h"

/* freestanding：没有 libc，字符串工具自己实现（仅本文件用） */
static uint32_t sv_len(const char *s) {
    uint32_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static char sv_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

/* 跳过前导 '/' 与空格，让 "/bin/x"、"bin/x"、"/system" 都能用 */
static const char *sv_skip(const char *p) {
    while (*p == '/' || *p == ' ') p++;
    return p;
}

/* 大小写不敏感比较；容忍两侧多余的尾随 '/' */
static int sv_ci_eq(const char *a, const char *b) {
    a = sv_skip(a);
    b = sv_skip(b);
    while (*a && *b) {
        if (sv_lower(*a) != sv_lower(*b)) return 0;
        a++;
        b++;
    }
    while (*a == '/') a++;
    while (*b == '/') b++;
    return (*a == 0 && *b == 0);
}

/* 首段是否等于 seg（后面必须是 '/' 或结束，避免 "/binabc" 误判成 "/bin"） */
static int sv_seg_is(const char *p, const char *seg) {
    p = sv_skip(p);
    if (*p == 0) return 0;
    while (*seg) {
        if (sv_lower(*p) != sv_lower(*seg)) return 0;
        p++;
        seg++;
    }
    return (*p == '/' || *p == 0 || *p == ' ');
}

/* 十进制打印（dmesg_write 只吃字符串） */
static void sv_put_u(char *buf, uint32_t *pos, uint32_t cap, uint32_t v) {
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0 && *pos + 1 < cap) buf[(*pos)++] = tmp[--n];
}

void sysvol_init(void) {
    char line[80];
    uint32_t p = 0;
    const char *head = "SYSVOL: ready, ";
    while (head[p]) { line[p] = head[p]; p++; }
    sv_put_u(line, &p, sizeof(line), sysvol_count);
    const char *mid = " files, ";
    for (uint32_t i = 0; mid[i] && p + 1 < sizeof(line); i++) line[p++] = mid[i];
    sv_put_u(line, &p, sizeof(line), sysvol_bytes);
    const char *tail = " bytes, read-only (/system /bin)";
    for (uint32_t i = 0; tail[i] && p + 1 < sizeof(line); i++) line[p++] = tail[i];
    line[p] = 0;
    dmesg_write(line);
}

int sysvol_is_path(const char *path) {
    if (path == 0 || path[0] == 0) return 0;
    if (sv_seg_is(path, "bin")) return 1;
    if (sv_seg_is(path, "system")) return 1;
    return 0;
}

int sysvol_lookup(const char *path) {
    if (path == 0) return -1;
    for (uint32_t i = 0; i < sysvol_count; i++) {
        if (sv_ci_eq(sysvol_table[i].path, path)) return (int)i;
    }
    /* 不带扩展名时补 ".elf"：让 `exec hello` 与 `exec hello.elf` 都行 */
    uint32_t n = sv_len(path);
    if (n == 0 || n + 4 >= SYSVOL_MAX_PATH) return -1;
    for (uint32_t k = 0; k < n; k++) {
        if (path[k] == '.') return -1;          /* 已有扩展名就不再补 */
    }
    char alt[SYSVOL_MAX_PATH];
    for (uint32_t k = 0; k < n; k++) alt[k] = path[k];
    alt[n] = '.'; alt[n + 1] = 'e'; alt[n + 2] = 'l'; alt[n + 3] = 'f';
    alt[n + 4] = 0;
    for (uint32_t i = 0; i < sysvol_count; i++) {
        if (sv_ci_eq(sysvol_table[i].path, alt)) return (int)i;
    }
    return -1;
}

uint32_t sysvol_size(const char *path) {
    int i = sysvol_lookup(path);
    if (i < 0) return 0;
    return sysvol_table[i].size;
}

int sysvol_read(const char *path, uint8_t *buffer, uint32_t max_size) {
    int i = sysvol_lookup(path);
    if (i < 0 || buffer == 0) return -1;
    uint32_t sz = sysvol_table[i].size;
    if (sz > max_size) sz = max_size;
    const uint8_t *src = sysvol_blob + sysvol_table[i].off;
    for (uint32_t k = 0; k < sz; k++) buffer[k] = src[k];
    return (int)sz;
}

int sysvol_list(const char *dir, fs_dir_entry_t *out, int max_entries) {
    if (out == 0 || max_entries <= 0) return -1;

    /* 根目录：只有 bin 与 system 两个子目录 */
    const char *d = sv_skip(dir);
    if (*d == 0) {
        int n = 0;
        if (n < max_entries) {
            out[n].name[0] = 'b'; out[n].name[1] = 'i'; out[n].name[2] = 'n';
            out[n].name[3] = 0;
            out[n].size = 0; out[n].is_dir = 1;
            n++;
        }
        if (n < max_entries) {
            const char *s = "system";
            for (uint32_t k = 0; s[k]; k++) out[n].name[k] = s[k];
            out[n].size = 0; out[n].is_dir = 1;
            n++;
        }
        return n;
    }

    /* 子目录：列出父目录正好等于 d 的条目（深层条目不展开，保持一层） */
    uint32_t dlen = sv_len(d);
    int n = 0;
    for (uint32_t i = 0; i < sysvol_count && n < max_entries; i++) {
        const char *p = sv_skip(sysvol_table[i].path);
        uint32_t k = 0;
        while (k < dlen && sv_lower(p[k]) == sv_lower(d[k])) k++;
        if (k != dlen) continue;
        if (p[k] != '/') continue;
        const char *leaf = p + k + 1;
        /* 只取一层：叶子段里还有 '/' 说明是更深层的条目，跳过 */
        uint32_t l = 0;
        int deep = 0;
        while (leaf[l]) {
            if (leaf[l] == '/') { deep = 1; break; }
            l++;
        }
        if (deep || l == 0 || l >= sizeof(out[n].name)) continue;
        for (uint32_t m = 0; m < l; m++) out[n].name[m] = leaf[m];
        out[n].name[l] = 0;
        out[n].size = sysvol_table[i].size;
        out[n].is_dir = 0;
        n++;
    }
    return n;
}

int sysvol_resolve_bin(const char *name, char *out, uint32_t outsz) {
    if (name == 0 || out == 0 || outsz < 8) return -1;

    /* 已经带 /bin 或 /system 前缀：原样查，不再拼 */
    if (sysvol_is_path(name)) {
        /* /system 下不是可执行程序，只有 /bin 才认 */
        if (!sv_seg_is(name, "bin")) return -1;
        if (sysvol_lookup(name) < 0) return -1;
        uint32_t n = sv_len(name);
        if (n + 1 > outsz) return -1;
        for (uint32_t k = 0; k < n; k++) out[k] = name[k];
        out[n] = 0;
        return 0;
    }

    /* 裸名：拼成 "/bin/<name>"（大小写不敏感查找会兜住 HELLO.ELF） */
    static const char pre[] = "/bin/";
    uint32_t n = sv_len(name);
    if (n == 0 || 5 + n + 1 > outsz) return -1;
    for (uint32_t k = 0; k < 5; k++) out[k] = pre[k];
    for (uint32_t k = 0; k < n; k++) out[5 + k] = name[k];
    out[5 + n] = 0;
    if (sysvol_lookup(out) < 0) return -1;
    return 0;
}
