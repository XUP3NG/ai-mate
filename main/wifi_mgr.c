/**
 * wifi_mgr — WiFi STA 多网络择优连接 + AP 配网门户 + 射频省电
 *
 * 连接策略:
 *   开机/换网 → 扫描附近热点, 从已保存网络中选信号最好的接入
 *   唤醒重连 → 直接用上次成功的 SSID 重连 (省去扫描 ~2s)
 *   连不上   → 重试 3 次后扫描换用其他已保存网络; 再无则进配网门户
 *
 * 注意: 事件回调里只能置标志 (不可阻塞/调 esp_wifi_stop),
 *       扫描换网由主任务调用 wifi_mgr_poll_rescan() 完成。
 */

#include "wifi_mgr.h"
#include "weather.h"
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
#include <stdio.h>
#include <stdarg.h>

static const char *TAG = "wifi";

#define RETRY_PER_NET   3      /* 每个网络重试次数 */
#define SCAN_MAX_AP     24

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data);

static app_config_t s_cfg;                 /* 配置副本 (事件回调/重扫使用) */
static int      s_retry_count = 0;
static bool     s_got_ip = false;
static bool     s_started = false;
static bool     s_auto_connect = false;    /* STA_START 时是否自动连当前配置 */
static bool     s_tried_rescan = false;    /* 本轮是否已尝试换网 */
static char     s_current_ssid[33];
static volatile bool s_need_portal = false;
static volatile bool s_need_rescan = false;
static volatile bool s_radio_off = false;

/* 配网页用的扫描结果缓存 (切到 AP 模式后无法再扫) */
static wifi_ap_record_t s_ap_cache[SCAN_MAX_AP];
static int s_ap_cache_n = 0;

/* 择优连接的扫描缓冲: 必须放静态区, 每个 record ~110B, 24 个放栈上会爆 main 任务栈 */
static wifi_ap_record_t s_scan_buf[SCAN_MAX_AP];

/* ── 扫描 ── */

int wifi_mgr_scan_ap(wifi_ap_record_t *out, int max) {
    if (!s_started || s_radio_off || !out || max <= 0) return 0;

    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return 0;

    uint16_t num = 0;
    if (esp_wifi_scan_get_ap_num(&num) != ESP_OK || num == 0) return 0;

    uint16_t want = (num > max) ? (uint16_t)max : num;
    wifi_ap_record_t *recs = calloc(want, sizeof(wifi_ap_record_t));
    if (!recs) return 0;
    uint16_t got = want;
    esp_err_t err = esp_wifi_scan_get_ap_records(&got, recs);   /* 同时释放驱动内部分配 */
    if (err != ESP_OK) { free(recs); return 0; }

    for (int i = 0; i < got; i++) out[i] = recs[i];
    free(recs);
    return got;
}

/* ── 连接 ── */

static bool connect_ssid(const char *ssid, const char *pass) {
    if (!ssid || !ssid[0]) return false;
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, pass ? pass : "", sizeof(wc.sta.password));
    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK) return false;
    strlcpy(s_current_ssid, ssid, sizeof(s_current_ssid));
    s_retry_count = 0;
    ESP_LOGI(TAG, "connecting \"%s\"...", ssid);
    esp_wifi_connect();
    return true;
}

/* 扫描并接入信号最好的已保存网络 (skip_ssid: 跳过当前, 用于换网) */
static bool scan_and_connect_known(const char *skip_ssid) {
    int n = wifi_mgr_scan_ap(s_scan_buf, SCAN_MAX_AP);
    if (n <= 0) {
        ESP_LOGW(TAG, "scan found nothing");
        return false;
    }

    int best = -1, best_rssi = -127;
    for (int i = 0; i < n; i++) {
        const char *ssid = (const char *)s_scan_buf[i].ssid;
        if (skip_ssid && skip_ssid[0] && strcmp(ssid, skip_ssid) == 0) continue;
        if (!config_net_find(&s_cfg, ssid)) continue;   /* 不在已保存列表 */
        if (s_scan_buf[i].rssi > best_rssi) { best_rssi = s_scan_buf[i].rssi; best = i; }
    }
    if (best < 0) {
        ESP_LOGW(TAG, "no saved network in range (%d APs scanned)", n);
        return false;
    }

    const char *ssid = (const char *)s_scan_buf[best].ssid;
    ESP_LOGI(TAG, "picked \"%s\" (rssi %d, %d APs)", ssid, best_rssi, n);
    return connect_ssid(ssid, config_net_find(&s_cfg, ssid));
}

void wifi_mgr_connect_best(const app_config_t *cfg) {
    s_cfg = *cfg;

    static bool s_events = false;
    if (!s_events) {
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, &s_cfg));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, &s_cfg));
        s_events = true;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    s_auto_connect = false;          /* 由本函数决定连哪个 */
    ESP_ERROR_CHECK(esp_wifi_start());
    s_started = true;
    esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
    s_retry_count = 0;
    s_tried_rescan = false;
    s_current_ssid[0] = '\0';

    /* 优先扫已知热点; 扫不到就退回 last_ssid 直连 (可能只是暂时不在范围内) */
    if (!scan_and_connect_known(NULL)) {
        const char *ssid = s_cfg.last_ssid[0] ? s_cfg.last_ssid : s_cfg.net_ssid[0];
        const char *pw = config_net_find(&s_cfg, ssid);
        ESP_LOGW(TAG, "no known AP found, trying \"%s\" anyway", ssid);
        connect_ssid(ssid, pw ? pw : "");
    }
}

bool wifi_is_connected(void) { return s_got_ip; }
const char *wifi_mgr_current_ssid(void) { return s_current_ssid; }
bool wifi_mgr_radio_on(void) { return !s_radio_off; }

/* ── 省电: 查询间隔内关射频 ── */

void wifi_mgr_radio_sleep(void) {
    if (s_radio_off) return;
    s_radio_off = true;
    s_got_ip = false;
    s_retry_count = 0;
    esp_wifi_stop();
    s_started = false;
    ESP_LOGI(TAG, "radio off (power save)");
}

void wifi_mgr_radio_wake(void) {
    if (!s_radio_off) return;
    s_radio_off = false;
    s_retry_count = 0;
    s_tried_rescan = false;
    s_auto_connect = true;           /* 直接用上次配置重连, 省去扫描 */
    ESP_ERROR_CHECK(esp_wifi_start());
    s_started = true;
    ESP_LOGI(TAG, "radio on, reconnecting \"%s\"...", s_current_ssid);
}

/* ── 门户请求 / 换网请求 (线程安全) ── */

void wifi_mgr_request_portal(void) { s_need_portal = true; }

bool wifi_mgr_poll_portal(void) {
    if (!s_need_portal) return false;
    s_need_portal = false;
    wifi_mgr_start_portal(NULL);
    return true;   /* 不会到达 */
}

/* 主任务调用: 重试耗尽后扫描换用其他已保存网络 */
void wifi_mgr_poll_rescan(void) {
    if (!s_need_rescan) return;
    s_need_rescan = false;
    if (s_got_ip || s_radio_off || !s_started) return;

    ESP_LOGW(TAG, "retries exhausted, looking for another saved network...");
    if (scan_and_connect_known(s_current_ssid)) {
        s_tried_rescan = true;        /* 已换网; 再失败就直接进配网 */
    } else {
        ESP_LOGE(TAG, "no other saved network, requesting portal");
        wifi_mgr_request_portal();
    }
}

/* ── 事件 ── */

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_auto_connect) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_radio_off) return;                 /* 主动关射频, 忽略 */
        s_got_ip = false;
        uint8_t reason = data ? ((wifi_event_sta_disconnected_t *)data)->reason : 0;

        if (s_retry_count < RETRY_PER_NET) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGW(TAG, "retry %d/%d (reason=%d)", s_retry_count, RETRY_PER_NET, reason);
        } else if (!s_tried_rescan) {
            s_need_rescan = true;                /* 交给主任务扫描换网 */
        } else {
            ESP_LOGE(TAG, "all known networks failed, requesting portal");
            wifi_mgr_request_portal();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        s_retry_count = 0;
        s_tried_rescan = false;
        s_got_ip = true;
        ESP_LOGI(TAG, "got ip " IPSTR " on \"%s\"", IP2STR(&evt->ip_info.ip), s_current_ssid);
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

/* ── 配网门户 ── */

#define PORTAL_HTML_MAX 4096
static char s_portal_html[PORTAL_HTML_MAX];

static void append(char **p, size_t *left, const char *fmt, ...) {
    if (*left == 0) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(*p, *left, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= *left) { *left = 0; return; }
    *p += n;
    *left -= n;
}

/* 动态生成配网页: 附带附近热点列表 (<datalist>) 与已保存网络 */
static void build_portal_html(void) {
    char *p = s_portal_html;
    size_t left = sizeof(s_portal_html);

    append(&p, &left,
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>AI-Mate 配网</title><style>"
        "body{font-family:sans-serif;max-width:420px;margin:16px auto;padding:0 12px;background:#111;color:#eee}"
        "h2{border-bottom:2px solid #4af;padding-bottom:6px;font-size:17px}"
        "label{display:block;margin:10px 0 3px;font-size:14px;color:#9cf}"
        "input{width:100%%;box-sizing:border-box;padding:8px;font-size:15px;border:1px solid #456;border-radius:5px;background:#1b1b1b;color:#eee}"
        "button{margin-top:16px;width:100%%;padding:12px;font-size:16px;background:#257;border:0;border-radius:6px;color:#fff}"
        "small{color:#888}ul{color:#9cf;font-size:14px}"
        "</style></head><body><h2>AI-Mate 配置</h2><form method='POST' action='/save'>");

    /* WiFi: 已保存网络列表 */
    if (s_cfg.net_count > 0) {
        append(&p, &left, "<label>已保存的 WiFi (%d 个, 开机自动择优连接)</label><ul>",
               s_cfg.net_count);
        for (int i = 0; i < s_cfg.net_count; i++)
            append(&p, &left, "<li>%s</li>", s_cfg.net_ssid[i]);
        append(&p, &left, "</ul>");
    }

    /* WiFi: 输入框 + 附近热点候选 */
    append(&p, &left,
        "<label>WiFi 名称 (SSID; 留空=保持已保存网络)</label>"
        "<input name='ssid' list='aps' placeholder='选择一个附近热点'>"
        "<datalist id='aps'>");
    {
        for (int i = 0; i < s_ap_cache_n; i++) {
            /* 去重: 同名 AP 只列一次 */
            bool dup = false;
            for (int k = 0; k < i; k++)
                if (strcmp((char *)s_ap_cache[k].ssid, (char *)s_ap_cache[i].ssid) == 0) { dup = true; break; }
            if (!dup && s_ap_cache[i].ssid[0])
                append(&p, &left, "<option value=\"%s\">%d dBm</option>",
                       (char *)s_ap_cache[i].ssid, s_ap_cache[i].rssi);
        }
    }
    append(&p, &left, "</datalist>"
        "<label>WiFi 密码 (新网络必须填)</label><input name='pass' type='password'>"
        "<label><input type='checkbox' name='forget' value='1' style='width:auto'> 清除所有已保存的 WiFi</label>");

    append(&p, &left,
        "<h2 style='margin-top:22px'>智谱 GLM Coding Plan</h2>"
        "<label>API Key</label><input name='gkey' value=\"%s\" required>"
        "<label>Organization ID (org-xxx)</label><input name='gorg' value=\"%s\" required>"
        "<label>Project ID (proj_xxx)</label><input name='gproj' value=\"%s\" required>"
        "<label>套餐类型 (1=个人 2=团队)</label><input name='gtype' value='%d' required>",
        s_cfg.glm_key, s_cfg.glm_org, s_cfg.glm_project, s_cfg.glm_type);

    append(&p, &left,
        "<h2 style='margin-top:22px'>DeepSeek</h2>"
        "<label>API Key</label><input name='dkey' value=\"%s\" required>",
        s_cfg.dsk_key);

    append(&p, &left,
        "<h2 style='margin-top:22px'>天气 (选填)</h2>"
        "<label>城市 (如 上海; 留空 = 按 IP 自动定位)</label><input name='wcity' value=\"%s\">",
        s_cfg.wx_city);

    append(&p, &left,
        "<h2 style='margin-top:22px'>其他</h2>"
        "<label>轮询间隔 (分钟)</label><input name='pmin' value='%d'>"
        "<button type='submit'>保存并重启</button>"
        "<p><small>智谱 org/project: 浏览器登录 bigmodel.cn/coding-plan → F12 → Network → "
        "找 quota/limit 请求头 bigmodel-organization / bigmodel-project</small></p>"
        "</form></body></html>", s_cfg.poll_min);
}

static const char SAVED_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'></head>"
"<body style='font-family:sans-serif;text-align:center;padding-top:60px'>"
"<h2>已保存 ✓ 设备正在重启并连接 WiFi</h2>"
"<p>若 1 分钟后屏幕仍显示连接失败, 请长按 BOOT 键 3 秒重新配网</p>"
"</body></html>";

static const char ERR_HTML[] =
"<!DOCTYPE html><html><head><meta charset='utf-8'></head>"
"<body style='font-family:sans-serif;text-align:center;padding-top:60px'>"
"<h2>配置不完整, 未保存</h2>"
"<p>至少需要一个 WiFi 网络, 以及智谱 / DeepSeek 的 API Key</p>"
"<p><a href='/'>返回重新填写</a></p>"
"</body></html>";

static esp_err_t portal_root(httpd_req_t *req) {
    build_portal_html();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, s_portal_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t portal_save(httpd_req_t *req) {
    static char body[2048];
    size_t total = req->content_len < sizeof(body) - 1 ? req->content_len : sizeof(body) - 1;
    int received = httpd_req_recv(req, body, total);
    if (received <= 0) return ESP_FAIL;
    body[received] = '\0';

    /* 从既有配置出发 (保留已保存的网络列表与未重填的字段) */
    app_config_t cfg;
    if (!config_load(&cfg)) config_defaults(&cfg);

    bool forget = strstr(body, "forget=1") != NULL;
    if (forget) {
        config_net_clear(&cfg);
        ESP_LOGW(TAG, "portal: clearing all saved WiFi networks");
    }

    char ssid[33] = "", pass[65] = "";
    form_field(body, "ssid", ssid, sizeof(ssid));
    form_field(body, "pass", pass, sizeof(pass));
    if (ssid[0]) {
        if (pass[0]) {
            config_net_add(&cfg, ssid, pass);          /* 新增/更新密码 */
        } else {
            const char *old = config_net_find(&cfg, ssid);
            if (old) {                                  /* 已保存过, 沿用旧密码 */
                strlcpy(cfg.last_ssid, ssid, sizeof(cfg.last_ssid));
            } else {
                ESP_LOGW(TAG, "portal: new SSID without password, ignored");
            }
        }
    }

    /* 其余字段: 表单已回填旧值, 直接覆盖 */
    char tmp[8];
    form_field(body, "gkey", cfg.glm_key, sizeof(cfg.glm_key));
    form_field(body, "gorg", cfg.glm_org, sizeof(cfg.glm_org));
    form_field(body, "gproj", cfg.glm_project, sizeof(cfg.glm_project));
    form_field(body, "dkey", cfg.dsk_key, sizeof(cfg.dsk_key));
    form_field(body, "wcity", cfg.wx_city, sizeof(cfg.wx_city));
    form_field(body, "gtype", tmp, sizeof(tmp));
    if (tmp[0] == '2') cfg.glm_type = 2; else if (tmp[0] == '1') cfg.glm_type = 1;
    form_field(body, "pmin", tmp, sizeof(tmp));
    int pm = atoi(tmp);
    if (pm >= 1 && pm <= 60) cfg.poll_min = (uint8_t)pm;

    /* 供运行时使用的 "当前网络" */
    strlcpy(cfg.wifi_ssid, cfg.last_ssid[0] ? cfg.last_ssid : cfg.net_ssid[0], sizeof(cfg.wifi_ssid));
    const char *pw = config_net_find(&cfg, cfg.wifi_ssid);
    strlcpy(cfg.wifi_pass, pw ? pw : "", sizeof(cfg.wifi_pass));

    httpd_resp_set_type(req, "text/html; charset=utf-8");

    if (cfg.net_count == 0 || !cfg.glm_key[0] || !cfg.dsk_key[0]) {
        ESP_LOGW(TAG, "portal: incomplete config, rejected");
        return httpd_resp_send(req, ERR_HTML, HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_send(req, SAVED_HTML, HTTPD_RESP_USE_STRLEN);
    config_save(&cfg);
    weather_coords_clear();      /* 更换网络/城市后重新定位 */
    ESP_LOGI(TAG, "portal: saved (%d networks), restarting...", cfg.net_count);
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;   /* 不会到达 */
}

static httpd_handle_t s_server = NULL;

void wifi_mgr_start_portal(app_config_t *cfg) {
    static app_config_t local;          /* ~830B, 别放栈上 (main 任务栈有限) */
    if (cfg) local = *cfg;
    else if (!config_load(&local)) config_defaults(&local);
    s_cfg = local;                       /* 供配网页回填与扫描 */

    ESP_LOGW(TAG, "=== AP 配网模式 ===");

    /* 切 AP 前先在 STA 模式扫一次, 结果缓存供配网页列出附近热点 */
    if (s_radio_off) {                  /* 休眠中: 临时开射频扫描 */
        s_radio_off = false;
        esp_wifi_set_mode(WIFI_MODE_STA);
        if (esp_wifi_start() == ESP_OK) { s_auto_connect = false; s_started = true; }
    } else if (!s_started) {
        esp_wifi_set_mode(WIFI_MODE_STA);
        if (esp_wifi_start() == ESP_OK) { s_auto_connect = false; s_started = true; }
    }
    s_ap_cache_n = wifi_mgr_scan_ap(s_ap_cache, SCAN_MAX_AP);
    ESP_LOGI(TAG, "portal scan: %d AP(s)", s_ap_cache_n);

    esp_wifi_stop();
    s_started = false;
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
        hc.stack_size = 6144;            /* 动态 HTML 生成需要更多栈 */
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
