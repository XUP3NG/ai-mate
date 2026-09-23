/**
 * weather — Open-Meteo 天气查询 (免 Key)
 *
 * 城市名 → geocoding (一次, 坐标缓存 NVS) → forecast (每轮)
 * 挂在 net_task 在线窗口内, 与 GLM/DSK 查询共用 duty-cycle。
 */

#pragma once

#include "cc_mate.h"
#include "config_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 查询一轮天气 (阻塞数秒), 更新 state->wx; 城市为空时置 err 直接返回 */
void weather_query(app_state_t *st, const app_config_t *cfg);

/* 清除坐标缓存 (城市变更时由配网页调用) */
void weather_coords_clear(void);

/* WMO 天气码 → 中文描述 */
const char *wmo_text(uint8_t code);

/* WMO 天气码 → 字体自带图标符号 (font_cjk_16 含 0x2600-0x27BF) */
const char *wmo_icon(uint8_t code);

#ifdef __cplusplus
}
#endif
