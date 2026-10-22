#include <Arduino.h>
#include <WiFi.h>
#include "secrets.h"
#include <PubSubClient.h>

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

const char *ssid = WIFI_SSID;
const char *password = WIFI_PASSWORD;
const char* MQTT_HOST = "10.0.1.46"; // Replace with server LAN IP
const int MQTT_PORT = 1883;
const char* MQTT_USER = "mqttuser";
const char* MQTT_PASSWORD = MQTT_PASSWRD;

// Rotary dial reader variables
const byte pin = 4;
int lastState = HIGH;
unsigned long changes = 0;
unsigned long started;
bool measuring = false;

// Hook switch variables (detekce zvednutí sluchátka)
const byte hookPin = 2; // GPIO pin pro hook switch (upravte dle zapojení)
bool lastHookState = false;

const char* MQTT_TOPIC = "rotarydial/dial";
const char* MQTT_HOOK_TOPIC = "rotarydial/hook";

/**
 * Zjistí, zda je telefon (sluchátko) zvednutý.
 * @return true = zvednuto (off-hook), false = položeno (on-hook).
 */
bool isPhonePickedUp() {
  // Při použití INPUT_PULLUP a spínače spojeného se zemí (GND):
  // LOW = zvednuto, HIGH = položeno.
  // Pokud má váš kontakt opačnou logiku (NO vs NC), změňte porovnání na == HIGH.
  return digitalRead(hookPin) == LOW;
}

/**
 * Přečte vytáčené číslo z rotačního číselníku (neblokující).
 * Vrací vytočené číslo (počet impulsů), nebo -1, pokud vytáčení ještě neproběhlo či neskončilo.
 */
int readNumber() {
  int state = digitalRead(pin);

  if (state != lastState) {
    delayMicroseconds(1000);

    state = digitalRead(pin);
    if (state != lastState) {
      lastState = state;
      changes++;

      if (!measuring) {
        measuring = true;
        started = millis();
      }
    }
  }

  if (measuring && millis() - started > 1500) {
    int number = changes / 2;
    changes = 0;
    measuring = false;

    // Poznámka: U většiny telefonů 10 impulsů odpovídá číslici '0'.
    // Pokud chcete tuto konverzi, můžete odkomentovat:
    // if (number == 10) number = 0;

    return number;
  }

  return -1;
}

void connectWifi() {
  Serial.println("Connecting to WiFi...");
  WiFi.setSleep(false);
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(100);
  }
  Serial.println("\nWiFi Connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
}

const char* DISCOVERY_TOPIC =
    "homeassistant/device/rotary_phone_esp/config";

bool publishDiscovery() {
  static const char discoveryConfig[] = R"rawliteral(
{
  "device": {
    "identifiers": ["rotary_phone_esp"],
    "name": "Rotary Phone",
    "manufacturer": "DIY",
    "model": "ESP Rotary Phone"
  },
  "origin": {
    "name": "Rotary Phone Firmware",
    "sw": "1.0"
  },
  "components": {
    "dial": {
      "platform": "sensor",
      "name": "Dial Number",
      "unique_id": "rotary_phone_dial",
      "default_entity_id": "sensor.rotary_phone_dial",
      "state_topic": "rotarydial/dial",
      "icon": "mdi:phone-dial"
    },
    "hook": {
      "platform": "binary_sensor",
      "name": "Phone Hook",
      "unique_id": "rotary_phone_hook",
      "default_entity_id": "binary_sensor.rotary_phone_hook",
      "state_topic": "rotarydial/hook",
      "payload_on": "picked_up",
      "payload_off": "hung_up",
      "icon": "mdi:phone-in-talk"
    }
  }
}
)rawliteral";

  bool success = mqtt.publish(
      DISCOVERY_TOPIC,
      discoveryConfig,
      true
  );

  Serial.println(
      success
          ? "MQTT Discovery konfigurace odeslana"
          : "CHYBA: MQTT Discovery konfiguraci se nepodarilo odeslat"
  );

  return success;
}
void connectMQTT() {
  while (!mqtt.connected()) {
    Serial.print("Pripojuji se k MQTT...");

    if (mqtt.connect("RotaryPhoneESP", MQTT_USER, MQTT_PASSWORD)) {
      Serial.println(" OK");

      publishDiscovery();
    } else {
      Serial.print(" chyba, state=");
      Serial.println(mqtt.state());
      delay(5000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(50);
  delay(1000);

  // Initialize rotary dial pin
  pinMode(pin, INPUT_PULLUP);
  lastState = digitalRead(pin);

  // Initialize hook switch pin
  pinMode(hookPin, INPUT_PULLUP);
  lastHookState = isPhonePickedUp();

  connectWifi();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(1024);

  Serial.println(WiFi.localIP());
  Serial.println("Listening for rotary dial pulses on GPIO 2...");
  Serial.print("Initial hook state: ");
  Serial.println(lastHookState ? "Picked up" : "Hung up");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWifi();
  }

  if (!mqtt.connected()) {
    connectMQTT();
  }
  mqtt.loop();

  // Kontrola změny stavu sluchátka
  bool pickedUp = isPhonePickedUp();
  if (pickedUp != lastHookState) {
    lastHookState = pickedUp;
    Serial.print("Stav sluchatka: ");
    Serial.println(pickedUp ? "Zvednuto" : "Polozeno");
    mqtt.publish(MQTT_HOOK_TOPIC, pickedUp ? "picked_up" : "hung_up");
  }

  // Čtení čísla z číselníku
  int number = readNumber();
  if (number != -1) {
    Serial.print("Vytocene cislo (zmeny/2): ");
    Serial.println(number);
    char message[4];
    snprintf(message, sizeof(message), "%d", number);
    mqtt.publish(MQTT_TOPIC, message);
  }
}