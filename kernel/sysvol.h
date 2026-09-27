/*
 * sysvol.h - 内核内置只读系统卷（挂载点 /system 与 /bin）
 *
 * 为什么是"内置卷"而不是磁盘分区
 * --------------------------------
 * 用户程序原先放在数据盘根目录，一条 `format` 就全没了。Linux 的解法是
 * 系统与数据分属不同挂载点。EZOS 32 位、分区布局又依赖 BIOS/UEFI 两条
 * 启动路径，做分区系统卷的前提太多；这里把系统卷直接编进内核镜像的
 * .rodata，挂载成 /system 与 /bin 两个只读路径：
 *   - format 绝对擦不到（内容在内存里，不在任何盘上）
 *   - 不依赖分区表是否正确，legacy / UEFI / AHCI / NVMe 都在
 *   - 不碰 exFAT/FAT/ext4/... 七个后端的全局单例状态，回归风险近零
 *
 * 约定
 *   - 路径一律以 '/' 开头："/bin/hello.elf"、"/system/version"
 *   - 查找大小写不敏感（兼容老的 exec HELLO.ELF 写法）
 *   - 只支持读与列目录；写/删/建目录一律拒绝
 *
 * 数据源 kernel/sysvol_data.c 由 tools/make_sysvol.py 生成，是纯数据，
 * 不含指针，因此不产生任何重定位项。
 */
#ifndef SYSVOL_H
#define SYSVOL_H

#include "types.h"
#include "fs.h"          /* fs_dir_entry_t：列目录复用同一结构 */

/* 表项路径字段长度上限（与生成脚本里的 MAX_PATH 必须一致） */
#define SYSVOL_MAX_PATH 32

typedef struct {
    char     path[SYSVOL_MAX_PATH];   /* "/bin/hello.elf" */
    uint32_t off;                     /* 在 sysvol_blob 中的偏移 */
    uint32_t size;                    /* 字节数 */
} sysvol_entry_t;

extern const sysvol_entry_t sysvol_table[];
extern const uint32_t       sysvol_count;
extern const uint32_t       sysvol_bytes;
extern const uint8_t        sysvol_blob[];

/* 启动期打印一行诊断（dmesg 权威通道，前缀 SYSVOL:） */
void sysvol_init(void);

/* 该路径是否属于系统卷（首段是 bin 或 system，大小写不敏感）。
 * 返回 1 表示必须由 sysvol 处理，fs.c 据此路由，不再走磁盘后端。 */
int sysvol_is_path(const char *path);

/* 按路径找文件：命中返回表下标，否则 -1。
 * 不带扩展名时会再试一次 "<path>.elf"（让 exec hello 也能用）。 */
int sysvol_lookup(const char *path);

/* 文件大小；不存在返回 0（与 fs_get_file_size 语义一致） */
uint32_t sysvol_size(const char *path);

/* 读文件：成功返回读到的字节数（>0），失败 -1。
 * max_size 小于文件大小时截断到 max_size（磁盘后端也是这个语义）。 */
int sysvol_read(const char *path, uint8_t *buffer, uint32_t max_size);

/* 列目录。dir 为 "/bin"、"/system" 或 "/"（根：返回 bin、system 两个目录项）。
 * 成功返回条目数，失败 -1。 */
int sysvol_list(const char *dir, fs_dir_entry_t *out, int max_entries);

/* exec 用：把裸程序名（"hello" / "HELLO.ELF"）解析成系统卷里的绝对路径，
 * 写进 out 并返回 0；系统卷里没有则返回 -1（调用方回落到数据盘）。 */
int sysvol_resolve_bin(const char *name, char *out, uint32_t outsz);

#endif
