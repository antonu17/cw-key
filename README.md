# DIY ESP32 CW trainer and iambic paddle

![Handmade PCB iambic paddle](key/img07.jpeg)

A homebrew Morse keyer built on an ESP32-C3 and a handmade dual paddle. It works
as a standalone practice keyer with sidetone and an on-screen decoder, and it can
also put you on [VBand](https://hamradio.solutions/vband/) — either through the
browser over USB serial, or straight over WiFi with no computer at all.

## Hardware

- Waveshare ESP-C3-32S-Kit (ESP32-C3, onboard CH340 USB-to-UART)
- 0.96-inch 128x64 SSD1306 I2C OLED, address `0x3C`
- Passive piezo buzzer (not an active fixed-tone buzzer)
- Two normally-open lever microswitches (Omron D2F/D2FC style)
- Breadboard, jumpers, USB cable

The OLED must be 3.3 V compatible. Check the markings before applying power.

## Wiring

All grounds common.

| Part | Terminal | ESP32 |
|---|---|---|
| OLED | VCC | 3V3 |
| OLED | GND | GND |
| OLED | SDA | GPIO 6 |
| OLED | SCL | GPIO 7 |
| Dit switch | COM | GND |
| Dit switch | NO | GPIO 9 |
| Dah switch | COM | GND |
| Dah switch | NO | GPIO 10 |
| Passive piezo | + | GPIO 0 through 100 ohm - 1 kOhm resistor |
| Passive piezo | - | GND |

Use `COM` and `NO`, not `NC` — confirm with a continuity meter, since terminal
markings vary between switch families.

Paddle inputs use internal pull-ups: open reads HIGH, pressed reads LOW. If dit
and dah come out swapped, change `PIN_PADDLE_DIT` / `PIN_PADDLE_DAH` in
`include/config.h` instead of rewiring.

**GPIO 9 is a strapping pin.** Don't hold the dit paddle while resetting or
powering on, or the board drops into download mode. The onboard BOOT button sits
on the same pin and will act like dit. Avoid GPIO 18/19 (native USB) and
GPIO 20/21 (the CH340 serial path). The piezo is on GPIO 0 to keep the sidetone
off GPIO 4, which drives the very bright onboard RGB LED — that LED now follows
the key at low PWM duty, set by `STATUS_LED_DUTY`.

A small piezo disc can be driven straight from the GPIO through the series
resistor. Anything inductive or higher-current needs a transistor and a flyback
diode. Don't drive a speaker directly from a pin.

## Build and upload

```sh
pio run
pio run --target upload
pio device monitor        # 115200 baud
```

## Using it

**As a standalone keyer.** Power it up and send. The top of the screen shows
speed and status; received code fills the upper pane and your own sending the
lower one, both decoded to text.

**On VBand through the browser.** Open VBand in Chrome or Edge (Web Serial isn't
in Firefox or Safari), go to Settings, click **Connect to CW Hotline** and pick
the CH340 port. Close the serial monitor first — the browser and the monitor
can't share the port. VBand deliberately doesn't sound your own serial-keyed
code, so the sidetone here comes from your buzzer.

**On VBand over WiFi, no computer.** Provision once over serial:

```
SSID your-network
PASS your-password
CHAN Channel 1
SAVE
```

Credentials go into NVS and survive reboots and reflashes. They are never stored
in the source tree — don't put them in `config.h`. You'll hear `OK` in Morse when
WiFi connects, and the header will move through `CONN` → `WIFI` → `WS` → `LINK`.
Once it says `LINK` you can unplug from the computer and run it on any USB power.

Other serial commands: `SHOW` (config, with the password masked), `SCAN` (list
2.4 GHz networks), `JOIN <channel>` (try a channel without saving), `NAME`,
`DEBUG on|off` (log WebSocket frames), `CLEAR`, `HELP`.

Channel names come from the server and are case-sensitive — `Practice Channel`,
`Channel 1`, and so on. `DEBUG on` prints the list at connect time.

The ESP32-C3 is **2.4 GHz only** and needs WPA2. If your phone hotspot won't
connect, that's usually the cause: turn on "Maximize Compatibility" on iPhone, or
pick the 2.4 GHz band on Android. On failure it beeps `NC` and prints a status
code, then keeps working as an offline keyer.

## Settings

Everything lives in `include/config.h`:

| Setting | Default | Notes |
|---|---|---|
| `DEFAULT_WPM` | 16 | keyer speed |
| `SIDETONE_HZ` | 650 | your own sending |
| `RX_SIDETONE_HZ` | 500 | received code, so you can tell them apart |
| `IAMBIC_MODE_B` | true | false for mode A |
| `STRAIGHT_KEY_MODE` | false | either lever keys directly, timing measured |
| `ENABLE_SIDETONE` | true | |
| `ENABLE_STATUS_LED` | true | |

## How the VBand link works

Worth writing down, because none of it is documented publicly and it was all
read out of VBand's own JavaScript.

VBand does **not** use keyboard emulation for hardware keys. Its adapter is a
serial device, and both transports carry the same payload:

```
SM,<space_ms>,<mark_ms>
```

`space` is the silence before the mark, `mark` is the key-down time, both in
milliseconds, both clamped to 3000 (which VBand also treats as its idle
sentinel). So the device does its own timing and reports each finished element —
that's why there's no latency and why the browser doesn't need focus.

- **Serial:** Web Serial at 115200. The browser sends `VBand On` / `VBand Off`.
- **WiFi:** `ws://hamradio.solutions:7385/`, subprotocol `lws-hrs-vband2`.
  Announce with `CN,<name>,0,VB 2.0`, take your id from `COK`, `LU`+`JC` to join
  a channel, then `SM` out and `SMK` in.

Real CW Hotline units also link device-to-device through a vendor server that
authenticates a per-device key, which a homebrew board can't join. Talking to
VBand's own relay gets to the same place without it.

## Decoding

Two decoders, because the two directions are different problems.

Your own sending needs no timing analysis — the keyer generated the elements, so
it already knows each one is a dit or a dah.

Received code is another operator's fist at their speed, so it's classified from
measured durations. A whole character is buffered and split against its own
shortest and longest mark; the speed estimate is tracked from intra-character
spaces, which are exactly one unit by definition. An abrupt mid-stream speed
change costs about one garbled character while it re-converges.

## Status codes

Beeped in Morse, the same convention CW Hotline uses:

| Code | Meaning |
|---|---|
| `OK` | WiFi connected |
| `NC` | WiFi failed; running offline |
| `NET` | connection dropped, retrying |
| `VB` / `VX` | entering / leaving VBand serial mode |

## Bring-up checklist

1. With power off, continuity-test each paddle from its GPIO wire to ground.
2. Power the ESP32 alone and check serial output.
3. Add the OLED; confirm voltage, pin order, address, and that the screen draws.
4. Add the piezo and test the sidetone briefly.
5. Connect one switch at a time and verify dit, dah, then squeeze.

## Still to do

- AP-mode captive portal for WiFi setup, instead of serial only
- WPM and tone adjustment from the paddle, saved to NVS
- Straight-key and iambic A/B selectable at runtime rather than compile time
- Koch/Farnsworth trainer, random groups, callsigns, score history
- Key-out line to drive a real transmitter
