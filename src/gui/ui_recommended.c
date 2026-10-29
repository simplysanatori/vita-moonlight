/*
 * This file is part of Moonlight Embedded.
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

// Settings > Apply recommended settings: stream settings for a good or weak
// connection and for what matters most to the player (balanced, lowest delay
// or battery life), picked by hand or by testing the connection where the
// player sits. The values come from benchmarks in docs/DECODE.md.

#include "ui_recommended.h"
#include "guilib.h"
#include "ui_connect.h"
#include "../session.h"
#include "../check_host.h"
#include "../config.h"
#include "../connection.h"
#include "../device.h"
#include "../netcount.h"
#include "../util.h"

#include <Limelight.h>

#include <stdbool.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <string.h>

// At or above this the signal counts as good. The stats overlay draws the
// reading in yellow below the same line.
#define GOOD_WIFI_PERCENT 60
// Auto-detect also wants the signal never to dip below this while measuring
#define GOOD_WIFI_MIN_PERCENT 50
#define MEASURE_SECONDS 5
#define MEASURE_INTERVAL_US 250000
// The test stream: the PC's Desktop, which every Sunshine install has, for
// this long after it settles. Even a still Desktop sends ~100 packets a second
// once error correction is counted, enough to see about 1% loss.
#define TEST_APP "Desktop"
#define TEST_SETTLE_US 2000000
#define TEST_SECONDS 10
#define TEST_CONNECT_TIMEOUT_US 20000000
// Good also needs the test stream to lose under this share of packets, with
// the round trip varying by less than this. On a still Desktop the variation
// measured ±3.8 ms at a 74% signal and ±7.2 ms at 60%; only two readings, so
// treat the line as a reasonable one rather than a proven one.
#define GOOD_MAX_LOSS_PERCENT 1.0f
#define GOOD_MAX_JITTER_MS 6

// What the player cares about most, the second question after the connection
enum {
  PRIORITY_BALANCED,
  PRIORITY_DELAY,
  PRIORITY_BATTERY,
  PRIORITY_COUNT,
};

static const char *PRIORITY_NAMES[PRIORITY_COUNT] = {"Balanced", "Lowest delay", "Battery life"};

typedef struct {
  int width, height, fps, bitrate_kbps;
  bool boost_clocks;
} preset;

enum {
  ROW_NOW,
  ROW_PRIORITY,
  ROW_AUTO,
  ROW_GOOD,
  ROW_WEAK,
  ROW_COUNT,
};

static int chosen;
static int priority = PRIORITY_BALANCED;
static bool restart_requested = false;

// A stream paused behind Settings: the new settings need it restarted
static bool stream_paused(void) {
  return connection_get_status() == LI_MINIMIZED;
}

// After applying, offer to restart a paused stream so the settings take effect
static void offer_restart(void) {
  if (!stream_paused()) {
    return;
  }
  restart_requested = display_confirm("These take effect on a new stream. Restart the stream now "
                                      "with the new settings? The game keeps running on your PC.");
}

bool ui_recommended_take_restart(void) {
  bool requested = restart_requested;
  restart_requested = false;
  return requested;
}

// A PS TV runs off the mains and usually sits on Ethernet
static bool is_pstv(void) {
  return config.model == SCE_KERNEL_MODEL_VITATV;
}

// The values, from benchmarks in docs/DECODE.md (with the out-of-order packet
// fix in place):
// - Balanced, good connection: 720p at 20 Mbps was sharp and smooth by the
//   access point (13.9 ms latency estimate); higher bitrates only added delay.
//   On a PS TV's big screen, 720p also gives the encoder more to work with.
// - Balanced, weak connection, and Lowest delay: 540p at 10 Mbps measured
//   ~12.5 ms against ~24 ms for 720p at 20 Mbps at a middling signal (9.4
//   against 13.9 by the access point), with as few freezes, and looks the
//   same on the Vita's 960x544 screen. 720p at 10 Mbps drops frames on busy
//   content, so lower delay stays at 540p.
// - Battery life: half the frames, half the bitrate, no clock boost. Over 10
//   minutes on battery: 312 mA against 426 mA for Balanced and 378 mA for
//   Lowest delay, about 7 hours on a full charge against 5.
// The frame rate follows the player's own when it's 50 or 60, since a stream
// that doesn't match the PC's display costs ~30 ms; otherwise 60.
static preset choose(int connection) {
  int fps = config.stream.fps == 50 || config.stream.fps == 60 ? config.stream.fps : 60;
  preset p = {960, 540, fps, 10000, true};
  if (priority == PRIORITY_BATTERY && !is_pstv()) {
    p.fps = 30;
    p.bitrate_kbps = 5000;
    p.boost_clocks = false;
  } else if (priority == PRIORITY_BALANCED && connection == RECOMMENDED_GOOD) {
    p.width = 1280;
    p.height = 720;
    p.bitrate_kbps = 20000;
  }
  return p;
}

static const char *connection_name(int connection) {
  return connection == RECOMMENDED_GOOD ? "a good connection" : "a weak connection";
}

static void describe(const preset *p, char *out, size_t size) {
  snprintf(out, size, "%dx%d, %d fps, %d Mbps, clock boost %s", p->width, p->height, p->fps,
           p->bitrate_kbps / 1000, p->boost_clocks ? "on" : "off");
}

static void apply(int connection) {
  preset p = choose(connection);
  config.stream.width = p.width;
  config.stream.height = p.height;
  config.stream.fps = p.fps;
  config.stream.bitrate = p.bitrate_kbps;
  config.boost_clocks = p.boost_clocks;
  // The lowest-latency choices, whatever else, except that smooth pacing costs a
  // frame of delay which only Lowest delay refuses
  config.enable_frame_pacer = false;
  config.enable_vita_vblank_wait = false;
  config.frame_pacing = priority == PRIORITY_DELAY ? FRAME_PACING_OFF : FRAME_PACING_SMOOTH;
}

static void show_applied(int connection) {
  preset p = choose(connection);
  char what[128];
  describe(&p, what, sizeof(what));
  display_error("Applied %s for %s: %s, V-Sync off, frame pacing %s.", PRIORITY_NAMES[priority],
                connection_name(connection), what, config.frame_pacing == FRAME_PACING_SMOOTH ? "smooth" : "off");
}

// Shows the live reading and the priority, and marks the choice that fits
static void refresh(menu_entry *menu) {
  int network = vita_network_type();
  bool wired = network == VITA_NETWORK_WIRED;
  bool mobile = network == VITA_NETWORK_MOBILE;
  int percent = vita_wifi_percent();
  if (wired) {
    snprintf(menu[ROW_NOW].subname, sizeof(menu[ROW_NOW].subname), "Wired network");
  } else if (mobile) {
    snprintf(menu[ROW_NOW].subname, sizeof(menu[ROW_NOW].subname), "Mobile network (3G): expect stutter");
  } else if (percent < 0) {
    snprintf(menu[ROW_NOW].subname, sizeof(menu[ROW_NOW].subname), "Not connected to Wi-Fi");
  } else {
    snprintf(menu[ROW_NOW].subname, sizeof(menu[ROW_NOW].subname), "Your Wi-Fi now: %d%%", percent);
  }
  snprintf(menu[ROW_PRIORITY].subname, sizeof(menu[ROW_PRIORITY].subname), "%s", PRIORITY_NAMES[priority]);
  bool known = wired || mobile || percent >= 0;
  bool good = wired || (!mobile && percent >= GOOD_WIFI_PERCENT);
  strcpy(menu[ROW_GOOD].subname, known && good ? "(fits now)" : "");
  strcpy(menu[ROW_WEAK].subname, known && !good ? "(fits now)" : "");
  // Auto-detect streams the PC's desktop, which can't happen while a game is
  menu[ROW_AUTO].disabled = stream_paused();
  strcpy(menu[ROW_AUTO].subname, stream_paused() ? "(quit the game first)" : "");
}

static int start_choice;

static int start_loop(int result, void *context, const input_data *input) {
  start_choice = result;
  return 1;
}

typedef struct {
  int signal_avg, signal_min;
  float loss_percent;
  float jitter_ms;
  bool streamed;
} link_test;

// Auto-detect needs a PC to test against: the one Settings was opened from,
// or else the first paired PC that's online, which it connects to itself (as
// the benchmark does), so the player doesn't have to open the PC's menu first.
// Returns false, after telling the player what to do, when there isn't one.
static bool have_test_pc(void) {
  if (session_server_ready()) {
    return true;
  }
  for (int i = 0; i < known_devices.count && i < MAX_HOSTS; i++) {
    device_info_t *host = &known_devices.devices[i];
    if (!host->paired) {
      continue;
    }
    // The host scan stops while Settings is open, so check again in case the
    // PC was switched on since
    if (g_host_status[i].status != HOST_ONLINE) {
      flash_message("Looking for %s...", host->name);
      if (check_host_status(host).status != HOST_ONLINE) {
        continue;
      }
    }
    flash_message("Connecting to %s...", host->name);
    char err[256] = "";
    if (session_open_headless(host, err, sizeof(err)) == 0) {
      return true;
    }
    display_error("Auto-detect couldn't connect to %s: %s", host->name, err);
    return false;
  }
  display_error("Auto-detect needs a paired PC that's switched on, to test the connection to it. "
                "Turn your PC on, or choose Good or Weak connection yourself.");
  return false;
}

static bool measure_signal(link_test *result) {
  int samples = 0, total = 0, lowest = 100;
  int count = MEASURE_SECONDS * 1000000 / MEASURE_INTERVAL_US;
  for (int i = 0; i < count; i++) {
    int percent = vita_wifi_percent();
    if (percent >= 0) {
      samples++;
      total += percent;
      lowest = percent < lowest ? percent : lowest;
    }
    int left = (count - i) * MEASURE_INTERVAL_US / 1000000 + 1;
    if (percent >= 0) {
      flash_message("Measuring the Wi-Fi signal, hold still...\n\nNow %d%%, %d s left", percent, left);
    } else {
      flash_message("Measuring the Wi-Fi signal, hold still...\n\nNo signal, %d s left", left);
    }
    sceKernelDelayThread(MEASURE_INTERVAL_US);
  }
  if (samples == 0) {
    return false;
  }
  result->signal_avg = total / samples;
  result->signal_min = lowest;
  return true;
}

static bool wait_connected(uint64_t timeout_us) {
  uint64_t start = LiGetMicroseconds();
  while (connection_get_status() != LI_CONNECTED) {
    if (LiGetMicroseconds() - start > timeout_us) {
      return false;
    }
    sceKernelDelayThread(50000);
  }
  return true;
}

// Streams the PC's Desktop at the Good settings and measures packet loss
// (gaps in the video packets' sequence numbers, src/netcount.c; common-c's own
// recovered and failed counters are only kept for audio) and jitter (the
// round trip's variation). Returns false with a message in err if it couldn't.
static bool test_stream(link_test *result, char *err, size_t err_len) {
  int app_id = session_find_app(TEST_APP);
  if (app_id < 0) {
    snprintf(err, err_len, "Your PC has no \"%s\" app in Sunshine, which Auto-detect streams "
             "to test the connection.", TEST_APP);
    return false;
  }
  // Test at 720p and 20 Mbps, the most a good connection gets, then put the
  // player's own settings back
  STREAM_CONFIGURATION saved = config.stream;
  config.stream.width = 1280;
  config.stream.height = 720;
  config.stream.bitrate = 20000;
  // The packet counter (src/netcount.c) only sees full-size video packets,
  // so test at the default size even if the player set a small one
  config.stream.packetSize = 1024;

  bool ok = false;
  if (session_start_app_by_id(app_id, err, err_len) != 0) {
    goto done;
  }
  result->streamed = true;
  if (!wait_connected(TEST_CONNECT_TIMEOUT_US)) {
    snprintf(err, err_len, "The test stream didn't start.");
    goto done;
  }
  sceKernelDelayThread(TEST_SETTLE_US);

  netcount before, after;
  netcount_read(&before);
  double variance_sum = 0;
  int variance_samples = 0;
  for (int i = 0; i < TEST_SECONDS * 2; i++) {
    sceKernelDelayThread(500000);
    if (connection_get_status() != LI_CONNECTED) {
      snprintf(err, err_len, "The test stream ended early.");
      goto done;
    }
    uint32_t rtt = 0, variance = 0;
    if (LiGetEstimatedRttInfo(&rtt, &variance)) {
      variance_sum += variance;
      variance_samples++;
    }
  }
  netcount_read(&after);
  uint32_t lost = netcount_lost(&before, &after);
  uint32_t packets = (after.received - before.received) + lost;
  if (packets == 0) {
    snprintf(err, err_len, "No video arrived during the test stream.");
    goto done;
  }
  if (variance_samples == 0) {
    // No round-trip readings: don't pass the jitter check unmeasured
    snprintf(err, err_len, "The connection's delay couldn't be measured.");
    goto done;
  }
  result->loss_percent = 100.0f * lost / packets;
  result->jitter_ms = variance_sum / variance_samples;
  ok = true;

done:
  if (result->streamed) {
    connection_terminate();
    session_quit_app();
    session_return_to_server();
  }
  config.stream = saved;
  return ok;
}

// Returns RECOMMENDED_NONE if the player cancels or the test can't run
static int auto_detect(void) {
  if (!have_test_pc()) {
    return RECOMMENDED_NONE;
  }
  char *captions[] = {"Start", "Cancel"};
  start_choice = 1;
  int network = vita_network_type();
  bool wired = network == VITA_NETWORK_WIRED;
  if (network == VITA_NETWORK_MOBILE) {
    // 3G can't carry a smooth stream, so there's nothing to measure
    display_error("The Vita is on a mobile (3G) network, which can't carry a smooth stream. Use "
                  "Wi-Fi if you can. Otherwise choose Weak connection, which needs the least.");
    return RECOMMENDED_NONE;
  }
  char intro[512];
  if (wired) {
    snprintf(intro, sizeof(intro),
             "Auto-detect streams %s's desktop for about %d seconds to test the connection. "
             "Press Start when you're ready.", session_server_name(), TEST_SECONDS);
  } else {
    snprintf(intro, sizeof(intro),
             "Take the Vita to where you'll play, and hold it as you normally would. Then press "
             "Start. Auto-detect measures the Wi-Fi signal for %d seconds, then streams %s's "
             "desktop for about %d seconds to test the connection.",
             MEASURE_SECONDS, session_server_name(), TEST_SECONDS);
  }
  display_alert(intro, captions, 2, start_loop, NULL);
  if (start_choice != 0) {
    return RECOMMENDED_NONE;
  }

  link_test result = {0};
  if (!wired && !measure_signal(&result)) {
    display_error("Couldn't read the Wi-Fi signal. Check the Vita is connected to Wi-Fi.");
    return RECOMMENDED_NONE;
  }
  flash_message("Testing the connection to %s.\n\nPlease wait while the connection is "
                "benchmarked. The PC's desktop shows for about %d seconds.",
                session_server_name(), TEST_SECONDS);
  char err[256] = "";
  if (!test_stream(&result, err, sizeof(err))) {
    display_error("Auto-detect couldn't finish the connection test: %s", err);
    return RECOMMENDED_NONE;
  }

  bool signal_ok = wired || (result.signal_avg >= GOOD_WIFI_PERCENT && result.signal_min >= GOOD_WIFI_MIN_PERCENT);
  bool loss_ok = result.loss_percent < GOOD_MAX_LOSS_PERCENT;
  bool jitter_ok = result.jitter_ms < GOOD_MAX_JITTER_MS;
  int which = signal_ok && loss_ok && jitter_ok ? RECOMMENDED_GOOD : RECOMMENDED_WEAK;
  const char *reason = which == RECOMMENDED_GOOD ? ""
                     : !signal_ok ? " The signal is on the weak side."
                     : !loss_ok ? " Packets went missing during the test."
                                : " The connection's timing was uneven.";

  preset p = choose(which);
  char what[128];
  describe(&p, what, sizeof(what));
  char signal[64];
  if (wired) {
    snprintf(signal, sizeof(signal), "Wired network");
  } else {
    snprintf(signal, sizeof(signal), "Signal %d%% (lowest %d%%)", result.signal_avg, result.signal_min);
  }
  char message[768];
  snprintf(message, sizeof(message),
           "%s, %.1f%% of packets lost, jitter %.0f ms: %s.%s%s\n\n"
           "Recommended for %s: %s. Apply it?",
           signal, result.loss_percent, result.jitter_ms, connection_name(which), reason,
           !wired && vita_bluetooth_enabled() ? " Bluetooth is on, which can make it worse." : "",
           PRIORITY_NAMES[priority], what);
  if (!display_confirm(message)) {
    return RECOMMENDED_NONE;
  }
  apply(which);
  show_applied(which);
  offer_restart();
  return which;
}

static const char *help_text(int row) {
  switch (row) {
    case ROW_PRIORITY:
      if (is_pstv()) {
        return "Balanced gives the sharpest picture a connection can carry smoothly: 1280x720 at "
               "20 Mbps on a good one. Lowest delay uses 960x540 at 10 Mbps, which cuts the delay by "
               "a few milliseconds, more on a weaker connection. Use left and right to change.";
      }
      return "Balanced gives the sharpest picture a connection can carry smoothly: 1280x720 at 20 "
             "Mbps on a good one. Lowest delay uses 960x540 at 10 Mbps, which looks the same on the "
             "Vita's screen and cuts the delay by a few milliseconds, more on weaker Wi-Fi. Battery "
             "life streams 30 frames a second at 5 Mbps with clock boost off. Use left and right to "
             "change.";
    case ROW_AUTO:
      return "Tests the connection where you'll play, then recommends settings for it and for "
             "what matters most to you. It measures the Wi-Fi signal (unless you're wired), then "
             "briefly streams your PC's desktop to check for lost packets and uneven timing. It "
             "uses your paired PC if it's on; with several, open Settings from the menu of the "
             "one you want. Quit any game on the PC first.";
    case ROW_GOOD:
      return "For a strong signal, such as in the same room as your router, or a wired PS TV. "
             "Applies the settings for what matters most to you; with Balanced that's 1280x720 at "
             "20 Mbps. Also turns V-Sync off and sets Frame pacing to Smooth, or Off when "
             "Lowest delay matters most.";
    case ROW_WEAK:
      return "For a weaker signal, such as further from your router or through walls. Applies "
             "the settings for what matters most to you; with Balanced that's 960x540 at 10 Mbps, "
             "which looks the same on the Vita's screen and needs half the bandwidth. Also turns "
             "V-Sync off and sets Frame pacing to Smooth, or Off when Lowest delay matters most.";
    default:
      return NULL;
  }
}

static int menu_loop(int id, void *context, const input_data *input) {
  menu_entry *menu = context;

  // Keep the live reading current, about once a second
  static int ticks = 0;
  if (++ticks >= 60) {
    ticks = 0;
    refresh(menu);
  }

  if (ui_pressed(input, UI_INTENT_HELP)) {
    const char *help = help_text(id);
    if (help) {
      display_help(menu[id].name, help);
    }
    return 0;
  }
  if (id == ROW_PRIORITY) {
    bool left = ui_pressed(input, UI_INTENT_LEFT);
    bool right = ui_pressed(input, UI_INTENT_RIGHT | UI_INTENT_CONFIRM);
    if (left || right) {
      // No battery on a PS TV, so no battery option
      int count = is_pstv() ? PRIORITY_BATTERY : PRIORITY_COUNT;
      priority = (priority + (left ? count - 1 : 1)) % count;
      refresh(menu);
    }
    return 0;
  }
  if (!ui_pressed(input, UI_INTENT_CONFIRM)) {
    return 0;
  }
  if (id == ROW_AUTO) {
    chosen = auto_detect();
    return chosen == RECOMMENDED_NONE ? 0 : 1;
  }
  if (id == ROW_GOOD || id == ROW_WEAK) {
    chosen = id == ROW_GOOD ? RECOMMENDED_GOOD : RECOMMENDED_WEAK;
    apply(chosen);
    show_applied(chosen);
    offer_restart();
    return 1;
  }
  return 0;
}

int ui_recommended_settings_menu(void) {
  menu_entry menu[ROW_COUNT];
  memset(menu, 0, sizeof(menu));
  if (is_pstv() && priority == PRIORITY_BATTERY) {
    priority = PRIORITY_BALANCED;
  }
  menu[ROW_NOW] = (menu_entry) { .name = "", .disabled = true };
  menu[ROW_PRIORITY] = (menu_entry) { .name = "What matters most", .id = ROW_PRIORITY, .adjustable = true };
  menu[ROW_AUTO] = (menu_entry) { .name = "Auto-detect: test where you'll play", .id = ROW_AUTO };
  menu[ROW_GOOD] = (menu_entry) { .name = "Good connection: near the router, or wired", .id = ROW_GOOD };
  menu[ROW_WEAK] = (menu_entry) { .name = "Weak connection: further away or through walls", .id = ROW_WEAK };
  refresh(menu);

  chosen = RECOMMENDED_NONE;
  restart_requested = false;
  // A row of 24 px each, plus margins
  menu_geom geom = make_geom_centered(620, (ROW_COUNT + 1) * 24);
  display_menu(menu, ROW_COUNT, &geom, menu_loop, NULL, NULL, menu);
  return chosen;
}
