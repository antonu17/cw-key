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

constexpr int DEFAULT_WPM = 16;
constexpr int SIDETONE_HZ = 650;
constexpr int DEBOUNCE_MS = 3;
// Arduino analogWrite uses 0..255. Five percent is visible without glare.
constexpr int STATUS_LED_DUTY = 13;

// Keep the sidetone on when keying VBand over serial: VBand never sounds your
// own serial-keyed code. SpaceMarkStream gives no sounder to your own id, and
// keyer.js only sounds keyboard input, so the tone has to come from here.
// Setting either flag false only silences the hardware; element timing and the
// SM durations reported to VBand are unaffected.
constexpr bool ENABLE_SIDETONE = true;
constexpr bool ENABLE_STATUS_LED = true;

// Iambic mode B sends one extra element from memory when both levers are
// released mid-element; mode A stops instead. Set false for mode A.
constexpr bool IAMBIC_MODE_B = true;
// Straight-key mode keys directly from either lever and measures real
// durations instead of generating them. No spare GPIO for a mode button yet,
// so this is a build-time choice.
constexpr bool STRAIGHT_KEY_MODE = false;
// Received code is sounded at a different pitch from local code, matching CW
// Hotline's behaviour of distinguishing local from remote sending.
constexpr int RX_SIDETONE_HZ = 500;

// VBand's own WebSocket relay, read from js/websockets.js. The plain-ws port
// needs no TLS stack, which keeps the firmware inside the 2MB flash budget.
constexpr char VBAND_WS_HOST[] = "hamradio.solutions";
constexpr int VBAND_WS_PORT = 7385;
constexpr char VBAND_WS_PROTOCOL[] = "lws-hrs-vband2";
constexpr char VBAND_CLIENT_VERSION[] = "VB 2.0";
// Credentials are provisioned at runtime over serial into NVS, never stored
// in this file. Only the non-secret defaults live here.
constexpr char DEFAULT_CHANNEL[] = "1";
constexpr char DEFAULT_NAME[] = "CWKEY";
constexpr uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;

