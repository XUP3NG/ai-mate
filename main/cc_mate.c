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
#include "ui/ui.h"
#include "rlcd_display.h"
#include "esp_lvgl_port.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

static const char *TAG = "ai_mate";

static app_config_t s_cfg;
static app_state_t  s_state;
static ui_elements_t s_ui;

/* ── 电池 ADC (可选) ── */
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#define BAT_ADC_CHANNEL  ADC_CHANNEL_3   /* GPIO2 */
#define BAT_ADC_UNIT     ADC_UNIT_1

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

static uint8_t bat_read_level(void) {
    if (!s_adc1) return 0;
    int raw_sum = 0;
    for (int i = 0; i < 8; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc1, BAT_ADC_CHANNEL, &raw) == ESP_OK) raw_sum += raw;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    int mv = 0;
    adc_cali_raw_to_voltage(s_adc1_cali, raw_sum / 8, &mv);
    float vbat = mv * 0.001f * 3.0f;
    if (vbat <= 3.0f) return 0;
    if (vbat >= 4.12f) return 100;
    static float s_vbat_ema = -1.0f;
    if (s_vbat_ema < 0) s_vbat_ema = vbat;
    else s_vbat_ema = s_vbat_ema * 0.85f + vbat * 0.15f;
    return (uint8_t)((s_vbat_ema - 3.0f) / (4.12f - 3.0f) * 100.0f);
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

        ESP_LOGI(TAG, "poll round");
        net_query_poll(&s_state, &s_cfg);
        weather_query(&s_state, &s_cfg);

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

        /* 电池: 每 30s (WiFi 模式下电流波动小) */
        if (now - last_bat > 30000) {
            s_state.battery_pct = bat_read_level();
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
