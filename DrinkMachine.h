// =============================================================================
// DrinkMachine.h – Hardware-Abstraktion des BarBot
// =============================================================================
// Kapselt Servo, Pumpen, LEDs und Display-Kommunikation.
// Enthält außerdem den Command-Enum und den HASP-Input-Parser.
// Zustandsspeicherung (Getränkenamen, Einstellungen) über NVS/Preferences.
// =============================================================================

#ifndef DRINKMACHINE_H
#define DRINKMACHINE_H

#include <ESP32Servo.h>
#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include "config.h"

// Getränkefarben für LEDs und Display (Hex-Strings für openHASP)
static const char* DRINK_HEX_COLORS[] = {"#0044FF", "#FF2200", "#998800"};

// =============================================================================
// COMMANDS (von Display oder Web → Hauptloop)
// =============================================================================

enum BotCommand {
  CMD_NONE,
  CMD_START, CMD_STOP, CMD_NEXT, CMD_PREV, CMD_PRIME,
  CMD_RESET_NAMES, CMD_RESET_WIFI, CMD_REBOOT,
  CMD_HOME, CMD_CANCEL,
  CMD_TOGGLE_CYD,
  CMD_TOGGLE_WIFI_SYNC,
  CMD_ALLPOS_D0, CMD_ALLPOS_D1, CMD_ALLPOS_D2,
  CMD_SELPOS,
  CMD_SETPOS_D0, CMD_SETPOS_D1, CMD_SETPOS_D2, CMD_SETPOS_NONE,
  CMD_POUR_PAGE,   // Öffnet Füllmengen-Seite (Seite 8)
  CMD_POUR_PLUS,   // Füllzeit +0.5s
  CMD_POUR_MINUS,  // Füllzeit -0.5s
  CMD_TOGGLE_PAIRING,  // Handy-Kopplungsmodus (Toggle via Display Seite 3, b99)
  CMD_CONFIRM_ALL  // Bestätigung Überschreiben (Seite 11, b10=Ja / b20=Nein)
};

// =============================================================================
// DRINKMACHINE
// =============================================================================

class DrinkMachine {
  private:
    Servo armServo;
    int currentServoAngle = SERVO_POS_PARK;

    // Pin-Arrays für die drei H-Brücken
    int motorPinsA[3] = {PIN_P1_A, PIN_P2_A, PIN_P3_A};
    int motorPinsB[3] = {PIN_P1_B, PIN_P2_B, PIN_P3_B};

    Preferences prefs;

  public:
    Adafruit_NeoPixel leds;

    // Slot-Konfiguration: welche Pumpe (0-2) ist welchem Slot zugeordnet
    int slotConfig[12];
    String drinkNames[3];

    // Zustandsflags (werden von Connectivity für WebSocket/MQTT genutzt)
    bool isBusy    = false;
    int activeSlot  = -1;
    int activeDrink = -1;

    // Anzeigemodus
    bool cydMode          = false;  // true = Halbkreis-Ansicht (Seite 6)
    bool wifiSyncEnabled  = true;   // WLAN-Daten per UART ans Display senden

    // Einschenkzeit in ms (wird per NVS gespeichert und über Web/Display angepasst)
    int pourTimeMs = 5000;

    // IP-Adressen für Statusanzeige
    String localIP   = "";  // IP des ESP32 im WLAN
    String displayIP = "";  // IP des CYD (aus openHASP-Statusmeldung)

    // Hilfsvariablen für Halbkreis-Modus
    int lastSelPos       = 0;
    int webSelectedDrink = -1;

    // Überschreib-Schutz (Mixed Mode)
    bool mixedMode      = false;
    int pendingAllDrink = -1;

    // Kalibrierung: ms pro Centiliter (Standard: 2000ms = 1cl)
    int msPerCl = 2000;

    // Flaschen-Mengenzähler (persistent über NVS)
    int bottleSizeMl[3] = {700, 700, 700};  // Flaschengröße in ml
    int dispensedMl[3]  = {0,   0,   0};    // Ausgegossene Menge in ml

  DrinkMachine() : leds(NUM_LEDS, PIN_LEDS, NEO_GRB + NEO_KHZ800) {
      for (int i = 0; i < 12; i++) slotConfig[i] = 0;
    }

    // -------------------------------------------------------------------------
    // Initialisierung
    // -------------------------------------------------------------------------

    void begin() {
      // Gespeicherte Werte aus NVS laden
      prefs.begin("barbot", false);
      drinkNames[0]   = prefs.getString("d1",      "Getraenk 1");
      drinkNames[1]   = prefs.getString("d2",      "Getraenk 2");
      drinkNames[2]   = prefs.getString("d3",      "Getraenk 3");
      cydMode         = prefs.getBool("cydMode",   false);
      wifiSyncEnabled = prefs.getBool("wifiSync",  true);
      pourTimeMs      = prefs.getInt("pourTime",   4000);
      msPerCl         = prefs.getInt("msPerCl",   2000);
      displayIP       = prefs.getString("dispIP",  "");  // Zuletzt erkannte Display-IP (persistent)
      bottleSizeMl[0] = prefs.getInt("btl0",  700);
      bottleSizeMl[1] = prefs.getInt("btl1",  700);
      bottleSizeMl[2] = prefs.getInt("btl2",  700);
      dispensedMl[0]  = prefs.getInt("dis0",  0);
      dispensedMl[1]  = prefs.getInt("dis1",  0);
      dispensedMl[2]  = prefs.getInt("dis2",  0);
      prefs.end();

      // UART zum Display initialisieren
      // "seriallog 0" schaltet die HASP-eigenen seriellen Logs aus,
      // damit nur unsere Kommandos über den Bus laufen
      Serial1.begin(CYD_BAUD, SERIAL_8N1, CYD_RX_PIN, CYD_TX_PIN);
      delay(100);
      Serial1.println();
      Serial1.println("seriallog 0");
      delay(100);

      // Motorpins als Output, initial LOW (alle Pumpen aus)
      for (int i = 0; i < 3; i++) {
        pinMode(motorPinsA[i], OUTPUT);
        pinMode(motorPinsB[i], OUTPUT);
        digitalWrite(motorPinsA[i], LOW);
        digitalWrite(motorPinsB[i], LOW);
      }

      // Endschalter: Pins 34+ haben keinen internen Pullup
      for (int i = 0; i < 12; i++) {
        int pin = SWITCH_PINS[i];
        if (pin >= 34) pinMode(pin, INPUT);
        else           pinMode(pin, INPUT_PULLUP);
      }

      // Servo in Parkposition fahren, dann trennen (spart Strom/Wärme)
      armServo.setPeriodHertz(50);
      armServo.attach(PIN_SERVO, 500, 2400);
      armServo.write(SERVO_POS_PARK);
      delay(500);
      armServo.detach();
      currentServoAngle = SERVO_POS_PARK;

      // LEDs initialisieren
      leds.begin();
      leds.show();
      delay(500);

      // Display auf Startseite
      setPage(1);
    }

    // -------------------------------------------------------------------------
    // Datenverwaltung (NVS)
    // -------------------------------------------------------------------------

    void resetDrinkNames() {
      drinkNames[0] = "Getraenk 1";
      drinkNames[1] = "Getraenk 2";
      drinkNames[2] = "Getraenk 3";
      prefs.begin("barbot", false);
      prefs.putString("d1", drinkNames[0]);
      prefs.putString("d2", drinkNames[1]);
      prefs.putString("d3", drinkNames[2]);
      prefs.end();
    }

    void saveDrinkName(int index, String name) {
      if (index < 0 || index > 2) return;
      drinkNames[index] = name;
      prefs.begin("barbot", false);
      String key = (index == 0) ? "d1" : (index == 1 ? "d2" : "d3");
      prefs.putString(key.c_str(), name);
      prefs.end();
    }

    // Einschenkmenge in cl speichern (wird intern als ms gespeichert)
    void savePourVolumeCl(float cl) {
      if (cl < 0.5f)  cl = 0.5f;
      if (cl > 15.0f) cl = 15.0f;
      cl = round(cl * 2.0f) / 2.0f;  // Auf 0.5cl runden
      savePourTime((int)(cl * msPerCl));
    }

    float getPourVolumeCl() const {
      return pourTimeMs / (float)msPerCl;
    }

    void savePourTime(int ms) {
      if (ms < 1000)  ms = 1000;
      if (ms > 30000) ms = 30000;
      pourTimeMs = ms;
      prefs.begin("barbot", false);
      prefs.putInt("pourTime", ms);
      prefs.end();
    }

    // Kalibrierung speichern: msPerCl = gemessene Laufzeit für genau 1cl
    void saveCalibration(int ms_per_cl) {
      if (ms_per_cl < 500)   ms_per_cl = 500;
      if (ms_per_cl > 10000) ms_per_cl = 10000;
      msPerCl = ms_per_cl;
      prefs.begin("barbot", false);
      prefs.putInt("msPerCl", ms_per_cl);
      prefs.end();
    }

  // -------------------------------------------------------------------------
  // Flaschen-Mengenzähler
  // -------------------------------------------------------------------------

    void addDispensed(int drinkIdx, int ml) {
      if (drinkIdx < 0 || drinkIdx > 2) return;
      dispensedMl[drinkIdx] += ml;
      prefs.begin("barbot", false);
      prefs.putInt(("dis" + String(drinkIdx)).c_str(), dispensedMl[drinkIdx]);
      prefs.end();
    }

    void resetBottle(int drinkIdx) {
      if (drinkIdx < 0 || drinkIdx > 2) return;
      dispensedMl[drinkIdx] = 0;
      prefs.begin("barbot", false);
      prefs.putInt(("dis" + String(drinkIdx)).c_str(), 0);
      prefs.end();
    }

    void saveBottleSize(int drinkIdx, int ml) {
      if (drinkIdx < 0 || drinkIdx > 2) return;
      bottleSizeMl[drinkIdx] = ml;
      prefs.begin("barbot", false);
      prefs.putInt(("btl" + String(drinkIdx)).c_str(), ml);
      prefs.end();
    }

  // -------------------------------------------------------------------------
  // Slot-Konfiguration
  // -------------------------------------------------------------------------

  void setAllSlots(int drinkIndex) {
    for (int i = 0; i < 12; i++) slotConfig[i] = drinkIndex;
    updateMixedMode(false);
  }

  void setSlot(int index, int drinkIndex) {
    if (index >= 0 && index < 12) {
      slotConfig[index] = drinkIndex;
      updateMixedMode(true);
    }
  }

  void updateMixedMode(bool isMixed) {
    mixedMode = isMixed;
    // Die optische Abdunkelung passiert jetzt sauber in der .ino Datei
    // in der Funktion updateSemiCirclePage(), damit sie nicht versehentlich
    // sofort wieder vom Display überschrieben wird.
  }

  // -------------------------------------------------------------------------
  // Display-Kommunikation (openHASP via UART Serial1)
    // -------------------------------------------------------------------------

    // Sendet einen einzelnen HASP-Befehl (z.B. "p1b11.text=Hallo")
    void sendHasp(String cmd) {
      Serial1.print(cmd);
      Serial1.print("\n");
    }

    void setPage(int page) {
      sendHasp("page " + String(page));
    }

    // Aktualisiert Getränkename und -farbe auf Klassik- und Halbkreis-Seite
    void updateDrinkScreen(String name, int drinkIndex) {
      String col = (drinkIndex >= 0 && drinkIndex <= 2)
                   ? String(DRINK_HEX_COLORS[drinkIndex])
                   : String("#444444");

      // Klassik-Seite (p1): Getränkename mit Getränkefarbe
      sendHasp("p1b11.text=" + name);
      sendHasp("p1b11.text_color=" + col);

      // Halbkreis-Seite (p6): Label-Hintergrund in Getränkefarbe
      sendHasp("p6b24.text=" + name);
      sendHasp("p6b24.bg_color=" + col);

      // Getränkebild (LVGL .bin, liegt auf der SD-Karte des Displays, Prefix L:/)
      sendHasp("p1b10.src=L:/drink" + String(drinkIndex) + ".bin");
    }

    // Gibt den Getränkenamen zurück – mit " (!)" Suffix wenn Flasche >= 80% leer
    String drinkNameWithWarning(int drinkIndex) {
      if (drinkIndex < 0 || drinkIndex > 2) return "";
      String name = drinkNames[drinkIndex];
      if (bottleSizeMl[drinkIndex] > 0 &&
          (dispensedMl[drinkIndex] * 100) / bottleSizeMl[drinkIndex] >= 80) {
        name += " (!)";
      }
      return name;
    }

    // Füllzeit-Anzeige auf Seite 8 aktualisieren
    void updatePourTimeDisplay() {
      char buf[16];
      snprintf(buf, sizeof(buf), "%.1f cl", pourTimeMs / (float)msPerCl);
      sendHasp("p8b10.text=\"" + String(buf) + "\"");
    }

    // Fortschrittsbalken und Statustext auf Seite 2 (Einschenk-Seite)
    void updateProgress(int current, int total, String statusText) {
      sendHasp("p2b20.val=" + String(current));
      sendHasp("p2b20.max=" + String(total));
      sendHasp("p2b10.text=\"" + statusText + "\"");
    }

    // -------------------------------------------------------------------------
    // Input-Parser (openHASP serielles Event-Format)
  // -------------------------------------------------------------------------

  // Parst eine empfangene HASP-Zeile und gibt den zugehörigen BotCommand zurück.
  BotCommand parseInput(String input) {
    input.trim();

    // Nur "up"-Events verarbeiten (kein Doppel-Auslösen durch "down")
    if (input.indexOf("\"event\":\"up\"") < 0) return CMD_NONE;

    int arrowPos = input.indexOf(" => {");
      if (arrowPos < 0) return CMD_NONE;

      // Page- und Button-ID rückwärts aus dem String extrahieren
      int bPos = -1, pPos = -1;
      for (int i = arrowPos - 1; i >= 0; i--) {
        if (input.charAt(i) == 'b') { bPos = i; break; }
      }
      if (bPos > 0) {
        for (int i = bPos - 1; i >= 0; i--) {
          if (input.charAt(i) == 'p') { pPos = i; break; }
        }
      }
      if (pPos < 0 || bPos < 0) return CMD_NONE;

      int page = input.substring(pPos + 1, bPos).toInt();
      int id   = input.substring(bPos + 1, arrowPos).toInt();
      if (page <= 0 || id <= 0) return CMD_NONE;

      // --- Seite 1: Klassik-Startseite ---
      if (page == 1) {
        if (id == 30) return CMD_START;
        if (id == 20) return CMD_PREV;
        if (id == 21) return CMD_NEXT;
      }
      // --- Seite 2: Einschenk-Fortschritt ---
      else if (page == 2) {
        if (id == 30) return CMD_STOP;
      }
      // --- Seite 3: Einstellungen ---
      else if (page == 3) {
        if (id == 2)  return CMD_HOME;
        if (id == 10) return CMD_RESET_NAMES;
        if (id == 20) return CMD_RESET_WIFI;
        if (id == 30) return CMD_REBOOT;
        if (id == 35) return CMD_TOGGLE_CYD;
        if (id == 36) return CMD_TOGGLE_WIFI_SYNC;
        if (id == 37) return CMD_POUR_PAGE;
        if (id == 40) return CMD_PRIME;
        if (id == 99) return CMD_TOGGLE_PAIRING;  // Handy-Kopplungsmodus
      }
      // --- Seite 5: Priming ---
      else if (page == 5) {
        if (id == 2 || id == 50) return CMD_STOP;
      }
      // --- Seite 6: Halbkreis-Ansicht ---
      else if (page == 6) {
        if (id == 6)  return CMD_START;
        if (id == 3)  return CMD_ALLPOS_D0;
        if (id == 4)  return CMD_ALLPOS_D1;
        if (id == 5)  return CMD_ALLPOS_D2;
        if (id == 22) return CMD_PREV;
        if (id == 23) return CMD_NEXT;
        if (id >= 10 && id <= 21) {
          lastSelPos = id - 10;
          return CMD_SELPOS;
        }
      }
      // --- Seite 7: Einzelslot-Zuweisung ---
      else if (page == 7) {
        if (id == 3) return CMD_SETPOS_D0;
        if (id == 4) return CMD_SETPOS_D1;
        if (id == 5) return CMD_SETPOS_D2;
        if (id == 6) return CMD_SETPOS_NONE;
        if (id == 7) return CMD_CANCEL;
      }
      // --- Seite 8: Füllmengen-Einstellung ---
      else if (page == 8) {
        if (id == 2) return CMD_HOME;
        if (id == 3) return CMD_POUR_PLUS;
        if (id == 4) return CMD_POUR_MINUS;
      }
      // --- Seite 9: Screensaver – jeder Touch weckt ---
      else if (page == 9) {
        return CMD_HOME;  // Beliebiger Touch beendet den Screensaver
      }
      // --- Seite 11: Überschreib-Bestätigung (kein msgbox, echter Dialog) ---
      else if (page == 11) {
        if (id == 10) return CMD_CONFIRM_ALL;  // "Ja"  → alle Slots überschreiben
        if (id == 20) return CMD_CANCEL;        // "Nein" → Abbrechen
      }

      return CMD_NONE;
    }

    // -------------------------------------------------------------------------
    // Sensoren
    // -------------------------------------------------------------------------

    bool isGlassPresent(int slotIndex) {
      if (slotIndex < 0 || slotIndex >= 12) return false;
      return (digitalRead(SWITCH_PINS[slotIndex]) == SWITCH_ACTIVE_STATE);
    }

    // -------------------------------------------------------------------------
    // Pumpensteuerung
    // -------------------------------------------------------------------------

    void runPump(int pumpIdx, bool forward) {
      if (pumpIdx < 0 || pumpIdx > 2) return;
      int pinA = motorPinsA[pumpIdx];
      int pinB = motorPinsB[pumpIdx];
      digitalWrite(pinA, LOW);
      digitalWrite(pinB, LOW);
      if (forward) digitalWrite(pinA, HIGH);
      else         digitalWrite(pinB, HIGH);
    }

    void stopPump(int pumpIdx) {
      if (pumpIdx < 0 || pumpIdx > 2) return;
      digitalWrite(motorPinsA[pumpIdx], LOW);
      digitalWrite(motorPinsB[pumpIdx], LOW);
    }

    // -------------------------------------------------------------------------
    // LED-Steuerung
    // -------------------------------------------------------------------------

    // Setzt alle 12 LEDs abhängig von Glaserkennung und Slot-Konfiguration
    void updateGlassLeds(uint32_t* colors) {
      bool change = false;
      for (int i = 0; i < 12; i++) {
        uint32_t targetColor = 0;
        if (isGlassPresent(i)) {
          int assignedDrink = slotConfig[i];
          if (assignedDrink >= 0 && assignedDrink <= 2)
            targetColor = colors[assignedDrink];
        }
        int physicalIdx = 11 - i;  // LED-Reihenfolge ist umgekehrt
        if (leds.getPixelColor(physicalIdx) != targetColor) {
          leds.setPixelColor(physicalIdx, targetColor);
          change = true;
        }
      }
      if (change) leds.show();
    }

    // Überladung: alle Gläser in einer einzigen Farbe
    void updateGlassLeds(uint32_t singleColor) {
      bool change = false;
      for (int i = 0; i < 12; i++) {
        uint32_t targetColor = isGlassPresent(i) ? singleColor : 0;
        int physicalIdx = 11 - i;
        if (leds.getPixelColor(physicalIdx) != targetColor) {
          leds.setPixelColor(physicalIdx, targetColor);
          change = true;
        }
      }
      if (change) leds.show();
    }

    // Einzelne LED setzen (Index 0–11, physikalische Reihenfolge umgekehrt)
    void setLed(int index, uint32_t color) {
      if (index >= 0 && index < 12) {
        leds.setPixelColor(11 - index, color);
        leds.show();
      }
    }

    void clearLeds() {
      leds.clear();
      leds.show();
    }

    // -------------------------------------------------------------------------
    // Servo-Steuerung
    // -------------------------------------------------------------------------

    // Fährt den Arm sanft auf einen Zielwinkel.
    // Fahrtzeit wird proportional zur Winkel-Differenz berechnet (min. 300ms).
    // Nach der Bewegung wird der Servo optional getrennt (spart Strom).
    void moveArmSmooth(int targetAngle, bool detachAfter = true) {
      if (targetAngle == currentServoAngle) return;
      if (!armServo.attached()) {
        armServo.attach(PIN_SERVO, 500, 2400);
        delayMicroseconds(50000);
      }
      int travelTime = max(300, abs(targetAngle - currentServoAngle) * 3);
      armServo.write(targetAngle);
      delayMicroseconds(travelTime * 1000UL);
      currentServoAngle = targetAngle;
      if (detachAfter) armServo.detach();
    }

    void moveArmToPark() {
      moveArmSmooth(SERVO_POS_PARK, true);
    }

    void moveArmToSlot(int slotIndex, bool detachAfter = true) {
      if (slotIndex < 0 || slotIndex >= 12) return;
      moveArmSmooth(SERVO_POSITIONS[slotIndex], detachAfter);
    }
};

#endif