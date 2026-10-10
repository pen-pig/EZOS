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

/* ===================================================================
 * 文件名编解码：UTF-16LE <-> UTF-8
 *
 * NTFS / exFAT / ReFS 的目录项里文件名是 UTF-16LE，内核其余地方
 * （sysvol 的源文件、shell 命令行、终端字模）是 UTF-8，所以每个后端
 * 都要在边界上转一次。
 *
 * 以前是每个后端各写各的，而且**三套降级策略互相不一样**：非 ASCII
 * 在 ntfs.c 里变成 '?'，在 exfat.c 里变成 '.'，在 refs.c 里干脆判定
 * "不匹配"。同一块盘上的同一个中文文件名，三个后端三个答案——写路径
 * 更糟，是把 UTF-8 的每个字节当 Latin-1 塞进 UTF-16（"中" 写成
 * U+00E4 U+00B8 两个字符而不是 U+4E2D），这种盘 Windows 一眼就认出来。
 *
 * 现在统一走这三份实现，契约见 rust/ezos_rs/src/lib.rs 的文件内注释：
 *   * 落单代理项 -> U+FFFD；非法 UTF-8 -> U+FFFD
 *   * **装不下就返回 -1，绝不返回半截名字**（半截名字比没有名字更坏：
 *     它会匹配到错误的文件）
 * =================================================================== */

/* C 参考实现（kernel/textenc.c）——rstest 对拍基线 + EZ_TEXT_IMPL=0 回退 */
int32_t utf16_to_utf8_c(const uint16_t *src, uint32_t units,
                        char *dst, uint32_t cap);
int32_t utf8_to_utf16_c(const char *src, uint16_t *dst, uint32_t cap);
uint32_t levenshtein_c(const char *a, const char *b, uint32_t limit);
uint32_t cksum_raw_c(const uint8_t *p, uint32_t n);
uint16_t cksum_c(const uint8_t *p, uint32_t n);

/* Rust */
int32_t utf16_to_utf8_rs(const uint16_t *src, uint32_t units,
                         uint8_t *dst, uint32_t cap);
int32_t utf8_to_utf16_rs(const uint8_t *src, uint16_t *dst, uint32_t cap);
uint32_t levenshtein_rs(const uint8_t *a, const uint8_t *b, uint32_t limit);
uint32_t cksum_raw_rs(const uint8_t *p, uint32_t n);
uint16_t cksum_rs(const uint8_t *p, uint32_t n);

/* Zig */
int32_t utf16_to_utf8_zig(const uint16_t *src, uint32_t units,
                          uint8_t *dst, uint32_t cap);
int32_t utf8_to_utf16_zig(const uint8_t *src, uint16_t *dst, uint32_t cap);
uint32_t levenshtein_zig(const uint8_t *a, const uint8_t *b, uint32_t limit);
uint32_t cksum_raw_zig(const uint8_t *p, uint32_t n);
uint16_t cksum_zig(const uint8_t *p, uint32_t n);

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

/* ---- 文本 / 网络算法：独立于 exFAT 的第二组开关 ---------------------
 *
 * 与 EZ_EXFAT_IMPL 分开是因为回退需求不同：exFAT 那组切错会写坏盘，
 * 必须三路对拍全绿才敢切；这一组切错最坏是文件名显示成 U+FFFD 或者
 * 建议词不中，不涉及落盘数据，可以独立灰度。
 *
 *   0 = C 参考实现（kernel/textenc.c / kernel/net.c，纯本地）
 *   1 = Rust（默认）
 *   2 = Zig
 */
#ifndef EZ_TEXT_IMPL
#define EZ_TEXT_IMPL  1
#endif

#if EZ_TEXT_IMPL == 2
#define utf16_to_utf8_active      utf16_to_utf8_zig
#define utf8_to_utf16_active      utf8_to_utf16_zig
#define levenshtein_active        levenshtein_zig
#define cksum_raw_active          cksum_raw_zig
#define cksum_active              cksum_zig
#elif EZ_TEXT_IMPL == 1
#define utf16_to_utf8_active      utf16_to_utf8_rs
#define utf8_to_utf16_active      utf8_to_utf16_rs
#define levenshtein_active        levenshtein_rs
#define cksum_raw_active          cksum_raw_rs
#define cksum_active              cksum_rs
#else
#define utf16_to_utf8_active      utf16_to_utf8_c
#define utf8_to_utf16_active      utf8_to_utf16_c
#define levenshtein_active        levenshtein_c
#define cksum_raw_active          cksum_raw_c
#define cksum_active              cksum_c
#endif

#endif
