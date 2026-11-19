#pragma once
// Oberfläche: wischbare Karten (lv_tileview) im Braiins-Deck-Violett, LVGL 8.4
void ui_init(void); // muss mit gehaltener LVGL-Sperre aufgerufen werden
void ui_settings_changed(void); // Einstellungen im Betrieb geändert (aus jeder Task): die Oberfläche übernimmt sie
