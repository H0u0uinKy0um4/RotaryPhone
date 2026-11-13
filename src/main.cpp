#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <esp_sleep.h>
#include "DialHandler.h"
#include "WifiMqttHandler.h"
#include "OtaHandler.h"
#include "SoundHandler.h"

const char* MQTT_DIAL_TOPIC = "rotarydial/dial";
const char* MQTT_HOOK_TOPIC = "rotarydial/hook";

// ---------------------------------------------------------------------------
// Dial timing - the only place these delays are set. All values in ms.
//
//   dial pulse --> TIME1 --> pip         feedback that the digit registered
//   last pulse --> TIME2 --> number sent  must stay above DIAL_DEBOUNCE
//   number sent --> TIME3 --> chime       chime repeats while waiting for the
//                                         announcement from Home Assistant
//   chime --> TIME4                      gives up if nothing arrives by then
// ---------------------------------------------------------------------------
static constexpr unsigned long TIME1_DIGIT_BEEP_MS = 0;
static constexpr unsigned long TIME2_SEND_MS = 3200;
static constexpr unsigned long TIME3_TUNE_MS = 0;
static constexpr unsigned long TIME4_TUNE_HOLD_MS = 8000;

// Pending cues and the moment they are due.
static bool beepScheduled = false;
static unsigned long beepAtMs = 0;
static bool tuneScheduled = false;
static unsigned long tuneAtMs = 0;

// Set to 1 to put the phone into deep sleep as soon as the hook is put down.
// The audio playback is always stopped, independently of this switch.
#define SHUTDOWN_ON_HANGUP 0

void setup() {
  Serial.begin(115200);
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
  // ESP32-C3 USB-CDC blocks up to ~2 s per write when no host is reading
  // (20 x 100 ms retries in HWCDC::write). That stalls OTA uploads and the
  // main loop, so make serial output non-blocking. When the monitor is
  // connected it drains normally; when it is closed, logs are dropped.
  Serial.setTxTimeoutMs(0);
#endif
  Serial.setTimeout(50);
  delay(1000);

  setupDial();

  connectWifi();
  mqttSetup();
  setupOTA();
  setupSound();

  Serial.println(WiFi.localIP());
  Serial.println("Listening for rotary dial pulses on GPIO 2...");
  Serial.print("Initial hook state: ");
  Serial.println(isPhonePickedUp() ? "Picked up" : "Hung up");
}

/**
 * Stops playback and (optionally) puts the phone into deep sleep.
 */
void shutdownPhone() {
  beepScheduled = false;
  tuneScheduled = false;
  stopAudio();

  // Hanging up: discard the number instead of sending it.
  dialReset();

  Serial.println("Telefon zavěšen - zastavuji prehravani");

#if SHUTDOWN_ON_HANGUP
  // Give the hook state message time to go out before sleeping.
  delay(1000);
  Serial.println("Prechod do deep sleep");
  esp_deep_sleep_start();
#endif
}

void loop() {
  ArduinoOTA.handle();

  // During OTA, do not run MQTT/audio/dial code: a blocking MQTT reconnect
  // or HTTP read can stall the OTA transfer.
  if (isOtaInProgress()) {
    return;
  }

  handleMqtt();
  handleAudio();

  bool pickedUp = false;
  if (pollHookChange(pickedUp)) {
    Serial.print("Stav sluchatka: ");
    Serial.println(pickedUp ? "Zvednuto" : "Polozeno");
    mqttPublish(MQTT_HOOK_TOPIC, pickedUp ? "picked_up" : "hung_up");

    if (pickedUp) {
      // New call: reset the accumulated number and allow audio again.
      dialReset();
      audioResetMute();
    } else {
      // Hang-up: discard the number - it is only sent after TIME2.
      // Do not publish an empty dial payload: HA would treat it as "0"
      // and run the default TTS action on every hang-up.
      dialReset();
      shutdownPhone();
    }
  }

  int number = pollDialDigit();
  if (number != -1) {
    Serial.print("Vytocene cislo (zmeny/2): ");
    Serial.println(number);

    // If the previous number was already sent, start a new one.
    if (dialNumberSent()) dialReset();

    dialAppend(number);
    if (beepsEnabled()) {
      beepScheduled = true;
      beepAtMs = millis() + TIME1_DIGIT_BEEP_MS;
    }
  }

  // Digit feedback pip, after TIME1.
  if (beepScheduled && (int32_t)(millis() - beepAtMs) >= 0) {
    beepScheduled = false;
    playDigitBeep();
  }

  // TIME2 without a new digit means the number is complete; only send while
  // the phone is still picked up.
  if (dialNumberReady(TIME2_SEND_MS)) {
    mqttPublish(MQTT_DIAL_TOPIC, dialNumber());
    dialMarkNumberSent();
    if (tuneEnabled()) {
      tuneScheduled = true;
      tuneAtMs = millis() + TIME3_TUNE_MS;
    }
    Serial.print("Odeslano cele cislo (timeout): ");
    Serial.println(dialNumber());
  }

  // Confirmation chime, after TIME3. If the announcement already started, the
  // chime is pointless, so skip it.
  if (tuneScheduled && (int32_t)(millis() - tuneAtMs) >= 0) {
    tuneScheduled = false;
    if (!isAudioPlaying()) playConfirmationTune(TIME4_TUNE_HOLD_MS);
  }
}
