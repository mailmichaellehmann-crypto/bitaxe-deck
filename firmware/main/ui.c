// Bitaxe Deck – Oberfläche für 480x320 (LVGL 8.4) im Strike-Stil
// Karten (nach links/rechts wischen):
//  0 Preis · 1 USD & EUR · 2 Chart · 3 Block · 4 Gebühren · 5 Difficulty · 6 Uhr · 7 Bitaxe · 8 Share-Lotterie
// Gestaltung: tiefschwarzer Grund, gedämpftes Weiß, Geist für große Zahlen, Geist Mono für
// Beschriftungen, 1-px-Rahmen, Punktmatrix-Chart, Pixel-Mosaik; einzige Akzentfarbe: Pink #FFD4D4.

#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "data.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "net.h"
#include "settings.h"
#include "shares.h"
#include "ota.h"

LV_FONT_DECLARE(font_g20);
LV_FONT_DECLARE(font_g26);
LV_FONT_DECLARE(font_g36);
LV_FONT_DECLARE(font_g50);
LV_FONT_DECLARE(font_g72);  // nur Ziffern . , : + - %
LV_FONT_DECLARE(font_g96);  // nur Ziffern . , : + - %
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
#define N_TILES 9
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

static lv_obj_t *s_tv;

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
static struct { lv_obj_t *time, *date; } G;                                                  // Uhr
static struct { lv_obj_t *title, *status, *dot, *hash, *unit, *legend, *cells[55], *val[3]; } HB; // Bitaxe

// ---- 0 Preis: großer Kurs, darunter der Kursverlauf als Pixel-Mosaik
#define MOS_CELL 8
static void mosaic_draw(const float *v, int n, double mn, double mx)
{
    canvas_t *c = &A.mos;
    lv_canvas_fill_bg(c->cv, C_BG, LV_OPA_COVER);
    int cols = c->w / MOS_CELL, rows = c->h / MOS_CELL;
    double range = mx - mn > 1e-9 ? mx - mn : 1;
    for (int col = 0; col < cols; col++) {
        float val = sample(v, n, (float)col / (cols - 1));
        int hcells = 1 + (int)((val - mn) / range * (rows - 1) + 0.5);
        for (int r = 0; r < rows; r++) {
            int from_bottom = rows - 1 - r;
            int x = col * MOS_CELL, y = r * MOS_CELL;
            uint32_t h = hash2(col, r);
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
    lv_obj_t *tag, *tag_lbl, *hi, *lo, *xline, *xdot, *tip, *tip_p, *tip_t, *hit;
    bool scrub;
} K;

static int val_y(double v) { return (int)((1.0 - (v - K.mn) / (K.mx - K.mn)) * (CH_H - 1) + 0.5); }

static void chart_draw(void)
{
    canvas_t *c = &CC;
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
    int idx = (x * (K.n - 1) + (CH_W - 1) / 2) / (CH_W - 1);
    int px = idx * (CH_W - 1) / (K.n - 1), py = val_y(K.v[idx]);
    lv_obj_set_pos(K.xline, CH_X + px, CH_Y);
    lv_obj_set_pos(K.xdot, CH_X + px - 5, CH_Y + py - 5);
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
        K.scrub = true;
        lv_obj_clear_flag(s_tv, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(K.xline, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(K.xdot, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(K.tip, LV_OBJ_FLAG_HIDDEN);
        scrub_update();
    } else if (code == LV_EVENT_PRESSING && K.scrub) {
        scrub_update();
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && K.scrub) {
        K.scrub = false;
        lv_obj_add_flag(s_tv, LV_OBJ_FLAG_SCROLLABLE);
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
    K.tag = rect(t, 0, 0, 90, 22, C_PINK);
    lv_obj_set_style_radius(K.tag, LV_RADIUS_CIRCLE, 0);
    K.tag_lbl = txt(K.tag, 90, &font_m14, C_BG, "", 'c', 45, 16);
    lv_obj_add_flag(K.tag, LV_OBJ_FLAG_HIDDEN);
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

// ---- 6 Uhr
static void build_clock(lv_obj_t *t)
{
    G.time = tight(txt(t, W, &font_g120, C_TEXT, "--:--", 'c', W / 2, 180), 4);
    G.date = txt(t, W, &font_m16, C_TEXT2, "", 'c', W / 2, 226);
    lv_obj_set_style_text_letter_space(G.date, 2, 0);
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


// ---- 8 Share-Lotterie: jeder gefundene Hash fällt als Punkt in ein Histogramm (log. Skala).
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
    bool visible = lv_tileview_get_tile_act(s_tv) == L.tile;
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

static uint32_t v_price, v_chart, v_net, v_miner;
static void check_price_alarm(double px);
static void toast_show(const char *title, const char *msg, int ms);

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
        lv_obj_update_layout(lbl[k]);
        int lw = lv_obj_get_width(lbl[k]);
        int lx = px - lw / 2;
        if (lx < CH_X) lx = CH_X;
        if (lx + lw > CH_X + CH_W - 100) lx = CH_X + CH_W - 100 - lw;
        int ly = k == 0 ? py - 22 : py + 10;
        if (ly < CH_Y - 8) ly = CH_Y - 8;
        if (ly > CH_Y + CH_H - 14) ly = CH_Y + CH_H - 14;
        lv_obj_align(lbl[k], LV_ALIGN_TOP_LEFT, lx, ly);
    }
    // Preis-Pille am letzten Wert
    fmt_de(n, sizeof(n), d->chart_last, 0);
    lv_label_set_text(K.tag_lbl, n);
    int ly = CH_Y + val_y(K.v[K.n - 1]);
    lv_obj_set_pos(K.tag, CH_X + CH_W - 90 - 14, ly < CH_Y + 30 ? ly + 10 : ly - 32);
    lv_obj_clear_flag(K.tag, LV_OBJ_FLAG_HIDDEN);
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
    static uint32_t last_height = 0;
    if (last_height && d->height > last_height && g_settings.block_note && d->blocks_n) {
        char t2[64];
        snprintf(t2, sizeof(t2), "%u · %s", (unsigned)d->height, d->blocks[0].pool);
        toast_show("NEUER BLOCK", t2, 6000);
    }
    last_height = d->height;
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
    if (strcmp(lv_label_get_text(G.date), full)) lv_label_set_text(G.date, full);
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

// Preisalarm: einmal pro Überschreiten, erneut erst nach 0,5 % Rückkehr
static void check_price_alarm(double px)
{
    static int state = 0;
    if (!g_settings.alarm_on || px <= 0) { state = 0; return; }
    char n[24], b[64];
    if (g_settings.alarm_hi > 0 && px >= g_settings.alarm_hi && state != 1) {
        state = 1;
        fmt_de(n, sizeof(n), g_settings.alarm_hi, 0);
        snprintf(b, sizeof(b), "Kurs über %s %s", n, cur_sym());
        toast_show("PREISALARM", b, 30000);
        lv_obj_set_tile_id(s_tv, 0, 0, LV_ANIM_ON);
    } else if (g_settings.alarm_lo > 0 && px <= g_settings.alarm_lo && state != -1) {
        state = -1;
        fmt_de(n, sizeof(n), g_settings.alarm_lo, 0);
        snprintf(b, sizeof(b), "Kurs unter %s %s", n, cur_sym());
        toast_show("PREISALARM", b, 30000);
        lv_obj_set_tile_id(s_tv, 0, 0, LV_ANIM_ON);
    } else if (state == 1 && px < g_settings.alarm_hi * 0.995) state = 0;
    else if (state == -1 && px > g_settings.alarm_lo * 1.005) state = 0;
}

// ------------------------------------------------------------------ Einstellungen & Kartenmenü

static lv_obj_t *s_cfg, *s_menu, *s_qr, *s_cfg_l1, *s_cfg_l2, *s_slider[2], *s_menu_tile[9];
static lv_obj_t *s_rot_val, *s_range_val, *s_cfg_ver, *s_upd_pill, *s_upd_lbl;

static void upd_cb(lv_event_t *e) { ota_install_now(); }

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
    if (o.state == OTA_AVAILABLE) {
        lv_label_set_text_fmt(s_upd_lbl, "UPDATE %s →", o.latest);
        lv_obj_clear_flag(s_upd_pill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_cfg_ver, LV_OBJ_FLAG_HIDDEN);
        if (strcmp(announced, o.latest) && !g_settings.auto_update) {
            strlcpy(announced, o.latest, sizeof(announced));
            char b[64];
            snprintf(b, sizeof(b), "Version %s – in den Einstellungen", o.latest);
            toast_show("UPDATE VERFÜGBAR", b, 8000);
        }
    } else if (o.state != OTA_INSTALLING) {
        lv_obj_add_flag(s_upd_pill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_cfg_ver, LV_OBJ_FLAG_HIDDEN);
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
    if (s) txt(b, d, f, C_TEXT, s, 'c', d / 2, d / 2 + ascent(f) / 2 - 1);
    return b;
}

static lv_obj_t *stepper_tile(lv_obj_t *p, int x, int y, const char *title, int code_minus)
{
    lv_obj_t *pn = panel(p, x, y, 144, 78);
    cap(pn, 144, title, 'l', 12, 24);
    lv_obj_t *v = txt(pn, 144, &font_g20, C_TEXT, "", 'l', 12, 60);
    lv_obj_t *m = round_btn(pn, 144 - 12 - 26 - 6 - 26, 39, 26, "–", &font_m16);
    lv_obj_add_event_cb(m, step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)code_minus);
    lv_obj_t *pl = round_btn(pn, 144 - 12 - 26, 39, 26, "+", &font_m16);
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

static void open_menu(void)
{
    lv_obj_t *act = lv_tileview_get_tile_act(s_tv);
    for (int i = 0; i < 9; i++)
        lv_obj_set_style_border_color(s_menu_tile[i], i < N_TILES && lv_obj_get_child(s_tv, i) == act ? C_PINK : C_LINE, 0);
    lv_obj_add_flag(s_cfg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
}

static void to_menu_cb(lv_event_t *e) { open_menu(); }
static void to_cfg_cb(lv_event_t *e) { open_cfg(); }

static void menu_item_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= N_TILES) { open_cfg(); return; }
    overlay_close();
    lv_obj_set_tile_id(s_tv, idx, 0, LV_ANIM_OFF);
}

static void long_press_cb(lv_event_t *e) { open_cfg(); }

// Untere Leiste: runder Knopf links, Helligkeit, Schließen
static void bottom_bar(lv_obj_t *p, int si, bool gear)
{
    lv_obj_t *l = round_btn(p, MX, 280, 34, gear ? LV_SYMBOL_SETTINGS : NULL, &font_m16);
    if (!gear) {
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++) {
                lv_obj_t *q = rect(l, 10 + c * 5, 10 + r * 5, 3, 3, C_TEXT);
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
    s_cfg_ver = txt(s_cfg, W, &font_m12, C_TEXT3, "", 'r', W - MX, 28);
    lv_label_set_text_fmt(s_cfg_ver, "v.%s", deck_version());
    s_upd_pill = pill(s_cfg, W - MX - 150, 12, 150, 26, "", &s_upd_lbl);
    lv_obj_set_style_bg_color(s_upd_pill, C_PINK, 0);
    lv_obj_set_style_border_width(s_upd_pill, 0, 0);
    lv_obj_set_style_text_color(s_upd_lbl, C_BG, 0);
    lv_obj_add_event_cb(s_upd_pill, upd_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_upd_pill, LV_OBJ_FLAG_HIDDEN);
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
    static const char *names[9] = {"Preis", "USD / EUR", "Chart", "Block", "Gebühren", "Difficulty", "Uhr", "Bitaxe", "Lotterie"};
    static const char *nums[9] = {"01", "02", "03", "04", "05", "06", "07", "08", "09"};
    for (int i = 0; i < 9; i++) {
        lv_obj_t *b = panel(s_menu, xs[i % 3], 14 + (i / 3) * 86, 144, 80);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_style_bg_color(b, lv_color_hex(0x111111), LV_STATE_PRESSED);
        txt(b, 144, &font_m12, C_TEXT3, nums[i], 'l', 12, 24);
        txt(b, 144, &font_g20, C_TEXT, names[i], 'l', 12, 64);
        lv_obj_add_event_cb(b, menu_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_menu_tile[i] = b;
    }
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
    if (g_settings.rotate_s && !overlay && !K.scrub && idle >= g_settings.rotate_s * 1000u && now - last_rot >= g_settings.rotate_s * 1000) {
        last_rot = now;
        lv_obj_t *act = lv_tileview_get_tile_act(s_tv);
        int idx = 0;
        for (int i = 0; i < N_TILES; i++) if (lv_obj_get_child(s_tv, i) == act) idx = i;
        lv_obj_set_tile_id(s_tv, (idx + 1) % N_TILES, 0, LV_ANIM_ON);
    }
    static int cur = -1;
    int want = g_settings.brightness;
    if (g_settings.night_on && net_time_valid() && idle > 30000 && !s_toast_until) {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        int f = g_settings.night_from, to = g_settings.night_to, h = tm.tm_hour;
        bool night = f <= to ? (h >= f && h < to) : (h >= f || h < to);
        if (night) want = 3;
    }
    if (want != cur) { cur = want; board_set_brightness(want); }
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
// Screenshot-Modus: nach dem Laden der Live-Daten jede Karte (+ Einstellungen, Menü) über USB ausgeben
static void bench_cb(lv_timer_t *t)
{
    static int idx = -1, wait = 0, row = -1, ticks = 0;
    static lv_img_dsc_t *snap;
    ticks++;
    if (idx >= N_TILES + 2) return;
    if (idx < 0) {
        deck_data_t *d = data_get();
        bool ready = d->price_ok && d->net_ok && d->chart_ok;
        if (!ready && ticks < 900) return;
        idx = 0;
        lv_obj_set_tile_id(s_tv, idx, 0, LV_ANIM_OFF);
        wait = 0;
        return;
    }
    if (row < 0) {
        if (++wait < 20) return;
        snap = lv_snapshot_take(lv_scr_act(), LV_IMG_CF_TRUE_COLOR);
        if (!snap) { printf("SNAPERR %d\n", idx); idx++; return; }
        printf("\nSNAP %d %d %d\n", idx, (int)snap->header.w, (int)snap->header.h);
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
        printf("SNAPEND %d\n", idx);
        lv_snapshot_free(snap);
        snap = NULL;
        row = -1;
        wait = 0;
        ++idx;
        if (idx < N_TILES) lv_obj_set_tile_id(s_tv, idx, 0, LV_ANIM_OFF);
        else if (idx == N_TILES) open_cfg();
        else if (idx == N_TILES + 1) open_menu();
        else overlay_close();
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
            if (idx < N_TILES) lv_obj_set_tile_id(s_tv, idx, 0, LV_ANIM_OFF);
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
        if (k + dir < 0 || k + dir >= N_TILES) dir = -dir;
        k += dir;
        lv_obj_set_tile_id(s_tv, k, 0, LV_ANIM_ON);
    }
}
#endif

void ui_init(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, &font_m14, 0);
    lv_obj_set_style_text_color(scr, C_TEXT, 0);

    s_tv = lv_tileview_create(scr);
    lv_obj_set_pos(s_tv, 0, 0);
    lv_obj_set_size(s_tv, W, H);
    lv_obj_set_style_bg_color(s_tv, C_BG, 0);
    lv_obj_set_style_bg_opa(s_tv, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_tv, LV_SCROLLBAR_MODE_OFF);
    lv_obj_t *tiles[N_TILES];
    for (int i = 0; i < N_TILES; i++) {
        lv_dir_t dir = (i > 0 ? LV_DIR_LEFT : 0) | (i < N_TILES - 1 ? LV_DIR_RIGHT : 0);
        tiles[i] = lv_tileview_add_tile(s_tv, i, 0, dir);
        lv_obj_set_scrollbar_mode(tiles[i], LV_SCROLLBAR_MODE_OFF);
    }
    build_price(tiles[0]);
    build_dual(tiles[1]);
    build_chart(tiles[2]);
    build_block(tiles[3]);
    build_fees(tiles[4]);
    build_difficulty(tiles[5]);
    build_clock(tiles[6]);
    build_miner(tiles[7]);
    build_lottery(tiles[8]);
    for (int i = 0; i < N_TILES; i++) lv_obj_add_event_cb(tiles[i], long_press_cb, LV_EVENT_LONG_PRESSED, NULL);
    build_overlays();
    toast_create();
    build_update_screen();

    lv_timer_create(ui_timer_cb, 500, NULL);
    lv_timer_create(lottery_tick, 120, NULL);
#if defined(CONFIG_DECK_SNAPSHOT) || defined(CONFIG_DECK_PERF_BENCH)
    lv_timer_create(bench_cb, 50, NULL);
#endif
}
