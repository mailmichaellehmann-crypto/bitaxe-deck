// WLAN, Zeit (SNTP) und Setup-Portal
// Ohne gespeichertes WLAN (oder wenn die Verbindung nicht klappt) öffnet das Gerät
// einen eigenen Access-Point. Unter http://192.168.4.1 trägt man WLAN, Bitaxe-IP und Strompreis ein.
// Im Heimnetz ist dieselbe Seite unter der IP des Geräts erreichbar.

#include "net.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "data.h"
#include "mdns.h"
#include "ota.h"
#include "regie.h"
#include "settings.h"
#include "ui.h"

static const char *TAG = "net";

static volatile net_state_t s_state = NET_CONNECTING;
static esp_netif_t *s_sta, *s_ap;
static char s_ip[16] = "";
static bool s_portal_on = false;
static esp_timer_handle_t s_fallback_timer;
#define SCAN_MAX 20
static struct { char ssid[33]; int8_t rssi; } s_scan[SCAN_MAX]; // gefundene Netze, stärkstes zuerst
static int s_scan_n;
static void scan_networks(void);
static bool authed(httpd_req_t *req);
static esp_err_t login_page(httpd_req_t *req, const char *msg);

// ------------------------------------------------------------------ Portal (HTTP)

// Einstellungsseite im Strike-Stil: schwarz, gedämpftes Weiß, Mono-Beschriftungen, feine Linien,
// Pixel-Mosaik oben, Akzent Pink #FFD4D4. Keine externen Schriften (läuft auch im Setup-WLAN).
static const char *PAGE_HEAD =
    "<!doctype html><html lang=de><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'><title>Satoshi Deck</title>"
    "<link rel=icon href=\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 25 25'%3E%3Crect width='25' height='25' rx='5' fill='%23000'/%3E%3Crect x='3' y='3' width='5' height='5' rx='1' fill='%235e5e5e'/%3E%3Crect x='10' y='3' width='5' height='5' rx='1' fill='%235e5e5e'/%3E%3Crect x='17' y='3' width='5' height='5' rx='1' fill='%239a9a9a'/%3E%3Crect x='3' y='10' width='5' height='5' rx='1' fill='%235e5e5e'/%3E%3Crect x='10' y='10' width='5' height='5' rx='1' fill='%235e5e5e'/%3E%3Crect x='17' y='10' width='5' height='5' rx='1' fill='%239a9a9a'/%3E%3Crect x='3' y='17' width='5' height='5' rx='1' fill='%235e5e5e'/%3E%3Crect x='10' y='17' width='5' height='5' rx='1' fill='%235e5e5e'/%3E%3Crect x='17' y='17' width='5' height='5' rx='1' fill='%23ffd4d4'/%3E%3C/svg%3E\"><style>"
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
    ".btn{display:inline-block;margin-top:24px;padding:13px 26px;border-radius:999px;font:500 14px var(--mono);"
    "color:#000;background:var(--fg);text-decoration:none}.btn.wait{background:#000;color:var(--mut);border:1px solid var(--line)}"
    "</style></head><body><div id=mos></div><main>"
    "<script>(function(){var m=document.getElementById('mos');for(var i=0;i<48*6;i++){var x=i%48,"
    "r=Math.random(),g=Math.round(14+r*r*58*(x/48+.3));var e=document.createElement('i');"
    "e.style.background=(i==48*6-1)?'var(--pink)':'rgb('+g+','+g+','+g+')';m.appendChild(e)}})()</script>";

// Akzentfarbe des Displays auch auf der Seite
static void acc_style(char *out, size_t len)
{
    snprintf(out, len, "<style>:root{--pink:#%06x}</style>", (unsigned)ACCENT_RGB[g_settings.accent < ACCENTS ? g_settings.accent : 0]);
}

// Text sicher in HTML einsetzen (WLAN-Namen können beliebige Zeichen enthalten)
static void html_esc(char *out, size_t len, const char *s)
{
    size_t o = 0;
    for (; *s && o + 7 < len; s++) {
        const char *r = *s == '&' ? "&amp;" : *s == '<' ? "&lt;" : *s == '>' ? "&gt;" : *s == '\'' ? "&#39;" : *s == '"' ? "&quot;" : NULL;
        if (r) o += snprintf(out + o, len - o, "%s", r);
        else out[o++] = *s;
    }
    out[o] = 0;
}

static esp_err_t root_get(httpd_req_t *req)
{
    if (!authed(req)) return login_page(req, NULL);
    const size_t page_len = 24576; // Seite und Bausteine im PSRAM: der Stack des Webservers ist knapp
    enum { WOPTS = 1600, RG = 1100, AOPT = 400, PINF = 400 };
    char *html = heap_caps_malloc(page_len, MALLOC_CAP_SPIRAM), *scratch = heap_caps_malloc(WOPTS + RG + AOPT + PINF, MALLOC_CAP_SPIRAM);
    if (!html || !scratch) {
        free(html);
        free(scratch);
        return httpd_resp_send_500(req);
    }
    char *wopts = scratch, *rg = wopts + WOPTS, *aopt = rg + RG, *pinf = aopt + AOPT;
    char q[32] = "", acc[48], notice[160] = "";
    acc_style(acc, sizeof(acc));
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (strstr(q, "pin=bad")) snprintf(notice, sizeof(notice), "<p class=acc>Gespeichert – aber die PIN braucht 4 bis 8 Ziffern und wurde nicht geändert.</p>");
        else if (strstr(q, "ok")) snprintf(notice, sizeof(notice), "<p class=acc>&#10003; Gespeichert. Die Änderungen gelten sofort.</p>");
    }
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
    const size_t cards_len = 4600; // auf dem Heap: der Stack des Webservers reicht dafür nicht
    char *cards = malloc(cards_len), order[48];
    if (!cards) {
        free(html);
        free(scratch);
        return httpd_resp_send_500(req);
    }
    int cl = 0, ol = 0;
    for (int p = 0; p < DECK_CARDS; p++) {
        int i = g_settings.card_order[p];
        cl += snprintf(cards + cl, cards_len - cl,
                       "<div class=ci data-id=%d><label><i>%02d</i><input type=checkbox name=c%d value=1%s>%s</label>"
                       "<button type=button onclick=mv(this,-1)>&uarr;</button><button type=button onclick=mv(this,1)>&darr;</button></div>",
                       i, p + 1, i, (g_settings.card_mask >> i) & 1 ? " checked" : "", DECK_CARD_NAMES[i]);
        ol += snprintf(order + ol, sizeof(order) - ol, "%s%d", p ? "," : "", i);
    }
    char nsrc[200];
    int sl = 0;
    for (int i = 0; i < NEWS_SOURCES; i++)
        sl += snprintf(nsrc + sl, sizeof(nsrc) - sl, "<option value=%d%s>%s</option>", i, g_settings.news_src == i ? " selected" : "",
                       NEWS_SOURCE_NAMES[i]);
    char ncol[400];
    int nl = 0;
    for (int i = 0; i <= NIGHT_COLORS; i++)
        nl += snprintf(ncol + nl, sizeof(ncol) - nl, "<option value=%d%s>%s</option>", i,
                       g_settings.night_color == i ? " selected" : "", NIGHT_COLOR_NAMES[i]);
    // WLAN: aktuelles Netz und zuletzt gefundene als Auswahl; die Seite fragt beim Laden /scan nach einer frischen Liste
    char ssid_esc[200];
    html_esc(ssid_esc, sizeof(ssid_esc), g_settings.wifi_ssid);
    int wl = 0;
    if (g_settings.wifi_ssid[0]) wl += snprintf(wopts, WOPTS, "<option value='%s' selected>%s</option>", ssid_esc, ssid_esc);
    for (int i = 0; i < s_scan_n && wl < WOPTS - 220; i++) {
        if (!strcmp(s_scan[i].ssid, g_settings.wifi_ssid)) continue;
        char e[200];
        html_esc(e, sizeof(e), s_scan[i].ssid);
        wl += snprintf(wopts + wl, WOPTS - wl, "<option value='%s'>%s</option>", e, e);
    }
    snprintf(wopts + wl, WOPTS - wl, "<option value=''%s>Anderes Netzwerk …</option>", g_settings.wifi_ssid[0] ? "" : " selected");
    // Akzentfarbe, Regie (Ereignisse zum Abwählen), PIN
    int al = 0, rl = 0;
    for (int i = 0; i < ACCENTS; i++)
        al += snprintf(aopt + al, AOPT - al, "<option value=%d%s>%s</option>", i, g_settings.accent == i ? " selected" : "", ACCENT_NAMES[i]);
    for (int i = 0; i < RG_N; i++)
        rl += snprintf(rg + rl, RG - rl, "<label><input type=checkbox name=rg%d value=1%s>%s</label>", i,
                       (g_settings.regie_mask >> i) & 1 ? " checked" : "", REGIE_NAMES[i]);
    snprintf(pinf, PINF, "<input name=pin type=password inputmode=numeric maxlength=8 autocomplete=new-password placeholder='%s'>%s",
             g_settings.web_pin[0] ? "unverändert lassen" : "ohne PIN",
             g_settings.web_pin[0] ? "<div class=cards style='grid-template-columns:1fr'><label><input type=checkbox name=pinoff value=1>"
                                     "PIN entfernen</label></div>"
                                   : "");
    char wx_lat[16] = "", wx_lon[16] = "";
    if (g_settings.wx_ll) {
        snprintf(wx_lat, sizeof(wx_lat), "%.5f", g_settings.wx_lat);
        snprintf(wx_lon, sizeof(wx_lon), "%.5f", g_settings.wx_lon);
    }
    // Wetter: eingetragener Ort und was das Display dazu gefunden hat
    char wx_line[200];
    data_lock();
    const weather_t *w = &data_get()->wx;
    if (!g_settings.wx_place[0]) snprintf(wx_line, sizeof(wx_line), "Stadt oder Postleitzahl eintippen und aus der Liste wählen");
    else if (w->place[0] && !w->err[0]) snprintf(wx_line, sizeof(wx_line), "Gefunden: <span class=acc>%s</span>%s%s", w->place, w->region[0] ? " · " : "", w->region);
    else if (w->err[0]) snprintf(wx_line, sizeof(wx_line), "<span class=acc>%s</span>", w->err);
    else snprintf(wx_line, sizeof(wx_line), "wird gesucht …");
    data_unlock();
    char ct[16];
    float c = g_settings.kwh_price * 100.0f;
    snprintf(ct, sizeof(ct), c == (int)c ? "%.0f" : "%.1f", c);
    for (char *q = ct; *q; q++) if (*q == '.') *q = ',';
    int len = snprintf(html, page_len,
             "%s%s<h1>Satoshi Deck</h1><p>Einstellungen für dein Display. Änderungen gelten sofort – nur ein neues WLAN startet es neu.</p>%s"
             "<form method=post action=/save onsubmit=wsub()>"
             "<div class=sec>Verbindung</div>"
             "<label>WLAN</label><select id=wsel onchange=ws()>%s</select>"
             "<input name=ssid id=ssid value='%s' required maxlength=32 autocomplete=off placeholder='Name des WLANs' style='margin-top:8px%s'>"
             "<p><small id=wst>suche Netzwerke …</small></p>"
             "<label>WLAN-Passwort</label><input name=pass type=password maxlength=64 placeholder='%s'>"
             "<label>Bitaxe – IP-Adresse</label><input name=bitaxe value='%s' placeholder='192.168.1.42' maxlength=63>"
             "<div class=sec>Preisalarm</div>"
             "<div class=row><div style='flex:1'><label>Oberhalb (%s)</label><input name=al_hi value='%.0f' inputmode=numeric></div>"
             "<div style='flex:1'><label>Unterhalb (%s)</label><input name=al_lo value='%.0f' inputmode=numeric></div></div>"
             "<div class=sec>Anzeige</div>"
             "<div class=row><div style='flex:1'><label>Nachtmodus ab</label><input name=n_from value='%d' inputmode=numeric></div>"
             "<div style='flex:1'><label>bis (Uhr)</label><input name=n_to value='%d' inputmode=numeric></div></div>"
             "<label>Farbe im Nachtmodus</label><select name=ncol>%s</select>"
             "<label>Akzentfarbe</label><select name=acc>%s</select>"
             "<label>Uhr</label><select name=clk><option value=3 %s>LED-Punktanzeige</option><option value=0 %s>Klassisch</option></select>"
             "<label>Chart-Fläche</label><select name=cstyle>"
             "<option value=0 %s>Punkte</option><option value=1 %s>Ohne Verlauf</option><option value=2 %s>Pixel</option></select>"
             "<label>Automatische Updates</label><select name=aupd>"
             "<option value=1 %s>Ein – nachts bzw. wenn niemand das Display benutzt</option><option value=0 %s>Aus</option></select>"
             "<div class=sec>Regie</div>"
             "<p><small>Passiert etwas, zeigt das Display 15 Sekunden die passende Karte und kehrt dann zurück. Nicht im Nachtmodus.</small></p>"
             "<select name=rg><option value=1 %s>Ein</option><option value=0 %s>Aus</option></select>"
             "<input type=hidden name=rgm value=1><div class=cards style='grid-template-columns:1fr'>%s</div>"
             "<div class=sec>Mining-Kosten</div>"
             "<label>Strompreis (ct/kWh)</label><input name=kwh_ct value='%s' inputmode=decimal maxlength=6>"
             "<div class=cards style='grid-template-columns:1fr'><label><input type=checkbox name=ereset value=1>"
             "Stromzähler zurücksetzen</label></div>"
             "<div class=sec>Wetter</div>"
             "<label>Ort</label><input name=wx id=wx value='%s' placeholder='Stadt oder Postleitzahl' maxlength=47 autocomplete=off oninput=wq()>"
             "<select id=wxs onchange=wp() style='margin-top:8px;display:none'></select>"
             "<input type=hidden name=wxlat id=wxlat value='%s'><input type=hidden name=wxlon id=wxlon value='%s'>"
             "<p><small id=wxi>%s</small></p>"
             "<div class=sec>News</div>"
             "<label>Quelle der Schlagzeilen</label><select name=news>%s</select>"
             "<p><small>Crypto Vision: englisch, viele Quellen · Blocktrainer: deutsch · Bitcoin Magazine: englisch</small></p>"
             "<div class=sec>Karten</div><p><small>Reihenfolge mit &uarr; &darr; – am Display: im Kartenmenü eine Kachel halten und ziehen</small></p>"
             "<input type=hidden name=cards value=1><input type=hidden name=order id=order value='%s'><div class='cards ord' id=cl>%s</div>"
             "<script>function mv(b,d){var r=b.parentNode,l=r.parentNode,o=d<0?r.previousElementSibling:r.nextElementSibling;"
             "if(!o)return;l.insertBefore(d<0?r:o,d<0?o:r);var a=[];l.querySelectorAll('.ci').forEach(function(e,k){"
             "a.push(e.dataset.id);e.querySelector('i').textContent=(k<9?'0':'')+(k+1)});"
             "document.getElementById('order').value=a.join(',')}</script>"
             "<div class=sec>Schutz</div>"
             "<label>PIN für diese Seite (4–8 Ziffern)</label>%s"
             "<p><small>Schützt die Einstellungen vor anderen im WLAN. Vergessen? Am Display in den Einstellungen den QR-Code 3 Sekunden halten.</small></p>"
             "<button>Speichern &rarr;</button></form>"
             "<div class=sec>Firmware</div>"
             "<p>Installiert: <b>%s</b> · %s</p>"
             "<form method=post action=/ota style='display:flex;gap:10px'>"
             "<button name=a value=check style='margin-top:12px;background:#000;color:var(--fg);border:1px solid var(--line)'>Nach Updates suchen</button>"
             "%s</form>"
             "<script>"
             // WLAN: Auswahl übernehmen bzw. Feld für einen eigenen Namen zeigen; frische Liste vom Display holen
             "function ws(){var s=document.getElementById('wsel'),i=document.getElementById('ssid');"
             "if(s.value){i.value=s.value;i.style.display='none'}else{i.style.display='';i.focus()}}"
             "function bars(r){return r>-56?'\\u25AE\\u25AE\\u25AE\\u25AE':r>-68?'\\u25AE\\u25AE\\u25AE\\u25AF':r>-76?'\\u25AE\\u25AE\\u25AF\\u25AF':'\\u25AE\\u25AF\\u25AF\\u25AF'}"
             "fetch('/scan').then(function(r){return r.json()}).then(function(a){var s=document.getElementById('wsel'),"
             "cur=document.getElementById('ssid').value,keep=s.value,seen={};while(s.options.length)s.remove(0);"
             "a.forEach(function(n){if(seen[n.s])return;seen[n.s]=1;s.add(new Option(n.s+'   '+bars(n.r),n.s))});"
             "if(cur&&!seen[cur])s.add(new Option(cur+'   (nicht in Reichweite)',cur),0);"
             "s.add(new Option('Anderes Netzwerk \\u2026',''));s.value=keep;"
             "var k=Object.keys(seen).length;document.getElementById('wst').textContent=k?k+' Netzwerke in Reichweite':'Keine Netzwerke gefunden'})"
             ".catch(function(){document.getElementById('wst').textContent='Suche nicht möglich \\u2013 Namen unter \\u201eAnderes Netzwerk\\u201c eintragen'});"
             // Wetter: Orte bei Open-Meteo suchen, Treffer als Auswahl; die Koordinaten gehen mit
             "var wt;function wq(){document.getElementById('wxlat').value='';document.getElementById('wxlon').value='';"
             "clearTimeout(wt);wt=setTimeout(wf,400)}"
             "function wf(){var q=document.getElementById('wx').value.trim(),s=document.getElementById('wxs'),"
             "t=document.getElementById('wxi');if(q.length<2){s.style.display='none';return}"
             "fetch('https://geocoding-api.open-meteo.com/v1/search?count=8&language=de&format=json&name='+encodeURIComponent(q))"
             ".then(function(r){return r.json()}).then(function(d){var a=d.results||[];while(s.options.length)s.remove(0);"
             "a.forEach(function(r){var o=new Option(r.name+' \\u00b7 '+[r.admin1,r.country].filter(Boolean).join(', '),"
             "r.latitude+','+r.longitude);o.dataset.n=r.name;s.add(o)});s.style.display=a.length?'':'none';"
             "t.textContent=a.length?'':'Kein Ort gefunden';if(a.length)wp(1)})"
             ".catch(function(){t.textContent='Ortssuche nicht erreichbar \\u2013 das Display sucht den Ort beim Speichern selbst'})}"
             // beim Tippen den ersten Treffer nur vormerken; den Namen übernimmt erst die Auswahl bzw. das Speichern
             "function wp(auto){var s=document.getElementById('wxs'),o=s.options[s.selectedIndex];if(!o)return;"
             "var c=o.value.split(',');document.getElementById('wxlat').value=c[0];document.getElementById('wxlon').value=c[1];"
             "if(!auto)document.getElementById('wx').value=o.dataset.n;document.getElementById('wxi').textContent='Gewählt: '+o.text}"
             "function wsub(){var s=document.getElementById('wxs'),o=s.options[s.selectedIndex];"
             "if(o&&document.getElementById('wxlat').value)document.getElementById('wx').value=o.dataset.n}"
             "</script>"
             "<p style='margin-top:28px'><small>0 = Alarm aus · IP des Bitaxe steht im AxeOS-Webinterface · "
             "Wetterdaten: <a href='https://open-meteo.com/'>Open-Meteo.com</a> (CC BY 4.0)</small></p>"
             "</main></body></html>",
             PAGE_HEAD, acc, notice, wopts, ssid_esc, g_settings.wifi_ssid[0] ? ";display:none" : "",
             g_settings.wifi_pass[0] ? "unverändert lassen" : "", g_settings.bitaxe_host[0],
             settings_cur(), g_settings.alarm_hi, settings_cur(), g_settings.alarm_lo, g_settings.night_from, g_settings.night_to, ncol, aopt,
             g_settings.clock_style == 3 ? "selected" : "", g_settings.clock_style == 0 ? "selected" : "",
             g_settings.chart_style == 0 ? "selected" : "", g_settings.chart_style == 1 ? "selected" : "",
             g_settings.chart_style == 2 ? "selected" : "",
             g_settings.auto_update ? "selected" : "", g_settings.auto_update ? "" : "selected",
             g_settings.regie_on ? "selected" : "", g_settings.regie_on ? "" : "selected", rg, ct, g_settings.wx_place, wx_lat, wx_lon, wx_line,
             nsrc, order, cards, pinf,
             deck_version(), ota_line, ota_btn);
    if (len >= (int)page_len) ESP_LOGW(TAG, "Einstellungsseite abgeschnitten (%d Zeichen)", len);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, html);
    free(cards);
    free(scratch);
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

// ------------------------------------------------------------------ PIN für die Browser-Seite (optional)

static char s_token[17];  // Sitzung: gilt bis zum Neustart oder bis die PIN geändert wird
static int s_pin_fails;
static int64_t s_pin_lock; // nach 5 falschen Versuchen gesperrt bis (ms)

static void token_new(void) { snprintf(s_token, sizeof(s_token), "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random()); }

static void set_session_cookie(httpd_req_t *req)
{
    static char ck[96]; // muss bis zum Senden der Antwort bestehen
    if (!s_token[0]) token_new();
    snprintf(ck, sizeof(ck), "sd=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=2592000", s_token);
    httpd_resp_set_hdr(req, "Set-Cookie", ck);
}

// darf diese Anfrage an die Einstellungen? Ohne PIN immer, sonst nur mit dem Sitzungs-Cookie
static bool authed(httpd_req_t *req)
{
    if (!g_settings.web_pin[0]) return true;
    char ck[256];
    if (!s_token[0] || httpd_req_get_hdr_value_str(req, "Cookie", ck, sizeof(ck)) != ESP_OK) return false;
    const char *p = strstr(ck, "sd=");
    return p && !strncmp(p + 3, s_token, 16);
}

static esp_err_t see_other(httpd_req_t *req, const char *where)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", where);
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t login_page(httpd_req_t *req, const char *msg)
{
    char *html = malloc(5120), acc[48];
    if (!html) return httpd_resp_send_500(req);
    acc_style(acc, sizeof(acc));
    snprintf(html, 5120,
             "%s%s<h1>PIN</h1><p>Die Einstellungen dieses Displays sind mit einer PIN geschützt.</p>%s%s%s"
             "<form method=post action=/login><label>PIN</label>"
             "<input name=pin type=password inputmode=numeric maxlength=8 autofocus required>"
             "<button>Öffnen &rarr;</button></form>"
             "<p style='margin-top:28px'><small>PIN vergessen? Am Display in den Einstellungen den QR-Code 3 Sekunden gedrückt halten.</small></p>"
             "</main></body></html>",
             PAGE_HEAD, acc, msg ? "<p class=acc>" : "", msg ? msg : "", msg ? "</p>" : "");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, html);
    free(html);
    return ESP_OK;
}

static esp_err_t login_post(httpd_req_t *req)
{
    char body[64] = {0}, pin[16] = "";
    httpd_req_recv(req, body, sizeof(body) - 1);
    form_field(body, "pin", pin, sizeof(pin));
    int64_t ms = esp_timer_get_time() / 1000;
    if (!g_settings.web_pin[0]) return see_other(req, "/");
    if (ms < s_pin_lock) return login_page(req, "Zu viele Versuche – bitte eine Minute warten.");
    if (strcmp(pin, g_settings.web_pin)) {
        if (++s_pin_fails >= 5) {
            s_pin_fails = 0;
            s_pin_lock = ms + 60000;
        }
        ESP_LOGW(TAG, "Browser-Seite: falsche PIN");
        return login_page(req, "Falsche PIN.");
    }
    s_pin_fails = 0;
    set_session_cookie(req);
    return see_other(req, "/");
}

static void restart_cb(void *arg)
{
    data_energy_save();
    esp_restart();
}

// Speichern: alles gilt sofort, ohne Neustart; nur ein geändertes WLAN (und die Ersteinrichtung) startet neu
static esp_err_t save_post(httpd_req_t *req)
{
    if (!authed(req)) return see_other(req, "/");
    const int body_len = 3072;
    char *body = calloc(1, body_len);
    if (!body) return httpd_resp_send_500(req);
    int total = 0;
    while (total < body_len - 1) {
        int r = httpd_req_recv(req, body + total, body_len - 1 - total);
        if (r <= 0) break;
        total += r;
        if (total >= req->content_len) break;
    }
    char v[96], old_ssid[33], old_pass[65];
    strlcpy(old_ssid, g_settings.wifi_ssid, sizeof(old_ssid));
    strlcpy(old_pass, g_settings.wifi_pass, sizeof(old_pass));
    data_lock(); // die Abruf-Tasks lesen Ort und Adressen
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
        char la[20] = "", lo[20] = "";
        form_field(body, "wxlat", la, sizeof(la));
        form_field(body, "wxlon", lo, sizeof(lo));
        char *e1, *e2;
        float fa = strtof(la, &e1), fo = strtof(lo, &e2);
        g_settings.wx_ll = la[0] && lo[0] && e1 != la && e2 != lo && fabsf(fa) <= 90 && fabsf(fo) <= 180;
        g_settings.wx_lat = fa;
        g_settings.wx_lon = fo;
    }
    if (form_field(body, "news", v, sizeof(v))) { int c = atoi(v); if (c >= 0 && c < NEWS_SOURCES) g_settings.news_src = c; }
    if (form_field(body, "clk", v, sizeof(v))) g_settings.clock_style = atoi(v) == 0 ? 0 : 3;
    if (form_field(body, "acc", v, sizeof(v))) { int c = atoi(v); if (c >= 0 && c < ACCENTS) g_settings.accent = c; }
    if (form_field(body, "rg", v, sizeof(v))) g_settings.regie_on = atoi(v) != 0;
    if (form_field(body, "rgm", v, sizeof(v))) { // nicht angehakte Kästchen werden nicht mitgeschickt
        uint16_t m = 0;
        for (int i = 0; i < RG_N; i++) {
            char k[6];
            snprintf(k, sizeof(k), "rg%d", i);
            if (form_field(body, k, v, sizeof(v))) m |= 1u << i;
        }
        g_settings.regie_mask = m;
    }
    // PIN: neu setzen (4–8 Ziffern) oder entfernen; danach gilt eine neue Sitzung
    bool pin_bad = false, pin_changed = false;
    if (form_field(body, "pinoff", v, sizeof(v))) {
        pin_changed = g_settings.web_pin[0] != 0;
        g_settings.web_pin[0] = 0;
    } else if (form_field(body, "pin", v, sizeof(v)) && v[0]) {
        size_t n = strlen(v);
        if (n < 4 || n > 8 || strspn(v, "0123456789") != n) pin_bad = true;
        else if (strcmp(v, g_settings.web_pin)) {
            strlcpy(g_settings.web_pin, v, sizeof(g_settings.web_pin));
            pin_changed = true;
        }
    }
    if (pin_changed) {
        token_new();
        if (g_settings.web_pin[0]) set_session_cookie(req); // dieser Browser bleibt angemeldet
    }
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
    data_unlock();
    if (form_field(body, "ereset", v, sizeof(v))) data_energy_reset();
    free(body);
    settings_save();
    bool restart = strcmp(old_ssid, g_settings.wifi_ssid) || strcmp(old_pass, g_settings.wifi_pass) || s_state != NET_CONNECTED;
    ESP_LOGI(TAG, "Einstellungen gespeichert (%s)", restart ? "Neustart" : "ohne Neustart");
    if (!restart) {
        data_refresh();        // Wetter und News gleich neu laden
        ui_settings_changed(); // Farben, Uhr, Karten …
        return see_other(req, pin_bad ? "/?pin=bad" : "/?ok=1");
    }

    char ssid_esc[200];
    html_esc(ssid_esc, sizeof(ssid_esc), g_settings.wifi_ssid);
    const size_t hl = 6144;
    char *html = malloc(hl);
    if (html) {
        snprintf(html, hl,
                 "%s<h1>Gespeichert.</h1><p>Das Display startet neu und verbindet sich mit <span class=acc>%s</span>.</p>"
                 "<p><small id=st>Neustart läuft – das dauert ein paar Sekunden.</small></p>"
                 "<a class='btn wait' id=bk href=/>Zurück zu den Einstellungen &rarr;</a>"
                 "<script>var n=0;function p(){fetch('/ping',{cache:'no-store'}).then(function(r){if(!r.ok)throw 0;"
                 "document.getElementById('bk').className='btn';document.getElementById('st').textContent='Das Display ist wieder da.'})"
                 ".catch(function(){if(++n<20)setTimeout(p,1500);else document.getElementById('st').textContent="
                 "'Keine Antwort. Wurde das WLAN geändert, hat das Display eine neue Adresse – sie steht in den Einstellungen am Display.'})}"
                 "setTimeout(p,5000)</script></main></body></html>",
                 PAGE_HEAD, ssid_esc);
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        httpd_resp_sendstr(req, html);
        free(html);
    } else httpd_resp_sendstr(req, "Gespeichert.");

    const esp_timer_create_args_t a = {.callback = restart_cb, .name = "restart"};
    esp_timer_handle_t t;
    esp_timer_create(&a, &t);
    esp_timer_start_once(t, 1500 * 1000);
    return ESP_OK;
}

static esp_err_t ota_post(httpd_req_t *req)
{
    if (!authed(req)) return see_other(req, "/");
    char body[64] = {0}, v[16];
    httpd_req_recv(req, body, sizeof(body) - 1);
    if (form_field(body, "a", v, sizeof(v))) {
        if (!strcmp(v, "install")) ota_install_now();
        else ota_check_now();
    }
    char *html = malloc(4096);
    if (!html) return httpd_resp_send_500(req);
    snprintf(html, 4096, "%s<h1>%s</h1><p>%s</p><p><a class=acc href=/>Zurück &rarr;</a></p></main></body></html>", PAGE_HEAD,
             strcmp(v, "install") ? "Suche läuft." : "Update startet.",
             strcmp(v, "install") ? "Das Display fragt GitHub nach einer neuen Version. Seite gleich neu laden."
                                  : "Das Display lädt die neue Firmware und startet danach neu. Nicht ausschalten.");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, html);
    free(html);
    return ESP_OK;
}

// Netze in Reichweite als JSON [{"s":Name,"r":Signal}]; im Setup-WLAN die beim Start gefundenen (eine neue Suche
// würde die Verbindung zum Handy kurz unterbrechen)
static esp_err_t scan_get(httpd_req_t *req)
{
    if (!authed(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        return httpd_resp_sendstr(req, "[]");
    }
    if (!s_portal_on) scan_networks();
    char *out = malloc(2600);
    if (!out) return httpd_resp_send_500(req);
    int o = snprintf(out, 2600, "[");
    for (int i = 0; i < s_scan_n && o < 2400; i++) {
        o += snprintf(out + o, 2600 - o, "%s{\"s\":\"", i ? "," : "");
        for (const char *c = s_scan[i].ssid; *c && o < 2560; c++) {
            if (*c == '"' || *c == '\\') out[o++] = '\\';
            if ((unsigned char)*c >= 0x20) out[o++] = *c;
        }
        o += snprintf(out + o, 2600 - o, "\",\"r\":%d}", s_scan[i].rssi);
    }
    snprintf(out + o, 2600 - o, "]");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, out);
    free(out);
    return ESP_OK;
}

// Lebenszeichen für die Seite nach dem Speichern
static esp_err_t ping_get(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, "ok");
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
    cfg.stack_size = 10240;
    httpd_handle_t srv;
    if (httpd_start(&srv, &cfg) != ESP_OK) return;
    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get};
    httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = save_post};
    httpd_uri_t any = {.uri = "/*", .method = HTTP_GET, .handler = redirect_get};
    httpd_uri_t ota = {.uri = "/ota", .method = HTTP_POST, .handler = ota_post};
    httpd_uri_t scan = {.uri = "/scan", .method = HTTP_GET, .handler = scan_get};
    httpd_uri_t ping = {.uri = "/ping", .method = HTTP_GET, .handler = ping_get};
    httpd_uri_t login = {.uri = "/login", .method = HTTP_POST, .handler = login_post};
    httpd_register_uri_handler(srv, &login);
    httpd_register_uri_handler(srv, &ota);
    httpd_register_uri_handler(srv, &scan);
    httpd_register_uri_handler(srv, &ping);
    httpd_register_uri_handler(srv, &root);
    httpd_register_uri_handler(srv, &save);
    httpd_register_uri_handler(srv, &any);
}

// ------------------------------------------------------------------ WLAN

static void scan_networks(void)
{
    wifi_scan_config_t sc = {.show_hidden = false};
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return;
    uint16_t n = SCAN_MAX;
    wifi_ap_record_t *recs = malloc(sizeof(wifi_ap_record_t) * SCAN_MAX);
    if (!recs) { esp_wifi_clear_ap_list(); return; }
    esp_wifi_scan_get_ap_records(&n, recs);
    int k = 0;
    for (int i = 0; i < n; i++) { // ohne leere Namen und doppelte (Repeater), stärkstes zuerst
        if (!recs[i].ssid[0]) continue;
        int j = 0;
        while (j < k && strcmp(s_scan[j].ssid, (char *)recs[i].ssid)) j++;
        if (j < k) {
            if (recs[i].rssi > s_scan[j].rssi) s_scan[j].rssi = recs[i].rssi;
            continue;
        }
        strlcpy(s_scan[k].ssid, (char *)recs[i].ssid, sizeof(s_scan[k].ssid));
        s_scan[k++].rssi = recs[i].rssi;
    }
    free(recs);
    for (int i = 1; i < k; i++)
        for (int j = i; j > 0 && s_scan[j].rssi > s_scan[j - 1].rssi; j--) {
            typeof(s_scan[0]) t = s_scan[j];
            s_scan[j] = s_scan[j - 1];
            s_scan[j - 1] = t;
        }
    s_scan_n = k;
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
    esp_netif_set_hostname(s_sta, DECK_HOSTNAME);
    // im Heimnetz unter http://satoshi-deck.local erreichbar
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(DECK_HOSTNAME);
        mdns_instance_name_set("Satoshi Deck");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }

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
