#pragma once
// Einstellungen im NVS (am Gerät per langem Druck oder im Browser änderbar)
#include <stdbool.h>
#include <stdint.h>

#define DECK_MAX_MINERS 1
#define DECK_CARDS 9
#define DECK_CARDS_ALL ((1u << DECK_CARDS) - 1)
extern const char *const DECK_CARD_NAMES[DECK_CARDS];

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char bitaxe_host[DECK_MAX_MINERS][64]; // z. B. 192.168.1.42, leer = nicht genutzt
    float kwh_price;     // €/kWh
    uint8_t currency;    // 0 = USD, 1 = EUR
    bool alarm_on;       // Preisalarm
    float alarm_hi;      // obere Grenze (0 = aus), in gewählter Währung
    float alarm_lo;      // untere Grenze (0 = aus)
    uint16_t rotate_s;   // automatischer Seitenwechsel in Sekunden, 0 = aus
    bool night_on;       // Nachtmodus
    uint8_t night_from;  // Stunde, ab der gedimmt wird
    uint8_t night_to;    // Stunde, bis zu der gedimmt wird
    bool block_note;     // Hinweis bei neuem Block
    uint8_t chart_range; // 0 = 24h, 1 = 7T, 2 = 30T, 3 = 1J
    uint8_t brightness;  // 5..100 %
    uint8_t chart_style; // Chart-Fläche: 0 = Punkte, 1 = ohne Verlauf, 2 = Pixel (Mosaik)
    bool auto_update;    // OTA-Updates automatisch installieren
    uint16_t card_mask;  // eingeschaltete Karten (Bit 0 = Preis … Bit 8 = Lotterie)
} deck_settings_t;

extern deck_settings_t g_settings;

void settings_load(void);
void settings_save(void);
bool settings_has_wifi(void);
const char *settings_cur(void); // "USD" oder "EUR"
const char *deck_version(void); // Firmware-Version, z. B. "1.4.0"
