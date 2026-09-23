#include "weather.h"
#include "net_query.h"
#include "wifi_mgr.h"
#include "nvs.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "wx";

#define GEO_URL   "https://geocoding-api.open-meteo.com/v1/search"
#define FC_URL    "https://api.open-meteo.com/v1/forecast"
#define WX_NS     "ai_hist"      /* 坐标缓存放历史命名空间 */
#define WX_BUF    4096

/* ── WMO 天气码映射 ── */

const char *wmo_text(uint8_t code) {
    switch (code) {
        case 0:  return "晴";
        case 1:  return "大部晴";
        case 2:  return "多云";
        case 3:  return "阴";
        case 45: case 48: return "雾";
        case 51: case 53: case 55: return "毛毛雨";
        case 56: case 57: return "冻毛毛雨";
        case 61: return "小雨";
        case 63: return "中雨";
        case 65: return "大雨";
        case 66: case 67: return "冻雨";
        case 71: return "小雪";
        case 73: return "中雪";
        case 75: return "大雪";
        case 77: return "雪粒";
        case 80: return "小阵雨";
        case 81: return "阵雨";
        case 82: return "强阵雨";
        case 85: case 86: return "阵雪";
        case 95: return "雷雨";
        case 96: case 99: return "雷雨冰雹";
        default: return "未知";
    }
}

const char *wmo_icon(uint8_t code) {
    /* U+2600 ☀  U+2601 ☁  U+2602 ☂  U+2614 ☔  U+2744 ❄  U+26A1 ⚡  U+2637 ☽? 用☰表雾 */
    if (code <= 1)        return "\xE2\x98\x80";          /* ☀ */
    if (code == 2 || code == 3) return "\xE2\x98\x81";    /* ☁ */
    if (code >= 45 && code <= 57) return "\xE2\x98\xB0";  /* ☰ 雾 */
    if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82)) return "\xE2\x98\x82"; /* ☂ */
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return "\xE2\x9D\x84";   /* ❄ */
    if (code >= 95)       return "\xE2\x9A\xA1";          /* ⚡ */
    return "\xE2\x98\x81";
}

/* ── URL 编码 (中文城市名) ── */
static void url_encode(const char *in, char *out, size_t sz) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < sz; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0xF];
        }
    }
    out[o] = '\0';
}

/* ── 坐标缓存 ── */
static bool coords_load(int32_t *lat, int32_t *lon) {
    nvs_handle_t h;
    if (nvs_open(WX_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t sz = 4;
    bool ok = nvs_get_i32(h, "wxlat", lat) == ESP_OK &&
              nvs_get_i32(h, "wxlon", lon) == ESP_OK;
    nvs_close(h);
    return ok;
}

static void coords_save(int32_t lat, int32_t lon) {
    nvs_handle_t h;
    if (nvs_open(WX_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "wxlat", lat);
    nvs_set_i32(h, "wxlon", lon);
    nvs_commit(h);
    nvs_close(h);
}

void weather_coords_clear(void) {
    nvs_handle_t h;
    if (nvs_open(WX_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "wxlat");
        nvs_erase_key(h, "wxlon");
        nvs_commit(h);
        nvs_close(h);
    }
}

/* 城市名 → 坐标 (x10000 定点), 成功 true */
static bool geocode(const char *city, int32_t *lat, int32_t *lon) {
    char enc[96], url[192], buf[1024];
    url_encode(city, enc, sizeof(enc));
    snprintf(url, sizeof(url),
             GEO_URL "?name=%s&count=1&language=zh&format=json", enc);

    int n = net_https_get(url, NULL, NULL, NULL, buf, sizeof(buf));
    if (n <= 0) return false;

    cJSON *root = cJSON_Parse(buf);
    if (!root) return false;
    cJSON *results = cJSON_GetObjectItem(root, "results");
    cJSON *first = cJSON_IsArray(results) ? cJSON_GetArrayItem(results, 0) : NULL;
    bool ok = false;
    if (first) {
        cJSON *j;
        double la = 0, lo = 0;
        if ((j = cJSON_GetObjectItem(first, "latitude")) && cJSON_IsNumber(j))  la = j->valuedouble;
        if ((j = cJSON_GetObjectItem(first, "longitude")) && cJSON_IsNumber(j)) lo = j->valuedouble;
        if (la != 0 || lo != 0) {
            *lat = (int32_t)(la * 10000);
            *lon = (int32_t)(lo * 10000);
            ok = true;
        }
    }
    cJSON_Delete(root);
    return ok;
}

/* ── 一轮天气查询 ── */
void weather_query(app_state_t *st, const app_config_t *cfg) {
    strlcpy(st->wx.city, cfg->wx_city, sizeof(st->wx.city));
    if (!cfg->wx_city[0]) {
        strlcpy(st->wx.err, "未配置城市", sizeof(st->wx.err));
        return;
    }
    if (!net_query_wifi_ok()) {
        strlcpy(st->wx.err, "WiFi 未连接", sizeof(st->wx.err));
        return;
    }

    /* 坐标: 缓存 → geocoding */
    int32_t lat = 0, lon = 0;
    if (!coords_load(&lat, &lon)) {
        if (!geocode(cfg->wx_city, &lat, &lon)) {
            strlcpy(st->wx.err, "城市未找到", sizeof(st->wx.err));
            ESP_LOGW(TAG, "geocode failed for \"%s\"", cfg->wx_city);
            return;
        }
        coords_save(lat, lon);
        ESP_LOGI(TAG, "geocoded \"%s\" -> %d.%04d, %d.%04d",
                 cfg->wx_city, lat / 10000, lat % 10000, lon / 10000, lon % 10000);
    }

    char url[288];
    snprintf(url, sizeof(url),
             FC_URL "?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,relative_humidity_2m,weather_code"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min"
             "&forecast_days=%d&timezone=Asia%%2FShanghai",
             lat / 10000.0, lon / 10000.0, WX_DAYS);

    static char buf[WX_BUF];
    int n = net_https_get(url, NULL, NULL, NULL, buf, sizeof(buf));
    if (n <= 0) {
        strlcpy(st->wx.err, "网络/HTTP 失败", sizeof(st->wx.err));
        return;
    }

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        strlcpy(st->wx.err, "JSON 解析失败", sizeof(st->wx.err));
        return;
    }

    weather_info_t *wx = &st->wx;
    bool ok = false;

    cJSON *cur = cJSON_GetObjectItem(root, "current");
    if (cur) {
        cJSON *j;
        if ((j = cJSON_GetObjectItem(cur, "temperature_2m")) && cJSON_IsNumber(j))
            wx->temp_x10 = (int16_t)(j->valuedouble * 10);
        if ((j = cJSON_GetObjectItem(cur, "relative_humidity_2m")) && cJSON_IsNumber(j))
            wx->humidity = (uint8_t)j->valuedouble;
        if ((j = cJSON_GetObjectItem(cur, "weather_code")) && cJSON_IsNumber(j))
            wx->code = (uint8_t)j->valuedouble;
        ok = true;
    }

    cJSON *daily = cJSON_GetObjectItem(root, "daily");
    if (daily) {
        cJSON *codes = cJSON_GetObjectItem(daily, "weather_code");
        cJSON *tmax  = cJSON_GetObjectItem(daily, "temperature_2m_max");
        cJSON *tmin  = cJSON_GetObjectItem(daily, "temperature_2m_min");
        if (cJSON_IsArray(codes)) {
            int nd = cJSON_GetArraySize(codes);
            if (nd > WX_DAYS) nd = WX_DAYS;
            for (int i = 0; i < nd; i++) {
                cJSON *c = cJSON_GetArrayItem(codes, i);
                cJSON *mx = tmax ? cJSON_GetArrayItem(tmax, i) : NULL;
                cJSON *mn = tmin ? cJSON_GetArrayItem(tmin, i) : NULL;
                if (c) wx->dcode[i] = (uint8_t)c->valuedouble;
                if (mx && cJSON_IsNumber(mx)) wx->tmax_x10[i] = (int16_t)(mx->valuedouble * 10);
                if (mn && cJSON_IsNumber(mn)) wx->tmin_x10[i] = (int16_t)(mn->valuedouble * 10);
            }
            ok = true;
        }
    }

    cJSON_Delete(root);

    if (ok) {
        wx->valid = true;
        wx->err[0] = '\0';
        wx->last_ok_ms = (uint32_t)(esp_timer_get_time() / 1000);
        ESP_LOGI(TAG, "%s: %s %.1f°C 湿%d%% | 明日 %s %.0f/%.0f°C",
                 cfg->wx_city, wmo_text(wx->code), wx->temp_x10 / 10.0, wx->humidity,
                 wmo_text(wx->dcode[1]), wx->tmax_x10[1] / 10.0, wx->tmin_x10[1] / 10.0);
    } else {
        strlcpy(st->wx.err, "响应缺少数据", sizeof(st->wx.err));
    }
}
