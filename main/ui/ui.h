#pragma once
#include <lvgl.h>
#include "cc_mate.h"

#define CHART_DAYS 30    /* 柱状图显示最近 30 天 */

/* 横向网格虚线: 1-bit 屏无法画灰色, 用黑色虚线表示"参考线" */
#define CHART_DASH_W   8
#define CHART_DASH_GAP 7
#define CHART_DASH_N   26   /* 每条网格线的虚线段数 */

typedef struct {
    /* TopBar */
    lv_obj_t *top_bar;
    lv_obj_t *title_label;
    lv_obj_t *clock_label;
    lv_obj_t *wifi_label;
    lv_obj_t *bat_label;

    /* ── Page 0: 额度总览 ── */
    lv_obj_t *page_main;

    /* 智谱卡片 */
    lv_obj_t *glm_title;
    lv_obj_t *glm5h_label;
    lv_obj_t *glm5h_bar;
    lv_obj_t *glm5h_pct;
    lv_obj_t *glmw_label;
    lv_obj_t *glmw_bar;
    lv_obj_t *glmw_pct;
    lv_obj_t *glm_credit;      /* 剩余积分行 */
    lv_obj_t *glm_reset;       /* 重置倒计时行 */
    lv_obj_t *glm_err;

    /* DeepSeek 卡片 */
    lv_obj_t *dsk_title;
    lv_obj_t *dsk_state;       /* 右对齐: 可用/停用 */
    lv_obj_t *dsk_balance;     /* 大号数字 (Montserrat 20) */
    lv_obj_t *dsk_currency;    /* "元" 紧贴余额右侧 (CJK 字体) */
    lv_obj_t *dsk_detail;      /* 赠金/充值 */
    lv_obj_t *dsk_spend;       /* 今日/本月消费 */
    lv_obj_t *dsk_err;

    lv_obj_t *status_label;    /* 底部更新时间 */

    /* ── Page 1: 消费柱状图 ── */
    lv_obj_t *page_heat;
    lv_obj_t *chart_title;
    lv_obj_t *chart_scale;     /* 右上: 满格 xx 元 */
    lv_obj_t *chart_bars[CHART_DAYS];
    lv_obj_t *chart_dash[3][CHART_DASH_N];  /* 3 条横向虚线网格 */
    lv_obj_t *chart_baseline;
    lv_obj_t *chart_axis[3];   /* 横轴: 起 / 中 / 今 */
    lv_obj_t *chart_info;      /* 今日/本月/30天 */
    lv_obj_t *chart_info2;     /* 日均/最高 */

    /* ── Page 3: 天气 ──
     * 上部: 城市 + 更新时间 | 36px 图标 + 36px 温度 + 描述 + 湿度
     * 下部: 4 列预报 (日期 / 24px 图标 / 高低温) + 底部数据源 */
    lv_obj_t *page_weather;
    lv_obj_t *wx_city;             /* 左上: 城市 */
    lv_obj_t *wx_meta;             /* 右上: 更新时间 / 错误 */
    lv_obj_t *wx_icon;             /* 当前天气大图标 (font_wx_icon_36) */
    lv_obj_t *wx_temp;             /* 当前温度 (font_wx_num_36) */
    lv_obj_t *wx_unit;             /* "度" */
    lv_obj_t *wx_desc;             /* 天气文字 */
    lv_obj_t *wx_hum;              /* 湿度 */
    lv_obj_t *wx_day[WX_DAYS];         /* 预报日期标签 (今天/明天/周X) */
    lv_obj_t *wx_day_icon[WX_DAYS];    /* 预报图标 (font_wx_icon_24) */
    lv_obj_t *wx_day_temp[WX_DAYS];    /* 预报高低温 (montserrat_20) */
    lv_obj_t *wx_footer;               /* 数据源 */

    /* ── Page 2: 配网提示 ── */
    lv_obj_t *page_portal;
    lv_obj_t *portal_ap;
    lv_obj_t *portal_url;
} ui_elements_t;

void ui_init(ui_elements_t *ui);
void ui_update(ui_elements_t *ui, app_state_t *s);
void ui_show_page(ui_elements_t *ui, int page);   /* 0=总览 1=柱状图 2=配网 */
