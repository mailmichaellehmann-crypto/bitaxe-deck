# Satoshi Deck – Firmware für das WT32-SC01 Plus

Das Satoshi-Deck-Dashboard als eigenständige Firmware für das **Smart Panlee / WT32-SC01 Plus**
(Aufdruck ZX3D50CE08S-V16, ESP32-S3, 3,5" IPS 480 × 320, ST7796 über 8-Bit-MCU8080, Touch FT6336U).
Das Display holt sich die Daten selbst über WLAN, ein Rechner muss dafür nicht laufen.

**Technik:** ESP-IDF 5.3 · LVGL **8.4.0** (über den Component Manager, `main/idf_component.yml`) ·
`espressif/esp_lcd_st7796` 1.5.0 · eigene Schriften: Geist und Geist Mono (SIL OFL, Quelle in `fonts-src/`)

## Karten

Alle Karten füllen den ganzen Bildschirm (ohne Kopfzeile und Überschriften). Nach links oder rechts wischen.

| # | Karte | Inhalt |
|---|-------|--------|
| 1 | Preis | Kurs groß, 24h-Änderung, Kurs in der anderen Währung, Kursverlauf als Pixel-Mosaik |
| 2 | USD & EUR | beide Kurse mit 24h-Änderung |
| 3 | Chart | geglätteter Kursverlauf mit Glow, Hoch/Tief, Preis-Label und pulsierendem Punkt; Tippen auf „24h“ wechselt zu 7T / 30T / 1J; Finger halten und ziehen zeigt Preis und Zeit |
| 4 | Block | Blockhöhe, Alter und Pool des letzten Blocks, Blöcke bis zum Halving |
| 5 | Blockzeit & Gebühren | Blockhöhe, Sats pro USD, Gebühren niedrig/mittel/hoch in sat/vB und $ |
| 6 | Difficulty-Anpassung | erwartete Änderung, Fortschritt, Blöcke bis Retarget |
| 7 | Uhr | Uhrzeit, Wochentag, Datum |
| 8 | Bitaxe | Vollbild: Name, IP/Status, Hashrate, Session/Best vs. Netzwerk, ASIC-/VR-Temperatur, Lüfter |
| 9 | Share-Lotterie | jeder vom Bitaxe gefundene Hash live als Punkt im Histogramm (log. Skala) – links häufig, rechts selten, ganz rechts der Block; letzter/bester Hash, Hashes pro Minute, Abstand zum Block |

Gestaltung im Stil von Strike (eigener Code): tiefschwarzer Grund, gedämpftes Weiß, Geist für große Zahlen,
Geist Mono für Beschriftungen, feine Rahmen, Punktmatrix-Chart, Pixel-Mosaik als Kursverlauf.
Einzige Akzentfarbe: softes Pink `#FFD4D4` (sRGB 255/212/212). Kursänderungen in gedämpftem Grün/Rot.

Ohne Bitaxe zeigt das Display Demo-Werte (Kennzeichen **DEMO**, türkis). Ist ein Bitaxe eingetragen,
aber nicht erreichbar, steht dort **OFFLINE** in Rot, mit dem Grund; auch dann laufen Demo-Werte.

## Boot-Screen

Nach jedem Start zeigt das Display etwa 10 Sekunden lang einen Boot-Screen: Pixel-Mosaik in einer
diagonalen Welle, Schriftzug „Satoshi Deck“ und der echte Startvorgang (Display, WLAN, Bitaxe, Kurs,
Block). Ein Tippen überspringt ihn.

## Share-Lotterie

Das Display liest den Log-Stream des Bitaxe (`ws://<IP>/api/ws`) und wertet jede Zeile
`asic_result: … diff 726.3 of 8192.` aus. Jeder Hash, den der ASIC meldet, fällt als pinker Punkt
in seine Säule. Je weiter rechts, desto seltener – die pinke Linie ganz rechts ist die
Netzwerk-Difficulty, also ein Block. Ohne erreichbaren Bitaxe wird die Verteilung simuliert
(Anzeige „SIMULIERT“ statt „LIVE“).

## Einstellungen am Gerät

**Finger ca. 0,7 s auf eine Karte halten** öffnet die Konfiguration (auf dem Chart selbst öffnet langes Halten das Fadenkreuz – dort oben auf Preis oder Uhrzeit drücken):

- QR-Code bzw. IP zur Einstellungsseite im Browser
- **Währung** EUR/USD (Preis, Chart, Gebühren, Sats-Umrechnung)
- **Preisalarm** ein/aus – Grenzen oben/unten auf der Browser-Seite
- **Seitenwechsel**: blättert automatisch weiter, wenn niemand das Display berührt (AUS, 5 s … 5 min)
- **Nightmode**: dimmt im Zeitfenster (Standard 22–7 Uhr, im Browser änderbar); Berührung weckt
- **Neuer Block**: kurzer Hinweis mit Blockhöhe und Pool
- **Chart**: Zeitraum 24h / 7T / 30T / 1J
- unten: Kartenmenü (Raster-Symbol), Helligkeit, Schließen (✕)

Im **Kartenmenü** springt ein Tippen direkt zur Karte; die aktuelle ist blau markiert. Zahnrad bzw. „Einstellungen“ führen zur Konfiguration.

Im Browser lässt sich außerdem die **Chart-Fläche** wählen: Punkte (Standard) oder ohne Verlauf (nur die Linie).

## Einrichtung (einmalig)

1. Board per USB-C mit Strom versorgen. Ohne WLAN öffnet sich die Konfiguration.
2. QR-Code mit dem Handy scannen (verbindet direkt mit dem WLAN **`SatoshiDeck-Setup`**) oder von Hand verbinden, Passwort **`satoshi21`**.
3. Im Browser **http://192.168.4.1** öffnen.
4. Heim-WLAN (2,4 GHz), WLAN-Passwort und die **IP-Adresse deines Bitaxe** (z. B. `192.168.1.42`) eintragen → *Speichern & neu starten*.

Später lassen sich die Einstellungen im Heimnetz unter `http://<IP des Displays>` ändern; IP und QR-Code zeigt die Konfiguration (langer Druck).
Ein Hostname wie `bitaxe.local` funktioniert auf dem ESP32 nicht, bitte die IP eintragen.

Klappt die WLAN-Verbindung 25 Sekunden lang nicht, öffnet das Display das Setup-WLAN wieder.

## Updates (OTA)

- Flash-Aufteilung: zwei App-Bereiche à 6 MB (`ota_0`, `ota_1`) und `otadata`; die Einstellungen (NVS) liegen unverändert bei `0x9000`.
- `main/ota.c` fragt alle 6 h `https://api.github.com/repos/<repo>/releases/latest` ab (Repo: `CONFIG_DECK_OTA_REPO`, im GitHub-Build automatisch gesetzt) und vergleicht mit `version.txt`.
- Installation über `esp_https_ota` mit Zertifikatsprüfung; danach Neustart.
- Rollback: Neue Firmware wird erst nach 60 s mit WLAN bestätigt, sonst startet der Bootloader die vorherige.
- Am Gerät: rosa Knopf „UPDATE x.y.z →“ oben rechts in den Einstellungen. Im Browser: Abschnitt „Firmware“.
- Automatisch (Standard, im Browser abschaltbar): Installation, sobald 5 Minuten niemand das Display berührt hat.

## Bauen und flashen

```bash
cd ~/BitCraft/firmware
. ~/Documents/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash monitor
```

`monitor` beendest du mit `Ctrl + ]`.

Falls sich das Board nicht flashen lässt („No serial data received“): USB-Kabel kurz ab- und wieder anstecken, dann erneut flashen.

**Display steht auf dem Kopf?** `idf.py menuconfig` → *Satoshi Deck* → *Display um 180° drehen*, dann neu flashen. Der Touch dreht sich mit.

## Dateien

| Datei | Aufgabe |
|-------|---------|
| `main/board.c` | Pins, ST7796 über i80, FT6336U-Touch über I2C, LVGL-Anbindung |
| `main/ui.c` | alle Karten im Strike-Stil, Chart und Mosaik als vorgerenderte Bilder, Einstellungen, Menü |
| `main/data.c` | Abruf von CoinGecko, mempool.space und der AxeOS-API, Demo-Werte |
| `main/net.c` | WLAN, Uhrzeit (NTP), Setup-Portal |
| `main/settings.c` | Einstellungen im NVS |
| `main/fonts/` | mit `lv_font_conv` erzeugte Schriften |

Pinbelegung: LCD D0–D7 = GPIO 9, 46, 3, 8, 18, 17, 16, 15 · WR 47 · DC 0 · RST 4 · Backlight 45 ·
Touch SDA 6 · SCL 5 · INT 7.

## Messen und Screenshots

Für Entwicklung: `idf.py menuconfig` → *Satoshi Deck*
- *Bildrate im Log ausgeben* + *Automatisch durch die Karten blättern*: Renderzeit pro Karte im Log
- *Screenshots aller Karten über USB ausgeben*: danach `python3 tools/snapshots.py /dev/cu.usbmodem1101 screenshots` (mit der Python-Umgebung von ESP-IDF, braucht `pyserial`)
