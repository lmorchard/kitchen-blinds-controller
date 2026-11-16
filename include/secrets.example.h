// Copy to include/secrets.h and fill in. include/secrets.h is git-ignored.
#pragma once

#define WIFI_SSID      "your-wifi-ssid"
#define WIFI_PASSWORD  "your-wifi-password"

#define MQTT_HOST      "192.168.1.10"
#define MQTT_PORT      1883
#define MQTT_USER      "mqtt-user"
#define MQTT_PASS      "mqtt-password"

// Must match ota.password in secrets.ini (used by `pio run -e firmware_ota`)
#define OTA_PASSWORD   "change-me"
