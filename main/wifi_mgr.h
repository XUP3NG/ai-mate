/**
 * wifi_mgr — WiFi STA 连接 + AP 配网门户
 *
 * 状态机:
 *   未配置/连接失败(20次)/长按BOOT → AP 配网门户 (SSID: AI-Mate-Setup, http://192.168.4.1)
 *   配置保存后自动重启 → STA 连接
 *
 * 注意: 事件回调里只能调 wifi_mgr_request_portal() 置标志,
 *       真正切门户 (阻塞) 必须由主任务调 wifi_mgr_poll_portal() 完成。
 */

#pragma once

#include "config_store.h"
#include "cc_mate.h"
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

void wifi_mgr_init(void);
bool wifi_is_connected(void);            /* STA 是否拿到 IP */
/* 连接: 优先试 last_ssid, 否则扫描附近热点, 选已保存网络中信号最好的接入 */
void wifi_mgr_connect_best(const app_config_t *cfg);
/* 主循环调用: 重试耗尽后扫描换网; 扫不到按 30s 退避重试, 连续 3 轮才进配网
 * —— 设备被移到新环境后, 最多一两分钟即可自动找回已保存的网络 */
void wifi_mgr_poll_rescan(void);
/* 配网门户是否运行中 (运行期间不能关射频, UI 固定显示配网页) */
bool wifi_mgr_portal_active(void);
/* 门户开启 >10 分钟且无人访问 → true, 主任务应重启以重试已保存网络 */
bool wifi_mgr_portal_timeout(void);
/* 当前 SSID (未连接时为空串) */
const char *wifi_mgr_current_ssid(void);
/* 扫描附近热点; 返回数量, 结果写入 out (最多 max 条) */
int wifi_mgr_scan_ap(wifi_ap_record_t *out, int max);
/* 启动 AP 配网门户 (非阻塞: 配置完成后立即返回, 门户由 httpd 自身任务维持) */
void wifi_mgr_start_portal(app_config_t *cfg);
/* 线程安全: 请求切入门户 (可在事件回调调用) */
void wifi_mgr_request_portal(void);
/* 主任务轮询: 若有门户请求则切换 (阻塞), 返回 false 表示无请求 */
bool wifi_mgr_poll_portal(void);
/* 方案B 省电: 查询间隔内关/开射频 (射频关闭期间时钟与 UI 正常, 仅断网) */
void wifi_mgr_radio_sleep(void);
void wifi_mgr_radio_wake(void);
bool wifi_mgr_radio_on(void);

#ifdef __cplusplus
}
#endif
