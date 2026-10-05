/*!
ezos_rs - EZOS 内核里的 Rust 增量试验田。

选 exFAT 的两个纯计算函数开刀是有原因的：
  * 纯逻辑、不碰硬件、不需要 allocator —— no_std 就能写，风险最小；
  * 这两处正是历史上出过事故的地方（Boot Checksum 写得不合规范 → Windows
    判卷损坏），宿主机有独立实现可对拍；
  * C 侧只有一个函数调用边界，不需要引入 Rust 的 ABI 复杂性。

C 侧声明见 kernel/rust_bridge.h。约定：**Rust 不持有任何内核资源**，
进出都是普通标量/指针，谁分配谁释放的规矩不变。
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

#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}
