#include "data.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net.h"
#include "settings.h"

static const char *TAG = "data";

static deck_data_t s_d;
static SemaphoreHandle_t s_mtx;
static volatile int s_chart_req = -1;

void data_lock(void) { xSemaphoreTake(s_mtx, portMAX_DELAY); }
void data_unlock(void) { xSemaphoreGive(s_mtx); }
deck_data_t *data_get(void) { return &s_d; }
void data_request_chart(chart_range_t r) { s_chart_req = r; }

// ------------------------------------------------------------------ HTTP

typedef struct {
    char *buf;
    int len, cap;
} http_buf_t;

static esp_err_t http_evt(esp_http_client_event_t *e)
{
    http_buf_t *b = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && b) {
        if (b->len + e->data_len + 1 > b->cap) {
            int ncap = b->cap ? b->cap * 2 : 16384;
            while (ncap < b->len + e->data_len + 1) ncap *= 2;
            if (ncap > 512 * 1024) return ESP_FAIL;
            char *n = heap_caps_realloc(b->buf, ncap, MALLOC_CAP_SPIRAM);
            if (!n) return ESP_FAIL;
            b->buf = n;
            b->cap = ncap;
        }
        memcpy(b->buf + b->len, e->data, e->data_len);
        b->len += e->data_len;
        b->buf[b->len] = 0;
    }
    return ESP_OK;
}

// Lädt eine URL komplett und parst sie als JSON (NULL bei Fehler)
static cJSON *http_json(const char *url, int timeout_ms, char *err, int errlen)
{
    http_buf_t b = {0};
    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = http_evt,
        .user_data = &b,
        .timeout_ms = timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "bitaxe-deck/1.0",
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "Accept", "application/json");
    esp_err_t r = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    cJSON *j = NULL;
    if (r == ESP_OK && status == 200 && b.buf) {
        j = cJSON_Parse(b.buf);
        if (!j && err) snprintf(err, errlen, "Ungültige Antwort");
    } else if (err) {
        if (r != ESP_OK) snprintf(err, errlen, "%s", r == ESP_ERR_HTTP_CONNECT ? "Keine Verbindung" : esp_err_to_name(r));
        else snprintf(err, errlen, "HTTP %d", status);
    }
    if (r != ESP_OK || status != 200) ESP_LOGW(TAG, "GET %s -> %s / %d", url, esp_err_to_name(r), status);
    free(b.buf);
    return j;
}

static double jnum(const cJSON *o, const char *k, double def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsString(v) && v->valuestring) return strtod(v->valuestring, NULL);
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v) ? 1 : 0;
    return def;
}

static void jstr(const cJSON *o, const char *k, char *out, size_t len)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(v) && v->valuestring) strlcpy(out, v->valuestring, len);
    else if (len) out[0] = 0;
}

// "4.29G" oder Zahl -> double
static double parse_diff(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (!cJSON_IsString(v) || !v->valuestring) return 0;
    char *end;
    double d = strtod(v->valuestring, &end);
    while (*end == ' ') end++;
    switch (*end) {
    case 'k': case 'K': d *= 1e3; break;
    case 'M': d *= 1e6; break;
    case 'G': d *= 1e9; break;
    case 'T': d *= 1e12; break;
    case 'P': d *= 1e15; break;
    case 'E': d *= 1e18; break;
    }
    return d;
}

// ------------------------------------------------------------------ Markt

static void fetch_price(void)
{
    cJSON *j = http_json("https://api.coingecko.com/api/v3/simple/price?ids=bitcoin&vs_currencies=usd,eur&include_24hr_change=true", 10000, NULL, 0);
    const cJSON *b = j ? cJSON_GetObjectItem(j, "bitcoin") : NULL;
    if (b) {
        data_lock();
        s_d.usd = jnum(b, "usd", s_d.usd);
        s_d.eur = jnum(b, "eur", s_d.eur);
        s_d.chg24 = jnum(b, "usd_24h_change", 0);
        s_d.chg24_eur = jnum(b, "eur_24h_change", 0);
        s_d.price_ok = true;
        s_d.price_ver++;
        data_unlock();
    } else { // Ausweichquelle
        cJSON *m = http_json("https://mempool.space/api/v1/prices", 10000, NULL, 0);
        if (m) {
            data_lock();
            s_d.usd = jnum(m, "USD", s_d.usd);
            s_d.eur = jnum(m, "EUR", s_d.eur);
            s_d.price_ok = true;
            s_d.price_ver++;
            data_unlock();
            cJSON_Delete(m);
        }
    }
    cJSON_Delete(j);
}

static void fetch_chart(chart_range_t r)
{
    static const int days[] = {1, 7, 30, 365};
    char url[160];
    snprintf(url, sizeof(url), "https://api.coingecko.com/api/v3/coins/bitcoin/market_chart?vs_currency=%s&days=%d", g_settings.currency ? "eur" : "usd", days[r]);
    cJSON *j = http_json(url, 15000, NULL, 0);
    const cJSON *prices = j ? cJSON_GetObjectItem(j, "prices") : NULL;
    int n = cJSON_GetArraySize(prices);
    if (n < 2) {
        data_lock();
        if (s_d.chart_range == r) { s_d.chart_ok = false; s_d.chart_ver++; }
        data_unlock();
        cJSON_Delete(j);
        return;
    }
    // In Buckets mitteln -> CHART_PTS Punkte
    float out[CHART_PTS];
    int outn = n < CHART_PTS ? n : CHART_PTS;
    double mn = 1e18, mx = 0;
    for (int i = 0; i < outn; i++) {
        int a = (int)((int64_t)i * n / outn), e = (int)((int64_t)(i + 1) * n / outn);
        if (e <= a) e = a + 1;
        double s = 0;
        int c = 0;
        for (int k = a; k < e; k++) {
            const cJSON *p = cJSON_GetArrayItem(prices, k);
            double v = cJSON_GetArrayItem(p, 1)->valuedouble;
            if (v < mn) mn = v;
            if (v > mx) mx = v;
            s += v;
            c++;
        }
        out[i] = (float)(s / c);
    }
    const cJSON *first = cJSON_GetArrayItem(prices, 0), *last = cJSON_GetArrayItem(prices, n - 1);
    data_lock();
    if (r == RANGE_24H) { // Sparkline mit 60 Punkten
        s_d.spark_n = 60;
        for (int i = 0; i < 60; i++) s_d.spark[i] = out[i * outn / 60];
        s_d.spark_ver++;
    }
    if (s_d.chart_range == r) {
        memcpy(s_d.chart, out, sizeof(float) * outn);
        s_d.chart_n = outn;
        s_d.chart_min = mn;
        s_d.chart_max = mx;
        s_d.chart_first = cJSON_GetArrayItem(first, 1)->valuedouble;
        s_d.chart_last = cJSON_GetArrayItem(last, 1)->valuedouble;
        s_d.chart_t0 = (int64_t)cJSON_GetArrayItem(first, 0)->valuedouble;
        s_d.chart_t1 = (int64_t)cJSON_GetArrayItem(last, 0)->valuedouble;
        s_d.chart_ok = true;
        s_d.chart_ver++;
    }
    data_unlock();
    cJSON_Delete(j);
}

// ------------------------------------------------------------------ Netzwerk

static void fetch_network(void)
{
    // mempool.space, bei Verbindungsproblemen die Spiegelung mempool.emzy.de (gleiche API)
    static const char *bases[] = {"https://mempool.space/api", "https://mempool.emzy.de/api"};
    static int bi = 0;
    bool any = false;
    char url[96];
    cJSON *blocks = NULL;
    for (int tries = 0; tries < 2 && !blocks; tries++) {
        snprintf(url, sizeof(url), "%s/v1/blocks", bases[bi]);
        blocks = http_json(url, 8000, NULL, 0);
        if (!blocks) bi = (bi + 1) % 2;
    }
    snprintf(url, sizeof(url), "%s/v1/mining/hashrate/3d", bases[bi]);
    cJSON *hr = http_json(url, 8000, NULL, 0);
    snprintf(url, sizeof(url), "%s/v1/difficulty-adjustment", bases[bi]);
    cJSON *adj = http_json(url, 8000, NULL, 0);
    snprintf(url, sizeof(url), "%s/v1/fees/recommended", bases[bi]);
    cJSON *fees = http_json(url, 8000, NULL, 0);

    data_lock();
    if (cJSON_IsArray(blocks) && cJSON_GetArraySize(blocks) > 0) {
        any = true;
        int n = cJSON_GetArraySize(blocks);
        double fees_sum = 0;
        int fees_n = 0;
        s_d.blocks_n = 0;
        for (int i = 0; i < n; i++) {
            const cJSON *b = cJSON_GetArrayItem(blocks, i);
            const cJSON *ex = cJSON_GetObjectItem(b, "extras");
            if (i == 0) {
                s_d.height = (uint32_t)jnum(b, "height", 0);
                if (s_d.difficulty <= 0) s_d.difficulty = jnum(b, "difficulty", 0);
            }
            if (ex) {
                double tf = jnum(ex, "totalFees", -1);
                if (tf >= 0) { fees_sum += tf; fees_n++; }
            }
            if (s_d.blocks_n < BLOCKS_N) {
                block_info_t *bi = &s_d.blocks[s_d.blocks_n++];
                bi->height = (uint32_t)jnum(b, "height", 0);
                bi->tx_count = (uint32_t)jnum(b, "tx_count", 0);
                bi->timestamp = (int64_t)jnum(b, "timestamp", 0);
                bi->median_fee = ex ? (float)jnum(ex, "medianFee", 0) : 0;
                bi->reward_btc = ex ? jnum(ex, "reward", 0) / 1e8 : 0;
                const cJSON *pool = ex ? cJSON_GetObjectItem(ex, "pool") : NULL;
                if (pool) jstr(pool, "name", bi->pool, sizeof(bi->pool));
                else strlcpy(bi->pool, "Unbekannt", sizeof(bi->pool));
            }
        }
        s_d.avg_fees_btc = fees_n ? fees_sum / fees_n / 1e8 : 0;
    }
    if (hr) {
        any = true;
        s_d.net_hash = jnum(hr, "currentHashrate", s_d.net_hash);
        double d = jnum(hr, "currentDifficulty", 0);
        if (d > 0) s_d.difficulty = d;
    }
    if (adj) {
        any = true;
        s_d.diff_change = jnum(adj, "difficultyChange", 0);
        s_d.diff_progress = jnum(adj, "progressPercent", 0);
        s_d.remaining_blocks = (int)jnum(adj, "remainingBlocks", 0);
        s_d.retarget_ms = (int64_t)jnum(adj, "estimatedRetargetDate", 0);
        s_d.block_time_ms = jnum(adj, "timeAvg", 600000);
    }
    if (fees) {
        any = true;
        s_d.fee_fast = (int)jnum(fees, "fastestFee", 0);
        s_d.fee_half = (int)jnum(fees, "halfHourFee", 0);
        s_d.fee_hour = (int)jnum(fees, "hourFee", 0);
        s_d.fee_eco = (int)jnum(fees, "economyFee", 0);
    }
    if (any) { s_d.net_ok = true; s_d.net_ver++; }
    data_unlock();
    cJSON_Delete(blocks);
    cJSON_Delete(hr);
    cJSON_Delete(adj);
    cJSON_Delete(fees);
}

// ------------------------------------------------------------------ Bitaxe

static void hist_push(double gh, double temp)
{
    s_d.hist_hash[s_d.hist_head] = (float)(gh / 1000.0);
    s_d.hist_temp[s_d.hist_head] = (float)temp;
    s_d.hist_head = (s_d.hist_head + 1) % HIST_PTS;
    if (s_d.hist_n < HIST_PTS) s_d.hist_n++;
    s_d.hist_ver++;
}

int data_hist_copy(float *hash, float *temp, int max)
{
    int n = s_d.hist_n < max ? s_d.hist_n : max;
    int start = (s_d.hist_head - n + HIST_PTS) % HIST_PTS;
    for (int i = 0; i < n; i++) {
        hash[i] = s_d.hist_hash[(start + i) % HIST_PTS];
        temp[i] = s_d.hist_temp[(start + i) % HIST_PTS];
    }
    return n;
}

// Simulierter Gamma 601 (BM1370) für Demo/Offline
static void fill_demo(miner_t *m, int idx)
{
    double t = esp_timer_get_time() / 1e6 + idx * 1000;
    double wob = sin(t / 420) * 30 + sin(t / 97) * 14 + ((rand() % 1000) / 1000.0 - 0.5) * 22;
    double temp = 58 + idx * 3 + sin(t / 600) * 1.6 + ((rand() % 100) / 100.0 - 0.5) * 0.6;
    m->hashrate_gh = 1290 + wob;
    m->expected_gh = 1200;
    m->temp = temp;
    m->vrtemp = temp + 9;
    m->power = 17.8 + sin(t / 90) * 0.5;
    m->voltage_v = 5.08;
    m->current_a = 3.5;
    m->best_diff = 922e6;
    m->session_diff = 43.9e6 + idx * 40e6;
    m->shares_acc = 18432 + (uint32_t)(t / 9);
    m->shares_rej = 21;
    m->uptime_s = 3 * 86400 + (uint32_t)t;
    m->fan_pct = 78 - idx * 4;
    m->fan_rpm = 4120;
    m->freq = 525;
    m->core_mv = 1146;
    m->core_mv_set = 1150;
    m->wifi_rssi = -54;
    strlcpy(m->hostname, "bitaxe-demo", sizeof(m->hostname));
    strlcpy(m->asic, "BM1370", sizeof(m->asic));
    strlcpy(m->board, "601", sizeof(m->board));
    strlcpy(m->version, "Demo", sizeof(m->version));
    strlcpy(m->pool, "solo.ckpool.org:3333", sizeof(m->pool));
}

static void parse_miner(miner_t *m, const cJSON *j)
{
    m->hashrate_gh = jnum(j, "hashRate", 0);
    m->expected_gh = jnum(j, "expectedHashrate", 0);
    m->temp = jnum(j, "temp", 0);
    m->vrtemp = jnum(j, "vrTemp", 0);
    m->power = jnum(j, "power", 0);
    m->voltage_v = jnum(j, "voltage", 0) / 1000.0;
    m->current_a = jnum(j, "current", 0) / 1000.0;
    m->best_diff = parse_diff(j, "bestDiff");
    m->session_diff = parse_diff(j, "bestSessionDiff");
    m->shares_acc = (uint32_t)jnum(j, "sharesAccepted", 0);
    m->shares_rej = (uint32_t)jnum(j, "sharesRejected", 0);
    m->uptime_s = (uint32_t)jnum(j, "uptimeSeconds", 0);
    m->fan_pct = (int)jnum(j, "fanspeed", 0);
    m->fan_rpm = (int)jnum(j, "fanrpm", 0);
    m->freq = (int)jnum(j, "frequency", 0);
    m->core_mv = (int)jnum(j, "coreVoltageActual", 0);
    m->core_mv_set = (int)jnum(j, "coreVoltage", 0);
    m->wifi_rssi = (int)jnum(j, "wifiRSSI", 0);
    jstr(j, "hostname", m->hostname, sizeof(m->hostname));
    jstr(j, "ASICModel", m->asic, sizeof(m->asic));
    jstr(j, "version", m->version, sizeof(m->version));
    jstr(j, "boardVersion", m->board, sizeof(m->board));
    bool fb = jnum(j, "isUsingFallbackStratum", 0) > 0;
    char purl[40] = "";
    jstr(j, fb ? "fallbackStratumURL" : "stratumURL", purl, sizeof(purl));
    snprintf(m->pool, sizeof(m->pool), "%s:%d", purl, (int)jnum(j, fb ? "fallbackStratumPort" : "stratumPort", 0));
}

static void fetch_bitaxe(void)
{
    int n = 0;
    for (int i = 0; i < DECK_MAX_MINERS; i++) {
        const char *host = g_settings.bitaxe_host[i];
        if (!host[0]) continue;
        char url[128], err[48] = "";
        snprintf(url, sizeof(url), "http://%s/api/system/info", host);
        cJSON *j = net_state() == NET_CONNECTED ? http_json(url, 4000, err, sizeof(err)) : NULL;
        if (!j && !err[0]) strlcpy(err, "Kein WLAN", sizeof(err));
        data_lock();
        miner_t *m = &s_d.miners[n];
        if (strcmp(m->host, host)) { memset(m, 0, sizeof(*m)); strlcpy(m->host, host, sizeof(m->host)); }
        if (j && cJSON_GetObjectItem(j, "hashRate")) {
            m->fails = 0;
            m->mode = MINER_ONLINE;
            m->last_online = time(NULL);
            parse_miner(m, j);
            if (n == 0) {
                if (s_d.hist_demo) { s_d.hist_n = 0; s_d.hist_head = 0; s_d.hist_demo = false; }
                static int64_t last_hist = 0;
                int64_t now = esp_timer_get_time();
                if (last_hist == 0 || now - last_hist >= 15 * 1000000LL) {
                    last_hist = now;
                    hist_push(m->hashrate_gh, m->temp);
                }
            }
        } else if (++m->fails >= 2 || m->mode != MINER_ONLINE) {
            // einen Aussetzer überbrücken, danach klar "offline" mit Demo-Werten
            m->mode = MINER_OFFLINE;
            strlcpy(m->err, err, sizeof(m->err));
            fill_demo(m, n);
        }
        data_unlock();
        cJSON_Delete(j);
        n++;
    }
    data_lock();
    if (n == 0) { // kein Gerät eingetragen: ein Demo-Gerät
        miner_t *m = &s_d.miners[0];
        memset(m, 0, sizeof(*m));
        m->mode = MINER_DEMO;
        fill_demo(m, 0);
        n = 1;
    }
    s_d.miner_n = n;
    s_d.miner_ver++;
    data_unlock();
}

// ------------------------------------------------------------------ Tasks

// Bitaxe im LAN alle 5 s, unabhängig von langsamen Internet-Abrufen
static void miner_task(void *arg)
{
    for (;;) {
        int64_t t0 = esp_timer_get_time();
        fetch_bitaxe();
        int64_t left = 5000 - (esp_timer_get_time() - t0) / 1000;
        vTaskDelay(pdMS_TO_TICKS(left > 200 ? left : 200));
    }
}

static void data_task(void *arg)
{
    int64_t next_price = 0, next_net = 0, next_chart = 0;
    chart_range_t cur = (chart_range_t)g_settings.chart_range;
    bool spark_loaded = true; // Sparkline wird nicht mehr angezeigt
    for (;;) {
        int64_t now = esp_timer_get_time() / 1000;
        if (net_state() == NET_CONNECTED && net_time_valid()) {
            int req = s_chart_req;
            if (req >= 0) {
                s_chart_req = -1;
                cur = (chart_range_t)req;
                data_lock();
                s_d.chart_range = cur;
                data_unlock();
                next_chart = 0;
            }
            if (now >= next_price) {
                fetch_price();
                next_price = now + 60000;
            }
            if (now >= next_chart) {
                fetch_chart(cur);
                if (cur == RANGE_24H) spark_loaded = true;
                next_chart = now + 5 * 60000;
            }
            if (!spark_loaded) {
                fetch_chart(RANGE_24H);
                spark_loaded = true;
            }
            if (now >= next_net) {
                fetch_network();
                next_net = now + 30000;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

void data_start(void)
{
    s_mtx = xSemaphoreCreateMutex();
    s_d.chart_range = (chart_range_t)g_settings.chart_range;
    s_d.hist_demo = true;
    // Demo-Verlauf vorbelegen, damit der Chart nie leer ist
    for (int i = 0; i < HIST_PTS; i++) {
        double t = i * 15.0;
        hist_push(1205 + sin(t / 420) * 30 + sin(t / 97) * 14, 61 + sin(t / 600) * 1.6);
    }
    xTaskCreatePinnedToCore(data_task, "data", 10240, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(miner_task, "miner", 6144, NULL, 3, NULL, 0);
}
