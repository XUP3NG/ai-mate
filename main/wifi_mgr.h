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
/* 主循环调用: 若断线重试已耗尽, 扫描并切换到其他已保存网络 (阻塞 ~2s) */
void wifi_mgr_poll_rescan(void);
/* 当前 SSID (未连接时为空串) */
const char *wifi_mgr_current_ssid(void);
/* 扫描附近热点; 返回数量, 结果写入 out (最多 max 条) */
int wifi_mgr_scan_ap(wifi_ap_record_t *out, int max);
/* 启动 AP 配网门户 (阻塞至配置保存后重启); 只能在主任务调用 */
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
