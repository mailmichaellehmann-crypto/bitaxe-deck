#include "settings.h"

#include <string.h>
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_app_desc.h"

deck_settings_t g_settings = {
    .kwh_price = 0.30f,
    .rotate_s = 0,
    .night_from = 22,
    .night_to = 7,
    .night_color = 0,
    .clock_style = 1,
    .block_note = true,
    .brightness = 80,
    .chart_style = 0,
    .auto_update = true,
    .card_mask = DECK_CARDS_ALL,
};

const char *const DECK_CARD_NAMES[DECK_CARDS] = {"Preis",    "USD / EUR", "Chart",      "Block", "Neuer Block", "Halving",
                                                 "Gebühren", "Difficulty", "Uhr",       "Bitaxe", "Kosten",     "Lotterie"};

const char *const NIGHT_COLOR_NAMES[NIGHT_COLORS + 1] = {"Bernstein", "Tiefrot", "Kerzenlicht", "Rosé",
                                                         "Phosphor",  "Eisblau", "Originalfarben"};
const uint32_t NIGHT_COLOR_RGB[NIGHT_COLORS] = {0xFFB000, 0xFF2A1A, 0xFFB46B, 0xF2A6A6, 0x4CFF7A, 0x9CC8FF};

// neue Karte an Platz pos einfügen (eingeschaltet), die folgenden rücken auf
static uint16_t card_insert(uint16_t r, int pos)
{
    uint16_t low = r & ((1u << pos) - 1);
    return low | (uint16_t)((r >> pos) << (pos + 1)) | (1u << pos);
}

// Karte an Platz pos entfernen, die folgenden rücken nach
static uint16_t card_remove(uint16_t r, int pos)
{
    return (r & ((1u << pos) - 1)) | (uint16_t)((r >> (pos + 1)) << pos);
}

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
    // Strompreis: neuer Schlüssel; vom alten nur selbst gesetzte Werte übernehmen (0,35 war der frühere Standard)
    uint32_t k;
    if (nvs_get_u32(h, "kwh2", &k) == ESP_OK) g_settings.kwh_price = k / 1000.0f;
    else if (nvs_get_u32(h, "kwh", &k) == ESP_OK && k != 350) g_settings.kwh_price = k / 1000.0f;
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
    get_u8(h, "ncol", &g_settings.night_color);
    get_u8(h, "clk", &g_settings.clock_style);
    get_u8(h, "range", &g_settings.chart_range);
    get_u8(h, "bright", &g_settings.brightness);
    get_u8(h, "cstyle", &g_settings.chart_style);
    uint16_t r;
    if (nvs_get_u16(h, "rot", &r) == ESP_OK) g_settings.rotate_s = r;
    if (nvs_get_u16(h, "cards", &r) == ESP_OK) {
        uint8_t n = 9;
        get_u8(h, "cardsn", &n);
        // ältere Firmware: neue Karten eingeschaltet einfügen
        if (n < 10) r = card_insert(r, 8);  // Kosten
        if (n < 11) r = card_insert(r, 4);  // Neuer Block
        if (n < 12) r = card_insert(r, 5);  // Halving
        if (n == 13) r = card_remove(r, 10); // Netzanteil (nur in einer Zwischenversion)
        g_settings.card_mask = r & DECK_CARDS_ALL;
    }
    if (!g_settings.card_mask) g_settings.card_mask = DECK_CARDS_ALL;
    nvs_close(h);
    if (g_settings.chart_range > 3) g_settings.chart_range = 0;
    if (g_settings.chart_style > 2) g_settings.chart_style = 0;
    if (g_settings.night_color > NIGHT_COLORS) g_settings.night_color = 0;
    if (g_settings.clock_style > 1) g_settings.clock_style = 1;
    if (g_settings.brightness < 5 || g_settings.brightness > 100) g_settings.brightness = 80;
}

void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open("deck", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", g_settings.wifi_ssid);
    nvs_set_str(h, "pass", g_settings.wifi_pass);
    for (int i = 0; i < DECK_MAX_MINERS; i++) nvs_set_str(h, MINER_KEYS[i], g_settings.bitaxe_host[i]);
    nvs_set_u32(h, "kwh2", (uint32_t)(g_settings.kwh_price * 1000.0f + 0.5f));
    nvs_set_u32(h, "al_hi", (uint32_t)(g_settings.alarm_hi + 0.5f));
    nvs_set_u32(h, "al_lo", (uint32_t)(g_settings.alarm_lo + 0.5f));
    nvs_set_u8(h, "cur", g_settings.currency);
    nvs_set_u8(h, "al_on", g_settings.alarm_on);
    nvs_set_u8(h, "n_on", g_settings.night_on);
    nvs_set_u8(h, "blk", g_settings.block_note);
    nvs_set_u8(h, "aupd", g_settings.auto_update);
    nvs_set_u8(h, "n_from", g_settings.night_from);
    nvs_set_u8(h, "n_to", g_settings.night_to);
    nvs_set_u8(h, "ncol", g_settings.night_color);
    nvs_set_u8(h, "clk", g_settings.clock_style);
    nvs_set_u8(h, "range", g_settings.chart_range);
    nvs_set_u8(h, "bright", g_settings.brightness);
    nvs_set_u8(h, "cstyle", g_settings.chart_style);
    nvs_set_u16(h, "rot", g_settings.rotate_s);
    nvs_set_u16(h, "cards", g_settings.card_mask);
    nvs_set_u8(h, "cardsn", DECK_CARDS);
    nvs_commit(h);
    nvs_close(h);
}

bool settings_has_wifi(void) { return g_settings.wifi_ssid[0] != 0; }

const char *settings_cur(void) { return g_settings.currency ? "EUR" : "USD"; }

const char *deck_version(void) { return esp_app_get_description()->version; }
