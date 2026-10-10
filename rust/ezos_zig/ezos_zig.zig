//! ezos_zig.zig - Zig 侧的增量模块（与 Rust 版同算法，供内核对拍）。
//!
//! x86-freestanding：不链接 libc、不需要运行时，产出的 .o 直接给
//! i686-elf-ld 链进内核。这里只放纯计算函数——和 Rust 一样，
//! Zig 代码不持有任何内核资源，进出都是标量/裸指针。
//!
//! 存在的意义不是"多一种语言"，而是**当第三支独立实现**：算法写歪、
//! 边界处理不一致（比如把落单代理项原样透传而不是替换成 U+FFFD），
//! 两份同源拷贝可能一起错，三份里两份一致才算稳。C 侧 `rstest` 逐项比对。
//!
//! **坑**：别写 `(sum << 31) | (sum >> 1)`。Zig 的 `<<` 在 ReleaseSafe 下
//! 会检查"被移出的位"，左移 31 位必然丢位 → 安全检查触发，结果和 C/Rust
//! 对不上（对拍当场红）。循环移位的正确写法是 std.math.rotl。
const std = @import("std");

// ---------- 栈探测：为什么这里要显式关掉 ----------
//
// Zig 的 ReleaseSafe 默认给每个函数插 `call __zig_probe_stack`，栈溢出时走
// `debug.FullPanic -> panicExtra -> Io.Writer.printValue` 这条链。而
// x86-freestanding 不提供 memset/memcpy，也没有 Zig 的 start.zig，于是
// Io.Writer + debug.panic 整条运行时会被拖进内核。
//
// 实测代价：**+300KB**（内核 494KB -> 803KB），其中没有一行是这个模块的
// 逻辑。也就是说 ReleaseSafe 的栈探测在"没有 Zig 运行时"的裸机环境里
// 是个净负担——它保护的东西本身就跑不起来。
//
// 所以每个导出函数体第一行写 `@setRuntimeSafety(false)`。注意这**不是**
// 全局 -fno-stack-safety：这里是把函数级安全检查整体关掉，代价是这个模块
// 不再有 Zig 的运行时保护，所以**契约必须由 C 侧保证**（见 kernel/rust_bridge.h
// 文件头：不许 panic，越界由调用方负责）。
//
// 真正保护内核栈的是既有那套机制：IRQ0 帧 + panic 屏幕 +
// tools/check_stack.py 守着的 12KB 单帧上限。这些函数都是叶子函数、
// 局部数组最大 100 字节。

// exFAT 卷校验和（规范 §7.2）：每字节右移循环并累加。
export fn exfat_checksum_zig(data: [*]const u8, len: u32) u32 {
    @setRuntimeSafety(false); // 见文件头：只为避开栈探测带来的 300KB 运行时
    var sum: u32 = 0;
    var i: u32 = 0;
    while (i < len) : (i += 1) {
        const b: u32 = data[i];
        sum = std.math.rotl(u32, sum, 31) +% b; // == ((sum << 31) | (sum >> 1))
    }
    return sum;
}

// exFAT **目录项集**校验和（规范 §7.4）：16 位宽度，跳过偏移 2-3（那两字节
// 就是校验和自身）。
//
// 与上面的 32 位卷校验和**不是同一件事**：32 位右移循环时 bit0 落到 bit31，
// 而 16 位版的 bit15 来自 bit15 之后的位——截断 32 位结果当 16 位用是错的
// （宿主机独立实现对拍抓到过：盘上 SetChecksum 全错，内核自己读不出来）。
export fn exfat_set_checksum_zig(data: [*]const u8, len: u32) u16 {
    @setRuntimeSafety(false); // 见文件头：只为避开栈探测带来的 300KB 运行时
    var sum: u16 = 0;
    var i: u32 = 0;
    while (i < len) : (i += 1) {
        if (i != 2 and i != 3) {
            const b: u16 = data[i];
            sum = std.math.rotl(u16, sum, 15) +% b;
        }
    }
    return sum;
}

/// exFAT 文件名 hash（规范 §7.6）：UTF-16LE 码元逐个混入。
///
/// 混入前要转成**大写**（严格来说走 Up-case Table，内核简化为 ASCII a-z 减 32）。
/// 这里必须与内核 C 实现逐位一致——第一版写成了 `c | 0x20`（转小写），
/// `rstest` 三方对拍当场就红了。
export fn exfat_name_hash_zig(utf16: [*]const u16, chars: u32) u16 {
    @setRuntimeSafety(false); // 见文件头：只为避开栈探测带来的 300KB 运行时
    var hash: u16 = 0;
    var i: u32 = 0;
    while (i < chars) : (i += 1) {
        const c = utf16[i];
        const up: u16 = if (c >= 'a' and c <= 'z') c - 32 else c;
        // 同样是循环移位：rotl(u16, h, 15) == ((h << 15) | (h >> 1))
        hash = std.math.rotl(u16, hash, 15) +% up;
    }
    return hash;
}

// ===================================================================
// UTF-16LE <-> UTF-8（与 Rust 同契约，见 rust/ezos_rs/src/lib.rs）
//
// 落单代理项 -> U+FFFD；输出装不下 -> 返回负数（不截断半截名字）。
// 这两条是三份实现必须逐位一致的地方，`rstest` 会钉死。
// ===================================================================

fn putUtf8(cp: u32, dst: [*]u8, room: usize) isize {
    const need: usize = if (cp < 0x80) 1 else if (cp < 0x800) 2 else if (cp < 0x10000) 3 else 4;
    if (room < need) return -1;
    switch (need) {
        1 => dst[0] = @intCast(cp & 0x7F),
        2 => {
            dst[0] = @intCast(0xC0 | (cp >> 6));
            dst[1] = @intCast(0x80 | (cp & 0x3F));
        },
        3 => {
            dst[0] = @intCast(0xE0 | (cp >> 12));
            dst[1] = @intCast(0x80 | ((cp >> 6) & 0x3F));
            dst[2] = @intCast(0x80 | (cp & 0x3F));
        },
        else => {
            dst[0] = @intCast(0xF0 | (cp >> 18));
            dst[1] = @intCast(0x80 | ((cp >> 12) & 0x3F));
            dst[2] = @intCast(0x80 | ((cp >> 6) & 0x3F));
            dst[3] = @intCast(0x80 | (cp & 0x3F));
        },
    }
    return @intCast(need);
}

export fn utf16_to_utf8_zig(src: [*]const u16, units: u32, dst: [*]u8, cap: u32) i32 {
    @setRuntimeSafety(false); // 见文件头：只为避开栈探测带来的 300KB 运行时
    if (units == 0 or cap == 0) return -1;
    var used: u32 = 0;
    var i: u32 = 0;
    while (i < units) {
        const c: u32 = src[i];
        i += 1;
        var cp: u32 = undefined;
        if (c >= 0xD800 and c < 0xDC00) {
            if (i < units) {
                const lo: u32 = src[i];
                if (lo >= 0xDC00 and lo < 0xE000) {
                    i += 1;
                    cp = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                } else cp = 0xFFFD;
            } else cp = 0xFFFD;
        } else if (c >= 0xDC00 and c < 0xE000) {
            cp = 0xFFFD;
        } else cp = c;
        const room: usize = cap - 1 - used;
        if (room == 0) return -1;
        const n = putUtf8(cp, dst + used, room);
        if (n < 0) return -1;
        used += @intCast(n);
    }
    dst[used] = 0;
    return @intCast(used);
}

export fn utf8_to_utf16_zig(src: [*]const u8, dst: [*]u16, cap: u32) i32 {
    @setRuntimeSafety(false); // 见文件头：只为避开栈探测带来的 300KB 运行时
    if (cap == 0) return -1;
    var used: u32 = 0;
    var i: u32 = 0;
    while (true) {
        const b: u32 = src[i];
        if (b == 0) break;
        var cp: u32 = 0;
        var adv: u32 = 1;
        if (b < 0x80) {
            cp = b;
        } else if (b < 0xE0) {
            if ((b & 0x1E) == 0) {
                cp = 0xFFFD;
            } else {
                const b1: u32 = src[i + 1];
                if ((b1 & 0xC0) != 0x80) {
                    cp = 0xFFFD;
                } else {
                    cp = ((b & 0x1F) << 6) | (b1 & 0x3F);
                    adv = 2;
                }
            }
        } else if (b < 0xF0) {
            const b1: u32 = src[i + 1];
            if ((b & 0x0F) == 0 and b < 0xE0) {
                cp = 0xFFFD;
            } else if ((b1 & 0xC0) != 0x80) {
                cp = 0xFFFD;
            } else {
                const b2: u32 = src[i + 2];
                if ((b2 & 0xC0) != 0x80) {
                    cp = 0xFFFD;
                } else {
                    const c0: u32 = ((b & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
                    if ((c0 >= 0xD800 and c0 < 0xE000) or c0 < 0x800) {
                        cp = 0xFFFD;
                    } else {
                        cp = c0;
                        adv = 3;
                    }
                }
            }
        } else if (b < 0xF8) {
            const b1: u32 = src[i + 1];
            if ((b1 & 0xC0) != 0x80) {
                cp = 0xFFFD;
            } else {
                const b2: u32 = src[i + 2];
                if ((b2 & 0xC0) != 0x80) {
                    cp = 0xFFFD;
                } else {
                    const b3: u32 = src[i + 3];
                    if ((b3 & 0xC0) != 0x80) {
                        cp = 0xFFFD;
                    } else {
                        const c0: u32 = ((b & 0x07) << 18) | ((b1 & 0x3F) << 12) |
                            ((b2 & 0x3F) << 6) | (b3 & 0x3F);
                        if (c0 < 0x10000 or c0 > 0x10FFFF) {
                            cp = 0xFFFD;
                        } else {
                            cp = c0;
                            adv = 4;
                        }
                    }
                }
            }
        } else cp = 0xFFFD;
        i += adv;
        if (cp >= 0x10000) {
            if (used + 2 > cap) return -1;
            const v = cp - 0x10000;
            dst[used] = @intCast(0xD800 + (v >> 10));
            dst[used + 1] = @intCast(0xDC00 + (v & 0x3FF));
            used += 2;
        } else {
            if (used + 1 > cap) return -1;
            dst[used] = @intCast(cp);
            used += 1;
        }
    }
    return @intCast(used);
}

// Levenshtein 编辑距离（shell "你是不是想输 xxx"）
//
// **实现注意：不要用两行数组再整体交换**（`const tmp = prev; prev = cur;
// cur = tmp;`）。那会让优化器有机会向量化，进而把整块运行时拖进内核——
// Rust 侧实测踩过：内核从 494KB 涨到 803KB，多出的 300KB 没有一行是
// 这个函数用的。改成"一块缓冲 + 奇偶下标翻转"就没有任何数组拷贝。
export fn levenshtein_zig(a: [*]const u8, b: [*]const u8, limit: u32) u32 {
    @setRuntimeSafety(false); // 见文件头：只为避开栈探测带来的 300KB 运行时
    var alen: u32 = 0;
    while (alen < 64 and a[alen] != 0) alen += 1;
    var blen: u32 = 0;
    while (blen < 64 and b[blen] != 0) blen += 1;
    if (alen > limit or blen > limit) return limit + 1;
    if (alen == 0) return blen;
    if (blen == 0) return alen;
    // 显式零初始化，**不要写 `= undefined`**：ReleaseSafe 下 undefined 数组
    // 会走 std.mem.set 路径，生成对 memset 的外部引用。而 Zig 的 freestanding
    // 目标不提供 memset，链接期报 undefined reference——被迫在内核里补一个
    // 全局 memset 才能链过，等于让整个 Zig 运行时（Io.Writer、浮点格式化）
    // 都被链接器拖进内核。实测这一处就值 300KB。
    var buf: [50]u16 = [_]u16{0} ** 50;
    var j: u32 = 0;
    while (j <= blen) : (j += 1) buf[j] = @intCast(j);
    var i: u32 = 1;
    while (i <= alen) : (i += 1) {
        const cur: usize = @as(usize, i % 2) * 25;
        const prev: usize = cur ^ 25;
        buf[cur] = @intCast(i);
        const ca = a[i - 1];
        j = 1;
        while (j <= blen) : (j += 1) {
            const cb = b[j - 1];
            const cost: u16 = if (ca == cb) 0 else 1;
            const del = buf[prev + @as(usize, j)] + 1;
            const ins = buf[cur + @as(usize, j) - 1] + 1;
            const sub = buf[prev + @as(usize, j) - 1] + cost;
            var m = del;
            if (ins < m) m = ins;
            if (sub < m) m = sub;
            buf[cur + @as(usize, j)] = m;
        }
    }
    return buf[@as(usize, alen % 2) * 25 + @as(usize, blen)];
}

// ===================================================================
// RFC 1071 Internet 校验和
// ===================================================================

export fn cksum_raw_zig(p: [*]const u8, n: u32) u32 {
    @setRuntimeSafety(false); // 见文件头：只为避开栈探测带来的 300KB 运行时
    var s: u32 = 0;
    var i: u32 = 0;
    var left = n;
    while (left >= 2) {
        s += (@as(u32, p[i]) << 8) | @as(u32, p[i + 1]);
        i += 2;
        left -= 2;
    }
    if (left != 0) s += @as(u32, p[i]) << 8;
    while ((s >> 16) != 0) s = (s & 0xFFFF) + (s >> 16);
    return s;
}

export fn cksum_zig(p: [*]const u8, n: u32) u16 {
    @setRuntimeSafety(false); // 见文件头：只为避开栈探测带来的 300KB 运行时
    // cksum_raw 的结果已被折进低 16 位（末尾的 while (s>>16) != 0 保证
    // s <= 0xFFFF），显式截断让 Zig 的类型系统满意，语义与 C/Rust 一致。
    return @truncate(~cksum_raw_zig(p, n));
}
