#pragma once
#include <Arduino.h>

#define PIN_HOOK  7
#define PIN_DIAL  21

#define DIAL_DEBOUNCE 300


// Configure the hook/Dial GPIOs and their initial state.
void setupDial();

// Raw hook position: true = picked up (off-hook).
bool isPhonePickedUp();

// Non-blocking hook debounce. Returns true on a debounced change and stores
// the new state in `pickedUp`.
bool pollHookChange(bool& pickedUp);

// Non-blocking rotary dial reader. Returns a digit 0-9, or -1 while idle.
int pollDialDigit();

// Clear the accumulated number and its pending/sent flags.
void dialReset();

// Append one digit to the accumulated number.
void dialAppend(int digit);

// True once a number is fully dialed and the inter-digit timeout has elapsed.
bool dialNumberReady(unsigned long timeoutMs);

// True after the accumulated number has been published.
bool dialNumberSent();
void dialMarkNumberSent();

// Accumulated number (empty string when nothing has been dialed).
const char* dialNumber();
