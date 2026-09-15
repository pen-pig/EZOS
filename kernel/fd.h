/*
 * fd.h - 文件描述符（步骤 6b）
 *
 * 每个任务（task_t）持有一张 fd 表，条目分两类：
 *   - 标准流（0/1/2 = stdin/stdout/stderr），不进文件系统
 *   - 文件句柄（open 得到）：整文件读入内核堆内存，带当前偏移 offset；
 *     write 在内存里改，close 时若被写过（dirty）则整文件落盘
 *
 * 为什么不直接读盘而是整文件进内存：现阶段 fs 层只有"整文件读"和
 * "整文件写"两个原语（fs_read_file / fs_create_file），没有"按偏移读扇区"
 * 的流式句柄。教学定位下，文件都很小，内存映射式句柄足够演示
 * open/read/write/lseek/close 的完整语义，代价是单文件 ≤ MAX_FD_FILE。
 *
 * fd 表挂在 task_t 上（见 task.h），为步骤 6c 的多用户进程做准备——
 * 届时每个进程有自己的 fd 表、自己的 cwd。当前只有 shell（task 0）一个
 * 用户上下文，exec 的用户程序继承它的 fd 表。
 */
#ifndef FD_H
#define FD_H

#include "types.h"

#define MAX_OPEN_FDS   16u             /* 每任务最多同时打开的文件 */
#define MAX_FD_FILE    65536u          /* 单文件上限 64KB（内存句柄） */
#define FD_MAX_NAME    64u             /* 打开时记录的文件名上限 */

/* open 的 flags（教学简化：读/写/读写） */
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2

/* lseek 的 whence */
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

/* 标准流 fd（每个任务默认开好） */
#define FD_STDIN   0u
#define FD_STDOUT  1u
#define FD_STDERR  2u

/* 条目类型（fd_entry_t.type 的取值）。
 * 注意与 fd 号宏区分：fd 号 0/1/2 是标准流的索引，而这里的 FD_TYPE_STDIN
 * 等是"这个条目是什么"的类型标记——fd_table_init 会把 0 号条目的 type 设成
 * FD_TYPE_STDIN，两者在语义上分离，避免同名枚举值/宏互相替换。 */
typedef enum {
    FD_TYPE_FREE = 0,
    FD_TYPE_STDIN,
    FD_TYPE_STDOUT,
    FD_TYPE_STDERR,
    FD_TYPE_FILE
} fd_type_t;

typedef struct {
    uint8_t  type;              /* FD_* */
    uint8_t  writable;          /* 打开时带写权限（O_WRONLY/O_RDWR） */
    uint8_t  dirty;             /* 被 write 过，close 时需落盘 */
    uint8_t  _pad;
    uint32_t size;              /* 文件字节数 */
    uint32_t offset;            /* 当前读写偏移 */
    uint8_t *data;              /* 文件内容缓冲（FD_FILE；空文件可为 NULL） */
    char     name[FD_MAX_NAME]; /* 文件名，close 落盘用 */
} fd_entry_t;

typedef struct {
    fd_entry_t fds[MAX_OPEN_FDS];
} fd_table_t;

/* 初始化一张空 fd 表：开好 0/1/2 标准流 */
void fd_table_init(fd_table_t *t);

/*
 * 系统调用实现。参数指针已由 syscall.c 做过 user_range_ok 校验，
 * 这里不再重复校验用户地址，但会做 fd 有效性 / 权限 / 越界等内核侧检查。
 * 返回值语义与 POSIX 对齐：成功返回字节数 / 新 fd / 新偏移，失败 -1。
 */
int fd_open(fd_table_t *t, const char *name, int flags);
int fd_close(fd_table_t *t, int fd);
int fd_read(fd_table_t *t, int fd, uint8_t *buf, uint32_t n);
int fd_write(fd_table_t *t, int fd, const uint8_t *buf, uint32_t n);
int fd_lseek(fd_table_t *t, int fd, int32_t offset, int whence);

/* 自检：open/read/lseek/write/close 走真实文件系统跑一遍 */
int fd_selftest(void (*out)(const char *));

#endif
