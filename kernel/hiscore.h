#ifndef HISCORE_H
#define HISCORE_H

/*
 * hiscore.h - 游戏高分持久化
 *
 * 高分表存放在 exFAT 根目录 SCORES.DAT（每游戏 4 字节小端 int32，共 7 项）。
 * 首次访问时懒加载（文件不存在则全 0）；破纪录时整表覆写回磁盘。
 * "best" 语义按游戏不同：分数类取最大，步数/轮数类取最小（由调用方用
 * hiscore_is_better() 判断，游戏 1/5/7 为"越小越好"）。
 */

#define HISCORE_GAMES 7   /* 1=猜数字 2=井字棋 3=贪吃蛇 4=2048 5=扫雷 6=RPS 7=记忆翻牌 */

/* 读当前高分（未加载则先从磁盘加载；无记录返回 0） */
int hiscore_get(int game);

/* 判定候选成绩是否更好：game 1/5/7 越小越好，其余越大越好 */
int hiscore_is_better(int game, int value);

/* 若更好则更新并写盘，返回 1=已刷新纪录 */
int hiscore_update(int game, int value);

#endif
