#pragma once
// WT32-SC01 Plus: ST7796 (i80, 8 Bit) + FT6336U Touch, LVGL 8.4 Anbindung
#include <stdbool.h>
#include "esp_err.h"

#define LCD_H_RES 480 // Querformat
#define LCD_V_RES 320

esp_err_t board_init(void);      // Display, Touch, LVGL, LVGL-Task
bool board_lvgl_lock(int timeout_ms);
void board_lvgl_unlock(void);
void board_set_brightness(int pct); // 0..100 %
