#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * RLCD display configuration (Waveshare ESP32-S3-RLCD-4.2)
 * 400x300 reflective LCD, SPI interface, black/white
 */
typedef struct {
    int mosi;   /* GPIO 12 */
    int scl;    /* GPIO 11 */
    int dc;     /* GPIO 5  */
    int cs;     /* GPIO 40 */
    int rst;    /* GPIO 41 */
    int width;  /* 400 */
    int height; /* 300 */
} rlcd_config_t;

/**
 * Initialize RLCD + LVGL.
 * Must be called once before any LVGL operations.
 * Returns the lv_display_t handle.
 */
esp_err_t rlcd_init(const rlcd_config_t *cfg);

/**
 * Get the LVGL display handle (valid after rlcd_init).
 */
lv_display_t *rlcd_get_display(void);

/**
 * Lock/unlock LVGL mutex (use around all lv_* calls).
 */
bool rlcd_lock(int timeout_ms);
void rlcd_unlock(void);

/**
 * Direct pixel write to frame buffer (for icon drawing).
 * x=0..399, y=0..299, white=1 → white pixel, 0 → black pixel.
 */
void rlcd_fb_set_pixel(int x, int y, int white);

/**
 * Flush frame buffer to display.
 */
void rlcd_fb_flush(void);

/** 注册回调: LVGL flush 后、SPI 发送前调用 (用于叠加像素图标) */
typedef void (*rlcd_hook_t)(void);
void rlcd_set_post_render_hook(rlcd_hook_t hook);

#ifdef __cplusplus
}
#endif
