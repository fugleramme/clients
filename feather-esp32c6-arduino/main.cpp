/* fugleramme-frame: a battery Fugleramme panel.
 *
 * The board wakes every MIN_CHECK_MINUTES (or the frame's Panel refresh, if
 * longer), asks the Fugleramme server whether the birds changed, and only
 * redraws the e-ink panel when they did. Nights are skipped. A failure keeps
 * the last birds on the glass and retries; a long run of failures, or a flat
 * battery, puts the error screen up instead.
 *
 * Each wake also reports to Home Assistant over MQTT, if MQTT_HOST is set,
 * and picks up a queued "redraw" from it.
 *
 * Hardware: Adafruit Feather ESP32-C6 + Pimoroni Inky Impression 13.3" (EL133UF1).
 * Wiring:   libs/el133/src/el133_pins.h
 * Settings: src/config.h, addresses and passwords in src/secrets.h
 *
 * The panel driver, battery, WiFi, clock and error screen come from
 * FutureSharks/e-paper-art-display (MIT, see LICENSE-e-paper-art-display).
 */

#include <Arduino.h>
#include <ArduinoLog.h>
#include <Preferences.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <time.h>

#include "el133.h"
#include "fuglefeed.h"
#include "src/battery.h"
#include "src/builtin_led.h"
#include "src/config.h"
#include "src/error_screen.h"
#include "src/ha_mqtt.h"
#include "src/power_rail.h"
#include "src/secrets.h"
#include "src/setup_time.h"
#include "src/setup_wifi.h"

#ifndef FRAME_URL
#error "src/secrets.h needs FRAME_URL: copy it from src/secrets_template.h"
#endif

static const uint64_t US_PER_SECOND = 1000000ULL;
static const uint32_t SECONDS_PER_MINUTE = 60;
static const uint32_t SLEEP_FOREVER = 0;

static const int MAX_WIFI_ATTEMPTS = 3;
static const uint32_t WIFI_RETRY_DELAY_MS = 3000;

static const uint32_t LED_FLASH_WAITING_MS = 600;
static const uint32_t LED_FLASH_DOWNLOADING_MS = 150;

static_assert(MIN_CHECK_MINUTES >= 5, "MIN_CHECK_MINUTES below 5 drains the battery fast");
static_assert(NIGHT_START_HOUR >= 0 && NIGHT_START_HOUR <= 23 && NIGHT_END_HOUR >= 0 &&
                  NIGHT_END_HOUR <= 23,
              "NIGHT_START_HOUR and NIGHT_END_HOUR are hours, 0-23");

/* What this wake knows, for the Home Assistant report. */
static esp_reset_reason_t reset_reason;
static BatteryStatus battery;
static bool battery_ok = false;

static const char *reset_reason_name(esp_reset_reason_t reason)
{
  switch (reason)
  {
  case ESP_RST_POWERON:   return "power-on";
  case ESP_RST_EXT:       return "reset button";
  case ESP_RST_SW:        return "software restart";
  case ESP_RST_PANIC:     return "crash";
  case ESP_RST_INT_WDT:   return "interrupt watchdog";
  case ESP_RST_TASK_WDT:  return "task watchdog";
  case ESP_RST_WDT:       return "other watchdog";
  case ESP_RST_DEEPSLEEP: return "deep sleep wake";
  case ESP_RST_BROWNOUT:  return "BROWNOUT - the 3V3 rail collapsed";
  case ESP_RST_USB:       return "USB reset (upload)";
  default:                return "unknown";
  }
}

/* ---- Small NVS state --------------------------------------------------- */

static const char *NVS_MAIN = "frame";

static uint32_t failures_get()
{
  Preferences prefs;
  prefs.begin(NVS_MAIN, true);
  uint32_t n = prefs.getUInt("failures", 0);
  prefs.end();
  return n;
}

static uint32_t failures_bump()
{
  Preferences prefs;
  prefs.begin(NVS_MAIN, false);
  uint32_t n = prefs.getUInt("failures", 0) + 1;
  prefs.putUInt("failures", n);
  prefs.end();
  return n;
}

static void failures_clear()
{
  Preferences prefs;
  prefs.begin(NVS_MAIN, false);
  if (prefs.getUInt("failures", 0) != 0)
    prefs.putUInt("failures", 0);
  prefs.end();
}

static time_t last_refresh_get()
{
  Preferences prefs;
  prefs.begin(NVS_MAIN, true);
  time_t t = (time_t)prefs.getULong64("refreshed", 0);
  prefs.end();
  return t;
}

static void last_refresh_set(time_t t)
{
  Preferences prefs;
  prefs.begin(NVS_MAIN, false);
  prefs.putULong64("refreshed", (uint64_t)t);
  prefs.end();
}

/* ---- Sleeping ---------------------------------------------------------- */

/* Never returns: a timer wake reboots the chip. */
static void sleep_for_seconds(uint32_t seconds, const char *why)
{
  if (seconds == SLEEP_FOREVER)
    Log.notice("%s; sleeping until reset" CR, why);
  else
    Log.notice("%s; sleeping %dh %dm" CR, why, (int)(seconds / 3600),
               (int)(seconds / 60 % 60));
  Serial.flush();
  builtin_led_off();

#if !DEEP_SLEEP
  Log.notice("DEEP_SLEEP off: staying awake, then restarting" CR);
  if (seconds == SLEEP_FOREVER)
    for (;;)
      delay(1000);
  delay((uint32_t)seconds * 1000UL);
  esp_restart();
#else
  if (seconds != SLEEP_FOREVER)
    esp_sleep_enable_timer_wakeup((uint64_t)seconds * US_PER_SECOND);
  power_rail_off_for_deep_sleep();
  esp_deep_sleep_start();
#endif
}

static bool is_night_hour(int hour)
{
  if (NIGHT_START_HOUR == NIGHT_END_HOUR)
    return false;
  if (NIGHT_START_HOUR < NIGHT_END_HOUR)
    return hour >= NIGHT_START_HOUR && hour < NIGHT_END_HOUR;
  return hour >= NIGHT_START_HOUR || hour < NIGHT_END_HOUR; // across midnight
}

static int local_hour_in(uint32_t seconds_from_now)
{
  time_t t = time(nullptr) + (time_t)seconds_from_now;
  struct tm local;
  localtime_r(&t, &local);
  return local.tm_hour;
}

/* `minutes` from now, or the end of the night if that lands in it. */
static uint32_t seconds_skipping_night(uint32_t minutes)
{
  uint32_t seconds = minutes * SECONDS_PER_MINUTE;
  if (time_valid())
  {
    time_zone_set();
    if (is_night_hour(local_hour_in(seconds)))
    {
      Log.notice("That lands in the night; waking at %d:00 instead" CR, NIGHT_END_HOUR);
      seconds = seconds_until_local_time(NIGHT_END_HOUR, 0, 0);
    }
  }
  return seconds;
}

/* ---- Home Assistant ---------------------------------------------------- */

static void report(const char *status, const char *error, uint32_t sleep_seconds,
                   uint32_t failures)
{
  if (!ha_enabled())
    return;

  HaState s = {};
  s.status = status;
  s.error = error != nullptr ? error : "";
  s.reset = reset_reason_name(reset_reason);
  s.battery_known = battery_ok && battery.present;
  s.battery_percent = battery.percent;
  s.battery_voltage = battery.voltage;
  const bool clock = time_valid();
  const time_t now = clock ? time(nullptr) : 0;
  s.last_check = now;
  s.last_refresh = last_refresh_get();
  s.next_check = (clock && sleep_seconds != SLEEP_FOREVER) ? now + (time_t)sleep_seconds : 0;
  s.failures = failures;
  ha_publish(s);
}

/* ---- Failures ---------------------------------------------------------- */

/* Put the error screen up. The birds are gone from the glass, so forget them:
 * the next good wake redraws the page even if it has not changed. */
static void show_error_screen(const char *what, const char *detail, const char *next)
{
  wifi_reset();

  char when[24] = "";
  if (time_valid())
  {
    time_zone_set();
    time_t now = time(nullptr);
    struct tm local;
    localtime_r(&now, &local);
    strftime(when, sizeof(when), "%d %b %H:%M - ", &local);
  }

  char message[400];
  const bool has_detail = detail != nullptr && detail[0] != '\0';
  snprintf(message, sizeof(message), "%s\n%s%s%s%s", what, has_detail ? detail : "",
           has_detail ? "\n" : "", when, next);

  if (error_screen_show(message))
    fuglefeed_forget();
  else
    Log.error("The error screen did not refresh either" CR);
}

/* Something went wrong this wake. Keep the birds and retry, unless it has gone
 * on long enough that the wall should say so. Never returns. */
static void failed(const char *what, const char *detail)
{
  const uint32_t n = failures_bump();
  Log.error("%s (failure %u in a row)" CR, what, (unsigned)n);

  const uint32_t seconds = seconds_skipping_night(RETRY_MINUTES);
  char error[240];
  snprintf(error, sizeof(error), "%s%s%s", what, (detail && detail[0]) ? ": " : "",
           detail ? detail : "");
  report("Failed", error, seconds, n);

  if (ERROR_SCREEN_AFTER_FAILURES > 0 && n == ERROR_SCREEN_AFTER_FAILURES)
  {
    char next[40];
    snprintf(next, sizeof(next), "trying again every %u min", (unsigned)RETRY_MINUTES);
    show_error_screen(what, detail, next);
  }
  wifi_reset();
  sleep_for_seconds(seconds, "Will retry");
}

/* ---- Progress ---------------------------------------------------------- */

static void on_stage(FuglefeedStage stage)
{
  switch (stage)
  {
  case FUGLEFEED_STAGE_CHECK:
  case FUGLEFEED_STAGE_DOWNLOAD:
    builtin_led_flash(LED_FLASH_DOWNLOADING_MS);
    break;
  case FUGLEFEED_STAGE_REFRESH:
    builtin_led_off();
    battery_monitor_start(); // the load that browns a weak supply out
    break;
  }
}

/* ---- The wake ---------------------------------------------------------- */

void setup()
{
  builtin_led_begin();

  Serial.begin(115200);
  unsigned long serial_start = millis();
  while (!Serial && (millis() - serial_start) < 1500)
    delay(10);

  Log.begin(LOG_LEVEL, &Serial);
  Log.notice("=== fugleramme-frame ===" CR);

  reset_reason = esp_reset_reason();
  if (reset_reason == ESP_RST_BROWNOUT)
    Log.error("Last reset: %s" CR, reset_reason_name(reset_reason));
  else
    Log.notice("Last reset: %s" CR, reset_reason_name(reset_reason));

  /* A timer wake that drifted into the night goes straight back to sleep,
   * before paying for WiFi. The RTC keeps the time through deep sleep. */
  if (reset_reason == ESP_RST_DEEPSLEEP && time_valid())
  {
    time_zone_set();
    if (is_night_hour(local_hour_in(0)))
      sleep_for_seconds(seconds_until_local_time(NIGHT_END_HOUR, 0, 0), "Night");
  }

  el133_begin();

  battery_begin();
  battery_log();
  battery_report_previous(reset_reason == ESP_RST_BROWNOUT);
  battery_ok = battery_read(battery);

  if (battery_ok && battery.present && battery.percent <= BATTERY_LOW_PERCENT &&
      battery.voltage < BATTERY_CHARGING_VOLTAGE)
  {
    char what[48];
    snprintf(what, sizeof(what), "Battery low (%d%%) - please charge",
             (int)(battery.percent + 0.5f));
    report("Battery low", what, SLEEP_FOREVER, failures_get());
    show_error_screen(what, nullptr, "then press reset");
    sleep_for_seconds(SLEEP_FOREVER, what);
  }

  builtin_led_flash(LED_FLASH_WAITING_MS);

  bool connected = false;
  for (int attempt = 1; attempt <= MAX_WIFI_ATTEMPTS && !connected; attempt++)
  {
    connected = wifi_connect();
    if (!connected)
    {
      wifi_reset();
      if (attempt < MAX_WIFI_ATTEMPTS)
        delay(WIFI_RETRY_DELAY_MS);
    }
  }
  if (!connected)
  {
    char detail[96];
    snprintf(detail, sizeof(detail), "Network \"%s\": %s", WIFI_SSID, wifi_failure_reason());
    failed("Could not connect to WiFi", detail);
  }

  /* Resync every wake: the RTC drifts over hours of deep sleep, and the clock
   * decides the night. A plain http:// page works without it; https needs it
   * to check the certificate. */
  if (!time_set(true) && !time_valid())
  {
    Log.warning("No clock from NTP; night mode is off this wake" CR);
    if (strncasecmp(FRAME_URL, "https://", 8) == 0)
      failed("Could not set the clock (NTP)", "https needs it to check the certificate");
  }

  /* Home Assistant may have queued a redraw while the board slept. */
  const bool ha_redraw = ha_begin();

  FuglefeedConfig cfg = fuglefeed_default_config();
  cfg.url = FRAME_URL;
  cfg.http_timeout_ms = HTTP_TIMEOUT_MS;
  cfg.on_stage = on_stage;
  /* Pressing reset, plugging the battery in, a fresh upload, or the button in
   * Home Assistant always redraws. */
  cfg.force = ha_redraw || reset_reason == ESP_RST_EXT || reset_reason == ESP_RST_POWERON ||
              reset_reason == ESP_RST_USB;

  const FuglefeedResult result = fuglefeed_update(cfg);
  if (result == FUGLEFEED_REFRESH_FAILED)
    battery_monitor_stop(false);
  else if (result == FUGLEFEED_SHOWN)
    battery_monitor_stop(true);

  switch (result)
  {
  case FUGLEFEED_FAILED:
    failed("Could not get the birds", fuglefeed_last_error());
    break;
  case FUGLEFEED_REFRESH_FAILED:
    failed("The panel did not refresh", "check the BUSY wire and the capacitor");
    break;
  case FUGLEFEED_SHOWN:
    if (time_valid())
      last_refresh_set(time(nullptr));
    battery_log();
    Log.notice("New birds on the wall" CR);
    break;
  case FUGLEFEED_UNCHANGED:
    break;
  }

  failures_clear();

  uint32_t minutes = fuglefeed_poll_minutes();
  if (minutes < MIN_CHECK_MINUTES)
    minutes = MIN_CHECK_MINUTES;
  const uint32_t seconds = seconds_skipping_night(minutes);

  report(result == FUGLEFEED_SHOWN ? "New birds" : "Unchanged", "", seconds, 0);
  wifi_reset();
  sleep_for_seconds(seconds, "Next check");
}

void loop()
{
  /* Never reached: setup() always ends in sleep. */
  delay(1000);
}
