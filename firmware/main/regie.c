#include "regie.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

const char *const REGIE_NAMES[RG_N] = {"Neuer Block",          "Kurssprung (1 % in 15 min)", "Gebühren steigen oder fallen",
                                       "Bitaxe: offline, heiß, Hashrate", "Neuer Bestwert-Share", "Neue Schlagzeile",
                                       "Wetter: Morgengruß, Regen, Schnee", "Difficulty-Anpassung"};

#define PENDING_MS 180000 // ein vorgemerktes Ereignis verfällt nach 3 Minuten
#define PX_N 16           // Kurs je Minute: 15 Minuten zurück
#define PX_JUMP 1.0       // Kurssprung in Prozent
#define TEMP_HOT 70.0     // ASIC-Temperatur
#define VR_HOT 85.0       // Spannungsregler

static struct {
    bool on;
    int64_t t;
    char text[64];
} s_pend[RG_N];
static int64_t s_cool[RG_N]; // frühestens dann wieder (ms)

static struct {
    // Kurs
    int64_t px_t[PX_N];
    double px_v[PX_N];
    int px_n;
    int cur; // Währung der gemerkten Kurse (-1 = keine)
    // Gebühren
    double fee_avg;
    int64_t fee_t0, fee_t;
    // Miner
    int mode;
    bool hot;
    int64_t slow_since;
    double best, session;
    bool share_init;
    // News, Wetter, Difficulty
    int64_t news_ts;
    bool news_init;
    int wx_cat;
    int wx_day; // Tag des letzten Morgengrußes
    int rem;
    bool diff_said;
} S = {.cur = -1, .mode = -1, .wx_cat = -1, .wx_day = -1, .rem = -1};

void regie_reset(void)
{
    memset(s_pend, 0, sizeof(s_pend));
    memset(s_cool, 0, sizeof(s_cool));
    int day = S.wx_day;
    memset(&S, 0, sizeof(S));
    S.cur = S.mode = S.wx_cat = S.rem = -1;
    S.wx_day = day; // der Morgengruß kommt trotzdem nur einmal am Tag
}

// deutsche Zahl: Komma statt Punkt, Minus als Halbgeviertstrich
static void de(char *out, size_t len, const char *fmt, double v)
{
    char t[32];
    snprintf(t, sizeof(t), fmt, fabs(v));
    for (char *p = t; *p; p++)
        if (*p == '.') *p = ',';
    snprintf(out, len, "%s%s", v < 0 ? "–" : "", t);
}

static void diff_str(char *out, size_t len, double v)
{
    static const char *const U[] = {"", " K", " M", " G", " T", " P"};
    int i = 0;
    while (v >= 1000 && i < 5) {
        v /= 1000;
        i++;
    }
    char n[24];
    de(n, sizeof(n), v >= 100 ? "%.0f" : "%.1f", v);
    snprintf(out, len, "%s%s", n, U[i]);
}

static void fire(regie_ev_t ev, int64_t ms, int cool_s, const char *fmt, ...)
{
    if (ms < s_cool[ev]) return;
    s_cool[ev] = ms + cool_s * 1000LL;
    s_pend[ev].on = true;
    s_pend[ev].t = ms;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_pend[ev].text, sizeof(s_pend[ev].text), fmt, ap);
    va_end(ap);
}

// Kurs: jede Minute merken, mit dem Stand vor bis zu 15 Minuten vergleichen
static void check_price(const deck_data_t *d, bool eur, int64_t ms)
{
    double px = eur ? d->eur : d->usd;
    if (!d->price_ok || px <= 0) return;
    if (S.cur != (int)eur) { // Währung gewechselt: von vorn
        S.cur = eur;
        S.px_n = 0;
    }
    if (S.px_n && ms - S.px_t[0] >= 5 * 60000) {
        double pct = (px / S.px_v[0] - 1) * 100;
        if (fabs(pct) >= PX_JUMP) {
            char n[16];
            de(n, sizeof(n), "%.1f", pct);
            int min = (int)((ms - S.px_t[0] + 30000) / 60000);
            // Ruhezeit länger als das Fenster, damit derselbe Sprung nicht noch einmal gemeldet wird
            fire(RG_PRICE, ms, 17 * 60, "KURS %s%s %% IN %d MIN", pct > 0 ? "+" : "", n, min > 15 ? 15 : min);
        }
    }
    if (S.px_n && ms - S.px_t[S.px_n - 1] < 60000) return;
    if (S.px_n == PX_N) { // ältesten Wert fallen lassen
        memmove(S.px_t, S.px_t + 1, sizeof(S.px_t[0]) * (PX_N - 1));
        memmove(S.px_v, S.px_v + 1, sizeof(S.px_v[0]) * (PX_N - 1));
        S.px_n--;
    }
    S.px_t[S.px_n] = ms;
    S.px_v[S.px_n++] = px;
}

// Gebühren: gegen den gleitenden Schnitt der letzten Stunde
static void check_fees(const deck_data_t *d, int64_t ms)
{
    double x = d->fee_fast;
    if (!d->net_ok || x <= 0) return;
    if (!S.fee_t0) {
        S.fee_t0 = S.fee_t = ms;
        S.fee_avg = x;
        return;
    }
    if (ms - S.fee_t < 60000) return;
    double k = (ms - S.fee_t) / 3600000.0; // Zeitkonstante 1 h
    S.fee_t = ms;
    bool warm = ms - S.fee_t0 >= 10 * 60000;
    if (warm && x >= 2 * S.fee_avg && x >= 10 && x - S.fee_avg >= 5) {
        fire(RG_FEES, ms, 30 * 60, "GEBÜHREN STEIGEN: %d SAT/VB", (int)x);
        S.fee_avg = x;
    } else if (warm && x <= 0.5 * S.fee_avg && S.fee_avg >= 10) {
        fire(RG_FEES, ms, 30 * 60, "GEBÜHREN FALLEN: %d SAT/VB", (int)x);
        S.fee_avg = x;
    } else S.fee_avg += (x - S.fee_avg) * (k > 1 ? 1 : k);
}

static void check_miner(const deck_data_t *d, int64_t ms)
{
    if (d->miner_n < 1) return;
    const miner_t *m = &d->miners[0];
    int mode = m->mode;
    if (S.mode == MINER_ONLINE && mode == MINER_OFFLINE) fire(RG_MINER, ms, 60, "BITAXE OFFLINE");
    else if (S.mode == MINER_OFFLINE && mode == MINER_ONLINE) fire(RG_MINER, ms, 60, "BITAXE WIEDER ONLINE");
    S.mode = mode;
    if (mode != MINER_ONLINE) {
        S.hot = false;
        S.slow_since = 0;
        S.share_init = false;
        return;
    }
    // zu heiß (erneut erst nach Abkühlen um 4 Grad)
    bool hot = m->temp >= TEMP_HOT || m->vrtemp >= VR_HOT;
    if (hot && !S.hot) {
        char n[16];
        if (m->temp >= TEMP_HOT) {
            de(n, sizeof(n), "%.0f", m->temp);
            fire(RG_MINER, ms, 15 * 60, "BITAXE HEISS: %s °C", n);
        } else {
            de(n, sizeof(n), "%.0f", m->vrtemp);
            fire(RG_MINER, ms, 15 * 60, "SPANNUNGSREGLER HEISS: %s °C", n);
        }
        S.hot = true;
    } else if (S.hot && m->temp < TEMP_HOT - 4 && m->vrtemp < VR_HOT - 4) S.hot = false;
    // Hashrate drei Minuten lang unter der Hälfte des Solls
    if (m->expected_gh > 0 && m->hashrate_gh < 0.5 * m->expected_gh && m->uptime_s > 300) {
        if (!S.slow_since) S.slow_since = ms;
        else if (ms - S.slow_since >= 180000) {
            char n[16];
            de(n, sizeof(n), "%.2f", m->hashrate_gh / 1000);
            fire(RG_MINER, ms, 15 * 60, "HASHRATE EINGEBROCHEN: %s TH/S", n);
            S.slow_since = ms;
        }
    } else S.slow_since = 0;
    // Bestwerte: Rekord über alles, sonst Bestwert seit dem Start des Miners
    if (S.share_init) {
        char n[24];
        if (m->best_diff > S.best * 1.0001) {
            diff_str(n, sizeof(n), m->best_diff);
            s_cool[RG_SHARE] = 0; // ein Rekord wird immer gemeldet
            fire(RG_SHARE, ms, 60, "NEUER REKORD-SHARE: %s", n);
        } else if (m->session_diff > S.session * 1.0001 && m->uptime_s > 600) {
            diff_str(n, sizeof(n), m->session_diff);
            fire(RG_SHARE, ms, 10 * 60, "NEUER BESTWERT: %s", n);
        }
    }
    S.share_init = true;
    S.best = m->best_diff;
    S.session = m->session_diff;
}

static void check_news(const deck_data_t *d, int64_t ms)
{
    if (!d->news.n) return;
    int64_t ts = d->news.item[0].ts;
    if (S.news_init && ts > S.news_ts) fire(RG_NEWS, ms, 10 * 60, "NEUE SCHLAGZEILE");
    if (!S.news_init || ts > S.news_ts) S.news_ts = ts;
    S.news_init = true;
}

// 0 trocken, 1 Regen, 2 Schnee, 3 Gewitter (WMO-Code)
static int wx_cat(int code)
{
    if (code >= 95) return 3;
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return 2;
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return 1;
    return 0;
}

static void check_weather(const deck_data_t *d, int64_t ms, time_t now, bool quiet)
{
    const weather_t *w = &d->wx;
    if (!w->ok) return;
    int cat = wx_cat(w->code);
    if (S.wx_cat >= 0 && cat != S.wx_cat && cat > 0) {
        static const char *const T[4] = {"", "ES REGNET", "ES SCHNEIT", "GEWITTER"};
        fire(RG_WEATHER, ms, 30 * 60, "%s", T[cat]);
    }
    S.wx_cat = cat;
    // Morgengruß: einmal am Tag zwischen 6 und 11 Uhr, sobald die Nachtruhe vorbei ist
    struct tm tm;
    localtime_r(&now, &tm);
    if (!quiet && now > 1600000000 && tm.tm_hour >= 6 && tm.tm_hour < 11 && tm.tm_yday != S.wx_day && w->days > 0) {
        S.wx_day = tm.tm_yday;
        char hi[12], lo[12];
        de(hi, sizeof(hi), "%.0f", round(w->d_max[0]));
        de(lo, sizeof(lo), "%.0f", round(w->d_min[0]));
        if (w->d_rain[0] >= 0) fire(RG_WEATHER, ms, 0, "GUTEN MORGEN · HEUTE %s° / %s° · REGEN %d %%", hi, lo, w->d_rain[0]);
        else fire(RG_WEATHER, ms, 0, "GUTEN MORGEN · HEUTE %s° / %s°", hi, lo);
    }
}

static void check_diff(const deck_data_t *d, int64_t ms)
{
    int rem = d->remaining_blocks;
    if (!d->net_ok || rem <= 0) return;
    if (S.rem > 0 && rem > S.rem + 1000) { // neue Periode
        fire(RG_DIFF, ms, 60, "DIFFICULTY ANGEPASST · NEUE PERIODE");
        S.diff_said = false;
    } else if (S.rem > 0 && rem <= 6 && !S.diff_said) {
        char n[16];
        de(n, sizeof(n), "%.1f", d->diff_change);
        fire(RG_DIFF, ms, 60, "DIFFICULTY IN %d BLÖCKEN: %s%s %%", rem, d->diff_change > 0 ? "+" : "", n);
        S.diff_said = true;
    }
    S.rem = rem;
}

void regie_feed(const deck_data_t *d, bool eur, int64_t ms, time_t now, bool quiet)
{
    check_price(d, eur, ms);
    check_fees(d, ms);
    check_miner(d, ms);
    check_news(d, ms);
    check_weather(d, ms, now, quiet);
    check_diff(d, ms);
}

bool regie_take(int64_t ms, uint16_t mask, regie_hit_t *hit)
{
    // Reihenfolge nach Wichtigkeit
    static const regie_ev_t order[] = {RG_MINER, RG_PRICE, RG_FEES, RG_DIFF, RG_SHARE, RG_NEWS, RG_WEATHER};
    bool found = false;
    for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        regie_ev_t ev = order[i];
        if (!s_pend[ev].on) continue;
        if (!((mask >> ev) & 1) || ms - s_pend[ev].t > PENDING_MS) { // abgewählt oder veraltet
            s_pend[ev].on = false;
            continue;
        }
        if (found) continue; // die übrigen bleiben vorgemerkt
        found = true;
        s_pend[ev].on = false;
        hit->ev = ev;
        strncpy(hit->text, s_pend[ev].text, sizeof(hit->text) - 1);
        hit->text[sizeof(hit->text) - 1] = 0;
    }
    return found;
}
