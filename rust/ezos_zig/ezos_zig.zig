//! ezos_zig.zig - Zig 侧的增量试验（与 Rust 版同算法，供内核对拍）。
//!
//! x86-freestanding：不链接 libc、不需要运行时，产出的 .o 直接给
//! i686-elf-ld 链进内核。这里只放纯计算函数——和 Rust 一样，
//! Zig 代码不持有任何内核资源，进出都是标量/裸指针。
//!
//! **坑**：别写 `(sum << 31) | (sum >> 1)`。Zig 的 `<<` 在 ReleaseSafe 下
//! 会检查"被移出的位"，左移 31 位必然丢位 → 安全检查触发，结果和 C/Rust
//! 对不上（对拍当场红）。循环移位的正确写法是 std.math.rotl。
const std = @import("std");

// exFAT 卷校验和（规范 §7.2）：每字节右移循环并累加。
export fn exfat_checksum_zig(data: [*]const u8, len: u32) u32 {
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
