// Pajoniiir serial-MIDI test controller — Adafruit Circuit Playground Classic
// (ATmega32U4 @ 8 MHz). Bring-up rig for the ESP32-P4 `midi_uart_link`
// component (bidirectional), using the board's built-in buttons and NeoPixels.
//
//   Left button  (D4)  -> Deck 1 CUE/PFL   (FLX4 Note On, channel 1, note 0x54)
//   Right button (D19)  -> Deck 2 CUE/PFL   (FLX4 Note On, channel 2, note 0x54)
//   NeoPixels 0-4 / 5-9 <- driven by the P4: it sends 9n 54 7F / 9n 54 00
//                          whenever a deck's PFL state changes.
//
// The P4 runs the built-in DDJ-FLX4 map + LED encoder for the UART link, so we
// speak FLX4-shaped notes both ways (no SD controller profile needed):
//   TX press:   9n 54 7F      release: 9n 54 00     (n = 0 -> D1, 1 -> D2)
//   RX:         9n 54 vv      -> half-ring lit for (vv != 0)
//
// Raw serial MIDI on Serial1 (hardware USART, pads #0/#1), independent of the
// USB CDC `Serial`. 19200 8N1: an 8 MHz 32U4 hits this with ~0.16% baud error.
//
// Board buttons are ACTIVE-HIGH on the Circuit Playground (onboard pulldowns) —
// pinMode(INPUT), pressed == HIGH.
//
// Library: only "Adafruit NeoPixel" (NeoPixels need bit-banged timing). The
// heavier Adafruit_CircuitPlayground lib is not required.
//
// Wiring (3.3 V logic both sides):
//   CP Classic  pad #1 / TX  ->  P4 UART RX  GPIO28  (JP1 pin 19)
//   CP Classic  pad #0 / RX  <-  P4 UART TX  GPIO29  (JP1 pin 12)
//   CP Classic  GND          <-> P4 GND             (JP1 pin 3/4/14)
// Power the CP over its own USB; share only GND + the two MIDI lines with the
// P4. (Feeding it from JP1 VCC3V3 is borderline once the pixels light.)

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>

// ── Circuit Playground Classic pin map ──────────────────────────────────────
static const uint8_t PIN_BTN_A     = 4;    // left button  -> Deck 1
static const uint8_t PIN_BTN_B     = 19;   // right button -> Deck 2
static const uint8_t PIN_NEOPIXEL  = 17;   // 10-pixel ring
static const uint8_t NEOPIXEL_COUNT = 10;
static const uint8_t NEOPIXEL_BRIGHTNESS = 40;

static Adafruit_NeoPixel strip(NEOPIXEL_COUNT, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);

// ── MIDI ────────────────────────────────────────────────────────────────────
static const uint32_t MIDI_BAUD = 19200;
static const uint8_t  NOTE_PFL  = 0x54;   // FLX4 PFL / headphone-cue button
static const uint8_t  STATUS_D1 = 0x90;   // Note On, channel 1
static const uint8_t  STATUS_D2 = 0x91;   // Note On, channel 2

static const uint16_t DEBOUNCE_MS = 25;

// ── Buttons (TX) ────────────────────────────────────────────────────────────
struct Button {
  uint8_t  pin;
  uint8_t  status;       // MIDI status byte for this deck
  bool     lastRaw;
  bool     stable;
  uint32_t lastChange;
};

static Button btnA = { PIN_BTN_A, STATUS_D1, false, false, 0 };
static Button btnB = { PIN_BTN_B, STATUS_D2, false, false, 0 };

// ── LED state (RX) ──────────────────────────────────────────────────────────
static bool d1PflOn = false;
static bool d2PflOn = false;

static void sendNote(uint8_t status, uint8_t note, uint8_t velocity) {
  Serial1.write(status);
  Serial1.write(note);
  Serial1.write(velocity);
}

static void showPixels() {
  uint32_t d1Col = d1PflOn ? strip.Color(0, 255, 0) : 0;
  uint32_t d2Col = d2PflOn ? strip.Color(0, 0, 255) : 0;
  for (uint8_t i = 0; i < 5; i++)  strip.setPixelColor(i, d1Col);
  for (uint8_t i = 5; i < 10; i++) strip.setPixelColor(i, d2Col);
  strip.show();
}

static void serviceButton(Button& b) {
  bool raw = (digitalRead(b.pin) == HIGH);   // active-high on Circuit Playground
  uint32_t now = millis();

  if (raw != b.lastRaw) {
    b.lastRaw = raw;
    b.lastChange = now;
  }
  if ((now - b.lastChange) < DEBOUNCE_MS || raw == b.stable) {
    return;
  }
  b.stable = raw;
  sendNote(b.status, NOTE_PFL, raw ? 0x7F : 0x00);
}

// ── Incoming MIDI (RX) — minimal running-status parser ───────────────────────
static uint8_t rxStatus = 0;
static uint8_t rxData[2];
static uint8_t rxCount = 0;

static void applyMessage(uint8_t status, uint8_t d1, uint8_t d2) {
  if ((status & 0xF0) != 0x90 || d1 != NOTE_PFL) {
    return;                                 // only PFL notes drive our pixels
  }
  uint8_t ch = status & 0x0F;
  bool on = (d2 != 0);
  if (ch == 0) d1PflOn = on;
  if (ch == 1) d2PflOn = on;
  showPixels();
}

static void pollSerialMidi() {
  while (Serial1.available() > 0) {
    uint8_t byte = (uint8_t)Serial1.read();

    if (byte >= 0xF8) {
      continue;                             // system real-time: ignore
    }
    if (byte & 0x80) {                      // status byte
      rxStatus = (byte >= 0xF0) ? 0 : byte; // drop context on sysex/common
      rxCount = 0;
      continue;
    }
    if (rxStatus == 0) {
      continue;                             // orphan data
    }
    rxData[rxCount++] = byte;
    if (rxCount >= 2) {                     // all notes we care about are 2-byte
      rxCount = 0;
      applyMessage(rxStatus, rxData[0], rxData[1]);
    }
  }
}

void setup() {
  pinMode(PIN_BTN_A, INPUT);
  pinMode(PIN_BTN_B, INPUT);

  strip.begin();
  strip.setBrightness(NEOPIXEL_BRIGHTNESS);
  strip.clear();
  strip.show();

  Serial1.begin(MIDI_BAUD);   // 8N1 default framing
}

void loop() {
  serviceButton(btnA);
  serviceButton(btnB);
  pollSerialMidi();
  delay(2);
}
