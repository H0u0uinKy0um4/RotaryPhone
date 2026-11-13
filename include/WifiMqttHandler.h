#pragma once
#include <Arduino.h>

void connectWifi();
void mqttSetup();
void handleMqtt();
void mqttPublish(const char * topic, const char * message);

// User settings, controlled from Home Assistant and stored in NVS.
bool beepsEnabled();
bool tuneEnabled();
