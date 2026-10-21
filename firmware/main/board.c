// Hardware-Anbindung für das Smart Panlee / WT32-SC01 Plus (ZX3D50CE08S-V16)
// Pins laut Board-Doku: 8080-Bus D0..D7 = 9,46,3,8,18,17,16,15 · WR 47 · DC 0 · RST 4 · BL 45
// Touch FT6336U: I2C SDA 6 · SCL 5 · INT 7 · Adresse 0x38

#include "board.h"

#include <stdlib.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7796.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sdkconfig.h"

static const char *TAG = "board";

#define PIN_LCD_WR 47
#define PIN_LCD_DC 0
#define PIN_LCD_RST 4
#define PIN_LCD_BL 45
static const int PIN_LCD_DATA[8] = {9, 46, 3, 8, 18, 17, 16, 15};

#define PIN_TP_SDA 6
#define PIN_TP_SCL 5
#define PIN_TP_INT 7
#define TP_ADDR 0x38

#define LCD_PCLK_HZ (40 * 1000 * 1000)
#define DRAW_BUF_LINES 80 // größter Versuch, siehe board_init()

static esp_lcd_panel_handle_t s_panel;
static i2c_master_dev_handle_t s_tp;
static SemaphoreHandle_t s_lvgl_mutex;
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t s_disp_drv;
static lv_indev_drv_t s_indev_drv;

// Bild um 180° drehen (falls das Board andersherum eingebaut ist)
#ifdef CONFIG_DECK_DISPLAY_FLIP
static const bool s_flip = true;
#else
static const bool s_flip = false;
#endif

// ------------------------------------------------------------------ Display

static bool on_color_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    lv_disp_flush_ready((lv_disp_drv_t *)ctx);
    return false;
}

// Nachtfarbe: Helligkeit (0 … 63) -> Farbe als RGB565 mit getauschten Bytes (LV_COLOR_16_SWAP)
static volatile bool s_tint_on;
static uint16_t s_tint_lut[64];

void board_set_tint(bool on, uint32_t rgb)
{
    if (on) {
        int R = (rgb >> 16) & 0xFF, G = (rgb >> 8) & 0xFF, B = rgb & 0xFF;
        for (int i = 0; i < 64; i++) {
            // je Kanal runden, sonst kippen sehr dunkle Stufen ins Grüne (Grün hat 6 Bit, Rot/Blau 5)
            int r = (R * i * 31 + 63 * 255 / 2) / (63 * 255), g = (G * i * 63 + 63 * 255 / 2) / (63 * 255),
                b = (B * i * 31 + 63 * 255 / 2) / (63 * 255);
            uint16_t c = (uint16_t)((r << 11) | (g << 5) | b);
            s_tint_lut[i] = (uint16_t)((c >> 8) | (c << 8));
        }
    }
    s_tint_on = on;
}

void board_tint_buf(void *px, int n)
{
    uint16_t *p = px;
    for (int i = 0; i < n; i++) {
        uint16_t v = (uint16_t)((p[i] >> 8) | (p[i] << 8));
        int r = v >> 11, g = (v >> 5) & 63, b = v & 31;
        p[i] = s_tint_lut[(r * 2 * 77 + g * 150 + b * 2 * 29) >> 8]; // Helligkeit wie das Auge (Rec. 601)
    }
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    if (s_tint_on) board_tint_buf(color_map, (area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1));
    esp_lcd_panel_draw_bitmap(s_panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_map);
}

static esp_err_t display_init(void)
{
    // Hintergrundbeleuchtung per PWM (dimmbar)
    ledc_timer_config_t lt = {.speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_10_BIT,
                              .timer_num = LEDC_TIMER_0, .freq_hz = 20000, .clk_cfg = LEDC_AUTO_CLK};
    ledc_timer_config(&lt);
    ledc_channel_config_t lc = {.gpio_num = PIN_LCD_BL, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0,
                                .timer_sel = LEDC_TIMER_0, .duty = 0, .hpoint = 0};
    ledc_channel_config(&lc);

    esp_lcd_i80_bus_handle_t bus = NULL;
    esp_lcd_i80_bus_config_t bus_cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .dc_gpio_num = PIN_LCD_DC,
        .wr_gpio_num = PIN_LCD_WR,
        .bus_width = 8,
        .max_transfer_bytes = LCD_H_RES * DRAW_BUF_LINES * sizeof(uint16_t),
        .psram_trans_align = 64,
        .sram_trans_align = 4,
    };
    for (int i = 0; i < 8; i++) bus_cfg.data_gpio_nums[i] = PIN_LCD_DATA[i];
    ESP_RETURN_ON_ERROR(esp_lcd_new_i80_bus(&bus_cfg, &bus), TAG, "i80 bus");

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i80_config_t io_cfg = {
        .cs_gpio_num = -1,
        .pclk_hz = LCD_PCLK_HZ,
        .trans_queue_depth = 10,
        .dc_levels = {.dc_idle_level = 0, .dc_cmd_level = 0, .dc_dummy_level = 0, .dc_data_level = 1},
        .on_color_trans_done = on_color_trans_done,
        .user_ctx = &s_disp_drv,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i80(bus, &io_cfg, &io), TAG, "panel io");

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7796(io, &panel_cfg, &s_panel), TAG, "st7796");
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_invert_color(s_panel, true);
    // Querformat 480x320: Zeilen/Spalten tauschen (reine Drehung, keine Spiegelung)
    esp_lcd_panel_swap_xy(s_panel, true);
    esp_lcd_panel_mirror(s_panel, s_flip, s_flip);
    esp_lcd_panel_disp_on_off(s_panel, true);
    return ESP_OK;
}

#ifdef CONFIG_DECK_PERF_LOG
// Messung: Bilder pro Sekunde und Renderzeit je Bild
volatile uint32_t g_perf_sum, g_perf_n;
static void lvgl_monitor_cb(lv_disp_drv_t *drv, uint32_t time, uint32_t px)
{
    g_perf_sum += time;
    g_perf_n++;
    static uint32_t n, sum, max, pxsum;
    static int64_t t0;
    int64_t now = esp_timer_get_time();
    if (!t0) t0 = now;
    n++;
    sum += time;
    pxsum += px;
    if (time > max) max = time;
    if (now - t0 >= 2500000) {
        float secs = (now - t0) / 1e6f;
        ESP_LOGI("perf", "%.1f Bilder/s · Ø %lu ms · max %lu ms · %lu kpx/Bild",
                 n / secs, (unsigned long)(sum / n), (unsigned long)max, (unsigned long)(pxsum / n / 1000));
        n = sum = max = pxsum = 0;
        t0 = now;
    }
}
#endif

// ------------------------------------------------------------------ Touch (FT6336U)

static esp_err_t touch_init(void)
{
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_TP_SDA,
        .scl_io_num = PIN_TP_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg, &bus), TAG, "i2c bus");
    i2c_device_config_t dev = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = TP_ADDR, .scl_speed_hz = 400000};
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev, &s_tp), TAG, "touch dev");
    gpio_config_t intr = {.pin_bit_mask = 1ULL << PIN_TP_INT, .mode = GPIO_MODE_INPUT, .pull_up_en = 1};
    gpio_config(&intr);
    uint8_t reg = 0xA8, id = 0;
    if (i2c_master_transmit_receive(s_tp, &reg, 1, &id, 1, 100) == ESP_OK) {
        ESP_LOGI(TAG, "Touch-Controller gefunden, Vendor-ID 0x%02x", id);
    } else {
        ESP_LOGW(TAG, "Touch-Controller antwortet nicht");
    }
    return ESP_OK;
}

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    static lv_coord_t last_x, last_y;
    uint8_t reg = 0x02, buf[5];
    data->state = LV_INDEV_STATE_REL;
    if (i2c_master_transmit_receive(s_tp, &reg, 1, buf, sizeof(buf), 20) == ESP_OK && (buf[0] & 0x0F) > 0) {
        // Rohkoordinaten im Hochformat (x 0..319, y 0..479)
        int tx = ((buf[1] & 0x0F) << 8) | buf[2];
        int ty = ((buf[3] & 0x0F) << 8) | buf[4];
        int x, y;
        if (!s_flip) { x = ty; y = (LCD_V_RES - 1) - tx; }
        else { x = (LCD_H_RES - 1) - ty; y = tx; }
        if (x < 0) x = 0;
        if (x >= LCD_H_RES) x = LCD_H_RES - 1;
        if (y < 0) y = 0;
        if (y >= LCD_V_RES) y = LCD_V_RES - 1;
        last_x = x;
        last_y = y;
        data->state = LV_INDEV_STATE_PR;
    }
    data->point.x = last_x;
    data->point.y = last_y;
}

// ------------------------------------------------------------------ LVGL

static void lv_tick_cb(void *arg) { lv_tick_inc(2); }

static void lvgl_task(void *arg)
{
    for (;;) {
        uint32_t wait = 10;
        if (board_lvgl_lock(-1)) {
            wait = lv_timer_handler();
            board_lvgl_unlock();
        }
        if (wait < 2) wait = 2;
        if (wait > 30) wait = 30;
        vTaskDelay(pdMS_TO_TICKS(wait));
    }
}

bool board_lvgl_lock(int timeout_ms)
{
    if (!s_lvgl_mutex) return false; // Display noch nicht initialisiert
    TickType_t t = timeout_ms < 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_lvgl_mutex, t) == pdTRUE;
}

void board_lvgl_unlock(void) { xSemaphoreGiveRecursive(s_lvgl_mutex); }

void board_set_brightness(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    // leicht quadratisch: kleine Werte feiner einstellbar
    uint32_t duty = (uint32_t)(pct * pct * 1023 / 10000);
    if (pct > 0 && duty < 4) duty = 4;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

esp_err_t board_init(void)
{
    s_lvgl_mutex = xSemaphoreCreateRecursiveMutex();
    lv_init();

    ESP_RETURN_ON_ERROR(display_init(), TAG, "display");
    touch_init();

    // Möglichst große Zeichenpuffer im schnellen internen DMA-RAM (weniger Teil-Flushes beim Wischen)
    static const int tries[] = {DRAW_BUF_LINES, 64, 48, 32};
    const size_t keep_free = 190 * 1024; // für WLAN/TLS im internen RAM lassen
    lv_color_t *b1 = NULL, *b2 = NULL;
    size_t buf_px = 0;
    for (int i = 0; i < 4 && !b2; i++) {
        buf_px = LCD_H_RES * tries[i];
        if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 2 * buf_px * sizeof(lv_color_t) + keep_free && i < 3) continue;
        free(b1);
        b1 = heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        b2 = b1 ? heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) : NULL;
        if (b1 && b2) ESP_LOGI(TAG, "Zeichenpuffer: 2 x %d Zeilen", tries[i]);
    }
    if (!b1 || !b2) return ESP_ERR_NO_MEM;
    lv_disp_draw_buf_init(&s_draw_buf, b1, b2, buf_px);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = LCD_H_RES;
    s_disp_drv.ver_res = LCD_V_RES;
    s_disp_drv.flush_cb = lvgl_flush_cb;
    s_disp_drv.draw_buf = &s_draw_buf;
#ifdef CONFIG_DECK_PERF_LOG
    s_disp_drv.monitor_cb = lvgl_monitor_cb;
#endif
    lv_disp_drv_register(&s_disp_drv);

    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type = LV_INDEV_TYPE_POINTER;
    s_indev_drv.read_cb = touch_read_cb;
    s_indev_drv.scroll_limit = 6; // Wischen startet früher (Standard 10 px)
    s_indev_drv.scroll_throw = 8; // etwas längerer Schwung (Standard 10)
    s_indev_drv.long_press_time = 700; // langer Druck öffnet die Einstellungen
    lv_indev_drv_register(&s_indev_drv);

    const esp_timer_create_args_t targs = {.callback = lv_tick_cb, .name = "lv_tick"};
    esp_timer_handle_t tick;
    esp_timer_create(&targs, &tick);
    esp_timer_start_periodic(tick, 2000);

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, 4, NULL, 1);
    return ESP_OK;
}
