#pragma once

// Waveshare ESP-C3-32S-Kit + SSD1306 I2C prototype pinout.
// GPIO9 is the ESP32-C3 BOOT strapping pin and is connected to the board's
// user/BOOT button. Do not hold DIT while resetting or powering on the board.
constexpr int PIN_PADDLE_DIT = 9;
constexpr int PIN_PADDLE_DAH = 10;
// GPIO4 also drives one channel of the extremely bright onboard RGB LED.
// The passive piezo was moved to GPIO0 so its tone does not drive that LED.
constexpr int PIN_BUZZER = 0;
constexpr int PIN_STATUS_LED = 4;
constexpr int PIN_I2C_SDA = 6;
constexpr int PIN_I2C_SCL = 7;

constexpr uint8_t OLED_ADDRESS = 0x3C;
constexpr int OLED_WIDTH = 128;
constexpr int OLED_HEIGHT = 64;

constexpr int DEFAULT_WPM = 18;
constexpr int SIDETONE_HZ = 650;
constexpr int DEBOUNCE_MS = 3;
// Arduino analogWrite uses 0..255. Five percent is visible without glare.
constexpr int STATUS_LED_DUTY = 13;
