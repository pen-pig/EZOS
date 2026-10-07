/* kernel/version.h - 版本号唯一事实来源。
 *
 * 为什么单独拎一个头文件
 * ----------------------
 * 之前 "0.9.0" 这个字符串被抄在四处（kernel.c 启动横幅、shell 的 `ver`、
 * net.c 的内置 HTTP 页、tests/ 里的 4 处断言），而 git 上早就打了 v1.1.0
 * ——改一处漏三处，版本号就永远对不上。测试里抄的那份过期了还会变成
 * 一次莫名其妙的假红（跟 sysvol 写死字节数那次一模一样）。
 *
 * 所以：C 侧一律用 EZOS_VERSION；Python 侧（tests/、tools/make_sysvol.py）
 * 一律用 ezos_env.kernel_version() 从本文件解析。别再手抄字符串。
 */
#ifndef EZOS_VERSION_H
#define EZOS_VERSION_H

#define EZOS_VERSION "1.1.2"

#endif /* EZOS_VERSION_H */
