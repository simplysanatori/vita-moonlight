#include "wifiturbo.h"

#include "config.h"
#include "debug.h"
#include "wifiturbo_api.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>

// Moonlight links the plugin's SceWifiTurbo functions weakly, so it starts
// without the plugin. Nothing below calls one until wqSelfTestPing() has
// returned the plugin's magic number, and anything else counts as "not
// installed".
//
// Settings the plugin could change under a stream are read, never changed,
// unless the player picks "Apply recommended Wi-Fi Turbo settings".

// The plugin restarts Wi-Fi and Bluetooth once, about 20 s after the first
// connection after start-up, when its receive buffer isn't Default
#define RECENT_BOOT_US 120000000ULL
// Largest notice, with its "Wi-Fi Turbo: " lead-in
#define NOTICE_SIZE 160

enum {
  PROBE_UNKNOWN = -1,
  PROBE_ABSENT = 0,
  PROBE_PRESENT = 1,
};
static int probe_state = PROBE_UNKNOWN;

static char stream_notice[NOTICE_SIZE] = "";

// What can go wrong, in the order it is listed
enum {
  ISSUE_ROAM,
  ISSUE_HT40,
  ISSUE_HUDGAME,
  ISSUE_RECENT_BOOT,
  ISSUE_PROFILE,
  ISSUE_COUNT,
};

static const struct {
  const char *fix;     // what to do, for the notice
  const char *detail;  // why, for the Settings help
} ISSUES[ISSUE_COUNT] = {
  {"turn off Smart Mesh Roaming",
   "Smart Mesh Roaming is on. It scans other channels and can reconnect Wi-Fi while you stream, "
   "which freezes the picture for several seconds. Turn it off."},
  {"turn off 40 MHz",
   "40 MHz is on. A stream needs far less than 20 MHz carries, and 40 MHz picks up more "
   "interference on a busy 2.4 GHz network. Turn it off."},
  {"turn off In-game HUD",
   "In-game HUD is on, so the plugin draws its overlay on top of the stream. Turn it off; the "
   "stats overlay here shows the signal instead."},
  {"wait a minute for Wi-Fi to settle",
   "The receive buffer is Enhanced or Maximum and the Vita started less than two minutes ago. "
   "The plugin restarts Wi-Fi and Bluetooth once after start-up to apply it, which would end "
   "the stream. Wait a minute, or stream after that restart has happened."},
  {"set Max performance",
   "The power profile isn't Max performance, so the Wi-Fi radio sleeps between packets, which "
   "adds 100 to 400 ms of delay when the video pauses. Set it to Max performance."},
};

static const char *const PROFILE_NAMES[] = {"Max performance", "Balanced", "Battery saver"};

/*
 * Finding the plugin
 */

static bool do_probe(void) {
  // If calling the plugin ever crashed the app (a weak import that isn't
  // really resolved), the file stays behind, and the plugin is not asked
  // again. Delete wifiturbo.probe from the Moonlight data folder to try again.
  char marker[sizeof(config.key_dir) + 16];
  snprintf(marker, sizeof(marker), "%swifiturbo.probe", config.key_dir);
  SceIoStat stat;
  if (sceIoGetstat(marker, &stat) >= 0) {
    vita_debug_log("[WIFITURBO] %s exists: the last check crashed, so Wi-Fi Turbo is ignored\n", marker);
    return false;
  }
  SceUID fd = sceIoOpen(marker, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (fd >= 0) {
    sceIoClose(fd);
  }
  int magic = wqSelfTestPing();
  if (fd >= 0) {
    sceIoRemove(marker);
  }
  vita_debug_log("[WIFITURBO] wqSelfTestPing returned 0x%08X\n", (unsigned)magic);
  return (uint32_t)magic == WQ_SELFTEST_MAGIC;
}

bool wifiturbo_present(void) {
  int state = __atomic_load_n(&probe_state, __ATOMIC_ACQUIRE);
  if (state == PROBE_UNKNOWN) {
    state = do_probe() ? PROBE_PRESENT : PROBE_ABSENT;
    __atomic_store_n(&probe_state, state, __ATOMIC_RELEASE);
  }
  return state == PROBE_PRESENT;
}

// Reads the plugin's published settings. False when it isn't there, can't be
// read, or returns values this code doesn't know, as a different plugin
// version might.
static bool read_state(WqUiState *state) {
  if (!wifiturbo_present()) {
    return false;
  }
  // The kernel copies its own idea of the struct's size, which a newer plugin
  // may have made larger than ours
  union {
    WqUiState state;
    char room[256];
  } buffer;
  memset(&buffer, 0, sizeof(buffer));
  if (wqUiReadState(&buffer.state) != WQ_OK) {
    return false;
  }
  const WqUiState *s = &buffer.state;
  bool known = (s->roam == 0 || s->roam == 1) && (s->ht40 == 0 || s->ht40 == 1) &&
               (s->hudgame == 0 || s->hudgame == 1) && s->profile >= WQ_PROFILE_MAX_PERF &&
               s->profile <= WQ_PROFILE_MAX_BATTERY && s->rxring >= 0 && s->rxring <= 2;
  if (!known) {
    vita_debug_log("[WIFITURBO] unexpected settings: roam %d ht40 %d hudgame %d profile %d rxring %d\n",
                   s->roam, s->ht40, s->hudgame, s->profile, s->rxring);
    return false;
  }
  *state = *s;
  return true;
}

/*
 * What can interrupt a stream
 */

// Fills issues with the ISSUE_* that apply and returns how many
static int find_issues(const WqUiState *state, int issues[ISSUE_COUNT]) {
  int n = 0;
  if (state->roam) {
    issues[n++] = ISSUE_ROAM;
  }
  if (state->ht40) {
    issues[n++] = ISSUE_HT40;
  }
  if (state->hudgame) {
    issues[n++] = ISSUE_HUDGAME;
  }
  // Time since start-up stands in for time since connecting, which the plugin
  // doesn't report; it is never less
  if (state->rxring != 0 && sceKernelGetSystemTimeWide() < RECENT_BOOT_US) {
    issues[n++] = ISSUE_RECENT_BOOT;
  }
  if (state->profile != WQ_PROFILE_MAX_PERF) {
    issues[n++] = ISSUE_PROFILE;
  }
  return n;
}

void wifiturbo_prestream_check(void) {
  stream_notice[0] = '\0';
  if (!config.wifiturbo_notices) {
    return;
  }
  WqUiState state;
  int issues[ISSUE_COUNT];
  if (!read_state(&state)) {
    return;
  }
  int n = find_issues(&state, issues);
  if (n == 0) {
    return;
  }
  // The first two on the line, as a pill on a 960 pixel screen only has room
  // for so much; Settings lists the rest
  if (n == 1) {
    snprintf(stream_notice, sizeof(stream_notice), "Wi-Fi Turbo: %s", ISSUES[issues[0]].fix);
  } else if (n == 2) {
    snprintf(stream_notice, sizeof(stream_notice), "Wi-Fi Turbo: %s, %s",
             ISSUES[issues[0]].fix, ISSUES[issues[1]].fix);
  } else {
    snprintf(stream_notice, sizeof(stream_notice), "Wi-Fi Turbo: %s, %s and %d more",
             ISSUES[issues[0]].fix, ISSUES[issues[1]].fix, n - 2);
  }
  vita_debug_log("[WIFITURBO] %s\n", stream_notice);
}

const char *wifiturbo_stream_notice(void) {
  return stream_notice[0] != '\0' ? stream_notice : NULL;
}

int wifiturbo_advice_details(char *out, size_t size) {
  out[0] = '\0';
  WqUiState state;
  if (!read_state(&state)) {
    return -1;
  }
  int issues[ISSUE_COUNT];
  int n = find_issues(&state, issues);
  size_t used = 0;
  for (int i = 0; i < n && used < size; i++) {
    used += snprintf(out + used, size - used, "%s%s", i > 0 ? "\n\n" : "", ISSUES[issues[i]].detail);
  }
  return n;
}

void wifiturbo_advice_value(char *out, size_t size) {
  WqUiState state;
  if (!read_state(&state)) {
    snprintf(out, size, "not found");
    return;
  }
  int issues[ISSUE_COUNT];
  int n = find_issues(&state, issues);
  if (n == 0) {
    snprintf(out, size, "good for streaming");
  } else {
    snprintf(out, size, "%d to change", n);
  }
}

/*
 * Applying the recommended settings
 */

// The commands that turn the settings that matter to what suits streaming,
// in the order they are sent: the quick ones first, as the last two
// reconnect Wi-Fi. These are the plugin's own command strings (user/dispatch.c).
static int recommended_commands(const WqUiState *state, const char *commands[4], const char *names[4],
                                bool *reconnects) {
  int n = 0;
  *reconnects = false;
  if (state->roam) {
    commands[n] = "roam off";
    names[n++] = "Smart Mesh Roaming off";
  }
  if (state->hudgame) {
    commands[n] = "hudgame off";
    names[n++] = "In-game HUD off";
  }
  if (state->ht40) {
    commands[n] = "ht40 off";
    names[n++] = "40 MHz off";
    *reconnects = true;
  }
  if (state->profile != WQ_PROFILE_MAX_PERF) {
    commands[n] = "profile maxperf";
    names[n++] = "Max performance";
    *reconnects = true;
  }
  return n;
}

int wifiturbo_recommended_changes(char *out, size_t size, bool *reconnects) {
  out[0] = '\0';
  *reconnects = false;
  WqUiState state;
  if (!read_state(&state)) {
    return -1;
  }
  const char *commands[4], *names[4];
  int n = recommended_commands(&state, commands, names, reconnects);
  size_t used = 0;
  for (int i = 0; i < n && used < size; i++) {
    used += snprintf(out + used, size - used, "%s%s", i > 0 ? ", " : "", names[i]);
  }
  return n;
}

int wifiturbo_apply_recommended(void) {
  WqUiState state;
  if (!read_state(&state)) {
    return -1;
  }
  const char *commands[4], *names[4];
  bool reconnects;
  int n = recommended_commands(&state, commands, names, &reconnects);
  for (int i = 0; i < n; i++) {
    // The plugin copies a whole command buffer, however short the text
    char line[WQ_UI_CMD_MAX];
    memset(line, 0, sizeof(line));
    snprintf(line, sizeof(line), "%s", commands[i]);
    int ret = wqUiSubmit(line);
    vita_debug_log("[WIFITURBO] wqUiSubmit(\"%s\") returned %d\n", commands[i], ret);
    if (ret != WQ_OK) {
      return -1;
    }
  }
  return n;
}

/*
 * The stats overlay
 */

bool wifiturbo_sample_link(wifiturbo_sample *sample) {
  memset(sample, 0, sizeof(*sample));
  if (!wifiturbo_present()) {
    return false;
  }
  WqLinkInternals link;
  memset(&link, 0, sizeof(link));
  if (wqChipObserveGetLink(&link) == WQ_OK && link.valid) {
    sample->link = true;
    sample->snr_db = link.snr_db;
    sample->noise_floor_dbm = link.noise_floor_dbm;
  }
  WqUiState state;
  if (read_state(&state)) {
    sample->profile = PROFILE_NAMES[state.profile];
  }
  return true;
}
