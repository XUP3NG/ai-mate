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

bool config_load(app_config_t *cfg) {
    config_defaults(cfg);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;

    size_t sz;
    sz = sizeof(cfg->wifi_ssid);  nvs_get_str(h, "ssid",  cfg->wifi_ssid, &sz);
    sz = sizeof(cfg->wifi_pass);  nvs_get_str(h, "pass",  cfg->wifi_pass, &sz);
    sz = sizeof(cfg->glm_key);    nvs_get_str(h, "gkey",  cfg->glm_key, &sz);
    sz = sizeof(cfg->glm_org);    nvs_get_str(h, "gorg",  cfg->glm_org, &sz);
    sz = sizeof(cfg->glm_project);nvs_get_str(h, "gproj", cfg->glm_project, &sz);
    sz = sizeof(cfg->dsk_key);    nvs_get_str(h, "dkey",  cfg->dsk_key, &sz);
    sz = sizeof(cfg->wx_city);    nvs_get_str(h, "wcity", cfg->wx_city, &sz);
    uint8_t u8 = 0;
    if (nvs_get_u8(h, "gtype", &u8) == ESP_OK && (u8 == 1 || u8 == 2)) cfg->glm_type = u8;
    if (nvs_get_u8(h, "pmin", &u8) == ESP_OK && u8 >= 1 && u8 <= 60) cfg->poll_min = u8;
    nvs_close(h);

    bool ok = cfg->wifi_ssid[0] != '\0';
    ESP_LOGI(TAG, "config %s", ok ? "loaded" : "empty");
    return ok;
}

void config_save(const app_config_t *cfg) {
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NS, NVS_READWRITE, &h));
    nvs_set_str(h, "ssid",  cfg->wifi_ssid);
    nvs_set_str(h, "pass",  cfg->wifi_pass);
    nvs_set_str(h, "gkey",  cfg->glm_key);
    nvs_set_str(h, "gorg",  cfg->glm_org);
    nvs_set_str(h, "gproj", cfg->glm_project);
    nvs_set_str(h, "dkey",  cfg->dsk_key);
    nvs_set_str(h, "wcity", cfg->wx_city);
    nvs_set_u8(h, "gtype", cfg->glm_type);
    nvs_set_u8(h, "pmin",  cfg->poll_min);
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
    ESP_LOGI(TAG, "config saved");
}

void config_clear(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "ssid");
        nvs_erase_key(h, "pass");
        nvs_commit(h);
        nvs_close(h);
    }
}
