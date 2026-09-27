/**
 * AI_Mate — AI 额度监控桌面摆件 (ESP-IDF)
 *
 * 芯片: ESP32-S3
 * 屏幕: Waveshare ESP32-S3-RLCD-4.2 (ST7305, 400×300 B/W, SPI)
 *
 * 功能:
 *   WiFi 直连查询智谱 GLM Coding Plan 额度 + DeepSeek 余额
 *   余额快照推算每日消费 → 热力图
 *   AP 配网门户 (长按 BOOT 3s 重新配网)
 */

#include "cc_mate.h"
#include "config_store.h"
#include "wifi_mgr.h"
#include "net_query.h"
#include "weather.h"
#include "shtc3.h"
#include "ui/ui.h"
#include "rlcd_display.h"
#include "esp_lvgl_port.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

static const char *TAG = "ai_mate";

static app_config_t s_cfg;
static app_state_t  s_state;
static ui_elements_t s_ui;

/* ── 电池 ADC ──
 *
 * 采样要点:
 *   1. 只在射频关闭时采 (WiFi 发射瞬间电压会掉 0.1~0.2V, 放电曲线用的是开路电压 OCV)
 *   2. 多采样 + 丢弃前几次 (ADC 首次转换偏差大)
 *   3. 电量用 OCV 查表插值, 不用线性映射 (锂电 3.7~3.95V 是平台区, 线性会差 30%+)
 *   4. 分压比可在配网页校准 (不同板子 1:2 / 1:3 不一)
 */
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#define BAT_ADC_CHANNEL  ADC_CHANNEL_3   /* GPIO2 */
#define BAT_ADC_UNIT     ADC_UNIT_1
#define BAT_SAMPLES      16
#define BAT_DISCARD      4

typedef struct { uint16_t mv; uint8_t pct; } soc_point_t;
/* 单体锂电开路电压 → 剩余电量 (轻载实测经验曲线) */
static const soc_point_t SOC_CURVE[] = {
    { 4200, 100 }, { 4150, 95 }, { 4110, 90 }, { 4080, 85 }, { 4020, 80 },
    { 3980, 75 },  { 3940, 70 }, { 3910, 65 }, { 3870, 60 }, { 3840, 55 },
    { 3820, 50 },  { 3800, 45 }, { 3790, 40 }, { 3780, 35 }, { 3770, 30 },
    { 3760, 25 },  { 3740, 20 }, { 3720, 15 }, { 3680, 10 }, { 3610, 5  },
    { 3500, 3  },  { 3300, 0  },
};
#define SOC_N (sizeof(SOC_CURVE) / sizeof(SOC_CURVE[0]))

static adc_oneshot_unit_handle_t s_adc1 = NULL;
static adc_cali_handle_t s_adc1_cali = NULL;

static void bat_adc_init(void) {
    adc_cali_curve_fitting_config_t cali = {
        .unit_id  = BAT_ADC_UNIT,
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali, &s_adc1_cali) != ESP_OK) return;
    adc_oneshot_unit_init_cfg_t init_cfg = { .unit_id = BAT_ADC_UNIT };
    if (adc_oneshot_new_unit(&init_cfg, &s_adc1) != ESP_OK) return;
    adc_oneshot_chan_cfg_t chan_cfg = { .bitwidth = ADC_BITWIDTH_12, .atten = ADC_ATTEN_DB_12 };
    if (adc_oneshot_config_channel(s_adc1, BAT_ADC_CHANNEL, &chan_cfg) != ESP_OK) return;
    s_state.battery_configured = true;
}

/* 采样一次, 返回 ADC 引脚上的电压 mV (0 = 失败) */
static uint32_t bat_read_pin_mv(void) {
    if (!s_adc1) return 0;
    int sum = 0, n = 0;
    for (int i = 0; i < BAT_SAMPLES; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc1, BAT_ADC_CHANNEL, &raw) == ESP_OK && i >= BAT_DISCARD) {
            sum += raw;
            n++;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (!n) return 0;
    int mv = 0;
    if (adc_cali_raw_to_voltage(s_adc1_cali, sum / n, &mv) != ESP_OK) return 0;
    return (uint32_t)(mv < 0 ? 0 : mv);
}

/* OCV → 电量: 查表 + 线性插值 */
static uint8_t bat_pct_from_mv(uint32_t pack_mv) {
    if (pack_mv >= SOC_CURVE[0].mv) return 100;
    if (pack_mv <= SOC_CURVE[SOC_N - 1].mv) return 0;
    for (size_t i = 0; i + 1 < SOC_N; i++) {
        uint16_t hi = SOC_CURVE[i].mv, lo = SOC_CURVE[i + 1].mv;
        if (pack_mv <= hi && pack_mv >= lo) {
            uint8_t phi = SOC_CURVE[i].pct, plo = SOC_CURVE[i + 1].pct;
            uint32_t span = hi - lo;
            uint32_t num = (uint32_t)(pack_mv - lo) * (phi - plo);
            return (uint8_t)(plo + (num + span / 2) / span);   /* 四舍五入到整百分比 */
        }
    }
    return 0;
}

/* 满电电压参考 (mV): 4.20V 为标准值; 但有些板子充电器稳压在 4.1V 左右,
 * 或 ADC 读数偏低, 会导致永远显示不到 100%。故支持两种方式:
 *   配置项 bat_full_mv > 0 → 用配置值
 *   否则自动学习: 充电中电压稳定 ≥30 分钟 → 认定该电压为满电, 存 NVS */
#define BAT_FULL_NVS_NS   "ai_mate"
#define BAT_FULL_NVS_KEY  "bfull_learned"
#define BAT_FULL_DEFAULT  4200
#define BAT_FULL_STABLE_MV 2          /* 判定"稳定"的波动阈值 */
#define BAT_FULL_STABLE_MS (30 * 60 * 1000)

static uint16_t s_full_mv = BAT_FULL_DEFAULT;
static bool     s_full_auto = true;
static uint16_t s_plateau_mv = 0;
static uint32_t s_plateau_ms = 0;

static void bat_full_load(const app_config_t *cfg) {
    if (cfg->bat_full_mv > 0) {                  /* 手动指定 */
        s_full_mv = cfg->bat_full_mv;
        s_full_auto = false;
        ESP_LOGI(TAG, "battery full ref: %u mV (manual)", s_full_mv);
        return;
    }
    s_full_auto = true;
    nvs_handle_t h;
    uint16_t learned = 0;
    if (nvs_open(BAT_FULL_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u16(h, BAT_FULL_NVS_KEY, &learned);
        nvs_close(h);
    }
    if (learned >= 3800 && learned <= BAT_FULL_DEFAULT) {
        s_full_mv = learned;
        ESP_LOGI(TAG, "battery full ref: %u mV (learned)", s_full_mv);
    } else {
        s_full_mv = BAT_FULL_DEFAULT;
        ESP_LOGI(TAG, "battery full ref: %u mV (default, will learn)", s_full_mv);
    }
}

static void bat_full_save(uint16_t mv) {
    nvs_handle_t h;
    if (nvs_open(BAT_FULL_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u16(h, BAT_FULL_NVS_KEY, mv);
    nvs_commit(h);
    nvs_close(h);
}

/* 读一次电池: 更新 s_state 的电压与电量 (分压比可配, 默认 3.00) */
static void bat_update(void) {
    uint32_t pin_mv = bat_read_pin_mv();
    if (pin_mv == 0) return;

    uint32_t div100 = s_cfg.bat_div_x100 ? s_cfg.bat_div_x100 : 300;
    uint32_t pack_mv = pin_mv * div100 / 100;

    /* 电压做 EMA 平滑 (负载/温度抖动), 电量由平滑后电压查表得出 */
    static float s_mv_ema = -1.0f;
    if (s_mv_ema < 0) s_mv_ema = (float)pack_mv;
    else s_mv_ema = s_mv_ema * 0.7f + (float)pack_mv * 0.3f;

    s_state.battery_mv = (uint16_t)s_mv_ema;

    /* 充电判定: 满充电压, 或短时间内电压持续上升 (深放电时电压低但确在充电) */
    static uint32_t s_trend_t0 = 0;
    static uint16_t s_trend_mv0 = 0;
    static bool s_trend_charging = false;
    static uint32_t s_last_rise_s = 0;               /* 最近一次观察到"在上升"的时刻 */
    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);
    if (s_trend_t0 == 0) {
        s_trend_t0 = now_s;
        s_trend_mv0 = (uint16_t)s_mv_ema;
    } else if (now_s - s_trend_t0 >= 600) {          /* 每 10 分钟评估一次趋势 */
        int32_t delta = (int32_t)s_mv_ema - (int32_t)s_trend_mv0;
        s_trend_charging = (delta >= 15);            /* 10 分钟涨 ≥15mV → 在充 */
        if (delta >= 5) s_last_rise_s = now_s;       /* 记录"确实在上升" */
        if (s_trend_charging) {
            ESP_LOGI(TAG, "BAT trend: +%d mV / %u s → charging", (int)delta,
                     (unsigned)(now_s - s_trend_t0));
        }
        s_trend_t0 = now_s;
        s_trend_mv0 = (uint16_t)s_mv_ema;
    }

    bool charging = (s_mv_ema >= 4150.0f) || s_trend_charging;
    s_state.battery_charging = charging;

    /* ── 自动学习满电电压 ──
     * 充电器进入恒压阶段后电压会长时间纹丝不动 (这时"趋势在充电"已不成立),
     * 故判据为: 电压 ≥4.0V 且 30 分钟内波动 ≤3mV。
     *
     * 但"长时间稳定"在放电时同样成立 (轻载下 30 分钟只降不到 1mV), 若不加限制
     * 会把放电平台误学成"满电", 参考值越学越低、电量虚高。
     * 因此额外要求: 最近 1 小时内观察到过电压上升 (说明确实在充电)。 */
    bool rose_recently = (s_last_rise_s != 0) && ((now_s - s_last_rise_s) <= 3600);
    if (s_full_auto && s_mv_ema >= 4000.0f && rose_recently) {
        if (s_plateau_mv == 0 || abs((int)s_mv_ema - (int)s_plateau_mv) > 3) {
            s_plateau_mv = (uint16_t)s_mv_ema;
            s_plateau_ms = 0;
        } else {
            s_plateau_ms += 30000;                   /* 采样间隔 30s */
            if (s_plateau_ms >= BAT_FULL_STABLE_MS &&
                s_plateau_mv >= BAT_FULL_DEFAULT - 400 && s_plateau_mv < BAT_FULL_DEFAULT) {
                s_full_mv = s_plateau_mv;
                bat_full_save(s_full_mv);
                ESP_LOGW(TAG, "battery full voltage learned: %u mV → 满电显示 100%%", s_full_mv);
                s_plateau_ms = 0;
            }
        }
    } else {
        s_plateau_mv = 0;
        s_plateau_ms = 0;
    }

    /* 电量: 按实际满电电压重标定后再查 OCV 表 (满电即 100%) */
    uint32_t scaled = (s_full_mv > 0) ? (uint32_t)(s_mv_ema * BAT_FULL_DEFAULT / s_full_mv)
                                      : (uint32_t)s_mv_ema;
    s_state.battery_pct = bat_pct_from_mv(scaled);

    ESP_LOGI(TAG, "BAT pin=%" PRIu32 "mV pack=%.0fmV pct=%u%% full=%umV%s%s",
             pin_mv, s_mv_ema, s_state.battery_pct, s_full_mv,
             s_full_auto ? "(auto)" : "(manual)",
             s_state.battery_charging ? " charging" : "");
}

/* 读板载 SHTC3 室内温湿度 (传感器不在位时 shtc3_read 立即返回 false) */
static void indoor_update(void) {
    float t = 0, rh = 0;
    if (!shtc3_read(&t, &rh)) return;               /* 保留上次的值, 不清零 */
    s_state.indoor_valid   = true;
    s_state.indoor_temp_x10 = (int16_t)(t * 10.0f + (t < 0 ? -0.5f : 0.5f));
    s_state.indoor_rh      = (uint8_t)(rh + 0.5f);
    ESP_LOGI(TAG, "INDOOR %.1f°C %u%%", t, s_state.indoor_rh);
}

/* ── 查询任务 (方案B: 查完断网省电) ──
 *
 * 周期: 唤醒 → 连 WiFi → 等时间同步 → 查询 → 关射频 → 睡 poll_min 分钟 → 周而复始
 * 射频关闭期间: 时钟/UI/电池照常, 仅断网。关射频触发的断连事件被 wifi_mgr 屏蔽。
 */
static void net_task(void *arg) {
    (void)arg;
    /* 首轮: 等 WiFi (app_main 已发起连接) */
    int wait = 0;
    while (!net_query_wifi_ok() && wait < 20000) {
        vTaskDelay(pdMS_TO_TICKS(500));
        wait += 500;
    }

    while (1) {
        int period = s_cfg.poll_min >= 1 ? s_cfg.poll_min : 5;
        int wx_period = s_cfg.wx_min >= 5 ? s_cfg.wx_min : 30;

        ESP_LOGI(TAG, "poll round");
        net_query_poll(&s_state, &s_cfg);

        /* 天气与轮询解耦: 默认 30 分钟才查一次 (省配额/省射频时间) */
        static uint32_t s_last_wx = 0;
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (s_last_wx == 0 || (now_ms - s_last_wx) >= (uint32_t)wx_period * 60000) {
            weather_query(&s_state, &s_cfg);
            s_last_wx = now_ms;
        }

        /* 时间未同步 (SNTP 未完成): 再等最多 10s 并补查一次, 保证消费历史有日期 */
        if (!s_state.time_valid) {
            for (int i = 0; i < 20 && !s_state.time_valid; i++) {
                vTaskDelay(pdMS_TO_TICKS(500));
                s_state.time_valid = (time(NULL) > 1700000000);
            }
            if (s_state.time_valid) {
                ESP_LOGI(TAG, "sntp synced late, re-poll");
                net_query_poll(&s_state, &s_cfg);
            }
        }

        /* ── 断网休眠 ── */
        wifi_mgr_radio_sleep();
        vTaskDelay(pdMS_TO_TICKS((uint32_t)period * 60000));

        /* ── 唤醒重连 ── */
        wifi_mgr_radio_wake();
        wait = 0;
        while (!net_query_wifi_ok() && wait < 30000) {
            vTaskDelay(pdMS_TO_TICKS(500));
            wait += 500;
        }
        /* 连不上: 断连事件的重试/门户逻辑接管; 下一轮 poll 会报错并重试 */
    }
}

/* ── BOOT 键任务: 长按 3s → 清 WiFi → 重启进配网 ── */
static void boot_key_task(void *arg) {
    (void)arg;
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << 0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    while (1) {
        int low_ms = 0;
        while (gpio_get_level(GPIO_NUM_0) == 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            low_ms += 50;
            if (low_ms >= 3000) {
                ESP_LOGW(TAG, "BOOT long-press: re-provision");
                config_clear();
                esp_restart();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ── app_main ── */
void app_main(void)
{
    ESP_LOGI(TAG, "=== AI Mate starting ===");

    /* NVS */
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* 电池 ADC (可选) */
    bat_adc_init();

    /* 板载 SHTC3 室内温湿度 (可选; 不在位则静默关闭) */
    shtc3_init();

    /* RLCD + LVGL 初始化 (先起屏幕, 配网页也要显示) */
    rlcd_config_t rlcd_cfg = {
        .width  = DISPLAY_WIDTH,
        .height = DISPLAY_HEIGHT,
        .mosi   = 12,
        .scl    = 11,
        .cs     = 40,
        .dc     = 5,
        .rst    = 41,
    };
    esp_err_t ret = rlcd_init(&rlcd_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RLCD init failed: %s, restarting...", esp_err_to_name(ret));
        esp_restart();
    }

    lvgl_port_lock(-1);
    ui_init(&s_ui);
    ui_update(&s_ui, &s_state);
    lvgl_port_unlock();

    /* WiFi / 配网 */
    wifi_mgr_init();
    bool configured = config_load(&s_cfg);

    if (!configured) {
        ESP_LOGW(TAG, "no config → portal");
        s_state.net = NET_PORTAL;
        lvgl_port_lock(-1);
        ui_show_page(&s_ui, 2);
        ui_update(&s_ui, &s_state);
        lvgl_port_unlock();
        wifi_mgr_start_portal(&s_cfg);   /* 阻塞, 保存后 esp_restart() */
        return;
    }

    s_state.net = NET_CONNECTING;
    net_query_init_time();
    bat_full_load(&s_cfg);           /* 满电电压参考: 配置值或上次学到的 */
    net_hist_sync(&s_state);         /* 先把 NVS 历史读出来, 柱状图开机即有数据 */
    wifi_mgr_connect_best(&s_cfg);   /* 扫描并连接信号最好的已保存网络 */

    xTaskCreatePinnedToCore(net_task, "net", 12288, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(boot_key_task, "bootkey", 3072, NULL, 3, NULL, 0);

    /* 主循环: 页面轮播 + 定期刷新 */
    uint32_t last_bat = 0, last_ui = 0, page_start = 0;
    int page = 0;

    while (1) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

        /* WiFi 状态: 连接 > 射频休眠 > 连接中 */
        if (net_query_wifi_ok()) s_state.net = NET_CONNECTED;
        else if (!wifi_mgr_radio_on()) s_state.net = NET_RADIO_SLEEP;
        else s_state.net = NET_CONNECTING;

        /* 当前 SSID (状态栏显示) */
        strlcpy(s_state.ssid, wifi_mgr_current_ssid(), sizeof(s_state.ssid));

        /* 事件回调请求的配网切换 (在主任务执行, 回调内不可阻塞) */
        if (wifi_mgr_poll_portal()) {
            /* 不会到达: portal 常驻直至保存重启 */
        }

        /* 断线重试耗尽 → 扫描换用其他已保存网络 (阻塞 ~2s) */
        wifi_mgr_poll_rescan();

        /* 电池: 每 30s, 且只在射频关闭时采 (WiFi 发射会拉低电压, 影响 OCV 判读) */
        if (now - last_bat > 30000 && !wifi_mgr_radio_on()) {
            bat_update();
            indoor_update();
            last_bat = now;
        }

        /* 页面轮播: 每 15s 切换 主页/柱状图/天气 (城市留空=IP 自动定位, 天气始终启用) */
        if (now - page_start > 15000) {
            static const int order[] = { 0, 1, 3 };   /* 0=主页 1=柱状图 3=天气 (2=配网不参与轮播) */
            page = (page + 1) % 3;
            page_start = now;
            lvgl_port_lock(-1);
            ui_show_page(&s_ui, order[page]);
            lvgl_port_unlock();
        }

        /* UI: 每 2s (省电; 时钟分钟级, 无需更快) */
        if (now - last_ui > 2000) {
            lvgl_port_lock(-1);
            ui_update(&s_ui, &s_state);
            lvgl_port_unlock();
            last_ui = now;
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
