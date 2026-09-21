// config.h - wiring and options for the relay unit.
// Edit this file to match how you wired the board. Nothing else needs changing.
#pragma once

// ---- Pins (ESP32-S3-WROOM-1 N16R8 development board) ------------------------
// Avoid: GPIO 0, 3, 45, 46 (boot strapping), 19, 20 (USB), 26-32 (flash),
//        33-37 (octal flash / PSRAM on N16R8), 43, 44 (UART0).
// Always check the pin labels printed on your own board.
#define PIN_ENLARGER   4     // relay module input IN1: enlarger
#define PIN_SAFELIGHT  5     // relay module input IN2: safelight
#define PIN_PEDAL      6     // foot pedal between this pin and GND (internal pull-up, normally open)
#define PIN_BUZZER     7     // passive piezo buzzer, other leg to GND

// ---- Output polarity ---------------------------------------------------------
// 0 = the relay turns ON when the pin is LOW. This is right for the common blue
//     2-channel relay module with optocoupler inputs (Songle SRD-05VDC-SL-C relays).
//     An unconnected or floating input leaves the relay OFF.
// 1 = the relay turns ON when the pin is HIGH (for example a transistor driver
//     between the pin and a relay or solid state relay input).
#define ENLARGER_ACTIVE_HIGH   0
#define SAFELIGHT_ACTIVE_HIGH  0

// ---- Timing ------------------------------------------------------------------
#define PEDAL_DEBOUNCE_MS   30
#define PEDAL_LONG_MS       1000     // hold this long to cancel

// ---- Bluetooth -----------------------------------------------------------------
#define BLE_DEVICE_NAME  "FStopRelay"

// ---- Debug output on the serial port (115200 baud) ------------------------------
#define SERIAL_DEBUG 1
