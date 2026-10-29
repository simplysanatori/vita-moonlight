#pragma once

#include <stdbool.h>
#include <stddef.h>

// Optional support for the vita-turbo-wifi plugin
// (https://github.com/juanjsebgarcia/vita-turbo-wifi). Everything here does
// nothing, and costs nothing, when the plugin isn't installed. Declarations
// copied from the plugin are in third_party/wifiturbo/.

// Finds out whether the plugin is installed, without calling it unless the
// system resolved its imports. Call once at start-up, from the main thread,
// before anything else here; wifiturbo_present() answers false until then.
void wifiturbo_init(void);

// Whether the plugin's kernel module is loaded, as wifiturbo_init() found.
// Safe from any thread.
bool wifiturbo_present(void);

// True for a while after wifiturbo_apply_recommended(): the plugin is still
// carrying the changes out, and what it reports is out of date
bool wifiturbo_apply_pending(void);

// Looks at the plugin's settings before a stream starts, and keeps a one-line
// notice for wifiturbo_stream_notice() when one of them can interrupt the
// stream. Reads only; never changes the plugin's settings. Does nothing while
// config.wifiturbo_notices is off.
void wifiturbo_prestream_check(void);

// The notice from the last wifiturbo_prestream_check(), or NULL for none
const char *wifiturbo_stream_notice(void);

// Writes the settings that can interrupt a stream, one per line, for the
// Settings help, or says that none do. Returns how many there are, or -1
// when the plugin isn't there or its settings can't be read.
int wifiturbo_advice_details(char *out, size_t size);

// A short summary for a Settings row: "good for streaming", "2 to change",
// "applying changes", or "unavailable"
void wifiturbo_advice_value(char *out, size_t size);

// Writes what wifiturbo_apply_recommended() would change, for a confirmation
// message, and whether it would reconnect Wi-Fi. Returns the number of
// changes, 0 when the settings are already right, or -1 when the plugin
// isn't there.
int wifiturbo_recommended_changes(char *out, size_t size, bool *reconnects);

// Asks the plugin to switch to its recommended streaming settings: Smart Mesh
// Roaming, 40 MHz and In-game HUD off, Max performance. The plugin saves
// them, and reconnects Wi-Fi for a few seconds if it changes the power
// profile or 40 MHz. Returns the number of changes asked for, or -1 on
// failure.
int wifiturbo_apply_recommended(void);

// What the stats overlay shows, read from the plugin once a second
typedef struct {
  bool link;               // snr_db and noise_floor_dbm are valid
  int snr_db;
  int noise_floor_dbm;
  const char *profile;     // the plugin's power profile, or NULL when unknown
} wifiturbo_sample;

// Fills in sample. Returns false when the plugin isn't there. Meant for the
// statistics thread, not for drawing.
bool wifiturbo_sample_link(wifiturbo_sample *sample);
