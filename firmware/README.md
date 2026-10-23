# Satoshi Deck – Firmware für das WT32-SC01 Plus

Das Satoshi-Deck-Dashboard als eigenständige Firmware für das **Smart Panlee / WT32-SC01 Plus**
(Aufdruck ZX3D50CE08S-V16, ESP32-S3, 3,5" IPS 480 × 320, ST7796 über 8-Bit-MCU8080, Touch FT6336U).
Das Display holt sich die Daten selbst über WLAN, ein Rechner muss dafür nicht laufen.

**Technik:** ESP-IDF 5.3 · LVGL **8.4.0** (über den Component Manager, `main/idf_component.yml`) ·
`espressif/esp_lcd_st7796` 1.5.0 · eigene Schriften: Geist und Geist Mono (SIL OFL, Quelle in `fonts-src/`)

## Karten

Alle Karten füllen den ganzen Bildschirm (ohne Kopfzeile und Überschriften). Zum Blättern nach links oder
rechts wischen oder auf den linken bzw. rechten Rand tippen (je ein Viertel der Breite). Die Karten laufen
endlos im Kreis: nach der letzten kommt wieder die erste, in beide Richtungen. Ausgeschaltete Karten werden
übersprungen. Die Reihenfolge lässt sich frei festlegen – im Kartenmenü am Display oder im Browser (siehe
„Einstellungen am Gerät“); die Nummern unten gelten für die Standard-Reihenfolge.

| # | Karte | Inhalt |
|---|-------|--------|
| 1 | Preis | Kurs groß, 24h-Änderung, Kurs in der anderen Währung, Kursverlauf als Pixel-Mosaik |
| 2 | USD & EUR | beide Kurse mit 24h-Änderung |
| 3 | Chart | Kursverlauf als Punktmatrix (oder als Pixel-Mosaik wie auf der Preiskarte), Hoch/Tief, letzter Wert in Pink; Tippen auf „24h“ wechselt zu 7T / 30T / 1J; Finger halten und ziehen zeigt Preis und Zeit |
| 4 | Block | Blockhöhe, Alter und Pool des letzten Blocks, Blöcke bis zum Halving |
| 5 | Neuer Block | Kette der letzten fünf Blöcke als Pixel-Blöcke (der neueste pink), Blockhöhe groß, Pool, Transaktionen, Belohnung, Uhrzeit und Alter; bei einem neuen Block läuft die Animation |
| 6 | Halving | Vollbild aus Pixel-Sand: unten die schon geschürften Blöcke der Epoche, darüber die restlichen; Blöcke bis zum Halving groß, Zielblock, geschätztes Datum, Belohnung vorher/nachher |
| 7 | Blockzeit & Gebühren | Blockhöhe, Sats pro USD, Gebühren niedrig/mittel/hoch in sat/vB und $ |
| 8 | Difficulty-Anpassung | erwartete Änderung, Fortschritt, Blöcke bis Retarget |
| 9 | Uhr | LED-Punktanzeige (Standard): Siebensegment-Ziffern aus quadratischen LED-Punkten in der Trendfarbe (hellgrün gestiegen, pink gefallen), Sekundenreihe, Datum; wahlweise klassisch |
| 10 | Wetter | aktuelles Wetter am eingestellten Ort mit bewegtem Pixel-Symbol, Temperatur, Wetterlage, heute Hoch/Tief, gefühlt, Wind, Regenwahrscheinlichkeit; darunter die nächsten 5 Tage (Daten: Open-Meteo) |
| 11 | Bitaxe | Vollbild: Name, IP/Status, Hashrate, Session/Best vs. Netzwerk, ASIC-/VR-Temperatur, Lüfter |
| 12 | Mining-Kosten | aktueller Verbrauch des Bitaxe in W, Kosten pro Tag/Monat/Jahr samt kWh, Tageskosten in sats („Lotterielos“), gezählter Verbrauch heute und gesamt |
| 13 | Share-Lotterie | jeder vom Bitaxe gefundene Hash live als Punkt im Histogramm (log. Skala) – links häufig, rechts selten, ganz rechts der Block; letzter/bester Hash, Hashes pro Minute, Abstand zum Block |
| 14 | News | Bitcoin-Schlagzeilen (Crypto Vision, Blocktrainer oder Bitcoin Magazine), vier je Seite; zu jeder Meldung ihr Alter in Blöcken und wie sich der Kurs seitdem bewegt hat (Prozent und Mini-Kurve) |

Gestaltung im Stil von Strike (eigener Code): tiefschwarzer Grund, gedämpftes Weiß, Geist für große Zahlen,
Geist Mono für Beschriftungen, feine Rahmen, Punktmatrix-Chart, Pixel-Mosaik als Kursverlauf.
Einzige Akzentfarbe: softes Pink `#FFD4D4` (sRGB 255/212/212). Kursänderungen in gedämpftem Grün/Rot.

Ohne Bitaxe zeigt das Display Demo-Werte (Kennzeichen **DEMO**, türkis). Ist ein Bitaxe eingetragen,
aber nicht erreichbar, steht dort **OFFLINE** in Rot, mit dem Grund; auch dann laufen Demo-Werte.

## Boot-Screen

Nach jedem Start zeigt das Display einen Boot-Screen: Pixel fallen von oben und bilden ein Mosaik-Band am oberen Rand,
Schriftzug „Satoshi Deck“ und der echte Startvorgang (Display, WLAN, Bitaxe, Kurs, Block). Er bleibt
stehen, bis alle Daten geladen sind (WLAN mit Uhrzeit, Bitaxe erreicht oder als offline erkannt, Kurs
und Chart, Blockdaten), mindestens etwa 5 Sekunden und höchstens 45 Sekunden. Ein Tippen überspringt ihn.

## Mining-Kosten

Die Karte rechnet aus der aktuellen Leistung des Bitaxe (AxeOS-Wert `power`) die Kosten pro Tag,
Monat (365/12 Tage) und Jahr hoch, jeweils mit den kWh dazu. Rechts oben steht der Strompreis
(Standard **30 ct/kWh**); einstellen lässt er sich auf der Browser-Seite unter „Mining-Kosten“.
Gerechnet wird in Euro, unabhängig von der Kurswährung;
auch die sats pro Tag beziehen sich auf den Euro-Kurs.

Der **Stromzähler** summiert die gemessene Leistung, solange der Bitaxe echte Werte liefert (nicht im
Demo- oder Offline-Zustand), und zeigt „heute“ (ab Mitternacht) und „gesamt seit …“ mit den Kosten.
Er wird alle 10 Minuten und vor jedem Neustart im Flash gesichert und lässt sich auf der Browser-Seite
zurücksetzen.

## Neuer Block

Eigene Karte mit der Kette der letzten fünf Blöcke als Pixel-Blöcke; der neueste ist pink, rechts
wartet ein freier Platz auf den nächsten. Darunter die Blockhöhe, Pool, Transaktionen und Belohnung,
oben Uhrzeit und Alter („NEUER BLOCK“ in den ersten 10 Minuten, danach „LETZTER BLOCK“).

Kommt ein Block, fällt ein pinker Pixel-Block auf den freien Platz, die Verbindung leuchtet auf,
die Blockhöhe rollt wie ein Zählwerk auf den neuen Wert, danach rückt die Kette einen Platz weiter.
Ist in den Einstellungen „Neuer Block“ eingeschaltet, springt das Display dafür zu dieser Karte.

## Halving

Die ganze Karte ist ein Feld aus Pixel-Sand und steht für die laufende Halving-Epoche (210.000
Blöcke, eine Zelle ≈ 88 Blöcke). Unten liegen die schon geschürften Blöcke als Sand mit gerader
Kante (ohne helle Oberkante): volle Reihen, die oberste füllt sich von links nach rechts, an der Stufe liegt das neueste
Korn in Pink. Der freie Raum darüber sind die restlichen Blöcke bis zum Halving. Körner rieseln
ständig herab; bei jedem neuen Block fällt ein pinkes Korn auf die Stufe. Groß stehen die Blöcke
bis zum Halving, knapp unter der Kante der Anteil der Epoche, unten Zielblock, geschätztes Datum
(aus der mittleren Blockzeit von mempool.space) und die Tage bis dahin, oben rechts die
Blockbelohnung vorher → nachher. Die Beschriftungen liegen so, dass die Kante frei bleibt; hinter
ihnen sind die Zellen ausgespart.

## Uhr: LED-Punktanzeige

Standard-Uhr im Stil einer klassischen LED-Anzeige, passend zur Punktmatrix des Charts: Die Ziffern sind
Siebensegment-Ziffern aus quadratischen LED-Punkten (7 × 13 Punkte je Ziffer, 8 px groß), wie die Punkte überall im Deck; leuchtende Punkte glimmen leicht. Unbeleuchtete Segmente schimmern
schwach, dahinter liegt ein feines Punktraster wie auf einem LED-Panel. Beim Minutenwechsel blenden die
Segmente kurz über, darunter füllt sich eine Reihe aus 60 kleinen LEDs – eine pro Sekunde.

**24h-Trend:** Die LEDs leuchten in der Farbe der Kursänderung der letzten 24 Stunden – hellgrün
(`#9BE3B0`), wenn der Bitcoin-Kurs gestiegen ist, pink (`#FFD4D4`), wenn er gefallen ist. Unter dem
Datum steht die Änderung („BTC · 24H +2,34 %“).

Auf der Browser-Seite lässt sich unter „Uhr“ stattdessen die klassische Uhr wählen; sie zeigt die
Änderung ebenfalls farbig unter dem Datum.

## Wetter

Aktuelles Wetter und die Aussicht für die nächsten 5 Tage von [Open-Meteo](https://open-meteo.com/)
(kostenlos, ohne Schlüssel; Daten unter CC BY 4.0). Den **Ort** trägt man auf der Browser-Seite unter
„Wetter“ ein: Name („Berlin“), Postleitzahl („80331“), mit Ländercode bei gleichnamigen Orten
(„Frankfurt, DE“) oder Koordinaten („52.52, 13.41“). Das Display sucht den Ort einmal nach dem Start und
zeigt auf der Browser-Seite, was es gefunden hat. Die Wetterdaten lädt es alle 15 Minuten neu.

Die Wettersymbole sind Pixel-Mosaike aus 13 × 13 Zellen wie der Kursverlauf auf der Preiskarte: Sonne
und Blitz in Pink, Wolken, Regen, Schnee und Nebel in Graustufen, nachts Mond statt Sonne. Das große
Symbol bewegt sich leicht – Regen und Schnee fallen, die Strahlen der Sonne glimmen im Wechsel, Nebel
zieht, bei Gewitter blitzt es. Rechts daneben stehen Hoch/Tief von heute, die gefühlte Temperatur, Wind
und die Regenwahrscheinlichkeit, unten die nächsten 5 Tage mit Hoch/Tief und Regenwahrscheinlichkeit.
Der Punkt neben dem Ort ist pink, solange die Daten aktuell sind (grau: älter als eine Stunde).

## Bitcoin-News

Die neuesten Bitcoin-Schlagzeilen, vier je Seite; die Seiten wechseln alle 9 Sekunden, ein Tippen in die
Mitte der Karte blättert sofort weiter. Die Quelle wird auf der Browser-Seite unter „News“ gewählt:

- **Crypto Vision** ([cryptocurrency.cv](https://cryptocurrency.cv), auch „Free Crypto News API“; Standard):
  kostenlos und ohne Schlüssel, sammelt Bitcoin-Meldungen aus vielen Quellen (englisch). Forums-Threads
  von Stacker News werden ausgelassen.
- **Blocktrainer** (deutsch) und **Bitcoin Magazine** (englisch) über ihre RSS-Feeds.

CryptoCompare und die CoinDesk-API verlangen inzwischen einen Schlüssel und sind deshalb nicht dabei.
Die Schlagzeilen werden alle 10 Minuten neu geladen, nur wenn die Karte eingeschaltet ist.

**Kurs seit Meldung:** Rechts neben jeder Schlagzeile steht, wie sich der Bitcoin-Kurs seit ihrer
Veröffentlichung bewegt hat – in Prozent und als Mini-Kurve aus Punkten, hellgrün gestiegen, pink
gefallen (Kursverlauf der letzten 7,5 Tage in 15-Minuten-Schritten von der öffentlichen Kraken-API, bis
zum aktuellen Kurs ergänzt). **Zeit in
Blöcken:** Statt „vor 40 Minuten“ steht über der Schlagzeile „VOR 4 BLÖCKEN“, gezählt an den letzten
Blöcken von mempool.space, für ältere Meldungen mit 10 Minuten je Block geschätzt; ist seit der Meldung
noch kein Block gefunden, steht dort ein pinkes „NEU“.

## Nachtmodus

Im eingestellten Zeitfenster schaltet das Display nach 30 Sekunden ohne Berührung auf schwache
Beleuchtung (12 %) und zeigt alles in einer sanften Farbe. Dafür wird jedes Pixel kurz vor dem
Übertragen ans Display in eine Helligkeitsstufe der gewählten Farbe umgerechnet – das gilt für alle
Karten, Animationen und Hinweise. Eine Berührung bringt Helligkeit und Originalfarben zurück.

Farbe auf der Browser-Seite unter „Farbe im Nachtmodus“: **Bernstein** (Standard, `#FFB000`),
**Tiefrot** (`#FF2A1A`, schont die Nachtsicht am meisten), **Kerzenlicht** (`#FFB46B`),
**Rosé** (`#F2A6A6`), **Phosphor** (`#4CFF7A`), **Eisblau** (`#9CC8FF`) oder **Originalfarben**
(nur gedimmt).

## Share-Lotterie

Das Display liest den Log-Stream des Bitaxe (`ws://<IP>/api/ws`) und wertet jede Zeile
`asic_result: … diff 726.3 of 8192.` aus. Jeder Hash, den der ASIC meldet, fällt als pinker Punkt
in seine Säule. Je weiter rechts, desto seltener – die pinke Linie ganz rechts ist die
Netzwerk-Difficulty, also ein Block. Ohne erreichbaren Bitaxe wird die Verteilung simuliert
(Anzeige „SIMULIERT“ statt „LIVE“) – erst, wenn 60 Sekunden lang keine echte Share kam.

Bricht der Log-Stream still ab (AxeOS schließt die Verbindung oder sie hängt), merkt das ein Wächter:
Kommen 30 Sekunden lang keine Daten, baut das Deck die Verbindung neu auf. Verbindungsabbrüche und
alle 5 Minuten die Zahl der empfangenen Zeilen und Shares stehen im seriellen Log.

## Einstellungen am Gerät

**Finger ca. 0,7 s auf eine Karte halten** öffnet die Konfiguration (auf dem Chart selbst öffnet langes Halten das Fadenkreuz – dort oben auf Preis oder Uhrzeit drücken):

- QR-Code bzw. IP zur Einstellungsseite im Browser
- oben rechts **Version · PRÜFEN**: sucht sofort nach Updates (zeigt „SUCHE …“, dann „✓ AKTUELL“ oder **UPDATE x →**, Tippen darauf installiert)
- **Währung** EUR/USD (Preis, Chart, Gebühren, Sats-Umrechnung)
- **Preisalarm** ein/aus – Grenzen oben/unten auf der Browser-Seite
- **Seitenwechsel**: blättert automatisch weiter, wenn niemand das Display berührt (AUS, 5 s … 5 min)
- **Nachtmodus**: im Zeitfenster (Standard 22–7 Uhr) nach 30 s ohne Berührung schwache Beleuchtung und alles in einer sanften Farbe (siehe unten); Berührung weckt
- **Neuer Block**: springt bei einem neuen Block zur Karte „Neuer Block“ mit Animation (siehe oben)
- **Chart**: Zeitraum 24h / 7T / 30T / 1J
- unten: Kartenmenü (Raster-Symbol), Helligkeit, Schließen (✕)

Im **Kartenmenü** springt ein Tippen direkt zur Karte; die aktuelle ist pink umrandet. Mit dem Schalter
oben rechts in jeder Kachel lässt sich die Karte ein- oder ausschalten; ausgeschaltete Karten erscheinen
weder beim Blättern noch beim automatischen Seitenwechsel. Eine Karte bleibt immer an. **Kachel halten
(0,7 s) und ziehen** ordnet die Karten um: Die Kachel hebt sich pink umrandet ab, die anderen rücken
nach, beim Loslassen ist die neue Reihenfolge gespeichert – in ihr wird geblättert, und mit der ersten
startet das Deck. Zahnrad bzw. „Einstellungen“ führen zur Konfiguration.

Im Browser lässt sich außerdem die **Chart-Fläche** wählen: Punkte (Standard), ohne Verlauf (nur die Linie)
oder Pixel (Säulen aus Pixel-Zellen wie das Mosaik der Preiskarte).
Unter **Wetter** wird der Ort fürs Wetter eingetragen, unter **News** die Quelle der Schlagzeilen (siehe oben).
Unter **Karten** stehen dort dieselben Ein/Aus-Schalter wie im Kartenmenü, in der eingestellten
Reihenfolge; mit ↑ und ↓ lassen sich die Karten auch dort verschieben.

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
- Am Gerät: Knopf oben rechts in den Einstellungen – „v… · PRÜFEN“ sucht sofort, bei einem Fund wird er zum rosa „UPDATE x.y.z →“ (installiert). Im Browser: Abschnitt „Firmware“.
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
| `main/data.c` | Abruf von CoinGecko, mempool.space, der AxeOS-API, Open-Meteo (Wetter) und der News (JSON und RSS), Demo-Werte |
| `main/net.c` | WLAN, Uhrzeit (NTP), Setup-Portal |
| `main/settings.c` | Einstellungen im NVS |
| `main/fonts/` | mit `lv_font_conv` erzeugte Schriften |

Pinbelegung: LCD D0–D7 = GPIO 9, 46, 3, 8, 18, 17, 16, 15 · WR 47 · DC 0 · RST 4 · Backlight 45 ·
Touch SDA 6 · SCL 5 · INT 7.

## Messen und Screenshots

Für Entwicklung: `idf.py menuconfig` → *Satoshi Deck*
- *Bildrate im Log ausgeben* + *Automatisch durch die Karten blättern*: Renderzeit pro Karte im Log
- *Screenshots aller Karten über USB ausgeben*: danach `python3 tools/snapshots.py /dev/cu.usbmodem1101 screenshots` (mit der Python-Umgebung von ESP-IDF, braucht `pyserial`)
