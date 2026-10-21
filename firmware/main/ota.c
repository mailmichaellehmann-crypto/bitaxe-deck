// OTA-Updates über GitHub Releases
// - fragt https://api.github.com/repos/<repo>/releases/latest ab (alle 6 h und auf Wunsch)
// - lädt das Asset „bitaxe_deck.bin“ per HTTPS (Zertifikatsprüfung) in die freie OTA-Partition
// - neue Firmware gilt erst als gut, wenn sie 60 s mit WLAN läuft; sonst Rücksprung (Rollback)

#include "ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "board.h"
#include "net.h"
#include "sdkconfig.h"
#include "settings.h"

static const char *TAG = "ota";

#define ASSET_NAME "bitaxe_deck.bin"
#define CHECK_INTERVAL_MS (6LL * 3600 * 1000)

static ota_status_t s_st;
static char s_url[512];
static SemaphoreHandle_t s_mtx;
static volatile bool s_req_check, s_req_install;

void ota_get(ota_status_t *out)
{
    if (!s_mtx) { memset(out, 0, sizeof(*out)); return; } // noch nicht gestartet
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *out = s_st;
    xSemaphoreGive(s_mtx);
}

static void set_state(ota_state_t st, const char *err)
{
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_st.state = st;
    if (err) strlcpy(s_st.error, err, sizeof(s_st.error));
    xSemaphoreGive(s_mtx);
}

void ota_check_now(void) { s_req_check = true; }
void ota_install_now(void) { s_req_install = true; }

// "v1.4.0" / "1.4.0" -> vergleichbare Zahl
static long ver_num(const char *v)
{
    while (*v && (*v < '0' || *v > '9')) v++;
    int a = 0, b = 0, c = 0;
    sscanf(v, "%d.%d.%d", &a, &b, &c);
    return a * 1000000L + b * 1000L + c;
}

typedef struct { char *buf; int len, cap; } buf_t;

static esp_err_t http_evt(esp_http_client_event_t *e)
{
    buf_t *b = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && b) {
        if (b->len + e->data_len + 1 > b->cap) {
            int ncap = b->cap ? b->cap * 2 : 8192;
            while (ncap < b->len + e->data_len + 1) ncap *= 2;
            if (ncap > 256 * 1024) return ESP_FAIL;
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

static void check_for_update(void)
{
    set_state(OTA_CHECKING, NULL);
    char api[160];
    snprintf(api, sizeof(api), "https://api.github.com/repos/%s/releases/latest", CONFIG_DECK_OTA_REPO);
    buf_t b = {0};
    esp_http_client_config_t cfg = {
        .url = api,
        .event_handler = http_evt,
        .user_data = &b,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "bitaxe-deck-ota",
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "Accept", "application/vnd.github+json");
    esp_err_t r = esp_http_client_perform(c);
    int status = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    if (r != ESP_OK || status != 200 || !b.buf) {
        char e[64];
        snprintf(e, sizeof(e), status == 404 ? "Noch kein Release" : "GitHub nicht erreichbar (%d)", status);
        ESP_LOGW(TAG, "Update-Prüfung: %s", e);
        free(b.buf);
        set_state(OTA_FAILED, e);
        return;
    }
    cJSON *j = cJSON_Parse(b.buf);
    free(b.buf);
    const cJSON *tag = j ? cJSON_GetObjectItem(j, "tag_name") : NULL;
    const cJSON *assets = j ? cJSON_GetObjectItem(j, "assets") : NULL;
    const char *url = NULL;
    cJSON *a;
    cJSON_ArrayForEach(a, assets)
    {
        const cJSON *n = cJSON_GetObjectItem(a, "name");
        if (cJSON_IsString(n) && !strcmp(n->valuestring, ASSET_NAME)) {
            const cJSON *u = cJSON_GetObjectItem(a, "browser_download_url");
            if (cJSON_IsString(u)) url = u->valuestring;
        }
    }
    if (!cJSON_IsString(tag) || !url) {
        cJSON_Delete(j);
        set_state(OTA_FAILED, "Release ohne " ASSET_NAME);
        return;
    }
    const char *cur = esp_app_get_description()->version;
    bool newer = ver_num(tag->valuestring) > ver_num(cur);
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    const char *t = tag->valuestring;
    if (*t == 'v' || *t == 'V') t++;
    strlcpy(s_st.latest, t, sizeof(s_st.latest));
    strlcpy(s_url, url, sizeof(s_url));
    s_st.state = newer ? OTA_AVAILABLE : OTA_UPTODATE;
    xSemaphoreGive(s_mtx);
    ESP_LOGI(TAG, "Installiert %s, neuestes Release %s%s", cur, tag->valuestring, newer ? " – Update verfügbar" : "");
    cJSON_Delete(j);
}

static void install(void)
{
    char url[sizeof(s_url)];
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    strlcpy(url, s_url, sizeof(url));
    s_st.state = OTA_INSTALLING;
    s_st.progress = 0;
    xSemaphoreGive(s_mtx);
    ESP_LOGI(TAG, "Lade %s", url);

    esp_http_client_config_t http = {
        .url = url,
        .timeout_ms = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "bitaxe-deck-ota",
        .buffer_size = 4096,
        .buffer_size_tx = 2048, // GitHub leitet auf lange Download-Adressen um
        .keep_alive_enable = true,
        .max_redirection_count = 5,
    };
    esp_https_ota_config_t cfg = {.http_config = &http};
    esp_https_ota_handle_t h = NULL;
    esp_err_t r = esp_https_ota_begin(&cfg, &h);
    if (r != ESP_OK) {
        set_state(OTA_FAILED, "Download fehlgeschlagen");
        return;
    }
    esp_app_desc_t desc;
    if (esp_https_ota_get_img_desc(h, &desc) == ESP_OK && strcmp(desc.project_name, esp_app_get_description()->project_name)) {
        esp_https_ota_abort(h);
        set_state(OTA_FAILED, "Falsche Firmware-Datei");
        return;
    }
    int total = esp_https_ota_get_image_size(h);
    while ((r = esp_https_ota_perform(h)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int done = esp_https_ota_get_image_len_read(h);
        if (total > 0) {
            xSemaphoreTake(s_mtx, portMAX_DELAY);
            s_st.progress = done * 100 / total;
            xSemaphoreGive(s_mtx);
        }
    }
    if (r != ESP_OK || !esp_https_ota_is_complete_data_received(h)) {
        esp_https_ota_abort(h);
        set_state(OTA_FAILED, "Download abgebrochen");
        return;
    }
    if (esp_https_ota_finish(h) != ESP_OK) {
        set_state(OTA_FAILED, "Firmware ungültig");
        return;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_st.progress = 100;
    xSemaphoreGive(s_mtx);
    ESP_LOGI(TAG, "Update installiert, starte neu");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static void ota_task(void *arg)
{
    // Neue Firmware erst nach 60 s mit WLAN als gut markieren (sonst Rollback beim nächsten Start)
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    bool pending = esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
    int64_t online_since = 0;
    int64_t next_check = 60 * 1000; // erste Prüfung 1 min nach dem Start
    for (;;) {
        int64_t now = esp_timer_get_time() / 1000;
        bool online = net_state() == NET_CONNECTED && net_time_valid();
        if (online && !online_since) online_since = now;
        if (!online) online_since = 0;
        if (pending && online_since && now - online_since > 60000) {
            esp_ota_mark_app_valid_cancel_rollback();
            pending = false;
            ESP_LOGI(TAG, "Firmware %s bestätigt", esp_app_get_description()->version);
        }
        if (online && (s_req_check || now >= next_check)) {
            s_req_check = false;
            next_check = now + CHECK_INTERVAL_MS;
            check_for_update();
        }
        ota_status_t cur;
        ota_get(&cur);
        // automatisch installieren, wenn niemand das Display benutzt (5 min ohne Berührung)
        bool idle = false;
        if (board_lvgl_lock(100)) {
            idle = lv_disp_get_inactive_time(NULL) > 5 * 60 * 1000;
            board_lvgl_unlock();
        }
        if (cur.state == OTA_AVAILABLE && (s_req_install || (g_settings.auto_update && idle && !pending))) {
            s_req_install = false;
            install();
        } else if (s_req_install && cur.state != OTA_AVAILABLE) {
            s_req_install = false;
            s_req_check = true;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void ota_start(void)
{
    s_mtx = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(ota_task, "ota", 8192, NULL, 2, NULL, 0);
}
