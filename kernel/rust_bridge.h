/*
 * rust_bridge.h - 内核里非 C 语言模块（Rust / Zig）的唯一入口声明。
 *
 * 约定（改之前先读 rust/ezos_rs/src/lib.rs 与 rust/ezos_zig/ezos_zig.zig
 * 的文件头注释）：
 *   1. **只放纯计算、不持有内核资源的东西**。进出都是标量或调用方担保的裸
 *      指针，不传递所有权，也不从 Rust/Zig 侧调回内核（避免两套 panic/
 *      错误传播机制纠缠）。
 *   2. 命名带 `_rs` / `_zig` 后缀——同一个算法可能有三份实现，名字混了
 *      对拍就失去意义。
 *   3. Rust 侧是 `#![no_std]` + `panic = "abort"`，panic 就是死循环；
 *      Zig 侧是 x86-freestanding，同样没有运行时。所以**这些函数不允许
 *       panic**——输入合法性由 C 侧保证。
 *   4. `rstest` 命令逐项对拍三者，任何一处算法漂移都会在那里变红。
 */
#ifndef RUST_BRIDGE_H
#define RUST_BRIDGE_H

#include "types.h"

/* Rust（rust/ezos_rs，crate-type=staticlib，libezos_rs.a） */
uint32_t exfat_checksum_rs(const uint8_t *data, uint32_t len);
uint16_t exfat_name_hash_rs(const uint16_t *utf16, uint32_t chars);

/* Zig（rust/ezos_zig，zig build-obj -target x86-freestanding，ezos_zig.o） */
uint32_t exfat_checksum_zig(const uint8_t *data, uint32_t len);
uint16_t exfat_name_hash_zig(const uint16_t *utf16, uint32_t chars);

#endif
