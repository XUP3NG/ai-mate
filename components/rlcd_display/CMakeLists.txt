idf_component_register(
    SRCS "rlcd_display.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver esp_lcd esp_lvgl_port lvgl heap
)
