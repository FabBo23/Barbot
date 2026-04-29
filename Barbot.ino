// =============================================================================
// BarBot – Hauptprogramm
// =============================================================================
// Koordiniert die drei Zustände (IDLE / POURING_SEQUENCE / PRIMING),
// verarbeitet Befehle vom Display (openHASP via UART) und vom Web (Connectivity),
// steuert den Screensaver und liest den physischen Button.
//
// Abhängigkeiten:
//   config.h       – Pin-Definitionen und Konstanten
//   DrinkMachine.h – Hardware-Abstraktion (Servo, Pumpen, LEDs, Display)
//   Connectivity.h – WiFi, Webserver, WebSocket, MQTT
// =============================================================================

#include <Arduino.h>
#include "config.h"
#include "DrinkMachine.h"
#include "Connectivity.h"

DrinkMachine bot;
Connectivity net(&bot);

// =============================================================================
// GLOBALE ZUSTÄNDE
// =============================================================================

// --- Physischer Button ---
int buttonState     = HIGH;
int lastButtonState = HIGH;
unsigned long lastDebounceTime = 0;
// buttonHeld: reserviert für Long-Press-Funktion (LONG_PRESS_TIME in config.h), noch nicht aktiv
bool buttonHeld = false;

// --- Hauptzustand ---
enum State { IDLE, POURING_SEQUENCE, PRIMING };
State currentState  = IDLE;
bool stopRequested  = false;

// --- Getränkeauswahl ---
int globalSelectedDrink = 0;
uint32_t menuColors[3];  // LED-Farben für das Menü (Blau, Rot, Gelb)

// --- Halbkreis-Modus (Seite 6) ---
int  lastSlotConfig[12];
bool lastGlassState[12];
bool needsSemiCircleUpdate = false;

// --- Handy Kopplung ---
bool pairingLastGlass[12] = {false};

// --- Identifikation (Blinken) ---
int identifySlotIdx = -1;
unsigned long identifyStartTime = 0;

// --- Glasentnahme-Sperre ---
// Nach einem erfolgreichen Durchlauf muessen alle befuellten Glaeser
// entnommen werden, bevor ein neuer Start erlaubt ist.
bool waitingForGlassRemoval = false;
bool filledSlots[12];  // Welche Slots wurden im letzten Durchlauf befuellt?

// =============================================================================
// SCREENSAVER
// =============================================================================

// Timing-Konstanten
#define SCREENSAVER_TIMEOUT_MS  (1UL * 60UL * 1000UL)  // Inaktivitätszeit bis Screensaver
#define SCREENSAVER_MOVE_MS     3000                     // Bildposition alle 3s verschieben
#define SCREENSAVER_IMG_MS      30000                    // Bild alle 30s wechseln

// Display-Abmessungen und Bildgröße für Bounce-Berechnung
#define SS_MAX_X  (320 - 140)  // 180 – maximale X-Position des 140px-Bildes
#define SS_MAX_Y  (240 - 90)   //  150 – maximale Y-Position des 90px-Bildes

unsigned long lastActivityTime  = 0;
unsigned long lastSsMove        = 0;
unsigned long lastSsImg         = 0;
bool screensaverActive          = false;

// Screensaver-Zustand
int  ssX = 20, ssY = 20;    // Aktuelle Bildposition
int  ssDX = 1, ssDY = 1;   // Bewegungsrichtung
int  ssImgIdx = 0;           // Aktuell angezeigtes Getränkebild (0–2)
bool ssGlassSnapshot[12];    // Glasstatus beim Einschlafen (für Wake-on-Glass)

// Prüft ob der Screensaver aktiviert werden soll, und animiert das Bild wenn aktiv.
// Muss jede Loop-Iteration aufgerufen werden.
void screensaverTick() {
  unsigned long now = millis();

  if (!screensaverActive) {
    if (now - lastActivityTime > SCREENSAVER_TIMEOUT_MS) {
      screensaverActive = true;
      ssX = 20; ssY = 20; ssDX = 1; ssDY = 1; ssImgIdx = 0;
      lastSsMove = now;
      lastSsImg  = now;
      bot.setPage(9);
      bot.sendHasp("p9b1.src=L:/drink0.bin");
      bot.sendHasp("p9b1.x=" + String(ssX));
      bot.sendHasp("p9b1.y=" + String(ssY));
      // Glasstatus beim Einschlafen merken, damit neues Glas aufweckt
      for (int i = 0; i < 12; i++) ssGlassSnapshot[i] = bot.isGlassPresent(i);
    }
    return;
  }

  // --- Screensaver aktiv ---

  // Bild alle 30s wechseln (rotiert durch alle 3 Getränkebilder)
  if (now - lastSsImg > SCREENSAVER_IMG_MS) {
    lastSsImg = now;
    ssImgIdx = (ssImgIdx + 1) % 3;
    bot.sendHasp("p9b1.src=L:/drink" + String(ssImgIdx) + ".bin");
  }

  // Bild alle 3s in Bounce-Bewegung verschieben
  if (now - lastSsMove > SCREENSAVER_MOVE_MS) {
    lastSsMove = now;
    ssX += ssDX * 20;
    ssY += ssDY * 15;
    if (ssX <= 0)        { ssX = 0;        ssDX =  1; }
    if (ssX >= SS_MAX_X) { ssX = SS_MAX_X; ssDX = -1; }
    if (ssY <= 0)        { ssY = 0;        ssDY =  1; }
    if (ssY >= SS_MAX_Y) { ssY = SS_MAX_Y; ssDY = -1; }
    bot.sendHasp("p9b1.x=" + String(ssX));
    bot.sendHasp("p9b1.y=" + String(ssY));
  }
}

// Beendet den Screensaver und setzt den Inaktivitäts-Timer zurück.
void wakeDisplay() {
  if (screensaverActive) {
    screensaverActive = false;
    goHome();
  }
  lastActivityTime = millis();
}

// =============================================================================
// SERIELLER ZEILENPUFFER (non-blocking UART-Leser für openHASP)
// =============================================================================

// Liest zeichenweise aus Serial1 und gibt true zurück wenn eine vollständige
// Zeile vorliegt. Die Zeile steht dann in serialBuf (null-terminiert).
static char serialBuf[256];
static int  serialBufLen = 0;

bool readSerialLine() {
  while (Serial1.available()) {
    char c = (char)Serial1.read();
    if (c == '\n' || c == '\r') {
      if (serialBufLen > 0) {
        serialBuf[serialBufLen] = '\0';
        serialBufLen = 0;
        return true;
      }
    } else {
      if (serialBufLen < (int)sizeof(serialBuf) - 1) {
        serialBuf[serialBufLen++] = c;
      } else {
        serialBufLen = 0;  // Buffer-Überlauf: Zeile verwerfen
      }
    }
  }
  return false;
}

// =============================================================================
// DISPLAY-HILFSFUNKTIONEN
// =============================================================================

// Aktualisiert alle dynamischen Elemente der Halbkreis-Seite (Seite 6):
// Getränkenamen-Buttons, Farb-Highlighting, Positionskreise.
void updateSemiCirclePage() {
  // Getränkenamen auf "Alle"-Buttons (max. 9 Zeichen damit sie reinpassen)
  String n0 = bot.drinkNames[0]; if (n0.length() > 9) n0 = n0.substring(0, 9);
  String n1 = bot.drinkNames[1]; if (n1.length() > 9) n1 = n1.substring(0, 9);
  String n2 = bot.drinkNames[2]; if (n2.length() > 9) n2 = n2.substring(0, 9);
  bot.sendHasp("p6b3.text=\"" + n0 + "\""); delay(20);
  bot.sendHasp("p6b4.text=\"" + n1 + "\""); delay(20);
  bot.sendHasp("p6b5.text=\"" + n2 + "\""); delay(20);

  // Ausgewähltes Getränk farbig hervorheben (nur wenn mixedMode aus ist).
  // Im MixedMode werden alle drei dunkelgrau, um "inaktiv" zu signalisieren.
  for (int i = 0; i < 3; i++) {
    String color = "#333333"; // Standard: Dunkelgrau
    if (!bot.mixedMode && i == globalSelectedDrink) {
      color = DRINK_HEX_COLORS[i]; // Nur bunt leuchten, wenn nicht im MixedMode
    }
    bot.sendHasp("p6b" + String(3 + i) + ".bg_color=" + color);
    delay(20);
  }

  // 12 Positionskreise: Getränkefarbe wenn Glas erkannt, sonst grau
  for (int i = 0; i < 12; i++) {
    int drink = bot.slotConfig[i];
    String col = (bot.isGlassPresent(i) && drink >= 0 && drink <= 2)
                 ? String(DRINK_HEX_COLORS[drink])
                 : "#444444";
    bot.sendHasp("p6b" + String(10 + i) + ".bg_color=" + col);
    delay(20);
  }

  // Snapshot für Änderungserkennung aktualisieren
  for (int i = 0; i < 12; i++) {
    lastSlotConfig[i] = bot.slotConfig[i];
    lastGlassState[i] = bot.isGlassPresent(i);
  }
  needsSemiCircleUpdate = false;

  // WiFi-Fehler-Icon: sichtbar wenn nicht verbunden
  bot.sendHasp("p6b50.opacity=" + String(WiFi.status() == WL_CONNECTED ? 0 : 50));
}

// Navigiert zur Home-Seite (Klassik = Seite 1, Halbkreis = Seite 6)
void goHome() {
  if (bot.cydMode) {
    bot.setPage(6);
    updateSemiCirclePage();
    bot.updateDrinkScreen(bot.drinkNameWithWarning(globalSelectedDrink), globalSelectedDrink);
    bot.sendHasp("p3b35.text=\"Ansicht: Halbkreis\"");
  } else {
    bot.setPage(1);
    bot.updateDrinkScreen(bot.drinkNameWithWarning(globalSelectedDrink), globalSelectedDrink);
    bot.setAllSlots(globalSelectedDrink);
    bot.sendHasp("p3b35.text=\"Ansicht: Klassisch\"");
  }
}

// Aktualisiert das Display nach Getränkewechsel.
// WICHTIG: Im cydMode + mixedMode kein setAllSlots – sonst werden individuelle
// Slot-Zuweisungen überschrieben und mixedMode sofort auf false zurückgesetzt.
void updateDisplay() {
  if (bot.cydMode) {
    if (!bot.mixedMode) bot.setAllSlots(globalSelectedDrink);
    bot.updateDrinkScreen(bot.drinkNameWithWarning(globalSelectedDrink), globalSelectedDrink);
    goHome();
  } else {
    bot.updateDrinkScreen(bot.drinkNameWithWarning(globalSelectedDrink), globalSelectedDrink);
    bot.setAllSlots(globalSelectedDrink);
  }
  delay(100);
}

// =============================================================================
// COMMAND-HANDLER
// =============================================================================

void handleCommand(BotCommand cmd) {
  switch (cmd) {
    case CMD_START:
      if (currentState == IDLE) {
        if (waitingForGlassRemoval) {
          // Start blockiert – Feedback ans Display
          bot.sendHasp("p1b11.text=\"Bitte Glaeser entnehmen!\"");
          bot.sendHasp("p6b24.text=\"Bitte Glaeser entnehmen!\"");
        } else {
          currentState = POURING_SEQUENCE;
        }
      }
      break;

    case CMD_PRIME:
      if (currentState == IDLE) currentState = PRIMING;
      break;

    case CMD_STOP:
      stopRequested = true;
      break;

    case CMD_NEXT:
      if (currentState == IDLE) {
        globalSelectedDrink = (globalSelectedDrink + 1) % 3;
        updateDisplay();
      }
      break;

    case CMD_PREV:
      if (currentState == IDLE) {
        globalSelectedDrink = (globalSelectedDrink > 0) ? globalSelectedDrink - 1 : 2;
        updateDisplay();
      }
      break;

    case CMD_RESET_NAMES:
      bot.resetDrinkNames();
      updateDisplay();
      break;

    case CMD_RESET_WIFI:
      net.resetWifiSettings();
      break;

    case CMD_REBOOT:
      ESP.restart();
      break;

    case CMD_HOME:
      // Einstellungsseite verlassen: aktuellen Status auf p3 anzeigen, dann Home
      bot.sendHasp("p3b36.text=\"Disp-WiFi: " + String(bot.wifiSyncEnabled ? "AN" : "AUS") + "\"");
      { char buf[20]; snprintf(buf, sizeof(buf), "Fuellmenge: %.1fs", bot.pourTimeMs / 1000.0f);
        bot.sendHasp("p3b37.text=\"" + String(buf) + "\""); }
      goHome();
      break;

    case CMD_CANCEL:
      bot.pendingAllDrink = -1; // Reset Überschreib-Status
      bot.setPage(bot.cydMode ? 6 : 1);
      break;

    case CMD_TOGGLE_CYD:
      bot.cydMode = !bot.cydMode;
      { Preferences p; p.begin("barbot", false); p.putBool("cydMode", bot.cydMode); p.end(); }
      goHome();
      break;

    case CMD_TOGGLE_WIFI_SYNC:
      bot.wifiSyncEnabled = !bot.wifiSyncEnabled;
      { Preferences p; p.begin("barbot", false); p.putBool("wifiSync", bot.wifiSyncEnabled); p.end(); }
      bot.sendHasp("p3b36.text=\"Disp-WiFi: " + String(bot.wifiSyncEnabled ? "AN" : "AUS") + "\"");
      break;

    // --- Handy Kopplung Button ---
    case CMD_TOGGLE_PAIRING:
      net.isPairingMode = !net.isPairingMode;
      Serial.println(net.isPairingMode ? "Kopplungsmodus AKTIV" : "Kopplungsmodus INAKTIV");
      if (net.isPairingMode) {
          bot.sendHasp("p3b99.bg_color=#10b981"); // Grün einfärben
          bot.sendHasp("p3b99.text=\"Kopplung AKTIV\"");
      } else {
          bot.sendHasp("p3b99.bg_color=#0284c7"); // Zurück auf Blau
          bot.sendHasp("p3b99.text=\"Handy Koppeln\"");
      }
      break;

    // --- Füllmengen-Seite (Seite 8) ---
    case CMD_POUR_PAGE:
      bot.setPage(8);
      bot.updatePourTimeDisplay();
      break;
    case CMD_POUR_PLUS:
      bot.savePourTime(bot.pourTimeMs + 500);
      bot.updatePourTimeDisplay();
      break;
    case CMD_POUR_MINUS:
      bot.savePourTime(bot.pourTimeMs - 500);
      bot.updatePourTimeDisplay();
      break;

    // =========================================================================
    // --- HALBKREIS-SLOTS (SEITE 6/7) MIT ÜBERSCHREIB-SCHUTZ ---
    // =========================================================================
    case CMD_ALLPOS_D0:
      if (bot.mixedMode) {
        bot.pendingAllDrink = 0;
        bot.setPage(11);  // Bestätigungs-Dialog (keine msgbox – openHASP kennt das nicht)
      } else {
        bot.setAllSlots(0);
        updateSemiCirclePage();
      }
      break;
      
    case CMD_ALLPOS_D1:
      if (bot.mixedMode) {
        bot.pendingAllDrink = 1;
        bot.setPage(11);  // Bestätigungs-Dialog (keine msgbox – openHASP kennt das nicht)
      } else {
        bot.setAllSlots(1);
        updateSemiCirclePage();
      }
      break;
      
    case CMD_ALLPOS_D2:
      if (bot.mixedMode) {
        bot.pendingAllDrink = 2;
        bot.setPage(11);  // Bestätigungs-Dialog (keine msgbox – openHASP kennt das nicht)
      } else {
        bot.setAllSlots(2);
        updateSemiCirclePage();
      }
      break;

    case CMD_CONFIRM_ALL:  // "Ja" auf Seite 11 → alle Slots überschreiben
      if (bot.pendingAllDrink >= 0 && bot.pendingAllDrink <= 2) {
        bot.setAllSlots(bot.pendingAllDrink);
        bot.pendingAllDrink = -1;
        bot.setPage(6);
        updateSemiCirclePage();
      }
      break;
    // =========================================================================

    case CMD_SELPOS:
      if (currentState == IDLE) {
        int p = bot.lastSelPos;
        bot.sendHasp("p7b2.text=\"Pos " + String(p + 1) + " - Getraenk:\"");
        bot.sendHasp("p7b3.text=\"" + bot.drinkNames[0] + "\"");
        bot.sendHasp("p7b4.text=\"" + bot.drinkNames[1] + "\"");
        bot.sendHasp("p7b5.text=\"" + bot.drinkNames[2] + "\"");
        bot.setPage(7);
      }
      break;

    case CMD_SETPOS_D0:   bot.setSlot(bot.lastSelPos, 0);  bot.setPage(6); updateSemiCirclePage(); break;
    case CMD_SETPOS_D1:   bot.setSlot(bot.lastSelPos, 1);  bot.setPage(6); updateSemiCirclePage(); break;
    case CMD_SETPOS_D2:   bot.setSlot(bot.lastSelPos, 2);  bot.setPage(6); updateSemiCirclePage(); break;
    case CMD_SETPOS_NONE: bot.setSlot(bot.lastSelPos, -1); bot.setPage(6); updateSemiCirclePage(); break;

    case CMD_NONE:
    default:
      break;
  }
}

// Zeigt Warnbanner auf Seite 1 und 6 wenn eine Flasche >= 80% geleert ist.
// Aktualisiert zusätzlich den Getränkenamen mit "(!)"-Suffix.
void checkBottleWarnings() {
  String warn = "";
  for (int i = 0; i < 3; i++) {
    if (bot.bottleSizeMl[i] > 0) {
      int pct = (bot.dispensedMl[i] * 100) / bot.bottleSizeMl[i];
      if (pct >= 80) {
        if (warn.length() > 0) warn += "  ";
        warn += "! Pumpe " + String(i + 1) + " fast leer!";
      }
    }
  }
  if (warn.length() > 0) {
    // bg_opa=255 zwingend: LVGL zeichnet Hintergrund sonst nicht
    bot.sendHasp("p1b60.text=\"" + warn + "\"");
    bot.sendHasp("p1b60.bg_color=#f97316");
    bot.sendHasp("p1b60.bg_opa=255");
    bot.sendHasp("p1b60.hidden=false");
    bot.sendHasp("p6b60.text=\"" + warn + "\"");
    bot.sendHasp("p6b60.bg_color=#f97316");
    bot.sendHasp("p6b60.bg_opa=255");
    bot.sendHasp("p6b60.hidden=false");
  } else {
    bot.sendHasp("p1b60.hidden=true");
    bot.sendHasp("p6b60.hidden=true");
  }
  // Getränkename mit Warnstatus auf dem Display aktualisieren
  bot.updateDrinkScreen(bot.drinkNameWithWarning(globalSelectedDrink), globalSelectedDrink);
}

// Liest alle verfügbaren UART-Zeilen, verarbeitet Commands, wertet HASP-Status aus.
// Holt außerdem Web-/MQTT-Anfragen aus Connectivity ab.
void checkCommands() {
  while (readSerialLine()) {
    String line = String(serialBuf);
    BotCommand cmd = bot.parseInput(line);

    if (cmd != CMD_NONE) {
      // Echter Touch-Befehl → Display aufwecken und Command ausführen
      wakeDisplay();
      handleCommand(cmd);
    }
    // Nicht-Touch-Meldungen (Logs, Fehler) dürfen den Inaktivitäts-Timer
    // NICHT zurücksetzen – sonst startet der Screensaver nie.

    // openHASP sendet die eigene IP in zwei möglichen Formaten:
    //   Alt: "HASP online 192.168.x.x"
    //   Neu: "...WIFI: Received IP address 192.168.x.x"
    // Aus dem rohen Substring nur Ziffern und Punkte übernehmen,
    // damit ANSI-Escape-Sequenzen (\x1B[...) den String nicht verunreinigen.
    {
      String rawAfter = "";
      int idx = line.indexOf("HASP online ");
      if (idx >= 0)       rawAfter = line.substring(idx + 12);
      else {
        idx = line.indexOf("IP address ");
        if (idx >= 0)     rawAfter = line.substring(idx + 11);
      }
      if (rawAfter.length() > 0) {
        String displayIP = "";
        for (int i = 0; i < rawAfter.length(); i++) {
          char c = rawAfter.charAt(i);
          if (isDigit(c) || c == '.') displayIP += c;
          else if (displayIP.length() > 0) break;  // erstes Nicht-IP-Zeichen nach dem Start → Ende
        }
        if (displayIP.length() >= 7) {  // Mindestlänge "1.2.3.4"
          bot.displayIP = displayIP;
          Serial.println("[CYD] Display-IP erkannt: " + displayIP);
          bot.sendHasp("p3b6.text=\"Disp: " + displayIP + "\"");
          Preferences p; p.begin("barbot", false); p.putString("dispIP", displayIP); p.end();
          updateDisplay();
        }
      }
    }
  }

  // Web-/MQTT-Anfragen aus Connectivity abholen
  net.loop();
  if (net.getStartReq() && currentState == IDLE && !waitingForGlassRemoval) {
    wakeDisplay();
    currentState = POURING_SEQUENCE;
  }
  if (net.getStopReq()) {
    wakeDisplay();
    stopRequested = true;
  }
}

// =============================================================================
// SETUP & LOOP
// =============================================================================

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  bot.begin();

  // Menü-Farben (Blau / Rot / Gelb) einmalig initialisieren
  menuColors[0] = bot.leds.Color(0,   0,   255);
  menuColors[1] = bot.leds.Color(255, 0,   0);
  menuColors[2] = bot.leds.Color(255, 255, 0);

  net.setup();
  delay(1000);

  // Snapshots fuer Aenderungserkennung und Glasentnahme initialisieren
  for (int i = 0; i < 12; i++) {
    lastSlotConfig[i]  = bot.slotConfig[i];
    lastGlassState[i]  = bot.isGlassPresent(i);
    pairingLastGlass[i] = bot.isGlassPresent(i);
    ssGlassSnapshot[i] = false;
    filledSlots[i]     = false;
  }
  waitingForGlassRemoval = false;
  lastActivityTime = millis();

  updateDisplay();
}

void loop() {
  checkCommands();
  screensaverTick();

  // Wake-on-Glass: neues Glas aufgestellt während Screensaver aktiv → aufwecken
  if (screensaverActive) {
    for (int i = 0; i < 12; i++) {
      bool cur = bot.isGlassPresent(i);
      if (cur && !ssGlassSnapshot[i]) { wakeDisplay(); break; }
      if (!cur) ssGlassSnapshot[i] = false;  // Entferntes Glas: Snapshot zurücksetzen
    }
  }

  // --- Glas Erkennung für Handy Kopplung (völlig entkoppelt vom Display) ---
  for (int i = 0; i < 12; i++) {
      bool currentGlass = bot.isGlassPresent(i);
      if (currentGlass && !pairingLastGlass[i] && net.isPairingMode) {
          net.triggerPairingPending(i);
      }
      pairingLastGlass[i] = currentGlass;
  }

  // --- Physischer Button (mit Entprellung) ---
  // Kurzer Druck: Getränk weiterschalten
  int reading = digitalRead(PIN_BUTTON);
  if (reading != lastButtonState) lastDebounceTime = millis();
  if ((millis() - lastDebounceTime) > BUTTON_DEBOUNCE) {
    if (reading != buttonState) {
      buttonState = reading;
      if (buttonState == HIGH && !buttonHeld) {
        wakeDisplay();
        if (currentState == IDLE) {
          if (waitingForGlassRemoval) {
            // Kurzer Druck waehrend Glasentnahme-Sperre: Feedback anzeigen
            bot.sendHasp("p1b11.text=\"Bitte Glaeser entnehmen!\"");
            bot.sendHasp("p6b24.text=\"Bitte Glaeser entnehmen!\"");
          } else {
            globalSelectedDrink = (globalSelectedDrink + 1) % 3;
            updateDisplay();
          }
        }
      }
    }
  }
  lastButtonState = reading;

  // ============================================================
  // ZUSTAND: IDLE
  // ============================================================
  // Flaschenwarnungen alle 5s prüfen – aber nur im Idle, damit die
  // Pour-Fortschrittsseite und der Screensaver nicht überschrieben werden.
  static unsigned long lastBottleCheck = 0;
  if (currentState == IDLE && !screensaverActive &&
      millis() - lastBottleCheck > 5000) {
    lastBottleCheck = millis();
    checkBottleWarnings();
  }

  if (currentState == IDLE) {
    bot.isBusy      = false;
    bot.activeSlot  = -1;
    bot.activeDrink = -1;

    // --- Identifikations-Blinken übernehmen ---
    if (net.pendingIdentifySlot >= 0) {
        identifySlotIdx = net.pendingIdentifySlot;
        identifyStartTime = millis();
        net.pendingIdentifySlot = -1;
    }

    if (waitingForGlassRemoval) {
      // Orange LEDs fuer Slots die noch entnommen werden muessen,
      // normale Menufarbe fuer leere Slots
      uint32_t orangeColor = bot.leds.Color(255, 80, 0);
      bool anyStillPresent = false;
      for (int i = 0; i < 12; i++) {
        bool present = bot.isGlassPresent(i);
        if (filledSlots[i] && present) {
          // Glas noch da: orange leuchten
          bot.setLed(i, orangeColor);
          anyStillPresent = true;
        } else if (filledSlots[i] && !present) {
          // Glas entnommen: Slot als erledigt markieren
          filledSlots[i] = false;
          bot.setLed(i, 0);
        } else {
          // Slot war nicht befuellt: normal anzeigen
          bot.setLed(i, bot.isGlassPresent(i) ? menuColors[bot.slotConfig[i] >= 0 ? bot.slotConfig[i] : 0] : 0);
        }
      }
      if (!anyStillPresent) {
        // Alle Glaeser entnommen -> Sperre aufheben
        waitingForGlassRemoval = false;
        updateDisplay();
      }
    } else {
      
      // --- Manuelle LED-Schleife für normales Update + Blink-Effekt ---
      bool ledChanged = false;
      for (int i = 0; i < 12; i++) {
        uint32_t targetColor = 0;
        if (bot.isGlassPresent(i)) {
          int assignedDrink = bot.slotConfig[i];
          if (assignedDrink >= 0 && assignedDrink <= 2) {
             targetColor = menuColors[assignedDrink];
          }
        }
        
        // Wenn dieser Slot gerade blinken soll (Tippen am Handy), überschreibe die normale Farbe
        if (i == identifySlotIdx) {
          if (millis() - identifyStartTime < 5000) {
            // Blinkrhythmus: 250ms an, 250ms aus
            if ((millis() / 250) % 2 == 0) {
              targetColor = bot.leds.Color(255, 100, 0); // Orange blinken
            } else {
              targetColor = 0; // Aus
            }
          } else {
            identifySlotIdx = -1; // Blinken beenden
          }
        }
        
        int physicalIdx = 11 - i;
        if (bot.leds.getPixelColor(physicalIdx) != targetColor) {
          bot.leds.setPixelColor(physicalIdx, targetColor);
          ledChanged = true;
        }
      }
      if (ledChanged) bot.leds.show();
      // -----------------------------------------------------------
      
    }

    // Getränkewechsel aus dem Web verarbeiten
    if (bot.webSelectedDrink >= 0) {
      globalSelectedDrink  = bot.webSelectedDrink;
      bot.webSelectedDrink = -1;
      needsSemiCircleUpdate = true;
    }

    // Halbkreis-Seite bei Änderungen aktualisieren
    if (bot.cydMode && needsSemiCircleUpdate) {
      updateSemiCirclePage();
    } else if (bot.cydMode) {
      bool changed = false;
      for (int i = 0; i < 12; i++) {
        if (bot.slotConfig[i]    != lastSlotConfig[i] ||
            bot.isGlassPresent(i) != lastGlassState[i]) {
          changed = true; break;
        }
      }
      if (changed) needsSemiCircleUpdate = true;
    } else {
      // Klassik-Modus: Glasstatus für nächsten Vergleich merken
      for (int i = 0; i < 12; i++) lastGlassState[i] = bot.isGlassPresent(i);
    }
  }

  // ============================================================
  // ZUSTAND: POURING_SEQUENCE (Einschenken)
  // ============================================================
  if (currentState == POURING_SEQUENCE) {
    stopRequested = false;
    bot.isBusy = true;
    bot.setPage(2);
    bot.clearLeds();
    for (int i = 0; i < 12; i++) filledSlots[i] = false;  // Slots-Protokoll zuruecksetzen

    for (int i = 0; i < NUM_POSITIONS; i++) {
      checkCommands();
      if (stopRequested) break;

      int drinkForThisSlot = bot.slotConfig[i];
      if (!bot.isGlassPresent(i) || drinkForThisSlot < 0) continue;

      bot.activeSlot  = i;
      bot.activeDrink = drinkForThisSlot;

      bot.updateProgress(i, NUM_POSITIONS, "Fahre zu Glas " + String(i + 1));
      bot.moveArmToSlot(i, false);
      bot.setLed(i, menuColors[drinkForThisSlot]);

      bot.updateProgress(i, NUM_POSITIONS, "Schenke " + bot.drinkNames[drinkForThisSlot] + "...");
      bot.runPump(drinkForThisSlot, true);

      unsigned long startPour = millis();
      while (millis() - startPour < (unsigned long)bot.pourTimeMs) {
        delay(10);
        checkCommands();
        if (stopRequested) break;
      }

      bot.stopPump(drinkForThisSlot);
      bot.setLed(i, bot.leds.Color(0, 255, 0));  // Gruen = fertig
      delay(50);
      bot.runPump(drinkForThisSlot, false);  // Anti-Drip Ruecklauf
      delay(ANTIDRIP_TIME_MS);
      bot.stopPump(drinkForThisSlot);

      // Wichtig: erst multiplizieren, dann teilen – sonst geht durch
      // Integer-Division Genauigkeit verloren (z.B. 4000/2800 = 1 statt 1.43)
      bot.addDispensed(drinkForThisSlot, (bot.pourTimeMs * 10) / bot.msPerCl);
      filledSlots[i] = true;
      // Warnung erst NACH Abschluss der ganzen Sequenz prüfen,
      // damit Fortschrittsseite (p2) nicht überschrieben wird
      if (stopRequested) break;
    }

    bot.moveArmToPark();
    bot.isBusy      = false;
    bot.activeSlot  = -1;
    bot.activeDrink = -1;

    if (!stopRequested) {
      bot.updateProgress(12, 12, "FERTIG! PROST!");
      net.publishEvent("DONE");
      // Abschluss-Animation: 3x gruenes Blinken
      for (int k = 0; k < 3; k++) {
        bot.clearLeds(); delay(300);
        for (int i = 0; i < NUM_LEDS; i++) bot.setLed(i, bot.leds.Color(0, 50, 0));
        delay(300);
      }
      // Glasentnahme-Sperre aktivieren wenn mindestens ein Glas befuellt wurde
      waitingForGlassRemoval = false;
      for (int i = 0; i < 12; i++) {
        if (filledSlots[i]) { waitingForGlassRemoval = true; break; }
      }
    } else {
      bot.updateProgress(0, 12, "ABGEBROCHEN!");
      net.publishEvent("STOPPED");
      delay(2000);
      waitingForGlassRemoval = false;  // Bei Abbruch keine Sperre
    }

    bot.setPage(1);
    currentState = IDLE;
    updateDisplay();
    checkBottleWarnings();  // Warnstatus nach Ausschank-Ende neu prüfen
  }

  // ============================================================
  // ZUSTAND: PRIMING (Schlauchbefüllung)
  // ============================================================
  if (currentState == PRIMING) {
    stopRequested = false;
    bot.isBusy = true;
    bot.setPage(5);

    // Feste Priming-Positionen: eine pro Pumpe, möglichst weit verteilt
    const int   PRIME_SLOTS[3]   = {0, 5, 11};
    const char* PRIME_COL_ON[3]  = {"#0044FF", "#FF2200", "#CCCC00"};
    const char* PRIME_COL_OFF[3] = {"#001166", "#550000", "#444400"};
    uint32_t primeLedOn[3]  = { bot.leds.Color(0, 0, 180), bot.leds.Color(180, 0, 0),   bot.leds.Color(130, 130, 0) };
    uint32_t primeLedDim[3] = { bot.leds.Color(0, 0, 25),  bot.leds.Color(25, 0, 0),    bot.leds.Color(18, 18, 0)  };
    bool pumpRunning[3] = {false, false, false};

    // Initiale Anzeige aufbauen (Button-Farbe, Name, Position, Status)
    for (int p = 0; p < 3; p++) {
      int base = (p + 1) * 10;
      bot.sendHasp("p5b" + String(base)   + ".bg_color=" + String(PRIME_COL_OFF[p]));
      bot.sendHasp("p5b" + String(base+1) + ".text=\"" + bot.drinkNames[p] + "\"");
      bot.sendHasp("p5b" + String(base+2) + ".text=\"Pos " + String(PRIME_SLOTS[p] + 1) + "\"");
      bot.sendHasp("p5b" + String(base+3) + ".text=\"Bereit\"");
      bot.sendHasp("p5b" + String(base+3) + ".text_color=#44FF44");
      bot.setLed(PRIME_SLOTS[p], primeLedDim[p]);
    }
    bot.leds.show();
    bot.sendHasp("p5b40.text=\"Glas auf Pos 1, 6 oder 12 stellen\"");

    // Hauptschleife: Glaserkennung → Pumpe starten/stoppen
    while (!stopRequested) {
      checkCommands();
      for (int p = 0; p < 3; p++) {
        bool glass = bot.isGlassPresent(PRIME_SLOTS[p]);
        int base = (p + 1) * 10;

        if (glass && !pumpRunning[p]) {
          // Glas aufgestellt → Pumpe starten
          bot.moveArmToSlot(PRIME_SLOTS[p]);
          bot.runPump(p, true);
          pumpRunning[p] = true;
          bot.setLed(PRIME_SLOTS[p], primeLedOn[p]); bot.leds.show();
          bot.sendHasp("p5b" + String(base)   + ".bg_color=" + String(PRIME_COL_ON[p]));
          bot.sendHasp("p5b" + String(base+3) + ".text=\"LAUFT!\"");
          bot.sendHasp("p5b" + String(base+3) + ".text_color=#FFFFFF");
          bot.sendHasp("p5b40.text=\"Pumpe " + String(p + 1) + " laeuft...\"");

        } else if (!glass && pumpRunning[p]) {
          // Glas entfernt → Pumpe stoppen + Anti-Drip
          bot.stopPump(p); delay(50);
          bot.runPump(p, false); delay(ANTIDRIP_TIME_MS); bot.stopPump(p);
          pumpRunning[p] = false;
          bot.setLed(PRIME_SLOTS[p], primeLedDim[p]); bot.leds.show();
          bot.sendHasp("p5b" + String(base)   + ".bg_color=" + String(PRIME_COL_OFF[p]));
          bot.sendHasp("p5b" + String(base+3) + ".text=\"Bereit\"");
          bot.sendHasp("p5b" + String(base+3) + ".text_color=#44FF44");
          bot.sendHasp("p5b40.text=\"Bereit - warte auf Glas...\"");
        }
      }
      delay(50);
    }

    // Alle laufenden Pumpen sicher stoppen
    for (int p = 0; p < 3; p++) {
      if (pumpRunning[p]) {
        bot.stopPump(p); delay(50);
        bot.runPump(p, false); delay(ANTIDRIP_TIME_MS); bot.stopPump(p);
      }
    }

    bot.moveArmToPark();
    bot.clearLeds();
    bot.isBusy = false;
    bot.setPage(1);
    currentState = IDLE;
    updateDisplay();
  }
}