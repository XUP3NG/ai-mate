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

/* ── 预警等级 ── */
static const char *alert_color_cn(const char *code) {
    if (!code) return "";
    if (!strcmp(code, "red"))    return "红色";
    if (!strcmp(code, "orange")) return "橙色";
    if (!strcmp(code, "yellow")) return "黄色";
    if (!strcmp(code, "blue"))   return "蓝色";
    if (!strcmp(code, "black"))  return "黑色";
    if (!strcmp(code, "white"))  return "白色";
    if (!strcmp(code, "green"))  return "绿色";
    if (!strcmp(code, "gray"))   return "灰色";
    if (!strcmp(code, "purple")) return "紫色";
    if (!strcmp(code, "amber"))  return "琥珀色";
    return "";
}

/* 颜色 → 严重度排序 (用于多条预警时选最严重的一条) */
static int alert_rank(const char *color, const char *severity) {
    int r = 1;
    if (color) {
        if (!strcmp(color, "red"))         r = 6;
        else if (!strcmp(color, "black"))  r = 6;
        else if (!strcmp(color, "purple")) r = 5;
        else if (!strcmp(color, "orange")) r = 4;
        else if (!strcmp(color, "yellow")) r = 3;
        else if (!strcmp(color, "blue"))   r = 2;
    }
    if (severity) {
        if (!strcmp(severity, "extreme"))      { if (r < 6) r = 6; }
        else if (!strcmp(severity, "severe"))  { if (r < 4) r = 4; }
        else if (!strcmp(severity, "moderate")){ if (r < 3) r = 3; }
    }
    return r;
}

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

/* ── 图标: QWeather Icons 字体 (https://icons.qweather.com, MIT) ──
 * 图标命名 = 和风天气代码 (sunny=100 晴, light-rain=305 小雨 …), 码点在 PUA 区 0xF101 起。
 * 生成的字体只含 --range 0xF101-0xF146,0xF21A,0xF2E6 (见 ui/font_qw_*.c)。
 * 映射踩过坑: 3xx/4xx/5xx 的码点**不是**按代码线性排的, 必须查表。
 */
typedef struct { short code; unsigned short cp; } qw_icon_map_t;
static const qw_icon_map_t QW_ICON_MAP[] = {
    /* 1xx 晴/多云/阴 */
    {100,0xF101},{101,0xF102},{102,0xF103},{103,0xF104},{104,0xF105},
    {150,0xF106},{151,0xF107},{152,0xF108},{153,0xF109},
    /* 3xx 雨 (350/351 为夜间阵雨) */
    {300,0xF10A},{301,0xF10B},{302,0xF10C},{303,0xF10D},{304,0xF10E},
    {305,0xF10F},{306,0xF110},{307,0xF111},{308,0xF112},{309,0xF113},
    {310,0xF114},{311,0xF115},{312,0xF116},{313,0xF117},{314,0xF118},
    {315,0xF119},{316,0xF11A},{317,0xF11B},{318,0xF11C},
    {350,0xF11D},{351,0xF11E},{399,0xF11F},
    /* 4xx 雪 (456/457 为夜间阵雪) */
    {400,0xF120},{401,0xF121},{402,0xF122},{403,0xF123},{404,0xF124},
    {405,0xF125},{406,0xF126},{407,0xF127},{408,0xF128},{409,0xF129},
    {410,0xF12A},{456,0xF12B},{457,0xF12C},{499,0xF12D},
    /* 5xx 雾/霾/沙尘 */
    {500,0xF12E},{501,0xF12F},{502,0xF130},{503,0xF132},{504,0xF131},
    {507,0xF134},{508,0xF133},{509,0xF1AD},{510,0xF135},{511,0xF137},
    {512,0xF138},{513,0xF139},{514,0xF13A},{515,0xF13B},
    /* 9xx 冷热 */
    {900,0xF144},{901,0xF145},
};
#define QW_ICON_UNKNOWN 0xF146   /* unknown */
#define QW_ICON_AIRQ    0xF2E6   /* air-quality */
#define QW_ICON_WIND    0xF21A   /* wind */

static int qw_cp(int code) {
    for (unsigned i = 0; i < sizeof(QW_ICON_MAP) / sizeof(QW_ICON_MAP[0]); i++)
        if (QW_ICON_MAP[i].code == code) return QW_ICON_MAP[i].cp;
    return QW_ICON_UNKNOWN;
}

void wx_icon_utf8(int cp, char *out, size_t sz) {
    if (sz < 4) return;
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    out[3] = '\0';
}

const char *wmo_icon(uint8_t code) {
    /* Open-Meteo 的 WMO 代码 → 和风代码 → 图标 */
    static char out[4];
    int q;
    if (code == 0)                     q = 100;
    else if (code == 1 || code == 2)   q = 102;      /* 少云 */
    else if (code == 3)                q = 101;      /* 多云 */
    else if (code == 45 || code == 48) q = 501;      /* 雾 */
    else if (code >= 51 && code <= 55) q = 309;      /* 毛毛雨 */
    else if (code == 56 || code == 57) q = 313;      /* 冻雨 */
    else if (code == 61)               q = 305;
    else if (code == 63)               q = 306;
    else if (code == 65)               q = 307;
    else if (code == 66 || code == 67) q = 313;
    else if (code == 71)               q = 400;
    else if (code == 73)               q = 401;
    else if (code == 75)               q = 402;
    else if (code == 76 || code == 77) q = 499;
    else if (code == 80 || code == 81) q = 300;      /* 阵雨 */
    else if (code == 82)               q = 301;      /* 强阵雨 */
    else if (code == 85 || code == 86) q = 407;      /* 阵雪 */
    else if (code == 95)               q = 302;      /* 雷阵雨 */
    else if (code == 96 || code == 99) q = 304;      /* 雷阵雨伴冰雹 */
    else                               q = 999;
    wx_icon_utf8(qw_cp(q), out, sizeof(out));
    return out;
}

static void qw_icon(const char *code, char *out, size_t sz) {
    wx_icon_utf8(qw_cp(atoi(code ? code : "0")), out, sz);
}

/* 前向声明 (定义在下方"位置绑定"一节) */
static bool loc_load_net(const char *ssid, wx_loc_t *out);
static void loc_save_net(const char *ssid, int32_t la, int32_t lo,
                         const char *city, uint8_t src);

bool wx_loc_copy(const char *from_ssid, const char *to_ssid) {
    wx_loc_t loc;
    if (!from_ssid || !to_ssid || !loc_load_net(from_ssid, &loc)) return false;
    loc_save_net(to_ssid, loc.lat_x1e4, loc.lon_x1e4,
                 loc.city[0] ? loc.city : "未知", WX_LOC_MANUAL);
    ESP_LOGI(TAG, "location copied: \"%s\" → \"%s\" (%.4f, %.4f)",
             from_ssid, to_ssid, loc.lat_x1e4 / 10000.0, loc.lon_x1e4 / 10000.0);
    return true;
}

const char *wx_aqi_icon(void) {
    static char out[4];
    wx_icon_utf8(QW_ICON_AIRQ, out, sizeof(out));
    return out;
}

const char *wx_wind_icon(void) {
    static char out[4];
    wx_icon_utf8(QW_ICON_WIND, out, sizeof(out));
    return out;
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

/* ── 位置绑定: 位置跟着 WiFi 网络走, 永久缓存 ──
 *
 * WiFi 是固定的 → 它所在的位置也是固定的, 所以定位结果按 SSID 永久记住,
 * 不再"每天重查一次"(重查只会让 IP 库偶尔给个邻居市, 城市名和天气跟着跳)。
 *
 * 优先级: 本网络手填坐标 > 本网络城市名解析 > 本网络 IP 自动定位 > 全局手填坐标
 * 存储: NVS ai_hist, 键 al/ao/ac/as + fnv1a(ssid) 8位hex (10 字符, NVS 上限 15);
 *       全局手填用 gml/gmo/gmc (给尚未单独绑定的新网络兜底)。
 */

static uint32_t ssid_hash(const char *ssid) {
    uint32_t h = 2166136261u;                       /* FNV-1a 32 */
    for (const unsigned char *p = (const unsigned char *)ssid; *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }
    return h;
}

static void loc_key(char *dst, size_t sz, char kind, const char *ssid) {
    snprintf(dst, sz, "a%c%08x", kind, (unsigned)ssid_hash(ssid));
}

static bool loc_load_net(const char *ssid, wx_loc_t *out) {
    if (!ssid || !ssid[0]) return false;
    nvs_handle_t h;
    if (nvs_open(WX_NS, NVS_READONLY, &h) != ESP_OK) return false;

    char k[16];
    int32_t la = 0, lo = 0;
    loc_key(k, sizeof(k), 'l', ssid);
    bool ok = (nvs_get_i32(h, k, &la) == ESP_OK);
    loc_key(k, sizeof(k), 'o', ssid);
    ok = ok && (nvs_get_i32(h, k, &lo) == ESP_OK);
    if (ok) {
        memset(out, 0, sizeof(*out));
        out->lat_x1e4 = la;
        out->lon_x1e4 = lo;
        size_t sz = sizeof(out->city);
        loc_key(k, sizeof(k), 'c', ssid);
        if (nvs_get_str(h, k, out->city, &sz) != ESP_OK) out->city[0] = '\0';
        uint8_t src = WX_LOC_AUTO;
        loc_key(k, sizeof(k), 's', ssid);
        nvs_get_u8(h, k, &src);
        out->src = src;
        out->from_global = false;
    }
    nvs_close(h);
    return ok;
}

static bool loc_load_global(wx_loc_t *out) {
    nvs_handle_t h;
    if (nvs_open(WX_NS, NVS_READONLY, &h) != ESP_OK) return false;
    int32_t la = 0, lo = 0;
    bool ok = (nvs_get_i32(h, "gml", &la) == ESP_OK) &&
              (nvs_get_i32(h, "gmo", &lo) == ESP_OK);
    if (ok) {
        memset(out, 0, sizeof(*out));
        out->lat_x1e4 = la;
        out->lon_x1e4 = lo;
        size_t sz = sizeof(out->city);
        if (nvs_get_str(h, "gmc", out->city, &sz) != ESP_OK) out->city[0] = '\0';
        out->src = WX_LOC_MANUAL;
        out->from_global = true;
    }
    nvs_close(h);
    return ok;
}

static void loc_save_net(const char *ssid, int32_t la, int32_t lo,
                         const char *city, uint8_t src) {
    if (!ssid || !ssid[0]) return;
    nvs_handle_t h;
    if (nvs_open(WX_NS, NVS_READWRITE, &h) != ESP_OK) return;
    char k[16];
    loc_key(k, sizeof(k), 'l', ssid); nvs_set_i32(h, k, la);
    loc_key(k, sizeof(k), 'o', ssid); nvs_set_i32(h, k, lo);
    loc_key(k, sizeof(k), 'c', ssid); nvs_set_str(h, k, (city && city[0]) ? city : "");
    loc_key(k, sizeof(k), 's', ssid); nvs_set_u8(h, k, src);
    nvs_commit(h);
    nvs_close(h);
}

static void loc_erase_net(const char *ssid) {
    if (!ssid || !ssid[0]) return;
    nvs_handle_t h;
    if (nvs_open(WX_NS, NVS_READWRITE, &h) != ESP_OK) return;
    char k[16];
    loc_key(k, sizeof(k), 'l', ssid); nvs_erase_key(h, k);
    loc_key(k, sizeof(k), 'o', ssid); nvs_erase_key(h, k);
    loc_key(k, sizeof(k), 'c', ssid); nvs_erase_key(h, k);
    loc_key(k, sizeof(k), 's', ssid); nvs_erase_key(h, k);
    nvs_commit(h);
    nvs_close(h);
}

static void loc_save_global(int32_t la, int32_t lo, const char *city) {
    nvs_handle_t h;
    if (nvs_open(WX_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "gml", la);
    nvs_set_i32(h, "gmo", lo);
    nvs_set_str(h, "gmc", (city && city[0]) ? city : "");
    nvs_commit(h);
    nvs_close(h);
}

static void loc_erase_global(void) {
    nvs_handle_t h;
    if (nvs_open(WX_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, "gml");
    nvs_erase_key(h, "gmo");
    nvs_erase_key(h, "gmc");
    nvs_commit(h);
    nvs_close(h);
}

bool wx_loc_peek(const char *ssid, wx_loc_t *out) {
    memset(out, 0, sizeof(*out));
    if (loc_load_net(ssid, out)) return true;
    return loc_load_global(out);
}

void wx_loc_set_manual(const char *ssid, const app_config_t *cfg,
                       int32_t la, int32_t lo, const char *city, bool all) {
    if (all && cfg) {                               /* 所有已保存网络都在同一地点 */
        for (int i = 0; i < cfg->net_count && i < CFG_NET_MAX; i++)
            loc_save_net(cfg->net_ssid[i], la, lo, city, WX_LOC_MANUAL);
        loc_save_global(la, lo, city);              /* 兜底: 以后新加的网络 */
        if (ssid && ssid[0]) loc_save_net(ssid, la, lo, city, WX_LOC_MANUAL);
    } else if (ssid && ssid[0]) {
        loc_save_net(ssid, la, lo, city, WX_LOC_MANUAL);
    } else {
        loc_save_global(la, lo, city);              /* 没有当前网络 → 只能存全局 */
    }
    ESP_LOGI(TAG, "manual location set: %.4f, %.4f (%s)", la / 10000.0, lo / 10000.0,
             all ? "all networks" : (ssid && ssid[0] ? ssid : "global"));
}

void wx_loc_clear_manual(const char *ssid, const app_config_t *cfg, bool all) {
    if (all && cfg) {
        for (int i = 0; i < cfg->net_count && i < CFG_NET_MAX; i++) loc_erase_net(cfg->net_ssid[i]);
        loc_erase_global();
        if (ssid && ssid[0]) loc_erase_net(ssid);
    } else if (ssid && ssid[0]) {
        loc_erase_net(ssid);
    } else {
        loc_erase_global();
    }
    ESP_LOGI(TAG, "manual location cleared (%s)",
             all ? "all networks" : (ssid && ssid[0] ? ssid : "global"));
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

/* m/s → 蒲福风级 (GB/T 35221 阈值) */
static uint8_t ms_to_beaufort(double ms) {
    static const double t[] = {0.3,1.6,3.4,5.5,8.0,10.8,13.9,17.2,20.8,24.5,28.5,32.7};
    for (int i = 0; i < 12; i++) if (ms < t[i]) return (uint8_t)i;
    return 12;
}

static bool fetch_openmeteo(weather_info_t *wx, int32_t lat, int32_t lon) {
    char url[360];
    snprintf(url, sizeof(url),
             OM_URL "?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,relative_humidity_2m,weather_code,apparent_temperature,wind_speed_10m"
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
        if ((j = cJSON_GetObjectItem(cur, "apparent_temperature")) && cJSON_IsNumber(j)) {
            wx->feels_x10 = (int16_t)(j->valuedouble * 10);
            wx->feels_valid = true;
        }
        if ((j = cJSON_GetObjectItem(cur, "wind_speed_10m")) && cJSON_IsNumber(j)) {
            wx->wind_scale = ms_to_beaufort(j->valuedouble);   /* m/s → 蒲福风级 */
            wx->wind_valid = true;
        }
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

/* ── 天气预警 (和风) ── */
static void fetch_qw_alert(weather_info_t *wx, const app_config_t *cfg, int32_t lat, int32_t lon) {
    char url[224];
    snprintf(url, sizeof(url), "https://%s/weatheralert/v1/current/%.2f/%.2f?lang=zh",
             cfg->qw_host, lat / 10000.0, lon / 10000.0);

    int n = net_https_get_ex(url, NULL, NULL, NULL, cfg->qw_key, s_buf, sizeof(s_buf));
    if (n <= 0) return;
    int dl = net_http_body_decode(s_buf, n, s_dec, sizeof(s_dec));
    if (dl <= 0) return;

    cJSON *root = cJSON_Parse(s_dec);
    if (!root) {
        ESP_LOGW(TAG, "qw alert: JSON 解析失败, body[%d]: %.160s", dl, s_dec);
        return;
    }

    wx->alert.valid = true;                       /* 拿到响应即视为有效 (可为"无预警") */
    cJSON *alerts = cJSON_GetObjectItem(root, "alerts");
    if (!cJSON_IsArray(alerts)) { cJSON_Delete(root); return; }

    int cnt = cJSON_GetArraySize(alerts);
    if (cnt <= 0) { cJSON_Delete(root); return; }

    /* 选最严重的一条作为横幅内容 */
    int best_rank = -1;
    const char *best_name = NULL, *best_color = NULL;
    for (int i = 0; i < cnt; i++) {
        cJSON *a = cJSON_GetArrayItem(alerts, i);
        if (!a) continue;
        cJSON *et = cJSON_GetObjectItem(a, "eventType");
        cJSON *col = cJSON_GetObjectItem(a, "color");
        cJSON *sev = cJSON_GetObjectItem(a, "severity");
        cJSON *jnm = et ? cJSON_GetObjectItem(et, "name") : NULL;
        cJSON *jcc = col ? cJSON_GetObjectItem(col, "code") : NULL;
        const char *nm = (jnm && jnm->valuestring) ? jnm->valuestring : NULL;
        const char *cc = (jcc && jcc->valuestring) ? jcc->valuestring : NULL;
        const char *sv = (sev && sev->valuestring) ? sev->valuestring : NULL;
        int r = alert_rank(cc, sv);
        if (r > best_rank) {
            best_rank = r;
            best_name = nm ? nm : "天气";
            best_color = cc ? cc : "";
        }
    }

    wx->alert.count = (uint8_t)(cnt > 99 ? 99 : cnt);
    wx->alert.severe = (best_rank >= 4);          /* 橙及以上 → 反白强调 */
    snprintf(wx->alert.title, sizeof(wx->alert.title), "%s%s预警",
             best_name ? best_name : "天气", alert_color_cn(best_color));
    cJSON_Delete(root);

    ESP_LOGW(TAG, "alert: %s (共 %u 条, %s)", wx->alert.title,
             wx->alert.count, wx->alert.severe ? "严重" : "一般");
}

/* ── 空气质量 (和风, indexes[] 里优先取中国标准 chn) ── */
static void fetch_qw_air(weather_info_t *wx, const app_config_t *cfg, int32_t lat, int32_t lon) {
    char url[224];
    snprintf(url, sizeof(url), "https://%s/airquality/v1/current/%.2f/%.2f?lang=zh",
             cfg->qw_host, lat / 10000.0, lon / 10000.0);

    int n = net_https_get_ex(url, NULL, NULL, NULL, cfg->qw_key, s_buf, sizeof(s_buf));
    if (n <= 0) return;
    int dl = net_http_body_decode(s_buf, n, s_dec, sizeof(s_dec));
    if (dl <= 0) return;

    cJSON *root = cJSON_Parse(s_dec);
    if (!root) {
        ESP_LOGW(TAG, "qw air: JSON 解析失败, body[%d]: %.160s", dl, s_dec);
        return;
    }

    cJSON *idxs = cJSON_GetObjectItem(root, "indexes");
    if (cJSON_IsArray(idxs)) {
        int cnt = cJSON_GetArraySize(idxs);
        cJSON *pick = NULL;
        for (int i = 0; i < cnt; i++) {
            cJSON *e = cJSON_GetArrayItem(idxs, i);
            if (!e) continue;
            cJSON *c = cJSON_GetObjectItem(e, "code");
            if (c && c->valuestring && strcmp(c->valuestring, "chn") == 0) { pick = e; break; }
            if (!pick) pick = e;                  /* 没有中国标准就退回第一条 */
        }
        if (pick) {
            cJSON *ja = cJSON_GetObjectItem(pick, "aqi");
            cJSON *jc = cJSON_GetObjectItem(pick, "category");
            if (ja && cJSON_IsNumber(ja)) {
                double v = ja->valuedouble;
                wx->aqi = (uint16_t)(v < 0 ? 0 : (v > 999 ? 999 : v + 0.5));
                wx->aqi_valid = true;
            }
            if (jc && jc->valuestring)
                strlcpy(wx->aqi_cat, jc->valuestring, sizeof(wx->aqi_cat));
        }
    }
    cJSON_Delete(root);
    if (wx->aqi_valid) ESP_LOGI(TAG, "air: AQI %u %s", wx->aqi, wx->aqi_cat);
}

/* ── 分钟级降水 (和风, 未来 2 小时) ── */
static void fetch_qw_minutely(weather_info_t *wx, const app_config_t *cfg, int32_t lat, int32_t lon) {
    char url[224];
    snprintf(url, sizeof(url), "https://%s/v7/minutely/5m?location=%.2f,%.2f&lang=zh",
             cfg->qw_host, lon / 10000.0, lat / 10000.0);

    int n = net_https_get_ex(url, NULL, NULL, NULL, cfg->qw_key, s_buf, sizeof(s_buf));
    if (n <= 0) return;
    int dl = net_http_body_decode(s_buf, n, s_dec, sizeof(s_dec));
    if (dl <= 0) return;

    cJSON *root = cJSON_Parse(s_dec);
    if (!root) {
        ESP_LOGW(TAG, "qw minutely: JSON 解析失败, body[%d]: %.160s", dl, s_dec);
        return;
    }

    cJSON *sum = cJSON_GetObjectItem(root, "summary");
    if (sum && sum->valuestring)
        strlcpy(wx->minutely.summary, sum->valuestring, sizeof(wx->minutely.summary));

    cJSON *arr = cJSON_GetObjectItem(root, "minutely");
    if (cJSON_IsArray(arr)) {
        int cnt = cJSON_GetArraySize(arr);
        if (cnt > WX_MIN_N) cnt = WX_MIN_N;

        /* 先取峰值, 再归一化 */
        double vals[WX_MIN_N];
        double peak = 0;
        for (int i = 0; i < cnt; i++) {
            cJSON *e = cJSON_GetArrayItem(arr, i);
            cJSON *p = e ? cJSON_GetObjectItem(e, "precip") : NULL;
            vals[i] = (p && p->valuestring) ? atof(p->valuestring) : 0;
            if (vals[i] > peak) peak = vals[i];
        }
        wx->minutely.peak_x100 = (uint16_t)(peak * 100 + 0.5);
        double scale = peak > 0.05 ? peak : 0.05;      /* 极小值也给个底, 避免噪声满格 */
        for (int i = 0; i < WX_MIN_N; i++) {
            double v = (i < cnt) ? vals[i] : 0;
            int h = (int)(v / scale * 100.0 + 0.5);
            wx->minutely.bar[i] = (uint8_t)(h > 100 ? 100 : h);
        }
        wx->minutely.valid = true;
        ESP_LOGI(TAG, "minutely: \"%s\", 24 格峰值 %.2f mm/5min",
                 wx->minutely.summary, peak);
    }
    cJSON_Delete(root);
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
        if (jc && jc->valuestring) qw_icon(jc->valuestring, wx->icon, sizeof(wx->icon));
        ok = true;
    }
    cJSON *jt = cJSON_GetObjectItem(root, "temperature");
    if (jt) {
        cJSON *v = cJSON_GetObjectItem(jt, "value");
        if (v && cJSON_IsNumber(v)) wx->temp_x10 = (int16_t)(v->valuedouble * 10);
    }
    cJSON *jh = cJSON_GetObjectItem(root, "humidity");      /* 0~1 小数 */
    if (jh && cJSON_IsNumber(jh)) wx->humidity = (uint8_t)(jh->valuedouble * 100 + 0.5);
    cJSON *jf = cJSON_GetObjectItem(root, "feelsLike");     /* 体感温度 (同响应, 零额外请求) */
    if (jf) {
        cJSON *v = cJSON_GetObjectItem(jf, "value");
        if (v && cJSON_IsNumber(v)) {
            wx->feels_x10 = (int16_t)(v->valuedouble * 10);
            wx->feels_valid = true;
        }
    }
    cJSON *jw = cJSON_GetObjectItem(root, "wind");          /* 风: scale = 蒲福风级 */
    if (jw) {
        cJSON *js = cJSON_GetObjectItem(jw, "scale");
        if (js && cJSON_IsNumber(js)) {
            wx->wind_scale = (uint8_t)js->valuedouble;
            wx->wind_valid = true;
        }
    }
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
                    if (k && k->valuestring) qw_icon(k->valuestring, wx->dicon[i], sizeof(wx->dicon[i]));
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

    /* 预警 + 分钟级降水 + 空气质量 (和风独有, 失败不影响主数据) */
    fetch_qw_alert(wx, cfg, lat, lon);
    fetch_qw_minutely(wx, cfg, lat, lon);
    fetch_qw_air(wx, cfg, lat, lon);

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

    bool named = cfg->wx_city[0] != '\0';
    if (named) strlcpy(st->wx.city, cfg->wx_city, sizeof(st->wx.city));

    /* 坐标策略: 手填坐标(最高) > 城市名解析 > 本网络 IP 定位(永久) > 全局手填坐标 */
    const char *ssid = wifi_mgr_current_ssid();
    int32_t lat = 0, lon = 0;
    wx_loc_t loc;
    bool have = false;

    if (loc_load_net(ssid, &loc)) {
        if (loc.src == WX_LOC_MANUAL) {
            have = true;                            /* 配网页手填 → 永远优先 */
        } else if (named) {
            /* 城市名模式: 名字没变就复用, 改了名字自动重新解析 */
            have = (loc.src == WX_LOC_GEOC) && (strcmp(loc.city, cfg->wx_city) == 0);
        } else if (loc.src == WX_LOC_AUTO) {
            have = true;                            /* IP 结果永久有效, 不再每日重查 */
        }
    }
    if (!have && loc_load_global(&loc)) have = true;

    if (have) {
        lat = loc.lat_x1e4;
        lon = loc.lon_x1e4;
        if (!named && loc.city[0]) strlcpy(st->wx.city, loc.city, sizeof(st->wx.city));
        static const char *SRC[] = { "IP 自动", "城市名解析", "手填坐标" };
        ESP_LOGI(TAG, "location cached: %.4f, %.4f (%s%s) ssid \"%s\"",
                 lat / 10000.0, lon / 10000.0,
                 SRC[loc.src < 3 ? loc.src : 0], loc.from_global ? ", 全局" : "",
                 ssid ? ssid : "");
    } else if (named) {
        if (!geocode(cfg->wx_city, &lat, &lon)) {
            strlcpy(st->wx.err, "城市未找到", sizeof(st->wx.err));
            ESP_LOGW(TAG, "geocode failed for \"%s\"", cfg->wx_city);
            return;
        }
        loc_save_net(ssid, lat, lon, cfg->wx_city, WX_LOC_GEOC);
        ESP_LOGI(TAG, "geocoded \"%s\" -> %.4f, %.4f (ssid \"%s\")",
                 cfg->wx_city, lat / 10000.0, lon / 10000.0, ssid ? ssid : "");
    } else {
        char ipcity[24] = "";
        if (!ip_locate(&lat, &lon, ipcity, sizeof(ipcity))) {
            strlcpy(st->wx.err, "IP 定位失败", sizeof(st->wx.err));
            ESP_LOGW(TAG, "ip locate failed");
            return;
        }
        loc_save_net(ssid, lat, lon, ipcity, WX_LOC_AUTO);
        ESP_LOGI(TAG, "ip-located -> %s (%.4f, %.4f) ssid \"%s\" [已永久绑定]",
                 ipcity, lat / 10000.0, lon / 10000.0, ssid ? ssid : "");
        strlcpy(st->wx.city, ipcity[0] ? ipcity : "自动定位", sizeof(st->wx.city));
    }

    /* 手填坐标但没填城市名 → 用这个占位, 别显示成"未知位置" */
    if (st->wx.city[0] == '\0') strlcpy(st->wx.city, "自定义位置", sizeof(st->wx.city));

    /* 选择数据源: 和风参数齐全则用和风, 否则 Open-Meteo */
    bool use_qw = cfg->qw_host[0] && cfg->qw_key[0];
    bool ok = use_qw ? fetch_qweather(&st->wx, cfg, lat, lon)
                     : fetch_openmeteo(&st->wx, lat, lon);

    if (ok) {
        st->wx.valid = true;
        st->wx.err[0] = '\0';
        st->wx.last_ok_ms = (uint32_t)(esp_timer_get_time() / 1000);
        char feel[24] = "", air[40] = "";
        if (st->wx.feels_valid) snprintf(feel, sizeof(feel), " 体感%.1f°C", st->wx.feels_x10 / 10.0);
        if (st->wx.aqi_valid)   snprintf(air, sizeof(air), " 空气%s%u", st->wx.aqi_cat, st->wx.aqi);
        ESP_LOGI(TAG, "[%s] %s: %s %.1f°C 湿%d%%%s%s | 明日 %s %.0f/%.0f°C",
                 st->wx.src, st->wx.city, st->wx.text, st->wx.temp_x10 / 10.0, st->wx.humidity,
                 feel, air,
                 st->wx.dtext[1], st->wx.tmax_x10[1] / 10.0, st->wx.tmin_x10[1] / 10.0);
    } else {
        ESP_LOGW(TAG, "weather failed: %s", st->wx.err);
    }
}
