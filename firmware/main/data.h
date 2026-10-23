#pragma once
// Datenquellen: CoinGecko (Preis/Chart), mempool.space (Netzwerk), AxeOS-API des Bitaxe, Open-Meteo (Wetter)
// und Bitcoin-News (Crypto Vision oder RSS-Feeds)
#include <stdbool.h>
#include <stdint.h>

#include "settings.h"

#define CHART_PTS 120
#define HIST_PTS 240 // 1 h bei 15 s Abstand
#define BLOCKS_N 6
#define WX_DAYS 6 // Wetter: heute und die nächsten 5 Tage
#define NEWS_N 12 // Schlagzeilen
#define PXS_N 760 // Kursreihe für „Kurs seit Meldung“: 7,5 Tage in 15-Minuten-Schritten (Kraken)

typedef enum { RANGE_24H = 0, RANGE_7D, RANGE_30D, RANGE_1Y } chart_range_t;
typedef enum { MINER_DEMO = 0, MINER_ONLINE, MINER_OFFLINE } miner_mode_t;

typedef struct {
    uint32_t height;
    char pool[24];
    float median_fee;
    uint32_t tx_count;
    int64_t timestamp;
    double reward_btc;
} block_info_t;

typedef struct {
    miner_mode_t mode;
    char host[64];
    char err[48];
    int64_t last_online; // Unix-Sekunden
    int fails;
    double hashrate_gh, expected_gh, temp, vrtemp, power, voltage_v, current_a;
    double best_diff, session_diff;
    uint32_t shares_acc, shares_rej, uptime_s;
    int fan_pct, fan_rpm, freq, core_mv, core_mv_set, wifi_rssi;
    char hostname[32], asic[16], version[24], pool[56], board[12];
} miner_t;

// Wetter am eingestellten Ort (Open-Meteo)
typedef struct {
    bool ok;            // Daten vorhanden (bei Fehlern bleiben die letzten stehen)
    char place[48];     // gefundener Ort, z. B. „Berlin“
    char region[64];    // Bundesland und Land (Browser-Seite)
    char err[48];       // letzter Fehler, leer = in Ordnung
    double lat, lon;
    float temp, feels, wind; // °C, °C, km/h
    int humidity, code;      // %, WMO-Wettercode
    bool is_day;
    int days;                                               // Einträge in d_*
    int d_code[WX_DAYS], d_rain[WX_DAYS], d_wday[WX_DAYS]; // Code, Regenwahrscheinlichkeit (%, -1 = unbekannt), Wochentag (0 = So)
    float d_max[WX_DAYS], d_min[WX_DAYS];
    int64_t updated; // Unix-Sekunden des letzten erfolgreichen Abrufs
    uint32_t ver;
} weather_t;

typedef struct {
    char title[176];  // bereinigt (nur Zeichen der Schrift)
    char source[28];
    int64_t ts;       // Unix-Sekunden, 0 = unbekannt
} news_item_t;

typedef struct {
    int n;
    news_item_t item[NEWS_N]; // neueste zuerst
    char err[48];
    int64_t updated;
    uint32_t ver;
} news_t;

// Kursverlauf in USD mit Zeitstempeln (Unix-Sekunden), älteste zuerst
typedef struct {
    int n;
    uint32_t t[PXS_N];
    float v[PXS_N];
} px_series_t;

typedef struct {
    // Markt
    bool price_ok;
    double usd, eur, chg24, chg24_eur;
    uint32_t price_ver;

    // Preis-Chart
    chart_range_t chart_range;
    bool chart_ok;
    int chart_n;
    float chart[CHART_PTS];
    double chart_min, chart_max, chart_first, chart_last;
    int64_t chart_t0, chart_t1; // ms
    uint32_t chart_ver;

    // 24h-Sparkline
    int spark_n;
    float spark[60];
    uint32_t spark_ver;

    // Netzwerk
    bool net_ok;
    uint32_t height;
    double net_hash, difficulty, diff_change, diff_progress, block_time_ms;
    int remaining_blocks;
    int64_t retarget_ms;
    int fee_fast, fee_half, fee_hour, fee_eco;
    block_info_t blocks[BLOCKS_N];
    int blocks_n;
    double avg_fees_btc;
    uint32_t net_ver;

    // Bitaxe-Geräte (miners[0] = Hauptgerät für die Detailkarte)
    miner_t miners[DECK_MAX_MINERS];
    int miner_n; // Anzahl genutzter Einträge (mind. 1, sonst Demo)
    uint32_t miner_ver;

    // Verlauf (Ringpuffer, älteste zuerst nach data_hist_copy)
    int hist_n, hist_head;
    bool hist_demo; // true = simulierter Verlauf
    float hist_hash[HIST_PTS], hist_temp[HIST_PTS];
    uint32_t hist_ver;

    // Stromzähler des Bitaxe: zählt, solange echte Messwerte kommen (gespeichert im NVS)
    double energy_wh;      // gesamt seit energy_since
    double energy_day_wh;  // heute
    int64_t energy_since;  // Unix-Sekunden, 0 = Uhrzeit noch unbekannt
    uint32_t energy_day;   // Datum von energy_day_wh als JJJJMMTT

    weather_t wx;

    // Bitcoin-News und dazu der Kursverlauf (im PSRAM)
    news_t news;
    px_series_t *px;
    uint32_t px_ver;
} deck_data_t;

void data_start(void);
void data_lock(void);
void data_unlock(void);
deck_data_t *data_get(void); // nur zwischen data_lock/unlock lesen
void data_request_chart(chart_range_t r);
int data_hist_copy(float *hash, float *temp, int max); // gibt Anzahl zurück
void data_energy_save(void);  // Stromzähler sofort sichern (z. B. vor einem Neustart)
void data_energy_reset(void); // Stromzähler auf 0 setzen
