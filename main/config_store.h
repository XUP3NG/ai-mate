/**
 * config_store — NVS 配置存储
 *
 * 存储: WiFi 网络列表(最多 CFG_NET_MAX 组) + 上次成功 SSID,
 *       智谱 API Key/org/project/type, DeepSeek API Key, 天气城市
 * 命名空间: "ai_mate"
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CFG_NET_MAX 4       /* 最多记住 4 个 WiFi 网络 */

typedef struct {
    /* ── WiFi 网络列表 (自动重连时择优) ── */
    char     net_ssid[CFG_NET_MAX][33];
    char     net_pass[CFG_NET_MAX][65];
    uint8_t  net_count;
    char     last_ssid[33];     /* 上次成功连接的 SSID, 唤醒时优先尝试 */

    /* ── 配网页当前填写的网络 (保存时合并进列表) ── */
    char     wifi_ssid[33];
    char     wifi_pass[65];

    /* ── 智谱 GLM Coding Plan ── */
    char     glm_key[96];       /* API Key (raw, 无 Bearer) */
    char     glm_org[40];       /* bigmodel-organization */
    char     glm_project[48];   /* bigmodel-project */
    uint8_t  glm_type;          /* 1=个人 2=团队 */

    /* ── DeepSeek ── */
    char     dsk_key[96];       /* API Key */

    /* ── 天气 (Open-Meteo 免 Key, 城市名留空=IP 自动定位) ── */
    char     wx_city[24];

    /* ── 轮询间隔 (分钟, 1–60) ── */
    uint8_t  poll_min;
} app_config_t;

void config_defaults(app_config_t *cfg);
bool config_load(app_config_t *cfg);            /* false = 未配置 (无 WiFi 列表) */
void config_save(const app_config_t *cfg);
void config_clear(void);                        /* 清空 WiFi (重新配网) */

/* ── WiFi 列表操作 ── */

/* 把 ssid/pass 加入列表(已存在则更新密码), 并设为 last_ssid; 返回索引, 满则返回 -1 */
int  config_net_add(app_config_t *cfg, const char *ssid, const char *pass);
/* 查密码; 未找到返回 NULL */
const char *config_net_find(const app_config_t *cfg, const char *ssid);
/* 清空列表 */
void config_net_clear(app_config_t *cfg);

#ifdef __cplusplus
}
#endif
