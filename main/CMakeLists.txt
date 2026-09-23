idf_component_register(
    SRCS
        "cc_mate.c"
        "config_store.c"
        "wifi_mgr.c"
        "net_query.c"
        "weather.c"
        "ui/ui.c"
        "ui/font_cjk_16.c"
        "ui/font_wx_icon_36.c"
        "ui/font_wx_icon_24.c"
        "ui/font_wx_num_36.c"
    INCLUDE_DIRS
        "."
        "ui"
    REQUIRES
        driver esp_lcd esp_lvgl_port lvgl heap rlcd_display esp_wifi
        esp_http_server esp_http_client esp-tls esp_netif lwip json
        nvs_flash freertos esp_timer esp_adc
)
