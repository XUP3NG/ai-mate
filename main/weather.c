/**
 * weather — 双数据源天气
 *
 *   和风天气 (QWeather): 需 API Host + API Key, 中国城市精度更好
 *     GET https://<host>/weather/v1/current/{lat}/{lon}   Header: X-QW-Api-Key
 *     GET https://<host>/weather/v1/daily/{lat}/{lon}?days=N
 *   Open-Meteo: 免 Key 保底 (配置里没填和风参数时使用)
 *
 * 定位: 城市名 → geocoding 一次; 留空 → IP 自动定位 (24h 刷新)。两源共用经纬度。
 * 数据来源标注: 和风天气要求注明来源, UI 页脚会显示来源名。
 */

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
#define OM_URL    "https://api.open-meteo.com/v1/forecast"
#define WX_NS     "ai_hist"      /* 坐标缓存放历史命名空间 */
#define WX_BUF    16384          /* 和风 daily 响应较冗长 (含天文数据), 缓冲区要大 */

static char s_buf[WX_BUF];       /* 接收缓冲 (可能是 gzip) */
static char s_dec[WX_BUF];       /* 解码后的 JSON (zlib 解压输出) */

/* ── 未来几小时降水提醒 ──
 * 用逐小时预报推算: 窗口内首个"有降水或概率≥50%"的小时数。
 * 两数据源各自填好 hour_wx_t 后调用 rain_eval()。
 */
#define RAIN_WINDOW_H   6      /* 提醒窗口: 未来 6 小时 */
#define RAIN_REQ_H      8      /* 请求小时数 (要覆盖窗口 +1) */
#define RAIN_PROB_MIN   50     /* 概率阈值 % */

typedef struct {
    bool    precip;     /* 该小时有降水量 */
    bool    snow;       /* 是雪 */
    uint8_t prob;       /* 降水概率 % */
} hour_wx_t;

static void rain_eval(weather_info_t *wx, const hour_wx_t *h, int n) {
    int first = -1, maxprob = 0;
    bool snow = false;

    for (int i = 1; i <= RAIN_WINDOW_H && i < n; i++) {
        if (h[i].prob > maxprob) maxprob = h[i].prob;
        bool wet = h[i].precip || h[i].prob >= RAIN_PROB_MIN;
        if (wet && first < 0) {
            first = i;
            snow = h[i].snow;
        }
    }

    wx->rain_valid = true;
    wx->rain_in_hours = first > 0 ? (uint8_t)first : 0;
    wx->rain_prob = (uint8_t)maxprob;

    if (first > 0) {
        strlcpy(wx->rain_icon, snow ? "\xE2\x9D\x84" : "\xE2\x98\x94", sizeof(wx->rain_icon)); /* ❄ / ☔ */
        snprintf(wx->rain_text, sizeof(wx->rain_text), "%d小时后有%s (%d%%)",
                 first, snow ? "雪" : "雨", maxprob);
        ESP_LOGI(TAG, "rain alert: %s", wx->rain_text);
    } else {
        wx->rain_icon[0] = '\0';
        snprintf(wx->rain_text, sizeof(wx->rain_text), "未来%d小时无降水", RAIN_WINDOW_H);
    }
}

/* ── WMO 天气码 (Open-Meteo) ── */

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
    if (code <= 1)        return "\xE2\x98\x80";          /* ☀ 晴 */
    if (code == 2 || code == 3) return "\xE2\x98\x81";    /* ☁ 多云/阴 */
    if (code == 45 || code == 48) return "\xE2\x98\xB0";  /* ☰ 雾 */
    if (code >= 51 && code <= 57) return "\xE2\x98\x94";  /* ☔ 毛毛雨 */
    if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82)) return "\xE2\x98\x82"; /* ☂ 雨 */
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return "\xE2\x9D\x84";   /* ❄ 雪 */
    if (code >= 95)       return "\xE2\x9A\xA1";          /* ⚡ 雷雨 */
    return "\xE2\x98\x81";
}

/* ── 和风图标代码 → 符号 (官方图标: 1xx 晴/云, 2xx 风, 3xx 雨, 4xx 雪, 5xx 雾霾) ── */
static const char *qw_icon(const char *code) {
    int c = atoi(code ? code : "0");
    if (c == 100 || c == 102 || c == 103 || c == 150 || c == 153) return "\xE2\x98\x80"; /* ☀ */
    if (c == 302 || c == 303 || c == 304) return "\xE2\x9A\xA1";                          /* ⚡ 雷阵雨 */
    if (c >= 300 && c <= 399) return (c >= 310 && c <= 313) ? "\xE2\x98\x94" : "\xE2\x98\x82"; /* ☔/☂ 雨 */
    if (c >= 400 && c <= 499) return "\xE2\x9D\x84";                                       /* ❄ 雪 */
    if (c >= 500 && c <= 515) return "\xE2\x98\xB0";                                       /* ☰ 雾/霾/沙尘 */
    return "\xE2\x98\x81";                                                                 /* ☁ 其他含风 */
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

/* ── 坐标缓存 (含来源与时间戳 与 城市名) ── */

static bool coords_load(int32_t *lat, int32_t *lon, char *city, size_t citysz, int64_t *epo) {
    nvs_handle_t h;
    if (nvs_open(WX_NS, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = nvs_get_i32(h, "wxlat", lat) == ESP_OK &&
              nvs_get_i32(h, "wxlon", lon) == ESP_OK;
    if (ok && city) {
        size_t csz = citysz;
        if (nvs_get_str(h, "wxcity2", city, &csz) != ESP_OK) city[0] = '\0';
    }
    if (ok && epo) {
        size_t esz = 8;
        if (nvs_get_i64(h, "wxepo", epo) != ESP_OK) *epo = 0;
    }
    nvs_close(h);
    return ok;
}

static void coords_save(int32_t lat, int32_t lon, const char *city) {
    nvs_handle_t h;
    if (nvs_open(WX_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "wxlat", lat);
    nvs_set_i32(h, "wxlon", lon);
    if (city) nvs_set_str(h, "wxcity2", city);
    nvs_set_i64(h, "wxepo", (int64_t)time(NULL));
    nvs_commit(h);
    nvs_close(h);
}

void weather_coords_clear(void) {
    nvs_handle_t h;
    if (nvs_open(WX_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "wxlat");
        nvs_erase_key(h, "wxlon");
        nvs_erase_key(h, "wxcity2");
        nvs_erase_key(h, "wxepo");
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ── 定位: IP (设备直连公网) 或 城市名 geocoding ── */

static bool ip_locate(int32_t *lat, int32_t *lon, char *city, size_t citysz) {
    /* 主: ip-api.com (免 Key, UTF-8, 直接给经纬度和中文城市名) */
    {
        int n = net_https_get("http://ip-api.com/json/?lang=zh-CN&fields=status,lat,lon,city",
                              NULL, NULL, NULL, s_buf, sizeof(s_buf));
        if (n > 0) {
            cJSON *root = cJSON_Parse(s_buf);
            if (root) {
                cJSON *j;
                const char *st = NULL;
                cJSON *js = cJSON_GetObjectItem(root, "status");
                if (js && js->valuestring) st = js->valuestring;
                if (!st || strcmp(st, "success") == 0) {
                    double la = 0, lo = 0;
                    if ((j = cJSON_GetObjectItem(root, "latitude")) && cJSON_IsNumber(j)) la = j->valuedouble;
                    if ((j = cJSON_GetObjectItem(root, "longitude")) && cJSON_IsNumber(j)) lo = j->valuedouble;
                    if (la == 0 && (j = cJSON_GetObjectItem(root, "lat")) && cJSON_IsNumber(j)) la = j->valuedouble;
                    if (lo == 0 && (j = cJSON_GetObjectItem(root, "lon")) && cJSON_IsNumber(j)) lo = j->valuedouble;
                    if (la != 0 || lo != 0) {
                        *lat = (int32_t)(la * 10000);
                        *lon = (int32_t)(lo * 10000);
                        if (city && citysz) {
                            city[0] = '\0';
                            cJSON *jc = cJSON_GetObjectItem(root, "city");
                            if (jc && jc->valuestring) strlcpy(city, jc->valuestring, citysz);
                        }
                        cJSON_Delete(root);
                        return true;
                    }
                }
                cJSON_Delete(root);
            }
        }
    }
    /* 备: ip.sb (HTTPS) */
    {
        int n = net_https_get("https://api.ip.sb/geoip", NULL, NULL, NULL, s_buf, sizeof(s_buf));
        if (n > 0) {
            cJSON *root = cJSON_Parse(s_buf);
            if (root) {
                cJSON *j;
                double la = 0, lo = 0;
                if ((j = cJSON_GetObjectItem(root, "latitude")) && cJSON_IsNumber(j)) la = j->valuedouble;
                if ((j = cJSON_GetObjectItem(root, "longitude")) && cJSON_IsNumber(j)) lo = j->valuedouble;
                if (la != 0 || lo != 0) {
                    *lat = (int32_t)(la * 10000);
                    *lon = (int32_t)(lo * 10000);
                    if (city && citysz) {
                        city[0] = '\0';
                        cJSON *jc = cJSON_GetObjectItem(root, "city");
                        if (jc && jc->valuestring) strlcpy(city, jc->valuestring, citysz);
                    }
                    cJSON_Delete(root);
                    return true;
                }
                cJSON_Delete(root);
            }
        }
    }
    return false;
}

static bool geocode(const char *city, int32_t *lat, int32_t *lon) {
    char enc[96], url[192];
    url_encode(city, enc, sizeof(enc));
    snprintf(url, sizeof(url), GEO_URL "?name=%s&count=1&language=zh&format=json", enc);

    int n = net_https_get(url, NULL, NULL, NULL, s_buf, 1024);
    if (n <= 0) return false;

    cJSON *root = cJSON_Parse(s_buf);
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

/* ── 数据源 1: Open-Meteo ── */

static bool fetch_openmeteo(weather_info_t *wx, int32_t lat, int32_t lon) {
    char url[360];
    snprintf(url, sizeof(url),
             OM_URL "?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,relative_humidity_2m,weather_code"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min"
             "&hourly=precipitation_probability,precipitation,weather_code"
             "&forecast_days=%d&forecast_hours=%d&timezone=Asia%%2FShanghai",
             lat / 10000.0, lon / 10000.0, WX_DAYS, RAIN_REQ_H);

    int n = net_https_get(url, NULL, NULL, NULL, s_buf, sizeof(s_buf));
    if (n <= 0) {
        strlcpy(wx->err, "网络/HTTP 失败", sizeof(wx->err));
        return false;
    }
    int dl = net_http_body_decode(s_buf, n, s_dec, sizeof(s_dec));
    if (dl < 0) {
        strlcpy(wx->err, "响应解压失败", sizeof(wx->err));
        return false;
    }
    cJSON *root = cJSON_Parse(s_dec);
    if (!root) {
        ESP_LOGW(TAG, "om: JSON 解析失败, body[%d]: %.200s", dl, s_dec);
        strlcpy(wx->err, "JSON 解析失败", sizeof(wx->err));
        return false;
    }

    bool ok = false;
    cJSON *cur = cJSON_GetObjectItem(root, "current");
    if (cur) {
        cJSON *j;
        if ((j = cJSON_GetObjectItem(cur, "temperature_2m")) && cJSON_IsNumber(j))
            wx->temp_x10 = (int16_t)(j->valuedouble * 10);
        if ((j = cJSON_GetObjectItem(cur, "relative_humidity_2m")) && cJSON_IsNumber(j))
            wx->humidity = (uint8_t)j->valuedouble;
        if ((j = cJSON_GetObjectItem(cur, "weather_code")) && cJSON_IsNumber(j)) {
            uint8_t code = (uint8_t)j->valuedouble;
            strlcpy(wx->text, wmo_text(code), sizeof(wx->text));
            strlcpy(wx->icon, wmo_icon(code), sizeof(wx->icon));
        }
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
                if (c && cJSON_IsNumber(c)) {
                    uint8_t code = (uint8_t)c->valuedouble;
                    strlcpy(wx->dtext[i], wmo_text(code), sizeof(wx->dtext[i]));
                    strlcpy(wx->dicon[i], wmo_icon(code), sizeof(wx->dicon[i]));
                }
                if (mx && cJSON_IsNumber(mx)) wx->tmax_x10[i] = (int16_t)(mx->valuedouble * 10);
                if (mn && cJSON_IsNumber(mn)) wx->tmin_x10[i] = (int16_t)(mn->valuedouble * 10);
            }
            ok = true;
        }
    }
    /* 逐小时: 推算未来几小时降水 */
    cJSON *hourly = cJSON_GetObjectItem(root, "hourly");
    if (hourly) {
        cJSON *probs = cJSON_GetObjectItem(hourly, "precipitation_probability");
        cJSON *amts  = cJSON_GetObjectItem(hourly, "precipitation");
        cJSON *codes = cJSON_GetObjectItem(hourly, "weather_code");
        if (cJSON_IsArray(probs)) {
            hour_wx_t hrs[RAIN_REQ_H];
            memset(hrs, 0, sizeof(hrs));
            int nh = cJSON_GetArraySize(probs);
            if (nh > RAIN_REQ_H) nh = RAIN_REQ_H;
            for (int i = 0; i < nh; i++) {
                cJSON *p = cJSON_GetArrayItem(probs, i);
                cJSON *a = amts ? cJSON_GetArrayItem(amts, i) : NULL;
                cJSON *c = codes ? cJSON_GetArrayItem(codes, i) : NULL;
                if (p && cJSON_IsNumber(p)) hrs[i].prob = (uint8_t)p->valuedouble;
                if (a && cJSON_IsNumber(a)) hrs[i].precip = (a->valuedouble > 0.05);
                if (c && cJSON_IsNumber(c)) {
                    uint8_t code = (uint8_t)c->valuedouble;
                    hrs[i].snow = (code >= 71 && code <= 77) || code == 85 || code == 86;
                    if (!hrs[i].precip) hrs[i].precip = (code >= 51);   /* 有天气码即视为降水 */
                }
            }
            rain_eval(wx, hrs, nh);
        }
    }

    cJSON_Delete(root);
    strlcpy(wx->src, "Open-Meteo", sizeof(wx->src));
    if (!ok) strlcpy(wx->err, "响应缺少数据", sizeof(wx->err));
    return ok;
}

/* ── 数据源 2: 和风天气 (API Host + API Key) ── */

static bool fetch_qweather(weather_info_t *wx, const app_config_t *cfg, int32_t lat, int32_t lon) {
    char url[224];

    /* 实时天气 */
    snprintf(url, sizeof(url), "https://%s/weather/v1/current/%.2f/%.2f?lang=zh",
             cfg->qw_host, lat / 10000.0, lon / 10000.0);
    int n = net_https_get_ex(url, NULL, NULL, NULL, cfg->qw_key, s_buf, sizeof(s_buf));
    if (n <= 0) {
        strlcpy(wx->err, "和风: 网络/认证失败", sizeof(wx->err));
        return false;
    }
    int dl = net_http_body_decode(s_buf, n, s_dec, sizeof(s_dec));
    if (dl < 0) {
        strlcpy(wx->err, "和风: 响应解压失败", sizeof(wx->err));
        return false;
    }
    cJSON *root = cJSON_Parse(s_dec);
    if (!root) {
        ESP_LOGW(TAG, "qw current: JSON 解析失败, body[%d]: %.200s", dl, s_dec);
        strlcpy(wx->err, "和风: JSON 解析失败", sizeof(wx->err));
        return false;
    }
    bool ok = false;
    cJSON *cond = cJSON_GetObjectItem(root, "condition");
    if (cond) {
        cJSON *jt = cJSON_GetObjectItem(cond, "text");
        cJSON *jc = cJSON_GetObjectItem(cond, "code");
        if (jt && jt->valuestring) strlcpy(wx->text, jt->valuestring, sizeof(wx->text));
        if (jc && jc->valuestring) strlcpy(wx->icon, qw_icon(jc->valuestring), sizeof(wx->icon));
        ok = true;
    }
    cJSON *jt = cJSON_GetObjectItem(root, "temperature");
    if (jt) {
        cJSON *v = cJSON_GetObjectItem(jt, "value");
        if (v && cJSON_IsNumber(v)) wx->temp_x10 = (int16_t)(v->valuedouble * 10);
    }
    cJSON *jh = cJSON_GetObjectItem(root, "humidity");      /* 0~1 小数 */
    if (jh && cJSON_IsNumber(jh)) wx->humidity = (uint8_t)(jh->valuedouble * 100 + 0.5);
    cJSON_Delete(root);

    /* 每日预报 */
    snprintf(url, sizeof(url), "https://%s/weather/v1/daily/%.2f/%.2f?days=%d&lang=zh",
             cfg->qw_host, lat / 10000.0, lon / 10000.0, WX_DAYS);
    n = net_https_get_ex(url, NULL, NULL, NULL, cfg->qw_key, s_buf, sizeof(s_buf));
    if (n <= 0) {
        strlcpy(wx->src, "和风天气", sizeof(wx->src));
        if (ok) return true;                                 /* 实时拿到了, 预报失败不算全败 */
        strlcpy(wx->err, "和风: 预报请求失败", sizeof(wx->err));
        return false;
    }
    int dl2 = net_http_body_decode(s_buf, n, s_dec, sizeof(s_dec));
    if (dl2 < 0) {
        strlcpy(wx->src, "和风天气", sizeof(wx->src));
        if (ok) return true;
        strlcpy(wx->err, "和风: 预报解压失败", sizeof(wx->err));
        return false;
    }
    root = cJSON_Parse(s_dec);
    if (!root) {
        ESP_LOGW(TAG, "qw daily: JSON 解析失败, body[%d]: %.200s", dl2, s_dec);
        strlcpy(wx->src, "和风天气", sizeof(wx->src));
        if (ok) return true;
        strlcpy(wx->err, "和风: 预报解析失败", sizeof(wx->err));
        return false;
    }
    cJSON *days = cJSON_GetObjectItem(root, "days");
    if (cJSON_IsArray(days)) {
        int nd = cJSON_GetArraySize(days);
        if (nd > WX_DAYS) nd = WX_DAYS;
        for (int i = 0; i < nd; i++) {
            cJSON *d = cJSON_GetArrayItem(days, i);
            if (!d) continue;
            cJSON *mx = cJSON_GetObjectItem(d, "temperatureMax");
            cJSON *mn = cJSON_GetObjectItem(d, "temperatureMin");
            if (mx) { cJSON *v = cJSON_GetObjectItem(mx, "value");
                      if (v && cJSON_IsNumber(v)) wx->tmax_x10[i] = (int16_t)(v->valuedouble * 10); }
            if (mn) { cJSON *v = cJSON_GetObjectItem(mn, "value");
                      if (v && cJSON_IsNumber(v)) wx->tmin_x10[i] = (int16_t)(v->valuedouble * 10); }
            cJSON *day = cJSON_GetObjectItem(d, "daytime");
            if (day) {
                cJSON *c = cJSON_GetObjectItem(day, "condition");
                if (c) {
                    cJSON *t = cJSON_GetObjectItem(c, "text");
                    cJSON *k = cJSON_GetObjectItem(c, "code");
                    if (t && t->valuestring) strlcpy(wx->dtext[i], t->valuestring, sizeof(wx->dtext[i]));
                    if (k && k->valuestring) strlcpy(wx->dicon[i], qw_icon(k->valuestring), sizeof(wx->dicon[i]));
                }
            }
        }
        ok = true;
    }
    /* 逐小时预报 → 未来几小时降水提醒 */
    snprintf(url, sizeof(url), "https://%s/weather/v1/hourly/%.2f/%.2f?hours=%d&lang=zh",
             cfg->qw_host, lat / 10000.0, lon / 10000.0, RAIN_REQ_H);
    n = net_https_get_ex(url, NULL, NULL, NULL, cfg->qw_key, s_buf, sizeof(s_buf));
    if (n > 0) {
        int dl3 = net_http_body_decode(s_buf, n, s_dec, sizeof(s_dec));
        if (dl3 > 0) {
            cJSON *hroot = cJSON_Parse(s_dec);
            cJSON *hours = hroot ? cJSON_GetObjectItem(hroot, "hours") : NULL;
            if (cJSON_IsArray(hours)) {
                hour_wx_t hrs[RAIN_REQ_H];
                memset(hrs, 0, sizeof(hrs));
                int nh = cJSON_GetArraySize(hours);
                if (nh > RAIN_REQ_H) nh = RAIN_REQ_H;
                for (int i = 0; i < nh; i++) {
                    cJSON *h = cJSON_GetArrayItem(hours, i);
                    if (!h) continue;
                    cJSON *pc = cJSON_GetObjectItem(h, "condition");
                    cJSON *pr = cJSON_GetObjectItem(h, "precipitation");
                    if (pr) {
                        cJSON *prob = cJSON_GetObjectItem(pr, "probability");
                        cJSON *amt  = cJSON_GetObjectItem(pr, "amount");
                        cJSON *typ  = cJSON_GetObjectItem(pr, "type");
                        if (prob && cJSON_IsNumber(prob))
                            hrs[i].prob = (uint8_t)(prob->valuedouble * 100 + 0.5);
                        if (amt) {
                            cJSON *v = cJSON_GetObjectItem(amt, "value");
                            if (v && cJSON_IsNumber(v)) hrs[i].precip = (v->valuedouble > 0.05);
                        }
                        if (typ && typ->valuestring)
                            hrs[i].snow = (strcmp(typ->valuestring, "snow") == 0);
                    }
                    if (pc) {
                        cJSON *k = cJSON_GetObjectItem(pc, "code");
                        if (k && k->valuestring) {
                            int code = atoi(k->valuestring);
                            if (code >= 400 && code <= 499) hrs[i].snow = true;
                            if (code >= 300 && !hrs[i].precip) hrs[i].precip = true;
                        }
                    }
                }
                rain_eval(wx, hrs, nh);
            }
            if (hroot) cJSON_Delete(hroot);
        }
    }

    cJSON_Delete(root);
    strlcpy(wx->src, "和风天气", sizeof(wx->src));
    if (!ok) strlcpy(wx->err, "和风: 无预报数据", sizeof(wx->err));
    return ok;
}

/* ── 一轮天气查询 ── */

void weather_query(app_state_t *st, const app_config_t *cfg) {
    if (!net_query_wifi_ok()) {
        strlcpy(st->wx.err, "WiFi 未连接", sizeof(st->wx.err));
        return;
    }

    bool manual = cfg->wx_city[0] != '\0';
    if (manual) strlcpy(st->wx.city, cfg->wx_city, sizeof(st->wx.city));

    /* 坐标策略: 手动城市 → geocoding 一次永久缓存;
     *          留空 → IP 自动定位, 缓存 24h 每日刷新 (换网络自动跟新) */
    int32_t lat = 0, lon = 0;
    char cached_city[24] = "";
    int64_t wxepo = 0;
    bool have = coords_load(&lat, &lon, cached_city, sizeof(cached_city), &wxepo);

    if (manual) {
        if (!have) {
            if (!geocode(cfg->wx_city, &lat, &lon)) {
                strlcpy(st->wx.err, "城市未找到", sizeof(st->wx.err));
                ESP_LOGW(TAG, "geocode failed for \"%s\"", cfg->wx_city);
                return;
            }
            coords_save(lat, lon, cfg->wx_city);
            ESP_LOGI(TAG, "geocoded \"%s\" -> %.4f, %.4f",
                     cfg->wx_city, lat / 10000.0, lon / 10000.0);
        }
    } else {
        int64_t age = (int64_t)time(NULL) - wxepo;
        if (!have || age < 0 || age > 86400) {
            char ipcity[24] = "";
            if (!ip_locate(&lat, &lon, ipcity, sizeof(ipcity))) {
                strlcpy(st->wx.err, "IP 定位失败", sizeof(st->wx.err));
                ESP_LOGW(TAG, "ip locate failed");
                return;
            }
            coords_save(lat, lon, ipcity);
            ESP_LOGI(TAG, "ip-located -> %s (%.4f, %.4f)", ipcity, lat / 10000.0, lon / 10000.0);
            strlcpy(st->wx.city, ipcity[0] ? ipcity : "自动定位", sizeof(st->wx.city));
        } else if (st->wx.city[0] == '\0') {
            strlcpy(st->wx.city, cached_city[0] ? cached_city : "自动定位", sizeof(st->wx.city));
        }
    }

    /* 选择数据源: 和风参数齐全则用和风, 否则 Open-Meteo */
    bool use_qw = cfg->qw_host[0] && cfg->qw_key[0];
    bool ok = use_qw ? fetch_qweather(&st->wx, cfg, lat, lon)
                     : fetch_openmeteo(&st->wx, lat, lon);

    if (ok) {
        st->wx.valid = true;
        st->wx.err[0] = '\0';
        st->wx.last_ok_ms = (uint32_t)(esp_timer_get_time() / 1000);
        ESP_LOGI(TAG, "[%s] %s: %s %.1f°C 湿%d%% | 明日 %s %.0f/%.0f°C",
                 st->wx.src, st->wx.city, st->wx.text, st->wx.temp_x10 / 10.0, st->wx.humidity,
                 st->wx.dtext[1], st->wx.tmax_x10[1] / 10.0, st->wx.tmin_x10[1] / 10.0);
    } else {
        ESP_LOGW(TAG, "weather failed: %s", st->wx.err);
    }
}
