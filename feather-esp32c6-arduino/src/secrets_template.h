// Copy this file to secrets.h and fill in your values.

#ifndef SECRETS_H
#define SECRETS_H

// ===== WiFi =====
// 2.4 GHz, WPA2. The ESP32-C6 does not see 5/6 GHz networks.
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// ===== Fugleramme page =====
// The full address of /frame.e6. Turn on "External e-ink panel" and set
// "External panel size" to 13.3 in the Fugleramme admin page first; opening
// this address in a browser should then download a file.
// http:// and https:// both work. If the log says the server redirects, use
// the address it names.
#define FRAME_URL "http://birds-frame.example/frame.e6"

// ===== Home Assistant (MQTT) =====
// Your MQTT broker, usually the Mosquitto add-on on the Home Assistant host.
// Leave MQTT_HOST empty ("") to turn reporting off.
#define MQTT_HOST ""
#define MQTT_PORT 1883
#define MQTT_USER ""
#define MQTT_PASSWORD ""

#endif
