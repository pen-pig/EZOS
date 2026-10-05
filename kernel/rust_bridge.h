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

/* C 参考实现（kernel/exfat.c）：无条件声明——它同时是 `rstest` 的对拍基线
 * 和 EZ_EXFAT_IMPL=0 时的回退目标，即使默认走 Rust 也必须能取到。 */
uint32_t exfat_checksum_c(const uint8_t *data, int len);
uint16_t exfat_name_hash_c(const uint16_t *utf16, int chars);
uint16_t exfat_set_checksum_c(const uint8_t *entries, int total_bytes);

/* Rust（rust/ezos_rs，crate-type=staticlib，libezos_rs.a） */
uint32_t exfat_checksum_rs(const uint8_t *data, uint32_t len);
uint16_t exfat_name_hash_rs(const uint16_t *utf16, uint32_t chars);
uint16_t exfat_set_checksum_rs(const uint8_t *data, uint32_t len);

/* Zig（rust/ezos_zig，zig build-obj -target x86-freestanding，ezos_zig.o） */
uint32_t exfat_checksum_zig(const uint8_t *data, uint32_t len);
uint16_t exfat_name_hash_zig(const uint16_t *utf16, uint32_t chars);
uint16_t exfat_set_checksum_zig(const uint8_t *data, uint32_t len);

/* ---- 生产实现选择 ----------------------------------------------------
 *
 * 同一个算法现在有三份实现，但**生产路径只走一份**——三份一起混着调用，
 * 出错时无法定位，也没法一键回退。改这个常量就能整体切换：
 *
 *   0 = C 参考实现（kernel/exfat.c，最慢但最"本地"，出问题时用它二分）
 *   1 = Rust（默认）
 *   2 = Zig
 *
 * 切换的前提是 `rstest` 三路对拍全绿；换了之后还要跑 tests/test_fsref.py
 * ——宿主机独立 exFAT 实现会重算盘上的 SetChecksum / NameHash，生产实现
 * 算错一个字节，那边立刻红（内核自己读自己的盘看不出来）。
 */
#ifndef EZ_EXFAT_IMPL
#define EZ_EXFAT_IMPL  1
#endif

#if EZ_EXFAT_IMPL == 2
#define exfat_checksum_active       exfat_checksum_zig
#define exfat_set_checksum_active   exfat_set_checksum_zig
#define exfat_name_hash_active      exfat_name_hash_zig
#elif EZ_EXFAT_IMPL == 1
#define exfat_checksum_active       exfat_checksum_rs
#define exfat_set_checksum_active   exfat_set_checksum_rs
#define exfat_name_hash_active      exfat_name_hash_rs
#else
#define exfat_checksum_active       exfat_checksum_c
#define exfat_set_checksum_active   exfat_set_checksum_c
#define exfat_name_hash_active      exfat_name_hash_c
#endif

/* 两套校验和别混用（rstest 会同时比它们）：
 *   exfat_checksum_active      = 32 位**卷**校验和（Boot Checksum Sector）
 *   exfat_set_checksum_active  = 16 位**目录项集**校验和，且跳过偏移 2-3
 * 宽度不同就是不同的算法，截断代替是错的。 */

#endif
