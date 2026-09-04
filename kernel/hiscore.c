/*
 * hiscore.c - 游戏高分持久化（SCORES.DAT，exFAT 根目录）
 *
 * 设计：
 *   - 内存表 scores[7] 懒加载：第一次 hiscore_get 前未加载则读文件；
 *     文件不存在/读取失败视为全 0（首次运行）。
 *   - 写回用 exfat_create_file 覆盖写（exFAT 无截断接口，整文件重写）。
 *   - exFAT 不可用（未挂载）时静默降级为内存高分（本次会话内有效）。
 */

#include "hiscore.h"
#include "fs.h"
#include "types.h"

static int scores[HISCORE_GAMES];
static int loaded = 0;

static void hiscore_load(void)
{
    loaded = 1;
    for (int i = 0; i < HISCORE_GAMES; i++) scores[i] = 0;
    uint8_t buf[HISCORE_GAMES * 4];
    uint32_t sz = fs_get_file_size("SCORES.DAT");
    if (sz == 0) return;                        /* 无纪录文件 */
    if (sz > sizeof(buf)) sz = sizeof(buf);
    if (fs_read_file("SCORES.DAT", buf, sz) != 0) return;
    for (int i = 0; i < HISCORE_GAMES; i++) {
        if ((uint32_t)(i * 4 + 4) > sz) break;  /* 短文件：剩余保持 0 */
        scores[i] = (int)((uint32_t)buf[i * 4]
                        | ((uint32_t)buf[i * 4 + 1] << 8)
                        | ((uint32_t)buf[i * 4 + 2] << 16)
                        | ((uint32_t)buf[i * 4 + 3] << 24));
        if (scores[i] < 0) scores[i] = 0;       /* 防损坏数据 */
    }
}

static void hiscore_save(void)
{
    uint8_t buf[HISCORE_GAMES * 4];
    for (int i = 0; i < HISCORE_GAMES; i++) {
        uint32_t v = (uint32_t)scores[i];
        buf[i * 4]     = (uint8_t)(v);
        buf[i * 4 + 1] = (uint8_t)(v >> 8);
        buf[i * 4 + 2] = (uint8_t)(v >> 16);
        buf[i * 4 + 3] = (uint8_t)(v >> 24);
    }
    fs_create_file("SCORES.DAT", buf, sizeof(buf));
}

int hiscore_get(int game)
{
    if (game < 1 || game > HISCORE_GAMES) return 0;
    if (!loaded) hiscore_load();
    return scores[game - 1];
}

int hiscore_is_better(int game, int value)
{
    if (game < 1 || game > HISCORE_GAMES || value < 0) return 0;
    int cur = hiscore_get(game);
    if (cur == 0) return 1;                     /* 无纪录：任何有效成绩都刷新 */
    /* 猜数字(1)轮数 / 扫雷(5)时间 / 记忆翻牌(7)步数：越小越好 */
    if (game == 1 || game == 5 || game == 7) return value < cur;
    return value > cur;                         /* 贪吃蛇/2048/RPS 分数：越大越好 */
}

int hiscore_update(int game, int value)
{
    if (!hiscore_is_better(game, value)) return 0;
    if (!loaded) hiscore_load();
    scores[game - 1] = value;
    hiscore_save();
    return 1;
}
