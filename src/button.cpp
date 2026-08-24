#include "button.h"
#include "Arduino.h"

#define DEBOUNCE_MS   30
#define TAPS_REQUIRED 5
#define TAP_WINDOW_MS 2000

static uint8_t s_pin = 0;

static volatile uint32_t s_lastChangeTime = 0;
static volatile uint8_t  s_tapCount       = 0;
static volatile uint32_t s_lastTapTime    = 0;
static volatile bool     s_pendingTrigger = false;

// Momentary mode: pin idles LOW, goes HIGH only while touched. Count the
// press (RISING) only — the release (FALLING) isn't a separate tap.
static void IRAM_ATTR onButtonRising() {
  uint32_t now = millis();

  if (now - s_lastChangeTime < DEBOUNCE_MS) {
    return;
  }
  s_lastChangeTime = now;

  if (now - s_lastTapTime > TAP_WINDOW_MS) {
    s_tapCount = 0;
  }

  s_tapCount++;
  s_lastTapTime = now;

  if (s_tapCount >= TAPS_REQUIRED) {
    s_tapCount = 0;
    s_pendingTrigger = true;
  }
}

void initButton(uint8_t pin) {
  s_pin = pin;
  pinMode(s_pin, INPUT);
  attachInterrupt(digitalPinToInterrupt(s_pin), onButtonRising, RISING);
  Serial.println("[button] Interrupt attached (RISING, momentary mode)");
}

bool buttonActivated() {
  if (!s_pendingTrigger) {
    return false;
  }
  noInterrupts();
  s_pendingTrigger = false;
  interrupts();
  return true;
}

void disableButtonInterrupt() {
  detachInterrupt(digitalPinToInterrupt(s_pin));
  Serial.println("[button] Interrupt detached — pin free for sleep config");
}