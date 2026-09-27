/**
 * art — AI 每日像素画 (DeepSeek 生成)
 *
 * 每天 (UTC 日) 让 DeepSeek 生成一幅 40×30 的 1-bit 点阵画:
 *   prompt 里带日期/星期/天气 + 具体题材候选 + 密度约束 → 输出 '#'/'.' 字符画
 *   UI 以 8×8 块放大到 320×240 显示 (块状像素正是像素画的味道)
 *
 * 画布刻意取小: 文本模型对大画布的空间控制力很差, 80×60 出来全是抽象色块,
 * 40×30 才画得出可辨认的剪影。
 *
 * 成本: 每天一次, 输出约 1200 字符 ≈ 1K token, 几分钱。
 * 失败重试: 同一天最多 3 次 (计数按"日+prompt版本"持久化, 防重启后重复扣费)。
 * 存储: NVS ai_art — day/ver(已成功) tryd/tryv/try(当日尝试) title px(150B 位图)
 */

#pragma once

#include "cc_mate.h"
#include "config_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 开机加载: NVS 里昨天的画 + 状态; 画在时 UI 立刻可显示 */
void art_init(app_state_t *st);

/* net_task 每轮调用: 时间已同步且今天还没画 → 生成 (阻塞可达 1~2 分钟) */
void art_poll(app_state_t *st, const app_config_t *cfg);

/* 40×30 位图, 行主序 MSB-first, 行 5 字节共 150 字节 (UI 放大用) */
const uint8_t *art_bitmap(void);

/* 画布版本号: 每次有新画 +1, UI 据此判断是否需要重绘 */
int art_rev(void);

#ifdef __cplusplus
}
#endif
