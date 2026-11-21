#include "ha_mqtt.h"

#include <Arduino.h>
#include <ArduinoLog.h>
#include <PubSubClient.h>
#include <WiFi.h>

#include "config.h"
#include "secrets.h"
#include "setup_wifi.h"

#ifndef MQTT_HOST
#define MQTT_HOST ""
#endif
#ifndef MQTT_PORT
#define MQTT_PORT 1883
#endif
#ifndef MQTT_USER
#define MQTT_USER ""
#endif
#ifndef MQTT_PASSWORD
#define MQTT_PASSWORD ""
#endif

static const char *NODE = "fugleramme_frame";        // discovery object id
static const char *STATE_TOPIC = "fugleramme-frame/state";
static const char *CMD_TOPIC = "fugleramme-frame/cmd";
static const char *DISCOVERY_PREFIX = "homeassistant";

/* How long to listen for a retained command after subscribing. The broker
 * sends it straight away; this only covers a slow link. */
static const uint32_t COMMAND_WAIT_MS = 800;

static WiFiClient net;
static PubSubClient mqtt(net);
static bool redraw_requested = false;

bool ha_enabled()
{
  return MQTT_HOST[0] != '\0';
}

static void on_message(char *topic, uint8_t *payload, unsigned int len)
{
  if (strcmp(topic, CMD_TOPIC) != 0 || len == 0)
    return;
  char cmd[24];
  size_t n = len < sizeof(cmd) - 1 ? len : sizeof(cmd) - 1;
  memcpy(cmd, payload, n);
  cmd[n] = '\0';
  Log.notice("Home Assistant command: %s" CR, cmd);
  if (strcmp(cmd, "redraw") == 0)
    redraw_requested = true;
}

static bool connect_mqtt()
{
  if (mqtt.connected())
    return true;
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(1024);
  mqtt.setSocketTimeout(5);
  mqtt.setKeepAlive(30);
  mqtt.setCallback(on_message);

  const char *user = MQTT_USER[0] ? MQTT_USER : nullptr;
  const char *pass = MQTT_PASSWORD[0] ? MQTT_PASSWORD : nullptr;
  if (!mqtt.connect("fugleramme-frame", user, pass))
  {
    Log.warning("MQTT: cannot connect to %s:%d (state %d)" CR, MQTT_HOST, MQTT_PORT,
                mqtt.state());
    return false;
  }
  return true;
}

/* ---- Discovery --------------------------------------------------------- */

static const char *DEVICE_JSON =
    "\"dev\":{\"ids\":[\"fugleramme_frame\"],\"name\":\"Fugleramme frame\","
    "\"mf\":\"DIY\",\"mdl\":\"Feather ESP32-C6 + Inky Impression 13.3\","
    "\"sw\":\"1.1\"}";

/* One sensor reading `key` from the state JSON. `extra` is more JSON fields,
 * starting with a comma, or "". */
static void discover_sensor(const char *key, const char *name, const char *extra)
{
  char topic[96];
  snprintf(topic, sizeof(topic), "%s/sensor/%s/%s/config", DISCOVERY_PREFIX, NODE, key);
  char payload[640];
  snprintf(payload, sizeof(payload),
           "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"stat_t\":\"%s\","
           "\"val_tpl\":\"{{ value_json.%s }}\"%s,%s}",
           name, NODE, key, STATE_TOPIC, key, extra, DEVICE_JSON);
  if (!mqtt.publish(topic, payload, true))
    Log.warning("MQTT: discovery for %s did not send" CR, key);
}

static void publish_discovery()
{
  discover_sensor("battery", "Battery",
                  ",\"dev_cla\":\"battery\",\"unit_of_meas\":\"%\",\"stat_cla\":\"measurement\"");
  discover_sensor("voltage", "Battery voltage",
                  ",\"dev_cla\":\"voltage\",\"unit_of_meas\":\"V\",\"stat_cla\":\"measurement\","
                  "\"ent_cat\":\"diagnostic\"");
  discover_sensor("status", "Status", ",\"ic\":\"mdi:bird\"");
  discover_sensor("error", "Last error", ",\"ic\":\"mdi:alert-circle-outline\","
                                         "\"ent_cat\":\"diagnostic\"");
  discover_sensor("reset", "Last reset reason", ",\"ic\":\"mdi:restart\","
                                                "\"ent_cat\":\"diagnostic\"");
  discover_sensor("ssid", "WiFi network", ",\"ic\":\"mdi:wifi\",\"ent_cat\":\"diagnostic\"");
  discover_sensor("rssi", "WiFi signal",
                  ",\"dev_cla\":\"signal_strength\",\"unit_of_meas\":\"dBm\","
                  "\"stat_cla\":\"measurement\",\"ent_cat\":\"diagnostic\"");
  discover_sensor("ip", "IP address", ",\"ic\":\"mdi:ip-network\",\"ent_cat\":\"diagnostic\"");
  discover_sensor("last_refresh", "Last refresh", ",\"dev_cla\":\"timestamp\"");
  discover_sensor("last_check", "Last check",
                  ",\"dev_cla\":\"timestamp\",\"ent_cat\":\"diagnostic\"");
  discover_sensor("next_check", "Next check", ",\"dev_cla\":\"timestamp\"");
  discover_sensor("failures", "Failures in a row",
                  ",\"ic\":\"mdi:counter\",\"stat_cla\":\"measurement\","
                  "\"ent_cat\":\"diagnostic\"");

  char topic[96];
  snprintf(topic, sizeof(topic), "%s/button/%s/redraw/config", DISCOVERY_PREFIX, NODE);
  char payload[512];
  snprintf(payload, sizeof(payload),
           "{\"name\":\"Redraw on next wake\",\"uniq_id\":\"%s_redraw\",\"cmd_t\":\"%s\","
           "\"pl_prs\":\"redraw\",\"ret\":true,\"ic\":\"mdi:image-refresh\",%s}",
           NODE, CMD_TOPIC, DEVICE_JSON);
  if (!mqtt.publish(topic, payload, true))
    Log.warning("MQTT: discovery for the button did not send" CR);
}

/* ---- Wake ------------------------------------------------------------- */

bool ha_begin()
{
  if (!ha_enabled() || !WiFi.isConnected())
    return false;
  if (!connect_mqtt())
    return false;

  publish_discovery();

  redraw_requested = false;
  mqtt.subscribe(CMD_TOPIC);
  const uint32_t t0 = millis();
  while (millis() - t0 < COMMAND_WAIT_MS)
  {
    mqtt.loop();
    if (redraw_requested)
      break;
    delay(20);
  }
  mqtt.unsubscribe(CMD_TOPIC);

  if (redraw_requested)
  {
    /* Clear the retained command so it runs once. */
    mqtt.publish(CMD_TOPIC, (const uint8_t *)"", 0, true);
    mqtt.loop();
  }

  /* Close now: a refresh turns WiFi off, and a socket left open across that
   * is dead by the time the state is published. ha_publish() reconnects. */
  mqtt.disconnect();
  delay(50);
  return redraw_requested;
}

static void iso_utc(time_t t, char *out, size_t len)
{
  if (t <= 0)
  {
    snprintf(out, len, "null");
    return;
  }
  struct tm utc;
  gmtime_r(&t, &utc);
  strftime(out, len, "\"%Y-%m-%dT%H:%M:%S+00:00\"", &utc);
}

/* Copy `in` into `out` as the inside of a JSON string. */
static void json_escape(const char *in, char *out, size_t len)
{
  size_t o = 0;
  for (; *in && o + 2 < len; in++)
  {
    char c = *in;
    if (c == '"' || c == '\\')
    {
      out[o++] = '\\';
      out[o++] = c;
    }
    else if (c == '\n' || c == '\r')
      out[o++] = ' ';
    else if ((unsigned char)c >= 0x20)
      out[o++] = c;
  }
  out[o] = '\0';
}

void ha_publish(const HaState &s)
{
  if (!ha_enabled())
    return;

  if (!WiFi.isConnected())
  {
    Log.notice("MQTT: reconnecting WiFi to report" CR);
    mqtt.disconnect(); // drop any socket from before WiFi went down
    if (!wifi_connect())
    {
      wifi_reset();
      return;
    }
  }
  if (!connect_mqtt())
    return;

  char last_check[40], last_refresh[40], next_check[40];
  iso_utc(s.last_check, last_check, sizeof(last_check));
  iso_utc(s.last_refresh, last_refresh, sizeof(last_refresh));
  iso_utc(s.next_check, next_check, sizeof(next_check));

  char status[64], error[200], ssid[40];
  json_escape(s.status, status, sizeof(status));
  json_escape(s.error, error, sizeof(error));
  json_escape(WiFi.SSID().c_str(), ssid, sizeof(ssid));

  char battery[16] = "null", voltage[16] = "null";
  if (s.battery_known)
  {
    snprintf(battery, sizeof(battery), "%.0f", s.battery_percent);
    snprintf(voltage, sizeof(voltage), "%.2f", s.battery_voltage);
  }

  /* Timestamps are JSON strings, or null, which Home Assistant shows as
   * "unknown". */
  char payload[768];
  snprintf(payload, sizeof(payload),
           "{\"battery\":%s,\"voltage\":%s,\"status\":\"%s\",\"error\":\"%s\","
           "\"reset\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d,\"ip\":\"%s\","
           "\"last_check\":%s,\"last_refresh\":%s,\"next_check\":%s,\"failures\":%u}",
           battery, voltage, status, error, s.reset, ssid, (int)WiFi.RSSI(),
           WiFi.localIP().toString().c_str(), last_check, last_refresh, next_check,
           (unsigned)s.failures);

  if (mqtt.publish(STATE_TOPIC, payload, true))
    Log.notice("MQTT: state sent" CR);
  else
    Log.warning("MQTT: state did not send" CR);

  mqtt.loop();
  mqtt.disconnect();
  delay(100); // let the TCP stack flush before the radio goes off
}