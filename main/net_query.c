#include "net_query.h"
#include "wifi_mgr.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sntp.h"
#include "nvs.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "net";

#define GLM_URL_BASE  "https://bigmodel.cn/api/monitor/usage/quota/limit"
#define DSK_URL       "https://api.deepseek.com/user/balance"

/* ── SNTP ── */
static void sntp_cb(struct timeval *tv) { (void)tv; }

void net_query_init_time(void) {
    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(sntp_cb);
    esp_sntp_init();
}

bool net_query_wifi_ok(void) { return wifi_is_connected(); }

/* ── HTTP GET 工具 ── */
#define HTTP_BUF 3072

static int https_get(const char *url, const char *hdr_auth, const char *hdr_org,
                     const char *hdr_proj, char *buf, size_t bufsz) {
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 12000,
    };
    esp_http_client_handle_t cl = esp_http_client_init(&cfg);
    if (!cl) return -1;
    if (hdr_auth) esp_http_client_set_header(cl, "Authorization", hdr_auth);
    if (hdr_org)  esp_http_client_set_header(cl, "bigmodel-organization", hdr_org);
    if (hdr_proj) esp_http_client_set_header(cl, "bigmodel-project", hdr_proj);
    esp_http_client_set_header(cl, "Accept-Encoding", "identity");
    esp_http_client_set_header(cl, "Accept", "application/json, text/plain, */*");
    esp_http_client_set_header(cl, "User-Agent", "Mozilla/5.0 (compatible; AI-Mate/1.0)");

    esp_err_t err = esp_http_client_open(cl, 0);
    int status = -1, n = 0;
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(cl);
        /* 统一流式读取: 兼容 Content-Length 与 chunked (返回 0) 两种情况 */
        while (n < (int)bufsz - 1) {
            int r = esp_http_client_read(cl, buf + n, bufsz - 1 - n);
            if (r <= 0) break;
            n += r;
        }
        buf[n] = '\0';
        status = esp_http_client_get_status_code(cl);
    }
    esp_http_client_close(cl);
    esp_http_client_cleanup(cl);
    if (status != 200) {
        ESP_LOGW(TAG, "GET %s -> %d", url, status);
        return -2;
    }
    return n;
}

/* ── 智谱 GLM Coding Plan ── */

static void parse_glm_window(cJSON *lim, glm_window_t *w) {
    cJSON *j;
    if ((j = cJSON_GetObjectItem(lim, "percentage"))) {
        double p = cJSON_IsNumber(j) ? j->valuedouble : 0;
        w->pct = (uint8_t)(p > 100 ? 100 : (p < 0 ? 0 : p));
    }
    if ((j = cJSON_GetObjectItem(lim, "usage")) && cJSON_IsNumber(j)) w->usage = (uint32_t)j->valuedouble;
    if ((j = cJSON_GetObjectItem(lim, "currentValue")) && cJSON_IsNumber(j)) w->current = (uint32_t)j->valuedouble;
    if ((j = cJSON_GetObjectItem(lim, "remaining")) && cJSON_IsNumber(j)) w->remaining = (uint32_t)j->valuedouble;
    if ((j = cJSON_GetObjectItem(lim, "nextResetTime")) && cJSON_IsNumber(j)) w->reset_ms = (int64_t)j->valuedouble;
    w->valid = true;
}

static void query_glm(app_state_t *st, const app_config_t *cfg) {
    if (!cfg->glm_key[0]) {
        strlcpy(st->glm.err, "未配置智谱 Key", sizeof(st->glm.err));
        return;
    }
    char url[128];
    snprintf(url, sizeof(url), GLM_URL_BASE "?type=%d", cfg->glm_type);

    static char buf[HTTP_BUF];
    char auth[110];
    snprintf(auth, sizeof(auth), "%s", cfg->glm_key);   /* raw key, 无 Bearer */

    int n = https_get(url, auth, cfg->glm_org, cfg->glm_project, buf, sizeof(buf));
    if (n < 0) {
        strlcpy(st->glm.err, "网络/HTTP 失败", sizeof(st->glm.err));
        return;
    }

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        ESP_LOGW(TAG, "glm: JSON parse failed, body[%d]: %.160s", n, buf);
        strlcpy(st->glm.err, "JSON 解析失败", sizeof(st->glm.err));
        return;
    }
    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *limits = data ? cJSON_GetObjectItem(data, "limits") : NULL;
    if (!cJSON_IsArray(limits)) {
        strlcpy(st->glm.err, "无 limits (检查 org/proj/type)", sizeof(st->glm.err));
        cJSON_Delete(root);
        return;
    }

    cJSON *lim;
    cJSON_ArrayForEach(lim, limits) {
        cJSON *unit = cJSON_GetObjectItem(lim, "unit");
        if (!cJSON_IsNumber(unit)) continue;
        if (unit->valueint == 3) parse_glm_window(lim, &st->glm.win_5h);
        else if (unit->valueint == 6) parse_glm_window(lim, &st->glm.win_w);
    }

    if (st->glm.win_5h.valid || st->glm.win_w.valid) {
        st->glm.valid = true;
        st->glm.err[0] = '\0';
        st->glm.last_ok_ms = (uint32_t)(esp_timer_get_time() / 1000);
    } else {
        strlcpy(st->glm.err, "套餐无数据", sizeof(st->glm.err));
    }
    cJSON_Delete(root);
}

/* ── DeepSeek 余额 + 消费历史推算 ──
 *
 * 模型 (quote0-deepseek-balance 的思路):
 *   每日消费 = 当日开盘余额 − 当日收盘余额 + 当日充值
 *   充值检测: 两次快照间隔 < 35min 且余额跳增 ≥ 8 元 → 记为充值
 *   设备离线跨多天 → 中间天记 -1 (无数据)
 *
 * NVS (命名空间 ai_hist):
 *   base   = cents[0] 对应日期 (YYYYMMDD)
 *   cents  = int32[HIST_DAYS] 每日消费(分), -1=无数据, 末位=今天(实时)
 *   dbase  = 今日开盘余额 (分)
 *   rechg  = 今日累计充值 (分)
 *   lbal   = 最近一次快照余额 (分)
 *   lepo   = 最近一次快照 epoch
 */

#define HIST_NS         "ai_hist"
#define RECHARGE_JUMP_C 800
#define GAP_RECH_SEC    (35 * 60)
#define GAP_MAX_SEC     (36 * 3600)

static int32_t date_key(struct tm *tm) {
    return (tm->tm_year + 1900) * 10000 + (tm->tm_mon + 1) * 100 + tm->tm_mday;
}

/* YYYYMMDD ↔ 天数序号 (Howard Hinnant 算法, 1970-01-01 = 0)
 * 正反变换必须成对使用同一套公式, 否则回推日期会错。 */
static int32_t day_no(int32_t key) {
    int y = key / 10000;
    int m = (key / 100) % 100;
    int d = key % 100;
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;                                     /* [0,399] */
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;     /* [0,365] */
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;             /* [0,146096] */
    return era * 146097 + doe - 719468;
}

/* 天数序号 → YYYYMMDD */
static int32_t key_no(int32_t z) {
    z += 719468;
    int era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = z - era * 146097;                                   /* [0,146096] */
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = yoe + era * 400;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);            /* [0,365] */
    int mp = (5 * doy + 2) / 153;                                 /* [0,11] */
    int d = doy - (153 * mp + 2) / 5 + 1;                         /* [1,31] */
    int m = mp + (mp < 10 ? 3 : -9);                              /* [1,12] */
    y += (m <= 2);
    return y * 10000 + m * 100 + d;
}

static void query_dsk(app_state_t *st, const app_config_t *cfg) {
    if (!cfg->dsk_key[0]) {
        strlcpy(st->dsk.err, "未配置 DeepSeek Key", sizeof(st->dsk.err));
        return;
    }
    static char buf[HTTP_BUF];
    char auth[110];
    snprintf(auth, sizeof(auth), "Bearer %s", cfg->dsk_key);

    int n = https_get(DSK_URL, auth, NULL, NULL, buf, sizeof(buf));
    if (n < 0) {
        strlcpy(st->dsk.err, "网络/HTTP 失败", sizeof(st->dsk.err));
        return;
    }
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        strlcpy(st->dsk.err, "JSON 解析失败", sizeof(st->dsk.err));
        return;
    }
    cJSON *avail = cJSON_GetObjectItem(root, "is_available");
    cJSON *bifs = cJSON_GetObjectItem(root, "balance_infos");
    if (!cJSON_IsArray(bifs)) {
        strlcpy(st->dsk.err, "Key 无效或无余额数据", sizeof(st->dsk.err));
        cJSON_Delete(root);
        return;
    }
    cJSON *bi = cJSON_GetArrayItem(bifs, 0);
    if (bi) {
        cJSON *j;
        if ((j = cJSON_GetObjectItem(bi, "total_balance")))
            st->dsk.total = atof(j->valuestring ? j->valuestring : "0");
        if ((j = cJSON_GetObjectItem(bi, "granted_balance")))
            st->dsk.granted = atof(j->valuestring ? j->valuestring : "0");
        if ((j = cJSON_GetObjectItem(bi, "topped_up_balance")))
            st->dsk.topped = atof(j->valuestring ? j->valuestring : "0");
        if ((j = cJSON_GetObjectItem(bi, "currency")))
            strlcpy(st->dsk.currency, j->valuestring ? j->valuestring : "CNY", sizeof(st->dsk.currency));
        st->dsk.is_available = cJSON_IsTrue(avail);
        st->dsk.valid = true;
        st->dsk.err[0] = '\0';
        st->dsk.last_ok_ms = (uint32_t)(esp_timer_get_time() / 1000);
    }
    cJSON_Delete(root);

    /* ── 消费历史推算 ── */
    if (!st->dsk.valid || !st->time_valid) return;

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    int32_t today_key = date_key(&tm_now);
    int32_t bal_c = (int32_t)(st->dsk.total * 100 + (st->dsk.total >= 0 ? 0.5 : -0.5));

    /* 读取持久状态 */
    int32_t cents[HIST_DAYS], base_date = 0, dbase = 0, rechg = 0, lbal = 0;
    int64_t lepo = 0;
    for (int i = 0; i < HIST_DAYS; i++) cents[i] = -1;
    {
        nvs_handle_t h;
        if (nvs_open(HIST_NS, NVS_READONLY, &h) == ESP_OK) {
            size_t sz = HIST_DAYS * 4;
            nvs_get_blob(h, "cents", cents, &sz);
            nvs_get_i32(h, "base", &base_date);
            nvs_get_i32(h, "dbase", &dbase);
            nvs_get_i32(h, "rechg", &rechg);
            nvs_get_i32(h, "lbal", &lbal);
            nvs_get_i64(h, "lepo", &lepo);
            nvs_close(h);
        }
    }

    if (base_date == 0 || lbal == 0) {
        /* 首次快照: 以今天开盘 */
        base_date = today_key;
        dbase = bal_c;
        rechg = 0;
    } else {
        int diff = day_no(today_key) - day_no(base_date);
        if (diff < 0) {
            /* 时钟回拨, 忽略 */
        } else if (diff > 0) {
            int64_t gap = (int64_t)now - lepo;
            /* 结算昨天: 开盘 - 收盘 + 充值。写到最后一位, 左移后即变"昨天" */
            if (diff == 1 && gap <= GAP_MAX_SEC) {
                int32_t cost = dbase - lbal + rechg;
                cents[HIST_DAYS - 1] = (cost >= 0) ? cost : -1;
            }
            /* 中间离线的天数记 -1 (已通过左移覆盖) */
            int shift = diff > HIST_DAYS ? HIST_DAYS : diff;
            for (int i = 0; i + shift < HIST_DAYS; i++) cents[i] = cents[i + shift];
            for (int i = HIST_DAYS - shift; i < HIST_DAYS; i++) cents[i] = -1;
            base_date = today_key;
            dbase = bal_c;
            rechg = 0;
        } else {
            /* 同日: 充值检测 (短间隔跳增) */
            int64_t gap = (int64_t)now - lepo;
            if (gap > 60 && gap < GAP_RECH_SEC && bal_c - lbal >= RECHARGE_JUMP_C) {
                rechg += bal_c - lbal;
                ESP_LOGI(TAG, "recharge: +%.2f", (bal_c - lbal) / 100.0);
            }
        }
    }

    /* 今天实时消费 = 开盘 − 当前 + 充值 */
    int32_t today_cost = dbase - bal_c + rechg;
    cents[HIST_DAYS - 1] = today_cost > 0 ? today_cost : 0;

    /* 持久化 */
    {
        nvs_handle_t h;
        if (nvs_open(HIST_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_blob(h, "cents", cents, HIST_DAYS * 4);
            nvs_set_i32(h, "base", base_date);
            nvs_set_i32(h, "dbase", dbase);
            nvs_set_i32(h, "rechg", rechg);
            nvs_set_i32(h, "lbal", bal_c);
            nvs_set_i64(h, "lepo", (int64_t)now);
            nvs_commit(h);
            nvs_close(h);
        }
    }

    /* 回填 UI 状态 */
    hist_info_t *hi = &st->hist;
    memcpy(hi->cents, cents, sizeof(cents));
    hi->year = base_date / 10000;
    hi->month = (base_date / 100) % 100;
    hi->day = base_date % 100;
    hi->count = HIST_DAYS;
    hi->today_cents = cents[HIST_DAYS - 1];

    /* 本月消费: 日期落在当前月份的天求和 (cents 末位=今天, 向前推日期) */
    int32_t month_c = 0;
    int bn_today = day_no(today_key);
    for (int i = 0; i < HIST_DAYS; i++) {
        if (cents[i] <= 0) continue;
        int32_t key = key_no(bn_today - (HIST_DAYS - 1 - i));
        if ((key / 100) % 100 == (int32_t)tm_now.tm_mon + 1 && key / 10000 == tm_now.tm_year + 1900)
            month_c += cents[i];
    }
    hi->month_cents = month_c;
}

/* ── 一轮完整查询 ── */
void net_query_poll(app_state_t *state, const app_config_t *cfg) {
    if (!net_query_wifi_ok()) {
        strlcpy(state->glm.err, "WiFi 未连接", sizeof(state->glm.err));
        strlcpy(state->dsk.err, "WiFi 未连接", sizeof(state->dsk.err));
        return;
    }
    state->time_valid = (time(NULL) > 1700000000);
    query_glm(state, cfg);
    query_dsk(state, cfg);

    /* ── 数据自检日志 (便于核对数值是否合理) ── */
    if (state->glm.valid) {
        ESP_LOGI(TAG, "GLM  5h: %u%%  已用 %u / 总 %u  剩 %u",
                 state->glm.win_5h.pct, (unsigned)state->glm.win_5h.current,
                 (unsigned)state->glm.win_5h.usage, (unsigned)state->glm.win_5h.remaining);
        ESP_LOGI(TAG, "GLM  周 : %u%%  已用 %u / 总 %u  剩 %u",
                 state->glm.win_w.pct, (unsigned)state->glm.win_w.current,
                 (unsigned)state->glm.win_w.usage, (unsigned)state->glm.win_w.remaining);
    } else {
        ESP_LOGW(TAG, "GLM  err: %s", state->glm.err);
    }
    if (state->dsk.valid) {
        ESP_LOGI(TAG, "DSK  总 %.2f  赠金 %.2f  充值 %.2f  (%s)  可用=%d",
                 state->dsk.total, state->dsk.granted, state->dsk.topped,
                 state->dsk.currency, (int)state->dsk.is_available);
        ESP_LOGI(TAG, "HIST 今日 %.2f 元  本月 %.2f 元  (积分基准 dbase 见下轮)",
                 state->hist.today_cents / 100.0, state->hist.month_cents / 100.0);
    } else {
        ESP_LOGW(TAG, "DSK  err: %s", state->dsk.err);
    }
}
