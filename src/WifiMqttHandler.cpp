#include "WifiMqttHandler.h"
#include <PubSubClient.h>
#include <WiFi.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include "secrets.h"
#include "SoundHandler.h"

const char *wifi_ssid = WIFI_SSID;

// Combined Home Assistant device discovery payload.
static const char* DISCOVERY_TOPIC =
    "homeassistant/device/rotary_phone_esp/config";

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
    "audio": {
    "platform": "text",
    "name": "Audio URL",
    "unique_id": "rotary_phone_audio_url",
    "command_topic": "rotarydial/audio/url",
    "mode": "text",
    "max": 200,
    "icon": "mdi:music"
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
    },
    "beeps": {
      "platform": "switch",
      "name": "Digit Beeps",
      "unique_id": "rotary_phone_beeps",
      "default_entity_id": "switch.rotary_phone_digit_beeps",
      "command_topic": "rotarydial/settings/beeps/set",
      "state_topic": "rotarydial/settings/beeps",
      "payload_on": "ON",
      "payload_off": "OFF",
      "icon": "mdi:bell-ring-outline"
    },
    "tune": {
      "platform": "switch",
      "name": "Dial Tune",
      "unique_id": "rotary_phone_tune",
      "default_entity_id": "switch.rotary_phone_dial_tune",
      "command_topic": "rotarydial/settings/tune/set",
      "state_topic": "rotarydial/settings/tune",
      "payload_on": "ON",
      "payload_off": "OFF",
      "icon": "mdi:music-note"
    }
  }
}
)rawliteral";

// MQTT Media Player topics (Home Assistant native media player)
static const char* MP_DISCOVERY_TOPIC = "homeassistant/media_player/rotary_phone/config";
static const char* MP_COMMAND_TOPIC = "rotary_phone/media/cmd";
static const char* MP_PLAY_TOPIC = "rotary_phone/media/play";
static const char* MP_STATE_TOPIC = "rotary_phone/media/state";

static const char mpDiscoveryConfig[] = R"rawliteral(
{
  "name": "Rotary Phone Speaker",
  "unique_id": "rotary_phone_speaker_01",
  "cmd_t": "rotary_phone/media/cmd",
  "command_playmedia_topic": "rotary_phone/media/play",
  "stat_t": "rotary_phone/media/state",
  "device": {
    "identifiers": ["rotary_phone_esp"],
    "name": "Rotary Phone",
    "manufacturer": "DIY"
  }
}
)rawliteral";

// Topics for Hook and Dial discovery
static const char* HOOK_DISCOVERY_TOPIC = "homeassistant/binary_sensor/rotary_phone/hook/config";
static const char* DIAL_DISCOVERY_TOPIC = "homeassistant/sensor/rotary_phone/dial/config";

// Payload for the Hook (Binary Sensor)
static const char hookDiscoveryConfig[] = R"rawliteral(
{
  "name": "Hook",
  "unique_id": "rotary_phone_hook_01",
  "stat_t": "rotarydial/hook",
  "pl_off": "hung_up",
  "pl_on": "picked_up",
  "dev_cla": "door",
  "device": {
    "identifiers": ["rotary_phone_esp"],
    "name": "Rotary Phone",
    "manufacturer": "DIY"
  }
}
)rawliteral";

// Payload for the Dial (Sensor)
static const char dialDiscoveryConfig[] = R"rawliteral(
{
  "name": "Last Dialed Number",
  "unique_id": "rotary_phone_dial_01",
  "stat_t": "rotarydial/dial",
  "icon": "mdi:phone-dial",
  "device": {
    "identifiers": ["rotary_phone_esp"],
    "name": "Rotary Phone",
    "manufacturer": "DIY"
  }
}
)rawliteral";

const char *wifi_password = WIFI_PASSWORD;
const char *mqtt_password = MQTT_PASSWORD;
const char *mqtt_host = MQTT_HOST;
const char *mqtt_user =MQTT_USER;
const uint16_t mqtt_port =1883;
const char* const MQTT_CLIENT_ID = "RotaryPhone-esp32";

// Sound settings, mirrored to Home Assistant as switches.
static const char* BEEPS_SET_TOPIC = "rotarydial/settings/beeps/set";
static const char* BEEPS_STATE_TOPIC = "rotarydial/settings/beeps";
static const char* TUNE_SET_TOPIC = "rotarydial/settings/tune/set";
static const char* TUNE_STATE_TOPIC = "rotarydial/settings/tune";
static const char* VOLUME_SET_TOPIC = "rotarydial/settings/volume/set";
static const char* VOLUME_STATE_TOPIC = "rotarydial/settings/volume";

static Preferences settings;
static bool beepsOn = true;
static bool tuneOn = true;
static int volumePercent = 100;

bool beepsEnabled() { return beepsOn; }
bool tuneEnabled() { return tuneOn; }
int volumeLevel() { return volumePercent; }

static void loadSettings() {
  settings.begin("rotaryphone", false);
  beepsOn = settings.getBool("beeps", true);
  tuneOn = settings.getBool("tune", true);
  volumePercent = settings.getInt("volume", 100);
  if (volumePercent < 0) volumePercent = 0;
  if (volumePercent > 100) volumePercent = 100;
  Serial.printf("Settings: beeps=%d tune=%d volume=%d\n", beepsOn, tuneOn, volumePercent);
}

static void storeSettings() {
  settings.putBool("beeps", beepsOn);
  settings.putBool("tune", tuneOn);
  settings.putInt("volume", volumePercent);
}

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

static void publishSettings() {
  mqtt.publish(BEEPS_STATE_TOPIC, beepsOn ? "ON" : "OFF", true);
  mqtt.publish(TUNE_STATE_TOPIC, tuneOn ? "ON" : "OFF", true);
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", volumePercent);
  mqtt.publish(VOLUME_STATE_TOPIC, buf, true);
}

// Accepts ON/1/TRUE (any case); everything else is treated as off.
static bool payloadIsOn(const byte* payload, unsigned int length) {
  char buf[8];
  if (length >= sizeof(buf)) return false;
  for (unsigned int i = 0; i < length; i++) {
    char c = (char)payload[i];
    if (c >= 'a' && c <= 'z') c -= 32;
    buf[i] = c;
  }
  buf[length] = '\0';
  return strcmp(buf, "ON") == 0 || strcmp(buf, "1") == 0 || strcmp(buf, "TRUE") == 0;
}

void connectWifi() {
  Serial.println("Connecting to WiFi...");
  WiFi.setSleep(false);
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  WiFi.begin(wifi_ssid, wifi_password);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(100);
  }
  Serial.println("\nWiFi Connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
  // Force modem sleep off again after association (some C3 builds ignore the
  // pre-connect setting).
  esp_wifi_set_ps(WIFI_PS_NONE);
  WiFi.setSleep(false);
  Serial.print("WiFi RSSI: ");
  Serial.println(WiFi.RSSI());
}



// Extract a quoted JSON string value for `key` (key must include its quotes).
static bool extractJsonString(const char* json, const char* key, char* out, size_t outSize) {
    const char* p = strstr(json, key);
    if (p == nullptr) return false;

    p += strlen(key);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != ':') return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return false;
    p++;

    size_t i = 0;
    while (*p && *p != '"' && i + 1 < outSize) {
        if (*p == '\\' && p[1]) p++;   // keep the escaped character literally
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i > 0;
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    if (strcmp(topic, BEEPS_SET_TOPIC) == 0) {
        beepsOn = payloadIsOn(payload, length);
        storeSettings();
        publishSettings();
        Serial.printf("Digit beeps %s\n", beepsOn ? "on" : "off");
        return;
    }

    if (strcmp(topic, TUNE_SET_TOPIC) == 0) {
        tuneOn = payloadIsOn(payload, length);
        storeSettings();
        publishSettings();
        Serial.printf("Dial tune %s\n", tuneOn ? "on" : "off");
        return;
    }

    if (strcmp(topic, VOLUME_SET_TOPIC) == 0) {
        char buf[8];
        unsigned int copyLen = length;
        if (copyLen >= sizeof(buf)) copyLen = sizeof(buf) - 1;
        memcpy(buf, payload, copyLen);
        buf[copyLen] = '\0';
        int v = atoi(buf);
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        volumePercent = v;
        storeSettings();
        setAudioGain(volumePercent / 100.0f);
        publishSettings();
        Serial.printf("Volume set to %d%%\n", volumePercent);
        return;
    }

    if (strcmp(topic, "rotarydial/audio/url") == 0) {
        char url[201];

        if (length == 0 || length >= sizeof(url)) {
            Serial.println("Audio URL length invalid");
            return;
        }

        memcpy(url, payload, length);
        url[length] = '\0';

        queueAudioUrl(url);
    }

    // Home Assistant native media player: play media (URL provided by HA TTS)
    if (strcmp(topic, MP_PLAY_TOPIC) == 0) {
        // Copy payload into a null-terminated buffer.
        static char playPayload[512];
        unsigned int copyLen = length;
        if (copyLen >= sizeof(playPayload)) copyLen = sizeof(playPayload) - 1;
        memcpy(playPayload, payload, copyLen);
        playPayload[copyLen] = '\0';

        // Home Assistant sends the track URL as "media_id" (an older variant
        // used "media_content_id"); accept both, plus a plain "url" key.
        char url[201] = "";
        bool haveUrl = extractJsonString(playPayload, "\"media_id\"", url, sizeof(url)) ||
                       extractJsonString(playPayload, "\"media_content_id\"", url, sizeof(url)) ||
                       extractJsonString(playPayload, "\"url\"", url, sizeof(url));

        // Some senders publish the bare URL as the whole payload.
        if (!haveUrl && (strncmp(playPayload, "http://", 7) == 0 ||
                         strncmp(playPayload, "https://", 8) == 0)) {
            strlcpy(url, playPayload, sizeof(url));
            haveUrl = true;
        }

        if (haveUrl) {
            Serial.print("MP play URL: ");
            Serial.println(url);
            queueAudioUrl(url);
        } else {
            Serial.println("MP play: no URL found in payload");
        }
    }

    // Home Assistant native media player: commands
    if (strcmp(topic, MP_COMMAND_TOPIC) == 0) {
        static char cmdBuf[64];
        unsigned int copyLen = length;
        if (copyLen >= sizeof(cmdBuf)) copyLen = sizeof(cmdBuf) - 1;
        memcpy(cmdBuf, payload, copyLen);
        cmdBuf[copyLen] = '\0';

        // Normalize to uppercase for comparison.
        for (unsigned int i = 0; cmdBuf[i]; i++) {
            if (cmdBuf[i] >= 'a' && cmdBuf[i] <= 'z') cmdBuf[i] -= 32;
        }

        if (strcmp(cmdBuf, "STOP") == 0) {
            stopAudio();
        } else if (strcmp(cmdBuf, "PAUSE") == 0) {
            stopAudio();
        } else {
            Serial.print("MP command: ");
            Serial.println(cmdBuf);
        }
    }
}


void mqttSetup()
{
  loadSettings();
  setAudioGain(volumePercent / 100.0f);
  mqtt.setServer(mqtt_host, mqtt_port);
  // The combined device discovery payload is larger than 1 KB.
  mqtt.setBufferSize(2048);
  mqtt.setCallback(mqttCallback);
}


static bool publishDiscovery() {
  bool ok = mqtt.publish(DISCOVERY_TOPIC, discoveryConfig, true);
  
  // Publish the Media Player, Hook, and Dial
  ok = mqtt.publish(MP_DISCOVERY_TOPIC, mpDiscoveryConfig, true) && ok;
  ok = mqtt.publish(HOOK_DISCOVERY_TOPIC, hookDiscoveryConfig, true) && ok;
  ok = mqtt.publish(DIAL_DISCOVERY_TOPIC, dialDiscoveryConfig, true) && ok;
  
  return ok;
}

void handleMqtt() {
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }

    if (!mqtt.connected()) {
        static unsigned long lastAttempt = 0;

        if (millis() - lastAttempt < 5000) {
            return;
        }
        lastAttempt = millis();

        Serial.printf("Connecting to MQTT broker %s...\n", mqtt_host);

        if (mqtt.connect(MQTT_CLIENT_ID, mqtt_user, mqtt_password,
            "RotaryPhoneESP", 0, true, "offline"))
        {
            Serial.println("MQTT connected");

            if (mqtt.subscribe("rotarydial/audio/url")) {
                Serial.println("Subscribed to rotarydial/audio/url");
            } else {
                Serial.println("Audio URL subscription failed");
            }

            if (mqtt.subscribe(MP_COMMAND_TOPIC)) {
                Serial.println("Subscribed to MP command topic");
            }
            if (mqtt.subscribe(MP_PLAY_TOPIC)) {
                Serial.println("Subscribed to MP play topic");
            }
            if (mqtt.subscribe(BEEPS_SET_TOPIC) && mqtt.subscribe(TUNE_SET_TOPIC) && mqtt.subscribe(VOLUME_SET_TOPIC)) {
                Serial.println("Subscribed to settings topics");
            }

            publishDiscovery();
            publishSettings();
            mqtt.publish(MP_STATE_TOPIC, isAudioPlaying() ? "playing" : "idle", true);
        }
        else {
            Serial.printf("MQTT connection failed: %d\n",mqtt.state());
        }
    }
    else {
        mqtt.loop();

        // Publish media player state changes.
        static bool lastPlaying = false;
        static bool stateInitialized = false;
        bool playing = isAudioPlaying();
        if (!stateInitialized || playing != lastPlaying) {
            mqtt.publish(MP_STATE_TOPIC, playing ? "playing" : "idle", true);
            lastPlaying = playing;
            stateInitialized = true;
        }
    }
}

void mqttPublish(const char * topic, const char * message)
{
  mqtt.publish(topic, message);
}
