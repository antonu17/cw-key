#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>

#include "config.h"

namespace {
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

enum class Element : uint8_t { None, Dit, Dah };

struct PaddleState {
  bool dit = false;
  bool dah = false;
};

PaddleState readPaddles() {
  // Switches short the input to ground; internal pull-ups hold idle HIGH.
  return {
      digitalRead(PIN_PADDLE_DIT) == LOW,
      digitalRead(PIN_PADDLE_DAH) == LOW,
  };
}

uint32_t ditMs() { return 1200U / DEFAULT_WPM; }

void showStatus(const char* event) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(F("CW TRAINER / KEYER"));
  display.drawFastHLine(0, 10, OLED_WIDTH, SSD1306_WHITE);
  display.setCursor(0, 18);
  display.print(F("Speed: "));
  display.print(DEFAULT_WPM);
  display.println(F(" WPM"));
  display.print(F("Tone:  "));
  display.print(SIDETONE_HZ);
  display.println(F(" Hz"));
  display.setTextSize(2);
  display.setCursor(0, 44);
  display.print(event);
  display.display();
}

void soundElement(Element element) {
  if (element == Element::None) return;

  const uint32_t duration = (element == Element::Dit) ? ditMs() : 3U * ditMs();
  const char* label = (element == Element::Dit) ? "DIT" : "DAH";
  showStatus(label);
  analogWrite(PIN_STATUS_LED, STATUS_LED_DUTY);
  tone(PIN_BUZZER, SIDETONE_HZ);
  delay(duration);
  noTone(PIN_BUZZER);
  analogWrite(PIN_STATUS_LED, 0);
  delay(ditMs());  // one-element inter-element gap
}
}  // namespace

void setup() {
  Serial.begin(115200);
  pinMode(PIN_PADDLE_DIT, INPUT_PULLUP);
  pinMode(PIN_PADDLE_DAH, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_STATUS_LED, OUTPUT);
  analogWrite(PIN_STATUS_LED, 0);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println(F("SSD1306 allocation/connection failed"));
    while (true) delay(1000);
  }
  showStatus("READY");
}

void loop() {
  static Element lastSqueezeElement = Element::Dah;
  const PaddleState paddles = readPaddles();

  Element next = Element::None;
  if (paddles.dit && paddles.dah) {
    // Minimal alternating squeeze behavior. A state-machine implementation
    // with element memory will replace this blocking prototype.
    next = (lastSqueezeElement == Element::Dit) ? Element::Dah : Element::Dit;
    lastSqueezeElement = next;
  } else if (paddles.dit) {
    next = Element::Dit;
  } else if (paddles.dah) {
    next = Element::Dah;
  }

  if (next != Element::None) {
    delay(DEBOUNCE_MS);
    soundElement(next);
  } else {
    delay(1);
  }
}
