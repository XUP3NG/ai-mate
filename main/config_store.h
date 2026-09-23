/**
 * config_store — NVS 配置存储
 *
 * 存储: WiFi SSID/密码, 智谱 API Key/org/project/type, DeepSeek API Key
 * 命名空间: "ai_mate"
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* WiFi */
    char wifi_ssid[33];
    char wifi_pass[65];
    /* 智谱 GLM Coding Plan */
    char glm_key[96];       /* API Key (raw, 无 Bearer) */
    char glm_org[40];       /* bigmodel-organization */
    char glm_project[48];   /* bigmodel-project */
    uint8_t glm_type;       /* 1=个人 2=团队 */
    /* DeepSeek */
    char dsk_key[96];       /* API Key */
    /* 天气 (Open-Meteo 免 Key, 城市名留空=不启用) */
    char wx_city[24];
    /* 轮询间隔 (分钟, 1–60) */
    uint8_t poll_min;
} app_config_t;

void config_defaults(app_config_t *cfg);
bool config_load(app_config_t *cfg);            /* false = 未配置 */
void config_save(const app_config_t *cfg);
void config_clear(void);                        /* 清空 WiFi (重新配网) */

#ifdef __cplusplus
}
#endif
