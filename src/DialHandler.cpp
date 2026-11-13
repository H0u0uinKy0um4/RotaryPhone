#include "DialHandler.h"
#include <cstring>
#include "SoundHandler.h"



static constexpr unsigned long HOOK_DEBOUNCE_MS = 30;
static constexpr size_t DIAL_BUFFER_SIZE = 64;

// Rotary dial pulse reader state.
static int lastState = HIGH;
static unsigned long changes = 0;
static unsigned long started = 0;
static bool measuring = false;

// Accumulated dialed number (whole number, e.g. "666123409").
static char dialedBuffer[DIAL_BUFFER_SIZE] = "";
static unsigned long lastDigitTime = 0;
static bool hasPendingNumber = false;
static bool numberSent = false;

// Hook switch state, debounced.
static bool lastHookState = false;
static bool rawHookState = false;
static unsigned long rawHookChangedAt = 0;

bool isPhonePickedUp() {
  // With INPUT_PULLUP and a switch to GND: LOW = picked up, HIGH = hung up.
  return digitalRead(PIN_HOOK) == LOW;
}

void IRAM_ATTR hookDownISR() {
  // Silence audio immediately; the main loop will clean up the decoder.
  audioMuteImmediately();
}

void setupDial() {
  pinMode(PIN_DIAL, INPUT_PULLUP);
  lastState = digitalRead(PIN_DIAL);

  pinMode(PIN_HOOK, INPUT_PULLUP);
  // Hook down (hang up) is a rising edge: INPUT_PULLUP, switch to GND,
  // so picked up = LOW, hung up = HIGH.
  attachInterrupt(digitalPinToInterrupt(PIN_HOOK), hookDownISR, RISING);
  lastHookState = isPhonePickedUp();
  rawHookState = lastHookState;
  rawHookChangedAt = millis();
}

bool pollHookChange(bool& pickedUp) {
  bool pickedUpRaw = isPhonePickedUp();
  if (pickedUpRaw != rawHookState) {
    rawHookState = pickedUpRaw;
    rawHookChangedAt = millis();
  }
  if (rawHookState != lastHookState &&
      millis() - rawHookChangedAt >= HOOK_DEBOUNCE_MS) {
    lastHookState = rawHookState;
    pickedUp = lastHookState;
    return true;
  }
  return false;
}

int pollDialDigit() {
  int state = digitalRead(PIN_DIAL);

  if (state != lastState) {
    delayMicroseconds(1000);

    state = digitalRead(PIN_DIAL);
    if (state != lastState) {
      lastState = state;
      changes++;

      // The timer restarts on every pulse, so the digit is evaluated 1500 ms
      // after the LAST pulse (when the dial has returned to rest).
      measuring = true;
      started = millis();
    }
  }

  if (measuring && millis() - started > DIAL_DEBOUNCE) {
    int number = changes / 2;
    changes = 0;
    measuring = false;

    // 10 pulses correspond to the digit '0'.
    if (number == 10) number = 0;

    return number;
  }

  return -1;
}

void dialReset() {
  dialedBuffer[0] = '\0';
  hasPendingNumber = false;
  numberSent = false;
  // Discard any in-progress rotary pulse measurement so a hung-up or
  // cancelled dial cannot leak into the next call.
  changes = 0;
  measuring = false;
  started = 0;
  lastState = digitalRead(PIN_DIAL);
}

void dialAppend(int digit) {
  size_t len = strlen(dialedBuffer);
  if (len + 1 < sizeof(dialedBuffer)) {
    dialedBuffer[len] = (char)('0' + (digit % 10));
    dialedBuffer[len + 1] = '\0';
  }
  lastDigitTime = millis();
  hasPendingNumber = true;
}

bool dialNumberReady(unsigned long timeoutMs) {
  return hasPendingNumber && !numberSent && dialedBuffer[0] != '\0' &&
         (millis() - lastDigitTime > timeoutMs);
}

bool dialNumberSent() { return numberSent; }

void dialMarkNumberSent() { numberSent = true; }

const char* dialNumber() { return dialedBuffer; }
