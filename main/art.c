/**
 * art — 每日一图实现 (Bing 壁纸 → JPEG 解码 → 自动色阶 + 抖动 → 1-bit), 见 art.h
 */

#include "art.h"
#include "net_query.h"
#include "nvs.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_jpeg_dec.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

static const char *TAG = "art";

#define ART_W_PX       ART_W
#define ART_H_PX       ART_H
#define AROW           (ART_W_PX / 8)              /* 50 字节/行 */
#define ABYTES         (AROW * ART_H_PX)           /* 12400 */

#define ART_NS         "ai_art"
#define ART_PART_LABEL "storage"                   /* 1MB 裸分区, 原用于 SPIFFS, 本项目未用 */
#define JPG_CAP        (192 * 1024)                /* 原始/解压后的 JPEG (实测 20KB) */
#define RGB_CAP        (ART_W_PX * ART_H_PX * 2)   /* RGB565: 198400 */
#define LUM_CAP        (ART_W_PX * ART_H_PX * 2)   /* int16 亮度: 198400 */
#define ART_PROMPT_VER 7                           /* 渲染/存储格式版本: 变了重取一次 */
#define MAX_TRIES_PER_DAY 3

#define BING_JSON_URL  "https://www.bing.com/HPImageArchive.aspx?format=js&idx=0&n=1&mkt=zh-CN"

/* ── 分区里的持久化格式 ── */
typedef struct __attribute__((packed)) {
    uint32_t magic;                    /* 'AIMG' */
    int32_t  day;                      /* UTC 日 */
    int32_t  ver;                      /* 渲染算法版本 */
    char     title[96];                /* 图片说明 (Bing copyright, 超长由 UI 截断) */
    uint32_t crc;                      /* 位图校验 (字节和) */
} art_hdr_t;
#define ART_MAGIC 0x474D4941u          /* "AIMG" (小端) */

static uint8_t  s_bits[ABYTES];
static char     s_title[96];
static int32_t  s_day = -1, s_try_day = -1, s_try_ver = -1, s_ver = 0;
static uint8_t  s_tries = 0;
static int      s_rev = 1;

static uint8_t *s_jpg;                 /* PSRAM: 下载的 JPEG */
static uint8_t *s_rgb;                 /* PSRAM: 解码后的 RGB565 */
static int16_t *s_lum;                 /* PSRAM: 亮度工作缓冲 (抖动就地修改) */

const uint8_t *art_bitmap(void) { return s_bits; }
int art_rev(void) { return s_rev; }

/* ══════════ 分区读写 (裸分区, 不用文件系统) ══════════ */

static const esp_partition_t *art_part(void) {
    static const esp_partition_t *p = NULL;
    static bool tried = false;
    if (!tried) {
        tried = true;
        p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
                                     ART_PART_LABEL);
        if (!p) ESP_LOGW(TAG, "找不到分区 %s, 图无法持久化", ART_PART_LABEL);
    }
    return p;
}

static void art_store(int32_t day, const char *title) {
    const esp_partition_t *p = art_part();
    if (!p) return;
    art_hdr_t h = { .magic = ART_MAGIC, .day = day, .ver = ART_PROMPT_VER, .crc = 0 };
    strlcpy(h.title, title, sizeof(h.title));
    for (int i = 0; i < ABYTES; i++) h.crc += s_bits[i];

    /* 需要按扇区擦除: 头 + 位图 ≈ 12.5KB → 擦 16KB */
    uint32_t need = sizeof(h) + ABYTES;
    esp_err_t e = esp_partition_erase_range(p, 0, 0x4000);
    if (e != ESP_OK) { ESP_LOGW(TAG, "擦除失败 %s", esp_err_to_name(e)); return; }
    e = esp_partition_write(p, 0, &h, sizeof(h));
    if (e == ESP_OK) e = esp_partition_write(p, sizeof(h), s_bits, ABYTES);
    if (e != ESP_OK) ESP_LOGW(TAG, "写分区失败 %s", esp_err_to_name(e));
    else             ESP_LOGI(TAG, "已存图到分区 (%u 字节)", (unsigned)need);
}

static bool art_load_stored(app_state_t *st) {
    const esp_partition_t *p = art_part();
    if (!p) return false;
    art_hdr_t h;
    if (esp_partition_read(p, 0, &h, sizeof(h)) != ESP_OK) return false;
    if (h.magic != ART_MAGIC) return false;
    if (esp_partition_read(p, sizeof(h), s_bits, ABYTES) != ESP_OK) return false;

    uint32_t crc = 0;
    for (int i = 0; i < ABYTES; i++) crc += s_bits[i];
    if (crc != h.crc) { ESP_LOGW(TAG, "分区里的位图校验失败"); return false; }

    strlcpy(s_title, h.title[0] ? h.title : "每日一图", sizeof(s_title));
    strlcpy(st->art.title, s_title, sizeof(st->art.title));
    st->art.valid = true;
    ESP_LOGI(TAG, "已读回上次的图 \"%s\" (day=%ld ver=%ld)", s_title, (long)h.day, (long)h.ver);
    return true;
}

/* ══════════ 抖动 ══════════
 * 自动色阶 + 平坦区保护: 直接上 Floyd–Steinberg 会在大片暗部/亮部长满麻点,
 * 而固定阈值又会把暗调照片整幅变黑 —— 必须按画面直方图定黑白场,
 * 再让"大面积纯色区"直接定死, 只对中间调抖动。
 */

#define FLAT_LO 45     /* 拉伸后 <= 此值直接黑 */
#define FLAT_HI 210    /* 拉伸后 >= 此值直接白 */

static void art_dither(void) {
    /* 1. 直方图 → 10%/90% 分位当黑白场 */
    int hist[256];
    memset(hist, 0, sizeof(hist));
    int total = ART_W_PX * ART_H_PX;
    for (int i = 0; i < total; i++) {
        int v = s_lum[i];
        hist[v < 0 ? 0 : (v > 255 ? 255 : v)]++;
    }
    int lo = 0, hi = 255, acc = 0;
    for (int i = 0; i < 256; i++) { acc += hist[i]; if (acc >= total / 10) { lo = i; break; } }
    acc = 0;
    for (int i = 0; i < 256; i++) { acc += hist[i]; if (acc >= total * 9 / 10) { hi = i; break; } }
    if (hi - lo < 40) { hi = lo + 40; if (hi > 255) { hi = 255; lo = hi - 40; } }
    int span = hi - lo;
    if (span < 1) span = 1;

    /* 2. 拉伸 + 抖动 (误差以拉伸前的尺度回灌) */
    memset(s_bits, 0, sizeof(s_bits));
    for (int y = 0; y < ART_H_PX; y++) {
        for (int x = 0; x < ART_W_PX; x++) {
            int idx = y * ART_W_PX + x;
            int v = (s_lum[idx] - lo) * 255 / span;
            if (v < 0) v = 0;
            if (v > 255) v = 255;

            /* on = 1 表示"白"(不着墨)。位图里 bit=1 表示**黑**, 所以下面取反。
             * (这里踩过坑: 最初写成 if (on) 置位, 结果整幅图黑白反相) */
            int on;
            long err = 0;
            if (v <= FLAT_LO)      { on = 0; }
            else if (v >= FLAT_HI) { on = 1; }
            else {
                int t = (v - FLAT_LO) * 255 / (FLAT_HI - FLAT_LO);   /* 中间调再拉伸 */
                on = (t >= 128);
                err = (long)(t - (on ? 255 : 0)) * (FLAT_HI - FLAT_LO) / 255 * span / 255;
            }
            if (!on) s_bits[y * AROW + (x >> 3)] |= (uint8_t)(0x80 >> (x & 7));

            if (err) {
                if (x + 1 < ART_W_PX) {
                    s_lum[idx + 1] += (int16_t)(err * 7 / 16);
                }
                if (y + 1 < ART_H_PX) {
                    if (x > 0)             s_lum[idx + ART_W_PX - 1] += (int16_t)(err * 3 / 16);
                    s_lum[idx + ART_W_PX]     += (int16_t)(err * 5 / 16);
                    if (x + 1 < ART_W_PX)  s_lum[idx + ART_W_PX + 1] += (int16_t)(err * 1 / 16);
                }
            }
        }
    }
    ESP_LOGI(TAG, "抖动完成: 黑白场 %d..%d", lo, hi);
}

/* 串口预览: 8 行 × 4 列统计墨点密度 → 灰阶字符
 * (抖动图的墨点近乎均匀分布, OR 降采样会糊成一片 '#', 必须按密度映射) */
static void art_preview(const char *title) {
    static const char *RAMP = " .:-=+*#%@";     /* 10 级 */
    char line[ART_W_PX / 4 + 1];
    ESP_LOGI(TAG, "── \"%s\" ──", title);
    for (int r = 0; r < ART_H_PX; r += 8) {
        for (int c = 0; c < ART_W_PX; c += 4) {
            int ink = 0;
            for (int dy = 0; dy < 8; dy++)
                for (int dx = 0; dx < 4; dx++) {
                    int y = r + dy, x = c + dx;
                    if (y < ART_H_PX && (s_bits[y * AROW + (x >> 3)] >> (7 - (x & 7))) & 1) ink++;
                }
            int lv = ink * 9 / 32;              /* 0..9 */
            line[c / 4] = RAMP[lv];
        }
        line[ART_W_PX / 4] = '\0';
        ESP_LOGI(TAG, "|%s|", line);
    }
}

/* ══════════ 拉取 + 解码 ══════════ */

static bool art_fetch(app_state_t *st) {
    /* 1. 拿今天的 urlbase 与标题 */
    static char json[3072];
    int n = net_https_get(BING_JSON_URL, NULL, NULL, NULL, json, sizeof(json));
    if (n <= 0) { ESP_LOGW(TAG, "bing json 请求失败 %d", n); return false; }

    static char jtxt[3072];
    int jl = net_http_body_decode(json, n, jtxt, sizeof(jtxt));
    if (jl <= 0) { ESP_LOGW(TAG, "bing json 解码失败"); return false; }

    cJSON *root = cJSON_Parse(jtxt);
    if (!root) { ESP_LOGW(TAG, "bing json 解析失败: %.120s", jtxt); return false; }
    cJSON *imgs = cJSON_GetObjectItem(root, "images");
    cJSON *im0  = cJSON_IsArray(imgs) ? cJSON_GetArrayItem(imgs, 0) : NULL;
    cJSON *jub  = im0 ? cJSON_GetObjectItem(im0, "urlbase") : NULL;
    cJSON *jcp  = im0 ? cJSON_GetObjectItem(im0, "copyright") : NULL;   /* 图片说明 */
    cJSON *jti  = im0 ? cJSON_GetObjectItem(im0, "title") : NULL;
    char urlbase[192] = "", title[96] = "";
    if (jub && jub->valuestring) strlcpy(urlbase, jub->valuestring, sizeof(urlbase));
    /* 优先用说明文字 (如 "海笔上的装饰蟹，科莫多国家公园，印度尼西亚 (© xx)"),
     * 去掉结尾的版权括号; 没有就退回短标题 */
    if (jcp && jcp->valuestring) {
        char *cut = strstr(jcp->valuestring, " (");
        if (cut) strlcpy(title, jcp->valuestring, (size_t)(cut - jcp->valuestring) + 1);
        else     strlcpy(title, jcp->valuestring, sizeof(title));
    }
    if (!title[0] && jti && jti->valuestring) strlcpy(title, jti->valuestring, sizeof(title));
    cJSON_Delete(root);
    if (!urlbase[0]) { ESP_LOGW(TAG, "bing json 里没有 urlbase"); return false; }
    if (!title[0]) strlcpy(title, "每日一图", sizeof(title));

    /* 2. 让 Bing 服务端直接裁成 400×248 (原图 340KB → 20KB) */
    char url[384];
    snprintf(url, sizeof(url),
             "https://www.bing.com%s_1920x1080.jpg&w=%d&h=%d&rs=1&c=4&pid=hp",
             urlbase, ART_W_PX, ART_H_PX);

    n = net_https_get(url, NULL, NULL, NULL, (char *)s_jpg, JPG_CAP);
    if (n <= 0) { ESP_LOGW(TAG, "壁纸下载失败 %d", n); return false; }
    ESP_LOGI(TAG, "壁纸 %d 字节 (%.1fKB), \"%s\"", n, n / 1024.0, title);

    /* 3. 解码 (Bing 可能无视 identity 回 gzip, 先解容器) */
    int dl = net_http_body_decode((char *)s_jpg, n, (char *)s_jpg, JPG_CAP);
    if (dl <= 0) { ESP_LOGW(TAG, "壁纸解压失败"); return false; }
    if (!(s_jpg[0] == 0xFF && s_jpg[1] == 0xD8)) {
        ESP_LOGW(TAG, "不是 JPEG (magic %02X %02X)", s_jpg[0], s_jpg[1]);
        return false;
    }

    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_dec_handle_t dec = NULL;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) { ESP_LOGW(TAG, "jpeg_dec_open 失败"); return false; }

    jpeg_dec_io_t io;
    memset(&io, 0, sizeof(io));
    io.inbuf = s_jpg;
    io.inbuf_len = dl;
    jpeg_dec_header_info_t info;
    if (jpeg_dec_parse_header(dec, &io, &info) != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "JPEG 头解析失败"); jpeg_dec_close(dec); return false;
    }
    int outlen = 0;
    jpeg_dec_get_outbuf_len(dec, &outlen);
    ESP_LOGI(TAG, "JPEG %dx%d → 输出 %d 字节", info.width, info.height, outlen);
    if (outlen <= 0 || outlen > RGB_CAP || info.width != ART_W_PX || info.height != ART_H_PX) {
        ESP_LOGW(TAG, "尺寸不符 (期望 %dx%d)", ART_W_PX, ART_H_PX);
        jpeg_dec_close(dec);
        return false;
    }
    io.outbuf = s_rgb;
    io.out_size = outlen;
    if (jpeg_dec_process(dec, &io) != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "JPEG 解码失败"); jpeg_dec_close(dec); return false;
    }
    jpeg_dec_close(dec);

    /* 4. RGB565 → 亮度 */
    for (int i = 0; i < ART_W_PX * ART_H_PX; i++) {
        uint16_t p = (uint16_t)(s_rgb[i * 2] | (s_rgb[i * 2 + 1] << 8));
        int r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
        r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
        s_lum[i] = (int16_t)((r * 77 + g * 150 + b * 29) >> 8);
    }

    /* 5. 抖动 + 落盘 */
    art_dither();
    strlcpy(s_title, title, sizeof(s_title));
    strlcpy(st->art.title, title, sizeof(st->art.title));
    art_store((int32_t)(time(NULL) / 86400), title);
    s_rev++;
    art_preview(title);
    return true;
}

/* ══════════ 外部接口 ══════════ */

void art_init(app_state_t *st) {
    nvs_handle_t h;
    if (nvs_open(ART_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, "day", &s_day);
        nvs_get_i32(h, "tryd", &s_try_day);
        nvs_get_i32(h, "tryv", &s_try_ver);
        nvs_get_u8(h, "try", &s_tries);
        nvs_get_i32(h, "ver", &s_ver);
        nvs_close(h);
    }

    /* PSRAM 缓冲 (16 字节对齐: JPEG 输出要求) */
    s_jpg = heap_caps_aligned_alloc(16, JPG_CAP, MALLOC_CAP_SPIRAM);
    s_rgb = heap_caps_aligned_alloc(16, RGB_CAP, MALLOC_CAP_SPIRAM);
    s_lum = heap_caps_aligned_alloc(16, LUM_CAP, MALLOC_CAP_SPIRAM);
    if (!s_jpg || !s_rgb || !s_lum) {
        ESP_LOGE(TAG, "PSRAM 分配失败 (jpg=%p rgb=%p lum=%p)", s_jpg, s_rgb, s_lum);
        s_jpg = s_rgb = NULL; s_lum = NULL;
        return;
    }

    art_load_stored(st);
    /* 关键: ui_init / 首次 ui_update 跑在本函数之前, 那时画布还是空的(全白);
     * 加载完必须 +1 让 UI 知道要重绘, 否则画布永远停在开机时的白屏 */
    s_rev++;
    ESP_LOGI(TAG, "art: %s (done_day=%ld ver=%ld/%d)", st->art.valid ? "loaded" : "empty",
             (long)s_day, (long)s_ver, ART_PROMPT_VER);
}

void art_poll(app_state_t *st) {
    if (st->art.generating || !s_jpg) return;
    time_t t = time(NULL);
    if (t < 1700000000) return;                    /* 时间未同步, 无法判定"今天" */

    int32_t day = (int32_t)(t / 86400);
    if (day == s_day && s_ver == ART_PROMPT_VER) return;   /* 今天已取且算法未变 */

    if (s_try_day != day || s_try_ver != ART_PROMPT_VER) {
        s_try_day = day;
        s_try_ver = ART_PROMPT_VER;
        s_tries = 0;
    }
    if (s_tries >= MAX_TRIES_PER_DAY) return;

    ESP_LOGI(TAG, "fetching today's art (try %u/%u)...", s_tries + 1, MAX_TRIES_PER_DAY);
    st->art.generating = true;
    bool ok = art_fetch(st);
    st->art.generating = false;

    if (ok) {
        s_day = day;
        s_ver = ART_PROMPT_VER;
        st->art.valid = true;
        nvs_handle_t h;
        if (nvs_open(ART_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_i32(h, "day", day);
            nvs_set_i32(h, "ver", ART_PROMPT_VER);
            nvs_set_u8(h, "try", 0);
            nvs_commit(h);
            nvs_close(h);
        }
    } else {
        s_tries++;
        nvs_handle_t h;
        if (nvs_open(ART_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_i32(h, "tryd", s_try_day);
            nvs_set_i32(h, "tryv", s_try_ver);
            nvs_set_u8(h, "try", s_tries);
            nvs_commit(h);
            nvs_close(h);
        }
    }
}
