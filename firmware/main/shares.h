#pragma once
// Share-Lotterie: liest den Log-Stream des Bitaxe (ws://<ip>/api/ws) und wertet jede
// gefundene Nonce aus („asic_result: … diff 726.3 of 3184.“). Ohne Stream wird simuliert.
#include <stdbool.h>
#include <stdint.h>

#define SH_BUCKETS 160 // 0,1-Dekaden ab 10^1 bis 10^17
#define SH_RECENT 64

typedef struct {
    uint32_t hist[SH_BUCKETS]; // Anzahl Shares je log10-Bereich
    uint32_t total;
    double last, best;
    int64_t last_ms;           // Zeitpunkt des letzten Shares (ms seit Start)
    bool live;                 // true = echte Daten aus dem Log-Stream
    bool connected;            // WebSocket verbunden
    uint32_t ver;              // erhöht sich bei jedem Share
    int64_t ts[SH_RECENT];     // Zeitstempel der letzten Shares (Ringpuffer) für die Rate
    int ts_n, ts_head;
} share_stats_t;

void shares_start(void);
void shares_lock(void);
void shares_unlock(void);
share_stats_t *shares_get(void); // nur zwischen lock/unlock lesen
double shares_per_min(const share_stats_t *s);
int shares_bucket(double diff);  // Index in hist[]
double shares_bucket_log(int b); // log10-Mitte des Bereichs
