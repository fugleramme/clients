#ifndef CONFIG_H
#define CONFIG_H

#include <ArduinoLog.h>

// The page address, FRAME_URL, and the WiFi details are set in src/secrets.h.

// ===== Time Zone =====
// A POSIX TZ rule, including the daylight saving dates. This one is
// Europe/Madrid (the same rule as the rest of Central Europe).
#define TIME_ZONE "CET-1CEST,M3.5.0,M10.5.0/3"

// ===== Checking for new birds =====
// How often the frame wakes to ask whether the page changed. The frame's own
// "Panel refresh" setting in the Fugleramme admin page is used when it is
// longer; this is the floor. An unchanged page costs a few seconds of WiFi and
// no refresh, so the battery mostly pays for redraws, not checks.
#define MIN_CHECK_MINUTES 15

// After a failure (WiFi, proxy, server down), wait this long before retrying.
// The panel keeps showing the last birds meanwhile.
#define RETRY_MINUTES 15

// After this many failures in a row, replace the birds with the error screen
// so the problem is visible on the wall. 12 x 15 min = 3 hours. 0 = never.
#define ERROR_SCREEN_AFTER_FAILURES 12

// ===== Night =====
// No checks between these local hours; birds are quiet and so is the frame.
// Set both to the same value to check around the clock.
#define NIGHT_START_HOUR 23
#define NIGHT_END_HOUR 6

// ===== Sleep =====
#define DEEP_SLEEP true // false stays awake and restarts instead; only for debugging

// ===== Battery =====
// MAX17048 fuel gauge on I2C at 0x36. At or below the low level the frame
// shows "please charge" once and sleeps until it is reset.
#define BATTERY_LOW_PERCENT 10        // At or below this, stop: charge, then press reset
#define BATTERY_CHARGING_VOLTAGE 4.0f // Above this the cell is on a charger, so carry on

// ===== Network =====
#define NTP_SERVER "pool.ntp.org" // For night mode and for checking https certificates
#define HTTP_TIMEOUT_MS 20000

// ===== Logging =====
#define DEBUG_MODE false // Verbose logging

#if DEBUG_MODE
#define LOG_LEVEL LOG_LEVEL_VERBOSE
#else
#define LOG_LEVEL LOG_LEVEL_NOTICE
#endif

#endif
