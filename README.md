# BarBot – Technische Dokumentation

Automatischer Schnaps-Roboter auf Basis eines ESP32 – ausschließlich für Schnaps, keine Mischgetränke.
Steuert 3 Pumpen und einen Servoarm zum Befüllen von bis zu 12 Gläsern in einer halbkreisförmigen Anordnung.

---

## Projektübersicht

Der BarBot nimmt bis zu 12 Gläser in einer U-förmigen Anordnung auf. Ein Servoarm schwenkt über jede Position und schenkt das konfigurierte Getränk ein. Die Getränkeauswahl erfolgt über drei Wege:

- **CYD-Touchdisplay** (direkt am Gerät)
- **Web-Oberfläche** (im lokalen Netzwerk)
- **Handy-App** über MQTT (Cloud-basiert, auch außer Haus nutzbar)

---

## Dateistruktur

| Datei | Beschreibung |
|-------|--------------|
| `BarBot_v3_10_kommentare_anleitung.ino` | Hauptprogramm: Zustandsmaschine, Button, Screensaver, Flaschenlogik |
| `DrinkMachine.h` | Hardware-Abstraktion: Servo, Pumpen, LEDs, UART-Display, NVS |
| `Connectivity.h` | WiFi, Webserver (async), WebSocket, MQTT, OTA |
| `config.h` | Alle Pin-Definitionen und Zeitkonstanten |
| `index.html` | Handy-App (MQTT-basiert, Cloud-fähig) |
| `settings.html` | Einstellungen: Namen, MQTT, Hardware, Kalibrierung, Flaschen |
| `portal.html` | Captive-Portal für WLAN-Ersteinrichtung |
| `filemanager.html` | LittleFS-Dateimanager im Browser |
| `ota.html` | Firmware-Update über Browser |
| `display_upload.html` | Bild zuschneiden und als LVGL-.bin ans Display hochladen |
| `pages.jsonl` | openHASP-Seitendefinition für das CYD-Display |

---

## Hardware

### Pin-Belegung (ESP32)

| Funktion | Pin(s) |
|----------|--------|
| Pumpe 1 (A / B) | 18, 23 |
| Pumpe 2 (A / B) | 17, 4 |
| Pumpe 3 (A / B) | 16, 19 |
| Servoarm | 2 |
| WS2812 LED-Ring (12x) | 15 |
| UART TX → Display | 21 |
| UART RX ← Display | 22 |
| Taster (Start / Weiter) | 5 |
| Endschalter 1–4 | 13, 12, 14, 27 |
| Endschalter 5–7 | 26, 25, 33 |
| Endschalter 8–10 | 32, 35, 34 |
| Endschalter 11–12 | 39, 36 |

> Pins 34–39 am ESP32 sind input-only und haben keinen internen Pullup.

### Display

CYD (Cheap Yellow Display) mit **openHASP**-Firmware, verbunden über UART (115200 Baud, 8N1).
Getränkebilder liegen als LVGL-`.bin` auf der SD-Karte des Displays (`L:/drink0.bin` etc.).
→ Upload über `display_upload.html`

---

## openHASP-Seiten

| Seite | Beschreibung |
|-------|--------------|
| 1 | Klassik-Startseite: Getränk wählen, Start, Warnbanner |
| 2 | Einschenk-Fortschritt (Balken + Status) |
| 3 | Einstellungen (Reset, Modi, Füllmenge, Display-IP, MQTT-Status) |
| 5 | Priming (Schlauchbefüllung, 3 Pumpen gleichzeitig) |
| 6 | Halbkreis-Ansicht: 12 Positionen + Getränkewahl, Warnbanner |
| 7 | Einzelslot-Zuweisung (Getränk für eine Position) |
| 8 | Füllmengen-Einstellung (±0,5 cl) |
| 9 | Screensaver (springendes Getränkebild) |
| 11 | Überschreib-Bestätigung (Ja / Nein Dialog) |

---

## Zustandsmaschine

```
IDLE  ──[Start]──►  POURING_SEQUENCE  ──[fertig/stop]──►  IDLE
  │                                                          │
  └──[Prime]──►  PRIMING  ──────────────────[stop]──────────┘
```

- **IDLE**: LEDs zeigen Glaserkennung, Display-Updates, Button-Handling, Flaschen-Warnungen
- **POURING_SEQUENCE**: Arm fährt Position für Position ab, Pumpe läuft für berechnete Zeit (cl × msPerCl)
- **PRIMING**: Alle 3 Pumpen reagieren unabhängig auf Glaserkennung an Positionen 1, 6, 12

Nach einem erfolgreichen Durchlauf: **Glasentnahme-Sperre** aktiv, bis alle befüllten Gläser entnommen wurden.

---

## Kalibrierung

Standard: **2000 ms pro cl**. Kann in den Einstellungen angepasst werden.

**Vorgehen:**
1. Pumpe X Sekunden einschenken lassen (z.B. 10 s)
2. Ausgegossene Menge messen (z.B. 5 cl)
3. `ms/cl = 10000 ms ÷ 5 cl = 2000 ms/cl`
4. Wert in Settings → Kalibrierung & Flaschen eintragen und speichern

---

## NVS-Schlüssel (Preferences)

### Namespace `barbot` (DrinkMachine)

| Schlüssel | Typ | Beschreibung |
|-----------|-----|--------------|
| `d1`, `d2`, `d3` | String | Getränkenamen |
| `cydMode` | Bool | Halbkreis-Ansicht aktiv |
| `wifiSync` | Bool | WLAN-Daten ans Display senden |
| `pourTime` | Int | Einschenkzeit pro Glas in ms |
| `msPerCl` | Int | Kalibrierung (Standard: 2000) |
| `dispIP` | String | Zuletzt bekannte Display-IP |
| `btl0`, `btl1`, `btl2` | Int | Flaschengröße in ml |
| `dis0`, `dis1`, `dis2` | Int | Ausgegossene Menge in ml |

### Namespace `barbot_cfg` (Connectivity)

| Schlüssel | Typ | Beschreibung |
|-----------|-----|--------------|
| `w_ssid`, `w_pass` | String | WLAN-Zugangsdaten |
| `mq_en` | Bool | MQTT aktiviert |
| `mq_srv` | String | MQTT-Broker-IP / Hostname |
| `mq_prt` | Int | MQTT-Port (Standard: 1883, TLS: 8883) |
| `mq_usr`, `mq_pwd` | String | MQTT-Zugangsdaten |
| `mq_tls` | Bool | SSL/TLS für MQTT (z.B. HiveMQ Cloud) |

---

## MQTT-Topics

| Topic | Richtung | Beschreibung |
|-------|----------|--------------|
| `barbot/status` | pub | `IDLE` / `BUSY` / `OFFLINE` |
| `barbot/event` | pub | `DONE` / `STOPPED` |
| `barbot/activeSlot` | pub | Aktuell befüllte Position (0–11, -1 = keine) |
| `barbot/activeDrink` | pub | Aktuelles Getränk (0–2, -1 = keines) |
| `barbot/glass/N` | pub | `ON` / `OFF` für Glas an Position N |
| `barbot/slot/N` | pub | Zugewiesenes Getränk an Position N |
| `barbot/drink/N/name` | pub | Getränkename (retained) |
| `barbot/drink/N/image` | pub | URL des Getränkebilds (retained) |
| `barbot/cmd` | sub | `start` / `stop` |
| `barbot/slot/N/set` | sub | Getränk-ID für Position N setzen |
| `barbot/pairing/pending` | pub | Glas erkannt im Kopplungsmodus: `{"slot": N}` |
| `barbot/pairing/accept` | sub | Handy bestätigt: `{"slot": N, "client_id": "..."}` |
| `barbot/pairing/success` | pub | Kopplung erfolgreich |
| `barbot/identify` | sub | Slot N blinken lassen (5 s orange) |

---

## Handy-Kopplung (Phone Pairing)

1. Kopplungsmodus am Display aktivieren (Seite 3 → „Handy Koppeln")
2. Handy-App abonniert automatisch `barbot/pairing/pending`
3. Glas auf freien Stellplatz stellen → ESP32 sendet `{"slot": N}`
4. Handy zeigt „Neues Glas erkannt" → Nutzer bestätigt per Button
5. App publiziert auf `barbot/pairing/accept`
6. ESP32 merkt sich die Zuordnung und bestätigt via `barbot/pairing/success`
7. Kopplungsmodus beendet sich automatisch

**Slot identifizieren:** In der App auf die Stellplatz-Nummer tippen → LED blinkt 5 s orange.

---

## Web-API

| Endpunkt | Methode | Beschreibung |
|----------|---------|--------------|
| `/` | GET | Haupt-UI (oder Portal im AP-Modus) |
| `/app` | GET | Haupt-UI (auch im AP-Modus) |
| `/settings` | GET | Einstellungsseite |
| `/files` | GET | Datei-Manager |
| `/ota` | GET | OTA-Update |
| `/display_upload` | GET | Display-Bild-Upload |
| `/api/status` | GET | Status-Snapshot (JSON) |
| `/api/config` | GET | Konfiguration (JSON): Namen, MQTT, Flaschen, Kalibrierung |
| `/api/saveConfig` | POST | Konfiguration speichern (JSON-Body) |
| `/api/start?id=N` | GET | Einschenken starten |
| `/api/stop` | GET | Einschenken stoppen |
| `/api/setSlot?id=N&val=D` | GET | Slot N auf Getränk D setzen |
| `/api/setAll?val=D` | GET | Alle Slots auf Getränk D setzen |
| `/api/resetBottle?drink=N` | GET | Flaschen-Mengenzähler zurücksetzen |
| `/api/upload?type=X` | POST | Bild hochladen (bg, d0, d1, d2) |
| `/api/uploadAny` | POST | Beliebige Datei in LittleFS |
| `/api/files` | GET | Dateiliste (JSON) |
| `/api/fsinfo` | GET | Speicher-Info (JSON) |
| `/api/deleteFile?name=X` | GET | Datei löschen |
| `/api/ota` | POST | Firmware-Binary flashen |
| `/api/scan` | GET | WLAN-Scan-Ergebnisse (JSON) |
| `/api/resetNames` | POST | Getränkenamen + Bilder zurücksetzen |

---

## Flaschen-Mengenzähler

Zählt pro Pumpe die ausgegossene Menge und warnt bei 80% Leerstand:

- **Orange Warnbanner** auf Display-Seite 1 und 6
- **„(!)"-Suffix** hinter dem Getränkenamen auf dem Display
- **Rote Balken** in der Web-Oberfläche bei ≥ 80%

Flaschengrößen wählbar: 250 / 330 / 500 / 700 / 1000 / 1500 ml.
Reset einzeln pro Flasche über Settings oder `/api/resetBottle?drink=N`.

---

## Ersteinrichtung

1. Firmware flashen (Arduino IDE, Board: ESP32 Dev Module)
2. LittleFS-Dateien hochladen (`Sketch → Upload Filesystem Image`)
3. Beim ersten Start ohne gespeicherte WLAN-Daten startet der ESP32 als Access Point
   → SSID `Barbot`, Passwort `barbot123`
4. Mit dem Gerät verbinden → Browser öffnet automatisch das Portal (`192.168.4.1`)
5. WLAN-Zugangsdaten eingeben, Getränkenamen setzen, optional MQTT konfigurieren
6. Nach Neustart: Getränkebilder über `/display_upload` hochladen
7. Schläuche über Seite 3 → „Schlauch Befüllen" fluten

**WLAN-Reset**: Physischen Button beim Einschalten gedrückt halten → alle Zugangsdaten werden gelöscht.

---

## Arduino-Bibliotheken

- `ESP32Servo`
- `Adafruit NeoPixel`
- `ESPAsyncWebServer` + `AsyncTCP`
- `PubSubClient`
- `ArduinoJson`
- `LittleFS` (im ESP32-Core)
- `WiFiClientSecure` (im ESP32-Core, für MQTT TLS)
