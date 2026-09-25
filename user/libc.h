/*
 * user/libc.h - 用户态极简 libc（用户态生态 ③）
 *
 * 这是教学内核的用户态"标准库"最小集，目标是把 5 个用户程序里重复手抄的
 * 系统调用封装和串/内存小工具收拢到一处，链接时用 `user/libc.o` 复用
 * （每个程序各自独立链接 user/user.ld，freestanding）。
 *
 * 设计边界：
 *   - 真正的陷入（int 0x80）由 crt0.asm 提供 write/read/open/close/lseek/
 *     _exit/sockcall 这些符号；libc 直接声明它们为 extern 并复用，不重复实现。
 *   - fork/waitpid/pipe/getpid 内核没有对应的 crt0 包装，由 libc 自己用内联
 *     汇编陷入（与 forktest 之前的写法一致）。
 *   - 不引入任何标准头（freestanding），只用编译器内建类型。
 */
#ifndef LIBC_H
#define LIBC_H

/* ---- crt0.asm 提供的基础系统调用封装（链接时由 crt0.o 解析） ---- */
int write(int fd, const void *buf, unsigned int n);
int read(int fd, void *buf, unsigned int n);
int open(const char *name, int flags);
int close(int fd);
int lseek(int fd, int off, int whence);
void _exit(int code) __attribute__((noreturn));
int sockcall(int subcmd, void *args);

/* ---- libc 提供的系统调用薄封装（int 0x80 直接陷入） ---- */
int  syscall(int num, int a, int b, int c);
void exit(int code) __attribute__((noreturn));
int  fork(void);                       /* 返回：父=子pid，子=0，失败=-1 */
int  waitpid(int pid, int *status, int options);
int  pipe(int fds[2]);                 /* fds[0]=读, fds[1]=写 */
int  getpid(void);

/* ---- 字符串 / 内存 ---- */
unsigned int strlen(const char *s);
int          strcmp(const char *a, const char *b);
int          strncmp(const char *a, const char *b, unsigned int n);
void        *memcpy(void *dst, const void *src, unsigned int n);
void        *memset(void *dst, int v, unsigned int n);
void         itoa(unsigned int v, char *out);   /* 十进制，NUL 结尾 */
void         puts(const char *s);               /* 原样写出，不加换行 */
void         putdec(int v);                     /* 十进制打印（支持负数） */

/* ---- 系统调用号（与内核 syscall.h 对齐；只封装内核已实现的） ---- */
#define SYS_READ    0u
#define SYS_WRITE   1u
#define SYS_EXIT    2u
#define SYS_OPEN    5u
#define SYS_CLOSE   6u
#define SYS_LSEEK   19u
#define SYS_PIPE    42u
#define SYS_FORK    43u
#define SYS_WAITPID 44u
#define SYS_GETPID  45u
#define WNOHANG     1        /* 非阻塞 waitpid：子进程仍在跑则返回 0（不收割） */

#endif
