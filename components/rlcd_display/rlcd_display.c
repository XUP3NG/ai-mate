/**
 * Standalone RLCD driver for Waveshare ESP32-S3-RLCD-4.2
 * ST7305, 400x300 reflective B/W LCD, SPI interface.
 *
 * Rewritten for ESP-IDF with direct SPI transactions (no esp_lcd_panel_io).
 * 源自微雪官方 demo + Arduino 原版驱动。
 */
#include "rlcd_display.h"

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"

static const char *TAG = "rlcd";

/* SPI host */
#define RLCD_SPI_HOST   SPI3_HOST

/* State */
static spi_device_handle_t s_spi_dev = NULL;
static lv_display_t       *s_display = NULL;
static rlcd_hook_t          s_post_render_hook = NULL;

void rlcd_set_post_render_hook(rlcd_hook_t hook) {
    s_post_render_hook = hook;
}

/* Display geometry */
static int s_width  = 400;
static int s_height = 300;
static int s_dc     = -1;

/* Frame buffer (1bpp, packed as 2x4 pixel groups, stored in PSRAM) */
static uint8_t *s_fb = NULL;
static int      s_fb_len = 0;

/* Pixel lookup tables (PSRAM) */
#define FB_COLS   (400 / 2)   /* 200 */
#define FB_ROWS   (300 / 4)   /* 75  */

/* ── Low-level SPI ───────────────────────────────────────────── */

static void spi_cmd(uint8_t cmd)
{
    spi_transaction_t t = {
        .length    = 8,
        .tx_buffer = &cmd,
        .user      = (void *)0,  /* DC low = command */
    };
    spi_device_polling_transmit(s_spi_dev, &t);
}

static void spi_data(uint8_t data)
{
    spi_transaction_t t = {
        .length    = 8,
        .tx_buffer = &data,
        .user      = (void *)1,  /* DC high = data */
    };
    spi_device_polling_transmit(s_spi_dev, &t);
}

static void spi_data_buf(const uint8_t *buf, int len)
{
    spi_transaction_t t = {
        .length    = len * 8,
        .tx_buffer = buf,
        .user      = (void *)1,
    };
    spi_device_polling_transmit(s_spi_dev, &t);
}

/* ── DC callback for spi_device ──────────────────────────────── */

static void spi_pre_transfer_cb(spi_transaction_t *t)
{
    gpio_set_level((gpio_num_t)s_dc, (int)(t->user) ? 1 : 0);
}

/* ── Hardware reset ──────────────────────────────────────────── */

static void hw_reset(int rst_pin)
{
    gpio_set_level((gpio_num_t)rst_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level((gpio_num_t)rst_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level((gpio_num_t)rst_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

/* ── Init sequence (微雪官方 ST7305 初始化序列) ────────────── */

static void hw_init(int rst_pin)
{
    hw_reset(rst_pin);

    spi_cmd(0xD6); spi_data(0x17); spi_data(0x02);
    spi_cmd(0xD1); spi_data(0x01);
    spi_cmd(0xC0); spi_data(0x11); spi_data(0x04);

    spi_cmd(0xC1);
    for (int i = 0; i < 4; i++) spi_data(0x69);

    spi_cmd(0xC2);
    for (int i = 0; i < 4; i++) spi_data(0x19);

    spi_cmd(0xC4);
    for (int i = 0; i < 4; i++) spi_data(0x4B);

    spi_cmd(0xC5);
    for (int i = 0; i < 4; i++) spi_data(0x19);

    spi_cmd(0xD8); spi_data(0x80); spi_data(0xE9);
    spi_cmd(0xB2); spi_data(0x02);

    spi_cmd(0xB3);
    {
        uint8_t d[] = {0xE5,0xF6,0x05,0x46,0x77,0x77,0x77,0x77,0x76,0x45};
        for (int i = 0; i < 10; i++) spi_data(d[i]);
    }

    spi_cmd(0xB4);
    {
        uint8_t d[] = {0x05,0x46,0x77,0x77,0x77,0x77,0x76,0x45};
        for (int i = 0; i < 8; i++) spi_data(d[i]);
    }

    spi_cmd(0x62); spi_data(0x32); spi_data(0x03); spi_data(0x1F);
    spi_cmd(0xB7); spi_data(0x13);
    spi_cmd(0xB0); spi_data(0x64);

    /* Sleep out */
    spi_cmd(0x11);
    vTaskDelay(pdMS_TO_TICKS(200));

    spi_cmd(0xC9); spi_data(0x00);
    spi_cmd(0x36); spi_data(0x48);   /* MADCTL: landscape */
    spi_cmd(0x3A); spi_data(0x11);   /* COLMOD: 1bpp */
    spi_cmd(0xB9); spi_data(0x20);
    spi_cmd(0xB8); spi_data(0x29);
    spi_cmd(0x21);                   /* INVON */

    /* Set column address (横屏: 18-42) */
    spi_cmd(0x2A); spi_data(0x12); spi_data(0x2A);
    /* Set row address (0-199) */
    spi_cmd(0x2B); spi_data(0x00); spi_data(0xC7);

    spi_cmd(0x35); spi_data(0x00);   /* TEON */
    spi_cmd(0xD0); spi_data(0xFF);
    spi_cmd(0x38);
    spi_cmd(0x29);                   /* DISPON */

    ESP_LOGI(TAG, "ST7305 init sequence complete");
}

/* ── Frame buffer flush ──────────────────────────────────────── */

static void fb_flush(void)
{
    spi_cmd(0x2A); spi_data(0x12); spi_data(0x2A);
    spi_cmd(0x2B); spi_data(0x00); spi_data(0xC7);
    spi_cmd(0x2C);  /* RAMWR */
    spi_data_buf(s_fb, s_fb_len);
}

/* ── Set pixel (横屏布局: 每字节 2×4 像素) ──────────────────── */

static void fb_set_pixel(int x, int y, int white)
{
    if (x < 0 || x >= s_width || y < 0 || y >= s_height) return;

    int inv_y   = s_height - 1 - y;
    int byte_x  = x >> 1;
    int block_y = inv_y >> 2;
    int local_x = x & 1;
    int local_y = inv_y & 3;

    int idx = byte_x * FB_ROWS + block_y;
    int bit = 7 - ((local_y << 1) | local_x);

    if (white)
        s_fb[idx] |=  (1 << bit);
    else
        s_fb[idx] &= ~(1 << bit);
}

/* ── LVGL flush callback: RGB565 → 1-bit 纯黑白 ─── */

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *color_p)
{
    (void)disp;
    uint16_t *buf = (uint16_t *)color_p;
    for (int y = area->y1; y <= area->y2; y++) {
        for (int x = area->x1; x <= area->x2; x++) {
            uint16_t c = *buf++;
            int r8 = ((c >> 11) & 0x1F) << 3;
            int g8 = ((c >> 5)  & 0x3F) << 2;
            int b8 = (c & 0x1F) << 3;
            int lum = (r8 * 38 + g8 * 75 + b8 * 15) >> 7;  /* 0..255 */
            fb_set_pixel(x, y, (lum >= 80) ? 1 : 0);
        }
    }
    /* 调用后处理钩子 (像素图标等), 再统一刷屏 */
    if (s_post_render_hook) s_post_render_hook();
    fb_flush();
    lv_disp_flush_ready(disp);
}

/* ── Public API ───────────────────────────────────────────────── */

esp_err_t rlcd_init(const rlcd_config_t *cfg)
{
    ESP_LOGI(TAG, "Init RLCD %dx%d (direct SPI)", cfg->width, cfg->height);

    s_width  = cfg->width;
    s_height = cfg->height;
    s_dc     = cfg->dc;
    s_fb_len = FB_COLS * FB_ROWS;  /* 200 * 75 = 15000 */

    /* Alloc frame buffer in PSRAM */
    s_fb = heap_caps_malloc(s_fb_len, MALLOC_CAP_SPIRAM);
    if (!s_fb) {
        ESP_LOGE(TAG, "FB PSRAM alloc failed");
        return ESP_ERR_NO_MEM;
    }
    memset(s_fb, 0xFF, s_fb_len);  /* Start with white */

    /* Configure GPIOs */
    gpio_set_direction((gpio_num_t)cfg->dc, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)cfg->dc, 1);

    gpio_set_direction((gpio_num_t)cfg->cs, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)cfg->cs, 1);

    gpio_set_direction((gpio_num_t)cfg->rst, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)cfg->rst, 1);

    /* SPI bus */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = cfg->mosi,
        .miso_io_num     = -1,
        .sclk_io_num     = cfg->scl,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = s_fb_len,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(RLCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    /* SPI device (attach to bus) */
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = 10 * 1000 * 1000,  /* 10 MHz */
        .mode           = 0,
        .spics_io_num   = cfg->cs,
        .queue_size     = 7,
        .pre_cb         = spi_pre_transfer_cb,  /* controls DC pin */
    };
    ESP_ERROR_CHECK(spi_bus_add_device(RLCD_SPI_HOST, &dev_cfg, &s_spi_dev));

    /* Hardware init */
    hw_init(cfg->rst);

    /* Flush white frame buffer to screen */
    fb_flush();
    ESP_LOGI(TAG, "Screen initialized to white");

    /* Test: alternate black/white to verify SPI is working */
    vTaskDelay(pdMS_TO_TICKS(100));
    memset(s_fb, 0x00, s_fb_len);
    fb_flush();
    ESP_LOGI(TAG, "Test: screen black");
    vTaskDelay(pdMS_TO_TICKS(200));
    memset(s_fb, 0xFF, s_fb_len);
    fb_flush();
    ESP_LOGI(TAG, "Test: screen white again");

    /* LVGL */
    lv_init();
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority   = 2;
    port_cfg.timer_period_ms = 50;
    lvgl_port_init(&port_cfg);

    lvgl_port_lock(0);
    s_display = lv_display_create(s_width, s_height);
    lv_display_set_flush_cb(s_display, lvgl_flush_cb);

    size_t lvgl_buf_sz = LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565)
                         * s_width * s_height;
    uint8_t *lvgl_buf  = heap_caps_malloc(lvgl_buf_sz, MALLOC_CAP_SPIRAM);
    assert(lvgl_buf);
    lv_display_set_buffers(s_display, lvgl_buf, NULL, lvgl_buf_sz,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "RLCD + LVGL ready");
    return ESP_OK;
}

lv_display_t *rlcd_get_display(void)
{
    return s_display;
}

void rlcd_fb_set_pixel(int x, int y, int white)
{
    fb_set_pixel(x, y, white);
}

void rlcd_fb_flush(void)
{
    fb_flush();
}

bool rlcd_lock(int timeout_ms)
{
    return lvgl_port_lock(timeout_ms);
}

void rlcd_unlock(void)
{
    lvgl_port_unlock();
}