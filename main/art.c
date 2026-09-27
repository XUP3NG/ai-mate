/**
 * art — AI 每日像素画实现, 见 art.h
 */

#include "art.h"
#include "net_query.h"
#include "nvs.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "art";

#define ART_NS         "ai_art"
#define ART_W          40
#define ART_H          30
#define ART_ROW_BYTES  (ART_W / 8)          /* 5 */
#define ART_BYTES      (ART_ROW_BYTES * ART_H)   /* 150 */

#define DSK_CHAT_URL   "https://api.deepseek.com/chat/completions"
#define MAX_TRIES_PER_DAY  3
/* prompt 版本: 改画法/尺寸时 +1 → 已存的老画自动作废重画一次 (之后恢复每日一次) */
#define ART_PROMPT_VER     2

static char    s_rx[16384];                 /* HTTP 响应 (含 JSON 转义) */
static uint8_t s_px[ART_BYTES];
static int32_t s_day = -1;                  /* 已成功生成的 UTC 日 */
static int32_t s_try_day = -1;
static int32_t s_try_ver = -1;              /* 计数对应的 prompt 版本 */
static uint8_t s_tries = 0;
static int32_t s_ver = 0;                   /* 已存画的 prompt 版本 */
static int     s_rev = 1;

const uint8_t *art_bitmap(void) { return s_px; }
int art_rev(void) { return s_rev; }

/* ── NVS ── */

void art_init(app_state_t *st) {
    nvs_handle_t h;
    if (nvs_open(ART_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t blob[ART_BYTES];
        size_t sz = sizeof(blob);
        if (nvs_get_blob(h, "px", blob, &sz) == ESP_OK && sz == ART_BYTES) {
            memcpy(s_px, blob, ART_BYTES);
            st->art.valid = true;
        }
        nvs_get_i32(h, "day", &s_day);
        nvs_get_i32(h, "tryd", &s_try_day);
        nvs_get_i32(h, "tryv", &s_try_ver);
        nvs_get_u8(h, "try", &s_tries);
        nvs_get_i32(h, "ver", &s_ver);
        size_t ts = sizeof(st->art.title);
        if (nvs_get_str(h, "title", st->art.title, &ts) != ESP_OK || !st->art.title[0])
            strlcpy(st->art.title, "像素画", sizeof(st->art.title));
        nvs_close(h);
    }
    ESP_LOGI(TAG, "art: %s (done_day=%ld ver=%ld/%d)", st->art.valid ? "loaded" : "empty",
             (long)s_day, (long)s_ver, ART_PROMPT_VER);
}

static void art_save(int32_t day, const char *title) {
    nvs_handle_t h;
    if (nvs_open(ART_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "px", s_px, ART_BYTES);
    nvs_set_str(h, "title", title);
    nvs_set_i32(h, "day", day);
    nvs_set_i32(h, "tryd", day);
    nvs_set_i32(h, "tryv", ART_PROMPT_VER);
    nvs_set_i32(h, "ver", ART_PROMPT_VER);
    nvs_set_u8(h, "try", 0);
    nvs_commit(h);
    nvs_close(h);
}

/* ── 解析模型输出: "标题：xx" + 若干行只含 # 和 . 的画布 ── */

static bool art_parse(const char *content, char *title, size_t tsz) {
    static uint8_t rows[ART_H][ART_W];      /* static: 4.8KB 别放栈上 */
    memset(rows, 0, sizeof(rows));
    int nr = 0;
    title[0] = '\0';

    const char *p = content;
    while (*p && nr < ART_H) {
        const char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        const char *line_end = e ? e : p + len;

        /* 标题行: "标题：xxx" (中文或 ASCII 冒号) */
        if (!title[0] && len > 8 && strncmp(p, "标题", 6) == 0) {
            const char *colon = NULL; int cskip = 1;
            for (const char *q = p; q < line_end; q++)
                if (*q == ':') { colon = q; break; }
            for (const char *q = p; q + 2 < line_end; q++)
                if ((uint8_t)q[0] == 0xEF && (uint8_t)q[1] == 0xBC && (uint8_t)q[2] == 0x9A) {
                    colon = q; cskip = 3; break;    /* 中文冒号 UTF-8 */
                }
            if (colon) {
                colon += cskip;
                size_t n = (size_t)(line_end - colon);
                while (n > 0 && (colon[n-1] == '\r' || colon[n-1] == ' ')) n--;
                if (n >= tsz) n = tsz - 1;
                memcpy(title, colon, n);
                title[n] = '\0';
            }
            if (!e) break;
            p = e + 1;
            continue;
        }

        /* 画布行: 只含 # 和 . */
        bool canvas = (len >= ART_W / 2);
        for (size_t i = 0; i < len && canvas; i++)
            if (p[i] != '#' && p[i] != '.') canvas = false;
        if (canvas) {
            size_t n = len > ART_W ? ART_W : len;
            for (size_t i = 0; i < ART_W; i++)
                rows[nr][i] = (i < n && p[i] == '#') ? 1 : 0;
            nr++;
        }
        if (!e) break;
        p = e + 1;
    }

    if (nr < ART_H / 2) {
        ESP_LOGW(TAG, "parse: 画布行数不足 (%d 行)", nr);
        return false;
    }
    if (!title[0]) strlcpy(title, "无题", tsz);

    /* 垂直居中后打包成位图 */
    memset(s_px, 0, sizeof(s_px));
    int off = (ART_H - nr) / 2;
    for (int r = 0; r < nr; r++)
        for (int x = 0; x < ART_W; x++)
            if (rows[r][x])
                s_px[(r + off) * ART_ROW_BYTES + (x >> 3)] |= (uint8_t)(0x80 >> (x & 7));

    /* 黑点占比 sanity: 全黑或全白都判失败 (模型抽风) */
    int ink = 0;
    for (int i = 0; i < ART_BYTES; i++)
        for (int b = 0; b < 8; b++) ink += (s_px[i] >> b) & 1;
    if (ink < ART_W * ART_H / 25 || ink > ART_W * ART_H * 45 / 100) {
        ESP_LOGW(TAG, "parse: 黑点占比异常 %d/%d", ink, ART_W * ART_H);
        return false;
    }
    return true;
}

/* 把成品打到串口日志 (逐行 ASCII) —— 不看屏幕也能判断画得像不像 */
static void art_preview(const char *title) {
    char line[ART_W + 1];
    ESP_LOGI(TAG, "── \"%s\" ──", title);
    for (int r = 0; r < ART_H; r++) {
        for (int c = 0; c < ART_W; c++)
            line[c] = (s_px[r * ART_ROW_BYTES + (c >> 3)] & (0x80 >> (c & 7))) ? '#' : '.';
        line[ART_W] = '\0';
        ESP_LOGI(TAG, "|%s|", line);
    }
}

/* ── 生成 ── */

static bool art_generate(app_state_t *st, const app_config_t *cfg) {
    char prompt[1400];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    static const char *WD[] = {"日","一","二","三","四","五","六"};
    char wxline[64] = "天气未知";
    if (st->wx.valid)
        snprintf(wxline, sizeof(wxline), "天气%s 气温%d度", st->wx.text, st->wx.temp_x10 / 10);

    /* 关键: 让模型画**一个具体实物**并明确给密度约束。
     * 早先版本只说"自由联想 + 剪影风", 结果出来全是抽象色块 —— 文本模型对
     * 大画布(80×60)的空间控制力很差, 格子越少越画得像。 */
    snprintf(prompt, sizeof(prompt),
        "你是像素艺术家, 为黑白点阵屏作画。画布 %d 列 × %d 行, '#'=黑, '.'=白。\n"
        "严格遵守:\n"
        "1. 第一行只输出: 标题：xxx (4个汉字以内, 就是画的是什么)\n"
        "2. 然后输出恰好 %d 行, 每行恰好 %d 个字符, 只能含 '#' 和 '.', 用 ``` 围起来\n"
        "3. 画一个**具体可辨认的实物**, 一眼就能看出是什么; 不要抽象图案、几何色块、随机噪点\n"
        "4. 主体占画面 60%%~80%%, 居中, 四周留白; 黑色像素占总量的 15%%~35%%\n"
        "5. 不加边框、不写文字、不签名\n"
        "候选题材(选一个, 也可另选别的具体实物, 优先贴合今天天气/季节):\n"
        "猫 狗 灯塔 帆船 鲸鱼 蘑菇 仙人掌 自行车 咖啡杯 老式相机 台灯 飞鸟\n"
        "山峰与月亮 小屋 雨伞 吉他 热气球 金鱼 树叶 雪人 机器人 火箭 企鹅\n"
        "示例(只示范线条密度与留白, 不要照抄, 真实输出必须 %d 行 × %d 字符):\n"
        "标题：帆船\n"
        ".........#.........\n"
        "........###........\n"
        ".......#####.......\n"
        "......#######......\n"
        ".....#########.....\n"
        ".........#.........\n"
        ".........#.........\n"
        "今天是%d月%d日 星期%s, %s。请选择题材并创作。",
        ART_W, ART_H, ART_H, ART_W, ART_H, ART_W,
        tm.tm_mon + 1, tm.tm_mday, WD[tm.tm_wday], wxline);

    /* JSON 请求体 */
    static char body[3072];
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", "deepseek-chat");
    cJSON_AddNumberToObject(root, "max_tokens", 2500);
    cJSON_AddNumberToObject(root, "temperature", 0.9);
    cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "role", "user");
    cJSON_AddStringToObject(m, "content", prompt);
    cJSON_AddItemToArray(msgs, m);
    bool printed = cJSON_PrintPreallocated(root, body, sizeof(body) - 1, false);
    cJSON_Delete(root);
    if (!printed) return false;

    char auth[112];
    snprintf(auth, sizeof(auth), "Bearer %s", cfg->dsk_key);

    uint32_t t0 = (uint32_t)(esp_timer_get_time() / 1000);
    int n = net_https_post_json(DSK_CHAT_URL, body, auth, 150000, s_rx, sizeof(s_rx));
    if (n <= 0) {
        ESP_LOGW(TAG, "POST failed: %d", n);
        return false;
    }

    cJSON *resp = cJSON_Parse(s_rx);
    if (!resp) {
        ESP_LOGW(TAG, "resp JSON 解析失败 [%d]: %.160s", n, s_rx);
        return false;
    }
    cJSON *ch  = cJSON_GetObjectItem(resp, "choices");
    cJSON *c0  = cJSON_IsArray(ch) ? cJSON_GetArrayItem(ch, 0) : NULL;
    cJSON *msg = c0 ? cJSON_GetObjectItem(c0, "message") : NULL;
    cJSON *ct  = msg ? cJSON_GetObjectItem(msg, "content") : NULL;

    bool ok = false;
    char title[24] = "";
    if (ct && ct->valuestring) {
        ok = art_parse(ct->valuestring, title, sizeof(title));
    } else {
        ESP_LOGW(TAG, "无 content [%d]: %.200s", n, s_rx);
    }
    cJSON_Delete(resp);

    uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000) - t0;
    if (ok) {
        strlcpy(st->art.title, title, sizeof(st->art.title));
        art_save((int32_t)(t / 86400), st->art.title);
        s_ver = ART_PROMPT_VER;
        s_rev++;
        art_preview(st->art.title);
        ESP_LOGI(TAG, "new art \"%s\" (%u ms)", st->art.title, ms);
    } else {
        ESP_LOGW(TAG, "generate/parse failed (%u ms)", ms);
    }
    return ok;
}

void art_poll(app_state_t *st, const app_config_t *cfg) {
    if (st->art.generating) return;
    time_t t = time(NULL);
    if (t < 1700000000) return;                  /* 时间未同步, 无法判定"今天" */
    if (!cfg->dsk_key[0]) return;

    int32_t day = (int32_t)(t / 86400);
    if (day == s_day && s_ver == ART_PROMPT_VER) return;   /* 今天已生成且画法未变 */

    /* 换天 **或** 换 prompt 版本 → 重新获得 3 次机会 (不重置的话改版当天会一直放弃) */
    if (s_try_day != day || s_try_ver != ART_PROMPT_VER) {
        s_try_day = day;
        s_try_ver = ART_PROMPT_VER;
        s_tries = 0;
    }
    if (s_tries >= MAX_TRIES_PER_DAY) return;     /* 今天已放弃, 明天再说 */

    ESP_LOGI(TAG, "generating today's art (try %u/%u)...", s_tries + 1, MAX_TRIES_PER_DAY);
    st->art.generating = true;                    /* UI 显示"生成中" */
    bool ok = art_generate(st, cfg);
    st->art.generating = false;

    if (ok) {
        s_day = day;
        st->art.valid = true;
    } else {
        s_tries++;
        nvs_handle_t h;                           /* 持久化计数, 防重启后重复扣费 */
        if (nvs_open(ART_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_i32(h, "tryd", s_try_day);
            nvs_set_i32(h, "tryv", s_try_ver);
            nvs_set_u8(h, "try", s_tries);
            nvs_commit(h);
            nvs_close(h);
        }
    }
}
