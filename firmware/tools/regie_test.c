// Test der Regie am Rechner (ohne Gerät):
//   cc -Wall -I main tools/regie_test.c main/regie.c -lm -o /tmp/regie_test && /tmp/regie_test
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "regie.h"

static int fails;
static deck_data_t d;
static int64_t ms;
static time_t now;

#define CHECK(cond, ...) \
    do { \
        if (!(cond)) { \
            fails++; \
            printf("FEHLER Zeile %d: ", __LINE__); \
            printf(__VA_ARGS__); \
            printf("\n"); \
        } \
    } while (0)

// Zeit vergehen lassen, alle 2 s füttern
static void run(int seconds, bool quiet)
{
    for (int i = 0; i < seconds; i += 2) {
        ms += 2000;
        now += 2;
        regie_feed(&d, false, ms, now, quiet);
    }
}

// erwartet genau dieses Ereignis (oder keines, wenn text NULL)
static void expect(regie_ev_t ev, const char *text, int line)
{
    regie_hit_t h;
    bool got = regie_take(ms, REGIE_ALL, &h);
    if (!text) {
        if (got) {
            fails++;
            printf("FEHLER Zeile %d: unerwartetes Ereignis %d „%s“\n", line, h.ev, h.text);
        }
        return;
    }
    if (!got || h.ev != ev || strcmp(h.text, text)) {
        fails++;
        printf("FEHLER Zeile %d: erwartet %d „%s“, bekommen %s%d „%s“\n", line, ev, text, got ? "" : "nichts ", got ? h.ev : -1, got ? h.text : "");
    } else printf("ok    %s\n", h.text);
}
#define EXPECT(ev, text) expect(ev, text, __LINE__)
#define NONE() expect(0, NULL, __LINE__)

static void base(void)
{
    memset(&d, 0, sizeof(d));
    regie_reset();
    ms = 100000;
    setenv("TZ", "UTC", 1);
    tzset();
    now = 1794000000; // 2026-11-07 … 14:40 UTC, kein Morgen
    d.price_ok = true;
    d.usd = 80000;
    d.eur = 74000;
    d.net_ok = true;
    d.fee_fast = 4;
    d.remaining_blocks = 900;
    d.miner_n = 1;
    d.miners[0].mode = MINER_DEMO;
}

int main(void)
{
    // ---- Kurs
    base();
    run(20 * 60, false);
    NONE();
    d.usd = 80000 * 1.004; // kleiner als 1 %
    run(60, false);
    NONE();
    d.usd = 80000 * 1.013;
    run(4, false);
    EXPECT(RG_PRICE, "KURS +1,3 % IN 15 MIN");
    d.usd = 80000 * 1.030; // gleich danach weiter: Ruhezeit von 17 Minuten
    run(5 * 60, false);
    NONE();
    run(13 * 60, false); // Ruhezeit vorbei, Vergleichswert ist inzwischen der neue Kurs
    NONE();
    d.usd = 80000 * 1.030 * 0.985;
    run(4, false);
    EXPECT(RG_PRICE, "KURS –1,5 % IN 15 MIN");
    // Währungswechsel darf kein Sprung sein
    base();
    run(10 * 60, false);
    for (int i = 0; i < 300; i += 2) {
        ms += 2000;
        regie_feed(&d, true, ms, now, false);
    }
    NONE();
    // kurz nach dem Start: erst nach 5 Minuten Vergleichswert
    base();
    run(120, false);
    d.usd *= 1.02;
    run(60, false);
    NONE();
    run(180, false);
    EXPECT(RG_PRICE, "KURS +2,0 % IN 5 MIN");

    // ---- Gebühren
    base();
    run(5 * 60, false);
    d.fee_fast = 30; // noch in der Aufwärmzeit
    run(60, false);
    NONE();
    base();
    run(15 * 60, false);
    d.fee_fast = 30;
    run(62, false);
    EXPECT(RG_FEES, "GEBÜHREN STEIGEN: 30 SAT/VB");
    run(40 * 60, false);
    d.fee_fast = 12;
    run(62, false);
    EXPECT(RG_FEES, "GEBÜHREN FALLEN: 12 SAT/VB");
    base(); // kleine Schwankung bei niedrigen Gebühren: nichts
    run(15 * 60, false);
    d.fee_fast = 8;
    run(5 * 60, false);
    NONE();

    // ---- Miner
    base();
    miner_t *m = &d.miners[0];
    run(60, false);
    m->mode = MINER_ONLINE; // Demo -> online beim Start: kein Ereignis
    m->temp = 60;
    m->vrtemp = 65;
    m->expected_gh = 1200;
    m->hashrate_gh = 1250;
    m->uptime_s = 5000;
    m->best_diff = 900e6;
    m->session_diff = 40e6;
    run(60, false);
    NONE();
    m->mode = MINER_OFFLINE;
    run(4, false);
    EXPECT(RG_MINER, "BITAXE OFFLINE");
    run(120, false);
    m->mode = MINER_ONLINE;
    run(4, false);
    EXPECT(RG_MINER, "BITAXE WIEDER ONLINE");
    run(20 * 60, false);
    NONE();
    m->temp = 71.4;
    run(4, false);
    EXPECT(RG_MINER, "BITAXE HEISS: 71 °C");
    run(20 * 60, false); // bleibt heiß: kein zweites Mal
    NONE();
    m->temp = 60;
    run(60, false);
    m->vrtemp = 88;
    run(4, false);
    EXPECT(RG_MINER, "SPANNUNGSREGLER HEISS: 88 °C");
    m->vrtemp = 60;
    run(20 * 60, false);
    m->hashrate_gh = 420;
    run(170, false); // noch keine drei Minuten
    NONE();
    run(20, false);
    EXPECT(RG_MINER, "HASHRATE EINGEBROCHEN: 0,42 TH/S");
    m->hashrate_gh = 1250;
    // Bestwerte
    m->session_diff = 43.9e6;
    run(4, false);
    EXPECT(RG_SHARE, "NEUER BESTWERT: 43,9 M");
    m->session_diff = 60e6; // Ruhezeit 10 Minuten
    run(4, false);
    NONE();
    m->best_diff = 1.02e9;
    m->session_diff = 1.02e9;
    run(120, false);
    EXPECT(RG_SHARE, "NEUER REKORD-SHARE: 1,0 G");
    m->session_diff = 5e3; // Miner neu gestartet: Bestwert der Sitzung beginnt von vorn
    m->uptime_s = 30;
    run(15 * 60, false);
    NONE();

    // ---- News
    base();
    run(60, false);
    d.news.n = 4;
    d.news.item[0].ts = now - 3600;
    run(60, false); // erste Ladung
    NONE();
    d.news.item[0].ts = now - 10;
    run(4, false);
    EXPECT(RG_NEWS, "NEUE SCHLAGZEILE");
    d.news.item[0].ts = now - 5000; // ältere Meldung oben (andere Quelle): nichts
    run(15 * 60, false);
    NONE();

    // ---- Wetter
    base();
    d.wx.ok = true;
    d.wx.code = 3;
    d.wx.days = 6;
    d.wx.d_max[0] = 13.6;
    d.wx.d_min[0] = -0.4;
    d.wx.d_rain[0] = 75;
    run(60, false);
    NONE();
    d.wx.code = 61;
    run(4, false);
    EXPECT(RG_WEATHER, "ES REGNET");
    d.wx.code = 63; // weiter Regen: nichts
    run(40 * 60, false);
    NONE();
    d.wx.code = 95;
    run(4, false);
    EXPECT(RG_WEATHER, "GEWITTER");
    d.wx.code = 2; // trocken: nichts
    run(40 * 60, false);
    NONE();
    // Morgengruß: 06:10 Uhr noch Nachtruhe, danach einmal
    now = 1794031800; // 06:10 UTC am Folgetag
    run(20 * 60, true);
    NONE();
    run(4, false);
    EXPECT(RG_WEATHER, "GUTEN MORGEN · HEUTE 14° / 0° · REGEN 75 %");
    run(3 * 3600, false);
    NONE();

    // ---- Difficulty
    base();
    run(60, false);
    d.remaining_blocks = 6;
    d.diff_change = 2.14;
    run(4, false);
    EXPECT(RG_DIFF, "DIFFICULTY IN 6 BLÖCKEN: +2,1 %");
    d.remaining_blocks = 3;
    run(120, false);
    NONE();
    d.remaining_blocks = 2015;
    run(4, false);
    EXPECT(RG_DIFF, "DIFFICULTY ANGEPASST · NEUE PERIODE");

    // ---- Reihenfolge, Abwahl, Verfall
    base();
    m = &d.miners[0];
    m->mode = MINER_ONLINE;
    m->temp = 60;
    run(20 * 60, false);
    d.usd *= 1.02;
    m->mode = MINER_OFFLINE;
    d.news.n = 1;
    d.news.item[0].ts = 5;
    run(4, false);
    d.news.item[0].ts = 9;
    run(4, false);
    EXPECT(RG_MINER, "BITAXE OFFLINE"); // das Wichtigste zuerst
    regie_hit_t h;
    CHECK(regie_take(ms, REGIE_ALL & ~(1u << RG_PRICE), &h) && h.ev == RG_NEWS, "abgewählter Kurssprung muss verfallen, News folgen");
    NONE();
    d.usd *= 1.02;
    run(16 * 60, false);
    d.usd *= 1.02;
    run(4, false);
    ms += 181000; // länger als drei Minuten nicht abgeholt
    NONE();

    printf(fails ? "\n%d FEHLER\n" : "\nalle Prüfungen bestanden\n", fails);
    return fails ? 1 : 0;
}
