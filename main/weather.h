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

/* ── 位置绑定 ──
 * 位置跟着 WiFi 网络走: 每个 SSID 永久记住一份坐标 (网络固定 = 位置固定),
 * 配网页也可手填经纬度把它钉死。不再每日重查 IP (那只会让城市偶尔跳变)。
 */

#define WX_LOC_AUTO    0    /* IP 自动定位 (免 Key) */
#define WX_LOC_GEOC    1    /* 城市名 → geocoding */
#define WX_LOC_MANUAL  2    /* 配网页手填经纬度 */

typedef struct {
    int32_t lat_x1e4;       /* 纬度 ×10000 */
    int32_t lon_x1e4;       /* 经度 ×10000 */
    char    city[24];       /* 城市名 (显示用) */
    uint8_t src;            /* WX_LOC_* */
    bool    from_global;    /* 来自"所有 WiFi 同一地点"的全局兜底值 */
} wx_loc_t;

/* 读某网络当前绑定的位置 (纯 NVS, 不联网); 未绑定返回 false */
bool wx_loc_peek(const char *ssid, wx_loc_t *out);

/* 配网页: 写入手填经纬度; all=true 时覆盖所有已保存网络 + 全局兜底 */
void wx_loc_set_manual(const char *ssid, const app_config_t *cfg,
                       int32_t lat_x1e4, int32_t lon_x1e4,
                       const char *city, bool all);

/* 清空经纬度保存 = 删除绑定 → 回落 IP 自动定位; all=true 清除所有网络 + 全局 */
void wx_loc_clear_manual(const char *ssid, const app_config_t *cfg, bool all);

/* 把 from 网络的坐标绑定复制给 to 网络 (标记为手填, 不会再被覆盖) */
bool wx_loc_copy(const char *from_ssid, const char *to_ssid);

/* WMO 天气码 → 中文描述 */
const char *wmo_text(uint8_t code);

/* WMO 天气码 → 字体图标 (QWeather Icons 码点, UTF-8; 静态缓冲, 单线程用) */
const char *wmo_icon(uint8_t code);

/* 空气质量图标 (QWeather Icons air-quality, UTF-8; 静态缓冲) */
const char *wx_aqi_icon(void);

/* 风力图标 (QWeather Icons wind, UTF-8; 静态缓冲) */
const char *wx_wind_icon(void);

/* 码点 → UTF-8 (QWeather Icons PUA 区; 供 UI 行内小图标用) */
void wx_icon_utf8(int cp, char *out, size_t sz);

/* 图标码点: 风 / 空气质量 (与 ui/font_qw_*.c 的 --range 对应) */
#define WX_ICON_WIND   0xF21A
#define WX_ICON_AIRQ   0xF2E6

#ifdef __cplusplus
}
#endif
