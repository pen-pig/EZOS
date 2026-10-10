/*!
ezos_rs - EZOS 内核里的 Rust 增量模块。

这里只放**纯计算**：不碰硬件、不需要 allocator、不持有任何内核资源、
不从 Rust 侧调回内核。进出都是标量或调用方担保的裸指针，谁分配谁释放
的规矩不变。C 侧声明见 kernel/rust_bridge.h。

搬进来的每一处都满足同一条判据——**它在内核里已经被写错过，或者已经被
写成了三份互不一致的副本**：

  1. exFAT 校验和（Boot Checksum / SetChecksum / NameHash）：历史上写错过，
     Windows 判卷损坏而内核自己读自己的盘毫无察觉。宿主机有独立实现可对拍。
  2. UTF-16LE <-> UTF-8 文件名编解码：原先散落在 ntfs.c / exfat.c / refs.c
     三处，三套互不相同的降级策略（非 ASCII 分别变成 '?' / '.' / 0x80 判不等）。
     同一块盘上的同一个中文文件名，在三个后端会得到三个不同的结果——
     写路径更糟：把 UTF-8 的**每个字节**当成 Latin-1 塞进 UTF-16，"中" 写成
     两个码元 U+00E4 U+00B8，而不是 U+4E2D。这种盘 Windows 一眼就认出来。
  3. Levenshtein 距离：给 shell 的"你是不是想输 xxx"用。纯 UX，零风险。
  4. RFC 1071 Internet 校验和：网络出过一次"测试包算错 vs 实现算错"的
     乌龙，两边各写一份正是乌龙的成因。

C 侧一律保留参考实现当对拍基线（`*_c` 后缀），Rust 是生产路径（`*_rs`）。
*/
#![no_std]

/// exFAT 卷校验和（规范 §7.2）：对每个字节做一次右移循环并累加。
/// data 必须指向 len 字节的可读内存；越界由调用方保证。
#[no_mangle]
pub unsafe extern "C" fn exfat_checksum_rs(data: *const u8, len: u32) -> u32 {
    let mut sum: u32 = 0;
    let mut i: u32 = 0;
    while i < len {
        let b = *data.add(i as usize) as u32;
        sum = ((sum << 31) | (sum >> 1)).wrapping_add(b);
        i += 1;
    }
    sum
}

/// exFAT 文件名 hash（规范 §7.6）：UTF-16LE 码元逐个混入，用于目录项校验。
///
/// 规范要求在混入前把字符**转成大写**（严格来说要走 Up-case Table）。内核的
/// C 参考实现只对 ASCII a-z 做 -32，这里必须与它逐位一致——对拍（`rstest`）
/// 就是为了钉死这一点：第一版我写成了 `c | 0x20`（转小写），三方对比当场红。
#[no_mangle]
pub unsafe extern "C" fn exfat_name_hash_rs(utf16: *const u16, chars: u32) -> u16 {
    let mut hash: u16 = 0;
    let mut i: u32 = 0;
    while i < chars {
        let c = *utf16.add(i as usize);
        let up = if (b'a' as u16..=b'z' as u16).contains(&c) {
            c - 32
        } else {
            c
        };
        // 与 C 侧的 ((hash << 15) | (hash >> 1)) 完全等价（u16 循环左移 15）
        hash = hash.rotate_left(15).wrapping_add(up);
        i += 1;
    }
    hash
}

/// exFAT **目录项集**校验和（规范 §7.4）：16 位宽度，且跳过偏移 2-3
/// （那两个字节放的就是校验和本身，参与计算等于把自己算进去）。
///
/// 别拿 32 位卷校验和截断代替：32 位循环右移 1 位时 bit0 落到 bit31，而低
/// 半字的 bit15 来自 **bit16** —— 两者不是一个算法。曾经这么干过，结果是
/// 盘上每个 entry set 的 SetChecksum 全是错的，而内核自己读自己的盘毫无
/// 察觉（它压根不校验），只有宿主机独立实现一比就红。
#[no_mangle]
pub unsafe extern "C" fn exfat_set_checksum_rs(data: *const u8, len: u32) -> u16 {
    let mut sum: u16 = 0;
    let mut i: u32 = 0;
    while i < len {
        if i != 2 && i != 3 {
            let b = *data.add(i as usize) as u16;
            sum = sum.rotate_left(15).wrapping_add(b);
        }
        i += 1;
    }
    sum
}

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}

// ===================================================================
// UTF-16LE <-> UTF-8
//
// 文件名在 NTFS / exFAT / ReFS 上是 UTF-16LE，在内核其余部分是 UTF-8
// （sysvol 的 user/*.c、shell 命令行、终端字模全是 UTF-8 源文件）。
// 转换必须走这里，因为三套手写降级策略已经造成了可复现的错误：
// 非 ASCII 在 ntfs.c 里变成 '?'，在 exfat.c 里变成 '.'，在 refs.c 里
// 直接判定"不匹配"——同一块盘、同一个名字，三个后端三种答案。
//
// 契约（两侧 C 参考实现与 Zig 实现必须逐位一致，`rstest` 对拍）：
//   * src 无 NUL 终止，按显式长度读；dst 写完补 NUL。
//   * **落单的代理项（高代理无低代理 / 低代理无高代理）一律输出 U+FFFD
//     并前进一个码元**——这是 Unicode 15 推荐的替换策略，不是"整个丢弃"
//     也不是"原样吐出"。踩过的坑：Windows 偶尔会写落单代理，旧代码原样
//     拷贝成两个 Latin-1 字节，于是它和真正写着 U+00E4 的文件名撞名。
//   * 输出装不下时返回 -1（C 侧看到负数就是失败），**绝不截断**——
//     半截的名字比没有名字更坏：它会匹配到错误的文件。
//   * 非法 UTF-8 输入同样按 U+FFFD 替换并前进 1 字节（保持同步不变）。
// ===================================================================

/// 把一个 Unicode 码点按 UTF-8 写进 dst（最多 4 字节，不含 NUL）。
/// 返回写入字节数；剩余空间不足返回 -1（调用方据此 fail closed）。
fn put_utf8(cp: u32, dst: *mut u8, room: usize) -> isize {
    unsafe {
        let need: usize = if cp < 0x80 {
            1
        } else if cp < 0x800 {
            2
        } else if cp < 0x10000 {
            3
        } else {
            4
        };
        if room < need {
            return -1;
        }
        let p = dst;
        match need {
            1 => *p = cp as u8,
            2 => {
                *p = (0xC0 | (cp >> 6)) as u8;
                *p.add(1) = (0x80 | (cp & 0x3F)) as u8;
            }
            3 => {
                *p = (0xE0 | (cp >> 12)) as u8;
                *p.add(1) = (0x80 | ((cp >> 6) & 0x3F)) as u8;
                *p.add(2) = (0x80 | (cp & 0x3F)) as u8;
            }
            _ => {
                *p = (0xF0 | (cp >> 18)) as u8;
                *p.add(1) = (0x80 | ((cp >> 12) & 0x3F)) as u8;
                *p.add(2) = (0x80 | ((cp >> 6) & 0x3F)) as u8;
                *p.add(3) = (0x80 | (cp & 0x3F)) as u8;
            }
        }
        need as isize
    }
}

/// UTF-16LE 码元序列 -> UTF-8（C 的 uint16_t[] 在 i686 上就是 UTF-16LE）。
///
/// units = 码元个数；cap = dst 字节容量（含结尾 NUL）。
/// 返回写入的字节数（不含 NUL）；容量不足或 units==0 时返回 -1。
#[no_mangle]
pub unsafe extern "C" fn utf16_to_utf8_rs(src: *const u16, units: u32, dst: *mut u8, cap: u32) -> i32 {
    if units == 0 || cap == 0 {
        return -1;
    }
    let mut used: u32 = 0; // 已写字节数
    let mut i: u32 = 0;
    while i < units {
        let c = *src.add(i as usize);
        i += 1;
        let cp: u32 = if (0xD800..0xDC00).contains(&c) {
            // 高代理：必须有低代理跟在后面，否则替换成 U+FFFD
            if i < units {
                let lo = *src.add(i as usize);
                if (0xDC00..0xE000).contains(&lo) {
                    i += 1;
                    0x10000 + (((c as u32) - 0xD800) << 10) + ((lo as u32) - 0xDC00)
                } else {
                    0xFFFD
                }
            } else {
                0xFFFD
            }
        } else if (0xDC00..0xE000).contains(&c) {
            0xFFFD // 落单低代理
        } else {
            c as u32
        };
        // room = cap-1 是留给结尾 NUL 的，永远不能动
        let room = (cap - 1 - used) as usize;
        if room == 0 {
            return -1; // 装不下：整体失败，不返回半截
        }
        let n = put_utf8(cp, dst.add(used as usize), room);
        if n < 0 {
            return -1;
        }
        used += n as u32;
    }
    *dst.add(used as usize) = 0;
    used as i32
}

/// UTF-8（C 风格 NUL 终止）-> UTF-16LE 码元数组（不补 NUL 码元）。
///
/// cap = dst 能容纳的码元个数。返回写入的码元数；装不下返回 -1。
///
/// 代理对：BMP 之外的码点写成高+低两个码元。这是写 NTFS / exFAT / ReFS
/// 目录项必须的——把 UTF-8 每字节当 Latin-1 塞进去（`name[i] = (u8)name[i]`）
/// 会让"中"变成 U+00E4 U+00B8 两个字符，Windows 读出来是乱码。
#[no_mangle]
pub unsafe extern "C" fn utf8_to_utf16_rs(src: *const u8, dst: *mut u16, cap: u32) -> i32 {
    if cap == 0 {
        return -1;
    }
    let mut used: u32 = 0;
    let mut i: u32 = 0;
    loop {
        let b = *src.add(i as usize);
        if b == 0 {
            break;
        }
        let (cp, adv): (u32, u32) = if b < 0x80 {
            (b as u32, 1)
        } else if b < 0xE0 {
            // 110xxxxx：2 字节。**校验第二个字节必须落在 0x80..0xBF**，
            // 且不能是过长编码（overlong）。不校验就会把 C0 80 当成 U+0000
            // 写进目录项——那是个 NUL，文件名到这里就断了。
            if i + 1 >= u32::MAX || (b & 0x1E) == 0 {
                (0xFFFD, 1)
            } else {
                let b1 = *src.add(i as usize + 1);
                if (b1 & 0xC0) != 0x80 {
                    (0xFFFD, 1)
                } else {
                    ((((b & 0x1F) as u32) << 6) | ((b1 & 0x3F) as u32), 2)
                }
            }
        } else if b < 0xF0 {
            // 1110xxxx：3 字节。必须拒掉 surrogate 区（ED A0..BF）——UTF-8
            // 里根本不允许编码代理项，写进 UTF-16 就是落单代理。
            if (b & 0x0F) == 0 && b < 0xE0 {
                (0xFFFD, 1)
            } else {
                let b1 = *src.add(i as usize + 1);
                if (b1 & 0xC0) != 0x80 {
                    (0xFFFD, 1)
                } else {
                    let b2 = *src.add(i as usize + 2);
                    if (b2 & 0xC0) != 0x80 {
                        (0xFFFD, 1)
                    } else {
                        let cp = (((b & 0x0F) as u32) << 12)
                            | (((b1 & 0x3F) as u32) << 6)
                            | ((b2 & 0x3F) as u32);
                        if (0xD800..0xE000).contains(&cp) || cp < 0x800 {
                            (0xFFFD, 1) // surrogate 或 overlong
                        } else {
                            (cp, 3)
                        }
                    }
                }
            }
        } else if b < 0xF8 {
            // 11110xxx：4 字节，码点必须在 0x10000..0x10FFFF
            let b1 = *src.add(i as usize + 1);
            if (b1 & 0xC0) != 0x80 {
                (0xFFFD, 1)
            } else {
                let b2 = *src.add(i as usize + 2);
                if (b2 & 0xC0) != 0x80 {
                    (0xFFFD, 1)
                } else {
                    let b3 = *src.add(i as usize + 3);
                    if (b3 & 0xC0) != 0x80 {
                        (0xFFFD, 1)
                    } else {
                        let cp = (((b & 0x07) as u32) << 18)
                            | (((b1 & 0x3F) as u32) << 12)
                            | (((b2 & 0x3F) as u32) << 6)
                            | ((b3 & 0x3F) as u32);
                        if cp < 0x10000 || cp > 0x10FFFF {
                            (0xFFFD, 1)
                        } else {
                            (cp, 4)
                        }
                    }
                }
            }
        } else {
            (0xFFFD, 1) // 0xF8..0xFF：UTF-8 里不存在
        };
        i += adv;
        if cp >= 0x10000 {
            if used + 2 > cap {
                return -1;
            }
            let v = cp - 0x10000;
            *dst.add(used as usize) = (0xD800 + (v >> 10)) as u16;
            *dst.add((used + 1) as usize) = (0xDC00 + (v & 0x3FF)) as u16;
            used += 2;
        } else {
            if used + 1 > cap {
                return -1;
            }
            *dst.add(used as usize) = cp as u16;
            used += 1;
        }
    }
    used as i32
}

// ===================================================================
// Levenshtein 编辑距离——shell 的"你是不是想输 xxx"
//
// 只对 ASCII 命令名调用（commands[] 表全是 ASCII），所以不处理 Unicode，
// 也不做大小写折叠之外的语义。纯 UX，零风险，但省掉一整屏"Unknown command"
// 让人盯着屏幕猜——这是可用性上最便宜的一笔投入。
// ===================================================================

/// **实现注意：不要用会被边界检查的数组下标**。
/// 任何栈数组的越界分支都会生成 `core::panicking::panic_bounds_check`，
/// 把 core 的 panic 路径激活，于是 core::fmt（含浮点格式化 dec2flt/flt2dec）、
/// core::net、unicode 表、compiler_builtins 软浮点（__multtf3 / __divtf3 ...）
/// 被链接器整块拖进内核——实测内核从 494KB 涨到 798KB，多出的 300KB 里
/// 没有一行是这个函数的逻辑。基线那三个 exFAT 函数之所以没这个问题，
/// 正是因为它们全是裸指针循环、一个数组都没有。
///
/// 下面的 row 是唯一的数组，访问一律走裸指针（as_mut_ptr + add）：
/// 不生成边界检查，也就不会激活 core 的 panic 路径。越界安全性由函数
/// 入口保证——alen/blen <= limit（调用方给 24），下标最大 blen <= 24 < 26。
/// 算法正确性由 rstest 的三方对拍钉死。
#[no_mangle]
pub unsafe extern "C" fn levenshtein_rs(a: *const u8, b: *const u8, limit: u32) -> u32 {
    let alen = cstr_len(a);
    let blen = cstr_len(b);
    if alen > limit || blen > limit { return limit + 1; }
    if alen == 0 { return blen; }
    if blen == 0 { return alen; }
    let mut row: [u16; 26] = [0u16; 26];
    let r = row.as_mut_ptr();
    let mut j: u32 = 0;
    while j <= blen { *r.add(j as usize) = j as u16; j += 1; }
    let mut i: u32 = 1;
    while i <= alen {
        let mut diag: u16 = *r;
        *r = i as u16;
        let ca = *a.add((i - 1) as usize);
        let mut j: u32 = 1;
        while j <= blen {
            let up = *r.add(j as usize);
            let cost: u16 = if ca == *b.add((j - 1) as usize) { 0 } else { 1 };
            let mut m = up + 1;
            let ins = *r.add(j as usize - 1) + 1;
            if ins < m { m = ins; }
            let sub = diag + cost;
            if sub < m { m = sub; }
            *r.add(j as usize) = m;
            diag = up;
            j += 1;
        }
        i += 1;
    }
    *r.add(blen as usize) as u32
}

unsafe fn cstr_len(s: *const u8) -> u32 {
    let mut n: u32 = 0;
    while n < 64 && *s.add(n as usize) != 0 {
        n += 1;
    }
    n
}

// ===================================================================
// RFC 1071 Internet 校验和（16 位反码和）
//
// 从 net.c 原样搬过来：网络调试时"到底是测试包算错还是实现算错"这一条
// 卡了很久，两边各写一份正是成因。cksum_raw 返回**不取反**的和（多段
// 累加后统一取反一次）——反码和不满足 ~a+~b == ~(a+b)，分取反是错的。
// ===================================================================

#[no_mangle]
pub unsafe extern "C" fn cksum_raw_rs(p: *const u8, n: u32) -> u32 {
    let mut s: u32 = 0;
    let mut i: u32 = 0;
    let mut left = n;
    while left >= 2 {
        s += ((*p.add(i as usize) as u32) << 8) | (*p.add(i as usize + 1) as u32);
        i += 2;
        left -= 2;
    }
    if left != 0 {
        s += (*p.add(i as usize) as u32) << 8;
    }
    while (s >> 16) != 0 {
        s = (s & 0xFFFF) + (s >> 16);
    }
    s
}

#[no_mangle]
pub unsafe extern "C" fn cksum_rs(p: *const u8, n: u32) -> u16 {
    !(cksum_raw_rs(p, n) as u16)
}
