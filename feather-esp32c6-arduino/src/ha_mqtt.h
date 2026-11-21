#ifndef HA_MQTT_H
#define HA_MQTT_H

#include <stdint.h>
#include <time.h>

/*
 * Home Assistant over MQTT, with discovery: the frame shows up as a device
 * with battery, WiFi, reset reason and refresh times, plus a "Redraw on next
 * wake" button.
 *
 * The board sleeps between wakes, so Home Assistant cannot reach it. The
 * button publishes a *retained* command instead; the frame reads it on its
 * next wake, acts on it, and clears it.
 *
 * Everything here is best effort: a missing broker never stops the birds.
 * Leave MQTT_HOST empty in secrets.h to turn it all off.
 */

/* Whether MQTT_HOST is set. */
bool ha_enabled();

/* Connect (WiFi must be up), publish discovery, and read any queued command.
 * Returns true if Home Assistant asked for a redraw; the command is cleared. */
bool ha_begin();

struct HaState
{
  const char *status;      // e.g. "New birds", "Unchanged", "Failed"
  const char *error;       // last error, or "" when all is well
  const char *reset;       // why the board started
  bool battery_known;
  float battery_percent;
  float battery_voltage;
  time_t last_check;       // this wake, 0 if the clock is unknown
  time_t last_refresh;     // last new page drawn, 0 if never
  time_t next_check;       // when the board wakes next, 0 for "until reset"
  uint32_t failures;       // failures in a row
};

/* Publish the state, retained. Reconnects WiFi and MQTT if they are down,
 * once and briefly; gives up quietly. Disconnects cleanly afterwards. */
void ha_publish(const HaState &state);

#endif
