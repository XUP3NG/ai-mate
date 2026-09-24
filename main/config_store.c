#include "config_store.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "cfg";
#define NS "ai_mate"

void config_defaults(app_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->glm_type = 1;      /* 个人版 */
    cfg->poll_min = 5;
}

/* ── WiFi 列表操作 ── */

const char *config_net_find(const app_config_t *cfg, const char *ssid) {
    if (!ssid || !ssid[0]) return NULL;
    for (int i = 0; i < cfg->net_count && i < CFG_NET_MAX; i++) {
        if (strcmp(cfg->net_ssid[i], ssid) == 0) return cfg->net_pass[i];
    }
    return NULL;
}

int config_net_add(app_config_t *cfg, const char *ssid, const char *pass) {
    if (!ssid || !ssid[0] || ssid[0] == '\0') return -1;
    if (!pass) pass = "";

    int idx = -1;
    for (int i = 0; i < cfg->net_count && i < CFG_NET_MAX; i++) {
        if (strcmp(cfg->net_ssid[i], ssid) == 0) { idx = i; break; }
    }
    if (idx < 0) {
        if (cfg->net_count >= CFG_NET_MAX) {
            /* 列表满: 挤掉最老的 (0 号), 整体前移 */
            for (int i = 1; i < CFG_NET_MAX; i++) {
                strlcpy(cfg->net_ssid[i - 1], cfg->net_ssid[i], sizeof(cfg->net_ssid[0]));
                strlcpy(cfg->net_pass[i - 1], cfg->net_pass[i], sizeof(cfg->net_pass[0]));
            }
            idx = CFG_NET_MAX - 1;
        } else {
            idx = cfg->net_count++;
        }
    }
    strlcpy(cfg->net_ssid[idx], ssid, sizeof(cfg->net_ssid[0]));
    strlcpy(cfg->net_pass[idx], pass, sizeof(cfg->net_pass[0]));
    strlcpy(cfg->last_ssid, ssid, sizeof(cfg->last_ssid));
    return idx;
}

void config_net_clear(app_config_t *cfg) {
    cfg->net_count = 0;
    cfg->last_ssid[0] = '\0';
    for (int i = 0; i < CFG_NET_MAX; i++) {
        cfg->net_ssid[i][0] = '\0';
        cfg->net_pass[i][0] = '\0';
    }
}

/* ── 读写 ── */

bool config_load(app_config_t *cfg) {
    config_defaults(cfg);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;

    size_t sz;
    sz = sizeof(cfg->glm_key);    nvs_get_str(h, "gkey",  cfg->glm_key, &sz);
    sz = sizeof(cfg->glm_org);    nvs_get_str(h, "gorg",  cfg->glm_org, &sz);
    sz = sizeof(cfg->glm_project);nvs_get_str(h, "gproj", cfg->glm_project, &sz);
    sz = sizeof(cfg->dsk_key);    nvs_get_str(h, "dkey",  cfg->dsk_key, &sz);
    sz = sizeof(cfg->wx_city);    nvs_get_str(h, "wcity", cfg->wx_city, &sz);
    uint8_t u8 = 0;
    if (nvs_get_u8(h, "gtype", &u8) == ESP_OK && (u8 == 1 || u8 == 2)) cfg->glm_type = u8;
    if (nvs_get_u8(h, "pmin", &u8) == ESP_OK && u8 >= 1 && u8 <= 60) cfg->poll_min = u8;

    /* WiFi 列表 */
    u8 = 0;
    if (nvs_get_u8(h, "nnet", &u8) == ESP_OK && u8 <= CFG_NET_MAX) cfg->net_count = u8;
    for (int i = 0; i < cfg->net_count; i++) {
        char key[8];
        snprintf(key, sizeof(key), "s%d", i);
        sz = sizeof(cfg->net_ssid[i]); nvs_get_str(h, key, cfg->net_ssid[i], &sz);
        snprintf(key, sizeof(key), "p%d", i);
        sz = sizeof(cfg->net_pass[i]); nvs_get_str(h, key, cfg->net_pass[i], &sz);
    }
    sz = sizeof(cfg->last_ssid);  nvs_get_str(h, "last", cfg->last_ssid, &sz);

    /* 旧版本迁移: 只有单个 ssid/pass, 无列表 */
    if (cfg->net_count == 0) {
        char ssid[33] = "", pass[65] = "";
        sz = sizeof(ssid); nvs_get_str(h, "ssid", ssid, &sz);
        sz = sizeof(pass); nvs_get_str(h, "pass", pass, &sz);
        if (ssid[0]) {
            config_net_add(cfg, ssid, pass);
            ESP_LOGI(TAG, "migrated legacy single-network config");
        }
    }
    nvs_close(h);

    /* 对外暴露 "当前网络": 优先上次成功的 */
    strlcpy(cfg->wifi_ssid, cfg->last_ssid[0] ? cfg->last_ssid : cfg->net_ssid[0],
            sizeof(cfg->wifi_ssid));
    const char *pw = config_net_find(cfg, cfg->wifi_ssid);
    strlcpy(cfg->wifi_pass, pw ? pw : "", sizeof(cfg->wifi_pass));

    bool ok = cfg->net_count > 0 || cfg->wifi_ssid[0] != '\0';
    ESP_LOGI(TAG, "config %s (%d network(s) saved, last=\"%s\")",
             ok ? "loaded" : "empty", cfg->net_count, cfg->last_ssid);
    return ok;
}

void config_save(const app_config_t *cfg) {
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NS, NVS_READWRITE, &h));

    nvs_set_str(h, "gkey",  cfg->glm_key);
    nvs_set_str(h, "gorg",  cfg->glm_org);
    nvs_set_str(h, "gproj", cfg->glm_project);
    nvs_set_str(h, "dkey",  cfg->dsk_key);
    nvs_set_str(h, "wcity", cfg->wx_city);
    nvs_set_u8(h, "gtype", cfg->glm_type);
    nvs_set_u8(h, "pmin",  cfg->poll_min);

    /* WiFi 列表 + 首选 */
    uint8_t n = cfg->net_count > CFG_NET_MAX ? CFG_NET_MAX : cfg->net_count;
    nvs_set_u8(h, "nnet", n);
    for (int i = 0; i < n; i++) {
        char key[8];
        snprintf(key, sizeof(key), "s%d", i);
        nvs_set_str(h, key, cfg->net_ssid[i]);
        snprintf(key, sizeof(key), "p%d", i);
        nvs_set_str(h, key, cfg->net_pass[i]);
    }
    nvs_set_str(h, "last", cfg->last_ssid);
    /* 兼容旧字段 */
    nvs_set_str(h, "ssid", cfg->net_ssid[0]);
    nvs_set_str(h, "pass", cfg->net_pass[0]);

    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
    ESP_LOGI(TAG, "config saved (%d network(s))", n);
}

void config_clear(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "nnet");
        nvs_erase_key(h, "last");
        nvs_erase_key(h, "ssid");
        nvs_erase_key(h, "pass");
        for (int i = 0; i < CFG_NET_MAX; i++) {
            char key[8];
            snprintf(key, sizeof(key), "s%d", i);
            nvs_erase_key(h, key);
            snprintf(key, sizeof(key), "p%d", i);
            nvs_erase_key(h, key);
        }
        nvs_commit(h);
        nvs_close(h);
    }
}
