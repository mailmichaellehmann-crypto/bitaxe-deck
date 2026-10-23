// Satoshi Deck – Oberfläche für 480x320 (LVGL 8.4) im Strike-Stil
// Karten (nach links/rechts wischen):
//  0 Preis · 1 USD & EUR · 2 Chart · 3 Block · 4 Neuer Block · 5 Halving · 6 Gebühren · 7 Difficulty · 8 Uhr
//  9 Wetter · 10 Bitaxe · 11 Kosten · 12 Share-Lotterie · 13 News (Reihenfolge einstellbar)
// Gestaltung: tiefschwarzer Grund, gedämpftes Weiß, Geist für große Zahlen, Geist Mono für
// Beschriftungen, 1-px-Rahmen, Punktmatrix-Chart, Pixel-Mosaik; einzige Akzentfarbe: Pink #FFD4D4.

#include "ui.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "data.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "net.h"
#include "settings.h"
#include "shares.h"
#ifdef CONFIG_DECK_SNAPSHOT
#include "mbedtls/base64.h"
#endif
#include "ota.h"

LV_FONT_DECLARE(font_g20);
LV_FONT_DECLARE(font_g26);
LV_FONT_DECLARE(font_g36);
LV_FONT_DECLARE(font_g50);
LV_FONT_DECLARE(font_g72);  // nur Ziffern . , : + - % °
LV_FONT_DECLARE(font_g96);  // nur Ziffern . , : + - % °
LV_FONT_DECLARE(font_g120); // nur Ziffern . , : + - %
LV_FONT_DECLARE(font_m12);
LV_FONT_DECLARE(font_m14);
LV_FONT_DECLARE(font_m16);
LV_FONT_DECLARE(font_m20);

// ------------------------------------------------------------------ Farben
#define C_BG lv_color_hex(0x000000)
#define C_PANEL lv_color_hex(0x0A0A0A)
#define C_LINE lv_color_hex(0x1F1F1F)
#define C_LINE2 lv_color_hex(0x333333)
#define C_TEXT lv_color_hex(0xECECEC)
#define C_TEXT2 lv_color_hex(0x9A9A9A)
#define C_TEXT3 lv_color_hex(0x5E5E5E)
#define C_PINK lv_color_hex(0xFFD4D4)    // Akzent (sRGB 255/212/212)
#define C_PINK_DIM lv_color_hex(0x7A6464)
#define C_UP lv_color_hex(0x9BE3B0)
#define C_DOWN lv_color_hex(0xFF9C9C)

#define W LCD_H_RES
#define H LCD_V_RES
#define N_TILES DECK_CARDS
#define CARD_NEWBLOCK 4 // Karte „Neuer Block“
#define CARD_WEATHER 9  // Karte „Wetter“
#define CARD_NEWS 13    // Karte „News“
#define NIGHT_BRIGHT 12 // Beleuchtung im Nachtmodus (%), schwach, aber nicht aus
#define MX 20 // Seitenrand

// ------------------------------------------------------------------ Formatierung (deutsch)

static void fmt_de(char *out, size_t len, double v, int dec)
{
    char tmp[48];
    bool neg = v < 0;
    if (neg) v = -v;
    snprintf(tmp, sizeof(tmp), "%.*f", dec, v);
    char *dot = strchr(tmp, '.');
    int ilen = dot ? (int)(dot - tmp) : (int)strlen(tmp);
    char res[64];
    int o = 0;
    if (neg) res[o++] = '-';
    for (int i = 0; i < ilen; i++) {
        res[o++] = tmp[i];
        int rem = ilen - i - 1;
        if (rem > 0 && rem % 3 == 0) res[o++] = '.';
    }
    if (dot) {
        res[o++] = ',';
        strcpy(res + o, dot + 1);
    } else res[o] = 0;
    strlcpy(out, res, len);
}

static void fmt_plain(char *out, size_t len, double v) { snprintf(out, len, "%.0f", v); }

static const char *fmt_diff(char *out, size_t len, double v)
{
    static const char *const U[] = {"", "K", "M", "G", "T", "P", "E"};
    int i = 0;
    while (fabs(v) >= 1000 && i < 6) { v /= 1000; i++; }
    char num[32];
    fmt_de(num, sizeof(num), v, v >= 100 ? 0 : 1);
    snprintf(out, len, "%s%s%s", num, U[i][0] ? " " : "", U[i]);
    return out;
}

static void fmt_change(char *out, size_t len, double pct)
{
    char n[24];
    fmt_de(n, sizeof(n), fabs(pct), 2);
    snprintf(out, len, "%s%s %%", pct >= 0 ? "+" : "–", n);
}

static const char *cur_sym(void) { return g_settings.currency ? "€" : "$"; }

// ------------------------------------------------------------------ Grundbausteine


static void no_scroll(lv_obj_t *o)
{
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_GESTURE_BUBBLE);
}

static lv_obj_t *rect(lv_obj_t *parent, int x, int y, int w, int h, lv_color_t c)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, c, 0);
    no_scroll(o);
    return o;
}

// Fläche mit 1-px-Rahmen und runden Ecken
static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *p = rect(parent, x, y, w, h, C_BG);
    lv_obj_set_style_radius(p, 10, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_color(p, C_LINE, 0);
    return p;
}

// Text an Grundlinie setzen: anchor 'l' links ab x, 'r' rechtsbündig bis x, 'c' mittig auf x.
static lv_obj_t *txt(lv_obj_t *parent, int pw, const lv_font_t *f, lv_color_t c, const char *s, char anchor, int x, int baseline)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_label_set_text(l, s);
    int y = baseline - (f->line_height - f->base_line);
    if (anchor == 'r') lv_obj_align(l, LV_ALIGN_TOP_RIGHT, x - pw, y);
    else if (anchor == 'c') lv_obj_align(l, LV_ALIGN_TOP_MID, x - pw / 2, y);
    else lv_obj_align(l, LV_ALIGN_TOP_LEFT, x, y);
    return l;
}

// Kleine Mono-Beschriftung in Großbuchstaben, leicht gesperrt
static lv_obj_t *cap(lv_obj_t *parent, int pw, const char *s, char anchor, int x, int baseline)
{
    lv_obj_t *l = txt(parent, pw, &font_m12, C_TEXT2, s, anchor, x, baseline);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    return l;
}

static int ascent(const lv_font_t *f) { return f->line_height - f->base_line; }

// Große Zahlen enger setzen (wie bei Strike: negative Laufweite)
static lv_obj_t *tight(lv_obj_t *l, int px)
{
    lv_obj_set_style_text_letter_space(l, -px, 0);
    return l;
}

// sanft „atmender“ Punkt (Live-Anzeige)
static void breathe_exec(void *var, int32_t v) { lv_obj_set_style_bg_opa(var, (lv_opa_t)(90 + v * 165 / 1000), 0); }
static void breathe(lv_obj_t *o)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, breathe_exec);
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_set_time(&a, 1400);
    lv_anim_set_playback_time(&a, 1400);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

// Weiches Hochzählen großer Zahlen
typedef void (*fmt_fn)(char *, size_t, double);
typedef struct { lv_obj_t *lbl; fmt_fn fn; double from, to, cur; bool init; } tween_t;

static void tween_exec(void *var, int32_t v)
{
    tween_t *t = var;
    t->cur = t->from + (t->to - t->from) * v / 1000.0;
    char buf[48];
    t->fn(buf, sizeof(buf), t->cur);
    lv_label_set_text(t->lbl, buf);
}

static void tween_to(tween_t *t, double to)
{
    char buf[48];
    if (!t->init || to == t->to) {
        t->init = true;
        t->cur = t->to = to;
        t->fn(buf, sizeof(buf), to);
        lv_label_set_text(t->lbl, buf);
        return;
    }
    t->from = t->cur;
    t->to = to;
    lv_anim_del(t, tween_exec);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, t);
    lv_anim_set_exec_cb(&a, tween_exec);
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_set_time(&a, 700);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void f_int(char *o, size_t l, double v) { fmt_de(o, l, v, 0); }

// Großen Preis auf 120 px setzen, bei sechsstelligen Kursen auf 96 px verkleinern
static void fit_big(lv_obj_t *l, double v, int x, int baseline)
{
    const lv_font_t *f = v >= 100000 ? &font_g96 : &font_g120;
    if (lv_obj_get_style_text_font(l, 0) == f) return;
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, x, baseline - ascent(f));
}

// Pixel-Puffer (Canvas) – wird nur bei neuen Daten gezeichnet
typedef struct {
    lv_obj_t *cv;
    lv_color_t *buf;
    int w, h;
} canvas_t;

static lv_obj_t *canvas_create(canvas_t *c, lv_obj_t *parent, int x, int y, int w, int h)
{
    c->w = w;
    c->h = h;
    c->buf = heap_caps_malloc(LV_CANVAS_BUF_SIZE_TRUE_COLOR(w, h), MALLOC_CAP_SPIRAM);
    c->cv = lv_canvas_create(parent);
    lv_canvas_set_buffer(c->cv, c->buf, w, h, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(c->cv, x, y);
    lv_canvas_fill_bg(c->cv, C_BG, LV_OPA_COVER);
    no_scroll(c->cv);
    return c->cv;
}

static void fill_box(canvas_t *c, int x, int y, int w, int h, lv_color_t col)
{
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= c->h) continue;
        for (int xx = x; xx < x + w; xx++)
            if (xx >= 0 && xx < c->w) c->buf[yy * c->w + xx] = col;
    }
}

// 8-Bit-Leinwand mit Grau-Palette für bildschirmfüllende Mosaike (halber Speicher):
// Index = Grauwert 0 … 239, darüber 16 Stufen Pink (PX_PINK0 = schwach … PX_PINK = voll)
#define PX_PINK0 240
#define PX_PINK 255
#define PX_PINK_DIM (PX_PINK0 + 7)
typedef struct {
    lv_obj_t *cv;
    void *buf;
    uint8_t *px;
    int w, h;
} canvas8_t;

static void canvas8_create(canvas8_t *c, lv_obj_t *parent, int x, int y, int w, int h)
{
    c->w = w;
    c->h = h;
    c->buf = heap_caps_malloc(LV_CANVAS_BUF_SIZE_INDEXED_8BIT(w, h), MALLOC_CAP_SPIRAM);
    c->cv = lv_canvas_create(parent);
    lv_canvas_set_buffer(c->cv, c->buf, w, h, LV_IMG_CF_INDEXED_8BIT);
    for (int i = 0; i < PX_PINK0; i++) lv_canvas_set_palette(c->cv, i, lv_color_make(i, i, i));
    for (int i = 0; i < 16; i++) lv_canvas_set_palette(c->cv, PX_PINK0 + i, lv_color_mix(C_PINK, C_BG, (uint8_t)(32 + i * 223 / 15)));
    c->px = (uint8_t *)c->buf + 4 * 256;
    memset(c->px, 0, w * h);
    lv_obj_set_pos(c->cv, x, y);
    lv_obj_clear_flag(c->cv, LV_OBJ_FLAG_CLICKABLE);
    no_scroll(c->cv);
}

static void fill8(canvas8_t *c, int x, int y, int w, int h, uint8_t v)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > c->w) w = c->w - x;
    if (y + h > c->h) h = c->h - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; yy++) memset(c->px + yy * c->w + x, v, w);
}

static uint8_t g8(int v) { return v < 0 ? 0 : v >= PX_PINK0 ? PX_PINK0 - 1 : v; } // Grauwert als Index

static uint32_t hash2(int a, int b)
{
    uint32_t h = (uint32_t)a * 374761393u + (uint32_t)b * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static lv_color_t gray(int v) { return lv_color_make(v, v, v); }

// Wert der Datenreihe an Position t (0..1), linear interpoliert
static float sample(const float *v, int n, float t)
{
    float p = t * (n - 1);
    int i = (int)p;
    if (i >= n - 1) return v[n - 1];
    float f = p - i;
    return v[i] + (v[i + 1] - v[i]) * f;
}

// ------------------------------------------------------------------ Karten

static struct { tween_t px; lv_obj_t *cur, *chg, *alt, *time, *hilo; canvas_t mos; } A;      // Preis
static struct { tween_t usd, eur; lv_obj_t *chg_usd, *chg_eur, *time; } B;                     // USD/EUR
static struct { lv_obj_t *cur, *chg, *range_lbl, *range, *xlab[5], *msg; tween_t px; } C;      // Chart
static struct { lv_obj_t *height, *age, *halving; } D;                                       // Block
static struct { lv_obj_t *fee[3], *fiat[3], *height, *sats, *sats_t; } E;                    // Gebühren
static struct { lv_obj_t *chg, *cells[55], *blocks, *pct; } F;                               // Difficulty
static struct { lv_obj_t *time, *date, *trend; } G;                                          // Uhr
static struct { lv_obj_t *title, *status, *dot, *hash, *unit, *legend, *cells[55], *val[3]; } HB; // Bitaxe

// ---- 0 Preis: großer Kurs, darunter der Kursverlauf als Pixel-Mosaik
#define MOS_CELL 8

// Höhe einer Mosaik-Säule in Zellen (mindestens 1)
static int mos_cells(double val, double mn, double mx, int rows)
{
    double range = mx - mn > 1e-9 ? mx - mn : 1;
    int hc = 1 + (int)((val - mn) / range * (rows - 1) + 0.5);
    return hc < 1 ? 1 : hc > rows ? rows : hc;
}

// Kursverlauf als Säulen aus Pixel-Zellen; y0 = Abstand oben, damit die Säulen unten bündig stehen
static void mosaic_fill(canvas_t *c, int y0, int rows, const float *v, int n, double mn, double mx, int seed)
{
    lv_canvas_fill_bg(c->cv, C_BG, LV_OPA_COVER);
    int cols = c->w / MOS_CELL;
    for (int col = 0; col < cols; col++) {
        float val = sample(v, n, (float)col / (cols - 1));
        int hcells = mos_cells(val, mn, mx, rows);
        for (int r = 0; r < rows; r++) {
            int from_bottom = rows - 1 - r;
            int x = col * MOS_CELL, y = y0 + r * MOS_CELL;
            uint32_t h = hash2(col, r + seed);
            lv_color_t cl;
            if (from_bottom < hcells - 1) {
                int g = 0x1C + (int)(0x2A * (float)from_bottom / rows) + (int)(h % 0x14); // Säule
                cl = gray(g);
            } else if (from_bottom == hcells - 1) {
                cl = col == cols - 1 ? C_PINK : gray(0x8A + (int)(h % 0x20)); // Spitze, letzte in Pink
            } else {
                cl = (h % 7) == 0 ? gray(0x10) : gray(0x08); // leere Zellen, ganz schwach
            }
            fill_box(c, x, y, MOS_CELL - 1, MOS_CELL - 1, cl);
        }
    }
    lv_obj_invalidate(c->cv);
}

static void mosaic_draw(const float *v, int n, double mn, double mx)
{
    mosaic_fill(&A.mos, 0, A.mos.h / MOS_CELL, v, n, mn, mx, 0);
}

static void build_price(lv_obj_t *t)
{
    A.cur = cap(t, W, "BTC / USD", 'l', MX, 30);
    A.time = txt(t, W, &font_m14, C_TEXT2, "", 'r', W - MX, 30);
    A.px.lbl = tight(txt(t, W, &font_g120, C_TEXT, "–", 'l', MX - 6, 150), 5);
    A.px.fn = f_int;
    A.chg = txt(t, W, &font_m16, C_UP, "", 'l', MX, 186);
    A.alt = txt(t, W, &font_m16, C_TEXT2, "", 'r', W - MX, 186);
    canvas_create(&A.mos, t, MX, 204, 440, 96);
    A.hilo = cap(t, W, "", 'l', MX, 314);
}

// ---- 1 USD & EUR
static void build_dual(lv_obj_t *t)
{
    cap(t, W, "BTC / USD", 'l', MX, 30);
    B.time = txt(t, W, &font_m14, C_TEXT2, "", 'r', W - MX, 30);
    B.usd.lbl = tight(txt(t, W, &font_g96, C_TEXT, "–", 'l', MX - 5, 128), 4);
    B.usd.fn = f_int;
    B.chg_usd = txt(t, W, &font_m16, C_UP, "", 'r', W - MX, 128);
    rect(t, MX, 158, W - 2 * MX, 1, C_LINE);
    cap(t, W, "BTC / EUR", 'l', MX, 190);
    B.eur.lbl = tight(txt(t, W, &font_g96, C_TEXT, "–", 'l', MX - 5, 288), 4);
    B.eur.fn = f_int;
    B.chg_eur = txt(t, W, &font_m16, C_UP, "", 'r', W - MX, 288);
}

// ---- 2 Chart: Punktmatrix, letzter Wert mit pinkem Plus
#define CH_X MX
#define CH_Y 98
#define CH_W 440
#define CH_H 188
#define DOT 4
static canvas_t CC;
static struct {
    float v[CHART_PTS];
    int n;
    double mn, mx;
    int64_t t0, t1;
    lv_obj_t *hi, *lo, *xline, *xdot, *tip, *tip_p, *tip_t, *hit;
    bool scrub;
} K;

// ------------------------------------------------------------------ Karten-Wechsler
// Eigener Wechsler statt lv_tileview: wischen oder links/rechts tippen, endlos im Kreis in der
// eingestellten Reihenfolge (g_settings.card_order), ausgeschaltete Karten werden übersprungen. Nur die aktuelle (und beim Wechsel die nächste)
// Karte ist sichtbar.
static lv_obj_t *s_card[N_TILES];
static int s_cur = 0;       // aktuelle Karte
static int s_nb = -1;       // Nachbarkarte während Wischen/Animation
static int s_dir = 1;       // +1 = nächste kommt von rechts, -1 = vorige von links
static bool s_animating;
static bool s_booting = true; // Boot-Screen sichtbar: kein automatischer Seitenwechsel
static struct { bool down, drag, long_pr; lv_point_t p0; int64_t t0; int dx; } SW;
static void open_cfg(void);

static bool card_on(int i) { return (g_settings.card_mask >> i) & 1; }

static int card_pos(int id)
{
    for (int p = 0; p < N_TILES; p++)
        if (g_settings.card_order[p] == id) return p;
    return 0;
}

// nächste eingeschaltete Karte in der Reihenfolge (dir = +1 / -1)
static int card_step(int from, int dir)
{
    int p0 = card_pos(from);
    for (int k = 1; k <= N_TILES; k++) {
        int i = g_settings.card_order[((p0 + dir * k) % N_TILES + N_TILES) % N_TILES];
        if (card_on(i)) return i;
    }
    return from;
}

static void car_place(int dx)
{
    lv_obj_set_x(s_card[s_cur], dx);
    if (s_nb >= 0 && s_nb != s_cur) lv_obj_set_x(s_card[s_nb], (s_dir > 0 ? W : -W) + dx);
}

static void car_set_nb(int dir)
{
    int nb = card_step(s_cur, dir);
    if (s_nb >= 0 && s_nb != nb && s_nb != s_cur) lv_obj_add_flag(s_card[s_nb], LV_OBJ_FLAG_HIDDEN);
    s_dir = dir;
    s_nb = nb == s_cur ? -1 : nb;
    if (s_nb >= 0) lv_obj_clear_flag(s_card[s_nb], LV_OBJ_FLAG_HIDDEN);
}

static void car_anim_exec(void *var, int32_t v) { car_place(v); }

static void car_anim_ready(lv_anim_t *a)
{
    bool switched = a->end_value != 0;
    if (switched && s_nb >= 0) {
        lv_obj_add_flag(s_card[s_cur], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_x(s_card[s_cur], 0);
        s_cur = s_nb;
    } else if (s_nb >= 0) {
        lv_obj_add_flag(s_card[s_nb], LV_OBJ_FLAG_HIDDEN);
    }
    s_nb = -1;
    lv_obj_set_x(s_card[s_cur], 0);
    s_animating = false;
}

static void car_animate(int from, int to, int ms)
{
    s_animating = true;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_card[s_cur]);
    lv_anim_set_exec_cb(&a, car_anim_exec);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&a, car_anim_ready);
    lv_anim_start(&a);
}

// eine Karte weiter (+1) oder zurück (-1), mit Animation
static void car_go(int dir)
{
    if (s_animating || SW.drag) return;
    car_set_nb(dir);
    if (s_nb < 0) return;
    car_place(0);
    car_animate(0, dir > 0 ? -W : W, 280);
}

// direkt zu einer Karte springen (Menü, Preisalarm)
static void car_jump(int idx, bool anim)
{
    if (idx < 0 || idx >= N_TILES) return;
    if (idx == s_cur && !s_animating && s_nb < 0) return;
    if (anim && !s_animating && idx != s_cur) {
        if (s_nb >= 0) lv_obj_add_flag(s_card[s_nb], LV_OBJ_FLAG_HIDDEN);
        s_dir = 1;
        s_nb = idx;
        lv_obj_clear_flag(s_card[s_nb], LV_OBJ_FLAG_HIDDEN);
        car_place(0);
        car_animate(0, -W, 280);
        return;
    }
    lv_anim_del(s_card[s_cur], car_anim_exec);
    for (int i = 0; i < N_TILES; i++) {
        lv_obj_set_x(s_card[i], 0);
        if (i != idx) lv_obj_add_flag(s_card[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(s_card[idx], LV_OBJ_FLAG_HIDDEN);
    s_cur = idx;
    s_nb = -1;
    s_animating = false;
}

static int car_current(void) { return s_cur; }

static void card_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_point_t p;
    lv_indev_t *in = lv_indev_get_act();
    if (!in) return;
    lv_indev_get_point(in, &p);
    if (code == LV_EVENT_PRESSED) {
        SW.down = true;
        SW.drag = false;
        SW.long_pr = false;
        SW.p0 = p;
        SW.t0 = esp_timer_get_time() / 1000;
        SW.dx = 0;
    } else if (code == LV_EVENT_PRESSING && SW.down) {
        if (K.scrub || s_animating) return;
        int dx = p.x - SW.p0.x, dy = p.y - SW.p0.y;
        if (!SW.drag && abs(dx) > 10 && abs(dx) > abs(dy)) {
            SW.drag = true;
            car_set_nb(dx < 0 ? 1 : -1);
        }
        if (SW.drag) {
            int dir = dx < 0 ? 1 : -1;
            if (dir != s_dir) car_set_nb(dir); // Richtung gewechselt
            SW.dx = dx;
            car_place(dx);
        }
    } else if (code == LV_EVENT_LONG_PRESSED) {
        SW.long_pr = true;
        if (!SW.drag && lv_event_get_target(e) != K.hit) open_cfg();
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && SW.down) {
        SW.down = false;
        if (SW.drag) {
            SW.drag = false;
            int64_t dt = esp_timer_get_time() / 1000 - SW.t0;
            bool fling = dt < 300 && abs(SW.dx) > 40;
            if (s_nb >= 0 && (abs(SW.dx) > W / 4 || fling)) car_animate(SW.dx, s_dir > 0 ? -W : W, 220);
            else car_animate(SW.dx, 0, 200);
        } else if (!SW.long_pr && !K.scrub && code == LV_EVENT_RELEASED) {
            if (p.x < W / 4) car_go(-1);
            else if (p.x > W * 3 / 4) car_go(1);
        }
    }
}

// Chart-Stil „Pixel“: dasselbe Mosaik wie auf der Preiskarte, 55 × 23 Zellen
#define PIX_ROWS (CH_H / MOS_CELL)
#define PIX_Y0 (CH_H - PIX_ROWS * MOS_CELL)
static bool chart_pixel(void) { return g_settings.chart_style == 2; }

// y im Chart zu einem Kurs; im Pixel-Stil die Mitte der obersten Zelle der Säule
static int val_y(double v)
{
    if (chart_pixel()) return PIX_Y0 + (PIX_ROWS - mos_cells(v, K.mn, K.mx, PIX_ROWS)) * MOS_CELL + (MOS_CELL - 1) / 2;
    return (int)((1.0 - (v - K.mn) / (K.mx - K.mn)) * (CH_H - 1) + 0.5);
}

static void chart_draw(void)
{
    canvas_t *c = &CC;
    if (chart_pixel()) {
        mosaic_fill(c, PIX_Y0, PIX_ROWS, K.v, K.n, K.mn, K.mx, 97);
        return;
    }
    lv_canvas_fill_bg(c->cv, C_BG, LV_OPA_COVER);
    int cols = c->w / DOT, rows = c->h / DOT;
    int prev_r = -1;
    for (int col = 0; col < cols; col++) {
        float val = sample(K.v, K.n, (float)col / (cols - 1));
        int r = val_y(val) / DOT;
        if (r >= rows) r = rows - 1;
        // Linie als Treppe aus Punkten: Lücken zur Nachbarspalte schließen
        int r0 = r, r1 = r;
        if (prev_r >= 0) {
            if (prev_r < r) r0 = prev_r + 1;
            else if (prev_r > r) r1 = prev_r - 1;
        }
        for (int rr = 0; rr < rows; rr++) {
            int x = col * DOT + 1, y = rr * DOT + 1;
            if (rr >= r0 && rr <= r1) fill_box(c, x, y, 2, 2, C_TEXT);
            else if (rr > r1 && g_settings.chart_style == 0) {
                int depth = rr - r;
                int g = 0x34 - depth * 0x26 / rows; // Fläche: schwache Punkte, nach unten dunkler
                if (g < 0x12) g = 0x12;
                fill_box(c, x, y, 2, 2, gray(g));
            } else if ((rr % 6) == 5 && (col % 2) == 0) {
                fill_box(c, x, y, 1, 1, gray(0x1A)); // feines Raster
            }
        }
        prev_r = r;
    }
    // letzter Wert: gepunktete pinke Senkrechte und Plus
    int lx = (cols - 1) * DOT + 1, ly = val_y(K.v[K.n - 1]);
    for (int y = ly + 8; y < c->h; y += DOT) fill_box(c, lx, y, 2, 2, C_PINK_DIM);
    fill_box(c, lx - 4, ly - 1, 10, 3, C_PINK);
    fill_box(c, lx, ly - 5, 3, 11, C_PINK);
    lv_obj_invalidate(c->cv);
}

static void scrub_update(void)
{
    lv_point_t p;
    lv_indev_get_point(lv_indev_get_act(), &p);
    int x = p.x - CH_X;
    if (x < 0) x = 0;
    if (x > CH_W - 1) x = CH_W - 1;
    int idx, px, py, ds;
    if (chart_pixel()) { // auf die Säule einrasten, oberste Zelle leuchtet pink
        int cols = CH_W / MOS_CELL, col = x / MOS_CELL;
        if (col > cols - 1) col = cols - 1;
        float t = (float)col / (cols - 1);
        idx = (int)(t * (K.n - 1) + 0.5f);
        px = col * MOS_CELL + (MOS_CELL - 1) / 2;
        py = val_y(sample(K.v, K.n, t));
        ds = MOS_CELL - 1;
    } else {
        idx = (x * (K.n - 1) + (CH_W - 1) / 2) / (CH_W - 1);
        px = idx * (CH_W - 1) / (K.n - 1);
        py = val_y(K.v[idx]);
        ds = 10;
    }
    lv_obj_set_size(K.xdot, ds, ds);
    lv_obj_set_style_radius(K.xdot, chart_pixel() ? 0 : LV_RADIUS_CIRCLE, 0);
    lv_obj_set_pos(K.xline, CH_X + px, CH_Y);
    lv_obj_set_pos(K.xdot, CH_X + px - ds / 2, CH_Y + py - ds / 2);
    char b[40], n[24];
    fmt_de(n, sizeof(n), K.v[idx], 0);
    snprintf(b, sizeof(b), "%s %s", n, cur_sym());
    lv_label_set_text(K.tip_p, b);
    time_t ts = (time_t)((K.t0 + (K.t1 - K.t0) * idx / (K.n - 1)) / 1000);
    struct tm tm;
    localtime_r(&ts, &tm);
    strftime(b, sizeof(b), "%d.%m.%Y %H:%M", &tm);
    lv_label_set_text(K.tip_t, b);
    int tx = px + 14;
    if (tx + 150 > CH_W) tx = px - 14 - 150;
    lv_obj_set_pos(K.tip, CH_X + tx, CH_Y + (py > CH_H / 2 ? 6 : CH_H - 60));
}

static void scrub_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (K.n < 2) return;
    if (code == LV_EVENT_LONG_PRESSED) {
        if (SW.drag) return; // gerade am Wischen
        K.scrub = true;
        lv_obj_clear_flag(K.xline, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(K.xdot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(K.tip, LV_OBJ_FLAG_HIDDEN);
        scrub_update();
    } else if (code == LV_EVENT_PRESSING && K.scrub) {
        scrub_update();
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && K.scrub) {
        K.scrub = false;
        lv_obj_add_flag(K.xline, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(K.xdot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(K.tip, LV_OBJ_FLAG_HIDDEN);
    }
}

static const char *RANGE_NAMES[4] = {"24H", "7T", "30T", "1J"};

static void chart_loading(void)
{
    lv_label_set_text(C.msg, "LÄDT …");
    lv_obj_clear_flag(C.msg, LV_OBJ_FLAG_HIDDEN);
}

static void range_cb(lv_event_t *e)
{
    g_settings.chart_range = (g_settings.chart_range + 1) % 4;
    settings_save();
    lv_label_set_text(C.range_lbl, RANGE_NAMES[g_settings.chart_range]);
    chart_loading();
    data_request_chart((chart_range_t)g_settings.chart_range);
}

// Pillen-Knopf mit Rahmen
static lv_obj_t *pill(lv_obj_t *parent, int x, int y, int w, int h, const char *s, lv_obj_t **lbl)
{
    lv_obj_t *p = rect(parent, x, y, w, h, C_BG);
    lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_color(p, C_LINE2, 0);
    lv_obj_set_style_bg_color(p, lv_color_hex(0x161616), LV_STATE_PRESSED);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_ext_click_area(p, 10);
    lv_obj_t *l = txt(p, w, &font_m14, C_TEXT, s, 'c', w / 2, h / 2 + 5);
    if (lbl) *lbl = l;
    return p;
}

static void build_chart(lv_obj_t *t)
{
    C.cur = cap(t, W, "BTC / USD", 'l', MX, 30);
    C.range = pill(t, W - MX - 64, 12, 64, 28, RANGE_NAMES[g_settings.chart_range], &C.range_lbl);
    lv_obj_add_event_cb(C.range, range_cb, LV_EVENT_CLICKED, NULL);
    C.px.lbl = tight(txt(t, W, &font_g50, C_TEXT, "–", 'l', MX - 3, 82), 2);
    C.px.fn = f_int;
    C.chg = txt(t, W, &font_m16, C_UP, "", 'r', W - MX, 82);
    canvas_create(&CC, t, CH_X, CH_Y, CH_W, CH_H);
    for (int i = 0; i < 5; i++) C.xlab[i] = txt(t, W, &font_m12, C_TEXT3, "", i == 0 ? 'l' : i == 4 ? 'r' : 'c', CH_X + i * CH_W / 4, 308);
    C.msg = txt(t, W, &font_m14, C_TEXT2, "LÄDT …", 'c', W / 2, CH_Y + CH_H / 2);
    K.hi = cap(t, W, "", 'l', 0, 0);
    K.lo = cap(t, W, "", 'l', 0, 0);
    // Fadenkreuz: Finger kurz halten, dann ziehen
    K.hit = rect(t, CH_X, CH_Y, CH_W, CH_H, C_BG);
    lv_obj_set_style_bg_opa(K.hit, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(K.hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(K.hit, scrub_cb, LV_EVENT_ALL, NULL);
    K.xline = rect(t, 0, CH_Y, 1, CH_H, C_TEXT);
    lv_obj_set_style_bg_opa(K.xline, LV_OPA_40, 0);
    K.xdot = rect(t, 0, 0, 10, 10, C_PINK);
    lv_obj_set_style_radius(K.xdot, LV_RADIUS_CIRCLE, 0);
    K.tip = panel(t, 0, 0, 150, 52);
    lv_obj_set_style_bg_color(K.tip, C_PANEL, 0);
    lv_obj_set_style_border_color(K.tip, C_LINE2, 0);
    K.tip_p = txt(K.tip, 150, &font_g20, C_TEXT, "", 'l', 12, 24);
    K.tip_t = txt(K.tip, 150, &font_m12, C_TEXT2, "", 'l', 12, 42);
    lv_obj_add_flag(K.xline, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(K.xdot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(K.tip, LV_OBJ_FLAG_HIDDEN);
}

// ---- 3 Block
static void build_block(lv_obj_t *t)
{
    cap(t, W, "BLOCKHÖHE", 'l', MX, 30);
    D.height = tight(txt(t, W, &font_g120, C_TEXT, "–", 'l', MX - 6, 190), 5);
    rect(t, MX, 222, W - 2 * MX, 1, C_LINE);
    D.age = txt(t, W, &font_m14, C_TEXT2, "", 'l', MX, 252);
    D.halving = txt(t, W, &font_m14, C_TEXT2, "", 'l', MX, 278);
}

// ---- 4 Gebühren
static void build_fees(lv_obj_t *t)
{
    cap(t, W, "TRANSAKTIONSGEBÜHR", 'l', MX, 30);
    static const char *pn[3] = {"NIEDRIG", "MITTEL", "HOCH"};
    for (int i = 0; i < 3; i++) {
        int x = MX + i * 148, w = 144;
        lv_obj_t *p = panel(t, x, 44, w, 128);
        cap(p, w, pn[i], 'l', 14, 26);
        E.fee[i] = tight(txt(p, w, &font_g50, C_TEXT, "–", 'l', 12, 84), 2);
        txt(p, w, &font_m14, C_TEXT2, "sat/vB", 'l', 14, 108);
        E.fiat[i] = txt(p, w, &font_m14, C_TEXT, "", 'r', w - 14, 108);
    }
    lv_obj_t *p1 = panel(t, MX, 182, 218, 118);
    cap(p1, 218, "BLOCKHÖHE", 'l', 14, 26);
    E.height = tight(txt(p1, 218, &font_g36, C_TEXT, "–", 'l', 12, 84), 1);
    lv_obj_t *p2 = panel(t, MX + 222, 182, 218, 118);
    E.sats_t = cap(p2, 218, "SATS / 1 USD", 'l', 14, 26);
    E.sats = tight(txt(p2, 218, &font_g36, C_TEXT, "–", 'l', 12, 84), 1);
}

// Reihe aus Kästchen als Fortschrittsanzeige
static void cell_row(lv_obj_t *t, lv_obj_t **cells, int y)
{
    for (int i = 0; i < 55; i++) {
        cells[i] = rect(t, MX + i * 8, y, 6, 6, C_LINE);
        lv_obj_set_style_radius(cells[i], 1, 0);
    }
}

static void cell_fill(lv_obj_t **cells, int n1, lv_color_t c1, int n2, lv_color_t c2)
{
    for (int i = 0; i < 55; i++) lv_obj_set_style_bg_color(cells[i], i < n1 ? c1 : i < n2 ? c2 : C_LINE, 0);
}

// ---- 5 Difficulty
static void build_difficulty(lv_obj_t *t)
{
    cap(t, W, "DIFFICULTY-ANPASSUNG", 'l', MX, 30);
    cap(t, W, "ERWARTETE ÄNDERUNG", 'l', MX, 72);
    F.chg = tight(txt(t, W, &font_g72, C_UP, "–", 'l', MX - 4, 150), 3);
    cell_row(t, F.cells, 176);
    rect(t, MX, 204, W - 2 * MX, 1, C_LINE);
    cap(t, W, "RETARGET IN", 'l', MX, 232);
    F.blocks = tight(txt(t, W, &font_g36, C_TEXT, "–", 'l', MX - 1, 276), 1);
    txt(t, W, &font_m14, C_TEXT2, "Blöcken", 'l', MX, 300);
    cap(t, W, "FORTSCHRITT", 'r', W - MX, 232);
    F.pct = tight(txt(t, W, &font_g36, C_TEXT, "–", 'r', W - MX, 276), 1);
    txt(t, W, &font_m14, C_TEXT2, "der Epoche", 'r', W - MX, 300);
}

// ---- 6 Uhr: als LED-Punktanzeige (Standard) oder klassisch. Beide zeigen, ob der Kurs in 24 h gestiegen
// (hellgrün) oder gefallen (pink) ist.
#define DECK_SNAP_CLOCK 3                 // Screenshot-Modus: Uhr-Stil für die Aufnahmen
static void build_led(lv_obj_t *t);
static int s_trend;                       // 24h-Kurs: +1 gestiegen, -1 gefallen, 0 unbekannt
static lv_color_t trend_col(void) { return s_trend > 0 ? C_UP : s_trend < 0 ? C_PINK : C_TEXT3; }

static void build_clock(lv_obj_t *t)
{
    G.time = tight(txt(t, W, &font_g120, C_TEXT, "--:--", 'c', W / 2, 180), 4);
    G.date = txt(t, W, &font_m16, C_TEXT2, "", 'c', W / 2, 226);
    lv_obj_set_style_text_letter_space(G.date, 2, 0);
    G.trend = txt(t, W, &font_m14, C_TEXT3, "", 'c', W / 2, 262);
    lv_obj_set_style_text_letter_space(G.trend, 1, 0);
    int style = g_settings.clock_style;
#ifdef CONFIG_DECK_SNAPSHOT
    style = DECK_SNAP_CLOCK; // Aufnahmen mit festem Uhr-Stil (ändert die gespeicherte Einstellung nicht)
#endif
    if (style == 3) { // LED: die klassischen Beschriftungen bleiben (Zeit auf den Preiskarten), unsichtbar
        lv_obj_add_flag(G.time, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(G.date, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(G.trend, LV_OBJ_FLAG_HIDDEN);
        build_led(t);
    }
}

// ---- Uhr als LED-Punktanzeige: Siebensegment-Ziffern aus LED-Punkten (7 × 13 je Ziffer), passend
// zur Punktmatrix des Charts – quadratische Punkte wie überall im Deck, leuchtende glimmen leicht.
// Unbeleuchtete Segmente schimmern schwach, dahinter ein feines Punktraster.
// Die LEDs leuchten in der Trendfarbe (gestiegen hellgrün, gefallen pink), darunter eine Sekundenreihe.
#define LD_P 10                           // Abstand der LED-Punkte
#define LD_DOT 8                          // Kantenlänge eines LED-Punkts
#define LD_CW 7                           // Ziffer: 7 × 13 Punkte
#define LD_CH 13
#define LD_COLS 37                        // Ziffer, 2 frei, Ziffer, 2 frei, Doppelpunkt, 2 frei, Ziffer, 2 frei, Ziffer
#define LD_Y 46                           // Lage der Leinwand
#define LD_H 182
#define LD_ROW0 1                         // erste Punktreihe der Ziffern
#define LD_SEC_Y 164                      // Mitte der Sekundenreihe (in der Leinwand)
#define LD_FADE 180                       // Überblenden der Segmente (ms)
#define LD_GRID 16                        // Helligkeit: Raster, unbeleuchtetes Segment, Glimmen, Sekunden
#define LD_GHOST 34
#define LD_PAST 92
#define LD_GLOW 46                        // Glimmen um leuchtende Punkte
static canvas8_t LDC;
static struct {
    lv_obj_t *tile, *date, *trend;
    uint8_t seg_old[4], seg_new[4];       // beleuchtete Segmente je Ziffer (Bits a … g)
    int64_t t0;                           // Start des Überblendens, 0 = ruhig
    int shown, sec, x0;
    bool on;
} LD;

// Segmente: a oben, b rechts oben, c rechts unten, d unten, e links unten, f links oben, g Mitte
static uint8_t ld_segs(char ch)
{
    static const uint8_t m[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
    return ch >= '0' && ch <= '9' ? m[ch - '0'] : 0x40; // sonst „-“
}

// gehört der Punkt (x, y) einer Ziffer zu einem Segment? Liefert dessen Bit, sonst 0
static uint8_t ld_seg_at(int x, int y)
{
    if (y == 0 && x >= 1 && x <= 5) return 0x01;
    if (x == 6 && y >= 1 && y <= 5) return 0x02;
    if (x == 6 && y >= 7 && y <= 11) return 0x04;
    if (y == 12 && x >= 1 && x <= 5) return 0x08;
    if (x == 0 && y >= 7 && y <= 11) return 0x10;
    if (x == 0 && y >= 1 && y <= 5) return 0x20;
    if (y == 6 && x >= 1 && x <= 5) return 0x40;
    return 0;
}

// quadratischer Punkt (wie die Zellen und Punkte überall im Deck); Werte sind Helligkeiten
// (Palette = Trendfarbe), es gewinnt der hellere
static void ld_dot(int cx, int cy, int size, uint8_t v)
{
    canvas8_t *c = &LDC;
    int x0 = cx - size / 2, y0 = cy - size / 2;
    for (int y = y0; y < y0 + size; y++) {
        if (y < 0 || y >= c->h) continue;
        for (int x = x0; x < x0 + size; x++) {
            if (x < 0 || x >= c->w) continue;
            uint8_t *p = &c->px[y * c->w + x];
            if (v > *p) *p = v;
        }
    }
}

static void ld_palette(void)
{
    lv_color_t col = s_trend ? trend_col() : C_TEXT;
    for (int i = 0; i < 256; i++) lv_canvas_set_palette(LDC.cv, i, lv_color_mix(col, C_BG, (uint8_t)i));
}

static void ld_draw(int64_t now)
{
    canvas8_t *c = &LDC;
    memset(c->px, 0, c->w * c->h);
    float f = LD.t0 ? (now - LD.t0) / (float)LD_FADE : 1;
    if (f >= 1) { f = 1; LD.t0 = 0; }
    int rows = LD_SEC_Y / LD_P - 1;
    // feines Raster wie ein LED-Panel (an den Ziffern ausgerichtet)
    for (int gy = 0; gy < rows; gy++)
        for (int gx = -(LD.x0 / LD_P); gx * LD_P + LD.x0 < c->w; gx++)
            ld_dot(LD.x0 + gx * LD_P + LD_P / 2, gy * LD_P + LD_P / 2, 2, LD_GRID);
    static const int dcol[4] = {0, 9, 21, 30};
    for (int k = 0; k < 4; k++)
        for (int y = 0; y < LD_CH; y++)
            for (int x = 0; x < LD_CW; x++) {
                uint8_t bit = ld_seg_at(x, y);
                if (!bit) continue;
                float o = (LD.seg_old[k] & bit) ? 1 : 0, n = (LD.seg_new[k] & bit) ? 1 : 0, on = o + (n - o) * f;
                int cx = LD.x0 + (dcol[k] + x) * LD_P + LD_P / 2, cy = (LD_ROW0 + y) * LD_P + LD_P / 2;
                if (on > 0.02f) ld_dot(cx, cy, LD_DOT + 4, (uint8_t)(LD_GLOW * on)); // Glimmen
                ld_dot(cx, cy, LD_DOT, (uint8_t)(LD_GHOST + (255 - LD_GHOST) * on));
            }
    // Doppelpunkt
    bool colon = LD.sec < 0 || LD.sec % 2 == 0;
    for (int i = 0; i < 2; i++) {
        int cx = LD.x0 + 18 * LD_P + LD_P / 2, cy = (LD_ROW0 + (i ? 9 : 3)) * LD_P + LD_P / 2;
        if (colon) ld_dot(cx, cy, LD_DOT + 4, LD_GLOW);
        ld_dot(cx, cy, LD_DOT, colon ? 255 : 70);
    }
    // Sekundenreihe: 60 kleine LEDs
    int sp = 6, sx0 = (c->w - 59 * sp) / 2;
    for (int q = 0; q < 60; q++) {
        uint8_t v = LD.sec < 0 || q > LD.sec ? LD_GHOST : q == LD.sec ? 255 : LD_PAST;
        ld_dot(sx0 + q * sp, LD_SEC_Y, 3, v);
    }
    lv_obj_invalidate(c->cv);
}

static void ld_tick(lv_timer_t *tm)
{
    if (!LD.on) return;
    int64_t now = esp_timer_get_time() / 1000;
    bool vis = s_card[s_cur] == LD.tile || (s_nb >= 0 && s_card[s_nb] == LD.tile);
    char d[4] = {'-', '-', '-', '-'};
    int minute = -1, sec = -1;
    if (net_time_valid()) {
        time_t tt = time(NULL);
        struct tm tmv;
        localtime_r(&tt, &tmv);
        d[0] = '0' + tmv.tm_hour / 10;
        d[1] = '0' + tmv.tm_hour % 10;
        d[2] = '0' + tmv.tm_min / 10;
        d[3] = '0' + tmv.tm_min % 10;
        minute = tmv.tm_hour * 60 + tmv.tm_min;
        sec = tmv.tm_sec;
    }
    bool redraw = false;
    if (minute != LD.shown) { // neue Minute: geänderte Segmente blenden über
        for (int k = 0; k < 4; k++) {
            LD.seg_old[k] = LD.seg_new[k];
            LD.seg_new[k] = ld_segs(d[k]);
        }
        LD.t0 = vis && LD.shown != -2 ? now : 0;
        LD.shown = minute;
        redraw = true;
    }
    if (sec != LD.sec) {
        LD.sec = sec;
        redraw = true;
    }
    if (LD.t0) redraw = true;
    if (redraw && (vis || !LD.t0)) ld_draw(now);
}

static void build_led(lv_obj_t *t)
{
    LD.on = true;
    LD.tile = t;
    canvas8_create(&LDC, t, 0, LD_Y, W, LD_H);
    LD.x0 = (W - LD_COLS * LD_P) / 2;
    for (int k = 0; k < 4; k++) LD.seg_old[k] = LD.seg_new[k] = 0x40;
    LD.date = txt(t, W, &font_m16, C_TEXT2, "", 'c', W / 2, LD_Y + LD_H + 34);
    lv_obj_set_style_text_letter_space(LD.date, 2, 0);
    LD.trend = txt(t, W, &font_m14, C_TEXT3, "", 'c', W / 2, LD_Y + LD_H + 62);
    lv_obj_set_style_text_letter_space(LD.trend, 1, 0);
    ld_palette();
    LD.shown = -2;
    LD.sec = -1;
    ld_draw(0);
    lv_timer_create(ld_tick, 30, NULL);
}

// 24h-Änderung auf der Uhr zeigen: LED-Farbe und Zeile unter dem Datum in Grün oder Pink
static void clock_trend_apply(double chg)
{
    int tr = chg > 0.004 ? 1 : chg < -0.004 ? -1 : 0;
    char n[24], b[48];
    fmt_change(n, sizeof(n), chg);
    snprintf(b, sizeof(b), "BTC · 24H  %s", n);
    bool changed = tr != s_trend;
    s_trend = tr;
    lv_label_set_text(G.trend, b);
    lv_obj_set_style_text_color(G.trend, trend_col(), 0);
    if (LD.on) {
        lv_label_set_text(LD.trend, b);
        lv_obj_set_style_text_color(LD.trend, trend_col(), 0);
        if (changed) ld_palette();
    }
}

static void clock_trend(deck_data_t *d)
{
    static int32_t last = INT32_MIN;
    if (!d->price_ok) return;
    double chg = g_settings.currency ? d->chg24_eur : d->chg24;
    int32_t key = (int32_t)lround(chg * 100) * 2 + g_settings.currency;
    if (key == last) return;
    last = key;
    clock_trend_apply(chg);
}

#ifdef CONFIG_DECK_SNAPSHOT
// Testhilfe (Screenshot-Modus): Wechsel von hh:mm nach hh:mm+1 starten
static void ld_test(int hh, int mm)
{
    int m2 = (mm + 1) % 60, h2 = m2 ? hh : (hh + 1) % 24;
    char a[4] = {'0' + hh / 10, '0' + hh % 10, '0' + mm / 10, '0' + mm % 10};
    char b[4] = {'0' + h2 / 10, '0' + h2 % 10, '0' + m2 / 10, '0' + m2 % 10};
    for (int k = 0; k < 4; k++) {
        LD.seg_old[k] = ld_segs(a[k]);
        LD.seg_new[k] = ld_segs(b[k]);
    }
    LD.t0 = esp_timer_get_time() / 1000;
    time_t tt = time(NULL);
    struct tm tmv;
    localtime_r(&tt, &tmv);
    LD.shown = net_time_valid() ? tmv.tm_hour * 60 + tmv.tm_min : -1;
}
#endif

// ---- Wetter: aktuelles Wetter und die nächsten 5 Tage (Open-Meteo). Die Symbole sind Pixel-Mosaike
// aus 13 × 13 Zellen wie der Kursverlauf; Sonne und Blitz in Pink, sonst Graustufen. Groß bewegt sich
// das Symbol leicht: Regen und Schnee fallen, die Strahlen der Sonne glimmen, Nebel zieht, es blitzt.
#define WX_N 13     // Zellen je Seite
#define WX_BIG 8    // Zelle groß: 7 px und Fuge, wie das Mosaik
#define WX_SMALL 4  // Zelle klein (Vorhersage)
#define WX_FALL0 8  // ab dieser Reihe fallen Regen und Schnee
enum { WI_SUN, WI_MOON, WI_SUN_CLOUD, WI_MOON_CLOUD, WI_CLOUD, WI_FOG, WI_DRIZZLE, WI_RAIN, WI_SNOW, WI_THUNDER, WI_COUNT };
// S Sonne · r/R Strahl · M Mond · C/c Wolke (unten dunkler) · G/g Regenwolke · d Tropfen · * Flocke · B Blitz · F/f Nebel
static const char *const WX_ICON[WI_COUNT][WX_N] = {
    [WI_SUN] = {"......r......", "......r......", "..R.......R..", "...R.....R...", ".....SSS.....", "....SSSSS....", "rr..SSSSS..rr",
                "....SSSSS....", ".....SSS.....", "...R.....R...", "..R.......R..", "......r......", "......r......"},
    [WI_MOON] = {".............", ".....M.......", "...MMM.......", "..MMM........", "..MMM........", ".MMMM........", ".MMMM........",
                 ".MMMMM.......", "..MMMMM......", "..MMMMMMMMM..", "...MMMMMMM...", ".....MMM.....", "............."},
    [WI_SUN_CLOUD] = {"....r........", ".r.....r.....", "...SSS.......", "..SSSSS......", "r.SSSSS......", "..SSSCCCCC...", "...SCCCCCC...",
                      "...CCCCCCCCC.", "..CCCCCCCCCCC", "..CCCCCCCCCCC", ".CCCCCCCCCCC.", ".ccccccccccc.", "............."},
    [WI_MOON_CLOUD] = {"..........M..", ".........MM..", "........MM...", "........MM...", "........MMM.M", ".........MMMM", "....GGGGG....",
                       "...GGGGGG....", "..GGGGGGGGG..", ".GGGGGGGGGGG.", ".GGGGGGGGGGG.", "GGGGGGGGGGG..", "ggggggggggg.."},
    [WI_CLOUD] = {".............", ".............", ".............", ".............", ".....CCCCC...", "....CCCCCC...", "...CCCCCCCCC.",
                  "..CCCCCCCCCCC", "..CCCCCCCCCCC", ".CCCCCCCCCCC.", ".ccccccccccc.", ".............", "............."},
    [WI_FOG] = {".............", ".............", "..FFFFFFFFF..", ".............", "fffffffff....", ".............", "...FFFFFFFFFF",
                ".............", ".fffffffff...", ".............", "....FFFFFFFF.", ".............", "............."},
    [WI_DRIZZLE] = {".............", ".....GGGGG...", "....GGGGGG...", "...GGGGGGGGG.", "..GGGGGGGGGGG", "..GGGGGGGGGGG", ".GGGGGGGGGGG.",
                    ".ggggggggggg.", ".............", "...d.....d...", ".............", "......d......", ".d.........d."},
    [WI_RAIN] = {".............", ".....GGGGG...", "....GGGGGG...", "...GGGGGGGGG.", "..GGGGGGGGGGG", "..GGGGGGGGGGG", ".GGGGGGGGGGG.",
                 ".ggggggggggg.", ".............", "...d.....d...", "...d.....d...", "d.....d.....d", "d.....d.....d"},
    [WI_SNOW] = {".............", ".....CCCCC...", "....CCCCCC...", "...CCCCCCCCC.", "..CCCCCCCCCCC", "..CCCCCCCCCCC", ".CCCCCCCCCCC.",
                 ".ccccccccccc.", ".............", "...*........*", "..***....*...", "...*....***..", "*.....*..*..."},
    [WI_THUNDER] = {".............", ".....GGGGG...", "....GGGGGG...", "...GGGGGGGGG.", "..GGGGGGGGGGG", "..GGGGGGGGGGG", ".GGGGGGGGGGG.",
                    ".ggggggBBggg.", "......BB.....", "..d..BBBB....", "..d...BB...d.", ".....BB....d.", ".....B......."},
};
static canvas8_t WXC, WXD[5];
static struct {
    lv_obj_t *tile, *status, *dot, *temp, *cond, *val[4], *day[5], *dtemp[5], *drain[5];
    int icon; // großes Symbol, -1 = keine Daten
} WX = {.icon = -1};

static int wx_icon(int code, bool day)
{
    if (code <= 1) return day ? WI_SUN : WI_MOON;
    if (code == 2) return day ? WI_SUN_CLOUD : WI_MOON_CLOUD;
    if (code == 45 || code == 48) return WI_FOG;
    if (code >= 51 && code <= 57) return WI_DRIZZLE;
    if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82)) return WI_RAIN;
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return WI_SNOW;
    if (code >= 95) return WI_THUNDER;
    return WI_CLOUD;
}

static const char *wx_text(int code, bool day)
{
    switch (code) {
    case 0: return day ? "Sonnig" : "Klar";
    case 1: return day ? "Heiter" : "Überwiegend klar";
    case 2: return "Wolkig";
    case 3: return "Bedeckt";
    case 45: return "Nebel";
    case 48: return "Reifnebel";
    case 51: return "Leichter Niesel";
    case 53: return "Nieselregen";
    case 55: return "Starker Niesel";
    case 56: case 57: return "Gefrierender Niesel";
    case 61: return "Leichter Regen";
    case 63: return "Regen";
    case 65: return "Starker Regen";
    case 66: case 67: return "Gefrierender Regen";
    case 71: return "Leichter Schnee";
    case 73: return "Schneefall";
    case 75: return "Starker Schnee";
    case 77: return "Schneegriesel";
    case 80: return "Leichte Schauer";
    case 81: return "Regenschauer";
    case 82: return "Heftige Schauer";
    case 85: return "Schneeschauer";
    case 86: return "Starke Schneeschauer";
    case 95: return "Gewitter";
    case 96: case 99: return "Gewitter mit Hagel";
    }
    return "";
}

static void fmt_temp(char *o, size_t l, float v)
{
    int t = (int)lroundf(v);
    snprintf(o, l, "%s%d°", t < 0 ? "–" : "", abs(t));
}

// Symbol zeichnen; t = Zeit in ms für die Bewegung, -1 = ruhend
static void wx_draw(canvas8_t *c, int icon, int cell, int64_t t)
{
    memset(c->px, 0, c->w * c->h);
    for (int y = 0; y < WX_N; y++) // leere Zellen ganz schwach, wie im Mosaik
        for (int x = 0; x < WX_N; x++) fill8(c, x * cell, y * cell, cell - 1, cell - 1, (hash2(x + 500, y) % 7) == 0 ? 0x10 : 0x08);
    if (icon < 0) {
        lv_obj_invalidate(c->cv);
        return;
    }
    bool anim = t >= 0;
    int fall = anim ? (int)(t / (icon == WI_SNOW ? 420 : 140)) : 0;           // Regen schnell, Schnee langsam
    float tw = anim ? 0.5f + 0.5f * sinf((float)(t % 2400) / 2400 * 6.2832f) : 0.5f; // Strahlen glimmen im Wechsel
    int fog = anim ? (int)((t / 900) % 4) : 0;
    fog = fog == 1 ? 1 : fog == 3 ? -1 : 0;
    bool flash = anim && (t % 3200 < 90 || (t % 3200 > 220 && t % 3200 < 300)); // Doppelblitz
    for (int y = 0; y < WX_N; y++)
        for (int x = 0; x < WX_N; x++) {
            char ch = WX_ICON[icon][y][x];
            int xx = x, yy = y;
            uint8_t v;
            switch (ch) {
            case 'S': v = PX_PINK; break;
            case 'r': v = PX_PINK0 + 7 + (int)(6 * tw); break;
            case 'R': v = PX_PINK0 + 4 + (int)(6 * (1 - tw)); break;
            case 'B': v = flash ? PX_PINK : PX_PINK0 + 10; break;
            case 'M': v = 0xE0; break;
            case 'C': v = 0xC8; break;
            case 'c': v = 0x86; break;
            case 'G': v = 0x9A; break;
            case 'g': v = 0x5E; break;
            case 'd': case '*':
                v = ch == 'd' ? 0xB4 : 0xEC;
                yy = WX_FALL0 + (y - WX_FALL0 + fall) % (WX_N - WX_FALL0);
                break;
            case 'F': case 'f':
                v = ch == 'F' ? 0x8A : 0x50;
                xx = x + ((y / 2) & 1 ? -fog : fog);
                if (xx < 0 || xx >= WX_N) continue;
                break;
            default: continue;
            }
            fill8(c, xx * cell, yy * cell, cell - 1, cell - 1, v);
        }
    lv_obj_invalidate(c->cv);
}

static void wx_tick(lv_timer_t *tm)
{
    if (WX.icon < 0 || WX.icon == WI_CLOUD || WX.icon == WI_MOON || WX.icon == WI_MOON_CLOUD) return;
    bool vis = s_card[s_cur] == WX.tile || (s_nb >= 0 && s_card[s_nb] == WX.tile);
    if (vis && !s_booting) wx_draw(&WXC, WX.icon, WX_BIG, esp_timer_get_time() / 1000);
}

static void build_weather(lv_obj_t *t)
{
    WX.tile = t;
    cap(t, W, "WETTER", 'l', MX, 30);
    WX.status = txt(t, W, &font_m14, C_TEXT2, "", 'r', W - MX, 30);
    WX.dot = rect(t, 0, 20, 7, 7, C_TEXT3);
    lv_obj_set_style_radius(WX.dot, LV_RADIUS_CIRCLE, 0);
    breathe(WX.dot);
    canvas8_create(&WXC, t, MX, 46, WX_N * WX_BIG, WX_N * WX_BIG);
    wx_draw(&WXC, -1, WX_BIG, -1);
    WX.temp = tight(txt(t, W, &font_g96, C_TEXT, "–", 'l', 140, 120), 4);
    WX.cond = txt(t, W, &font_m14, C_TEXT2, "", 'l', 144, 148);
    static const char *sn[4] = {"HEUTE", "GEFÜHLT", "WIND", "REGEN"};
    for (int i = 0; i < 4; i++) {
        cap(t, W, sn[i], 'l', 336, 66 + i * 28);
        WX.val[i] = txt(t, W, &font_g20, C_TEXT, "–", 'r', W - MX, 66 + i * 28);
        lv_label_set_recolor(WX.val[i], true);
    }
    for (int i = 0; i < 5; i++) {
        int w = 84;
        lv_obj_t *p = panel(t, MX + i * 89, 168, w, 136);
        WX.day[i] = cap(p, w, "", 'c', w / 2, 22);
        canvas8_create(&WXD[i], p, (w - WX_N * WX_SMALL) / 2, 32, WX_N * WX_SMALL, WX_N * WX_SMALL);
        wx_draw(&WXD[i], -1, WX_SMALL, -1);
        WX.dtemp[i] = txt(p, w, &font_g20, C_TEXT, "", 'c', w / 2, 108);
        lv_label_set_recolor(WX.dtemp[i], true);
        WX.drain[i] = txt(p, w, &font_m12, C_TEXT3, "", 'c', w / 2, 126);
    }
    lv_timer_create(wx_tick, 100, NULL);
}

// Ort, Punkt (pink = aktuell, grau = keine oder alte Daten) und Fehler
static void wx_status(const weather_t *w)
{
    bool fresh = w->ok && time(NULL) - w->updated < 3600;
    lv_label_set_text(WX.status, w->place[0] ? w->place : g_settings.wx_place[0] ? "" : "KEIN ORT");
    lv_obj_set_style_bg_color(WX.dot, fresh ? C_PINK : w->ok ? C_TEXT3 : C_DOWN, 0);
    lv_obj_update_layout(WX.status);
    lv_obj_set_x(WX.dot, lv_obj_get_x(WX.status) - 14);
}

static uint32_t v_wx;
static void update_weather(deck_data_t *d)
{
    static int n = 0;
    const weather_t *w = &d->wx;
    if (++n % 20 == 0) wx_status(w); // alle 10 s: Daten zu alt?
    if (w->ver == v_wx) return;
    v_wx = w->ver;
    wx_status(w);
    if (!w->ok) {
        lv_label_set_text(WX.cond, w->err);
        lv_obj_set_style_text_color(WX.cond, C_PINK, 0);
        return;
    }
    static const char *const wd[7] = {"SO", "MO", "DI", "MI", "DO", "FR", "SA"};
    char b[48], a1[16], a2[16];
    fmt_temp(b, sizeof(b), w->temp);
    const lv_font_t *f = lroundf(w->temp) <= -10 ? &font_g72 : &font_g96; // „–12°“ passt nur kleiner neben die Werte
    if (lv_obj_get_style_text_font(WX.temp, 0) != f) {
        lv_obj_set_style_text_font(WX.temp, f, 0);
        lv_obj_set_y(WX.temp, 120 - ascent(f));
    }
    lv_label_set_text(WX.temp, b);
    lv_label_set_text(WX.cond, w->err[0] ? w->err : wx_text(w->code, w->is_day));
    lv_obj_set_style_text_color(WX.cond, w->err[0] ? C_PINK : C_TEXT2, 0);
    fmt_temp(a1, sizeof(a1), w->d_max[0]);
    fmt_temp(a2, sizeof(a2), w->d_min[0]);
    snprintf(b, sizeof(b), "%s #9A9A9A / %s#", a1, a2);
    lv_label_set_text(WX.val[0], b);
    fmt_temp(b, sizeof(b), w->feels);
    lv_label_set_text(WX.val[1], b);
    snprintf(b, sizeof(b), "%d #9A9A9A km/h#", (int)lroundf(w->wind));
    lv_label_set_text(WX.val[2], b);
    if (w->d_rain[0] >= 0) snprintf(b, sizeof(b), "%d #9A9A9A %%#", w->d_rain[0]);
    else strlcpy(b, "–", sizeof(b));
    lv_label_set_text(WX.val[3], b);
    int icon = wx_icon(w->code, w->is_day);
    if (icon != WX.icon) {
        WX.icon = icon;
        wx_draw(&WXC, icon, WX_BIG, -1);
    }
    for (int i = 0; i < 5; i++) {
        int k = i + 1;
        bool ok = k < w->days;
        lv_label_set_text(WX.day[i], ok && w->d_wday[k] >= 0 ? wd[w->d_wday[k]] : "");
        wx_draw(&WXD[i], ok ? wx_icon(w->d_code[k], true) : -1, WX_SMALL, -1);
        if (!ok) {
            lv_label_set_text(WX.dtemp[i], "");
            lv_label_set_text(WX.drain[i], "");
            continue;
        }
        fmt_temp(a1, sizeof(a1), w->d_max[k]);
        fmt_temp(a2, sizeof(a2), w->d_min[k]);
        snprintf(b, sizeof(b), "%s #5E5E5E %s#", a1, a2);
        lv_label_set_text(WX.dtemp[i], b);
        if (w->d_rain[k] >= 0) snprintf(b, sizeof(b), "\xEF\x81\x83 %d %%", w->d_rain[k]); // Tropfen, Regenwahrscheinlichkeit
        else b[0] = 0;
        lv_label_set_text(WX.drain[i], b);
        lv_obj_set_style_text_color(WX.drain[i], w->d_rain[k] >= 50 ? C_TEXT2 : C_TEXT3, 0);
    }
}

// ---- News: die vier neuesten Bitcoin-Schlagzeilen (Crypto Vision oder RSS-Feed). Zu jeder Meldung steht rechts,
// wie sich der Kurs seitdem bewegt hat – Prozent und Mini-Kurve aus Punkten, hellgrün gestiegen, pink gefallen –
// und darüber ihr Alter in Blöcken statt Minuten.
#define NW_ROWS NEWS_N
#define NW_Y0 42        // erste Zeile
#define NW_RH 67        // Zeilenhöhe
#define NW_TW 344       // Breite der Schlagzeile
#define NW_CX 376       // Mini-Kurven: x
#define NW_COLS 28      // Spalten der Mini-Kurve (3 px Abstand)
#define NW_LEVELS 6     // Punkte übereinander
#define NW_PINK0 240    // Palette der Mini-Kurven: 8 Stufen Pink, danach 8 Stufen Grün
#define NW_GREEN0 248
static canvas8_t NWC;
static struct {
    lv_obj_t *tile, *meta[NW_ROWS], *title[NW_ROWS], *chg[NW_ROWS], *msg;
} NW;

// Kurs (USD) zum Zeitpunkt ts aus der Kursreihe (7,5 Tage); nach deren letztem Punkt bis zum aktuellen Kurs
// interpoliert. -1 = zu alt oder keine Daten
static double px_at(const deck_data_t *d, int64_t ts, int64_t now, double live)
{
    const px_series_t *s = d->px;
    if (!s || s->n < 2 || ts < s->t[0]) return -1;
    int n = s->n;
    if (ts >= s->t[n - 1]) {
        if (now <= s->t[n - 1]) return s->v[n - 1];
        double f = (double)(ts - s->t[n - 1]) / (now - s->t[n - 1]);
        return s->v[n - 1] + (live - s->v[n - 1]) * (f > 1 ? 1 : f);
    }
    int lo = 0, hi = n - 1; // t[lo] <= ts < t[hi]
    while (hi - lo > 1) {
        int m = (lo + hi) / 2;
        if (s->t[m] <= ts) lo = m;
        else hi = m;
    }
    return s->v[lo] + (s->v[hi] - s->v[lo]) * (double)(ts - s->t[lo]) / (s->t[hi] - s->t[lo]);
}

// Blöcke seit ts: gezählt an den bekannten Blöcken, davor mit 10 Minuten je Block geschätzt
static int blocks_since(const deck_data_t *d, int64_t ts)
{
    if (!d->blocks_n || !ts) return -1;
    int n = 0;
    int64_t oldest = INT64_MAX;
    for (int i = 0; i < d->blocks_n; i++) {
        if (d->blocks[i].timestamp > ts) n++;
        if (d->blocks[i].timestamp < oldest) oldest = d->blocks[i].timestamp;
    }
    if (n == d->blocks_n && oldest > ts) n += (int)((oldest - ts) / 600);
    return n;
}

// Mini-Kurve von der Meldung bis jetzt: Säulen aus Punkten, oben hell, die letzte am hellsten
static void nw_spark(int r, const deck_data_t *d, int64_t ts, int64_t now, double live, int dir)
{
    double v[NW_COLS], mn = 1e18, mx = -1e18;
    for (int k = 0; k < NW_COLS; k++) {
        v[k] = px_at(d, ts + (now - ts) * k / (NW_COLS - 1), now, live);
        if (v[k] < 0) return;
        if (v[k] < mn) mn = v[k];
        if (v[k] > mx) mx = v[k];
    }
    int base = dir > 0 ? NW_GREEN0 : NW_PINK0, y0 = r * NW_RH + 45;
    for (int k = 0; k < NW_COLS; k++) {
        int lv = mx - mn > 1e-6 ? (int)((v[k] - mn) / (mx - mn) * (NW_LEVELS - 1) + 0.5) : NW_LEVELS / 2;
        for (int q = 0; q <= lv; q++)
            fill8(&NWC, k * 3, y0 + (NW_LEVELS - 1 - q) * 3, 2, 2, q < lv ? base + 2 : k == NW_COLS - 1 ? base + 7 : base + 6);
    }
}

static void news_render(const deck_data_t *d)
{
    const news_t *nw = &d->news;
    memset(NWC.px, 0, NWC.w * NWC.h);
    lv_label_set_text(NW.msg, nw->n ? "" : nw->err[0] ? nw->err : "Lade Schlagzeilen …");
    int64_t now = time(NULL);
    double live = d->price_ok ? d->usd : -1;
    time_t tt = now;
    struct tm today, tm;
    localtime_r(&tt, &today);
    for (int r = 0; r < NW_ROWS; r++) {
        if (r >= nw->n) {
            lv_label_set_text(NW.meta[r], "");
            lv_label_set_text(NW.title[r], "");
            lv_label_set_text(NW.chg[r], "");
            continue;
        }
        const news_item_t *it = &nw->item[r];
        // Alter in Blöcken · Uhrzeit (heute) bzw. Datum · Quelle
        char age[40] = "", when[16] = "", src[32], b[160];
        int bl = net_time_valid() ? blocks_since(d, it->ts) : -1;
        if (bl == 0) strlcpy(age, "#FFD4D4 NEU#", sizeof(age));
        else if (bl == 1) strlcpy(age, "VOR 1 BLOCK", sizeof(age));
        else if (bl > 1) {
            char n[16];
            fmt_de(n, sizeof(n), bl, 0);
            snprintf(age, sizeof(age), "VOR %s BLÖCKEN", n);
        }
        if (it->ts) {
            tt = it->ts;
            localtime_r(&tt, &tm);
            strftime(when, sizeof(when), tm.tm_yday == today.tm_yday && tm.tm_year == today.tm_year ? "%H:%M" : "%d.%m.", &tm);
        }
        int i = 0;
        for (; it->source[i] && i < (int)sizeof(src) - 1; i++) src[i] = toupper((unsigned char)it->source[i]);
        src[i] = 0;
        snprintf(b, sizeof(b), "%s%s%s%s%s", age, age[0] && when[0] ? " · " : "", when, src[0] && (age[0] || when[0]) ? " · " : "", src);
        lv_label_set_text(NW.meta[r], b);
        lv_label_set_text(NW.title[r], it->title);
        // Kurs seit der Meldung
        double p0 = it->ts && live > 0 ? px_at(d, it->ts, now, live) : -1;
        if (p0 > 0) {
            double pct = (live / p0 - 1) * 100;
            if (fabs(pct) < 0.005) strlcpy(b, "0,00 %", sizeof(b)); // kein „–0,00 %“
            else fmt_change(b, sizeof(b), pct);
            lv_label_set_text(NW.chg[r], b);
            lv_obj_set_style_text_color(NW.chg[r], pct >= 0.005 ? C_UP : pct <= -0.005 ? C_PINK : C_TEXT2, 0);
            nw_spark(r, d, it->ts, now, live, pct >= 0 ? 1 : -1);
        } else {
            lv_label_set_text(NW.chg[r], "–");
            lv_obj_set_style_text_color(NW.chg[r], C_TEXT3, 0);
        }
    }
    lv_obj_invalidate(NWC.cv);
}

static void build_news(lv_obj_t *t)
{
    NW.tile = t;
    cap(t, W, "BITCOIN NEWS", 'l', MX, 30);
    lv_obj_set_style_text_color(cap(t, W, "KURS SEIT MELDUNG", 'r', W - MX, 30), C_TEXT3, 0);
    canvas8_create(&NWC, t, NW_CX, NW_Y0, W - MX - NW_CX, NW_ROWS * NW_RH);
    for (int i = 0; i < 8; i++) {
        uint8_t a = 40 + i * 215 / 7;
        lv_canvas_set_palette(NWC.cv, NW_PINK0 + i, lv_color_mix(C_PINK, C_BG, a));
        lv_canvas_set_palette(NWC.cv, NW_GREEN0 + i, lv_color_mix(C_UP, C_BG, a));
    }
    for (int r = 0; r < NW_ROWS; r++) {
        int top = NW_Y0 + r * NW_RH;
        NW.meta[r] = cap(t, W, "", 'l', MX, top + 15);
        lv_obj_set_style_text_color(NW.meta[r], C_TEXT3, 0);
        lv_label_set_recolor(NW.meta[r], true);
        lv_label_set_long_mode(NW.meta[r], LV_LABEL_LONG_CLIP);
        lv_obj_set_width(NW.meta[r], W - 2 * MX);
        NW.title[r] = txt(t, W, &font_g20, C_TEXT, "", 'l', MX, top + 39);
        lv_obj_set_style_text_line_space(NW.title[r], -3, 0);
        lv_label_set_long_mode(NW.title[r], LV_LABEL_LONG_DOT);
        lv_obj_set_size(NW.title[r], NW_TW, 2 * font_g20.line_height - 3);
        NW.chg[r] = txt(t, W, &font_g20, C_TEXT3, "", 'r', W - MX, top + 39);
    }
    NW.msg = txt(t, W, &font_m14, C_TEXT2, "Lade Schlagzeilen …", 'c', W / 2, 170);
}

static uint32_t v_news, v_news_px;
static void update_news(deck_data_t *d)
{
    static int64_t t_render;
    int64_t ms = esp_timer_get_time() / 1000;
    bool vis = s_card[s_cur] == NW.tile || (s_nb >= 0 && s_card[s_nb] == NW.tile);
    if (d->news.ver != v_news || d->px_ver != v_news_px || (vis && ms - t_render > 30000)) {
        news_render(d); // neue Daten oder alle 30 s: Alter und Kurs nachziehen
        t_render = ms;
    }
    v_news = d->news.ver;
    v_news_px = d->px_ver;
}

// ---- 7 Bitaxe
static void build_miner(lv_obj_t *t)
{
    HB.title = cap(t, W, "BITAXE", 'l', MX, 30);
    HB.status = txt(t, W, &font_m14, C_TEXT2, "", 'r', W - MX, 30);
    HB.dot = rect(t, 0, 20, 7, 7, C_PINK);
    lv_obj_set_style_radius(HB.dot, LV_RADIUS_CIRCLE, 0);
    breathe(HB.dot);
    HB.hash = tight(txt(t, W, &font_g96, C_TEXT, "–", 'l', MX - 5, 132), 4);
    HB.unit = txt(t, W, &font_m20, C_TEXT2, "TH/s", 'l', 0, 132);
    cell_row(t, HB.cells, 152);
    HB.legend = txt(t, W, &font_m12, C_TEXT2, "", 'l', MX, 182);
    lv_label_set_recolor(HB.legend, true);
    static const char *tn[3] = {"ASIC", "VR-TEMP.", "LÜFTER"};
    for (int i = 0; i < 3; i++) {
        int x = MX + i * 148, w = 144;
        lv_obj_t *p = panel(t, x, 200, w, 100);
        cap(p, w, tn[i], 'l', 14, 26);
        HB.val[i] = tight(txt(p, w, &font_g36, C_TEXT, "–", 'l', 12, 76), 1);
        txt(p, w, &font_m14, C_TEXT2, i == 2 ? "%" : "°C", 'r', w - 14, 76);
    }
}

// ---- 10 Mining-Kosten: aktueller Verbrauch, hochgerechnete Kosten, gezählte kWh
static struct {
    lv_obj_t *status, *dot, *watt, *unit, *price, *ticket, *cost[3], *kwh[3], *e_title[2], *e_val[2], *e_cost[2];
    double power, btc;       // Leistung (W), Kurs in Euro (Strom wird in Euro bezahlt)
    double e_wh, e_day_wh;   // gezählter Verbrauch gesamt / heute
    int64_t since;
    bool have;
} KS;

static void cost_render(void)
{
    char b[64], n[24];
    double ct = g_settings.kwh_price * 100.0;
    fmt_de(n, sizeof(n), ct, fabs(ct - round(ct)) < 0.05 ? 0 : 1);
    snprintf(b, sizeof(b), "%s ct/kWh", n);
    lv_label_set_text(KS.price, b);
    if (!KS.have) return;
    double p = KS.power > 0 ? KS.power : 0;
    fmt_de(n, sizeof(n), p, 1);
    lv_label_set_text(KS.watt, n);
    lv_obj_update_layout(KS.watt);
    lv_obj_set_x(KS.unit, lv_obj_get_x(KS.watt) + lv_obj_get_width(KS.watt) + 10);
    // Hochrechnung aus der aktuellen Leistung
    static const double days[3] = {1, 365.0 / 12, 365};
    double kwh_day = p * 24 / 1000;
    for (int i = 0; i < 3; i++) {
        double kwh = kwh_day * days[i], cost = kwh * g_settings.kwh_price;
        fmt_de(n, sizeof(n), cost, cost >= 100 ? 0 : 2);
        lv_label_set_text(KS.cost[i], n);
        fmt_de(n, sizeof(n), kwh, kwh >= 100 ? 0 : kwh >= 10 ? 1 : 2);
        snprintf(b, sizeof(b), "%s kWh", n);
        lv_label_set_text(KS.kwh[i], b);
    }
    // Kosten eines Tages in sats: der Preis des Lotterieloses
    if (KS.btc > 0) {
        fmt_de(n, sizeof(n), kwh_day * g_settings.kwh_price / KS.btc * 1e8, 0);
        snprintf(b, sizeof(b), "ca. #FFD4D4 %s sat# pro Tag", n);
    } else snprintf(b, sizeof(b), "ca. – sat pro Tag");
    lv_label_set_text(KS.ticket, b);
    // gezählter Verbrauch
    double wh[2] = {KS.e_day_wh, KS.e_wh};
    for (int i = 0; i < 2; i++) {
        double kwh = wh[i] / 1000;
        fmt_de(n, sizeof(n), kwh, kwh >= 100 ? 0 : kwh >= 10 ? 1 : 2);
        snprintf(b, sizeof(b), "%s kWh", n);
        lv_label_set_text(KS.e_val[i], b);
        fmt_de(n, sizeof(n), kwh * g_settings.kwh_price, 2);
        snprintf(b, sizeof(b), "%s €", n);
        lv_label_set_text(KS.e_cost[i], b);
    }
    if (KS.since > 0) {
        time_t ts = (time_t)KS.since;
        struct tm tm;
        localtime_r(&ts, &tm);
        strftime(n, sizeof(n), "%d.%m.%y", &tm);
        snprintf(b, sizeof(b), "GESAMT SEIT %s", n);
    } else snprintf(b, sizeof(b), "GESAMT");
    lv_label_set_text(KS.e_title[1], b);
}

static void build_cost(lv_obj_t *t)
{
    cap(t, W, "MINING-KOSTEN", 'l', MX, 30);
    KS.status = txt(t, W, &font_m14, C_TEXT2, "", 'r', W - MX, 30);
    KS.dot = rect(t, 0, 20, 7, 7, C_PINK);
    lv_obj_set_style_radius(KS.dot, LV_RADIUS_CIRCLE, 0);
    breathe(KS.dot);
    KS.watt = tight(txt(t, W, &font_g96, C_TEXT, "–", 'l', MX - 5, 132), 4);
    KS.unit = txt(t, W, &font_m20, C_TEXT2, "W", 'l', 0, 132);
    // Strompreis (einstellbar auf der Browser-Seite)
    cap(t, W, "STROMPREIS", 'r', W - MX, 66);
    KS.price = tight(txt(t, W, &font_g26, C_TEXT, "", 'r', W - MX, 100), 1);
    KS.ticket = txt(t, W, &font_m14, C_TEXT2, "", 'r', W - MX, 132);
    lv_label_set_recolor(KS.ticket, true);
    static const char *tn[3] = {"PRO TAG", "PRO MONAT", "PRO JAHR"};
    for (int i = 0; i < 3; i++) {
        int x = MX + i * 148, w = 144;
        lv_obj_t *p = panel(t, x, 148, w, 88);
        cap(p, w, tn[i], 'l', 14, 22);
        KS.cost[i] = tight(txt(p, w, &font_g36, C_TEXT, "–", 'l', 12, 58), 1);
        txt(p, w, &font_m14, C_TEXT2, "€", 'r', w - 14, 58);
        KS.kwh[i] = txt(p, w, &font_m12, C_TEXT3, "", 'l', 14, 76);
    }
    for (int i = 0; i < 2; i++) {
        int x = MX + i * 222, w = 218;
        lv_obj_t *p = panel(t, x, 244, w, 60);
        KS.e_title[i] = cap(p, w, i ? "GESAMT" : "VERBRAUCHT HEUTE", 'l', 14, 22);
        KS.e_val[i] = tight(txt(p, w, &font_g26, C_TEXT, "–", 'l', 12, 48), 1);
        KS.e_cost[i] = txt(p, w, &font_m14, C_TEXT2, "", 'r', w - 14, 48);
    }
    cost_render();
}

// ---- 5 Halving: Sand über den ganzen Bildschirm. Jede Zelle steht für rund 88 Blöcke der Epoche
// (210.000 Blöcke); unten liegen die schon geschürften, der freie Raum darüber sind die restlichen.
// Körner rieseln herab, bei jedem neuen Block fällt ein pinkes.
#define HG_CELL 8
#define HG_COLS (W / HG_CELL)
#define HG_ROWS (H / HG_CELL)
#define HG_GRAINS 4
#define HG_FREE 5                        // oberste Reihen bleiben frei (Überschrift)
static canvas8_t HGC;
static struct {
    lv_obj_t *head, *num, *sub, *reward, *pct, *grain[HG_GRAINS], *drop;
    uint8_t colh[HG_COLS];               // Sandhöhe je Spalte (Zellen)
    int pink_col, drop_col;              // neuestes Korn / Ziel des fallenden pinken Korns
    uint32_t h;
} HG;

static void hg_y(void *o, int32_t v) { lv_obj_set_y(o, v); }
static int hg_surface(int col) { return H - (HG.colh[col] + 1) * HG_CELL; } // y der Zelle über dem Sand

// Zellen hinter einer Beschriftung bleiben leer – ausgespart im Raster statt schwarzer Kästen
static bool hg_masked(int q, int r)
{
    lv_obj_t *ls[5] = {HG.head, HG.reward, HG.num, HG.sub, HG.pct};
    int x = q * HG_CELL, y = r * HG_CELL;
    for (int i = 0; i < 5; i++) {
        if (!lv_label_get_text(ls[i])[0]) continue;
        int lx = lv_obj_get_x(ls[i]) - 5, ly = lv_obj_get_y(ls[i]) - 3;
        int lw = lv_obj_get_width(ls[i]) + 10, lh = lv_obj_get_height(ls[i]) + 4;
        if (x + HG_CELL > lx && x < lx + lw && y + HG_CELL > ly && y < ly + lh) return true;
    }
    return false;
}

static void hg_draw(void)
{
    canvas8_t *c = &HGC;
    memset(c->px, 0, c->w * c->h);
    lv_obj_update_layout(lv_obj_get_parent(HG.num));
    for (int q = 0; q < HG_COLS; q++)
        for (int r = 0; r < HG_ROWS; r++) {
            int fb = HG_ROWS - 1 - r; // von unten gezählt
            uint32_t hh = hash2(q + 300, r);
            uint8_t cl;
            if (hg_masked(q, r)) cl = (hh % 7) == 0 ? 0x0C : 0x05;
            else if (fb == HG.colh[q] - 1 && q == HG.pink_col) cl = PX_PINK; // neuestes Korn
            else if (fb < HG.colh[q]) cl = g8(0x1C + (int)(0x2A * (float)fb / HG_ROWS) + (int)(hh % 0x14)); // Sand
            else cl = (hh % 7) == 0 ? 0x10 : 0x08; // freier Raum
            fill8(c, q * HG_CELL, r * HG_CELL, HG_CELL - 1, HG_CELL - 1, cl);
        }
    lv_obj_invalidate(c->cv);
}

// Sandhöhen aus dem Fortschritt der Epoche: volle Reihen, die oberste von links nach rechts gefüllt –
// eine gerade Kante mit einer Stufe; das neueste Korn liegt an der Stufe
static void hg_heights(uint32_t height)
{
    int total = HG_COLS * (HG_ROWS - HG_FREE), fill = (int)((double)total * (height % 210000) / 210000 + 0.5);
    int full = fill / HG_COLS, rem = fill % HG_COLS;
    for (int q = 0; q < HG_COLS; q++) HG.colh[q] = full + (q < rem ? 1 : 0);
    HG.pink_col = rem > 0 ? rem - 1 : HG_COLS - 1;
    HG.drop_col = rem < HG_COLS ? rem : 0; // hier landet das nächste Korn
}

static void hg_grain_go(lv_obj_t *g, int delay);
static void hg_grain_ready(lv_anim_t *a) { hg_grain_go(a->var, rand() % 700); }

// ein Korn fällt in einer zufälligen Spalte bis auf den Sand, danach das nächste
static void hg_grain_go(lv_obj_t *g, int delay)
{
    int q = rand() % HG_COLS, y1 = hg_surface(q);
    if (y1 < 0) y1 = 0;
    lv_obj_set_pos(g, q * HG_CELL, -HG_CELL);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, g);
    lv_anim_set_exec_cb(&a, hg_y);
    lv_anim_set_values(&a, -HG_CELL, y1);
    lv_anim_set_time(&a, 500 + y1 * 3);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in); // schneller werdend
    lv_anim_set_ready_cb(&a, hg_grain_ready);
    lv_anim_start(&a);
}

static void hg_drop_ready(lv_anim_t *a)
{
    lv_obj_add_flag(HG.drop, LV_OBJ_FLAG_HIDDEN);
    hg_draw();
}

// neuer Block: ein pinkes Korn fällt und bleibt als neuestes oben auf dem Sand liegen
static void hg_drop(void)
{
    int q = HG.drop_col;
    lv_obj_set_pos(HG.drop, q * HG_CELL, -HG_CELL);
    lv_obj_clear_flag(HG.drop, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, HG.drop);
    lv_anim_set_exec_cb(&a, hg_y);
    lv_anim_set_values(&a, -HG_CELL, hg_surface(q) > 0 ? hg_surface(q) : 0);
    lv_anim_set_time(&a, 1300);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&a, hg_drop_ready);
    lv_anim_start(&a);
}

static void build_halving(lv_obj_t *t)
{
    canvas8_create(&HGC, t, 0, 0, W, H);
    for (int i = 0; i < HG_GRAINS; i++) {
        HG.grain[i] = rect(t, 0, -HG_CELL, HG_CELL - 1, HG_CELL - 1, gray(0xA8));
        lv_obj_clear_flag(HG.grain[i], LV_OBJ_FLAG_CLICKABLE); // Tippen geht an die Karte
    }
    HG.drop = rect(t, 0, -HG_CELL, HG_CELL - 1, HG_CELL - 1, C_PINK);
    lv_obj_clear_flag(HG.drop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(HG.drop, LV_OBJ_FLAG_HIDDEN);
    HG.head = cap(t, W, "BLÖCKE BIS ZUM HALVING", 'l', MX, 30);
    HG.reward = txt(t, W, &font_m14, C_TEXT2, "", 'r', W - MX, 30);
    HG.num = tight(txt(t, W, &font_g96, C_TEXT, "–", 'l', MX - 5, 120), 4);
    HG.sub = txt(t, W, &font_m14, C_TEXT2, "", 'l', MX, 146);
    HG.pct = txt(t, W, &font_m14, C_PINK, "", 'r', W - MX, 300);
    for (int i = 0; i < HG_GRAINS; i++) hg_grain_go(HG.grain[i], i * 450);
}

static void update_halving(deck_data_t *d)
{
    if (!d->net_ok || !d->height || d->height == HG.h) return;
    HG.h = d->height;
    uint32_t h = d->height, next = (h / 210000 + 1) * 210000, left = next - h;
    char b[96], n[24], n2[24], when[32] = "";
    snprintf(b, sizeof(b), "BLÖCKE BIS ZUM HALVING #%u", (unsigned)(next / 210000));
    lv_label_set_text(HG.head, b);
    fmt_de(n, sizeof(n), left, 0);
    lv_label_set_text(HG.num, n);
    double bt = d->block_time_ms > 60000 ? d->block_time_ms / 1000.0 : 600; // Sekunden je Block
    double secs = left * bt;
    if (net_time_valid()) {
        static const char *mon[12] = {"Januar", "Februar", "März", "April", "Mai", "Juni", "Juli", "August", "September", "Oktober", "November", "Dezember"};
        time_t t = time(NULL) + (time_t)secs;
        struct tm tm;
        localtime_r(&t, &tm);
        snprintf(when, sizeof(when), " · ca. %s %d", mon[tm.tm_mon], tm.tm_year + 1900);
    }
    fmt_de(n, sizeof(n), next, 0);
    fmt_de(n2, sizeof(n2), secs / 86400, 0);
    snprintf(b, sizeof(b), "bis Block %s%s · in %s Tagen", n, when, n2);
    lv_label_set_text(HG.sub, b);
    double sub = 50.0 / pow(2, h / 210000);
    fmt_de(n, sizeof(n), sub, 4);
    fmt_de(n2, sizeof(n2), sub / 2, 4);
    snprintf(b, sizeof(b), "%s → %s BTC", n, n2);
    lv_label_set_text(HG.reward, b);
    fmt_de(n, sizeof(n), 100.0 * (h % 210000) / 210000, 1);
    snprintf(b, sizeof(b), "%s %% der Epoche geschürft", n);
    lv_label_set_text(HG.pct, b);
    hg_heights(h);
    // Beschriftungen so legen, dass die Kante des Sands frei bleibt
    int asc14 = font_m14.line_height - font_m14.base_line;
    int full = HG.colh[HG_COLS - 1], top_full = H - full * HG_CELL;   // Oberkante der vollen Reihen
    int top_part = H - HG.colh[0] * HG_CELL;                          // Oberkante der angefangenen Reihe
    int sub_base = 146;                                               // unter der großen Zahl …
    if (sub_base + 10 > top_part) sub_base = H - 14;                  // … oder ganz unten im Sand
    lv_obj_align(HG.sub, LV_ALIGN_TOP_LEFT, MX, sub_base - asc14);
    int pct_base = top_full + 26;                                     // knapp unter der Kante im Sand
    if (pct_base > H - 34) pct_base = top_part - 12;                  // wenig Sand: darüber
    lv_obj_align(HG.pct, LV_ALIGN_TOP_RIGHT, -MX, pct_base - asc14);
    hg_draw();
}

// ---- 11 Share-Lotterie: jeder gefundene Hash fällt als Punkt in ein Histogramm (log. Skala).
// Links häufige kleine Shares, rechts seltene große, ganz rechts in Pink: der Block.
#define LT_X MX
#define LT_Y 118
#define LT_W 440
#define LT_H 136
#define LT_LMIN 2.0 // 10^2
static canvas_t LC;
static struct {
    lv_obj_t *tile, *status, *dot, *last, *best, *rate, *net, *foot, *axis[5], *drop;
    uint32_t ver;
    double lmax;
} L;

static int lot_col(double lg)
{
    int cols = LT_W / DOT;
    int c = (int)((lg - LT_LMIN) / (L.lmax - LT_LMIN) * (cols - 1) + 0.5);
    return c < 0 ? 0 : c >= cols ? cols - 1 : c;
}

static void lottery_draw(const share_stats_t *st)
{
    canvas_t *c = &LC;
    int cols = LT_W / DOT, rows = LT_H / DOT;
    static uint32_t colcnt[LT_W / DOT];
    memset(colcnt, 0, sizeof(colcnt));
    uint32_t mx = 1;
    for (int b = 0; b < SH_BUCKETS; b++) {
        if (!st->hist[b]) continue;
        int col = lot_col(shares_bucket_log(b));
        colcnt[col] += st->hist[b];
        if (colcnt[col] > mx) mx = colcnt[col];
    }
    int best_col = st->best > 0 ? lot_col(log10(st->best)) : -1;
    int last_col = st->last > 0 ? lot_col(log10(st->last)) : -1;
    float lmx = log10f(mx + 1.0f);
    lv_canvas_fill_bg(c->cv, C_BG, LV_OPA_COVER);
    for (int col = 0; col < cols; col++) {
        int h = colcnt[col] ? 1 + (int)(log10f(colcnt[col] + 1.0f) / lmx * (rows - 2) + 0.5f) : 0;
        for (int r = 0; r < rows; r++) {
            int fb = rows - 1 - r, x = col * DOT + 1, y = r * DOT + 1;
            if (fb < h) {
                lv_color_t cl = fb == h - 1 ? (col == best_col ? C_PINK : col == last_col ? C_TEXT : gray(0x9A)) : gray(0x3A - fb * 0x18 / rows);
                fill_box(c, x, y, 2, 2, cl);
            } else if (fb == 0) {
                fill_box(c, x, y, 2, 2, gray(0x1C)); // Grundlinie
            }
        }
    }
    // Abstand vom besten Share bis zum Block: gepunktete pinke Linie; Block-Marke ganz rechts
    if (best_col >= 0)
        for (int col = best_col + 2; col < cols - 1; col += 2) fill_box(c, col * DOT + 1, (rows - 1) * DOT + 1, 2, 2, C_PINK_DIM);
    for (int r = 0; r < rows; r += 2) fill_box(c, (cols - 1) * DOT + 1, r * DOT + 1, 2, 2, C_PINK);
    lv_obj_invalidate(c->cv);
}

static void drop_exec(void *var, int32_t v) { lv_obj_set_y(var, v); }
static void drop_ready(lv_anim_t *a)
{
    lv_obj_add_flag(L.drop, LV_OBJ_FLAG_HIDDEN);
    shares_lock();
    lottery_draw(shares_get());
    shares_unlock();
}

static void build_lottery(lv_obj_t *t)
{
    L.tile = t;
    L.lmax = 14;
    cap(t, W, "SHARE-LOTTERIE", 'l', MX, 30);
    L.status = txt(t, W, &font_m14, C_TEXT2, "", 'r', W - MX, 30);
    L.dot = rect(t, 0, 20, 7, 7, C_PINK);
    lv_obj_set_style_radius(L.dot, LV_RADIUS_CIRCLE, 0);
    breathe(L.dot);
    static const char *cn[3] = {"LETZTER HASH", "BESTER SEIT START", "PRO MINUTE"};
    lv_obj_t **vals[3] = {&L.last, &L.best, &L.rate};
    for (int i = 0; i < 3; i++) {
        int x = MX + i * 148;
        cap(t, W, cn[i], 'l', x, 62);
        *vals[i] = tight(txt(t, W, &font_g36, i == 1 ? C_PINK : C_TEXT, "–", 'l', x - 1, 100), 1);
    }
    canvas_create(&LC, t, LT_X, LT_Y, LT_W, LT_H);
    static const char *ax[5] = {"1K", "1M", "1G", "1T", "1P"};
    for (int i = 0; i < 5; i++) L.axis[i] = txt(t, W, &font_m12, C_TEXT3, ax[i], 'c', 0, LT_Y + LT_H + 16);
    L.net = txt(t, W, &font_m12, C_PINK, "", 'r', W - MX, LT_Y + LT_H + 16);
    L.foot = txt(t, W, &font_m12, C_TEXT2, "", 'l', MX, 306);
    lv_label_set_recolor(L.foot, true);
    L.drop = rect(t, 0, 0, 4, 4, C_PINK);
    lv_obj_add_flag(L.drop, LV_OBJ_FLAG_HIDDEN);
}

static void lottery_axis(void)
{
    static const int ex[5] = {3, 6, 9, 12, 15};
    for (int i = 0; i < 5; i++) {
        if (LT_X + lot_col(ex[i]) * DOT > W - MX - 100) { lv_obj_add_flag(L.axis[i], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_clear_flag(L.axis[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(L.axis[i], LV_ALIGN_TOP_MID, LT_X + lot_col(ex[i]) * DOT + 1 - W / 2, LT_Y + LT_H + 16 - ascent(&font_m12));
    }
}

static void lottery_tick(lv_timer_t *t)
{
    double D = 0;
    data_lock();
    D = data_get()->difficulty;
    data_unlock();
    double lmax = D > 1e6 ? log10(D) : 14;
    if (fabs(lmax - L.lmax) > 0.01) { L.lmax = lmax; lottery_axis(); L.ver = 0; }

    shares_lock();
    share_stats_t *st = shares_get();
    if (st->ver == L.ver) { shares_unlock(); return; }
    bool newer = L.ver != 0;
    L.ver = st->ver;
    char b[96], n1[24], n2[24];
    lv_label_set_text(L.last, fmt_diff(n1, sizeof(n1), st->last));
    lv_label_set_text(L.best, fmt_diff(n1, sizeof(n1), st->best));
    fmt_de(n1, sizeof(n1), shares_per_min(st), 0);
    lv_label_set_text(L.rate, n1);
    lv_label_set_text(L.status, st->live ? "LIVE" : "SIMULIERT");
    lv_obj_set_style_bg_color(L.dot, st->live ? C_PINK : C_TEXT3, 0);
    lv_obj_update_layout(L.status);
    lv_obj_set_x(L.dot, lv_obj_get_x(L.status) - 14);
    fmt_diff(n1, sizeof(n1), D);
    snprintf(b, sizeof(b), "BLOCK %s", n1);
    lv_label_set_text(L.net, b);
    fmt_de(n1, sizeof(n1), st->total, 0);
    double ratio = st->best > 0 && D > 0 ? D / st->best : 0;
    if (ratio >= 1e9) { fmt_de(n2, sizeof(n2), ratio / 1e9, 1); strlcat(n2, " Mrd.", sizeof(n2)); }
    else if (ratio >= 1e6) { fmt_de(n2, sizeof(n2), ratio / 1e6, 1); strlcat(n2, " Mio.", sizeof(n2)); }
    else if (ratio > 0) fmt_de(n2, sizeof(n2), ratio, 0);
    else strcpy(n2, "–");
    snprintf(b, sizeof(b), "%s Hashes  ·  bis zum Block fehlt noch Faktor #FFD4D4 %s#", n1, n2);
    lv_label_set_text(L.foot, b);
    bool visible = s_card[car_current()] == L.tile && s_nb < 0;
    double last = st->last;
    if (!visible || !newer || !lv_obj_has_flag(L.drop, LV_OBJ_FLAG_HIDDEN)) {
        lottery_draw(st);
        shares_unlock();
        return;
    }
    shares_unlock();
    // neuer Hash fällt als pinker Punkt in seine Säule
    int col = lot_col(log10(last > 1 ? last : 1));
    lv_obj_set_pos(L.drop, LT_X + col * DOT, LT_Y - 10);
    lv_obj_clear_flag(L.drop, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, L.drop);
    lv_anim_set_exec_cb(&a, drop_exec);
    lv_anim_set_values(&a, LT_Y - 10, LT_Y + LT_H - 24);
    lv_anim_set_time(&a, 420);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&a, drop_ready);
    lv_anim_start(&a);
}

// ------------------------------------------------------------------ Aktualisieren

static uint32_t v_price, v_chart, v_net, v_miner, v_cost, v_cost_p;
static void check_price_alarm(double px);
static void toast_show(const char *title, const char *msg, int ms);
static void block_anim_show(uint32_t old_h, uint32_t new_h, const char *pool, uint32_t tx, double reward, int64_t ts);
static void block_card_set(uint32_t h, const char *pool, uint32_t tx, double reward, int64_t ts);
static void block_card_age(void);

static void set_change(lv_obj_t *l, double pct, const char *suffix)
{
    char b[40], n[24];
    fmt_change(n, sizeof(n), pct);
    snprintf(b, sizeof(b), "%s%s", n, suffix);
    lv_label_set_text(l, b);
    lv_obj_set_style_text_color(l, pct >= 0 ? C_UP : C_DOWN, 0);
}

static void update_price(deck_data_t *d)
{
    if (!d->price_ok || d->price_ver == v_price) return;
    v_price = d->price_ver;
    char b[48], n[24];
    double px = g_settings.currency ? d->eur : d->usd, pchg = g_settings.currency ? d->chg24_eur : d->chg24;
    fit_big(A.px.lbl, px, MX - 6, 150);
    tween_to(&A.px, px);
    tween_to(&B.usd, d->usd);
    tween_to(&B.eur, d->eur);
    tween_to(&C.px, px);
    snprintf(b, sizeof(b), "BTC / %s", settings_cur());
    lv_label_set_text(A.cur, b);
    snprintf(b, sizeof(b), "BTC / %s · %s", settings_cur(), RANGE_NAMES[g_settings.chart_range]);
    lv_label_set_text(C.cur, b);
    set_change(A.chg, pchg, "  24H");
    fmt_de(n, sizeof(n), g_settings.currency ? d->usd : d->eur, 0);
    snprintf(b, sizeof(b), "%s %s", n, g_settings.currency ? "USD" : "EUR");
    lv_label_set_text(A.alt, b);
    set_change(B.chg_usd, d->chg24, "");
    set_change(B.chg_eur, d->chg24_eur, "");
    if (px > 0) {
        fmt_de(b, sizeof(b), 1e8 / px, 0);
        lv_label_set_text(E.sats, b);
    }
    snprintf(b, sizeof(b), "SATS / 1 %s", settings_cur());
    lv_label_set_text(E.sats_t, b);
    check_price_alarm(px);
}

static void update_chart(deck_data_t *d)
{
    if (d->chart_ver == v_chart) return;
    v_chart = d->chart_ver;
    if (!d->chart_ok || d->chart_n < 2) {
        lv_label_set_text(C.msg, "PREISDATEN NICHT VERFÜGBAR");
        lv_obj_clear_flag(C.msg, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(C.msg, LV_OBJ_FLAG_HIDDEN);
    char b[48], n[24];
    double chg = (d->chart_last - d->chart_first) / d->chart_first * 100;
    set_change(C.chg, chg, "");
    snprintf(b, sizeof(b), "BTC / %s · %s", settings_cur(), RANGE_NAMES[d->chart_range]);
    lv_label_set_text(C.cur, b);
    lv_label_set_text(C.range_lbl, RANGE_NAMES[d->chart_range]);
    double pad = (d->chart_max - d->chart_min) * 0.12;
    memcpy(K.v, d->chart, sizeof(float) * d->chart_n);
    K.n = d->chart_n;
    K.mn = d->chart_min - pad;
    K.mx = d->chart_max + pad;
    K.t0 = d->chart_t0;
    K.t1 = d->chart_t1;
#ifdef CONFIG_DECK_PERF_LOG
    int64_t t_draw = esp_timer_get_time();
#endif
    chart_draw();
    mosaic_draw(K.v, K.n, d->chart_min, d->chart_max);
#ifdef CONFIG_DECK_PERF_LOG
    printf("BENCH Chart gezeichnet in %d ms\n", (int)((esp_timer_get_time() - t_draw) / 1000));
#endif
    // Hoch / Tief
    int ih = 0, il = 0;
    for (int i = 1; i < K.n; i++) {
        if (K.v[i] > K.v[ih]) ih = i;
        if (K.v[i] < K.v[il]) il = i;
    }
    int idx[2] = {ih, il};
    lv_obj_t *lbl[2] = {K.hi, K.lo};
    for (int k = 0; k < 2; k++) {
        int px = CH_X + idx[k] * (CH_W - 1) / (K.n - 1), py = CH_Y + val_y(K.v[idx[k]]);
        fmt_de(n, sizeof(n), K.v[idx[k]], 0);
        snprintf(b, sizeof(b), "%s %s", k == 0 ? "H" : "T", n);
        lv_label_set_text(lbl[k], b);
        lv_obj_set_style_bg_color(lbl[k], C_BG, 0); // im Pixel-Stil schwarz hinterlegt, sonst unlesbar
        lv_obj_set_style_bg_opa(lbl[k], chart_pixel() ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_hor(lbl[k], chart_pixel() ? 4 : 0, 0);
        lv_obj_update_layout(lbl[k]);
        int lw = lv_obj_get_width(lbl[k]);
        int lx = px - lw / 2;
        if (lx < CH_X) lx = CH_X;
        if (lx + lw > CH_X + CH_W) lx = CH_X + CH_W - lw;
        int ly = k == 0 || chart_pixel() ? py - 22 : py + 10; // Pixel: Tief über seiner Säule
        if (ly < CH_Y - 8) ly = CH_Y - 8;
        if (ly > CH_Y + CH_H - 14) ly = CH_Y + CH_H - 14;
        lv_obj_align(lbl[k], LV_ALIGN_TOP_LEFT, lx, ly);
    }
    // Zeitachse
    for (int i = 0; i < 5; i++) {
        time_t ts = (time_t)((d->chart_t0 + (d->chart_t1 - d->chart_t0) * i / 4) / 1000);
        struct tm tm;
        localtime_r(&ts, &tm);
        strftime(b, sizeof(b), d->chart_range == RANGE_24H ? "%H:%M" : d->chart_range == RANGE_1Y ? "%m/%y" : "%d.%m.", &tm);
        lv_label_set_text(C.xlab[i], b);
    }
    // Hoch/Tief unter dem Mosaik der Preiskarte
    char a1[24], a2[24];
    fmt_de(a1, sizeof(a1), d->chart_max, 0);
    fmt_de(a2, sizeof(a2), d->chart_min, 0);
    snprintf(b, sizeof(b), "%s  H %s  T %s", RANGE_NAMES[d->chart_range], a1, a2);
    lv_label_set_text(A.hilo, b);
}

static double subsidy_at(uint32_t h) { return 50.0 / pow(2, h / 210000); }

static void update_network(deck_data_t *d)
{
    if (!d->net_ok || d->net_ver == v_net) return;
    v_net = d->net_ver;
    char b[96], n[24], n2[24];
    static uint32_t card_h = 0; // zuletzt auf der Karte „Neuer Block“ gezeigte Höhe
    if (d->blocks_n && d->height != card_h) {
        const block_info_t *bi = &d->blocks[0];
        if (card_h && d->height > card_h) {
            block_anim_show(card_h, d->height, bi->pool, bi->tx_count, bi->reward_btc, bi->timestamp);
            hg_drop();
            // „Neuer Block“ eingeschaltet: zur Karte springen und das Display wecken
            if (g_settings.block_note && card_on(CARD_NEWBLOCK) && !s_booting && !SW.down && !K.scrub) {
                car_jump(CARD_NEWBLOCK, true);
                lv_disp_trig_activity(NULL);
            }
        } else block_card_set(d->height, bi->pool, bi->tx_count, bi->reward_btc, bi->timestamp);
        card_h = d->height;
    }
    fmt_plain(b, sizeof(b), d->height);
    lv_label_set_text(D.height, b);
    lv_label_set_text(E.height, b);
    uint32_t next = (d->height / 210000 + 1) * 210000;
    fmt_de(n, sizeof(n), next - d->height, 0);
    fmt_de(n2, sizeof(n2), subsidy_at(next), 4);
    snprintf(b, sizeof(b), "Halving in %s Blöcken · danach %s BTC", n, n2);
    lv_label_set_text(D.halving, b);
    double px = g_settings.currency ? d->eur : d->usd;
    int fees[3] = {d->fee_hour, d->fee_half, d->fee_fast};
    for (int i = 0; i < 3; i++) {
        lv_label_set_text_fmt(E.fee[i], "%d", fees[i]);
        fmt_de(n, sizeof(n), fees[i] * 140.0 / 1e8 * px, 2); // typische Transaktion ~140 vB
        snprintf(b, sizeof(b), "%s %s", n, cur_sym());
        lv_label_set_text(E.fiat[i], px > 0 ? b : "");
    }
    fmt_de(n, sizeof(n), fabs(d->diff_change), 2);
    snprintf(b, sizeof(b), "%s%s %%", d->diff_change >= 0 ? "+" : "–", n);
    lv_label_set_text(F.chg, b);
    lv_obj_set_style_text_color(F.chg, d->diff_change >= 0 ? C_UP : C_DOWN, 0);
    cell_fill(F.cells, (int)(d->diff_progress * 55 / 100 + 0.5), C_PINK, 0, C_LINE);
    fmt_de(n, sizeof(n), d->remaining_blocks, 0);
    lv_label_set_text(F.blocks, n);
    fmt_de(n, sizeof(n), d->diff_progress, 1);
    snprintf(b, sizeof(b), "%s %%", n);
    lv_label_set_text(F.pct, b);
    v_miner = 0; // Netzwerk-Difficulty fließt in die Bitaxe-Karte ein
}

static void update_age(deck_data_t *d)
{
    if (!d->blocks_n || !net_time_valid()) return;
    int64_t s = time(NULL) - d->blocks[0].timestamp;
    char b[96];
    if (s < 60) snprintf(b, sizeof(b), "Letzter Block gerade eben · %s", d->blocks[0].pool);
    else snprintf(b, sizeof(b), "Letzter Block vor %d Min · %s", (int)(s / 60), d->blocks[0].pool);
    if (strcmp(lv_label_get_text(D.age), b)) lv_label_set_text(D.age, b);
    block_card_age();
}

static void update_miner(deck_data_t *d)
{
    if (d->miner_ver == v_miner || d->miner_n < 1) return;
    v_miner = d->miner_ver;
    char b[220], n1[24], n2[24], n3[24];
    miner_t *m = &d->miners[0];
    double D = d->difficulty > 0 ? d->difficulty : 1e14;
    snprintf(b, sizeof(b), "BITAXE %s", m->board[0] ? (strcmp(m->board, "601") ? m->board : "GAMMA 601") : "");
    lv_label_set_text(HB.title, b);
    lv_color_t dc;
    if (m->mode == MINER_ONLINE) { lv_label_set_text(HB.status, m->host); dc = C_PINK; }
    else if (m->mode == MINER_OFFLINE) { snprintf(b, sizeof(b), "OFFLINE · %s", m->host); lv_label_set_text(HB.status, b); dc = C_DOWN; }
    else { lv_label_set_text(HB.status, "DEMO"); dc = C_TEXT3; }
    lv_obj_set_style_bg_color(HB.dot, dc, 0);
    lv_obj_update_layout(HB.status);
    lv_obj_set_x(HB.dot, lv_obj_get_x(HB.status) - 14);
    fmt_de(n1, sizeof(n1), m->hashrate_gh / 1000, 2);
    lv_label_set_text(HB.hash, n1);
    lv_obj_update_layout(HB.hash);
    lv_obj_set_x(HB.unit, lv_obj_get_x(HB.hash) + lv_obj_get_width(HB.hash) + 10);
    // Session- und Best-Diff gegenüber Netzwerk (logarithmisch) als Kästchenreihe
    double ln = log10(D);
    int c1 = m->session_diff > 1 ? (int)(55 * fmin(1, log10(m->session_diff) / ln)) : 0;
    int c2 = m->best_diff > 1 ? (int)(55 * fmin(1, log10(m->best_diff) / ln)) : 0;
    cell_fill(HB.cells, c1, C_PINK, c2 > c1 ? c2 : c1, C_PINK_DIM);
    fmt_diff(n1, sizeof(n1), m->session_diff);
    fmt_diff(n2, sizeof(n2), m->best_diff);
    fmt_diff(n3, sizeof(n3), D);
    snprintf(b, sizeof(b), "#FFD4D4 SESSION %s#   #9A9A9A BEST %s#   #5E5E5E NETZ %s#", n1, n2, n3);
    lv_label_set_text(HB.legend, b);
    fmt_de(n1, sizeof(n1), m->temp, 1);
    lv_label_set_text(HB.val[0], n1);
    fmt_de(n1, sizeof(n1), m->vrtemp, 1);
    lv_label_set_text(HB.val[1], m->vrtemp > 0 ? n1 : "–");
    lv_label_set_text_fmt(HB.val[2], "%d", m->fan_pct);
}

static void update_cost(deck_data_t *d)
{
    if (d->miner_n < 1 || (d->miner_ver == v_cost && d->price_ver == v_cost_p)) return;
    v_cost = d->miner_ver;
    v_cost_p = d->price_ver;
    miner_t *m = &d->miners[0];
    char b[96];
    lv_color_t dc;
    if (m->mode == MINER_ONLINE) { lv_label_set_text(KS.status, m->host); dc = C_PINK; }
    else if (m->mode == MINER_OFFLINE) { snprintf(b, sizeof(b), "OFFLINE · %s", m->host); lv_label_set_text(KS.status, b); dc = C_DOWN; }
    else { lv_label_set_text(KS.status, "DEMO"); dc = C_TEXT3; }
    lv_obj_set_style_bg_color(KS.dot, dc, 0);
    lv_obj_update_layout(KS.status);
    lv_obj_set_x(KS.dot, lv_obj_get_x(KS.status) - 14);
    KS.power = m->power;
    KS.btc = d->price_ok ? d->eur : 0;
    KS.e_wh = d->energy_wh;
    KS.e_day_wh = d->energy_day_wh;
    KS.since = d->energy_since;
    KS.have = true;
    cost_render();
}

static void update_clock(void)
{
    static const char *days[7] = {"SONNTAG", "MONTAG", "DIENSTAG", "MITTWOCH", "DONNERSTAG", "FREITAG", "SAMSTAG"};
    if (!net_time_valid()) return;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char t[8], dt[16], full[40];
    strftime(t, sizeof(t), "%H:%M", &tm);
    strftime(dt, sizeof(dt), "%d.%m.%Y", &tm);
    if (strcmp(lv_label_get_text(G.time), t)) {
        lv_label_set_text(G.time, t);
        lv_label_set_text(A.time, t);
        lv_label_set_text(B.time, t);
    }
    snprintf(full, sizeof(full), "%s · %s", days[tm.tm_wday], dt);
    if (strcmp(lv_label_get_text(G.date), full)) {
        lv_label_set_text(G.date, full);
        if (LD.on) { // LED: Datum ausgeschrieben
            static const char *mon[12] = {"JANUAR", "FEBRUAR", "MÄRZ", "APRIL", "MAI", "JUNI", "JULI", "AUGUST", "SEPTEMBER", "OKTOBER", "NOVEMBER", "DEZEMBER"};
            char d2[40];
            snprintf(d2, sizeof(d2), "%d. %s %d", tm.tm_mday, mon[tm.tm_mon], tm.tm_year + 1900);
            lv_label_set_text(LD.date, d2);
        }
    }
}

// ------------------------------------------------------------------ Hinweise (Preisalarm, neuer Block)

static lv_obj_t *s_toast, *s_toast_t, *s_toast_m;
static int64_t s_toast_until;

static void toast_hide_cb(lv_event_t *e) { lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN); s_toast_until = 0; }

static void toast_create(void)
{
    s_toast = panel(lv_layer_top(), 40, 104, 400, 112);
    lv_obj_set_style_bg_color(s_toast, C_PANEL, 0);
    lv_obj_set_style_radius(s_toast, 14, 0);
    lv_obj_set_style_border_color(s_toast, C_PINK, 0);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(s_toast, toast_hide_cb, LV_EVENT_CLICKED, NULL);
    s_toast_t = txt(s_toast, 400, &font_m14, C_PINK, "", 'l', 22, 36);
    lv_obj_set_style_text_letter_space(s_toast_t, 2, 0);
    s_toast_m = txt(s_toast, 400, &font_g26, C_TEXT, "", 'l', 22, 80);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
}

static void toast_show(const char *title, const char *msg, int ms)
{
    lv_label_set_text(s_toast_t, title);
    lv_label_set_text(s_toast_m, msg);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    s_toast_until = esp_timer_get_time() / 1000 + ms;
    lv_disp_trig_activity(NULL); // weckt aus dem Nachtmodus
}

static void toast_tick(void)
{
    if (!s_toast_until) return;
    int64_t now = esp_timer_get_time() / 1000;
    if (now > s_toast_until) { toast_hide_cb(NULL); return; }
    lv_obj_set_style_border_opa(s_toast, (now / 500) % 2 ? LV_OPA_COVER : LV_OPA_30, 0);
}

// ------------------------------------------------------------------ 4 Neuer Block (eigene Karte)
// Kette der letzten Blöcke als Pixel-Blöcke, der neueste in Pink, rechts ein freier Platz für den
// nächsten. Kommt ein Block, fällt er auf den freien Platz, die Höhe rollt wie ein Zählwerk hoch,
// dann rückt die Kette einen Platz weiter.
#define BA_N 6                            // Plätze in der Kette (der letzte ist der nächste Block)
#define BA_SZ 48                          // Kantenlänge eines Blocks: 6 × 6 Zellen à 8 px
#define BA_PITCH 72
#define BA_X0 ((W - BA_N * BA_PITCH) / 2)
#define BA_Y 82
#define BA_NUM_Y 236                      // Grundlinie der Blockhöhe
#define BA_LS 3                           // engere Laufweite der großen Zahl
static struct {
    lv_obj_t *title, *time, *info, *pre, *num_box, *old_n, *new_n, *link[BA_N - 1];
    lv_obj_t *box[BA_N], *lbl[BA_N];      // Container: Block und Höhe darunter
    canvas_t cv[BA_N];
    int64_t ts;                           // Zeitstempel des neuesten Blocks
} BA;

static void ba_paint(canvas_t *c, uint32_t seed, bool fresh)
{
    static const uint32_t pk[4] = {0xFFD4D4, 0xE8BDBD, 0xC9A3A3, 0x9C8080};
    lv_canvas_fill_bg(c->cv, C_BG, LV_OPA_COVER);
    for (int r = 0; r < 6; r++)
        for (int q = 0; q < 6; q++) {
            uint32_t h = hash2((int)(seed % 100000) * 7 + q, r + 31);
            fill_box(c, q * 8, r * 8, 7, 7, fresh ? lv_color_hex(pk[h % 4]) : gray(0x2A + (int)(h % 0x38)));
        }
    lv_obj_invalidate(c->cv);
}

static void ba_x(void *o, int32_t v) { lv_obj_set_x(o, v); }
static void ba_y(void *o, int32_t v) { lv_obj_set_y(o, v); }
static void ba_fade(void *o, int32_t v) { lv_obj_set_style_opa(o, (lv_opa_t)v, 0); }
static void ba_link(void *o, int32_t v) { lv_obj_set_style_bg_color(o, lv_color_mix(C_LINE2, C_PINK, (uint8_t)v), 0); }
static void ba_numcol(void *o, int32_t v) { lv_obj_set_style_text_color(o, lv_color_mix(C_TEXT, C_PINK, (uint8_t)v), 0); }

static void ba_anim(void *var, lv_anim_exec_xcb_t cb, int from, int to, int delay, int ms, lv_anim_path_cb_t path, bool early)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, var);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_time(&a, ms);
    lv_anim_set_path_cb(&a, path ? path : lv_anim_path_ease_in_out);
    lv_anim_set_early_apply(&a, early);
    lv_anim_start(&a);
}

static void ba_stop(void)
{
    for (int i = 0; i < BA_N; i++) lv_anim_del(BA.box[i], NULL);
    for (int i = 0; i < BA_N - 1; i++) lv_anim_del(BA.link[i], NULL);
    lv_anim_del(BA.old_n, NULL);
    lv_anim_del(BA.new_n, NULL);
}

static void build_newblock(lv_obj_t *t)
{
    BA.title = cap(t, W, "NEUER BLOCK", 'l', MX, 30);
    BA.time = txt(t, W, &font_m14, C_TEXT2, "", 'r', W - MX, 30);
    // Verbindungen zwischen den Plätzen und ein leerer Platz für den nächsten Block
    for (int i = 0; i < BA_N - 1; i++)
        BA.link[i] = rect(t, BA_X0 + i * BA_PITCH + 12 + BA_SZ, BA_Y + BA_SZ / 2 - 1, BA_PITCH - BA_SZ, 2, C_LINE2);
    lv_obj_t *slot = rect(t, BA_X0 + (BA_N - 1) * BA_PITCH + 12, BA_Y, BA_SZ - 1, BA_SZ - 1, C_BG);
    lv_obj_set_style_border_width(slot, 1, 0);
    lv_obj_set_style_border_color(slot, C_LINE2, 0);
    lv_obj_set_style_radius(slot, 2, 0);
    for (int i = 0; i < BA_N; i++) {
        BA.box[i] = rect(t, BA_X0 + i * BA_PITCH, BA_Y, BA_PITCH, BA_SZ + 26, C_BG);
        lv_obj_set_style_bg_opa(BA.box[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_opa(BA.box[i], LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(BA.box[i], LV_OBJ_FLAG_CLICKABLE);
        canvas_create(&BA.cv[i], BA.box[i], 12, 0, BA_SZ, BA_SZ);
        BA.lbl[i] = txt(BA.box[i], BA_PITCH, &font_m12, C_TEXT3, "", 'c', BA_PITCH / 2, BA_SZ + 20);
    }
    // Blockhöhe: gleicher Anfang fest, die geänderten Ziffern rollen in einem Fenster
    const lv_font_t *f = &font_g72;
    BA.pre = tight(txt(t, W, f, C_TEXT, "–", 'c', W / 2, BA_NUM_Y), BA_LS);
    BA.num_box = rect(t, 0, BA_NUM_Y - ascent(f), 10, f->line_height, C_BG);
    lv_obj_clear_flag(BA.num_box, LV_OBJ_FLAG_CLICKABLE);
    BA.old_n = tight(txt(BA.num_box, W, f, C_TEXT, "", 'l', 0, ascent(f)), BA_LS);
    BA.new_n = tight(txt(BA.num_box, W, f, C_PINK, "", 'l', 0, ascent(f)), BA_LS);
    BA.info = txt(t, W, &font_m14, C_TEXT2, "", 'c', W / 2, 282);
}

// Zählwerk: gleicher Anfang steht, der Rest rollt von old_h auf new_h (gleich = nur anzeigen)
static void ba_number(uint32_t old_h, uint32_t new_h)
{
    char n1[24], n2[24], pre[24];
    fmt_plain(n1, sizeof(n1), old_h);
    fmt_plain(n2, sizeof(n2), new_h);
    int p = 0;
    if (strlen(n1) == strlen(n2))
        while (n1[p] && n1[p] == n2[p]) p++;
    memcpy(pre, n2, p);
    pre[p] = 0;
    lv_label_set_text(BA.pre, pre);
    lv_label_set_text(BA.old_n, n1 + p);
    lv_label_set_text(BA.new_n, n2 + p);
    const lv_font_t *f = &font_g72;
    int wp = p ? lv_txt_get_width(pre, strlen(pre), f, -BA_LS, LV_TEXT_FLAG_NONE) : 0;
    int wn = lv_txt_get_width(n2 + p, strlen(n2 + p), f, -BA_LS, LV_TEXT_FLAG_NONE);
    int wo = lv_txt_get_width(n1 + p, strlen(n1 + p), f, -BA_LS, LV_TEXT_FLAG_NONE);
    if (p && wn) wp -= BA_LS; // Laufweite zwischen festem und rollendem Teil
    int x0 = (W - wp - wn) / 2;
    lv_obj_align(BA.pre, LV_ALIGN_TOP_LEFT, x0, BA_NUM_Y - ascent(f));
    lv_obj_set_x(BA.num_box, x0 + wp);
    lv_obj_set_width(BA.num_box, (wn > wo ? wn : wo) + 6);
    lv_obj_set_y(BA.old_n, 0);
    lv_obj_set_y(BA.new_n, old_h == new_h ? 0 : f->line_height);
}

static void ba_info(const char *pool, uint32_t tx, double reward, int64_t ts)
{
    char b[96], n[24];
    int o = snprintf(b, sizeof(b), "%s", pool && pool[0] ? pool : "Unbekannter Pool");
    if (tx) {
        fmt_de(n, sizeof(n), tx, 0);
        o += snprintf(b + o, sizeof(b) - o, " · %s Transaktionen", n);
    }
    if (reward > 0) {
        fmt_de(n, sizeof(n), reward, 3);
        snprintf(b + o, sizeof(b) - o, " · %s BTC", n);
    }
    lv_label_set_text(BA.info, b);
    BA.ts = ts;
    block_card_age();
}

// Überschrift und Uhrzeit/Alter des neuesten Blocks (alle paar Sekunden)
static void block_card_age(void)
{
    if (BA.ts <= 0 || !net_time_valid()) { lv_label_set_text(BA.time, ""); return; }
    int64_t age = time(NULL) - BA.ts;
    time_t t = (time_t)BA.ts;
    struct tm tm;
    localtime_r(&t, &tm);
    char hm[8], b[48];
    strftime(hm, sizeof(hm), "%H:%M", &tm);
    if (age < 60) snprintf(b, sizeof(b), "%s Uhr · gerade eben", hm);
    else snprintf(b, sizeof(b), "%s Uhr · vor %d Min", hm, (int)(age / 60));
    if (strcmp(lv_label_get_text(BA.time), b)) lv_label_set_text(BA.time, b);
    const char *ti = age < 600 ? "NEUER BLOCK" : "LETZTER BLOCK";
    if (strcmp(lv_label_get_text(BA.title), ti)) lv_label_set_text(BA.title, ti);
}

// Ruhezustand: die letzten fünf Blöcke, der neueste pink, rechts der freie Platz
static void block_card_set(uint32_t h, const char *pool, uint32_t tx, double reward, int64_t ts)
{
    ba_stop();
    for (int i = 0; i < BA_N; i++) {
        bool on = i < BA_N - 1;
        uint32_t hh = h - (BA_N - 2) + i;
        lv_obj_set_pos(BA.box[i], BA_X0 + i * BA_PITCH, BA_Y);
        lv_obj_set_style_opa(BA.box[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        if (!on) continue;
        ba_paint(&BA.cv[i], hh, i == BA_N - 2);
        lv_label_set_text_fmt(BA.lbl[i], "%u", (unsigned)hh);
        lv_obj_set_style_text_color(BA.lbl[i], i == BA_N - 2 ? C_PINK : C_TEXT3, 0);
    }
    for (int i = 0; i < BA_N - 1; i++) lv_obj_set_style_bg_color(BA.link[i], C_LINE2, 0);
    ba_number(h, h);
    lv_obj_set_style_text_color(BA.new_n, C_TEXT, 0);
    ba_info(pool, tx, reward, ts);
}

static void block_anim_show(uint32_t old_h, uint32_t new_h, const char *pool, uint32_t tx, double reward, int64_t ts)
{
    ba_stop();
    // Kette: die letzten fünf Blöcke, der neue wartet oberhalb seines Platzes
    for (int i = 0; i < BA_N; i++) {
        bool fresh = i == BA_N - 1;
        uint32_t hh = new_h - (BA_N - 1) + i;
        lv_obj_set_pos(BA.box[i], BA_X0 + i * BA_PITCH, fresh ? -BA_SZ - 40 : BA_Y);
        lv_obj_set_style_opa(BA.box[i], LV_OPA_COVER, 0);
        ba_paint(&BA.cv[i], hh, fresh);
        lv_label_set_text_fmt(BA.lbl[i], "%u", (unsigned)hh);
        lv_obj_set_style_text_color(BA.lbl[i], fresh ? C_PINK : C_TEXT3, 0);
    }
    for (int i = 0; i < BA_N - 1; i++) lv_obj_set_style_bg_color(BA.link[i], C_LINE2, 0);
    ba_number(old_h, new_h);
    lv_obj_set_style_text_color(BA.new_n, C_PINK, 0);
    ba_info(pool, tx, reward, ts);
    int lh = font_g72.line_height;
    // 1. der neue Block fällt auf seinen Platz
    ba_anim(BA.box[BA_N - 1], ba_y, -BA_SZ - 40, BA_Y, 350, 700, lv_anim_path_bounce, true);
    // 2. Verbindung leuchtet auf, die Höhe rollt hoch
    ba_anim(BA.link[BA_N - 2], ba_link, 0, 255, 1000, 1400, lv_anim_path_ease_out, false);
    ba_anim(BA.old_n, ba_y, 0, -lh, 1050, 520, lv_anim_path_ease_out, true);
    ba_anim(BA.new_n, ba_y, lh, 0, 1050, 520, lv_anim_path_ease_out, true);
    // 3. die Kette rückt weiter, der älteste Block verschwindet links; die neue Ziffer wird wieder hell
    for (int i = 0; i < BA_N; i++) {
        int x = BA_X0 + i * BA_PITCH;
        ba_anim(BA.box[i], ba_x, x, x - BA_PITCH, 3000, 650, NULL, false);
    }
    ba_anim(BA.box[0], ba_fade, LV_OPA_COVER, LV_OPA_TRANSP, 3000, 450, NULL, false);
    ba_anim(BA.new_n, ba_numcol, 0, 255, 4200, 900, NULL, false);
}

// Preisalarm: einmal pro Überschreiten, erneut erst nach 0,5 % Rückkehr
// Zeitfenster des Nachtmodus (unabhängig davon, ob das Display gerade gedimmt ist)
static bool night_window(void)
{
    if (!g_settings.night_on || !net_time_valid()) return false;
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    int f = g_settings.night_from, to = g_settings.night_to, h = tm.tm_hour;
    return f <= to ? (h >= f && h < to) : (h >= f || h < to);
}

#define ALARM_MS 15000 // so lange steht der Preisalarm höchstens

// Preisalarm: im Zeitfenster des Nachtmodus still (kein Wecken); steht der Kurs danach noch jenseits der
// Grenze, kommt der Alarm dann
static void check_price_alarm(double px)
{
    static int state = 0;
    if (!g_settings.alarm_on || px <= 0) { state = 0; return; }
    if (night_window()) return;
    char n[24], b[64];
    if (g_settings.alarm_hi > 0 && px >= g_settings.alarm_hi && state != 1) {
        state = 1;
        fmt_de(n, sizeof(n), g_settings.alarm_hi, 0);
        snprintf(b, sizeof(b), "Kurs über %s %s", n, cur_sym());
        toast_show("PREISALARM", b, ALARM_MS);
        car_jump(0, true);
    } else if (g_settings.alarm_lo > 0 && px <= g_settings.alarm_lo && state != -1) {
        state = -1;
        fmt_de(n, sizeof(n), g_settings.alarm_lo, 0);
        snprintf(b, sizeof(b), "Kurs unter %s %s", n, cur_sym());
        toast_show("PREISALARM", b, ALARM_MS);
        car_jump(0, true);
    } else if (state == 1 && px < g_settings.alarm_hi * 0.995) state = 0;
    else if (state == -1 && px > g_settings.alarm_lo * 1.005) state = 0;
}

// ------------------------------------------------------------------ Einstellungen & Kartenmenü

static lv_obj_t *s_cfg, *s_menu, *s_qr, *s_cfg_l1, *s_cfg_l2, *s_slider[2], *s_menu_tile[DECK_CARDS];
static lv_obj_t *s_rot_val, *s_range_val, *s_upd_pill, *s_upd_lbl;

// Update-Knopf oben rechts: „v1.4.2 · PRÜFEN“ sucht sofort, „UPDATE x →“ installiert
static int s_upd_ask;                  // 0 = nichts, 1 = am Gerät angefragt, 2 = Suche läuft
static int64_t s_upd_t0, s_upd_until;  // Zeitpunkt der Anfrage / „aktuell“ zeigen bis (ms)
static void ota_tick(void);

static void upd_cb(lv_event_t *e)
{
    ota_status_t o;
    ota_get(&o);
    if (o.state == OTA_AVAILABLE) { ota_install_now(); return; }
    if (o.state == OTA_CHECKING || o.state == OTA_INSTALLING || s_upd_ask) return;
    if (net_state() != NET_CONNECTED) { toast_show("KEIN WLAN", "Prüfen geht nur online", 5000); return; }
    s_upd_ask = 1;
    s_upd_t0 = esp_timer_get_time() / 1000;
    s_upd_until = 0;
    ota_check_now();
    ota_tick(); // sofort „SUCHE …“ zeigen
}

static void upd_pill_set(const char *s, bool accent)
{
    if (strcmp(lv_label_get_text(s_upd_lbl), s)) lv_label_set_text(s_upd_lbl, s);
    lv_obj_set_style_bg_color(s_upd_pill, accent ? C_PINK : C_BG, 0);
    lv_obj_set_style_border_width(s_upd_pill, accent ? 0 : 1, 0);
    lv_obj_set_style_text_color(s_upd_lbl, accent ? C_BG : C_TEXT, 0);
}

// Vollbild während der Installation eines Updates
static struct { lv_obj_t *scr, *ver, *pct, *cells[55]; } U;

static void build_update_screen(void)
{
    U.scr = rect(lv_layer_top(), 0, 0, W, H, C_BG);
    lv_obj_add_flag(U.scr, LV_OBJ_FLAG_CLICKABLE);
    cap(U.scr, W, "SOFTWARE-UPDATE", 'l', MX, 30);
    U.ver = tight(txt(U.scr, W, &font_g36, C_TEXT, "", 'l', MX - 1, 150), 1);
    for (int i = 0; i < 55; i++) {
        U.cells[i] = rect(U.scr, MX + i * 8, 176, 6, 6, C_LINE);
        lv_obj_set_style_radius(U.cells[i], 1, 0);
    }
    U.pct = txt(U.scr, W, &font_m14, C_PINK, "", 'l', MX, 216);
    txt(U.scr, W, &font_m14, C_TEXT2, "Bitte nicht ausschalten. Das Display startet danach neu.", 'l', MX, 300);
    lv_obj_add_flag(U.scr, LV_OBJ_FLAG_HIDDEN);
}

static void ota_tick(void)
{
    static ota_state_t prev = OTA_IDLE;
    static char announced[24] = "";
    ota_status_t o;
    ota_get(&o);
    int64_t now = esp_timer_get_time() / 1000;
    char b[64];
    // Prüfung am Gerät: Ergebnis abwarten und melden
    if (s_upd_ask == 1 && o.state == OTA_CHECKING) s_upd_ask = 2;
    if (s_upd_ask == 1 && now - s_upd_t0 > 20000) {
        s_upd_ask = 0;
        toast_show("UPDATE-PRÜFUNG", "Keine Antwort", 5000);
    }
    if (s_upd_ask == 2 && o.state != OTA_CHECKING) {
        s_upd_ask = 0;
        if (o.state == OTA_UPTODATE) s_upd_until = now + 5000;
        else if (o.state == OTA_FAILED) toast_show("UPDATE-PRÜFUNG", o.error, 6000);
    }
    if (o.state == OTA_AVAILABLE) {
        snprintf(b, sizeof(b), "UPDATE %s →", o.latest);
        upd_pill_set(b, true);
        if (strcmp(announced, o.latest) && !g_settings.auto_update) {
            strlcpy(announced, o.latest, sizeof(announced));
            snprintf(b, sizeof(b), "Version %s – in den Einstellungen", o.latest);
            toast_show("UPDATE VERFÜGBAR", b, 8000);
        }
    } else if (s_upd_ask || o.state == OTA_CHECKING) {
        upd_pill_set("SUCHE …", false);
    } else if (now < s_upd_until) {
        snprintf(b, sizeof(b), "v%s " LV_SYMBOL_OK " AKTUELL", deck_version());
        upd_pill_set(b, false);
    } else {
        snprintf(b, sizeof(b), "v%s · PRÜFEN", deck_version());
        upd_pill_set(b, false);
    }
    if (o.state == OTA_INSTALLING) {
        if (lv_obj_has_flag(U.scr, LV_OBJ_FLAG_HIDDEN)) {
            lv_label_set_text_fmt(U.ver, "Version %s", o.latest);
            lv_obj_clear_flag(U.scr, LV_OBJ_FLAG_HIDDEN);
            lv_disp_trig_activity(NULL);
        }
        int n = o.progress * 55 / 100;
        for (int i = 0; i < 55; i++) lv_obj_set_style_bg_color(U.cells[i], i < n ? C_PINK : C_LINE, 0);
        lv_label_set_text_fmt(U.pct, "%d %%", o.progress);
    } else {
        lv_obj_add_flag(U.scr, LV_OBJ_FLAG_HIDDEN);
        if (prev == OTA_INSTALLING && o.state == OTA_FAILED) toast_show("UPDATE FEHLGESCHLAGEN", o.error, 8000);
    }
    prev = o.state;
}

typedef struct { lv_obj_t *track, *knob, *val; bool on; const char *t_on, *t_off; void (*cb)(bool); } toggle_t;
static toggle_t T_cur, T_alarm, T_night, T_block;

static void knob_exec(void *var, int32_t v) { lv_obj_set_x(var, v); }

static void toggle_set(toggle_t *t, bool on, bool anim)
{
    t->on = on;
    lv_obj_set_style_bg_color(t->track, on ? C_PINK : C_LINE, 0);
    lv_obj_set_style_bg_color(t->knob, on ? C_BG : C_TEXT2, 0);
    lv_label_set_text(t->val, on ? t->t_on : t->t_off);
    int to = on ? 44 - 18 - 4 : 4;
    if (!anim) { lv_obj_set_x(t->knob, to); return; }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, t->knob);
    lv_anim_set_exec_cb(&a, knob_exec);
    lv_anim_set_values(&a, lv_obj_get_x(t->knob), to);
    lv_anim_set_time(&a, 160);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void toggle_cb(lv_event_t *e)
{
    toggle_t *t = lv_event_get_user_data(e);
    toggle_set(t, !t->on, true);
    t->cb(t->on);
}

// Einstellungs-Kachel mit Beschriftung, Wert und Schalter
static void toggle_tile(toggle_t *t, lv_obj_t *p, int x, int y, const char *title, const char *t_on, const char *t_off, bool on, void (*cb)(bool))
{
    lv_obj_t *pn = panel(p, x, y, 144, 78);
    cap(pn, 144, title, 'l', 12, 24);
    t->val = txt(pn, 144, &font_g20, C_TEXT, "", 'l', 12, 60);
    t->track = rect(pn, 144 - 12 - 44, 40, 44, 26, C_LINE);
    lv_obj_set_style_radius(t->track, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(t->track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(t->track, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_ext_click_area(t->track, 18);
    lv_obj_add_event_cb(t->track, toggle_cb, LV_EVENT_CLICKED, t);
    t->knob = rect(t->track, 4, 4, 18, 18, C_TEXT2);
    lv_obj_set_style_radius(t->knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(t->knob, LV_OBJ_FLAG_CLICKABLE);
    t->t_on = t_on;
    t->t_off = t_off;
    t->cb = cb;
    toggle_set(t, on, false);
}

static void apply_currency(void)
{
    v_price = 0;
    v_net = 0;
    data_request_chart((chart_range_t)g_settings.chart_range);
    chart_loading();
}

static void on_cur(bool usd) { g_settings.currency = usd ? 0 : 1; settings_save(); apply_currency(); }
static void on_alarm(bool on) { g_settings.alarm_on = on; settings_save(); }
static void on_night(bool on) { g_settings.night_on = on; settings_save(); }
static void on_block(bool on) { g_settings.block_note = on; settings_save(); }

static const uint16_t ROT_STEPS[] = {0, 5, 10, 15, 20, 30, 45, 60, 120, 300};
#define ROT_N (sizeof(ROT_STEPS) / sizeof(ROT_STEPS[0]))

static void stepper_labels(void)
{
    if (g_settings.rotate_s) lv_label_set_text_fmt(s_rot_val, "%d s", g_settings.rotate_s);
    else lv_label_set_text(s_rot_val, "AUS");
    lv_label_set_text(s_range_val, RANGE_NAMES[g_settings.chart_range]);
}

static void step_cb(lv_event_t *e)
{
    int code = (int)(intptr_t)lv_event_get_user_data(e); // 0/1 = Seitenwechsel -/+, 2/3 = Chart -/+
    if (code < 2) {
        int i = 0;
        while (i < (int)ROT_N - 1 && ROT_STEPS[i] < g_settings.rotate_s) i++;
        i += code ? 1 : -1;
        if (i < 0) i = 0;
        if (i >= (int)ROT_N) i = ROT_N - 1;
        g_settings.rotate_s = ROT_STEPS[i];
    } else {
        int r = g_settings.chart_range + (code == 3 ? 1 : -1);
        if (r < 0) r = 3;
        if (r > 3) r = 0;
        g_settings.chart_range = r;
        lv_label_set_text(C.range_lbl, RANGE_NAMES[r]);
        data_request_chart((chart_range_t)r);
        chart_loading();
    }
    settings_save();
    stepper_labels();
}

// runder Knopf mit Rahmen
static lv_obj_t *round_btn(lv_obj_t *p, int x, int y, int d, const char *s, const lv_font_t *f)
{
    lv_obj_t *b = rect(p, x, y, d, d, C_BG);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, C_LINE2, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x161616), LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_ext_click_area(b, 8);
    if (s) {
        // Zeichen nach seinem tatsächlichen Umriss mittig setzen (nicht nach der Textzeile)
        lv_obj_t *l = lv_label_create(b);
        lv_obj_set_style_text_font(l, f, 0);
        lv_obj_set_style_text_color(l, C_TEXT, 0);
        lv_label_set_text(l, s);
        uint32_t i = 0, letter = _lv_txt_encoded_next(s, &i);
        lv_font_glyph_dsc_t g;
        float c = (d - 2) / 2.0f; // Innenmaß ohne 1-px-Rahmen
        if (lv_font_get_glyph_dsc(f, &g, letter, 0)) {
            float gx = c - g.ofs_x - g.box_w / 2.0f;
            float gy = c - ascent(f) + g.ofs_y + g.box_h / 2.0f;
            lv_obj_set_pos(l, (lv_coord_t)lroundf(gx), (lv_coord_t)lroundf(gy));
        } else {
            lv_obj_center(l);
        }
    }
    return b;
}

static lv_obj_t *stepper_tile(lv_obj_t *p, int x, int y, const char *title, int code_minus)
{
    lv_obj_t *pn = panel(p, x, y, 144, 78);
    cap(pn, 144, title, 'l', 12, 24);
    lv_obj_t *v = txt(pn, 144, &font_g20, C_TEXT, "", 'l', 12, 60);
    lv_obj_t *m = round_btn(pn, 144 - 12 - 26 - 6 - 26, 39, 26, "–", &font_m20);
    lv_obj_add_event_cb(m, step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)code_minus);
    lv_obj_t *pl = round_btn(pn, 144 - 12 - 26, 39, 26, "+", &font_m20);
    lv_obj_add_event_cb(pl, step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(code_minus + 1));
    return v;
}

static void slider_cb(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    int v = lv_slider_get_value(sl);
    g_settings.brightness = v;
    board_set_brightness(v);
    for (int i = 0; i < 2; i++) if (s_slider[i] && s_slider[i] != sl) lv_slider_set_value(s_slider[i], v, LV_ANIM_OFF);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) settings_save();
}

static void overlay_close(void)
{
    lv_obj_add_flag(s_cfg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
}

static void close_cb(lv_event_t *e) { overlay_close(); }

static void open_cfg(void)
{
    char ip[20], qr[96], b[128];
    net_ip(ip, sizeof(ip));
    if (net_state() == NET_PORTAL) {
        snprintf(qr, sizeof(qr), "WIFI:T:WPA;S:%s;P:%s;;", DECK_AP_SSID, DECK_AP_PASS);
        snprintf(b, sizeof(b), "WLAN %s · Passwort %s", DECK_AP_SSID, DECK_AP_PASS);
        lv_label_set_text(s_cfg_l1, b);
        lv_label_set_text(s_cfg_l2, "Dann 192.168.4.1 öffnen →");
    } else {
        snprintf(qr, sizeof(qr), "http://%s", ip[0] ? ip : "192.168.4.1");
        lv_label_set_text(s_cfg_l1, "QR-Code scannen oder im Browser öffnen:");
        snprintf(b, sizeof(b), "%s →", ip[0] ? ip : "–");
        lv_label_set_text(s_cfg_l2, b);
    }
    lv_qrcode_update(s_qr, qr, strlen(qr));
    stepper_labels();
    lv_obj_add_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_cfg, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *s_menu_sw[DECK_CARDS], *s_menu_knob[DECK_CARDS], *s_menu_name[DECK_CARDS], *s_menu_num[DECK_CARDS];

// Kartenmenü: Kachel halten und ziehen ordnet die Karten um; die anderen rücken während des Ziehens nach
#define MENU_PITCH_X 148
#define MENU_PITCH_Y 52
#define MENU_TW 144
#define MENU_TH 48
#define MENU_Y0 10
static struct {
    bool on;
    int id, slot;            // gezogene Karte, ihr Platz in order
    lv_point_t off;          // Finger relativ zur Kachel
    uint8_t order[DECK_CARDS];
} MV;

static void mv_x(void *o, int32_t v) { lv_obj_set_x(o, v); }
static void mv_y(void *o, int32_t v) { lv_obj_set_y(o, v); }

static void mv_anim(lv_obj_t *o, lv_anim_exec_xcb_t cb, int from, int to)
{
    if (from == to) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, 180);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

// Kacheln auf ihre Plätze (beim Ziehen nach der vorläufigen Reihenfolge), Nummern = Platz
static void menu_layout(bool anim)
{
    const uint8_t *order = MV.on ? MV.order : g_settings.card_order;
    for (int p = 0; p < DECK_CARDS; p++) {
        int id = order[p];
        lv_label_set_text_fmt(s_menu_num[id], "%02d", p + 1);
        if (MV.on && id == MV.id) continue;
        lv_obj_t *t = s_menu_tile[id];
        int x = MX + (p % 3) * MENU_PITCH_X, y = MENU_Y0 + (p / 3) * MENU_PITCH_Y;
        lv_anim_del(t, mv_x);
        lv_anim_del(t, mv_y);
        if (anim) {
            mv_anim(t, mv_x, lv_obj_get_x(t), x);
            mv_anim(t, mv_y, lv_obj_get_y(t), y);
        } else lv_obj_set_pos(t, x, y);
    }
}

static void menu_refresh(void);
static void menu_drag_cb(lv_event_t *e)
{
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_get_act();
    if (!in) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    lv_obj_t *t = s_menu_tile[id];
    if (code == LV_EVENT_LONG_PRESSED) { // anheben
        MV.on = true;
        MV.id = id;
        memcpy(MV.order, g_settings.card_order, DECK_CARDS);
        MV.slot = card_pos(id);
        lv_anim_del(t, mv_x);
        lv_anim_del(t, mv_y);
        MV.off.x = p.x - lv_obj_get_x(t);
        MV.off.y = p.y - lv_obj_get_y(t);
        lv_obj_move_foreground(t);
        lv_obj_set_style_border_color(t, C_PINK, 0);
        lv_obj_set_style_bg_color(t, lv_color_hex(0x1C1C1C), LV_STATE_PRESSED); // angehoben: heller
    } else if (code == LV_EVENT_PRESSING && MV.on && MV.id == id) {
        int x = p.x - MV.off.x, y = p.y - MV.off.y;
        int ymax = MENU_Y0 + ((DECK_CARDS - 1) / 3) * MENU_PITCH_Y;
        x = x < 0 ? 0 : x > W - MENU_TW ? W - MENU_TW : x;
        y = y < 0 ? 0 : y > ymax ? ymax : y;
        lv_obj_set_pos(t, x, y);
        // Ziel: der Platz unter der Mitte der Kachel
        int col = (x + MENU_TW / 2 - MX) / MENU_PITCH_X, row = (y + MENU_TH / 2 - MENU_Y0) / MENU_PITCH_Y;
        col = col < 0 ? 0 : col > 2 ? 2 : col;
        int slot = row * 3 + col;
        if (slot >= DECK_CARDS) slot = DECK_CARDS - 1;
        if (slot != MV.slot) {
            if (slot > MV.slot) memmove(MV.order + MV.slot, MV.order + MV.slot + 1, slot - MV.slot);
            else memmove(MV.order + slot + 1, MV.order + slot, MV.slot - slot);
            MV.order[slot] = id;
            MV.slot = slot;
            menu_layout(true);
        }
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && MV.on && MV.id == id) { // ablegen
        MV.on = false;
        if (memcmp(MV.order, g_settings.card_order, DECK_CARDS)) {
            memcpy(g_settings.card_order, MV.order, DECK_CARDS);
            settings_save();
        }
        lv_obj_set_style_bg_color(t, lv_color_hex(0x111111), LV_STATE_PRESSED);
        menu_layout(true);
        menu_refresh();
    }
}

#ifdef CONFIG_DECK_SNAPSHOT
// Testhilfe (Screenshot-Modus): Ziehen nachstellen – erste Karte angehoben über Platz 5 – bzw. beenden
static void menu_drag_test(bool on)
{
    int id = g_settings.card_order[0];
    lv_obj_t *t = s_menu_tile[id];
    MV.on = on;
    if (on) {
        MV.id = id;
        memcpy(MV.order, g_settings.card_order, DECK_CARDS);
        memmove(MV.order, MV.order + 1, 4);
        MV.order[4] = id;
        MV.slot = 4;
        lv_obj_move_foreground(t);
        lv_obj_set_style_border_color(t, C_PINK, 0);
        lv_obj_set_style_bg_color(t, lv_color_hex(0x1C1C1C), 0);
        lv_obj_set_pos(t, MX + MENU_PITCH_X + 26, MENU_Y0 + MENU_PITCH_Y + 12);
    } else lv_obj_set_style_bg_color(t, C_BG, 0);
    menu_layout(false);
    menu_refresh();
    if (on) lv_obj_set_style_border_color(t, C_PINK, 0);
}
#endif

static void menu_refresh(void)
{
    for (int i = 0; i < DECK_CARDS; i++) {
        bool on = card_on(i);
        lv_obj_set_style_border_color(s_menu_tile[i], i == s_cur ? C_PINK : C_LINE, 0);
        lv_obj_set_style_text_color(s_menu_name[i], on ? C_TEXT : C_TEXT3, 0);
        lv_obj_set_style_bg_color(s_menu_sw[i], on ? C_PINK : C_LINE, 0);
        lv_obj_set_style_bg_color(s_menu_knob[i], on ? C_BG : C_TEXT2, 0);
        lv_obj_set_x(s_menu_knob[i], on ? 34 - 14 - 2 : 2);
    }
}

static void card_toggle_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    uint16_t m = g_settings.card_mask ^ (1u << i);
    if (!m) return; // mindestens eine Karte bleibt an
    g_settings.card_mask = m;
    settings_save();
    if (!card_on(s_cur)) car_jump(card_step(s_cur, 1), false);
    menu_refresh();
}

static void open_menu(void)
{
    menu_layout(false);
    menu_refresh();
    lv_obj_add_flag(s_cfg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
}

static void to_menu_cb(lv_event_t *e) { open_menu(); }
static void to_cfg_cb(lv_event_t *e) { open_cfg(); }

static void menu_item_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (!card_on(idx)) return; // ausgeschaltete Karte: erst einschalten
    overlay_close();
    car_jump(idx, false);
}


// Untere Leiste: runder Knopf links, Helligkeit, Schließen
static void bottom_bar(lv_obj_t *p, int si, bool gear)
{
    lv_obj_t *l = round_btn(p, MX, 280, 34, gear ? LV_SYMBOL_SETTINGS : NULL, &font_m16);
    if (!gear) {
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) {
                lv_obj_t *q = rect(l, (34 - 2 - 13) / 2 + c * 5, (34 - 2 - 13) / 2 + r * 5, 3, 3, C_TEXT);
                lv_obj_clear_flag(q, LV_OBJ_FLAG_CLICKABLE);
            }
    }
    lv_obj_add_event_cb(l, gear ? to_cfg_cb : to_menu_cb, LV_EVENT_CLICKED, NULL);
    cap(p, W, "HELLIGKEIT", 'l', 70, 302);
    lv_obj_t *sl = lv_slider_create(p);
    lv_obj_set_pos(sl, 160, 295);
    lv_obj_set_size(sl, 236, 4);
    lv_slider_set_range(sl, 5, 100);
    lv_slider_set_value(sl, g_settings.brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, C_LINE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, C_PINK, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, C_TEXT, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sl, 7, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(sl, 0, LV_PART_KNOB);
    lv_obj_set_ext_click_area(sl, 18);
    lv_obj_add_event_cb(sl, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(sl, slider_cb, LV_EVENT_RELEASED, NULL);
    s_slider[si] = sl;
    lv_obj_t *x = round_btn(p, W - MX - 34, 280, 34, LV_SYMBOL_CLOSE, &font_m16);
    lv_obj_add_event_cb(x, close_cb, LV_EVENT_CLICKED, NULL);
}

static void build_overlays(void)
{
    // Konfiguration
    s_cfg = rect(lv_scr_act(), 0, 0, W, H, C_BG);
    lv_obj_clear_flag(s_cfg, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_t *qb = rect(s_cfg, MX, 14, 84, 84, C_TEXT);
    lv_obj_set_style_radius(qb, 10, 0);
    s_qr = lv_qrcode_create(qb, 72, lv_color_black(), C_TEXT);
    lv_obj_center(s_qr);
    txt(s_cfg, W, &font_g26, C_TEXT, "Einstellungen", 'l', 120, 40);
    lv_obj_t *pl;
    s_upd_pill = pill(s_cfg, W - MX - 150, 12, 150, 26, "", &pl);
    lv_obj_del(pl);
    s_upd_lbl = txt(s_upd_pill, 150, &font_m12, C_TEXT, "", 'c', 75, 17);
    lv_obj_set_style_text_letter_space(s_upd_lbl, 1, 0);
    lv_obj_add_event_cb(s_upd_pill, upd_cb, LV_EVENT_CLICKED, NULL);
    s_cfg_l1 = txt(s_cfg, W, &font_m12, C_TEXT2, "", 'l', 120, 66);
    s_cfg_l2 = txt(s_cfg, W, &font_m14, C_PINK, "", 'l', 120, 88);
    static const int xs[3] = {MX, MX + 148, MX + 296};
    toggle_tile(&T_cur, s_cfg, xs[0], 108, "WÄHRUNG", "USD", "EUR", g_settings.currency == 0, on_cur);
    toggle_tile(&T_alarm, s_cfg, xs[1], 108, "PREISALARM", "EIN", "AUS", g_settings.alarm_on, on_alarm);
    s_rot_val = stepper_tile(s_cfg, xs[2], 108, "SEITENWECHSEL", 0);
    toggle_tile(&T_night, s_cfg, xs[0], 192, "NACHTMODUS", "EIN", "AUS", g_settings.night_on, on_night);
    toggle_tile(&T_block, s_cfg, xs[1], 192, "NEUER BLOCK", "EIN", "AUS", g_settings.block_note, on_block);
    s_range_val = stepper_tile(s_cfg, xs[2], 192, "CHART", 2);
    bottom_bar(s_cfg, 0, false);
    lv_obj_add_flag(s_cfg, LV_OBJ_FLAG_HIDDEN);

    // Kartenmenü
    s_menu = rect(lv_scr_act(), 0, 0, W, H, C_BG);
    lv_obj_clear_flag(s_menu, LV_OBJ_FLAG_EVENT_BUBBLE);
    const char *const *names = DECK_CARD_NAMES;
    // 3 Spalten × 5 Reihen, angeordnet nach der eingestellten Reihenfolge (menu_layout)
    for (int i = 0; i < DECK_CARDS; i++) {
        lv_obj_t *b = panel(s_menu, 0, 0, MENU_TW, MENU_TH);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x111111), LV_STATE_PRESSED);
        s_menu_num[i] = txt(b, 144, &font_m12, C_TEXT3, "", 'l', 12, 18);
        s_menu_name[i] = txt(b, 144, &font_g20, C_TEXT, names[i], 'l', 12, 40);
        lv_obj_add_event_cb(b, menu_item_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i); // nicht nach dem Ziehen
        lv_obj_add_event_cb(b, menu_drag_cb, LV_EVENT_ALL, (void *)(intptr_t)i);
        s_menu_tile[i] = b;
        // Schalter: Karte ein/aus
        lv_obj_t *sw = rect(b, 144 - 12 - 34, 7, 34, 18, C_LINE);
        lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, 0);
        lv_obj_add_flag(sw, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(sw, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_ext_click_area(sw, 14);
        lv_obj_add_event_cb(sw, card_toggle_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_menu_sw[i] = sw;
        s_menu_knob[i] = rect(sw, 2, 2, 14, 14, C_TEXT2);
        lv_obj_set_style_radius(s_menu_knob[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_clear_flag(s_menu_knob[i], LV_OBJ_FLAG_CLICKABLE);
    }
    menu_layout(false);
    bottom_bar(s_menu, 1, true);
    lv_obj_add_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
}

// Seitenwechsel und Nachtmodus
static void auto_tick(void)
{
    uint32_t idle = lv_disp_get_inactive_time(NULL);
    bool overlay = !lv_obj_has_flag(s_cfg, LV_OBJ_FLAG_HIDDEN) || !lv_obj_has_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
    static int64_t last_rot = 0;
    int64_t now = esp_timer_get_time() / 1000;
    if (s_booting) last_rot = now; // erste Karte nach dem Boot-Screen voll stehen lassen
#ifdef CONFIG_DECK_SNAPSHOT
    last_rot = now; // Aufnahmen nicht durch Blättern stören
#endif
    if (g_settings.rotate_s && !s_booting && !overlay && !K.scrub && !SW.down && idle >= g_settings.rotate_s * 1000u &&
        now - last_rot >= g_settings.rotate_s * 1000) {
        last_rot = now;
        car_go(1);
    }
    static int cur = -1, tint = -1;
    int want = g_settings.brightness;
    bool night = idle > 30000 && !s_toast_until && night_window();
    // Nachtmodus: schwache Beleuchtung, alles in einer sanften Farbe; Berührung weckt
    int want_tint = night && g_settings.night_color < NIGHT_COLORS ? g_settings.night_color : -1;
    if (night) want = NIGHT_BRIGHT;
    if (want != cur) { cur = want; board_set_brightness(want); }
    if (want_tint != tint) {
        tint = want_tint;
        board_set_tint(tint >= 0, tint >= 0 ? NIGHT_COLOR_RGB[tint] : 0);
        lv_obj_invalidate(lv_scr_act());
        lv_obj_invalidate(lv_layer_top());
    }
}

static void ui_timer_cb(lv_timer_t *t)
{
    static int ticks = 0;
    data_lock();
    deck_data_t *d = data_get();
    update_price(d);
    update_chart(d);
    update_network(d);
    update_miner(d);
    update_cost(d);
    clock_trend(d);
    update_weather(d);
    update_news(d);
    update_halving(d);
    if (ticks % 10 == 0) update_age(d);
    data_unlock();
    if (ticks % 2 == 0) update_clock();
    toast_tick();
    auto_tick();
    ota_tick();
    // Ohne WLAN: Konfiguration mit Einrichtungs-QR-Code öffnen
    static bool portal_shown = false;
    if (!portal_shown && net_state() == NET_PORTAL) {
        portal_shown = true;
        open_cfg();
    }
    if (portal_shown && net_state() == NET_CONNECTED && !lv_obj_has_flag(s_cfg, LV_OBJ_FLAG_HIDDEN) && ticks % 4 == 0) open_cfg();
    ticks++;
}

// ------------------------------------------------------------------ Rahmen

#ifdef CONFIG_DECK_SNAPSHOT
#include "mbedtls/base64.h"
// Aufnahmen brauchen 300 KB am Stück – einmal früh reservieren, bevor der Speicher zerstückelt
static uint8_t *s_snap_buf;
static lv_img_dsc_t s_snap_dsc;
static lv_img_dsc_t *snap_take(lv_obj_t *obj)
{
    if (!s_snap_buf) s_snap_buf = heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM);
    if (!s_snap_buf || lv_snapshot_take_to_buf(obj, LV_IMG_CF_TRUE_COLOR, &s_snap_dsc, s_snap_buf, W * H * 2) != LV_RES_OK) return NULL;
    return &s_snap_dsc;
}
// Screenshot-Modus: nach dem Laden der Live-Daten jede Karte (+ Einstellungen, Menü) über USB ausgeben
static void bench_cb(lv_timer_t *t)
{
    static int idx = -1, wait = 0, row = -1, ticks = 0, sid = 0;
    static lv_img_dsc_t *snap;
    static const int ba_at[4] = {700, 1250, 1900, 3500}; // Block-Animation: Aufnahmezeitpunkte (ms)
    static const int ld_at[2] = {90, 600};           // Uhr: Aufnahmen während und nach dem Wechsel (ms)
    static const double tr_chg[2] = {2.34, -1.45};   // Uhr mit Trend rauf / runter
    ticks++;
    if (idx >= N_TILES + 3 + 4 + NIGHT_COLORS + 2 + 2) return;
    if (idx < 0) {
        deck_data_t *d = data_get();
        bool ready = d->price_ok && d->net_ok && d->chart_ok;
        if (s_booting || (!ready && ticks < 900)) return; // erst nach dem Boot-Screen (sein Puffer ist dann frei)
        idx = 0;
        car_jump(idx, false);
        wait = 0;
        return;
    }
    if (row < 0) {
        int k = idx - (N_TILES + 3); // davor: Karten, Einstellungen, Menü, Menü beim Ziehen
        if (k >= 4 + NIGHT_COLORS + 2) { // Uhr mit 24h-Trend rauf bzw. runter
            if (wait++ == 0) {
                car_jump(8, false);
                clock_trend_apply(tr_chg[k - 4 - NIGHT_COLORS - 2]);
                return;
            }
            if (wait < 12) return;
        } else if (k >= 4 + NIGHT_COLORS) { // LED-Uhr: 12:59 → 13:00
            static int64_t t0;
            int64_t now = esp_timer_get_time() / 1000;
            if (wait++ == 0) {
                car_jump(8, false);
                return;
            }
            if (wait == 10) {
                ld_test(12, 59);
                t0 = now;
                return;
            }
            if (wait < 10 || now - t0 < ld_at[k - 4 - NIGHT_COLORS]) return;
        } else if (k >= 4) { // Nachtfarben: Preiskarte, umgerechnet wie beim Übertragen ans Display
            if (wait++ == 0) {
                car_jump(0, false);
                return;
            }
            if (wait < 20) return;
        } else if (k >= 0) { // Block-Animation jeweils neu starten und zum Zeitpunkt k aufnehmen
            static int64_t t0;
            int64_t now = esp_timer_get_time() / 1000;
            if (wait++ == 0) {
                car_jump(CARD_NEWBLOCK, false);
                data_lock();
                deck_data_t *d = data_get();
                uint32_t h = d->height ? d->height : 920000;
                block_anim_show(h - 1, h, d->blocks_n ? d->blocks[0].pool : "", d->blocks_n ? d->blocks[0].tx_count : 0,
                                d->blocks_n ? d->blocks[0].reward_btc : 0, d->blocks_n ? d->blocks[0].timestamp : 0);
                data_unlock();
                t0 = now;
                return;
            }
            if (now - t0 < ba_at[k]) return;
        } else if (++wait < 20) return;
        sid = k >= 4 + NIGHT_COLORS + 2 ? 70 + k - 4 - NIGHT_COLORS - 2
            : k >= 4 + NIGHT_COLORS     ? 60 + k - 4 - NIGHT_COLORS
            : k >= 4                    ? 50 + k - 4
            : k >= 0                    ? 30 + k
                                        : idx;
        snap = snap_take(lv_scr_act());
        if (!snap) { printf("SNAPERR %d\n", sid); idx++; wait = 0; return; }
        if (k >= 4 && k < 4 + NIGHT_COLORS) {
            board_set_tint(true, NIGHT_COLOR_RGB[k - 4]);
            board_tint_buf((void *)snap->data, snap->header.w * snap->header.h);
            board_set_tint(false, 0);
        }
        printf("\nSNAP %d %d %d\n", sid, (int)snap->header.w, (int)snap->header.h);
        row = 0;
    }
    static unsigned char out[480 * 2 * 8 * 4 / 3 + 8];
    for (int k = 0; k < 8 && row < (int)snap->header.h; k++, row++) {
        size_t olen = 0;
        mbedtls_base64_encode(out, sizeof(out), &olen, snap->data + row * snap->header.w * 2, snap->header.w * 2);
        out[olen] = 0;
        printf("R%d %s\n", row, out);
    }
    if (row >= (int)snap->header.h) {
        printf("SNAPEND %d\n", sid);
        
        snap = NULL;
        row = -1;
        wait = 0;
        ++idx;
        if (idx < N_TILES) car_jump(idx, false);
        else if (idx == N_TILES) open_cfg();
        else if (idx == N_TILES + 1) open_menu();
        else if (idx == N_TILES + 2) menu_drag_test(true);
        else if (idx == N_TILES + 3) { menu_drag_test(false); overlay_close(); }
    }
}
#elif defined(CONFIG_DECK_PERF_BENCH)
// Messmodus: jede Karte 1,5 s ständig neu zeichnen (Ø Renderzeit), danach echtes Blättern
extern volatile uint32_t g_perf_sum, g_perf_n;
static void bench_cb(lv_timer_t *t)
{
    static int idx = -1, frame = 0, k = 0, dir = 1;
    if (idx < N_TILES) {
        if (frame == 0) {
            idx++;
            if (idx < N_TILES) car_jump(idx, false);
        }
        if (idx >= N_TILES) return;
        if (frame == 3) { g_perf_sum = 0; g_perf_n = 0; }
        lv_obj_invalidate(lv_scr_act());
        if (++frame >= 30) {
            printf("BENCH Karte %d: %lu ms\n", idx, (unsigned long)(g_perf_n ? g_perf_sum / g_perf_n : 0));
            frame = 0;
        }
        return;
    }
    if (++frame % 25 == 0) {
        (void)k;
        (void)dir;
        car_go(1);
    }
}
#endif


// ------------------------------------------------------------------ Boot-Screen (bis alle Daten geladen sind)
// Pixel fallen von oben und stapeln sich zum Mosaik, Schriftzug blendet ein, darunter der echte
// Startvorgang (Display, WLAN, Bitaxe, Kurs, Block). Ein Tippen überspringt ihn.
#define BOOT_MAX_MS 45000 // höchstens so lange, falls etwas nicht lädt
#define BC 8 // Mosaik-Zelle
#define BOOT_BAND 11                                 // Reihen des Mosaiks oben (darunter der Text)
#define BOOT_FALL 480                                // Fallzeit einer Zelle (ms)
#define BOOT_STEP 60                                 // Versatz je Reihe: die unteren landen zuerst
#define BOOT_FALL_END ((BOOT_BAND + 1) * BOOT_STEP + 420 + BOOT_FALL) // bis dahin sind alle gelandet
static struct {
    lv_obj_t *scr, *word, *sub, *ver, *row_v[5], *row_d[5], *cells[55];
    canvas_t mos;
    lv_timer_t *tm;
    int64_t t0;
    bool closing;
    uint8_t target[60 * 40]; // Ziel-Helligkeit je Zelle
    uint8_t cur[60 * 40];
} BS;

static void boot_fade_exec(void *var, int32_t v) { lv_obj_set_style_opa(var, (lv_opa_t)v, 0); }
static void boot_done(lv_anim_t *a)
{
    lv_timer_del(BS.tm);
    lv_obj_del(BS.scr);
    heap_caps_free(BS.mos.buf); // 300 KB Mosaik-Puffer wieder freigeben
    BS.mos.buf = NULL;
    BS.scr = NULL;
    s_booting = false;
    ESP_LOGI("ui", "Boot fertig: PSRAM frei %u KB, größter Block %u KB, intern frei %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
}

static void boot_close(void)
{
    if (BS.closing) return;
    BS.closing = true;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, BS.scr);
    lv_anim_set_exec_cb(&a, boot_fade_exec);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&a, 450);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&a, boot_done);
    lv_anim_start(&a);
}

static void boot_tap_cb(lv_event_t *e) { boot_close(); }

static void boot_row(int i, const char *val, bool ok)
{
    lv_label_set_text(BS.row_v[i], val);
    lv_obj_set_style_text_color(BS.row_v[i], ok ? C_TEXT : C_TEXT3, 0);
    lv_obj_set_style_bg_color(BS.row_d[i], ok ? C_PINK : C_LINE, 0);
}

static void boot_tick(lv_timer_t *t)
{
    if (!BS.scr || BS.closing) return;
    int64_t ms = esp_timer_get_time() / 1000 - BS.t0;
    const int cols = 60, rows = 40;
    bool dirty = false;
    if (ms < BOOT_FALL_END + 66) {
        // Mosaik: jede Zelle fällt von oben auf ihren Platz (schneller werdend), die unteren Reihen zuerst
        lv_canvas_fill_bg(BS.mos.cv, C_BG, LV_OPA_COVER);
        for (int r = 0; r < rows; r++)
            for (int c = 0; c < cols; c++) {
                int i = r * cols + c;
                if (!BS.target[i]) continue;
                int start = (BOOT_BAND - r) * BOOT_STEP + (int)(hash2(c, r) % 420);
                if (ms < start) continue;
                float p = (ms - start) / (float)BOOT_FALL;
                if (p > 1) p = 1;
                int y = -BC + (int)((r * BC + BC) * p * p);
                fill_box(&BS.mos, c * BC, y, BC - 1, BC - 1, BS.target[i] == 255 ? C_PINK : gray(BS.target[i]));
                BS.cur[i] = p >= 1 ? BS.target[i] : 0;
            }
        lv_obj_invalidate(BS.mos.cv);
    } else {
        // Ende des Falls: einmal das fertige Mosaik zeichnen (falls Bilder ausgefallen sind)
        static bool landed = false;
        if (!landed) {
            landed = true;
            lv_canvas_fill_bg(BS.mos.cv, C_BG, LV_OPA_COVER);
            for (int i = 0; i < cols * rows; i++) {
                BS.cur[i] = BS.target[i];
                if (BS.target[i])
                    fill_box(&BS.mos, (i % cols) * BC, (i / cols) * BC, BC - 1, BC - 1,
                             BS.target[i] == 255 ? C_PINK : gray(BS.target[i]));
            }
            dirty = true;
        }
    }
    // danach leichtes Flackern
    if (ms >= BOOT_FALL_END + 66)
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            int i = r * cols + c;
            int want = BS.target[i];
            if (ms > 3500 && BS.target[i] && (hash2(c * 7 + (int)(ms / 180), r) % 97) == 0) want = BS.target[i] / 3; // Flackern
            int v = BS.cur[i];
            if (v != want) {
                v += (want - v) / 3 + (want > v ? 1 : -1);
                if ((want > BS.cur[i] && v > want) || (want < BS.cur[i] && v < want)) v = want;
                BS.cur[i] = v;
                lv_color_t col = BS.target[i] == 255 ? lv_color_mix(C_PINK, C_BG, v) : gray(v);
                if (BS.target[i] == 255 && v < 255) col = lv_color_mix(C_PINK, C_BG, v);
                fill_box(&BS.mos, c * BC, r * BC, BC - 1, BC - 1, BS.target[i] == 255 ? col : gray(v));
                dirty = true;
            }
        }
    if (dirty) lv_obj_invalidate(BS.mos.cv);

    // Schriftzug einblenden
    int wo = ms < 1400 ? 0 : ms > 2400 ? 255 : (int)((ms - 1400) * 255 / 1000);
    lv_obj_set_style_text_opa(BS.word, wo, 0);
    lv_obj_set_style_text_opa(BS.sub, wo, 0);
    lv_obj_set_style_text_opa(BS.ver, wo, 0);

    // Startvorgang mit echten Daten: Zeilen erscheinen gestaffelt, der Screen bleibt,
    // bis alles geladen ist (mind. 4,5 s für die Animation, höchstens 45 s)
    char b[64], n[24];
    bool ok[5] = {true, false, false, false, false};
    char ip[20];
    net_ip(ip, sizeof(ip));
    net_state_t st = net_state();
    if (st == NET_PORTAL) {
        if (ms > 3000) { boot_close(); return; } // Einrichtung öffnet sich
    }
    if (ms > 2400) boot_row(0, "480 × 320 · LVGL 8.4", true);
    if (st == NET_CONNECTED && net_time_valid()) { ok[1] = true; snprintf(b, sizeof(b), "%s · %s", g_settings.wifi_ssid, ip); }
    else snprintf(b, sizeof(b), st == NET_CONNECTED ? "Uhrzeit …" : "verbinde …");
    if (ms > 2900) boot_row(1, b, ok[1]);
    data_lock();
    deck_data_t *d = data_get();
    miner_t *m = &d->miners[0];
    if (!g_settings.bitaxe_host[0][0]) { ok[2] = d->miner_n > 0; snprintf(b, sizeof(b), "Demo"); }
    else if (d->miner_n && m->mode == MINER_ONLINE) {
        ok[2] = true;
        fmt_de(n, sizeof(n), m->hashrate_gh / 1000, 2);
        snprintf(b, sizeof(b), "%s TH/s · %s", n, m->host);
    } else if (d->miner_n && m->mode == MINER_OFFLINE) { ok[2] = true; snprintf(b, sizeof(b), "nicht erreichbar · %s", m->host); }
    else snprintf(b, sizeof(b), "suche %s …", g_settings.bitaxe_host[0]);
    if (ms > 3400) boot_row(2, b, ok[2]);
    if (d->price_ok && d->chart_ok) {
        ok[3] = true;
        fmt_de(n, sizeof(n), g_settings.currency ? d->eur : d->usd, 0);
        snprintf(b, sizeof(b), "%s %s · Chart %s", n, settings_cur(), RANGE_NAMES[d->chart_range]);
    } else snprintf(b, sizeof(b), d->price_ok ? "Chart …" : "lade …");
    if (ms > 3900) boot_row(3, b, ok[3]);
    if (d->net_ok) { ok[4] = true; snprintf(b, sizeof(b), "%u · Gebühren · Difficulty", (unsigned)d->height); }
    else snprintf(b, sizeof(b), "lade …");
    if (ms > 4400) boot_row(4, b, ok[4]);
    data_unlock();
    int done = 0;
    for (int i = 0; i < 5; i++) done += ok[i];
    // Fortschritt: folgt den geladenen Daten, kriecht dazwischen langsam weiter
    static int shown = 0;
    int target = done * 11, crawl = done < 5 ? 9 : 0;
    int goal = target + (int)(crawl * (1.0f - expf(-(ms % 60000) / 8000.0f)));
    if (goal > 55) goal = 55;
    if (shown < goal && ms % 2 == 0) shown++;
    static int64_t all_ok_at = 0;
    if (done == 5 && !all_ok_at) all_ok_at = ms;
    if (done == 5) shown = shown + 2 > 55 ? 55 : shown + 2;
#ifdef CONFIG_DECK_SNAPSHOT
    static int shot = 0;
    static const int when[3] = {700, 1500, 3200};
    if (shot < 3 && ms >= when[shot]) {
        lv_refr_now(NULL);
        lv_img_dsc_t *sn = snap_take(BS.scr);
        if (sn) {
            printf("\nSNAP %d %d %d\n", 20 + shot, (int)sn->header.w, (int)sn->header.h);
            static unsigned char out[480 * 2 * 4 / 3 + 8];
            for (int r = 0; r < (int)sn->header.h; r++) {
                size_t olen = 0;
                mbedtls_base64_encode(out, sizeof(out), &olen, sn->data + r * sn->header.w * 2, sn->header.w * 2);
                out[olen] = 0;
                printf("R%d %s\n", r, out);
            }
            printf("SNAPEND %d\n", 20 + shot);
            
        }
        shot++;
    }
#endif
    for (int i = 0; i < 55; i++) lv_obj_set_style_bg_color(BS.cells[i], i < shown ? C_PINK : C_LINE, 0);
    if ((all_ok_at && ms >= 4800 && ms - all_ok_at > 600 && shown >= 55) || ms >= BOOT_MAX_MS) boot_close();
}

static void build_boot(void)
{
    BS.t0 = esp_timer_get_time() / 1000;
    BS.scr = rect(lv_layer_top(), 0, 0, W, H, C_BG);
    lv_obj_add_flag(BS.scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(BS.scr, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(BS.scr, boot_tap_cb, LV_EVENT_CLICKED, NULL);
    canvas_create(&BS.mos, BS.scr, 0, 0, W, H);
    // Zielbild: ein Band oben, ganz oben dicht und hell, nach unten ausfransend; darunter der Text
    for (int r = 0; r < 40; r++)
        for (int c = 0; c < 60; c++) {
            uint32_t h = hash2(c + 101, r + 37);
            float x = c / 59.0f, y = r / (float)BOOT_BAND; // y: 0 = oben … 1 = Unterkante des Bands
            float dens = r > BOOT_BAND ? 0 : 1.25f - y * 1.25f + 0.12f * sinf(x * 9.0f + 1.3f);
            int v = 0;
            if ((h % 1000) / 1000.0f < dens) v = 0x14 + (int)((h >> 10) % 0x30) + (int)((1 - y) * 0x24);
            if (v && (h % 61) == 0) v = 255; // vereinzelte pinke Zellen
            BS.target[r * 60 + c] = v;
        }
    BS.sub = cap(BS.scr, W, "BITCOIN DISPLAY", 'l', MX, 104);
    BS.word = tight(txt(BS.scr, W, &font_g50, C_TEXT, "Satoshi Deck", 'l', MX - 2, 158), 2);
    BS.ver = txt(BS.scr, W, &font_m12, C_TEXT3, "", 'l', MX, 182);
    lv_label_set_text_fmt(BS.ver, "v.%s", deck_version());
    static const char *names[5] = {"DISPLAY", "WLAN", "BITAXE", "KURS", "BLOCK"};
    for (int i = 0; i < 5; i++) {
        int y = 212 + i * 16;
        BS.row_d[i] = rect(BS.scr, MX, y - 7, 5, 5, C_LINE);
        lv_obj_set_style_radius(BS.row_d[i], 1, 0);
        cap(BS.scr, W, names[i], 'l', MX + 14, y);
        BS.row_v[i] = txt(BS.scr, W, &font_m12, C_TEXT3, "", 'l', MX + 90, y);
    }
    for (int i = 0; i < 55; i++) {
        BS.cells[i] = rect(BS.scr, MX + i * 8, 302, 6, 4, C_LINE);
        lv_obj_set_style_radius(BS.cells[i], 1, 0);
    }
    lv_obj_set_style_text_opa(BS.word, 0, 0);
    lv_obj_set_style_text_opa(BS.sub, 0, 0);
    lv_obj_set_style_text_opa(BS.ver, 0, 0);
    BS.tm = lv_timer_create(boot_tick, 33, NULL);
}

void ui_init(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, &font_m14, 0);
    lv_obj_set_style_text_color(scr, C_TEXT, 0);

#ifdef CONFIG_DECK_SNAPSHOT
    s_snap_buf = heap_caps_malloc(W * H * 2, MALLOC_CAP_SPIRAM);
#endif
    lv_obj_t *tiles[N_TILES];
    for (int i = 0; i < N_TILES; i++) {
        tiles[i] = rect(scr, 0, 0, W, H, C_BG);
        lv_obj_clear_flag(tiles[i], LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(tiles[i], card_event_cb, LV_EVENT_ALL, NULL);
        s_card[i] = tiles[i];
        if (i) lv_obj_add_flag(tiles[i], LV_OBJ_FLAG_HIDDEN);
    }
    build_price(tiles[0]);
    build_dual(tiles[1]);
    build_chart(tiles[2]);
    build_block(tiles[3]);
    build_newblock(tiles[CARD_NEWBLOCK]);
    build_halving(tiles[5]);
    build_fees(tiles[6]);
    build_difficulty(tiles[7]);
    build_clock(tiles[8]);
    build_weather(tiles[CARD_WEATHER]);
    build_miner(tiles[10]);
    build_cost(tiles[11]);
    build_lottery(tiles[12]);
    build_news(tiles[CARD_NEWS]);
    int first = g_settings.card_order[0]; // erste Karte der Reihenfolge
    car_jump(card_on(first) ? first : card_step(first, 1), false);
    build_overlays();
    toast_create();
    build_update_screen();
    build_boot();

    lv_timer_create(ui_timer_cb, 500, NULL);
    lv_timer_create(lottery_tick, 120, NULL);
#if defined(CONFIG_DECK_SNAPSHOT) || defined(CONFIG_DECK_PERF_BENCH)
    lv_timer_create(bench_cb, 50, NULL);
#endif
}
