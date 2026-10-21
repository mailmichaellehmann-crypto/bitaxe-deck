file(GLOB FONT_SRCS "fonts/*.c")
idf_component_register(
    SRCS "main.c" "board.c" "settings.c" "net.c" "data.c" "ui.c" "shares.c" "ota.c" ${FONT_SRCS}
    INCLUDE_DIRS "."
    REQUIRES esp_lcd esp_driver_gpio esp_driver_i2c esp_timer nvs_flash esp_wifi esp_netif esp_event
             esp_http_client esp_http_server esp_https_ota app_update esp_app_format json mbedtls lvgl__lvgl espressif__esp_lcd_st7796 espressif__esp_websocket_client
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format-truncation)
