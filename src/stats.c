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

#include "stats.h"
#include "audio/vita.h"
#include "config.h"
#include "debug.h"
#include "gui/guilib.h"
#include "wifiturbo.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <psp2/net/netctl.h>
#include <psp2/power.h>
#include <vita2d.h>

#define STATS_LINE_COUNT 11
#define STATS_LINE_LENGTH 80
#define STATS_FONT_SIZE 16
#define STATS_LINE_HEIGHT 18
// Below this the Wi-Fi reading is drawn in yellow; frame loss starts to climb
#define STATS_WEAK_WIFI_PERCENT 60
#define STATS_ORIGIN_X 10
#define STATS_ORIGIN_Y 20

typedef struct {
  uint64_t sum_us;
  uint64_t max_us;
  uint32_t count;
} timing;

// Counters for the current one-second window, written by the decode thread
typedef struct {
  uint32_t frames_received;
  uint32_t frames_rendered;
  uint32_t frames_dropped_by_pacer;
  uint32_t frames_lost;
  uint32_t idr_frames;
  uint64_t bytes;
  timing network;
  timing queue;
  timing decode;
  timing render;
  timing host;
  // Host + receive + queue + decode + render for each frame that reached the screen
  timing pipeline;
  // CPU time spent drawing the overlay itself, so its cost is visible
  timing overlay;
} window;

static pthread_mutex_t stats_mutex;
static window current = {0};
static int last_frame_number = 0;
// The frame being decoded, built up as it moves through the decode thread
static uint64_t frame_pipeline_us = 0;
static uint64_t last_tick_us = 0;
static RTP_VIDEO_STATS last_rtp_stats = {0};

// Rendered text, written by vitastats_tick() and read by vitastats_draw()
static char lines[STATS_LINE_COUNT][STATS_LINE_LENGTH] = {{0}};
static int line_count = 0;
static float rendered_fps = 0;
// The Wi-Fi signal from the last tick, and which line shows it, so a weak
// signal can be drawn in yellow
static int wifi_percent = -1;
static int wifi_line = -1;
// Bumped whenever lines change, so the draw only re-measures text when needed
static int lines_version = 0;

// Nothing is collected unless someone is looking: the overlay is on, or the
// numbers are going to the log. Read on every frame, so kept trivial.
static bool stats_active(void) {
  return config.stats_overlay != STATS_OVERLAY_OFF || config.save_debug_log;
}

static void timing_add(timing *t, uint64_t us) {
  t->sum_us += us;
  if (us > t->max_us) {
    t->max_us = us;
  }
  t->count++;
}

static float timing_avg_ms(const timing *t) {
  return t->count ? (float)t->sum_us / t->count / 1000.0f : 0;
}

static float timing_max_ms(const timing *t) {
  return (float)t->max_us / 1000.0f;
}

bool vitastats_init(void) {
  return pthread_mutex_init(&stats_mutex, NULL) == 0;
}

void vitastats_reset(void) {
  pthread_mutex_lock(&stats_mutex);
  memset(&current, 0, sizeof(current));
  memset(&last_rtp_stats, 0, sizeof(last_rtp_stats));
  last_frame_number = 0;
  frame_pipeline_us = 0;
  last_tick_us = LiGetMicroseconds();
  line_count = 0;
  rendered_fps = 0;
  pthread_mutex_unlock(&stats_mutex);
}

void vitastats_frame_received(const DECODE_UNIT *du, uint64_t submit_us) {
  if (!stats_active()) {
    return;
  }
  pthread_mutex_lock(&stats_mutex);
  current.frames_received++;
  current.bytes += du->fullLength;
  if (du->frameType == FRAME_TYPE_IDR) {
    current.idr_frames++;
  }
  // Gaps in the frame number are frames the network lost entirely
  if (last_frame_number > 0 && du->frameNumber > last_frame_number + 1) {
    current.frames_lost += du->frameNumber - last_frame_number - 1;
  }
  last_frame_number = du->frameNumber;

  frame_pipeline_us = 0;
  if (du->enqueueTimeUs >= du->receiveTimeUs) {
    timing_add(&current.network, du->enqueueTimeUs - du->receiveTimeUs);
    frame_pipeline_us += du->enqueueTimeUs - du->receiveTimeUs;
  }
  if (submit_us >= du->enqueueTimeUs) {
    timing_add(&current.queue, submit_us - du->enqueueTimeUs);
    frame_pipeline_us += submit_us - du->enqueueTimeUs;
  }
  // Reported in tenths of a millisecond, and zero when the host does not send it
  if (du->frameHostProcessingLatency != 0) {
    timing_add(&current.host, (uint64_t)du->frameHostProcessingLatency * 100);
    frame_pipeline_us += (uint64_t)du->frameHostProcessingLatency * 100;
  }
  pthread_mutex_unlock(&stats_mutex);
}

void vitastats_frame_decoded(uint64_t decode_us) {
  if (!stats_active()) {
    return;
  }
  pthread_mutex_lock(&stats_mutex);
  timing_add(&current.decode, decode_us);
  frame_pipeline_us += decode_us;
  pthread_mutex_unlock(&stats_mutex);
}

void vitastats_frame_rendered(uint64_t render_us) {
  if (!stats_active()) {
    return;
  }
  pthread_mutex_lock(&stats_mutex);
  current.frames_rendered++;
  timing_add(&current.render, render_us);
  timing_add(&current.pipeline, frame_pipeline_us + render_us);
  pthread_mutex_unlock(&stats_mutex);
}

void vitastats_frame_dropped_by_pacer(void) {
  if (!stats_active()) {
    return;
  }
  pthread_mutex_lock(&stats_mutex);
  current.frames_dropped_by_pacer++;
  pthread_mutex_unlock(&stats_mutex);
}

void vitastats_tick(void) {
  uint64_t now_us = LiGetMicroseconds();

  pthread_mutex_lock(&stats_mutex);
  window w = current;
  memset(&current, 0, sizeof(current));
  uint64_t elapsed_us = now_us - last_tick_us;
  last_tick_us = now_us;
  pthread_mutex_unlock(&stats_mutex);

  if (elapsed_us == 0) {
    return;
  }
  if (!stats_active()) {
    // Skip the network and Wi-Fi queries and the formatting entirely
    pthread_mutex_lock(&stats_mutex);
    line_count = 0;
    pthread_mutex_unlock(&stats_mutex);
    return;
  }
  float seconds = elapsed_us / 1000000.0f;

  // Everything below is gathered outside the lock so the decode thread never waits on it
  uint32_t rtt_ms = 0, rtt_var_ms = 0;
  bool have_rtt = LiGetEstimatedRttInfo(&rtt_ms, &rtt_var_ms);

  RTP_VIDEO_STATS rtp = {0};
  const RTP_VIDEO_STATS *rtp_ptr = LiGetRTPVideoStats();
  if (rtp_ptr != NULL) {
    rtp = *rtp_ptr;
  }
  uint32_t fec_recovered = rtp.packetCountFecRecovered - last_rtp_stats.packetCountFecRecovered;
  uint32_t fec_failed = rtp.packetCountFecFailed - last_rtp_stats.packetCountFecFailed;
  uint32_t out_of_sequence = rtp.packetCountOOS - last_rtp_stats.packetCountOOS;
  last_rtp_stats = rtp;

  int pending_frames = LiGetPendingVideoFrames();

  SceNetCtlInfo info;
  int rssi_percent = -1, rssi_dbm = 0, channel = -1;
  if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_RSSI_PERCENTAGE, &info) >= 0) {
    rssi_percent = info.rssi_percentage;
  }
  if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_RSSI_DBM, &info) >= 0) {
    rssi_dbm = (int)info.rssi_dbm;
  }
  if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_CHANNEL, &info) >= 0) {
    channel = info.channel;
  }

  // From the Wi-Fi Turbo plugin, when it's installed. Asked here, once a
  // second on the statistics thread, and only for the overlay that shows it.
  wifiturbo_sample turbo = {0};
  bool have_turbo = config.stats_overlay == STATS_OVERLAY_FULL && wifiturbo_sample_link(&turbo);

  float received_fps = w.frames_received / seconds;
  float drawn_fps = w.frames_rendered / seconds;
  float mbps = w.bytes * 8 / seconds / 1000000.0f;

  // Estimated time from the host capturing a frame to it being on the Vita's
  // screen. The clocks aren't synchronised, so the network leg is half the round
  // trip, and the display's own scan-out is not included.
  float one_way_ms = have_rtt ? rtt_ms / 2.0f : 0;
  float latency_ms = timing_avg_ms(&w.pipeline) + one_way_ms;
  float latency_max_ms = timing_max_ms(&w.pipeline) + one_way_ms;

  char new_lines[STATS_LINE_COUNT][STATS_LINE_LENGTH];
  int n = 0;
  if (w.pipeline.count == 0) {
    snprintf(new_lines[n++], STATS_LINE_LENGTH, "Latency n/a");
  } else {
    snprintf(new_lines[n++], STATS_LINE_LENGTH, "Latency ~%.0f ms (max %.0f)%s",
             latency_ms, latency_max_ms, have_rtt ? "" : ", no RTT yet");
  }
  snprintf(new_lines[n++], STATS_LINE_LENGTH, "%dx%d@%d  %.1f / %.1f Mbps",
           config.stream.width, config.stream.height, config.stream.fps,
           mbps, config.stream.bitrate / 1000.0f);
  snprintf(new_lines[n++], STATS_LINE_LENGTH, "FPS rx %.1f  drawn %.1f  pacer drop %u",
           received_fps, drawn_fps, w.frames_dropped_by_pacer);
  snprintf(new_lines[n++], STATS_LINE_LENGTH, "Lost %u  IDR %u  FEC rec %u fail %u  OOS %u",
           w.frames_lost, w.idr_frames, fec_recovered, fec_failed, out_of_sequence);
  snprintf(new_lines[n++], STATS_LINE_LENGTH, "Net %.1f/%.1f ms  Queue %.1f/%.1f ms (%d)",
           timing_avg_ms(&w.network), timing_max_ms(&w.network),
           timing_avg_ms(&w.queue), timing_max_ms(&w.queue), pending_frames);
  snprintf(new_lines[n++], STATS_LINE_LENGTH, "Decode %.1f/%.1f ms  Render %.1f/%.1f ms",
           timing_avg_ms(&w.decode), timing_max_ms(&w.decode),
           timing_avg_ms(&w.render), timing_max_ms(&w.render));
  if (have_rtt) {
    snprintf(new_lines[n++], STATS_LINE_LENGTH, "Host %.1f ms  RTT %u +/- %u ms",
             timing_avg_ms(&w.host), rtt_ms, rtt_var_ms);
  } else {
    snprintf(new_lines[n++], STATS_LINE_LENGTH, "Host %.1f ms  RTT n/a", timing_avg_ms(&w.host));
  }
  int new_wifi_line = n;
  snprintf(new_lines[n++], STATS_LINE_LENGTH, "Wi-Fi %d%% (%d dBm) ch %d  packet %d B",
           rssi_percent, rssi_dbm, channel, config.stream.packetSize);
  if (have_turbo) {
    // The signal against the noise says more than its strength alone: a
    // strong signal with a low SNR means interference
    char snr[40] = "SNR n/a";
    if (turbo.link) {
      snprintf(snr, sizeof(snr), "SNR %d dB  noise %d dBm", turbo.snr_db, turbo.noise_floor_dbm);
    }
    snprintf(new_lines[n++], STATS_LINE_LENGTH, "%s  profile %s", snr, turbo.profile ? turbo.profile : "n/a");
  }
  snprintf(new_lines[n++], STATS_LINE_LENGTH, "Clocks cpu %d bus %d gpu %d MHz",
           scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(),
           scePowerGetGpuClockFrequency());

  snprintf(new_lines[n++], STATS_LINE_LENGTH, "Overlay %.2f ms CPU per frame", timing_avg_ms(&w.overlay));

  pthread_mutex_lock(&stats_mutex);
  memcpy(lines, new_lines, sizeof(lines));
  line_count = n;
  lines_version++;
  rendered_fps = drawn_fps;
  wifi_percent = rssi_percent;
  wifi_line = new_wifi_line;
  pthread_mutex_unlock(&stats_mutex);

  // One line per second gives comparable numbers between builds or settings
  if (config.save_debug_log) {
    vita_debug_log("[STATS] latency %.1f/%.1f fps rx %.1f drawn %.1f pacer_drop %u lost %u idr %u mbps %.2f "
                   "net %.1f/%.1f queue %.1f/%.1f decode %.1f/%.1f render %.1f/%.1f host %.1f "
                   "rtt %u/%u fec_rec %u fec_fail %u oos %u pending %d audio_pending %d audio_drop %u "
                   "rssi %d%% ch %d overlay %.2f",
                   latency_ms, latency_max_ms,
                   received_fps, drawn_fps, w.frames_dropped_by_pacer, w.frames_lost, w.idr_frames, mbps,
                   timing_avg_ms(&w.network), timing_max_ms(&w.network),
                   timing_avg_ms(&w.queue), timing_max_ms(&w.queue),
                   timing_avg_ms(&w.decode), timing_max_ms(&w.decode),
                   timing_avg_ms(&w.render), timing_max_ms(&w.render),
                   timing_avg_ms(&w.host), rtt_ms, rtt_var_ms,
                   fec_recovered, fec_failed, out_of_sequence, pending_frames,
                   LiGetPendingAudioDuration(), vitaaudio_dropped_packets(),
                   rssi_percent, channel, timing_avg_ms(&w.overlay));
  }
}

void vitastats_draw(void) {
  if (config.stats_overlay == STATS_OVERLAY_OFF) {
    return;
  }

  // The previous frame's drawing cost is recorded under the same lock as the copy
  static uint64_t last_draw_us = 0;
  static int measured_version = -1;
  static int box_width = 0;
  uint64_t start_us = LiGetMicroseconds();

  // Copy out under the lock, then draw without holding it
  char local_lines[STATS_LINE_COUNT][STATS_LINE_LENGTH];
  int n;
  int version;
  float fps;
  int wifi;
  int wifi_at;
  pthread_mutex_lock(&stats_mutex);
  n = line_count;
  version = lines_version;
  fps = rendered_fps;
  wifi = wifi_percent;
  wifi_at = wifi_line;
  memcpy(local_lines, lines, sizeof(local_lines));
  if (last_draw_us > 0) {
    timing_add(&current.overlay, last_draw_us);
  }
  pthread_mutex_unlock(&stats_mutex);

  // Yellow when the signal is weak enough to start costing frames
  unsigned int wifi_color = wifi >= 0 && wifi < STATS_WEAK_WIFI_PERCENT ? RGBA8(0xFF, 0xD0, 0x00, 0xFF)
                                                                        : RGBA8(0xFF, 0xFF, 0xFF, 0xFF);
  if (config.stats_overlay == STATS_OVERLAY_FPS) {
    vita2d_font_draw_textf(font, 40, 20, RGBA8(0xFF, 0xFF, 0xFF, 0xFF), STATS_FONT_SIZE,
                           "fps: %.0f / %d", fps, config.stream.fps);
    if (wifi >= 0) {
      vita2d_font_draw_textf(font, 40, 20 + STATS_LINE_HEIGHT, wifi_color, STATS_FONT_SIZE, "Wi-Fi %d%%", wifi);
    }
    last_draw_us = 0;
    return;
  }

  if (n == 0) {
    last_draw_us = 0;
    return;
  }
  // Measuring text is the expensive part, so only redo it when the text changed
  if (version != measured_version) {
    box_width = 0;
    for (int i = 0; i < n; i++) {
      int w = vita2d_font_text_width(font, STATS_FONT_SIZE, local_lines[i]);
      if (w > box_width) {
        box_width = w;
      }
    }
    measured_version = version;
  }
  vita2d_draw_rectangle(STATS_ORIGIN_X - 6, STATS_ORIGIN_Y - STATS_LINE_HEIGHT + 2,
                        box_width + 12, n * STATS_LINE_HEIGHT + 8, RGBA8(0, 0, 0, 0xA0));
  for (int i = 0; i < n; i++) {
    vita2d_font_draw_text(font, STATS_ORIGIN_X, STATS_ORIGIN_Y + i * STATS_LINE_HEIGHT,
                          i == wifi_at ? wifi_color : RGBA8(0xFF, 0xFF, 0xFF, 0xFF), STATS_FONT_SIZE, local_lines[i]);
  }
  last_draw_us = LiGetMicroseconds() - start_us;
}
