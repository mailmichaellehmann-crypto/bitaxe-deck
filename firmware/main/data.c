#include "data.h"

#include <ctype.h>
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
#include "nvs.h"
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

// Lädt eine URL komplett (Puffer im PSRAM, mit free freigeben; NULL bei Fehler)
static char *http_text(const char *url, int timeout_ms, const char *accept, char *err, int errlen)
{
    http_buf_t b = {0};
    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = http_evt,
        .user_data = &b,
        .timeout_ms = timeout_ms,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "satoshi-deck/1.0",
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "Accept", accept);
    esp_err_t r = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    if (r == ESP_OK && status == 200 && b.buf) return b.buf;
    if (err) {
        if (r != ESP_OK) snprintf(err, errlen, "%s", r == ESP_ERR_HTTP_CONNECT ? "Keine Verbindung" : esp_err_to_name(r));
        else snprintf(err, errlen, "HTTP %d", status);
    }
    ESP_LOGW(TAG, "GET %s -> %s / %d", url, esp_err_to_name(r), status);
    free(b.buf);
    return NULL;
}

// Lädt eine URL komplett und parst sie als JSON (NULL bei Fehler)
static cJSON *http_json(const char *url, int timeout_ms, char *err, int errlen)
{
    char *t = http_text(url, timeout_ms, "application/json", err, errlen);
    if (!t) return NULL;
    cJSON *j = cJSON_Parse(t);
    if (!j && err) snprintf(err, errlen, "Ungültige Antwort");
    free(t);
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

// ------------------------------------------------------------------ Wetter (Open-Meteo, ohne Schlüssel)

static char s_wx_for[48]; // Ort, zu dem lat/lon gehören

static void url_enc(char *out, size_t len, const char *s)
{
    size_t o = 0;
    for (; *s && o + 4 < len; s++) {
        unsigned char c = *s;
        if (isalnum(c) || c == '-' || c == '.' || c == '_') out[o++] = c;
        else o += snprintf(out + o, len - o, "%%%02X", c);
    }
    out[o] = 0;
}

static void wx_error(const char *e)
{
    data_lock();
    strlcpy(s_d.wx.err, e, sizeof(s_d.wx.err));
    s_d.wx.ver++;
    data_unlock();
}

// Ort in Koordinaten umwandeln: „52.52, 13.41“ direkt, sonst Name oder Postleitzahl („Frankfurt, DE“ = mit Ländercode)
static bool wx_geocode(const char *place)
{
    double la, lo;
    char rest;
    if (sscanf(place, "%lf , %lf %c", &la, &lo, &rest) == 2 && fabs(la) <= 90 && fabs(lo) <= 180) {
        data_lock();
        s_d.wx.lat = la;
        s_d.wx.lon = lo;
        snprintf(s_d.wx.place, sizeof(s_d.wx.place), "%.2f° %c  %.2f° %c", fabs(la), la < 0 ? 'S' : 'N', fabs(lo), lo < 0 ? 'W' : 'O');
        for (char *p = s_d.wx.place; *p; p++) if (*p == '.') *p = ',';
        s_d.wx.region[0] = 0;
        data_unlock();
        return true;
    }
    char name[48], cc[3] = "";
    strlcpy(name, place, sizeof(name));
    char *comma = strchr(name, ',');
    if (comma) {
        *comma = 0;
        const char *p = comma + 1;
        while (*p == ' ') p++;
        if (isalpha((unsigned char)p[0]) && isalpha((unsigned char)p[1]) && (!p[2] || p[2] == ' ')) {
            cc[0] = toupper((unsigned char)p[0]);
            cc[1] = toupper((unsigned char)p[1]);
        }
    }
    for (int n = strlen(name); n > 0 && name[n - 1] == ' '; n--) name[n - 1] = 0;
    char enc[150], url[256], err[48] = "";
    url_enc(enc, sizeof(enc), name);
    snprintf(url, sizeof(url), "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&language=de&format=json%s%s", enc,
             cc[0] ? "&countryCode=" : "", cc);
    cJSON *j = http_json(url, 10000, err, sizeof(err));
    const cJSON *r = cJSON_GetArrayItem(cJSON_GetObjectItem(j, "results"), 0);
    if (r) {
        char a1[40], co[40];
        jstr(r, "admin1", a1, sizeof(a1));
        jstr(r, "country", co, sizeof(co));
        data_lock();
        s_d.wx.lat = jnum(r, "latitude", 0);
        s_d.wx.lon = jnum(r, "longitude", 0);
        jstr(r, "name", s_d.wx.place, sizeof(s_d.wx.place));
        snprintf(s_d.wx.region, sizeof(s_d.wx.region), "%s%s%s", a1, a1[0] && co[0] ? ", " : "", co);
        data_unlock();
        ESP_LOGI(TAG, "Wetter-Ort gefunden");
    } else wx_error(j ? "Ort nicht gefunden" : err[0] ? err : "Keine Verbindung");
    cJSON_Delete(j);
    return r != NULL;
}

// Wochentag (0 = Sonntag) aus „JJJJ-MM-TT“
static int wday_of(const char *s)
{
    static const int t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int y, m, d;
    if (!s || sscanf(s, "%d-%d-%d", &y, &m, &d) != 3 || m < 1 || m > 12) return -1;
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

static double jarr(const cJSON *o, const char *k, int i, double def)
{
    const cJSON *v = cJSON_GetArrayItem(cJSON_GetObjectItem(o, k), i);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

// gibt die Wartezeit bis zum nächsten Abruf zurück (ms)
static int fetch_weather(void)
{
    const char *place = g_settings.wx_place;
#ifdef CONFIG_DECK_SNAPSHOT
    place = "Berlin"; // Aufnahmen immer mit demselben Ort
#endif
    if (!place[0]) {
        data_lock();
        s_d.wx.ok = false;
        data_unlock();
        wx_error("Ort im Browser eintragen");
        return 60000;
    }
    bool ll = g_settings.wx_ll;
#ifdef CONFIG_DECK_SNAPSHOT
    ll = false;
#endif
    if (strcmp(s_wx_for, place)) {
        if (ll) { // im Browser aus der Ortssuche gewählt
            data_lock();
            s_d.wx.lat = g_settings.wx_lat;
            s_d.wx.lon = g_settings.wx_lon;
            strlcpy(s_d.wx.place, place, sizeof(s_d.wx.place));
            s_d.wx.region[0] = 0;
            data_unlock();
        } else if (!wx_geocode(place)) return 5 * 60000;
        strlcpy(s_wx_for, place, sizeof(s_wx_for));
    }
    char url[400], err[48] = "";
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,relative_humidity_2m,apparent_temperature,is_day,weather_code,wind_speed_10m"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
             "&timezone=auto&forecast_days=%d",
             s_d.wx.lat, s_d.wx.lon, WX_DAYS);
    cJSON *j = http_json(url, 10000, err, sizeof(err));
    const cJSON *cur = cJSON_GetObjectItem(j, "current"), *day = cJSON_GetObjectItem(j, "daily");
    if (!cur || !day) {
        wx_error(err[0] ? err : "Ungültige Antwort");
        cJSON_Delete(j);
        return 2 * 60000;
    }
    weather_t *w = &s_d.wx;
    data_lock();
    w->temp = jnum(cur, "temperature_2m", 0);
    w->feels = jnum(cur, "apparent_temperature", w->temp);
    w->wind = jnum(cur, "wind_speed_10m", 0);
    w->humidity = (int)jnum(cur, "relative_humidity_2m", 0);
    w->code = (int)jnum(cur, "weather_code", 0);
    w->is_day = jnum(cur, "is_day", 1) > 0;
    const cJSON *dt = cJSON_GetObjectItem(day, "time");
    w->days = cJSON_GetArraySize(dt) < WX_DAYS ? cJSON_GetArraySize(dt) : WX_DAYS;
    for (int i = 0; i < w->days; i++) {
        w->d_wday[i] = wday_of(cJSON_GetStringValue(cJSON_GetArrayItem(dt, i)));
        w->d_code[i] = (int)jarr(day, "weather_code", i, 0);
        w->d_max[i] = jarr(day, "temperature_2m_max", i, 0);
        w->d_min[i] = jarr(day, "temperature_2m_min", i, 0);
        w->d_rain[i] = (int)jarr(day, "precipitation_probability_max", i, -1);
    }
    w->updated = time(NULL);
    w->ok = w->days > 0;
    w->err[0] = 0;
    w->ver++;
    data_unlock();
    cJSON_Delete(j);
    return 15 * 60000; // Open-Meteo rechnet alle 15 Minuten neu
}

// ------------------------------------------------------------------ Bitcoin-News

#define NEWS_PARSE 12 // so viele lesen, davon die neuesten NEWS_N behalten

// Quellen in der Reihenfolge von NEWS_SOURCE_NAMES; Crypto Vision (cryptocurrency.cv) ohne Schlüssel,
// die anderen als RSS-Feed
static const struct { const char *url; bool rss; } NEWS_URL[NEWS_SOURCES] = {
    {"https://cryptocurrency.cv/api/bitcoin?limit=40", false},
    {"https://www.blocktrainer.de/feed.xml", true},
    {"https://bitcoinmagazine.com/feed", true},
};

// Zeichen, die die Schriften der Karte haben (Emojis u. Ä. fallen weg)
static bool font_has(uint32_t c)
{
    return (c >= 0x20 && c <= 0x7E) || (c >= 0xA0 && c <= 0xFF) || (c >= 0x2013 && c <= 0x201E) || c == 0x2022 || c == 0x2026 ||
           c == 0x20AC;
}

static void put_utf8(char **o, const char *end, uint32_t c)
{
    char b[4];
    int n = c < 0x80 ? 1 : c < 0x800 ? 2 : 3;
    if (n == 1) b[0] = c;
    else if (n == 2) { b[0] = 0xC0 | c >> 6; b[1] = 0x80 | (c & 0x3F); }
    else { b[0] = 0xE0 | c >> 12; b[1] = 0x80 | ((c >> 6) & 0x3F); b[2] = 0x80 | (c & 0x3F); }
    if (*o + n > end) return;
    memcpy(*o, b, n);
    *o += n;
}

// Schlagzeile säubern: CDATA, HTML-Entities, unbekannte Zeichen, mehrfacher Leerraum
static void news_clean(char *out, size_t len, const char *s, int n)
{
    const char *e = s + n;
    while (s < e && isspace((unsigned char)*s)) s++;
    if (e - s >= 12 && !strncmp(s, "<![CDATA[", 9)) {
        s += 9;
        for (const char *q = s; q + 3 <= e; q++)
            if (!strncmp(q, "]]>", 3)) { e = q; break; }
    }
    char *o = out, *end = out + len - 1;
    bool sp = true;
    while (s < e) {
        uint32_t c = (unsigned char)*s;
        if (c == '&') {
            const char *semi = memchr(s, ';', e - s < 10 ? e - s : 10);
            if (semi) {
                char ent[10] = {0};
                memcpy(ent, s + 1, semi - s - 1);
                c = ent[0] == '#' ? (uint32_t)strtoul(ent + 1 + (ent[1] == 'x'), NULL, ent[1] == 'x' ? 16 : 10)
                    : !strcmp(ent, "amp")  ? '&'
                    : !strcmp(ent, "lt")   ? '<'
                    : !strcmp(ent, "gt")   ? '>'
                    : !strcmp(ent, "quot") ? '"'
                    : !strcmp(ent, "apos") ? '\''
                    : !strcmp(ent, "nbsp") ? ' '
                                           : 0;
                s = semi + 1;
            } else s++;
        } else if (c < 0x80) s++;
        else { // UTF-8 lesen
            int k = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
            c &= k == 3 ? 0x07 : k == 2 ? 0x0F : 0x1F;
            s++;
            for (; k > 0 && s < e && ((unsigned char)*s & 0xC0) == 0x80; k--, s++) c = c << 6 | ((unsigned char)*s & 0x3F);
        }
        if (c == 0xA0 || c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (!font_has(c)) continue;
        if (c == ' ' && sp) continue;
        sp = c == ' ';
        put_utf8(&o, end, c);
    }
    while (o > out && o[-1] == ' ') o--;
    *o = 0;
}

static int64_t utc_secs(int y, int mo, int d, int h, int mi, int s)
{
    y -= mo <= 2; // Tage seit 1970 (H. Hinnant, days_from_civil)
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = y - era * 400, doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t days = era * 146097LL + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    return days * 86400 + h * 3600 + mi * 60 + s;
}

// „2026-10-23T10:30:18.000Z“
static int64_t parse_iso(const char *s)
{
    int y, mo, d, h, mi, se;
    if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
    return utc_secs(y, mo, d, h, mi, se);
}

// „Fri, 23 Oct 2026 09:49:29 +0200“
static int64_t parse_rfc822(const char *s)
{
    static const char M[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = "", tz[8] = "";
    int d, y, h, mi, se;
    const char *p = strchr(s, ',');
    if (sscanf(p ? p + 1 : s, "%d %3s %d %d:%d:%d %7s", &d, mon, &y, &h, &mi, &se, tz) < 6) return 0;
    const char *q = strstr(M, mon);
    if (!q || !mon[0]) return 0;
    int64_t t = utc_secs(y, (q - M) / 3 + 1, d, h, mi, se);
    if ((tz[0] == '+' || tz[0] == '-') && strlen(tz) >= 5) {
        int off = ((tz[1] - '0') * 10 + tz[2] - '0') * 3600 + ((tz[3] - '0') * 10 + tz[4] - '0') * 60;
        t -= tz[0] == '+' ? off : -off;
    }
    return t;
}

// Inhalt von <tag>…</tag> zwischen p und end
static const char *xml_tag(const char *p, const char *end, const char *tag, int *len)
{
    char a[16], b[16];
    snprintf(a, sizeof(a), "<%s>", tag);
    snprintf(b, sizeof(b), "</%s>", tag);
    const char *s = strstr(p, a);
    if (!s || s > end) return NULL;
    s += strlen(a);
    const char *e = strstr(s, b);
    if (!e || e > end) return NULL;
    *len = e - s;
    return s;
}

static int news_rss(const char *x, news_item_t *out, const char *src)
{
    int n = 0;
    for (const char *p = x; n < NEWS_PARSE && (p = strstr(p, "<item")) != NULL;) {
        const char *end = strstr(p, "</item>");
        if (!end) break;
        int tl, dl;
        const char *t = xml_tag(p, end, "title", &tl), *d = xml_tag(p, end, "pubDate", &dl);
        if (t) {
            news_clean(out[n].title, sizeof(out[n].title), t, tl);
            char ds[40] = "";
            if (d) memcpy(ds, d, dl < 39 ? dl : 39);
            out[n].ts = parse_rfc822(ds);
            strlcpy(out[n].source, src, sizeof(out[n].source));
            if (out[n].title[0]) n++;
        }
        p = end + 7;
    }
    return n;
}

static int news_cv(const cJSON *j, news_item_t *out)
{
    int n = 0;
    const cJSON *a;
    cJSON_ArrayForEach(a, cJSON_GetObjectItem(j, "articles"))
    {
        if (n >= NEWS_PARSE) break;
        const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(a, "title"));
        const char *src = cJSON_GetStringValue(cJSON_GetObjectItem(a, "source"));
        if (!t || (src && !strcmp(src, "Stacker News"))) continue; // dort vor allem Forums-Threads
        news_clean(out[n].title, sizeof(out[n].title), t, strlen(t));
        strlcpy(out[n].source, src ? src : "", sizeof(out[n].source));
        out[n].ts = parse_iso(cJSON_GetStringValue(cJSON_GetObjectItem(a, "pubDate")));
        if (out[n].title[0]) n++;
    }
    return n;
}

// gibt die Wartezeit bis zum nächsten Abruf zurück (ms)
static int fetch_news(void)
{
    int src = g_settings.news_src < NEWS_SOURCES ? g_settings.news_src : 0;
    news_item_t *it = heap_caps_calloc(NEWS_PARSE, sizeof(news_item_t), MALLOC_CAP_SPIRAM);
    if (!it) return 60000;
    char err[48] = "";
    int n = 0;
    if (NEWS_URL[src].rss) {
        char *x = http_text(NEWS_URL[src].url, 15000, "application/rss+xml, application/xml, */*", err, sizeof(err));
        if (x) n = news_rss(x, it, NEWS_SOURCE_NAMES[src]);
        free(x);
    } else {
        cJSON *j = http_json(NEWS_URL[src].url, 15000, err, sizeof(err));
        if (j) n = news_cv(j, it);
        cJSON_Delete(j);
    }
    for (int i = 1; i < n; i++) // neueste zuerst
        for (int k = i; k > 0 && it[k].ts > it[k - 1].ts; k--) {
            news_item_t tmp = it[k];
            it[k] = it[k - 1];
            it[k - 1] = tmp;
        }
    if (n > NEWS_N) n = NEWS_N;
    data_lock();
    if (n > 0) {
        memcpy(s_d.news.item, it, sizeof(news_item_t) * n);
        s_d.news.n = n;
        s_d.news.err[0] = 0;
        s_d.news.updated = time(NULL);
    } else strlcpy(s_d.news.err, err[0] ? err : "Keine Schlagzeilen", sizeof(s_d.news.err));
    s_d.news.ver++;
    data_unlock();
    free(it);
    return n > 0 ? 10 * 60000 : 2 * 60000;
}

// Kursverlauf in USD für „Kurs seit Meldung“: Kraken, 15-Minuten-Kerzen der letzten 7,5 Tage in einem
// Abruf (ohne Schlüssel; schont das Limit von CoinGecko). Je Kerze der Schlusskurs am Ende der Kerze.
static void fetch_series(void)
{
    cJSON *j = http_json("https://api.kraken.com/0/public/OHLC?pair=XBTUSD&interval=15", 15000, NULL, 0);
    const cJSON *res = cJSON_GetObjectItem(j, "result"), *rows = NULL, *r;
    cJSON_ArrayForEach(r, res) if (cJSON_IsArray(r)) { rows = r; break; } // „XXBTZUSD“
    uint32_t now = time(NULL);
    if (s_d.px && cJSON_GetArraySize(rows) >= 2) {
        data_lock();
        int k = 0;
        cJSON_ArrayForEach(r, rows)
        {
            if (k >= PXS_N) break;
            const char *close = cJSON_GetStringValue(cJSON_GetArrayItem(r, 4));
            uint32_t t = (uint32_t)cJSON_GetArrayItem(r, 0)->valuedouble + 900;
            if (!close) continue;
            s_d.px->t[k] = t < now ? t : now; // letzte Kerze läuft noch
            s_d.px->v[k] = strtof(close, NULL);
            if (k == 0 || s_d.px->t[k] > s_d.px->t[k - 1]) k++;
        }
        s_d.px->n = k;
        s_d.px_ver++;
        data_unlock();
    }
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

// ------------------------------------------------------------------ Stromzähler

typedef struct {
    uint32_t ver;
    uint32_t day;
    double wh, day_wh;
    int64_t since;
} energy_blob_t;

static bool s_energy_dirty;

static void energy_load(void)
{
    nvs_handle_t h;
    energy_blob_t e;
    size_t len = sizeof(e);
    if (nvs_open("deck", NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_blob(h, "energy", &e, &len) == ESP_OK && len == sizeof(e) && e.ver == 1) {
        s_d.energy_wh = e.wh;
        s_d.energy_day_wh = e.day_wh;
        s_d.energy_day = e.day;
        s_d.energy_since = e.since;
    }
    nvs_close(h);
}

void data_energy_save(void)
{
    data_lock();
    energy_blob_t e = {.ver = 1, .day = s_d.energy_day, .wh = s_d.energy_wh, .day_wh = s_d.energy_day_wh, .since = s_d.energy_since};
    s_energy_dirty = false;
    data_unlock();
    nvs_handle_t h;
    if (nvs_open("deck", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "energy", &e, sizeof(e));
    nvs_commit(h);
    nvs_close(h);
}

void data_energy_reset(void)
{
    data_lock();
    s_d.energy_wh = 0;
    s_d.energy_day_wh = 0;
    s_d.energy_since = net_time_valid() ? time(NULL) : 0;
    data_unlock();
    data_energy_save();
}

// Leistung über die Zeit aufsummieren (unter data_lock); watts < 0 = keine Messung, Lücke nicht zählen
static void energy_add(double watts)
{
    static int64_t last_us = 0;
    int64_t now = esp_timer_get_time();
    if (net_time_valid()) {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        uint32_t day = (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
        if (day != s_d.energy_day) { // neuer Tag
            s_d.energy_day = day;
            s_d.energy_day_wh = 0;
            s_energy_dirty = true;
        }
        if (!s_d.energy_since) s_d.energy_since = t;
    }
    if (watts >= 0 && watts < 5000 && last_us) {
        double dt_h = (now - last_us) / 3.6e9;
        if (dt_h > 30.0 / 3600) dt_h = 30.0 / 3600; // nach Aussetzern höchstens 30 s anrechnen
        s_d.energy_wh += watts * dt_h;
        s_d.energy_day_wh += watts * dt_h;
        s_energy_dirty = true;
    }
    last_us = watts >= 0 ? now : 0;
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
            if (n == 0) energy_add(m->power);
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
            if (n == 0) energy_add(-1);
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
        // Stromzähler alle 10 min sichern (schont den Flash)
        static int64_t last_save = 0;
        if (s_energy_dirty && t0 - last_save > 10 * 60 * 1000000LL) {
            last_save = t0;
            data_energy_save();
        }
        int64_t left = 5000 - (esp_timer_get_time() - t0) / 1000;
        vTaskDelay(pdMS_TO_TICKS(left > 200 ? left : 200));
    }
}

static void data_task(void *arg)
{
    int64_t next_price = 0, next_net = 0, next_chart = 0, next_wx = 0, next_news = 0, next_px = 0;
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
            if (now >= next_wx) next_wx = esp_timer_get_time() / 1000 + fetch_weather();
            if ((g_settings.card_mask >> 13) & 1) { // nur, wenn die News-Karte an ist
                if (now >= next_news) next_news = esp_timer_get_time() / 1000 + fetch_news();
                if (now >= next_px) {
                    fetch_series();
                    next_px = now + 10 * 60000;
                }
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
    s_d.px = heap_caps_calloc(1, sizeof(px_series_t), MALLOC_CAP_SPIRAM);
    energy_load();
    // Demo-Verlauf vorbelegen, damit der Chart nie leer ist
    for (int i = 0; i < HIST_PTS; i++) {
        double t = i * 15.0;
        hist_push(1205 + sin(t / 420) * 30 + sin(t / 97) * 14, 61 + sin(t / 600) * 1.6);
    }
    xTaskCreatePinnedToCore(data_task, "data", 10240, NULL, 3, NULL, 0);
    xTaskCreatePinnedToCore(miner_task, "miner", 6144, NULL, 3, NULL, 0);
}
