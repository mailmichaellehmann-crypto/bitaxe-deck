#include "settings.h"

#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_app_desc.h"

deck_settings_t g_settings = {
    .kwh_price = 0.35f,
    .rotate_s = 0,
    .night_from = 22,
    .night_to = 7,
    .block_note = true,
    .brightness = 80,
    .chart_style = 0,
    .auto_update = true,
};

static const char *MINER_KEYS[DECK_MAX_MINERS] = {"bitaxe"};

static void get_str(nvs_handle_t h, const char *key, char *out, size_t len)
{
    size_t l = len;
    if (nvs_get_str(h, key, out, &l) != ESP_OK) out[0] = 0;
}

static void get_u8(nvs_handle_t h, const char *key, uint8_t *v)
{
    uint8_t t;
    if (nvs_get_u8(h, key, &t) == ESP_OK) *v = t;
}

static void get_u32f(nvs_handle_t h, const char *key, float *v, float scale)
{
    uint32_t t;
    if (nvs_get_u32(h, key, &t) == ESP_OK) *v = t / scale;
}

void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open("deck", NVS_READONLY, &h) != ESP_OK) return;
    get_str(h, "ssid", g_settings.wifi_ssid, sizeof(g_settings.wifi_ssid));
    get_str(h, "pass", g_settings.wifi_pass, sizeof(g_settings.wifi_pass));
    for (int i = 0; i < DECK_MAX_MINERS; i++) get_str(h, MINER_KEYS[i], g_settings.bitaxe_host[i], sizeof(g_settings.bitaxe_host[i]));
    get_u32f(h, "kwh", &g_settings.kwh_price, 1000.0f);
    get_u32f(h, "al_hi", &g_settings.alarm_hi, 1.0f);
    get_u32f(h, "al_lo", &g_settings.alarm_lo, 1.0f);
    uint8_t b;
    b = g_settings.currency; get_u8(h, "cur", &b); g_settings.currency = b;
    b = g_settings.alarm_on; get_u8(h, "al_on", &b); g_settings.alarm_on = b;
    b = g_settings.night_on; get_u8(h, "n_on", &b); g_settings.night_on = b;
    b = g_settings.block_note; get_u8(h, "blk", &b); g_settings.block_note = b;
    b = g_settings.auto_update; get_u8(h, "aupd", &b); g_settings.auto_update = b;
    get_u8(h, "n_from", &g_settings.night_from);
    get_u8(h, "n_to", &g_settings.night_to);
    get_u8(h, "range", &g_settings.chart_range);
    get_u8(h, "bright", &g_settings.brightness);
    get_u8(h, "cstyle", &g_settings.chart_style);
    uint16_t r;
    if (nvs_get_u16(h, "rot", &r) == ESP_OK) g_settings.rotate_s = r;
    nvs_close(h);
    if (g_settings.chart_range > 3) g_settings.chart_range = 0;
    if (g_settings.chart_style > 1) g_settings.chart_style = 0; // früherer Stil „Verlauf“ -> Punkte
    if (g_settings.brightness < 5 || g_settings.brightness > 100) g_settings.brightness = 80;
}

void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open("deck", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", g_settings.wifi_ssid);
    nvs_set_str(h, "pass", g_settings.wifi_pass);
    for (int i = 0; i < DECK_MAX_MINERS; i++) nvs_set_str(h, MINER_KEYS[i], g_settings.bitaxe_host[i]);
    nvs_set_u32(h, "kwh", (uint32_t)(g_settings.kwh_price * 1000.0f + 0.5f));
    nvs_set_u32(h, "al_hi", (uint32_t)(g_settings.alarm_hi + 0.5f));
    nvs_set_u32(h, "al_lo", (uint32_t)(g_settings.alarm_lo + 0.5f));
    nvs_set_u8(h, "cur", g_settings.currency);
    nvs_set_u8(h, "al_on", g_settings.alarm_on);
    nvs_set_u8(h, "n_on", g_settings.night_on);
    nvs_set_u8(h, "blk", g_settings.block_note);
    nvs_set_u8(h, "aupd", g_settings.auto_update);
    nvs_set_u8(h, "n_from", g_settings.night_from);
    nvs_set_u8(h, "n_to", g_settings.night_to);
    nvs_set_u8(h, "range", g_settings.chart_range);
    nvs_set_u8(h, "bright", g_settings.brightness);
    nvs_set_u8(h, "cstyle", g_settings.chart_style);
    nvs_set_u16(h, "rot", g_settings.rotate_s);
    nvs_commit(h);
    nvs_close(h);
}

bool settings_has_wifi(void) { return g_settings.wifi_ssid[0] != 0; }

const char *settings_cur(void) { return g_settings.currency ? "EUR" : "USD"; }

const char *deck_version(void) { return esp_app_get_description()->version; }
