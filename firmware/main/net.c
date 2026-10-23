// WLAN, Zeit (SNTP) und Setup-Portal
// Ohne gespeichertes WLAN (oder wenn die Verbindung nicht klappt) öffnet das Gerät
// einen eigenen Access-Point. Unter http://192.168.4.1 trägt man WLAN, Bitaxe-IP und Strompreis ein.
// Im Heimnetz ist dieselbe Seite unter der IP des Geräts erreichbar.

#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "data.h"
#include "settings.h"
#include "ota.h"

static const char *TAG = "net";

static volatile net_state_t s_state = NET_CONNECTING;
static esp_netif_t *s_sta, *s_ap;
static char s_ip[16] = "";
static bool s_portal_on = false;
static esp_timer_handle_t s_fallback_timer;
static char s_scan_html[2048] = "";

// ------------------------------------------------------------------ Portal (HTTP)

// Einstellungsseite im Strike-Stil: schwarz, gedämpftes Weiß, Mono-Beschriftungen, feine Linien,
// Pixel-Mosaik oben, Akzent Pink #FFD4D4. Keine externen Schriften (läuft auch im Setup-WLAN).
static const char *PAGE_HEAD =
    "<!doctype html><html lang=de><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'><title>Satoshi Deck</title><style>"
    ":root{--fg:#ececec;--mut:#9a9a9a;--dim:#5e5e5e;--line:#262626;--pink:#ffd4d4;"
    "--sans:ui-sans-serif,-apple-system,'Helvetica Neue',Inter,Arial,sans-serif;"
    "--mono:ui-monospace,'SF Mono',Menlo,Consolas,monospace}"
    "*{box-sizing:border-box}"
    "body{margin:0;background:#000;color:var(--fg);font-family:var(--sans);-webkit-font-smoothing:antialiased}"
    "#mos{display:grid;grid-template-columns:repeat(48,1fr);gap:2px;padding:2px;height:64px;"
    "-webkit-mask-image:linear-gradient(90deg,transparent,#000 70%);mask-image:linear-gradient(90deg,transparent,#000 70%)}"
    "#mos i{display:block;border-radius:1px}"
    "main{max-width:440px;margin:0 auto;padding:28px 20px 48px}"
    "h1{font-weight:500;font-size:44px;letter-spacing:-1.2px;line-height:1;margin:8px 0 14px}"
    "p{color:var(--mut);font-size:15px;line-height:1.5;margin:0 0 8px}"
    ".sec{font-family:var(--mono);font-size:11px;letter-spacing:1.5px;color:var(--mut);text-transform:uppercase;"
    "margin:30px 0 4px;padding-bottom:10px;border-bottom:1px solid var(--line)}"
    "label{display:block;font-family:var(--mono);font-size:11px;letter-spacing:1px;color:var(--dim);"
    "text-transform:uppercase;margin:16px 0 7px}"
    "input,select{width:100%;padding:12px 14px;border-radius:10px;font:16px var(--sans);color:var(--fg);"
    "background:#000;border:1px solid var(--line);-webkit-appearance:none;appearance:none}"
    "input:focus,select:focus{outline:none;border-color:var(--fg)}"
    "input::placeholder{color:var(--dim)}"
    ".cards{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:16px}"
    ".cards label{display:flex;align-items:center;gap:10px;margin:0;padding:11px 14px;border:1px solid var(--line);"
    "border-radius:10px;font:15px var(--sans);letter-spacing:0;text-transform:none;color:var(--fg);cursor:pointer}"
    ".cards input{flex:none;width:16px;height:16px;padding:0;border-radius:4px}"
    ".cards input:checked{background:var(--pink);border-color:var(--pink)}"
    ".row{display:flex;gap:10px}"
    ".ord{grid-template-columns:1fr}.ci{display:flex;gap:6px}.ci label{flex:1}"
    ".ci i{font:12px var(--mono);font-style:normal;color:var(--dim);width:18px}"
    ".ci button{margin:0;padding:0 15px;border:1px solid var(--line);border-radius:10px;background:#000;"
    "color:var(--fg);font:15px var(--mono)}"
    "button{margin-top:32px;padding:13px 26px;border:0;border-radius:999px;font:500 14px var(--mono);"
    "color:#000;background:var(--fg);cursor:pointer}"
    "button:active{background:var(--pink)}"
    ".acc{color:var(--pink)}small{color:var(--dim);font-family:var(--mono);font-size:12px}"
    "</style></head><body><div id=mos></div><main>"
    "<script>(function(){var m=document.getElementById('mos');for(var i=0;i<48*6;i++){var x=i%48,"
    "r=Math.random(),g=Math.round(14+r*r*58*(x/48+.3));var e=document.createElement('i');"
    "e.style.background=(i==48*6-1)?'#ffd4d4':'rgb('+g+','+g+','+g+')';m.appendChild(e)}})()</script>";

static esp_err_t root_get(httpd_req_t *req)
{
    char *html = malloc(12288);
    if (!html) return ESP_FAIL;
    ota_status_t o;
    ota_get(&o);
    char ota_line[96], ota_btn[160] = "";
    switch (o.state) {
    case OTA_AVAILABLE:
        snprintf(ota_line, sizeof(ota_line), "<span class=acc>Version %s verfügbar</span>", o.latest);
        snprintf(ota_btn, sizeof(ota_btn), "<button name=a value=install style='margin-top:12px'>%s installieren &rarr;</button>", o.latest);
        break;
    case OTA_UPTODATE: snprintf(ota_line, sizeof(ota_line), "aktuell"); break;
    case OTA_CHECKING: snprintf(ota_line, sizeof(ota_line), "suche …"); break;
    case OTA_INSTALLING: snprintf(ota_line, sizeof(ota_line), "<span class=acc>installiere … %d %%</span>", o.progress); break;
    case OTA_FAILED: snprintf(ota_line, sizeof(ota_line), "%s", o.error); break;
    default: snprintf(ota_line, sizeof(ota_line), "noch nicht geprüft"); break;
    }
    // Karten in der eingestellten Reihenfolge, mit ↑ ↓ verschiebbar (schreibt das versteckte Feld „order“)
    char cards[3600], order[48];
    int cl = 0, ol = 0;
    for (int p = 0; p < DECK_CARDS; p++) {
        int i = g_settings.card_order[p];
        cl += snprintf(cards + cl, sizeof(cards) - cl,
                       "<div class=ci data-id=%d><label><i>%02d</i><input type=checkbox name=c%d value=1%s>%s</label>"
                       "<button type=button onclick=mv(this,-1)>&uarr;</button><button type=button onclick=mv(this,1)>&darr;</button></div>",
                       i, p + 1, i, (g_settings.card_mask >> i) & 1 ? " checked" : "", DECK_CARD_NAMES[i]);
        ol += snprintf(order + ol, sizeof(order) - ol, "%s%d", p ? "," : "", i);
    }
    char ncol[400];
    int nl = 0;
    for (int i = 0; i <= NIGHT_COLORS; i++)
        nl += snprintf(ncol + nl, sizeof(ncol) - nl, "<option value=%d%s>%s</option>", i,
                       g_settings.night_color == i ? " selected" : "", NIGHT_COLOR_NAMES[i]);
    // Wetter: eingetragener Ort und was das Display dazu gefunden hat
    char wx_line[200];
    data_lock();
    const weather_t *w = &data_get()->wx;
    if (!g_settings.wx_place[0]) snprintf(wx_line, sizeof(wx_line), "Name, Postleitzahl oder Koordinaten (Breite, Länge)");
    else if (w->place[0] && !w->err[0]) snprintf(wx_line, sizeof(wx_line), "Gefunden: <span class=acc>%s</span>%s%s", w->place, w->region[0] ? " · " : "", w->region);
    else if (w->err[0]) snprintf(wx_line, sizeof(wx_line), "<span class=acc>%s</span>", w->err);
    else snprintf(wx_line, sizeof(wx_line), "wird gesucht …");
    data_unlock();
    char ct[16];
    float c = g_settings.kwh_price * 100.0f;
    snprintf(ct, sizeof(ct), c == (int)c ? "%.0f" : "%.1f", c);
    for (char *q = ct; *q; q++) if (*q == '.') *q = ',';
    snprintf(html, 12288,
             "%s<h1>Satoshi Deck</h1><p>Einstellungen für dein Display. <span class=acc>Speichern startet es neu.</span></p>"
             "<form method=post action=/save>"
             "<div class=sec>Verbindung</div>"
             "<label>WLAN-Name</label><input name=ssid list=nets value='%s' required maxlength=32 autocomplete=off>"
             "<datalist id=nets>%s</datalist>"
             "<label>WLAN-Passwort</label><input name=pass type=password maxlength=64 placeholder='%s'>"
             "<label>Bitaxe – IP-Adresse</label><input name=bitaxe value='%s' placeholder='192.168.1.42' maxlength=63>"
             "<div class=sec>Preisalarm</div>"
             "<div class=row><div style='flex:1'><label>Oberhalb (%s)</label><input name=al_hi value='%.0f' inputmode=numeric></div>"
             "<div style='flex:1'><label>Unterhalb (%s)</label><input name=al_lo value='%.0f' inputmode=numeric></div></div>"
             "<div class=sec>Anzeige</div>"
             "<div class=row><div style='flex:1'><label>Nachtmodus ab</label><input name=n_from value='%d' inputmode=numeric></div>"
             "<div style='flex:1'><label>bis (Uhr)</label><input name=n_to value='%d' inputmode=numeric></div></div>"
             "<label>Farbe im Nachtmodus</label><select name=ncol>%s</select>"
             "<label>Uhr</label><select name=clk><option value=3 %s>LED-Punktanzeige</option><option value=0 %s>Klassisch</option></select>"
             "<label>Chart-Fläche</label><select name=cstyle>"
             "<option value=0 %s>Punkte</option><option value=1 %s>Ohne Verlauf</option><option value=2 %s>Pixel</option></select>"
             "<label>Automatische Updates</label><select name=aupd>"
             "<option value=1 %s>Ein – nachts bzw. wenn niemand das Display benutzt</option><option value=0 %s>Aus</option></select>"
             "<div class=sec>Mining-Kosten</div>"
             "<label>Strompreis (ct/kWh)</label><input name=kwh_ct value='%s' inputmode=decimal maxlength=6>"
             "<div class=cards style='grid-template-columns:1fr'><label><input type=checkbox name=ereset value=1>"
             "Stromzähler zurücksetzen</label></div>"
             "<div class=sec>Wetter</div>"
             "<label>Ort</label><input name=wx value='%s' placeholder='z. B. Berlin, 80331 oder Frankfurt, DE' maxlength=47>"
             "<p><small>%s</small></p>"
             "<div class=sec>Karten</div><p><small>Reihenfolge mit &uarr; &darr; – am Display: im Kartenmenü eine Kachel halten und ziehen</small></p>"
             "<input type=hidden name=cards value=1><input type=hidden name=order id=order value='%s'><div class='cards ord' id=cl>%s</div>"
             "<script>function mv(b,d){var r=b.parentNode,l=r.parentNode,o=d<0?r.previousElementSibling:r.nextElementSibling;"
             "if(!o)return;l.insertBefore(d<0?r:o,d<0?o:r);var a=[];l.querySelectorAll('.ci').forEach(function(e,k){"
             "a.push(e.dataset.id);e.querySelector('i').textContent=(k<9?'0':'')+(k+1)});"
             "document.getElementById('order').value=a.join(',')}</script>"
             "<button>Speichern &rarr;</button></form>"
             "<div class=sec>Firmware</div>"
             "<p>Installiert: <b>%s</b> · %s</p>"
             "<form method=post action=/ota style='display:flex;gap:10px'>"
             "<button name=a value=check style='margin-top:12px;background:#000;color:var(--fg);border:1px solid var(--line)'>Nach Updates suchen</button>"
             "%s</form>"
             "<p style='margin-top:28px'><small>0 = Alarm aus · IP des Bitaxe steht im AxeOS-Webinterface · "
             "Wetterdaten: <a href='https://open-meteo.com/'>Open-Meteo.com</a> (CC BY 4.0)</small></p>"
             "</main></body></html>",
             PAGE_HEAD, g_settings.wifi_ssid, s_scan_html,
             g_settings.wifi_pass[0] ? "unverändert lassen" : "", g_settings.bitaxe_host[0],
             settings_cur(), g_settings.alarm_hi, settings_cur(), g_settings.alarm_lo, g_settings.night_from, g_settings.night_to, ncol,
             g_settings.clock_style == 3 ? "selected" : "", g_settings.clock_style == 0 ? "selected" : "",
             g_settings.chart_style == 0 ? "selected" : "", g_settings.chart_style == 1 ? "selected" : "",
             g_settings.chart_style == 2 ? "selected" : "",
             g_settings.auto_update ? "selected" : "", g_settings.auto_update ? "" : "selected", ct, g_settings.wx_place, wx_line, order, cards,
             deck_version(), ota_line, ota_btn);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, html);
    free(html);
    return ESP_OK;
}

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') *o++ = ' ';
        else if (*s == '%' && s[1] && s[2]) {
            char hex[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else *o++ = *s;
    }
    *o = 0;
}

static bool form_field(const char *body, const char *key, char *out, size_t len)
{
    if (httpd_query_key_value(body, key, out, len) != ESP_OK) return false;
    url_decode(out);
    return true;
}

static void restart_cb(void *arg)
{
    data_energy_save();
    esp_restart();
}

static esp_err_t save_post(httpd_req_t *req)
{
    char body[1536] = {0};
    int total = 0;
    while (total < (int)sizeof(body) - 1) {
        int r = httpd_req_recv(req, body + total, sizeof(body) - 1 - total);
        if (r <= 0) break;
        total += r;
        if (total >= req->content_len) break;
    }
    char v[96];
    if (form_field(body, "ssid", v, sizeof(v)) && v[0]) strlcpy(g_settings.wifi_ssid, v, sizeof(g_settings.wifi_ssid));
    if (form_field(body, "pass", v, sizeof(v)) && v[0]) strlcpy(g_settings.wifi_pass, v, sizeof(g_settings.wifi_pass));
    static const char *keys[DECK_MAX_MINERS] = {"bitaxe"};
    for (int i = 0; i < DECK_MAX_MINERS; i++) {
        if (!form_field(body, keys[i], v, sizeof(v))) continue;
        char *p = v; // "http://" und Schrägstriche entfernen
        if (!strncmp(p, "http://", 7)) p += 7;
        p[strcspn(p, "/ ")] = 0;
        strlcpy(g_settings.bitaxe_host[i], p, sizeof(g_settings.bitaxe_host[i]));
    }
    if (form_field(body, "al_hi", v, sizeof(v))) g_settings.alarm_hi = strtof(v, NULL);
    if (form_field(body, "al_lo", v, sizeof(v))) g_settings.alarm_lo = strtof(v, NULL);
    if (form_field(body, "n_from", v, sizeof(v))) { int h = atoi(v); if (h >= 0 && h < 24) g_settings.night_from = h; }
    if (form_field(body, "n_to", v, sizeof(v))) { int h = atoi(v); if (h >= 0 && h < 24) g_settings.night_to = h; }
    if (form_field(body, "wx", v, sizeof(v))) {
        char *p = v; // Leerzeichen am Rand und Zeichen entfernen, die die Seite stören
        while (*p == ' ') p++;
        for (char *q = p; *q; q++) if (strchr("<>'\"&", *q)) *q = ' ';
        for (int n = strlen(p); n > 0 && p[n - 1] == ' '; n--) p[n - 1] = 0;
        strlcpy(g_settings.wx_place, p, sizeof(g_settings.wx_place));
    }
    if (form_field(body, "clk", v, sizeof(v))) g_settings.clock_style = atoi(v) == 0 ? 0 : 3;
    if (form_field(body, "ncol", v, sizeof(v))) { int c = atoi(v); if (c >= 0 && c <= NIGHT_COLORS) g_settings.night_color = c; }
    if (form_field(body, "aupd", v, sizeof(v))) g_settings.auto_update = atoi(v) != 0;
    if (form_field(body, "cstyle", v, sizeof(v))) { int c = atoi(v); if (c >= 0 && c <= 2) g_settings.chart_style = c; }
    if (form_field(body, "cards", v, sizeof(v))) { // nicht angehakte Kästchen werden nicht mitgeschickt
        uint16_t m = 0;
        for (int i = 0; i < DECK_CARDS; i++) {
            char k[4];
            snprintf(k, sizeof(k), "c%d", i);
            if (form_field(body, k, v, sizeof(v))) m |= 1u << i;
        }
        g_settings.card_mask = m ? m : DECK_CARDS_ALL; // mindestens eine Karte bleibt an
    }
    if (form_field(body, "order", v, sizeof(v)) && v[0]) { // z. B. „3,0,1,…“
        uint8_t o[DECK_CARDS];
        int n = 0;
        for (char *q = v, *e; *q && n < DECK_CARDS; q = e) {
            long k = strtol(q, &e, 10);
            if (e == q) break; // keine Zahl
            o[n++] = k >= 0 && k < DECK_CARDS ? (uint8_t)k : 0xFF;
            while (*e == ',' || *e == ' ') e++;
        }
        settings_order_fix(o, n);
        memcpy(g_settings.card_order, o, DECK_CARDS);
    }
    if (form_field(body, "kwh_ct", v, sizeof(v)) && v[0]) {
        for (char *c = v; *c; c++) if (*c == ',') *c = '.';
        float f = strtof(v, NULL);
        if (f >= 0 && f <= 500) g_settings.kwh_price = f / 100.0f;
    }
    if (form_field(body, "ereset", v, sizeof(v))) data_energy_reset();
    settings_save();
    ESP_LOGI(TAG, "Einstellungen gespeichert (SSID '%s', Bitaxe '%s')", g_settings.wifi_ssid, g_settings.bitaxe_host[0]);

    char html[3800];
    snprintf(html, sizeof(html), "%s<h1>Gespeichert.</h1><p>Das Display startet neu und verbindet sich mit <span class=acc>%s</span>.</p></main></body></html>",
             PAGE_HEAD, g_settings.wifi_ssid);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, html);

    const esp_timer_create_args_t a = {.callback = restart_cb, .name = "restart"};
    esp_timer_handle_t t;
    esp_timer_create(&a, &t);
    esp_timer_start_once(t, 1500 * 1000);
    return ESP_OK;
}

static esp_err_t ota_post(httpd_req_t *req)
{
    char body[64] = {0}, v[16];
    httpd_req_recv(req, body, sizeof(body) - 1);
    if (form_field(body, "a", v, sizeof(v))) {
        if (!strcmp(v, "install")) ota_install_now();
        else ota_check_now();
    }
    char html[3800];
    snprintf(html, sizeof(html), "%s<h1>%s</h1><p>%s</p><p><a class=acc href=/>Zurück &rarr;</a></p></main></body></html>", PAGE_HEAD,
             strcmp(v, "install") ? "Suche läuft." : "Update startet.",
             strcmp(v, "install") ? "Das Display fragt GitHub nach einer neuen Version. Seite gleich neu laden."
                                  : "Das Display lädt die neue Firmware und startet danach neu. Nicht ausschalten.");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, html);
    return ESP_OK;
}

static esp_err_t redirect_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static void start_http(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.stack_size = 8192;
    httpd_handle_t srv;
    if (httpd_start(&srv, &cfg) != ESP_OK) return;
    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get};
    httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = save_post};
    httpd_uri_t any = {.uri = "/*", .method = HTTP_GET, .handler = redirect_get};
    httpd_uri_t ota = {.uri = "/ota", .method = HTTP_POST, .handler = ota_post};
    httpd_register_uri_handler(srv, &ota);
    httpd_register_uri_handler(srv, &root);
    httpd_register_uri_handler(srv, &save);
    httpd_register_uri_handler(srv, &any);
}

// ------------------------------------------------------------------ WLAN

static void scan_networks(void)
{
    wifi_scan_config_t sc = {.show_hidden = false};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return;
    uint16_t n = 20;
    wifi_ap_record_t recs[20];
    esp_wifi_scan_get_ap_records(&n, recs);
    int off = 0;
    s_scan_html[0] = 0;
    for (int i = 0; i < n && off < (int)sizeof(s_scan_html) - 80; i++) {
        if (!recs[i].ssid[0] || strchr((char *)recs[i].ssid, '\'') || strchr((char *)recs[i].ssid, '<')) continue;
        off += snprintf(s_scan_html + off, sizeof(s_scan_html) - off, "<option value='%s'>", recs[i].ssid);
    }
}

static void start_portal(void)
{
    if (s_portal_on) return;
    s_portal_on = true;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, DECK_AP_SSID, sizeof(ap.ap.ssid));
    strlcpy((char *)ap.ap.password, DECK_AP_PASS, sizeof(ap.ap.password));
    ap.ap.ssid_len = strlen(DECK_AP_SSID);
    ap.ap.channel = 6;
    ap.ap.max_connection = 3;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    scan_networks();
    if (s_state != NET_CONNECTED) s_state = NET_PORTAL;
    ESP_LOGI(TAG, "Setup-Portal aktiv: WLAN '%s', http://192.168.4.1", DECK_AP_SSID);
}

static void portal_task(void *arg)
{
    start_portal();
    vTaskDelete(NULL);
}

static void fallback_cb(void *arg)
{
    if (s_state != NET_CONNECTED) xTaskCreate(portal_task, "portal", 6144, NULL, 3, NULL);
}

static esp_timer_handle_t s_reconnect_timer;
static void reconnect_cb(void *arg) { esp_wifi_connect(); }

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (settings_has_wifi()) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_state == NET_CONNECTED) s_state = s_portal_on ? NET_PORTAL : NET_CONNECTING;
        s_ip[0] = 0;
        if (settings_has_wifi()) {
            esp_timer_stop(s_reconnect_timer);
            esp_timer_start_once(s_reconnect_timer, (s_portal_on ? 10000 : 2000) * 1000ULL);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_state = NET_CONNECTED;
        ESP_LOGI(TAG, "Verbunden, IP %s", s_ip);
        if (s_portal_on) { // Access-Point wieder abschalten
            esp_wifi_set_mode(WIFI_MODE_STA);
            s_portal_on = false;
        }
    }
}

void net_init(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(s_sta, "satoshi-deck");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL);

    const esp_timer_create_args_t rc = {.callback = reconnect_cb, .name = "wifi_rc"};
    esp_timer_create(&rc, &s_reconnect_timer);

    esp_wifi_set_mode(WIFI_MODE_STA);
    if (settings_has_wifi()) {
        wifi_config_t sta = {0};
        strlcpy((char *)sta.sta.ssid, g_settings.wifi_ssid, sizeof(sta.sta.ssid));
        strlcpy((char *)sta.sta.password, g_settings.wifi_pass, sizeof(sta.sta.password));
        sta.sta.threshold.authmode = g_settings.wifi_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
        esp_wifi_set_config(WIFI_IF_STA, &sta);
    }
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    if (!settings_has_wifi()) {
        start_portal();
    } else {
        const esp_timer_create_args_t a = {.callback = fallback_cb, .name = "wifi_fb"};
        esp_timer_create(&a, &s_fallback_timer);
        esp_timer_start_once(s_fallback_timer, 25 * 1000 * 1000); // nach 25 s ohne WLAN: Portal
    }
    start_http();

    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp);
}

net_state_t net_state(void) { return s_state; }

bool net_time_valid(void) { return time(NULL) > 1700000000; }

void net_ip(char *out, int len)
{
    if (s_state == NET_CONNECTED) strlcpy(out, s_ip, len);
    else if (s_portal_on) strlcpy(out, "192.168.4.1", len);
    else strlcpy(out, "", len);
}

int net_rssi(void)
{
    wifi_ap_record_t ap;
    if (s_state == NET_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) return ap.rssi;
    return 0;
}
