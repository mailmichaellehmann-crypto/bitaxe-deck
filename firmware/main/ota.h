#pragma once
// OTA-Updates über GitHub Releases (Datei bitaxe_deck.bin im neuesten Release)
#include <stdbool.h>

typedef enum {
    OTA_IDLE = 0,    // noch nicht geprüft
    OTA_CHECKING,
    OTA_UPTODATE,
    OTA_AVAILABLE,   // neue Version gefunden
    OTA_INSTALLING,
    OTA_FAILED,
} ota_state_t;

typedef struct {
    ota_state_t state;
    char latest[24];  // z. B. "1.5.0"
    int progress;     // 0..100 während der Installation
    char error[64];
} ota_status_t;

void ota_start(void);              // Hintergrund-Task: prüft regelmäßig, bestätigt neue Firmware
void ota_get(ota_status_t *out);   // aktuellen Stand kopieren
void ota_check_now(void);          // sofort nach Updates suchen
void ota_install_now(void);        // gefundenes Update installieren (startet danach neu)
