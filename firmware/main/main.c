// Satoshi Deck – Bitcoin- & Bitaxe-Dashboard für das WT32-SC01 Plus (ESP32-S3, 480x320)
#include "board.h"
#include "data.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "net.h"
#include "nvs_flash.h"
#include "settings.h"
#include "shares.h"
#include "ota.h"
#include "ui.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    settings_load();
    data_start(); // vor der Oberfläche: legt Sperre und Demo-Verlauf an
    shares_start();
    ota_start(); // vor der Oberfläche: legt die Sperre für den Update-Status an

    ESP_ERROR_CHECK(board_init());
    if (board_lvgl_lock(-1)) {
        ui_init();
        board_lvgl_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(120)); // erstes Bild zeichnen, dann Licht an
    board_set_brightness(g_settings.brightness);

    net_init();
    ESP_LOGI("main", "Satoshi Deck gestartet, freier interner RAM %u, PSRAM %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
