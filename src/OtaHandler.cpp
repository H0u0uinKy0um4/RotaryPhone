#include <Arduino.h>
#include "OtaHandler.h"
#include "secrets.h"
#include <ArduinoOTA.h>
#include "SoundHandler.h"

static volatile bool otaInProgress = false;

bool isOtaInProgress() { return otaInProgress; }

void setupOTA()
{
  ArduinoOTA.setPort(3232);
  ArduinoOTA.setHostname("RotaryPhone");

  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA
  .onStart([]() {
    otaInProgress = true;
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT
    // The serial monitor is normally closed while `pio run -t upload` runs.
    // Without this, each progress print can block ~2 s and throttle OTA to
    // roughly 1 KB per 2 s.
    Serial.setTxTimeoutMs(0);
#endif
    Serial.println("OTA: Update started");
    stopAudio();   // free CPU/network so the OTA transfer is not starved
  })
  .onEnd([]() {
    otaInProgress = false;
    Serial.println("\nOTA: Update successful!");
  })
  .onProgress([](unsigned int progress, unsigned int total) {
    static unsigned int lastPercent = 100;
    unsigned int percent = (progress * 100) / total;
    if (percent / 10 != lastPercent / 10) {
      lastPercent = percent;
      Serial.printf("OTA Progress: %u%%\n", percent);
    }
  })
  .onError([](ota_error_t error) {
      otaInProgress = false;
      Serial.printf("\nOTA Error[%u]: ", error);

      if (error == OTA_AUTH_ERROR) {
          Serial.println("Authentication failed");
      } else if (error == OTA_BEGIN_ERROR) {
          Serial.println("Could not begin update");
      } else if (error == OTA_CONNECT_ERROR) {
          Serial.println("Connection failed");
      } else if (error == OTA_RECEIVE_ERROR) {
          Serial.println("Firmware receive failed");
      } else if (error == OTA_END_ERROR) {
          Serial.println("Update finalization failed");
      } else {
          Serial.println("Unknown OTA error");
      }
    });
  ArduinoOTA.begin();
}
