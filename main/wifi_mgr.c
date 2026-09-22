#include "wifi_mgr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "wifi";
static int s_retry_count = 0;
static bool s_got_ip = false;
static volatile bool s_need_portal = false;   /* 事件回调只置标志, 由主任务切门户 (回调内不可阻塞/调 esp_wifi_stop) */
static volatile bool s_radio_off = false;     /* 方案B: 查询间隔内射频关闭; 屏蔽由此产生的断连事件 */

/* 请求进入配网模式 (线程安全, 可从事件回调调用) */
void wifi_mgr_request_portal(void) { s_need_portal = true; }

/* 主任务轮询: 事件回调置位后由主任务真正切换 (阻塞式) */
bool wifi_mgr_poll_portal(void) {
    if (!s_need_portal) return false;
    s_need_portal = false;
    wifi_mgr_start_portal(NULL);
    return true;   /* 不会到达 (portal 内部常驻) */
}

/* ── 方案B: 查询间隔内关射频省电 ── */

void wifi_mgr_radio_sleep(void) {
    if (s_radio_off) return;
    s_radio_off = true;
    s_got_ip = false;
    s_retry_count = 0;
    esp_wifi_stop();          /* 触发的 DISCONNECT 事件被 s_radio_off 屏蔽 */
    ESP_LOGI(TAG, "radio off (power save)");
}

void wifi_mgr_radio_wake(void) {
    if (!s_radio_off) return;
    s_radio_off = false;
    s_retry_count = 0;
    ESP_ERROR_CHECK(esp_wifi_start());   /* STA_START 事件自动 esp_wifi_connect() */
    ESP_LOGI(TAG, "radio on, reconnecting...");
}

bool wifi_mgr_radio_on(void) { return !s_radio_off; }

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "sta started, connecting...");
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_radio_off) return;   /* 主动关射频产生的断连, 忽略 (不重试不进门户) */
        s_got_ip = false;
        uint8_t reason = data ? ((wifi_event_sta_disconnected_t *)data)->reason : 0;
        if (s_retry_count < 20) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGW(TAG, "retry connect (%d), reason=%d", s_retry_count, reason);
        } else {
            ESP_LOGE(TAG, "connect failed, requesting portal");
            wifi_mgr_request_portal();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        s_retry_count = 0;
        s_got_ip = true;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&evt->ip_info.ip));
    }
}

void wifi_mgr_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
}

bool wifi_is_connected(void) { return s_got_ip; }

void wifi_mgr_connect(const app_config_t *cfg) {
    static app_config_t s_cfg;
    s_cfg = *cfg;    /* keep a copy for event handler arg */

    /* 先挂事件 (只挂一次) */
    static bool s_events = false;
    if (!s_events) {
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, &s_cfg));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, &s_cfg));
        s_events = true;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, cfg->wifi_ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, cfg->wifi_pass, sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* 省电: MAX modem sleep — 信标间隔间射频休眠 (轮询场景延迟不敏感) */
    esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
    s_retry_count = 0;
    ESP_LOGI(TAG, "connecting to \"%s\" (via STA_START)...", cfg->wifi_ssid);
}

/* ── URL 解码 (表单提交) ── */
static void url_decode(char *s) {
    char *o = s;
    while (*s) {
        if (*s == '+') { *o++ = ' '; s++; }
        else if (*s == '%' && s[1] && s[2]) {
            char hex[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else *o++ = *s++;
    }
    *o = '\0';
}

/* 从 body 提取字段 */
static void form_field(const char *body, const char *name, char *out, size_t outsz) {
    char pat[32];
    snprintf(pat, sizeof(pat), "%s=", name);
    const char *p = strstr(body, pat);
    if (!p) { out[0] = '\0'; return; }
    p += strlen(pat);
    size_t i = 0;
    while (*p && *p != '&' && i + 1 < outsz) out[i++] = *p++;
    out[i] = '\0';
    url_decode(out);
}

/* ── 配网门户页面 ── */
static const char PORTAL_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>AI-Mate 配网</title><style>"
"body{font-family:sans-serif;max-width:420px;margin:16px auto;padding:0 12px;background:#111;color:#eee}"
"h2{border-bottom:2px solid #4af;padding-bottom:6px}"
"label{display:block;margin:10px 0 3px;font-size:14px;color:#9cf}"
"input{width:100%;box-sizing:border-box;padding:8px;font-size:15px;border:1px solid #456;border-radius:5px;background:#1b1b1b;color:#eee}"
"button{margin-top:16px;width:100%;padding:12px;font-size:16px;background:#257;border:0;border-radius:6px;color:#fff}"
"small{color:#888}"
"</style></head><body><h2>AI-Mate 配置</h2><form method='POST' action='/save'>"
"<label>WiFi 名称 (SSID)</label><input name='ssid' required>"
"<label>WiFi 密码</label><input name='pass' type='password'>"
"<h2 style='margin-top:22px'>智谱 GLM Coding Plan</h2>"
"<label>API Key</label><input name='gkey' required>"
"<label>Organization ID (org-xxx)</label><input name='gorg' required>"
"<label>Project ID (proj_xxx)</label><input name='gproj' required>"
"<label>套餐类型</label>"
"<input name='gtype' value='1' placeholder='1=个人版, 2=团队版' required>"
"<h2 style='margin-top:22px'>DeepSeek</h2>"
"<label>API Key</label><input name='dkey' required>"
"<label>轮询间隔 (分钟)</label><input name='pmin' value='5'>"
"<button type='submit'>保存并重启</button>"
"<p><small>智谱 org/project: 浏览器登录 bigmodel.cn/coding-plan → F12 → Network → "
"找 quota/limit 请求头 bigmodel-organization / bigmodel-project</small></p>"
"</form></body></html>";

static const char SAVED_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'></head>"
"<body style='font-family:sans-serif;text-align:center;padding-top:60px'>"
"<h2>已保存 ✓ 设备正在重启并连接 WiFi</h2>"
"<p>若 1 分钟后屏幕仍显示连接失败, 请长按 BOOT 键 3 秒重新配网</p>"
"</body></html>";

static esp_err_t portal_root(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

static const char ERR_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'></head>"
"<body style='font-family:sans-serif;text-align:center;padding-top:60px'>"
"<h2>配置不完整, 未保存</h2>"
"<p>WiFi 名称 / 智谱 API Key / DeepSeek API Key 为必填</p>"
"<p><a href='/'>返回重新填写</a></p>"
"</body></html>";

static esp_err_t portal_save(httpd_req_t *req) {
    char body[1024];
    size_t total = req->content_len < sizeof(body) - 1 ? req->content_len : sizeof(body) - 1;
    int received = httpd_req_recv(req, body, total);
    if (received <= 0) return ESP_FAIL;
    body[received] = '\0';

    app_config_t cfg;
    config_defaults(&cfg);
    form_field(body, "ssid", cfg.wifi_ssid, sizeof(cfg.wifi_ssid));
    form_field(body, "pass", cfg.wifi_pass, sizeof(cfg.wifi_pass));
    form_field(body, "gkey", cfg.glm_key, sizeof(cfg.glm_key));
    form_field(body, "gorg", cfg.glm_org, sizeof(cfg.glm_org));
    form_field(body, "gproj", cfg.glm_project, sizeof(cfg.glm_project));
    form_field(body, "dkey", cfg.dsk_key, sizeof(cfg.dsk_key));
    char tmp[8];
    form_field(body, "gtype", tmp, sizeof(tmp));
    if (tmp[0] == '2') cfg.glm_type = 2; else cfg.glm_type = 1;
    form_field(body, "pmin", tmp, sizeof(tmp));
    int pm = atoi(tmp);
    if (pm >= 1 && pm <= 60) cfg.poll_min = (uint8_t)pm;

    httpd_resp_set_type(req, "text/html; charset=utf-8");

    /* 先校验, 不完整返回错误页 (不保存不重启) */
    if (!cfg.wifi_ssid[0] || !cfg.glm_key[0] || !cfg.dsk_key[0]) {
        ESP_LOGW(TAG, "portal: incomplete config, rejected");
        return httpd_resp_send(req, ERR_HTML, HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_send(req, SAVED_HTML, HTTPD_RESP_USE_STRLEN);
    config_save(&cfg);
    ESP_LOGI(TAG, "portal: saved, restarting...");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;   /* 不会到达 */
}

static httpd_handle_t s_server = NULL;

void wifi_mgr_start_portal(app_config_t *cfg) {
    (void)cfg;
    ESP_LOGW(TAG, "=== AP 配网模式 ===");

    /* 停 STA, 起 SoftAP */
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    wifi_config_t ap = { 0 };
    strcpy((char *)ap.ap.ssid, "AI-Mate-Setup");
    ap.ap.ssid_len = strlen("AI-Mate-Setup");
    strcpy((char *)ap.ap.password, "aimate123");
    ap.ap.channel = 6;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    if (!s_server) {
        httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
        hc.max_uri_handlers = 4;
        if (httpd_start(&s_server, &hc) == ESP_OK) {
            httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = portal_root };
            httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = portal_save };
            httpd_register_uri_handler(s_server, &root);
            httpd_register_uri_handler(s_server, &save);
        }
    }

    /* 门户常驻, 等待保存后 esp_restart() */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
