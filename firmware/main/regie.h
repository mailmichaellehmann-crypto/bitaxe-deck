#pragma once
// Regie: erkennt in den Daten, was gerade passiert – Kurssprung, Gebührenspitze, Miner-Problem, neuer
// Bestwert, neue Schlagzeile, Wetterumschwung, Difficulty-Anpassung –, damit die Oberfläche für kurze Zeit
// die passende Karte zeigt. Ohne LVGL, lässt sich am Rechner testen (tools/regie_test.c).
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "data.h"

typedef enum {
    RG_BLOCK = 0, // neuer Block (erkennt die Oberfläche selbst, wegen der Animation)
    RG_PRICE,
    RG_FEES,
    RG_MINER,
    RG_SHARE,
    RG_NEWS,
    RG_WEATHER,
    RG_DIFF,
    RG_N
} regie_ev_t;

#define REGIE_ALL ((1u << RG_N) - 1)
extern const char *const REGIE_NAMES[RG_N]; // Beschriftung der Ereignisse (Browser-Seite)

typedef struct {
    regie_ev_t ev;
    char text[64]; // Anlass in Großbuchstaben, z. B. „KURS +1,2 % IN 15 MIN“
} regie_hit_t;

// Daten prüfen und Ereignisse vormerken; mit gehaltener Datensperre etwa alle 2 s aufrufen.
// eur: angezeigte Währung, ms: Zeit seit Start, now: Uhrzeit, quiet: Nachtruhe (kein Morgengruß)
void regie_feed(const deck_data_t *d, bool eur, int64_t ms, time_t now, bool quiet);
// nächstes vorgemerktes Ereignis holen (das wichtigste zuerst); abgewählte und veraltete verfallen
bool regie_take(int64_t ms, uint16_t mask, regie_hit_t *hit);
// Vergleichswerte vergessen (z. B. nach geänderter News-Quelle oder Währung)
void regie_reset(void);
