#include "shares.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "data.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net.h"
#include "settings.h"

static const char *TAG = "shares";

static share_stats_t s_st;
static SemaphoreHandle_t s_mtx;
static esp_websocket_client_handle_t s_ws;
static char s_line[384];
static int s_line_len;
static int s_logged;

void shares_lock(void) { xSemaphoreTake(s_mtx, portMAX_DELAY); }
void shares_unlock(void) { xSemaphoreGive(s_mtx); }
share_stats_t *shares_get(void) { return &s_st; }

int shares_bucket(double d)
{
    if (d < 10) return 0;
    int b = (int)((log10(d) - 1.0) * 10.0);
    if (b < 0) b = 0;
    if (b >= SH_BUCKETS) b = SH_BUCKETS - 1;
    return b;
}

double shares_bucket_log(int b) { return 1.0 + (b + 0.5) / 10.0; }

double shares_per_min(const share_stats_t *s)
{
    if (s->ts_n < 2) return 0;
    int newest = (s->ts_head - 1 + SH_RECENT) % SH_RECENT;
    int oldest = (s->ts_head - s->ts_n + SH_RECENT) % SH_RECENT;
    int64_t span = s->ts[newest] - s->ts[oldest];
    int64_t now = esp_timer_get_time() / 1000;
    if (now - s->ts[newest] > span) span = now - s->ts[oldest]; // länger nichts gekommen
    return span > 0 ? (s->ts_n - 1) * 60000.0 / span : 0;
}

static void record(double diff, bool live)
{
    shares_lock();
    if (live && !s_st.live && s_st.total) { // Wechsel von Simulation auf echte Daten: neu beginnen
        memset(s_st.hist, 0, sizeof(s_st.hist));
        s_st.total = 0;
        s_st.best = 0;
        s_st.ts_n = s_st.ts_head = 0;
    }
    s_st.live = live;
    s_st.hist[shares_bucket(diff)]++;
    s_st.total++;
    s_st.last = diff;
    if (diff > s_st.best) s_st.best = diff;
    s_st.last_ms = esp_timer_get_time() / 1000;
    s_st.ts[s_st.ts_head] = s_st.last_ms;
    s_st.ts_head = (s_st.ts_head + 1) % SH_RECENT;
    if (s_st.ts_n < SH_RECENT) s_st.ts_n++;
    s_st.ver++;
    shares_unlock();
}

// Eine Log-Zeile auswerten: „… diff 726.3 of 3184.“
static void parse_line(char *ln)
{
    char *p = strstr(ln, " diff ");
    if (!p || !strstr(p, " of ")) return;
    double d = strtod(p + 6, NULL);
    if (d <= 0) return;
    if (s_logged < 3) { // die ersten Zeilen zur Kontrolle ins Log
        ESP_LOGI(TAG, "Share aus Log-Stream: %.120s", ln);
        s_logged++;
    }
    record(d, true);
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_websocket_event_data_t *e = event_data;
    if (id == WEBSOCKET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "Log-Stream des Bitaxe verbunden");
        shares_lock();
        s_st.connected = true;
        shares_unlock();
    } else if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_CLOSED || id == WEBSOCKET_EVENT_ERROR) {
        shares_lock();
        s_st.connected = false;
        shares_unlock();
    } else if (id == WEBSOCKET_EVENT_DATA && (e->op_code == 0x01 || e->op_code == 0x00) && e->data_len > 0) {
        for (int i = 0; i < e->data_len; i++) {
            char c = e->data_ptr[i];
            if (c == '\n' || c == '\r') {
                s_line[s_line_len] = 0;
                if (s_line_len) parse_line(s_line);
                s_line_len = 0;
            } else if (s_line_len < (int)sizeof(s_line) - 1) {
                s_line[s_line_len++] = c;
            }
        }
        // Ende der Nachricht ohne Zeilenumbruch: trotzdem auswerten
        if (e->payload_offset + e->data_len >= e->payload_len && s_line_len) {
            s_line[s_line_len] = 0;
            parse_line(s_line);
            s_line_len = 0;
        }
    }
}

static void ws_start(const char *host)
{
    char uri[96];
    snprintf(uri, sizeof(uri), "ws://%s/api/ws", host);
    esp_websocket_client_config_t cfg = {
        .uri = uri,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 8000,
        .buffer_size = 1024,
        .task_stack = 4096,
    };
    s_ws = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    esp_websocket_client_start(s_ws);
    ESP_LOGI(TAG, "Verbinde mit %s", uri);
}

static double rand01(void) { return (esp_random() + 1.0) / 4294967297.0; } // (0, 1]

// Läuft dauerhaft: startet den Log-Stream und simuliert, solange keine echten Shares kommen
static void shares_task(void *arg)
{
    const double sim_min = 256; // typische Mindest-Difficulty der ASIC-Ergebnisse
    for (;;) {
        if (!s_ws && g_settings.bitaxe_host[0][0] && net_state() == NET_CONNECTED) ws_start(g_settings.bitaxe_host[0]);

        shares_lock();
        int64_t now = esp_timer_get_time() / 1000;
        bool live_recent = s_st.live && now - s_st.last_ms < 20000;
        shares_unlock();
        if (live_recent) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        // Simulation: Shares kommen zufällig (Poisson), ihre Difficulty folgt P(D > d) = d0 / d
        data_lock();
        double gh = data_get()->miner_n ? data_get()->miners[0].hashrate_gh : 1200;
        data_unlock();
        if (gh < 100) gh = 1200;
        double rate = gh * 1e9 / (sim_min * 4294967296.0); // Shares pro Sekunde
        double wait_ms = -log(rand01()) / rate * 1000;
        if (wait_ms > 8000) wait_ms = 8000;
        if (wait_ms < 60) wait_ms = 60;
        vTaskDelay(pdMS_TO_TICKS((uint32_t)wait_ms));
        shares_lock();
        bool live_now = s_st.live && esp_timer_get_time() / 1000 - s_st.last_ms < 20000;
        shares_unlock();
        if (!live_now) record(sim_min / rand01(), false);
    }
}

void shares_start(void)
{
    s_mtx = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(shares_task, "shares", 4096, NULL, 2, NULL, 0);
}
