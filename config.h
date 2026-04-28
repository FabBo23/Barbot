// =============================================================================
// config.h – Hardware-Mapping und Parameter für BarBot
// =============================================================================
// Alle physischen Pin-Zuordnungen und Zeitkonstanten an einem Ort.
// Hardware-Änderungen nur hier vornehmen, nie direkt im Code.
// =============================================================================

#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// Versions-String – muss mit dem Git-Tag beim Release übereinstimmen (z.B. "v1.2.3")
#define FIRMWARE_VERSION "v1.0.0"

// =============================================================================
// PUMPEN-PINS (H-Brücken-Steuerung, je 2 Pins pro Motor: Vorwärts / Rückwärts)
// =============================================================================

#define PIN_P1_A    18  // Pumpe 1 – Vorwärts
#define PIN_P1_B    23  // Pumpe 1 – Rückwärts
#define PIN_P2_A    17  // Pumpe 2 – Vorwärts
#define PIN_P2_B     4  // Pumpe 2 – Rückwärts
#define PIN_P3_A    16  // Pumpe 3 – Vorwärts
#define PIN_P3_B    19  // Pumpe 3 – Rückwärts

// =============================================================================
// AKTOREN
// =============================================================================

#define PIN_SERVO    2  // PWM-Pin für den Servoarm
#define PIN_LEDS    15  // Datenleitung für WS2812 LED-Ring
#define NUM_LEDS    12  // Anzahl LEDs (eine pro Glas-Position)

// =============================================================================
// KOMMUNIKATION (UART zum CYD-Display / openHASP)
// =============================================================================

#define CYD_TX_PIN  21   // ESP TX → Display RX
#define CYD_RX_PIN  22   // Display TX → ESP RX
#define CYD_BAUD    115200

// =============================================================================
// EINGABE
// =============================================================================

#define PIN_BUTTON   5  // Physischer Start/Weiter-Knopf

// =============================================================================
// SENSOREN (Endschalter / Glaserkennung, 12 Positionen)
// =============================================================================

// Pins 34–39 am ESP32 sind input-only (kein internen Pullup → INPUT statt INPUT_PULLUP)
const int SWITCH_PINS[12] = {
  13, 12, 14, 27,
  26, 25, 33,
  32, 35, 34,
  39, 36
};

// Logik-Level wenn Glas erkannt/Schalter gedrückt
#define SWITCH_ACTIVE_STATE LOW

// =============================================================================
// SERVO-POSITIONEN (Winkel in Grad für jede der 12 Positionen)
// =============================================================================

const int SERVO_POSITIONS[12] = {
  10, 24, 39, 54, 68, 83,
  98, 112, 125, 140, 152, 167
};

#define SERVO_POS_PARK  0  // Ruheposition des Arms

// =============================================================================
// ZEITSTEUERUNG
// =============================================================================

// Einschenkzeit wird zur Laufzeit über pourTimeMs in DrinkMachine gesteuert.
// POUR_TIME_MS ist der Startwert beim ersten Hochladen (ohne gespeicherten Wert).
#define POUR_TIME_MS      4000  // Startwert Einschenkzeit in ms
#define ANTIDRIP_TIME_MS     0  // Rücklaufzeit der Pumpe gegen Nachtropfen (ms)

#define NUM_POSITIONS     12    // Anzahl der Slot-Positionen
#define BUTTON_DEBOUNCE   50    // Entprellzeit Taster in ms
#define LONG_PRESS_TIME   1000  // Haltezeit für langen Tastendruck in ms (reserviert, noch nicht aktiv)

#endif