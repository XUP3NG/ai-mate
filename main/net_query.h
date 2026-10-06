/**
 * net_query — HTTPS 直连智谱/DeepSeek + SNTP 时间 + 消费历史推算
 */

#pragma once

#include "cc_mate.h"
#include "config_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化 SNTP (Asia/Shanghai), 非阻塞 */
void net_query_init_time(void);

/* HTTP 对时兜底: 从响应头 Date 解析 UTC 写入系统时钟 (防 UDP/123 被封的网络) */
bool net_time_http_sync(void);

/* 立即执行一轮查询 (阻塞数秒), 更新 state 中的 glm/dsk/hist */
void net_query_poll(app_state_t *state, const app_config_t *cfg);

/* 把 NVS 里的消费历史同步到 state (不依赖联网; 开机即可显示柱状图) */
void net_hist_sync(app_state_t *st);

/* WiFi 是否已连上 */
bool net_query_wifi_ok(void);

/* 通用 HTTPS GET (weather 等模块复用): 返回 body 长度, <0 失败 */
int net_https_get(const char *url, const char *hdr_auth, const char *hdr_org,
                  const char *hdr_proj, char *buf, size_t bufsz);

/* 同上, 额外支持一个自定义请求头 (如和风的 X-QW-Api-Key) */
int net_https_get_ex(const char *url, const char *hdr_auth, const char *hdr_org,
                     const char *hdr_proj, const char *hdr_xkey,
                     char *buf, size_t bufsz);

/* 响应体解码: 检测 gzip 魔数并解压 (部分服务端无视 Accept-Encoding: identity)
 * 返回写入 out 的长度 (不含结尾 \0), <0 失败 */
int net_http_body_decode(const char *in, int in_len, char *out, size_t outsz);

/* 通用 HTTPS POST JSON (LLM 对话等): body 为 JSON 字符串, bearer 为完整 Authorization 值
 * (如 "Bearer sk-xxx"), timeout_ms 单位毫秒 (LLM 生成可能要几十秒, 传大一点)。
 * 返回 body 长度, <0 失败; 非 200 返回 -2 */
int net_https_post_json(const char *url, const char *body, const char *bearer,
                        int timeout_ms, char *buf, size_t bufsz);

#ifdef __cplusplus
}
#endif
