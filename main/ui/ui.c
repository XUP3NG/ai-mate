/**
 * AI_Mate UI — 400×300 1-bit B/W
 *
 * 字体: font_cjk_16 (中文/文字) + lv_font_montserrat_20 (大号数字/百分比)
 *
 * Page 0 额度总览 (双卡片)  |  Page 1 30天消费柱状图  |  Page 2 配网提示
 *
 * 页面可用区: y = 24..300 (TopBar 占 0..23), 共 276px
 */

#include "ui.h"
#include "weather.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include "esp_timer.h"

extern const lv_font_t font_cjk_16;
extern const lv_font_t font_wx_icon_36;
extern const lv_font_t font_wx_icon_24;
extern const lv_font_t font_wx_num_36;

/* ── 柱状图几何常量 (page 1) ── */
#define CHART_X0      6      /* 首柱 x */
#define CHART_BAR_W   10     /* 柱宽 */
#define CHART_GAP     3      /* 柱间距 */
#define CHART_BASE_Y  216    /* 基线的 y (页面内坐标) */
#define CHART_MAX_H   162    /* 柱最大高度 */
#define CHART_W       (CHART_DAYS * (CHART_BAR_W + CHART_GAP) - CHART_GAP)  /* 387 */

static lv_color_t c_tx(void)  { return lv_color_black(); }
static lv_color_t c_bg(void)  { return lv_color_white(); }
static lv_color_t c_dim(void) { return lv_color_make(70,70,70); }   /* 1bit 下=黑 */

/* ── 小部件工厂 ── */
static lv_obj_t *label(lv_obj_t *parent, int x, int y, int w) {
    lv_obj_t *o = lv_label_create(parent);
    lv_label_set_text(o, "");
    lv_obj_set_style_text_color(o, c_tx(), 0);
    lv_obj_set_style_text_font(o, &font_cjk_16, 0);
    lv_obj_set_pos(o, x, y);
    if (w) lv_obj_set_width(o, w);
    return o;
}

static lv_obj_t *label_big(lv_obj_t *parent, int x, int y, int w) {
    lv_obj_t *o = label(parent, x, y, w);
    lv_obj_set_style_text_font(o, &lv_font_montserrat_20, 0);
    return o;
}

static lv_obj_t *card(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_bg_color(o, c_bg(), 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, c_tx(), 0);
    lv_obj_set_style_radius(o, 6, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    return o;
}

/* 进度条: 1-bit 屏没有灰色 → 轨道用白底 + 1px 黑描边(可见), 填充用纯黑 */
static lv_obj_t *bar(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *b = lv_bar_create(parent);
    lv_obj_remove_style_all(b);
    /* 轨道 */
    lv_obj_set_style_bg_color(b, c_bg(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(b, c_tx(), LV_PART_MAIN);
    lv_obj_set_style_radius(b, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(b, 1, LV_PART_MAIN);   /* 填充内缩, 不盖住描边 */
    /* 填充 */
    lv_obj_set_style_bg_color(b, c_tx(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(b, 0, LV_PART_INDICATOR);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    lv_bar_set_range(b, 0, 100);
    lv_bar_set_value(b, 0, LV_ANIM_OFF);
    return b;
}

static lv_obj_t *rect(lv_obj_t *parent, int x, int y, int w, int h, lv_color_t c) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    return o;
}

/* ── Init ── */
void ui_init(ui_elements_t *ui) {
    memset(ui, 0, sizeof(*ui));
    lv_obj_t *scr = lv_scr_act();
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, c_bg(), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    /* ════ TopBar (0..23) ════ */
    ui->top_bar = lv_obj_create(scr);
    lv_obj_set_size(ui->top_bar, DISPLAY_WIDTH, 24);
    lv_obj_set_pos(ui->top_bar, 0, 0);
    lv_obj_set_style_bg_color(ui->top_bar, c_tx(), 0);
    lv_obj_set_style_border_width(ui->top_bar, 0, 0);
    lv_obj_set_style_radius(ui->top_bar, 0, 0);
    lv_obj_set_style_pad_all(ui->top_bar, 0, 0);

    /* 顶栏分四栏固定位置, 宽度写死 + LONG_DOT, 保证任何内容都不重叠 */
    ui->title_label = label(ui->top_bar, 8, 3, 104);
    lv_label_set_text(ui->title_label, "AI 额度监控");
    lv_obj_set_style_text_color(ui->title_label, c_bg(), 0);
    lv_label_set_long_mode(ui->title_label, LV_LABEL_LONG_DOT);

    ui->clock_label = label(ui->top_bar, 118, 3, 152);      /* 118..270 */
    lv_obj_set_style_text_color(ui->clock_label, c_bg(), 0);
    lv_label_set_long_mode(ui->clock_label, LV_LABEL_LONG_DOT);

    ui->wifi_label = label(ui->top_bar, 278, 3, 58);        /* 278..336 */
    lv_obj_set_style_text_color(ui->wifi_label, c_bg(), 0);
    lv_label_set_long_mode(ui->wifi_label, LV_LABEL_LONG_DOT);

    ui->bat_label = label(ui->top_bar, 344, 3, 48);         /* 344..392 */
    lv_obj_set_style_text_color(ui->bat_label, c_bg(), 0);
    lv_label_set_long_mode(ui->bat_label, LV_LABEL_LONG_DOT);

    /* ════ Page 0: 额度总览 ════ */
    ui->page_main = lv_obj_create(scr);
    lv_obj_set_size(ui->page_main, DISPLAY_WIDTH, DISPLAY_HEIGHT - 24);
    lv_obj_set_pos(ui->page_main, 0, 24);
    lv_obj_set_style_bg_color(ui->page_main, c_bg(), 0);
    lv_obj_set_style_border_width(ui->page_main, 0, 0);
    lv_obj_set_style_radius(ui->page_main, 0, 0);
    lv_obj_set_style_pad_all(ui->page_main, 0, 0);
    lv_obj_remove_flag(ui->page_main, LV_OBJ_FLAG_SCROLLABLE);

    /* ── 智谱卡片: y 4..140 ── */
    card(ui->page_main, 4, 4, DISPLAY_WIDTH - 8, 136);

    ui->glm_title = label(ui->page_main, 16, 10, 360);
    lv_label_set_text(ui->glm_title, "智谱 GLM Coding Plan");

    /* 5h 窗口行 (y=34) */
    ui->glm5h_label = label(ui->page_main, 16, 34, 62);
    lv_label_set_text(ui->glm5h_label, "5h 窗");
    ui->glm5h_bar = bar(ui->page_main, 82, 38, 208, 12);
    ui->glm5h_pct = label_big(ui->page_main, 298, 31, 70);
    lv_label_set_text(ui->glm5h_pct, "--");

    /* 周窗口行 (y=60) */
    ui->glmw_label = label(ui->page_main, 16, 62, 62);
    lv_label_set_text(ui->glmw_label, "周 窗");
    ui->glmw_bar = bar(ui->page_main, 82, 66, 208, 12);
    ui->glmw_pct = label_big(ui->page_main, 298, 59, 70);
    lv_label_set_text(ui->glmw_pct, "--");

    /* 信息两行 */
    ui->glm_credit = label(ui->page_main, 16, 88, 368);
    ui->glm_reset  = label(ui->page_main, 16, 108, 368);
    lv_obj_set_style_text_color(ui->glm_reset, c_dim(), 0);
    ui->glm_err    = label(ui->page_main, 16, 122, 368);
    lv_obj_set_style_text_color(ui->glm_err, c_dim(), 0);

    /* ── DeepSeek 卡片: y 146..252 ── */
    card(ui->page_main, 4, 146, DISPLAY_WIDTH - 8, 106);

    ui->dsk_title = label(ui->page_main, 16, 152, 300);
    lv_label_set_text(ui->dsk_title, "DeepSeek 余额");

    /* 状态提示: 只在异常时显示, 与余额数字同一行右对齐 */
    ui->dsk_state = label(ui->page_main, 242, 176, 150);
    lv_obj_set_style_text_align(ui->dsk_state, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(ui->dsk_state, "");

    /* 大号余额数字 + 紧跟其后的"元" (字体不同, 用 align_to 动态贴合) */
    ui->dsk_balance = label_big(ui->page_main, 16, 172, 0);
    lv_obj_set_width(ui->dsk_balance, LV_SIZE_CONTENT);
    lv_label_set_text(ui->dsk_balance, "--");
    ui->dsk_currency = label(ui->page_main, 0, 178, 0);
    lv_obj_set_width(ui->dsk_currency, LV_SIZE_CONTENT);
    lv_label_set_text(ui->dsk_currency, "元");

    ui->dsk_detail = label(ui->page_main, 16, 200, 368);
    ui->dsk_spend  = label(ui->page_main, 16, 220, 368);
    ui->dsk_err    = label(ui->page_main, 16, 238, 368);
    lv_obj_set_style_text_color(ui->dsk_err, c_dim(), 0);

    /* 底部状态行 (页面高 276 → y=256 起, 16px 高, 不溢出) */
    ui->status_label = label(ui->page_main, 16, 256, 368);
    lv_obj_set_style_text_color(ui->status_label, c_dim(), 0);

    /* ════ Page 1: 消费柱状图 ════ */
    ui->page_heat = lv_obj_create(scr);
    lv_obj_set_size(ui->page_heat, DISPLAY_WIDTH, DISPLAY_HEIGHT - 24);
    lv_obj_set_pos(ui->page_heat, 0, 24);
    lv_obj_set_style_bg_color(ui->page_heat, c_bg(), 0);
    lv_obj_set_style_border_width(ui->page_heat, 0, 0);
    lv_obj_set_style_radius(ui->page_heat, 0, 0);
    lv_obj_set_style_pad_all(ui->page_heat, 0, 0);
    lv_obj_remove_flag(ui->page_heat, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ui->page_heat, LV_OBJ_FLAG_HIDDEN);

    /* 标题 + 右上刻度 */
    ui->chart_title = label(ui->page_heat, 8, 5, 260);
    lv_label_set_text(ui->chart_title, "DeepSeek 每日消费 (近30天)");

    ui->chart_scale = label(ui->page_heat, 0, 5, 130);
    lv_obj_set_style_text_color(ui->chart_scale, c_dim(), 0);
    lv_obj_align(ui->chart_scale, LV_ALIGN_TOP_RIGHT, -8, 5);

    /* 网格线 25 / 50 / 75%: 黑白屏无法画灰色 → 用黑色虚线 */
    for (int i = 0; i < 3; i++) {
        int gy = CHART_BASE_Y - CHART_MAX_H * (i + 1) / 4;
        for (int k = 0; k < CHART_DASH_N; k++) {
            ui->chart_dash[i][k] = rect(ui->page_heat,
                                        CHART_X0 + k * (CHART_DASH_W + CHART_DASH_GAP),
                                        gy, CHART_DASH_W, 1, c_tx());
        }
    }

    /* 30 根柱子 (先建, 后建基线保证压在最上层) */
    for (int i = 0; i < CHART_DAYS; i++) {
        lv_obj_t *b = rect(ui->page_heat,
                           CHART_X0 + i * (CHART_BAR_W + CHART_GAP),
                           CHART_BASE_Y, CHART_BAR_W, 0, c_tx());
        ui->chart_bars[i] = b;
    }

    /* 基线 */
    ui->chart_baseline = rect(ui->page_heat, CHART_X0, CHART_BASE_Y, CHART_W, 2, c_tx());

    /* 横轴日期标注: 起 / 中 / 今 */
    ui->chart_axis[0] = label(ui->page_heat, CHART_X0, 220, 60);
    ui->chart_axis[1] = label(ui->page_heat, 172, 220, 60);
    ui->chart_axis[2] = label(ui->page_heat, CHART_X0 + CHART_W - 34, 220, 34);
    for (int i = 0; i < 3; i++)
        lv_obj_set_style_text_color(ui->chart_axis[i], c_dim(), 0);

    /* 统计信息两行 */
    ui->chart_info  = label(ui->page_heat, 8, 240, 384);
    ui->chart_info2 = label(ui->page_heat, 8, 258, 384);
    lv_obj_set_style_text_color(ui->chart_info2, c_dim(), 0);

    /* ════ Page 3: 天气 ════
     *
     *  ┌ 无锡市 ───────────────────── 更新 2 分钟前 ┐   y=8
     *  │  ☁(36)      27.0 度                        │   y=34..72
     *  │              阴            湿度 64%         │   y=78
     *  ├────────────────────────────────────────────┤   y=104
     *  │  今天      明天      周三      周四          │   y=136
     *  │   ☁        ☂        ☀        ☀            │   y=162 (24px)
     *  │  30/22    28/20    31/21    32/22          │   y=200 (20px)
     *  │  Open-Meteo                                │   y=250
     *  └────────────────────────────────────────────┘
     */
    ui->page_weather = lv_obj_create(scr);
    lv_obj_set_size(ui->page_weather, DISPLAY_WIDTH, DISPLAY_HEIGHT - 24);
    lv_obj_set_pos(ui->page_weather, 0, 24);
    lv_obj_set_style_bg_color(ui->page_weather, c_bg(), 0);
    lv_obj_set_style_border_width(ui->page_weather, 0, 0);
    lv_obj_set_style_radius(ui->page_weather, 0, 0);
    lv_obj_set_style_pad_all(ui->page_weather, 0, 0);
    lv_obj_remove_flag(ui->page_weather, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ui->page_weather, LV_OBJ_FLAG_HIDDEN);

    ui->wx_city = label(ui->page_weather, 14, 6, 200);
    ui->wx_meta = label(ui->page_weather, 214, 6, 172);
    lv_obj_set_style_text_align(ui->wx_meta, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(ui->wx_meta, c_dim(), 0);

    /* 当前天气: 大图标 + 大号温度 */
    ui->wx_icon = label(ui->page_weather, 22, 30, 0);
    lv_obj_set_width(ui->wx_icon, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(ui->wx_icon, &font_wx_icon_36, 0);
    lv_label_set_text(ui->wx_icon, "");

    ui->wx_temp = label(ui->page_weather, 104, 28, 0);
    lv_obj_set_width(ui->wx_temp, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(ui->wx_temp, &font_wx_num_36, 0);
    lv_label_set_text(ui->wx_temp, "--");

    ui->wx_unit = label(ui->page_weather, 0, 46, 0);
    lv_obj_set_width(ui->wx_unit, LV_SIZE_CONTENT);
    lv_label_set_text(ui->wx_unit, "度");

    ui->wx_desc = label(ui->page_weather, 104, 78, 140);
    ui->wx_hum  = label(ui->page_weather, 250, 78, 136);
    lv_obj_set_style_text_align(ui->wx_hum, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(ui->wx_hum, c_dim(), 0);

    /* 分隔线 */
    {
        lv_obj_t *sep = rect(ui->page_weather, 14, 104, DISPLAY_WIDTH - 28, 1, c_tx());
        (void)sep;
    }

    /* 4 列预报 */
    {
        int col_x[WX_DAYS] = { 8, 104, 200, 296 };   /* 每列 96 宽, 内容居中 */
        for (int i = 0; i < WX_DAYS; i++) {
            ui->wx_day[i] = label(ui->page_weather, col_x[i], 126, 96);
            lv_obj_set_style_text_align(ui->wx_day[i], LV_TEXT_ALIGN_CENTER, 0);

            ui->wx_day_icon[i] = label(ui->page_weather, col_x[i], 154, 96);
            lv_obj_set_style_text_align(ui->wx_day_icon[i], LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_font(ui->wx_day_icon[i], &font_wx_icon_24, 0);

            ui->wx_day_temp[i] = label(ui->page_weather, col_x[i], 192, 96);
            lv_obj_set_style_text_align(ui->wx_day_temp[i], LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_font(ui->wx_day_temp[i], &lv_font_montserrat_20, 0);
        }
    }

    ui->wx_footer = label(ui->page_weather, 14, 240, 372);
    lv_obj_set_style_text_color(ui->wx_footer, c_dim(), 0);
    lv_label_set_text(ui->wx_footer, "数据源 Open-Meteo");

    /* ════ Page 2: 配网提示 ════ */
    ui->page_portal = lv_obj_create(scr);
    lv_obj_set_size(ui->page_portal, DISPLAY_WIDTH, DISPLAY_HEIGHT - 24);
    lv_obj_set_pos(ui->page_portal, 0, 24);
    lv_obj_set_style_bg_color(ui->page_portal, c_bg(), 0);
    lv_obj_set_style_border_width(ui->page_portal, 0, 0);
    lv_obj_set_style_radius(ui->page_portal, 0, 0);
    lv_obj_set_style_pad_all(ui->page_portal, 0, 0);
    lv_obj_add_flag(ui->page_portal, LV_OBJ_FLAG_HIDDEN);
    {
        lv_obj_t *t1 = label(ui->page_portal, 0, 56, DISPLAY_WIDTH);
        lv_label_set_text(t1, "配网模式");
        lv_obj_set_style_text_align(t1, LV_TEXT_ALIGN_CENTER, 0);

        ui->portal_ap = label(ui->page_portal, 0, 96, DISPLAY_WIDTH);
        lv_obj_set_style_text_align(ui->portal_ap, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(ui->portal_ap, "WiFi 连接: AI-Mate-Setup");

        lv_obj_t *t3 = label(ui->page_portal, 0, 120, DISPLAY_WIDTH);
        lv_obj_set_style_text_align(t3, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(t3, c_dim(), 0);
        lv_label_set_text(t3, "密码 aimate123");

        ui->portal_url = label(ui->page_portal, 0, 152, DISPLAY_WIDTH);
        lv_obj_set_style_text_align(ui->portal_url, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(ui->portal_url, "浏览器打开 http://192.168.4.1");

        lv_obj_t *t5 = label(ui->page_portal, 0, 184, DISPLAY_WIDTH);
        lv_obj_set_style_text_align(t5, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(t5, c_dim(), 0);
        lv_label_set_text(t5, "填写 WiFi 与 API Key 后保存");

        lv_obj_t *t6 = label(ui->page_portal, 0, 208, DISPLAY_WIDTH);
        lv_obj_set_style_text_align(t6, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(t6, c_dim(), 0);
        lv_label_set_text(t6, "长按 BOOT 键 3 秒可重新配网");
    }
}

/* ── 格式化 ── */
static void fmt_reset(char *b, size_t sz, int64_t reset_ms) {
    if (reset_ms <= 0) { snprintf(b, sz, "--"); return; }
    int64_t now_ms = (int64_t)time(NULL) * 1000;
    int64_t d = reset_ms - now_ms;
    if (d <= 0) { snprintf(b, sz, "重置中"); return; }
    int64_t sec = d / 1000;
    if (sec >= 86400) snprintf(b, sz, "%" PRId64 "d%" PRId64 "h", sec / 86400, (sec % 86400) / 3600);
    else if (sec >= 3600) snprintf(b, sz, "%" PRId64 "h%" PRId64 "m", sec / 3600, (sec % 3600) / 60);
    else snprintf(b, sz, "%" PRId64 "m", sec / 60);
}

static void fmt_cents(char *b, size_t sz, int32_t cents) {
    snprintf(b, sz, "%s%.2f", cents >= 0 ? "" : "-", ((double)(cents < 0 ? -cents : cents)) / 100.0);
}

static void fmt_md(char *b, size_t sz, int days_ago) {
    time_t t = time(NULL) - (time_t)days_ago * 86400;
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(b, sz, "%d/%d", tm.tm_mon + 1, tm.tm_mday);
}

/* ── Update ── */
void ui_update(ui_elements_t *ui, app_state_t *s) {
    if (!ui || !s) return;
    char b[192], b2[64];

    /* ── TopBar ── */
    if (s->time_valid) {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        const char *wd[] = {"日","一","二","三","四","五","六"};
        snprintf(b, sizeof(b), "%02d/%02d 周%s %02d:%02d",
                 tm.tm_mon + 1, tm.tm_mday, wd[tm.tm_wday], tm.tm_hour, tm.tm_min);
    } else snprintf(b, sizeof(b), "--/-- --:--");
    lv_label_set_text(ui->clock_label, b);

    switch (s->net) {
        case NET_CONNECTED:   lv_label_set_text(ui->wifi_label, "WiFi");   break;
        case NET_RADIO_SLEEP: lv_label_set_text(ui->wifi_label, "休眠");   break;
        case NET_CONNECTING:  lv_label_set_text(ui->wifi_label, "连接");   break;
        case NET_PORTAL:      lv_label_set_text(ui->wifi_label, "配网");   break;
        case NET_UNCONFIGURED:lv_label_set_text(ui->wifi_label, "未配置"); break;
        default:              lv_label_set_text(ui->wifi_label, "离线");   break;
    }

    if (s->battery_configured) snprintf(b, sizeof(b), "%d%%", s->battery_pct);
    else b[0] = '\0';
    lv_label_set_text(ui->bat_label, b);

    /* ── 智谱卡片 ── */
    if (s->glm.valid) {
        snprintf(b, sizeof(b), "%d%%", s->glm.win_5h.pct);
        lv_label_set_text(ui->glm5h_pct, b);
        lv_bar_set_value(ui->glm5h_bar, s->glm.win_5h.pct, LV_ANIM_OFF);

        snprintf(b, sizeof(b), "%d%%", s->glm.win_w.pct);
        lv_label_set_text(ui->glmw_pct, b);
        lv_bar_set_value(ui->glmw_bar, s->glm.win_w.pct, LV_ANIM_OFF);

        snprintf(b, sizeof(b), "剩余积分   5h %u/%u   周 %u/%u",
                 (unsigned)s->glm.win_5h.remaining, (unsigned)s->glm.win_5h.usage,
                 (unsigned)s->glm.win_w.remaining, (unsigned)s->glm.win_w.usage);
        lv_label_set_text(ui->glm_credit, b);

        char r5[16], rw[16];
        fmt_reset(r5, sizeof(r5), s->glm.win_5h.reset_ms);
        fmt_reset(rw, sizeof(rw), s->glm.win_w.reset_ms);
        snprintf(b, sizeof(b), "重置倒计时   5h %s   周 %s", r5, rw);
        lv_label_set_text(ui->glm_reset, b);

        lv_label_set_text(ui->glm_err, "");
    } else {
        lv_label_set_text(ui->glm5h_pct, "--");
        lv_label_set_text(ui->glmw_pct, "--");
        lv_label_set_text(ui->glm_credit, "");
        lv_label_set_text(ui->glm_reset, "");
        lv_label_set_text(ui->glm_err, s->glm.err[0] ? s->glm.err : "查询中...");
    }

    /* ── DeepSeek 卡片 ── */
    if (s->dsk.valid) {
        snprintf(b, sizeof(b), "%.2f", s->dsk.total);
        lv_label_set_text(ui->dsk_balance, b);
        lv_obj_align_to(ui->dsk_currency, ui->dsk_balance, LV_ALIGN_OUT_RIGHT_BOTTOM, 3, -3);

        lv_label_set_text(ui->dsk_state,
                          !s->dsk.is_available ? "账户停用" :
                          (s->dsk.total < 10.0 ? "余额偏低" : ""));

        snprintf(b, sizeof(b), "赠金余额 %.2f 元    充值余额 %.2f 元",
                 s->dsk.granted, s->dsk.topped);
        lv_label_set_text(ui->dsk_detail, b);

        char tb[16], mb[16];
        fmt_cents(tb, sizeof(tb), s->hist.today_cents);
        fmt_cents(mb, sizeof(mb), s->hist.month_cents);
        snprintf(b, sizeof(b), "今日消费 %s 元    本月消费 %s 元", tb, mb);
        lv_label_set_text(ui->dsk_spend, b);

        lv_label_set_text(ui->dsk_err, "");
    } else {
        lv_label_set_text(ui->dsk_balance, "--");
        lv_obj_align_to(ui->dsk_currency, ui->dsk_balance, LV_ALIGN_OUT_RIGHT_BOTTOM, 3, -3);
        lv_label_set_text(ui->dsk_state, "");
        lv_label_set_text(ui->dsk_detail, "");
        lv_label_set_text(ui->dsk_spend, "");
        lv_label_set_text(ui->dsk_err, s->dsk.err[0] ? s->dsk.err : "查询中...");
    }

    /* ── 底部状态 ── */
    {
        uint32_t g = s->glm.last_ok_ms, d = s->dsk.last_ok_ms;
        if (g || d) {
            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
            uint32_t latest = g > d ? g : d;
            uint32_t mins = (now - latest) / 60000;
            if (mins == 0) snprintf(b, sizeof(b), "数据已更新");
            else snprintf(b, sizeof(b), "更新于 %" PRIu32 " 分钟前", mins);
        } else snprintf(b, sizeof(b), "等待数据...");
        lv_label_set_text(ui->status_label, b);
    }

    /* ── 柱状图 (最近 30 天, cents 末位=今天) ── */
    {
        int32_t vals[CHART_DAYS];
        int32_t vmax = 0;
        int32_t sum30 = 0, sum_valid = 0, valid_days = 0, peak = 0, peak_idx = -1;

        for (int i = 0; i < CHART_DAYS; i++) {
            vals[i] = s->hist.cents[HIST_DAYS - CHART_DAYS + i];
            if (vals[i] > vmax) vmax = vals[i];
            if (vals[i] > 0) {
                sum30 += vals[i];
                sum_valid += vals[i];
                valid_days++;
                if (vals[i] > peak) { peak = vals[i]; peak_idx = i; }
            }
        }
        /* 刻度取整: 至少 5 元, 按 2 的倍数向上取整, 保证图标好看 */
        int32_t scale = 500;
        while (scale < vmax) scale *= 2;
        if (scale < 500) scale = 500;

        for (int i = 0; i < CHART_DAYS; i++) {
            int h = 0;
            if (vals[i] > 0) {
                h = (int)(((int64_t)vals[i] * CHART_MAX_H) / scale);
                if (h < 2) h = 2;                 /* 有消费至少显示 2px */
                if (h > CHART_MAX_H) h = CHART_MAX_H;
            }
            lv_obj_set_pos(ui->chart_bars[i],
                           CHART_X0 + i * (CHART_BAR_W + CHART_GAP),
                           CHART_BASE_Y - h);
            lv_obj_set_size(ui->chart_bars[i], CHART_BAR_W, h);
        }

        snprintf(b, sizeof(b), "满格 %d 元", (int)(scale / 100));
        lv_label_set_text(ui->chart_scale, b);

        /* 横轴日期 */
        if (s->time_valid) {
            fmt_md(b, sizeof(b), CHART_DAYS - 1);        /* 起始日 */
            lv_label_set_text(ui->chart_axis[0], b);
            fmt_md(b, sizeof(b), (CHART_DAYS - 1) / 2);  /* 中间日 */
            lv_label_set_text(ui->chart_axis[1], b);
            lv_label_set_text(ui->chart_axis[2], "今天");
        } else {
            lv_label_set_text(ui->chart_axis[0], "");
            lv_label_set_text(ui->chart_axis[1], "");
            lv_label_set_text(ui->chart_axis[2], "");
        }

        /* 统计信息 */
        char tb[16], mb[16], sb[16];
        fmt_cents(tb, sizeof(tb), s->hist.today_cents);
        fmt_cents(mb, sizeof(mb), s->hist.month_cents);
        fmt_cents(sb, sizeof(sb), sum30);
        snprintf(b, sizeof(b), "今日 %s 元    本月 %s 元    30天 %s 元", tb, mb, sb);
        lv_label_set_text(ui->chart_info, b);

        if (valid_days > 0) {
            char ab[16], pb[16];
            fmt_cents(ab, sizeof(ab), sum_valid / valid_days);
            fmt_cents(pb, sizeof(pb), peak);
            char pdate[16] = "";
            if (peak_idx >= 0 && s->time_valid)
                fmt_md(pdate, sizeof(pdate), CHART_DAYS - 1 - peak_idx);
            snprintf(b, sizeof(b), "日均 %s 元    最高 %s 元 (%s)",
                     ab, pb, peak_idx == CHART_DAYS - 1 ? "今天" : pdate);
        } else {
            snprintf(b, sizeof(b), "暂无消费数据");
        }
        lv_label_set_text(ui->chart_info2, b);
    }
    /* ── 天气页 ── */
    {
        lv_label_set_text(ui->wx_city, s->wx.city[0] ? s->wx.city : "定位中...");

        if (s->wx.valid) {
            lv_label_set_text(ui->wx_icon, wmo_icon(s->wx.code));

            snprintf(b, sizeof(b), "%.1f", s->wx.temp_x10 / 10.0);
            lv_label_set_text(ui->wx_temp, b);
            lv_obj_align_to(ui->wx_unit, ui->wx_temp, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -6);

            lv_label_set_text(ui->wx_desc, wmo_text(s->wx.code));
            snprintf(b, sizeof(b), "湿度 %d%%", s->wx.humidity);
            lv_label_set_text(ui->wx_hum, b);

            uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
            uint32_t mins = (now - s->wx.last_ok_ms) / 60000;
            if (mins == 0) snprintf(b, sizeof(b), "刚刚更新");
            else snprintf(b, sizeof(b), "更新于 %" PRIu32 " 分钟前", mins);
            lv_label_set_text(ui->wx_meta, b);
        } else {
            lv_label_set_text(ui->wx_icon, "");
            lv_label_set_text(ui->wx_temp, "--");
            lv_obj_align_to(ui->wx_unit, ui->wx_temp, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -6);
            lv_label_set_text(ui->wx_desc, s->wx.err[0] ? s->wx.err : "查询中...");
            lv_label_set_text(ui->wx_hum, "");
            lv_label_set_text(ui->wx_meta, "");
        }

        /* 4 列预报 */
        static const char *WD[] = {"周日","周一","周二","周三","周四","周五","周六"};
        for (int i = 0; i < WX_DAYS; i++) {
            char day[12];
            if (i == 0) strlcpy(day, "今天", sizeof(day));
            else if (i == 1) strlcpy(day, "明天", sizeof(day));
            else if (s->time_valid) {
                time_t t = time(NULL) + (time_t)i * 86400;
                struct tm tm;
                localtime_r(&t, &tm);
                strlcpy(day, WD[tm.tm_wday % 7], sizeof(day));
            } else strlcpy(day, "--", sizeof(day));
            lv_label_set_text(ui->wx_day[i], day);

            lv_label_set_text(ui->wx_day_icon[i],
                              s->wx.valid ? wmo_icon(s->wx.dcode[i]) : "");

            if (s->wx.valid)
                snprintf(b, sizeof(b), "%.0f/%.0f",
                         s->wx.tmax_x10[i] / 10.0, s->wx.tmin_x10[i] / 10.0);
            else
                snprintf(b, sizeof(b), "--");
            lv_label_set_text(ui->wx_day_temp[i], b);
        }
    }
}

void ui_show_page(ui_elements_t *ui, int page) {
    lv_obj_add_flag(ui->page_main, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui->page_heat, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui->page_portal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui->page_weather, LV_OBJ_FLAG_HIDDEN);
    if (page == 2)      lv_obj_remove_flag(ui->page_portal, LV_OBJ_FLAG_HIDDEN);
    else if (page == 1) lv_obj_remove_flag(ui->page_heat, LV_OBJ_FLAG_HIDDEN);
    else if (page == 3) lv_obj_remove_flag(ui->page_weather, LV_OBJ_FLAG_HIDDEN);
    else                lv_obj_remove_flag(ui->page_main, LV_OBJ_FLAG_HIDDEN);
}
