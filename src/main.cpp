#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Wire.h>
#include <string.h>
#include <strings.h>

#include "config.h"

#include <WiFi.h>
#include <WebSocketsClient.h>
#include <Preferences.h>

namespace {

// Forward declarations: the status beeper and link label are defined further
// down, after the queue and network state they depend on.
void queueMorseText(const char* text);
const char* netLabel();
void netBegin();
void joinChannel();
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

enum class Element : uint8_t { None, Dit, Dah };

// VBand (hamradio.solutions/vband) talks to a CW Hotline over Web Serial at
// 115200 baud rather than emulating a keyboard. It sends "VBand On" and
// "VBand Off", and expects one "SM,<space_ms>,<mark_ms>" line per completed
// element: space is the silence before the mark, mark is the key-down time.
// Both are clamped to VBand's max_time, where 3000 doubles as its idle
// sentinel (see decoder.js and stream.js on the VBand site).
constexpr uint32_t VBAND_MAX_TIME_MS = 3000;

bool vbandMode = false;
uint32_t lastMarkEndMs = 0;

// Long enough for an "SSID <value>" provisioning line.
char serialLine[96];
uint8_t serialLen = 0;

Preferences prefs;
String cfgSsid;
String cfgPass;   // held only in RAM and NVS; never printed or logged
String cfgChannel = DEFAULT_CHANNEL;
String cfgName = DEFAULT_NAME;

enum class NetState : uint8_t { NeedsConfig, Connecting, WifiUp, WsUp, Joined, Failed };
NetState netState = NetState::NeedsConfig;
uint32_t connectStartMs = 0;

WebSocketsClient ws;
String myWsId;
bool wsJoined = false;
bool wsDebug = true;   // logs inbound frames; DEBUG OFF to silence

uint32_t ditMs() { return 1200U / DEFAULT_WPM; }

// ---------------------------------------------------------------- debouncing

// Level must hold steady for DEBOUNCE_MS before it is believed. Replaces the
// old blocking delay(DEBOUNCE_MS), which cost real keying time.
struct Debounced {
  bool stable = false;
  bool candidate = false;
  uint32_t changedMs = 0;

  bool update(bool raw, uint32_t now) {
    if (raw != candidate) {
      candidate = raw;
      changedMs = now;
    } else if (candidate != stable && (now - changedMs) >= (uint32_t)DEBOUNCE_MS) {
      stable = candidate;
    }
    return stable;
  }
};

Debounced ditLever;
Debounced dahLever;
bool ditHeld = false;
bool dahHeld = false;

// ------------------------------------------------------------- tone arbitration

// Local keying and received code share one buzzer, so ownership is explicit
// and local sending always wins.
enum class ToneOwner : uint8_t { None, Local, Remote };
ToneOwner toneOwner = ToneOwner::None;

void toneStart(ToneOwner owner, int hz) {
  if (toneOwner == ToneOwner::Local && owner == ToneOwner::Remote) return;
  toneOwner = owner;
  if (ENABLE_SIDETONE) tone(PIN_BUZZER, hz);
}

void toneStop(ToneOwner owner) {
  if (toneOwner != owner) return;
  toneOwner = ToneOwner::None;
  if (ENABLE_SIDETONE) noTone(PIN_BUZZER);
}

void keyLed(bool on) {
  if (ENABLE_STATUS_LED) analogWrite(PIN_STATUS_LED, on ? STATUS_LED_DUTY : 0);
}

// -------------------------------------------------------------- morse decoding

struct MorseEntry {
  const char* pattern;
  char ch;
};

constexpr MorseEntry MORSE_TABLE[] = {
    {".-", 'A'},     {"-...", 'B'},   {"-.-.", 'C'},   {"-..", 'D'},
    {".", 'E'},      {"..-.", 'F'},   {"--.", 'G'},    {"....", 'H'},
    {"..", 'I'},     {".---", 'J'},   {"-.-", 'K'},    {".-..", 'L'},
    {"--", 'M'},     {"-.", 'N'},     {"---", 'O'},    {".--.", 'P'},
    {"--.-", 'Q'},   {".-.", 'R'},    {"...", 'S'},    {"-", 'T'},
    {"..-", 'U'},    {"...-", 'V'},   {".--", 'W'},    {"-..-", 'X'},
    {"-.--", 'Y'},   {"--..", 'Z'},
    {"-----", '0'},  {".----", '1'},  {"..---", '2'},  {"...--", '3'},
    {"....-", '4'},  {".....", '5'},  {"-....", '6'},  {"--...", '7'},
    {"---..", '8'},  {"----.", '9'},
    {".-.-.-", '.'}, {"--..--", ','}, {"..--..", '?'}, {"-..-.", '/'},
    {"-...-", '='},  {".-.-.", '+'},  {"-....-", '-'}, {"---...", ':'},
    {".----.", '\''},{".-..-.", '"'}, {"-.--.", '('},  {"-.--.-", ')'},
    {".--.-.", '@'}, {"-.-.--", '!'},
};

constexpr uint8_t PATTERN_CAPACITY = 7;
char pattern[PATTERN_CAPACITY + 1];
uint8_t patternLen = 0;

constexpr uint8_t DECODE_COLS = 21;
// Received code gets the larger pane: in a QSO it is what you are reading.
constexpr uint8_t RX_ROWS = 4;
constexpr uint8_t DECODE_ROWS = 2;
constexpr uint16_t RX_CAPACITY = DECODE_COLS * RX_ROWS;
constexpr uint16_t DECODE_CAPACITY = DECODE_COLS * DECODE_ROWS;
char decodedText[DECODE_CAPACITY + 1];
uint16_t decodedLen = 0;
char rxText[RX_CAPACITY + 1];
uint16_t rxTextLen = 0;

bool displayDirty = true;
bool wordSpaceSent = true;

char lookupPattern(const char* p) {
  for (const MorseEntry& entry : MORSE_TABLE) {
    if (strcmp(entry.pattern, p) == 0) return entry.ch;
  }
  return '*';  // unknown pattern, including an overrun squeeze
}

void appendTo(char* buf, uint16_t& len, uint16_t capacity, char c) {
  if (len == capacity) {
    memmove(buf, buf + DECODE_COLS, capacity - DECODE_COLS);
    len -= DECODE_COLS;
  }
  buf[len++] = c;
  buf[len] = '\0';
}

void appendDecoded(char c) { appendTo(decodedText, decodedLen, DECODE_CAPACITY, c); }
void appendReceived(char c) { appendTo(rxText, rxTextLen, RX_CAPACITY, c); }

void pushSymbol(char symbol) {
  if (patternLen < PATTERN_CAPACITY) pattern[patternLen++] = symbol;
  wordSpaceSent = false;
}

void flushPattern() {
  if (patternLen == 0) return;
  pattern[patternLen] = '\0';
  appendDecoded(lookupPattern(pattern));
  patternLen = 0;
  displayDirty = true;
}

// ------------------------------------------------------------------- display

void renderDisplay() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(0, 0);
  display.print(DEFAULT_WPM);
  display.print(F(" WPM"));
  if (STRAIGHT_KEY_MODE) display.print(F(" SK"));
  const char* label = netLabel();
  display.setCursor((DECODE_COLS - strlen(label)) * 6, 0);
  display.print(label);
  display.drawFastHLine(0, 10, OLED_WIDTH, SSD1306_WHITE);

  char row[DECODE_COLS + 1];
  // Received code, top pane.
  for (uint8_t r = 0; r < RX_ROWS; r++) {
    const uint16_t start = r * DECODE_COLS;
    if (start >= rxTextLen) break;
    uint16_t len = rxTextLen - start;
    if (len > DECODE_COLS) len = DECODE_COLS;
    memcpy(row, rxText + start, len);
    row[len] = '\0';
    display.setCursor(0, 14 + r * 8);
    display.print(row);
  }
  display.drawFastHLine(0, 46, OLED_WIDTH, SSD1306_WHITE);
  // Your own sending, bottom pane.
  for (uint8_t r = 0; r < DECODE_ROWS; r++) {
    const uint16_t start = r * DECODE_COLS;
    if (start >= decodedLen) break;
    uint16_t len = decodedLen - start;
    if (len > DECODE_COLS) len = DECODE_COLS;
    memcpy(row, decodedText + start, len);
    row[len] = '\0';
    display.setCursor(0, 48 + r * 8);
    display.print(row);
  }

  display.display();
  displayDirty = false;
}

// --------------------------------------------------------------- VBand serial

void reportSpaceMark(uint32_t space, uint32_t mark) {
  if (space > VBAND_MAX_TIME_MS) space = VBAND_MAX_TIME_MS;
  if (mark > VBAND_MAX_TIME_MS) mark = VBAND_MAX_TIME_MS;
  Serial.print(F("SM,"));
  Serial.print(space);
  Serial.print(',');
  Serial.print(mark);
  Serial.print('\n');
}

const char* authName(int mode) {
  switch (mode) {
    case 0: return "OPEN";
    case 1: return "WEP";
    case 2: return "WPA";
    case 3: return "WPA2";
    case 4: return "WPA/WPA2";
    case 6: return "WPA3";
    case 7: return "WPA2/WPA3";
    default: return "OTHER";
  }
}

void saveConfig() {
  prefs.putString("ssid", cfgSsid);
  prefs.putString("pass", cfgPass);
  prefs.putString("chan", cfgChannel);
  prefs.putString("name", cfgName);
}

void loadConfig() {
  prefs.begin("cwkey", false);
  cfgSsid = prefs.getString("ssid", "");
  cfgPass = prefs.getString("pass", "");
  cfgChannel = prefs.getString("chan", DEFAULT_CHANNEL);
  cfgName = prefs.getString("name", DEFAULT_NAME);
}

// Runtime provisioning so credentials live in NVS instead of the firmware
// image. The password is never echoed back, only reported as set or unset.
void handleConfigLine(char* line) {
  char* value = strchr(line, ' ');
  if (value != nullptr) {
    *value = '\0';
    value++;
    while (*value == ' ') value++;
  }
  const bool has = value != nullptr && *value != '\0';

  if (strcasecmp(line, "SSID") == 0 && has) {
    cfgSsid = value;
    Serial.println(F("ssid set"));
  } else if (strcasecmp(line, "PASS") == 0 && has) {
    cfgPass = value;
    Serial.println(F("pass set"));
  } else if (strcasecmp(line, "CHAN") == 0 && has) {
    cfgChannel = value;
    Serial.println(F("chan set"));
  } else if (strcasecmp(line, "NAME") == 0 && has) {
    cfgName = value;
    Serial.println(F("name set"));
  } else if (strcasecmp(line, "SAVE") == 0) {
    saveConfig();
    Serial.println(F("saved, reconnecting"));
    ws.disconnect();
    wsJoined = false;
    netBegin();
  } else if (strcasecmp(line, "SHOW") == 0) {
    Serial.print(F("ssid="));
    Serial.println(cfgSsid.c_str());
    Serial.print(F("pass="));
    Serial.println(cfgPass.length() ? "<set>" : "<unset>");
    Serial.print(F("chan="));
    Serial.println(cfgChannel.c_str());
    Serial.print(F("name="));
    Serial.println(cfgName.c_str());
    Serial.print(F("link="));
    Serial.println(netLabel());
  } else if (strcasecmp(line, "CLEAR") == 0) {
    prefs.clear();
    cfgSsid = "";
    cfgPass = "";
    Serial.println(F("cleared"));
  } else if (strcasecmp(line, "DEBUG") == 0 && has) {
    wsDebug = strcasecmp(value, "ON") == 0;
    Serial.println(wsDebug ? "debug on" : "debug off");
  } else if (strcasecmp(line, "JOIN") == 0 && has) {
    // Retry a join without re-provisioning, for probing channel names.
    cfgChannel = value;
    wsJoined = false;
    joinChannel();
    Serial.print(F("joining "));
    Serial.println(cfgChannel.c_str());
  } else if (strcasecmp(line, "SCAN") == 0) {
    // Blocks for a couple of seconds, so it is a diagnostic command only.
    Serial.println(F("scanning 2.4GHz (the C3 cannot see 5GHz at all)..."));
    const int found = WiFi.scanNetworks();
    for (int i = 0; i < found; i++) {
      Serial.print(WiFi.SSID(i).c_str());
      Serial.print(F("  rssi="));
      Serial.print((int)WiFi.RSSI(i));
      Serial.print(F("  ch="));
      Serial.print((int)WiFi.channel(i));
      Serial.print(F("  enc="));
      Serial.print(authName(WiFi.encryptionType(i)));
      Serial.print("\n");
    }
    if (found == 0) Serial.println(F("no 2.4GHz networks visible"));
    WiFi.scanDelete();
  } else if (strcasecmp(line, "HELP") == 0) {
    Serial.println(F("SSID x|PASS x|CHAN x|NAME x|SAVE|SHOW|SCAN|JOIN x|DEBUG on|CLEAR"));
  }
}

void pollVBandCommands() {
  while (Serial.available()) {
    const char c = Serial.read();
    if (c == '\n') {
      serialLine[serialLen] = '\0';
      if (strcasecmp(serialLine, "VBand On") == 0) {
        vbandMode = true;
        lastMarkEndMs = millis();
        displayDirty = true;
        queueMorseText("VB");
      } else if (strcasecmp(serialLine, "VBand Off") == 0) {
        vbandMode = false;
        displayDirty = true;
        queueMorseText("VX");
      } else if (!vbandMode && serialLen > 0) {
        // Config commands only outside VBand mode, so the SM stream stays clean.
        handleConfigLine(serialLine);
      }
      serialLen = 0;
    } else if (c != '\r' && serialLen < sizeof(serialLine) - 1) {
      serialLine[serialLen++] = c;
    }
  }
}

// ------------------------------------------------------------- local key edges

uint32_t markStartMs = 0;
bool localKeyDown = false;

void localKeyOn(uint32_t now) {
  if (localKeyDown) return;
  localKeyDown = true;
  markStartMs = now;
  toneStart(ToneOwner::Local, SIDETONE_HZ);
  keyLed(true);
}

// Reports the completed element to VBand and feeds the decoder. `symbol` is
// known outright in iambic mode; in straight-key mode it is classified from
// the measured mark, since the operator's fist supplies the timing.
void localKeyOff(uint32_t now, Element known) {
  if (!localKeyDown) return;
  localKeyDown = false;
  toneStop(ToneOwner::Local);
  keyLed(false);

  // Unsigned subtraction stays correct across millis() rollover.
  uint32_t space = markStartMs - lastMarkEndMs;
  uint32_t mark = now - markStartMs;
  if (space > VBAND_MAX_TIME_MS) space = VBAND_MAX_TIME_MS;
  if (mark > VBAND_MAX_TIME_MS) mark = VBAND_MAX_TIME_MS;

  if (vbandMode) {
    reportSpaceMark(space, mark);
  } else if (wsJoined) {
    // CW Hotline disables outgoing WiFi CW while in VBand serial mode, so the
    // channel never receives the same element twice.
    char frame[32];
    snprintf(frame, sizeof(frame), "SM,%lu,%lu", (unsigned long)space, (unsigned long)mark);
    ws.sendTXT(frame);
  }
  lastMarkEndMs = now;

  if (known != Element::None) {
    pushSymbol(known == Element::Dit ? '.' : '-');
  } else {
    pushSymbol(mark < 2U * ditMs() ? '.' : '-');
  }
}

// ------------------------------------------------------------- iambic keyer

enum class KeyPhase : uint8_t { Idle, Mark, Space };

KeyPhase phase = KeyPhase::Idle;
uint32_t phaseStartMs = 0;
uint32_t phaseLenMs = 0;
Element currentElement = Element::None;
Element lastElement = Element::Dah;
bool ditMemory = false;
bool dahMemory = false;

void beginElement(Element element, uint32_t now) {
  currentElement = element;
  lastElement = element;
  phase = KeyPhase::Mark;
  phaseStartMs = now;
  phaseLenMs = (element == Element::Dit) ? ditMs() : 3U * ditMs();
  if (element == Element::Dit) ditMemory = false; else dahMemory = false;
  localKeyOn(now);
}

// Latching the opposite lever mid-element is what makes a keyer feel iambic.
// Only the opposite lever is latched: remembering the lever already being sent
// would queue a spurious extra element every time it was released.
void latchMemory() {
  if (currentElement == Element::Dit) {
    if (dahHeld) dahMemory = true;
  } else if (currentElement == Element::Dah) {
    if (ditHeld) ditMemory = true;
  }
}

Element chooseNextElement() {
  const Element opposite = (lastElement == Element::Dit) ? Element::Dah : Element::Dit;
  const bool oppositeHeld = (opposite == Element::Dit) ? ditHeld : dahHeld;
  const bool oppositeMemory = (opposite == Element::Dit) ? ditMemory : dahMemory;
  const bool sameHeld = (lastElement == Element::Dit) ? ditHeld : dahHeld;

  if (oppositeHeld || oppositeMemory) return opposite;
  if (sameHeld) return lastElement;
  return Element::None;
}

void updateIambicKeyer(uint32_t now) {
  switch (phase) {
    case KeyPhase::Idle:
      // Memory is always consumed in the Space phase, so idle starts fresh.
      ditMemory = false;
      dahMemory = false;
      if (ditHeld) {
        beginElement(Element::Dit, now);
      } else if (dahHeld) {
        beginElement(Element::Dah, now);
      }
      break;

    case KeyPhase::Mark:
      latchMemory();
      if (now - phaseStartMs >= phaseLenMs) {
        localKeyOff(now, currentElement);
        phase = KeyPhase::Space;
        phaseStartMs = now;
        phaseLenMs = ditMs();
      }
      break;

    case KeyPhase::Space:
      latchMemory();
      if (now - phaseStartMs >= phaseLenMs) {
        if (!IAMBIC_MODE_B && !ditHeld && !dahHeld) {
          ditMemory = false;
          dahMemory = false;
        }
        const Element next = chooseNextElement();
        if (next == Element::None) {
          phase = KeyPhase::Idle;
        } else {
          beginElement(next, now);
        }
      }
      break;
  }
}

// --------------------------------------------------------- straight key mode

void updateStraightKey(uint32_t now) {
  const bool pressed = ditHeld || dahHeld;
  if (pressed && !localKeyDown) {
    localKeyOn(now);
  } else if (!pressed && localKeyDown) {
    localKeyOff(now, Element::None);  // classify from the measured mark
  }
}

// ------------------------------------------------------- received code player

// Stage 2 (WiFi/WebSocket) feeds this from inbound SMK frames. It is driven
// here so playback timing never blocks the keyer.
struct SpaceMark {
  uint16_t space;
  uint16_t mark;
  bool decode;   // false for our own status beeps, which must not self-decode
};

constexpr uint8_t RX_QUEUE_LEN = 32;
SpaceMark rxQueue[RX_QUEUE_LEN];
uint8_t rxHead = 0;
uint8_t rxTail = 0;
uint8_t rxCount = 0;

// Received code carries another operator's timing, so dit/dah and the gaps
// must be classified from measured durations against an adaptive unit rather
// than assumed from DEFAULT_WPM. Integer EMA: no soft-float on the C3.
// A whole character is buffered and classified together, against the shortest
// and longest mark within that character. Classifying each mark as it arrives
// against a running unit cannot work: the estimate is fed by its own output,
// so one misread dah drags the boundary up and every later dah is misread too.
char rxPattern[PATTERN_CAPACITY + 1];
uint16_t rxMarks[PATTERN_CAPACITY];
uint8_t rxMarkCount = 0;
uint32_t rxDitMs = 0;           // unit estimate, seeded in setup
uint32_t rxLastMarkEndMs = 0;
bool rxWordSpaceSent = true;

// Drops instantly when the sender is faster than we assumed, but rises only
// gradually, so one long mark cannot inflate the estimate.
void rxUpdateUnit(uint32_t observed) {
  if (observed < 20) observed = 20;      // 60 WPM ceiling
  if (observed > 240) observed = 240;    // 5 WPM floor
  rxDitMs = (observed < rxDitMs) ? observed : (rxDitMs * 3 + observed) / 4;
}

void rxFlushPattern() {
  if (rxMarkCount == 0) return;
  uint32_t lo = rxMarks[0];
  uint32_t hi = rxMarks[0];
  for (uint8_t i = 1; i < rxMarkCount; i++) {
    if (rxMarks[i] < lo) lo = rxMarks[i];
    if (rxMarks[i] > hi) hi = rxMarks[i];
  }
  // Clearly bimodal: split this character on its own evidence. Otherwise it is
  // all dits or all dahs, so fall back to the running estimate.
  const uint32_t threshold = (hi >= 2 * lo) ? (lo + hi) / 2 : 2 * rxDitMs;
  for (uint8_t i = 0; i < rxMarkCount; i++) {
    rxPattern[i] = (rxMarks[i] < threshold) ? '.' : '-';
  }
  rxPattern[rxMarkCount] = '\0';
  appendReceived(lookupPattern(rxPattern));
  rxUpdateUnit(lo);
  rxMarkCount = 0;
  displayDirty = true;
}

void rxOnSpace(uint32_t space) {
  if (space >= 5 * rxDitMs) {
    rxFlushPattern();
    if (!rxWordSpaceSent && rxTextLen > 0) {
      appendReceived(' ');
      rxWordSpaceSent = true;
      displayDirty = true;
    }
  } else if (space >= 2 * rxDitMs) {
    rxFlushPattern();
  } else if (space > 0) {
    // An intra-character space is exactly one unit, so it is the cleanest
    // speed reference available and needs no classification to be useful.
    rxUpdateUnit(space);
  }
}

void rxOnMark(uint32_t mark) {
  if (rxMarkCount < PATTERN_CAPACITY) rxMarks[rxMarkCount++] = (uint16_t)mark;
  rxWordSpaceSent = false;
}

enum class RxPhase : uint8_t { Idle, Space, Mark };
RxPhase rxPhase = RxPhase::Idle;
uint32_t rxPhaseStartMs = 0;
uint32_t rxPhaseLenMs = 0;
uint16_t rxPendingMark = 0;
bool rxPendingDecode = false;

bool rxEnqueue(uint16_t space, uint16_t mark, bool decode = true) {
  if (rxCount == RX_QUEUE_LEN) return false;  // drop rather than stall
  rxQueue[rxTail] = {space, mark, decode};
  rxTail = (rxTail + 1) % RX_QUEUE_LEN;
  rxCount++;
  return true;
}

void updateRxPlayer(uint32_t now) {
  switch (rxPhase) {
    case RxPhase::Idle:
      if (rxCount > 0) {
        const SpaceMark sm = rxQueue[rxHead];
        rxHead = (rxHead + 1) % RX_QUEUE_LEN;
        rxCount--;
        rxPendingMark = sm.mark;
        rxPendingDecode = sm.decode;
        if (sm.decode) rxOnSpace(sm.space);
        rxPhase = RxPhase::Space;
        rxPhaseStartMs = now;
        // A max_time space means "idle", not a literal three-second wait.
        rxPhaseLenMs = (sm.space >= VBAND_MAX_TIME_MS) ? 0 : sm.space;
      } else if (rxMarkCount > 0 && (now - rxLastMarkEndMs) >= 4 * rxDitMs) {
        // Nothing more arrived, so close out the letter the sender finished.
        // Deliberately slacker than the 2-unit letter gap used for a space we
        // were actually told about: here the silence may just be network jitter
        // between bursts, and splitting a character on that would be worse.
        rxFlushPattern();
      }
      break;

    case RxPhase::Space:
      if (now - rxPhaseStartMs >= rxPhaseLenMs) {
        rxPhase = RxPhase::Mark;
        rxPhaseStartMs = now;
        rxPhaseLenMs = rxPendingMark;
        toneStart(ToneOwner::Remote, RX_SIDETONE_HZ);
      }
      break;

    case RxPhase::Mark:
      if (now - rxPhaseStartMs >= rxPhaseLenMs) {
        toneStop(ToneOwner::Remote);
        if (rxPendingDecode) {
          rxOnMark(rxPhaseLenMs);
          rxLastMarkEndMs = now;
        }
        rxPhase = RxPhase::Idle;
      }
      break;
  }
}

bool rxBusy() { return rxPhase != RxPhase::Idle || rxCount > 0; }

const char* patternForChar(char c) {
  for (const MorseEntry& entry : MORSE_TABLE) {
    if (entry.ch == c) return entry.pattern;
  }
  return nullptr;
}

// CW Hotline announces its state in Morse over the speaker (OK, NC, NET, VB,
// VX). Reusing the RX queue means the status beeps are non-blocking too.
void queueMorseText(const char* text) {
  const uint16_t d = (uint16_t)ditMs();
  bool first = true;
  for (const char* p = text; *p; p++) {
    const char* pat = patternForChar(*p);
    if (pat == nullptr) continue;
    for (uint8_t i = 0; pat[i] != '\0'; i++) {
      const uint16_t space = first ? 0 : (i == 0 ? 3 * d : d);
      const uint16_t mark = (pat[i] == '.') ? d : 3 * d;
      if (!rxEnqueue(space, mark, false)) return;
      first = false;
    }
  }
}

// ------------------------------------------------------------------ network

void setNetState(NetState next) {
  if (netState == next) return;
  netState = next;
  displayDirty = true;
}

const char* netLabel() {
  if (vbandMode) return "VBAND";
  switch (netState) {
    case NetState::NeedsConfig: return "CFG";
    case NetState::Connecting:  return "CONN";
    case NetState::WifiUp:      return "WIFI";
    case NetState::WsUp:        return "WS";
    case NetState::Joined:      return "LINK";
    case NetState::Failed:      return "NC";
  }
  return "";
}

// Inbound keying: "SMK,<channel>,<id>,<name>,<space>,<mark>" (vband.js relays
// it to codeManager.incomingSpaceMark, which reads fields 3 and 4).
void handleSmk(const char* payload) {
  const char* field[6] = {nullptr};
  char buf[128];
  strncpy(buf, payload, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  uint8_t n = 0;
  char* cursor = buf;
  field[n++] = cursor;
  while (*cursor != '\0' && n < 6) {
    if (*cursor == ',') {
      *cursor = '\0';
      field[n++] = cursor + 1;
    }
    cursor++;
  }
  if (n < 5) return;
  // Skip our own code echoed back by the server.
  if (myWsId.length() > 0 && myWsId == field[1]) return;
  const long space = atol(field[3]);
  const long mark = atol(field[4]);
  if (mark <= 0) return;
  rxEnqueue((uint16_t)constrain(space, 0, (long)VBAND_MAX_TIME_MS),
            (uint16_t)constrain(mark, 0, (long)VBAND_MAX_TIME_MS));
}

// A private channel must be registered with LU before it can be joined; the
// browser does LU then LC then JC ("LC after LU for private" in vband.js).
// Harmless for public channels, which LC lists anyway.
void joinChannel() {
  char frame[80];
  snprintf(frame, sizeof(frame), "LU,%s", cfgChannel.c_str());
  ws.sendTXT(frame);
  ws.sendTXT("LC");
  snprintf(frame, sizeof(frame), "JC,%s", cfgChannel.c_str());
  ws.sendTXT(frame);
}

void handleWsText(const char* msg) {
  if (wsDebug && !vbandMode) {
    Serial.print(F("ws< "));
    Serial.print(msg);
    Serial.print("\n");
  }
  if (strncmp(msg, "COK,", 4) == 0) {
    const char* idStart = msg + 4;
    const char* comma = strchr(idStart, ',');
    myWsId = comma ? String(idStart).substring(0, comma - idStart) : String(idStart);
    joinChannel();
  } else if (strncmp(msg, "CJN,", 4) == 0) {
    wsJoined = true;
    setNetState(NetState::Joined);
  } else if (strncmp(msg, "SMK,", 4) == 0) {
    handleSmk(msg + 4);
  } else if (strncmp(msg, "CNF,", 4) == 0) {
    wsJoined = false;
    setNetState(NetState::Failed);
    queueMorseText("NC");
  }
}

void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      setNetState(NetState::WsUp);
      char frame[80];
      snprintf(frame, sizeof(frame), "CN,%s,0,%s", cfgName.c_str(), VBAND_CLIENT_VERSION);
      ws.sendTXT(frame);
      break;
    }
    case WStype_DISCONNECTED:
      wsJoined = false;
      myWsId = "";
      if (netState == NetState::Joined || netState == NetState::WsUp) {
        setNetState(NetState::WifiUp);
        queueMorseText("NET");
      }
      break;
    case WStype_TEXT: {
      char msg[192];
      const size_t n = length < sizeof(msg) - 1 ? length : sizeof(msg) - 1;
      memcpy(msg, payload, n);
      msg[n] = '\0';
      handleWsText(msg);
      break;
    }
    default:
      break;
  }
}

void netBegin() {
  if (cfgSsid.length() == 0) {
    setNetState(NetState::NeedsConfig);
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
  connectStartMs = millis();
  setNetState(NetState::Connecting);
}

void netUpdate(uint32_t now) {
  if (netState == NetState::NeedsConfig) return;

  const bool wifiUp = WiFi.status() == WL_CONNECTED;

  if (netState == NetState::Connecting) {
    if (wifiUp) {
      setNetState(NetState::WifiUp);
      queueMorseText("OK");
      ws.begin(VBAND_WS_HOST, VBAND_WS_PORT, "/", VBAND_WS_PROTOCOL);
      ws.onEvent(onWsEvent);
      ws.setReconnectInterval(5000);
    } else if (now - connectStartMs >= WIFI_CONNECT_TIMEOUT_MS) {
      // CW Hotline falls back to being a plain offline keyer and reports NC.
      const int status = WiFi.status();
      setNetState(NetState::Failed);
      queueMorseText("NC");
      if (!vbandMode) {
        // 1 = SSID not visible (wrong name, or a 5GHz-only hotspot).
        // 4 = found but rejected us (bad password, or WPA3-only).
        Serial.print(F("wifi failed, status="));
        Serial.print(status);
        Serial.print(F(" (1=ssid not found, 4=auth failed); try SCAN"));
        Serial.print("\n");
      }
    }
    return;
  }

  if (!wifiUp) {
    if (netState != NetState::Failed) {
      wsJoined = false;
      queueMorseText("NET");
      WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
      connectStartMs = now;
      setNetState(NetState::Connecting);
    }
    return;
  }
  ws.loop();
}

// ------------------------------------------------------------------- decoding

void updateDecoder(uint32_t now) {
  if (localKeyDown || phase != KeyPhase::Idle) return;
  const uint32_t idle = now - lastMarkEndMs;
  if (patternLen > 0 && idle >= 3U * ditMs()) {
    flushPattern();
  }
  if (patternLen == 0 && !wordSpaceSent && decodedLen > 0 && idle >= 7U * ditMs()) {
    appendDecoded(' ');
    wordSpaceSent = true;
    displayDirty = true;
  }
}
}  // namespace

void setup() {
  Serial.begin(115200);
  pinMode(PIN_PADDLE_DIT, INPUT_PULLUP);
  pinMode(PIN_PADDLE_DAH, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_STATUS_LED, OUTPUT);
  keyLed(false);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println(F("SSD1306 allocation/connection failed"));
    while (true) delay(1000);
  }
  decodedText[0] = '\0';
  rxText[0] = '\0';
  rxDitMs = ditMs();
  loadConfig();
  renderDisplay();
  lastMarkEndMs = millis();
  netBegin();
}

void loop() {
  const uint32_t now = millis();

  pollVBandCommands();

  // Switches short the input to ground; internal pull-ups hold idle HIGH.
  ditHeld = ditLever.update(digitalRead(PIN_PADDLE_DIT) == LOW, now);
  dahHeld = dahLever.update(digitalRead(PIN_PADDLE_DAH) == LOW, now);

  if (STRAIGHT_KEY_MODE) {
    updateStraightKey(now);
  } else {
    updateIambicKeyer(now);
  }
  netUpdate(now);
  updateRxPlayer(now);
  updateDecoder(now);

  // Refreshing costs tens of milliseconds over I2C, so it only happens with
  // the key up and nothing playing, never inside an element VBand is timing.
  if (displayDirty && !localKeyDown && phase == KeyPhase::Idle && !rxBusy()) {
    renderDisplay();
  }
}
