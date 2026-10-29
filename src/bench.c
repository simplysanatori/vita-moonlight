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

#include "bench.h"
#include "config.h"
#include "connection.h"
#include "debug.h"
#include "netcount.h"
#include "device.h"
#include "gui/guilib.h"
#include "gui/ui_connect.h"
#include "session.h"

#include <enet/enet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/display.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/netctl.h>
#include <psp2/power.h>
#include <psp2/rtc.h>

// The page's machine-readable strip, as fractions of the frame. These must
// match LAYOUT in tools/bench/bench.html.
#define MARKER_CX (0.010f + 0.060f / 2)
#define MARKER_CY (0.015f + 0.070f / 2)
#define BARCODE_X 0.100f
#define BARCODE_BLOCK_W 0.045f
#define BARCODE_CY (0.020f + 0.060f / 2)
// The two mouse pointer targets in the cursor lane (hotspot positions), and
// the box below and right of each where the pointer's arrow is drawn
static const float CURSOR_TARGET_X[2] = {0.930f, 0.965f};
#define CURSOR_TARGET_Y 0.020f
#define CURSOR_BOX_W 0.012f
#define CURSOR_BOX_H 0.035f
// Absolute mouse positions are sent on this scale
#define MOUSE_REFERENCE 10000
// Without the page's reference blocks (cursor mode on a plain dark screen),
// the pointer counts as present above this luminance
#define CURSOR_FIXED_THRESHOLD 160
#define CURSOR_PARK_TIMEOUT_US 5000000ULL
#define FRAME_BITS 16
#define TICK_MASK ((1 << FRAME_BITS) - 1)

// The white and black reference blocks must differ by at least this much
// luminance for a frame to count as showing the page
#define MIN_REFERENCE_CONTRAST 80

// Windows virtual key codes, which is what GameStream hosts expect
#define VK_SPACE 0x20
#define VK_R 0x52

#define CONNECT_TIMEOUT_US (20 * 1000000ULL)
// A browser started by Sunshine can take a while to show the page
#define PAGE_TIMEOUT_US (45 * 1000000ULL)
#define RESET_SETTLE_US 1500000ULL
// Slightly off one second, so presses drift across the page's frame phase
#define PRESS_INTERVAL_US 1013000ULL
#define KEY_HOLD_US 30000
#define POLL_US 5000
// A press whose marker hasn't appeared by now counts as missed
#define PRESS_TIMEOUT_US 800000ULL
// Room for frames at up to 120 fps
#define MAX_FPS_RECORDED 120
// Frame grabs (config.bench_grab): the decoded frame at these page ticks
// after the reset. The page draws the same picture at the same tick, so runs
// with different settings can be compared picture for picture
// (tools/bench/quality.py).
#define GRAB_COUNT 5
#define GRAB_FIRST_TICK 600
#define GRAB_TICK_STEP 600
// Report paths start with the config folder, which can be up to 4 KB
#define REPORT_PATH_MAX (sizeof(config.key_dir) + 64)

enum {
  PHASE_IDLE,
  PHASE_WARMUP,
  PHASE_MEASURING,
};

// What a timed press sends
enum {
  INPUT_KEY,
  INPUT_MOUSE,
  INPUT_GAMEPAD,
  // Moves the pointer between two targets and watches for it, which times
  // the host's input and capture without the page drawing anything
  INPUT_CURSOR,
  // No presses at all: just stream and measure, for any app
  INPUT_NONE,
};

typedef struct {
  float *values;
  int count;
  int capacity;
} series;

// What happened to one timed press, written into the report so odd results
// can be traced frame by frame
typedef struct {
  int target;          // cursor mode: the target the pointer was sent to
  int sent_frame;      // last frame number decoded when the press was sent
  int seen_frame;      // frame where it was detected, or -1
  int lum_target;      // brightest pixel at the expected spot in that frame
  int lum_other;       // and at the other spot (cursor mode)
  float lag_ms;        // send to on screen, or -1 if never seen
  float to_enet_ms;    // our input call to common-c handing the packet to ENet, or -1
  float to_wire_ms;    // our input call to the datagram being sent, or -1
} press_record;

static pthread_mutex_t bench_mutex;
static volatile int phase = PHASE_IDLE;

// A copy of one decoded frame, as the decoder wrote it
typedef struct {
  uint8_t *data;       // NULL until grabbed
  size_t bytes;
  int tick;
  int stride_bytes;
  int texture_height;
  float region[4];     // the part shown on screen: x1, y1, x2, y2
} frame_grab;

// Only touched by the decode thread while measuring, and by the run's own
// thread before and after it
static frame_grab grabs[GRAB_COUNT];

// Written by the decode thread under bench_mutex
static struct {
  series decode_ms, network_ms, queue_ms, render_ms, pipeline_ms, input_lag_ms;
  // decode_ms split into assembling the frame and the hardware decoder itself
  series assemble_ms, hw_decode_ms;
  // From a frame being handed to the display to the next screen refresh,
  // when vita2d's NEXTFRAME flip actually puts it up. None of the timings
  // above include this wait.
  series vblank_wait_ms;
  // Frames handed over but replaced by a newer one before the refresh, so
  // never shown
  uint32_t frames_replaced;
  uint64_t present_pending_us;
  int present_pending_count;
  uint32_t frames_received, frames_rendered, frames_lost, idr_frames;
  // Separate runs of missing frames: each is one freeze while the Vita waits
  // for a keyframe after a frame couldn't be rebuilt
  uint32_t freezes;
  uint64_t bytes;
  uint32_t ticks_read, ticks_unreadable, ticks_repeated, ticks_skipped, ticks_backwards;
  // Total forward ticks between frames, to work out the page's tick rate
  uint32_t ticks_advanced;
  int last_tick;
  bool have_tick;
  int last_frame_number;
  bool page_visible;
  int last_marker;
  // The frame being decoded, built up until it is rendered
  uint64_t frame_partial_us;
  uint64_t frame_decoded_us;
  bool frame_pending;
  // Input lag: the press being waited for, and one whose marker was decoded
  // but not yet shown
  uint64_t press_sent_us;
  bool awaiting_marker;
  uint64_t lag_send_us;
  bool lag_pending;
  uint32_t presses_sent, presses_seen, presses_missed;
  // Cursor mode: which target the pointer was last sent to
  int cursor_target;
  // Every press, in order; presses_sent is the count
  press_record *presses;
  int press_capacity;
  // The latest frame number decoded, measuring or not
  int newest_frame;
  // Cursor mode: where the pointer is in the latest frame, measuring or not
  bool pointer_at[2];
} state;

// The input type for the run in progress, read by the decode thread
static volatile int active_input = 0;

static void series_init(series *s, int capacity) {
  s->values = malloc(sizeof(float) * capacity);
  s->count = 0;
  s->capacity = s->values ? capacity : 0;
}

static void series_free(series *s) {
  free(s->values);
  s->values = NULL;
  s->count = s->capacity = 0;
}

static void series_add(series *s, float value) {
  if (s->count < s->capacity) {
    s->values[s->count++] = value;
  }
}

static int compare_floats(const void *a, const void *b) {
  float x = *(const float *)a;
  float y = *(const float *)b;
  return (x > y) - (x < y);
}

// Sorts in place, so call only after measuring has stopped
static float series_percentile(series *s, float p) {
  if (s->count == 0) {
    return 0;
  }
  qsort(s->values, s->count, sizeof(float), compare_floats);
  int index = (int)(p * (s->count - 1) + 0.5f);
  return s->values[index];
}

static float series_max(series *s) {
  float max = 0;
  for (int i = 0; i < s->count; i++) {
    if (s->values[i] > max) {
      max = s->values[i];
    }
  }
  return max;
}

// Timing the Vita's own send path. The linker wraps two ENet calls (see
// --wrap in CMakeLists.txt), so without touching moonlight-common-c or ENet
// we can see when an input packet is handed to ENet, and when the next
// datagram goes onto the network. Inputs use their own ENet channels, so
// anything on the keyboard channel or above is input.
#define FIRST_INPUT_CHANNEL 0x02
enum {
  SEND_PROBE_IDLE,
  SEND_PROBE_WAITING_ENET,
  SEND_PROBE_WAITING_WIRE,
  SEND_PROBE_DONE,
};
static volatile int send_probe = SEND_PROBE_IDLE;
static volatile uint64_t send_call_us, send_enet_us, send_wire_us;
static series send_to_enet_ms, send_to_wire_ms;

int __real_enet_peer_send(ENetPeer *peer, enet_uint8 channel, ENetPacket *packet);
int __wrap_enet_peer_send(ENetPeer *peer, enet_uint8 channel, ENetPacket *packet) {
  if (send_probe == SEND_PROBE_WAITING_ENET && channel >= FIRST_INPUT_CHANNEL) {
    send_enet_us = LiGetMicroseconds();
    send_probe = SEND_PROBE_WAITING_WIRE;
  }
  return __real_enet_peer_send(peer, channel, packet);
}

int __real_enet_socket_send(ENetSocket socket, const ENetAddress *address, const ENetAddress *local,
                            const ENetBuffer *buffers, size_t count);
int __wrap_enet_socket_send(ENetSocket socket, const ENetAddress *address, const ENetAddress *local,
                            const ENetBuffer *buffers, size_t count) {
  int ret = __real_enet_socket_send(socket, address, local, buffers, count);
  if (send_probe == SEND_PROBE_WAITING_WIRE && ret > 0) {
    send_wire_us = LiGetMicroseconds();
    send_probe = SEND_PROBE_DONE;
  }
  return ret;
}

// Called with bench_mutex held before each press, and once at the end, to
// file the previous press's send timings
static void finish_send_probe(void) {
  if (state.presses_sent == 0 || state.presses_sent > state.press_capacity) {
    send_probe = SEND_PROBE_IDLE;
    return;
  }
  press_record *r = &state.presses[state.presses_sent - 1];
  int probe = send_probe;
  if (probe == SEND_PROBE_WAITING_WIRE || probe == SEND_PROBE_DONE) {
    r->to_enet_ms = (send_enet_us - send_call_us) / 1000.0f;
    series_add(&send_to_enet_ms, r->to_enet_ms);
  }
  if (probe == SEND_PROBE_DONE) {
    r->to_wire_ms = (send_wire_us - send_call_us) / 1000.0f;
    series_add(&send_to_wire_ms, r->to_wire_ms);
  }
  send_probe = SEND_PROBE_IDLE;
}

bool vitabench_init(void) {
  return pthread_mutex_init(&bench_mutex, NULL) == 0;
}

bool vitabench_active(void) {
  return phase != PHASE_IDLE;
}

// 4 for RGBA output, 1 when the probes read the Y plane of a YUV frame
static int probe_bpp = 4;

static int pixel_lum(const uint8_t *p) {
  // The channel order doesn't matter for telling black from white
  return probe_bpp == 1 ? p[0] : (p[0] + p[1] + p[2]) / 3;
}

// Average luminance of a 3x3 patch at a point given as fractions of the region
static int probe(const uint8_t *pixels, int stride_bytes, float x1, float y1, float x2, float y2,
                 float fx, float fy) {
  int cx = (int)(x1 + fx * (x2 - x1));
  int cy = (int)(y1 + fy * (y2 - y1));
  int total = 0;
  for (int dy = -1; dy <= 1; dy++) {
    for (int dx = -1; dx <= 1; dx++) {
      total += pixel_lum(pixels + (cy + dy) * stride_bytes + (cx + dx) * probe_bpp);
    }
  }
  return total / 9;
}

// Brightest pixel in a box given as fractions of the region; the pointer is
// small, so a whole box is scanned rather than a single point
static int probe_box_max(const uint8_t *pixels, int stride_bytes, float x1, float y1, float x2, float y2,
                         float fx, float fy, float fw, float fh) {
  int left = (int)(x1 + fx * (x2 - x1));
  int top = (int)(y1 + fy * (y2 - y1));
  int right = (int)(x1 + (fx + fw) * (x2 - x1));
  int bottom = (int)(y1 + (fy + fh) * (y2 - y1));
  int max = 0;
  for (int y = top; y <= bottom; y++) {
    const uint8_t *row = pixels + y * stride_bytes;
    for (int x = left; x <= right; x++) {
      int lum = pixel_lum(row + x * probe_bpp);
      if (lum > max) {
        max = lum;
      }
    }
  }
  return max;
}

// The decoder writes RGBA, 4 bytes per pixel, or in YUV mode NV21: the Y
// plane, then a half-height plane of interleaved V and U
static size_t frame_bytes(int stride_bytes, int texture_height) {
  size_t plane = (size_t)stride_bytes * texture_height;
  return probe_bpp == 1 ? plane + plane / 2 : plane;
}

static const char *frame_format(void) {
  return probe_bpp == 1 ? "nv21" : "rgba";
}

// Copies the frame if tick is one of the grab ticks, exactly, so every run
// grabs the same picture. A few MB, a few times per run.
static void grab_frame(int tick, const uint8_t *pixels, int stride_bytes, int texture_height,
                       float x1, float y1, float x2, float y2) {
  if (tick < GRAB_FIRST_TICK || (tick - GRAB_FIRST_TICK) % GRAB_TICK_STEP != 0) {
    return;
  }
  int slot = (tick - GRAB_FIRST_TICK) / GRAB_TICK_STEP;
  if (slot >= GRAB_COUNT || grabs[slot].data != NULL) {
    return;
  }
  size_t bytes = frame_bytes(stride_bytes, texture_height);
  frame_grab *g = &grabs[slot];
  g->data = malloc(bytes);
  if (g->data == NULL) {
    return;
  }
  memcpy(g->data, pixels, bytes);
  g->bytes = bytes;
  g->tick = tick;
  g->stride_bytes = stride_bytes;
  g->texture_height = texture_height;
  g->region[0] = x1;
  g->region[1] = y1;
  g->region[2] = x2;
  g->region[3] = y2;
}

static void free_grabs(void) {
  for (int i = 0; i < GRAB_COUNT; i++) {
    free(grabs[i].data);
    grabs[i].data = NULL;
  }
}

void vitabench_frame_decoded(const DECODE_UNIT *du, uint64_t submit_us, uint64_t hw_start_us, uint64_t decoded_us,
                             const uint8_t *pixels, int stride_bytes, int bytes_per_pixel, int texture_height,
                             float region_x1, float region_y1, float region_x2, float region_y2) {
  if (phase == PHASE_IDLE || pixels == NULL) {
    return;
  }
  probe_bpp = bytes_per_pixel;

  // Read the strip outside the lock; it's the only part that touches pixels
  int white = probe(pixels, stride_bytes, region_x1, region_y1, region_x2, region_y2,
                    BARCODE_X + BARCODE_BLOCK_W * 0.5f, BARCODE_CY);
  int black = probe(pixels, stride_bytes, region_x1, region_y1, region_x2, region_y2,
                    BARCODE_X + BARCODE_BLOCK_W * 1.5f, BARCODE_CY);
  bool readable = white - black >= MIN_REFERENCE_CONTRAST;
  int threshold = (white + black) / 2;
  int tick = 0;
  int marker = 0;
  if (readable) {
    for (int i = 0; i < FRAME_BITS; i++) {
      int lum = probe(pixels, stride_bytes, region_x1, region_y1, region_x2, region_y2,
                      BARCODE_X + BARCODE_BLOCK_W * (i + 2.5f), BARCODE_CY);
      tick = (tick << 1) | (lum > threshold ? 1 : 0);
    }
    marker = probe(pixels, stride_bytes, region_x1, region_y1, region_x2, region_y2,
                   MARKER_CX, MARKER_CY) > threshold;
  }
  // Cursor mode works on the page or on any dark screen, such as a black
  // window on the Desktop, so it doesn't need the reference blocks
  bool cursor_at[2] = {false, false};
  int cursor_lum[2] = {0, 0};
  if (active_input == INPUT_CURSOR) {
    int cursor_threshold = readable ? threshold : CURSOR_FIXED_THRESHOLD;
    for (int i = 0; i < 2; i++) {
      cursor_lum[i] = probe_box_max(pixels, stride_bytes, region_x1, region_y1, region_x2, region_y2,
                                    CURSOR_TARGET_X[i], CURSOR_TARGET_Y, CURSOR_BOX_W, CURSOR_BOX_H);
      cursor_at[i] = cursor_lum[i] > cursor_threshold;
    }
  }

  pthread_mutex_lock(&bench_mutex);
  state.page_visible = readable;
  state.newest_frame = du->frameNumber;
  state.pointer_at[0] = cursor_at[0];
  state.pointer_at[1] = cursor_at[1];
  if (phase == PHASE_MEASURING) {
    state.frames_received++;
    state.bytes += du->fullLength;
    if (du->frameType == FRAME_TYPE_IDR) {
      state.idr_frames++;
    }
    if (state.last_frame_number > 0 && du->frameNumber > state.last_frame_number + 1) {
      state.frames_lost += du->frameNumber - state.last_frame_number - 1;
      state.freezes++;
    }
    state.last_frame_number = du->frameNumber;

    uint64_t network_us = du->enqueueTimeUs >= du->receiveTimeUs ? du->enqueueTimeUs - du->receiveTimeUs : 0;
    uint64_t queue_us = submit_us >= du->enqueueTimeUs ? submit_us - du->enqueueTimeUs : 0;
    uint64_t host_us = (uint64_t)du->frameHostProcessingLatency * 100;
    uint64_t decode_us = decoded_us - submit_us;
    series_add(&state.network_ms, network_us / 1000.0f);
    series_add(&state.queue_ms, queue_us / 1000.0f);
    series_add(&state.decode_ms, decode_us / 1000.0f);
    if (hw_start_us >= submit_us && decoded_us >= hw_start_us) {
      series_add(&state.assemble_ms, (hw_start_us - submit_us) / 1000.0f);
      series_add(&state.hw_decode_ms, (decoded_us - hw_start_us) / 1000.0f);
    }
    state.frame_partial_us = host_us + network_us + queue_us + decode_us;
    state.frame_decoded_us = decoded_us;
    state.frame_pending = true;

    if (readable) {
      state.ticks_read++;
      if (state.have_tick) {
        int step = (tick - state.last_tick) & TICK_MASK;
        if (step == 0) {
          state.ticks_repeated++;
        } else if (step < (1 << (FRAME_BITS - 1))) {
          state.ticks_skipped += step - 1;
          state.ticks_advanced += step;
        } else {
          state.ticks_backwards++;
        }
      }
      state.last_tick = tick;
      state.have_tick = true;

      if (config.bench_grab) {
        grab_frame(tick, pixels, stride_bytes, texture_height, region_x1, region_y1, region_x2, region_y2);
      }

      if (active_input != INPUT_CURSOR && marker && state.last_marker == 0 && state.awaiting_marker) {
        // The marker turning white after a press means the press got through
        state.awaiting_marker = false;
        state.lag_send_us = state.press_sent_us;
        state.lag_pending = true;
        state.presses_seen++;
        if (state.presses_sent > 0 && state.presses_sent <= state.press_capacity) {
          press_record *r = &state.presses[state.presses_sent - 1];
          r->seen_frame = du->frameNumber;
          r->lum_target = 255 * marker;
        }
      }
      state.last_marker = marker;
    } else {
      state.ticks_unreadable++;
    }

    if (active_input == INPUT_CURSOR) {
      // The pointer showing at the new target, and gone from the old one
      int target = state.cursor_target;
      if (state.awaiting_marker && cursor_at[target] && !cursor_at[1 - target]) {
        state.awaiting_marker = false;
        state.lag_send_us = state.press_sent_us;
        state.lag_pending = true;
        state.presses_seen++;
        if (state.presses_sent > 0 && state.presses_sent <= state.press_capacity) {
          press_record *r = &state.presses[state.presses_sent - 1];
          r->seen_frame = du->frameNumber;
          r->lum_target = cursor_lum[target];
          r->lum_other = cursor_lum[1 - target];
        }
      }
    }
  }
  pthread_mutex_unlock(&bench_mutex);
}

void vitabench_frame_rendered(uint64_t rendered_us) {
  if (phase != PHASE_MEASURING) {
    return;
  }
  pthread_mutex_lock(&bench_mutex);
  state.frames_rendered++;
  state.present_pending_us = rendered_us;
  state.present_pending_count++;
  if (state.frame_pending) {
    uint64_t render_us = rendered_us - state.frame_decoded_us;
    series_add(&state.render_ms, render_us / 1000.0f);
    series_add(&state.pipeline_ms, (state.frame_partial_us + render_us) / 1000.0f);
    state.frame_pending = false;
  }
  // Input lag ends when the frame showing the marker is on screen. A frame
  // dropped by the pacer is completed by the next one that is shown.
  if (state.lag_pending) {
    float lag_ms = (rendered_us - state.lag_send_us) / 1000.0f;
    series_add(&state.input_lag_ms, lag_ms);
    if (state.presses_sent > 0 && state.presses_sent <= state.press_capacity) {
      state.presses[state.presses_sent - 1].lag_ms = lag_ms;
    }
    state.lag_pending = false;
  }
  pthread_mutex_unlock(&bench_mutex);
}

// Wakes at every screen refresh while measuring, and times how long the last
// frame handed to the display waited for it. It counts from the swap call, so
// it includes the GPU finishing the frame (~0.3 ms); a frame the GPU finishes
// after the refresh really waits one more, which this doesn't see.
static void *vblank_thread(void *arg) {
  (void)arg;
  while (phase == PHASE_MEASURING) {
    sceDisplayWaitVblankStart();
    uint64_t now_us = LiGetMicroseconds();
    pthread_mutex_lock(&bench_mutex);
    if (state.present_pending_count > 0) {
      series_add(&state.vblank_wait_ms, (now_us - state.present_pending_us) / 1000.0f);
      state.frames_replaced += state.present_pending_count - 1;
      state.present_pending_count = 0;
    }
    pthread_mutex_unlock(&bench_mutex);
  }
  return NULL;
}

static void reset_measurements(void) {
  pthread_mutex_lock(&bench_mutex);
  state.decode_ms.count = state.network_ms.count = state.queue_ms.count = 0;
  state.assemble_ms.count = state.hw_decode_ms.count = 0;
  state.render_ms.count = state.pipeline_ms.count = state.input_lag_ms.count = 0;
  state.vblank_wait_ms.count = 0;
  state.frames_replaced = 0;
  state.present_pending_count = 0;
  state.frames_received = state.frames_rendered = state.frames_lost = state.idr_frames = 0;
  state.freezes = 0;
  state.bytes = 0;
  state.ticks_read = state.ticks_unreadable = state.ticks_repeated = 0;
  state.ticks_skipped = state.ticks_backwards = 0;
  state.ticks_advanced = 0;
  state.have_tick = false;
  state.last_frame_number = 0;
  state.last_marker = -1;
  state.frame_pending = false;
  state.awaiting_marker = false;
  state.lag_pending = false;
  state.presses_sent = state.presses_seen = state.presses_missed = 0;
  send_to_enet_ms.count = send_to_wire_ms.count = 0;
  send_probe = SEND_PROBE_IDLE;
  pthread_mutex_unlock(&bench_mutex);
}

static void send_key(short key) {
  LiSendKeyboardEvent(key, KEY_ACTION_DOWN, 0);
  sceKernelDelayThread(KEY_HOLD_US);
  LiSendKeyboardEvent(key, KEY_ACTION_UP, 0);
}

// Which input path the timed presses use, so they can be compared: a key goes
// through the host's keyboard injection, a click through its mouse, and a
// button through its virtual gamepad
static int bench_input_type(void) {
  if (config.bench_input && strcmp(config.bench_input, "mouse") == 0) {
    return INPUT_MOUSE;
  }
  if (config.bench_input && strcmp(config.bench_input, "gamepad") == 0) {
    return INPUT_GAMEPAD;
  }
  if (config.bench_input && strcmp(config.bench_input, "cursor") == 0) {
    return INPUT_CURSOR;
  }
  if (config.bench_input && strcmp(config.bench_input, "none") == 0) {
    return INPUT_NONE;
  }
  return INPUT_KEY;
}

static const char *bench_input_name(int type) {
  return type == INPUT_MOUSE ? "mouse" : type == INPUT_GAMEPAD ? "gamepad"
       : type == INPUT_CURSOR ? "cursor" : type == INPUT_NONE ? "none" : "key";
}

static void move_cursor_to(int target) {
  LiSendMousePositionEvent((short)(CURSOR_TARGET_X[target] * MOUSE_REFERENCE),
                           (short)(CURSOR_TARGET_Y * MOUSE_REFERENCE), MOUSE_REFERENCE, MOUSE_REFERENCE);
}

static void press_down(int type) {
  if (type == INPUT_CURSOR) {
    // Called with bench_mutex held, so the target and the send stay together
    state.cursor_target = 1 - state.cursor_target;
    move_cursor_to(state.cursor_target);
  } else if (type == INPUT_MOUSE) {
    LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT);
  } else if (type == INPUT_GAMEPAD) {
    LiSendMultiControllerEvent(0, 1, A_FLAG, 0, 0, 0, 0, 0, 0);
  } else {
    LiSendKeyboardEvent(VK_SPACE, KEY_ACTION_DOWN, 0);
  }
}

static void press_up(int type) {
  if (type == INPUT_CURSOR) {
    // Nothing to release
  } else if (type == INPUT_MOUSE) {
    LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
  } else if (type == INPUT_GAMEPAD) {
    LiSendMultiControllerEvent(0, 1, 0, 0, 0, 0, 0, 0, 0);
  } else {
    LiSendKeyboardEvent(VK_SPACE, KEY_ACTION_UP, 0);
  }
}

// For unattended runs only, which have no host menu to start from: the host
// named in bench_host, or the first paired one
static device_info_t *pick_host_for_automation(void) {
  for (int i = 0; i < known_devices.count; i++) {
    device_info_t *d = &known_devices.devices[i];
    if (!d->paired) {
      continue;
    }
    if (config.bench_host == NULL || config.bench_host[0] == '\0' || strcmp(d->name, config.bench_host) == 0) {
      return d;
    }
  }
  return NULL;
}

static bool wait_for(bool (*condition)(void), uint64_t timeout_us) {
  uint64_t start = LiGetMicroseconds();
  while (!condition()) {
    if (LiGetMicroseconds() - start > timeout_us) {
      return false;
    }
    sceKernelDelayThread(POLL_US * 10);
  }
  return true;
}

static bool is_connected(void) {
  return connection_get_status() == LI_CONNECTED;
}

static bool pointer_parked(void) {
  pthread_mutex_lock(&bench_mutex);
  bool parked = state.pointer_at[0] && !state.pointer_at[1];
  pthread_mutex_unlock(&bench_mutex);
  return parked;
}

static bool page_is_visible(void) {
  pthread_mutex_lock(&bench_mutex);
  bool visible = state.page_visible;
  pthread_mutex_unlock(&bench_mutex);
  return visible;
}

typedef struct {
  double rtt_sum;
  uint32_t rtt_max;
  // The round trip's variation, as common-c estimates it: a measure of jitter
  double rtt_var_sum;
  uint32_t rtt_var_max;
  int rtt_samples;
  // Video and error-correction packets the host announced while measuring
  // (common-c's counters), and those it sent and that arrived, from the RTP
  // sequence numbers (src/netcount.c)
  RTP_VIDEO_STATS rtp_start, rtp_end;
  // The battery while measuring, to compare settings by power use. Only
  // meaningful on battery: charging or a charger plugged in spoils it.
  int battery_mah_start, battery_mah_end;
  int battery_percent_start, battery_percent_end;
  int battery_mv_start, battery_mv_end;
  bool on_charger;
  netcount net_start, net_end;
  int rssi_sum;
  int rssi_min;
  int rssi_samples;
  int channel;
  int device; // SceNetCtlInfo.device: the network interface type, unnamed in VitaSDK
} link_samples;

static void sample_link(link_samples *link) {
  uint32_t rtt = 0, variance = 0;
  if (LiGetEstimatedRttInfo(&rtt, &variance)) {
    link->rtt_sum += rtt;
    link->rtt_max = rtt > link->rtt_max ? rtt : link->rtt_max;
    link->rtt_var_sum += variance;
    link->rtt_var_max = variance > link->rtt_var_max ? variance : link->rtt_var_max;
    link->rtt_samples++;
  }
  SceNetCtlInfo info;
  if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_RSSI_PERCENTAGE, &info) >= 0) {
    int rssi = info.rssi_percentage;
    link->rssi_sum += rssi;
    link->rssi_min = link->rssi_samples == 0 || rssi < link->rssi_min ? rssi : link->rssi_min;
    link->rssi_samples++;
  }
  if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_CHANNEL, &info) >= 0) {
    link->channel = info.channel;
  }
  if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_DEVICE, &info) >= 0) {
    link->device = (int)info.device;
  }
}

// JSON strings here are app and host names, so escape the few characters that matter
static void write_json_string(FILE *f, const char *s) {
  fputc('"', f);
  for (; s && *s; s++) {
    if (*s == '"' || *s == '\\') {
      fputc('\\', f);
      fputc(*s, f);
    } else if ((unsigned char)*s < 0x20) {
      fputc(' ', f);
    } else {
      fputc(*s, f);
    }
  }
  fputc('"', f);
}

static void write_timing(FILE *f, const char *name, series *s, bool last) {
  fprintf(f, "  \"%s\": {\"p50\": %.2f, \"p95\": %.2f, \"max\": %.2f}%s\n", name,
          series_percentile(s, 0.5f), series_percentile(s, 0.95f), series_max(s), last ? "" : ",");
}

typedef struct {
  char path[REPORT_PATH_MAX];
  float seconds;
  float input_lag_p50, input_lag_p95, latency_p50;
  float fps_rendered;
} report_summary;

static void write_report(FILE *f, const char *started, const char *host, const char *app, const char *error,
                         float seconds, const link_samples *link, report_summary *summary) {
  float one_way_ms = link->rtt_samples ? (float)(link->rtt_sum / link->rtt_samples) / 2.0f : 0;
  summary->seconds = seconds;
  summary->fps_rendered = seconds > 0 ? state.frames_rendered / seconds : 0;

  fprintf(f, "{\n");
  fprintf(f, "  \"format\": 1,\n");
  fprintf(f, "  \"version\": \"%d.%d.%d\",\n", VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);
  fprintf(f, "  \"label\": ");
  write_json_string(f, config.bench_label);
  fprintf(f, ",\n  \"started\": \"%s\",\n", started);
  fprintf(f, "  \"ok\": %s,\n  \"error\": ", error[0] ? "false" : "true");
  write_json_string(f, error);
  fprintf(f, ",\n  \"host\": ");
  write_json_string(f, host);
  fprintf(f, ",\n  \"app\": ");
  write_json_string(f, app);
  fprintf(f, ",\n  \"seconds\": %.1f,\n", seconds);
  fprintf(f, "  \"settings\": {\"width\": %d, \"height\": %d, \"fps\": %d, \"bitrate_kbps\": %d, "
             "\"packet_size\": %d, \"direct_decode\": %s, \"input_sync\": %s, \"boost_clocks\": %s, "
             "\"frame_pacer\": %s, \"vblank_wait\": %s, \"stats_overlay\": %d, \"debug_log\": %s, \"decode_yuv\": %s, "
             "\"pin_threads\": %s},\n",
          config.stream.width, config.stream.height, config.stream.fps, config.stream.bitrate,
          config.stream.packetSize, config.direct_decode ? "true" : "false",
          config.input_sync_to_controller ? "true" : "false", config.boost_clocks ? "true" : "false",
          config.enable_frame_pacer ? "true" : "false", config.enable_vita_vblank_wait ? "true" : "false",
          config.stats_overlay, config.save_debug_log ? "true" : "false", config.decode_yuv ? "true" : "false",
          config.pin_threads ? "true" : "false");
  fprintf(f, "  \"frames\": {\"received\": %u, \"rendered\": %u, \"fps_received\": %.2f, \"fps_rendered\": %.2f, "
             "\"lost_network\": %u, \"idr\": %u, \"replaced_before_shown\": %u, \"freezes\": %u},\n",
          state.frames_received, state.frames_rendered,
          seconds > 0 ? state.frames_received / seconds : 0, summary->fps_rendered,
          state.frames_lost, state.idr_frames, state.frames_replaced, state.freezes);
  // The page ticks at its own rate (60 by default), so a slower stream skips
  // ticks even when nothing is lost. Work out how many, and report the rest.
  float tick_rate = seconds > 0 ? state.ticks_advanced / seconds : 0;
  float expected_skips = tick_rate > config.stream.fps ? (tick_rate - config.stream.fps) * seconds : 0;
  float beyond_rate = state.ticks_skipped > expected_skips ? state.ticks_skipped - expected_skips : 0;
  fprintf(f, "  \"page\": {\"ticks_read\": %u, \"unreadable\": %u, \"repeated\": %u, \"skipped\": %u, "
             "\"backwards\": %u, \"tick_rate\": %.2f, \"expected_skips_from_rate\": %.0f, \"skipped_beyond_rate\": %.0f},\n",
          state.ticks_read, state.ticks_unreadable, state.ticks_repeated, state.ticks_skipped, state.ticks_backwards,
          tick_rate, expected_skips, beyond_rate);
  fprintf(f, "  \"bitrate_mbps\": %.2f,\n", seconds > 0 ? state.bytes * 8 / seconds / 1000000.0f : 0);

  // Latency estimate: the per-frame pipeline plus half the round trip
  float pipeline_p50 = series_percentile(&state.pipeline_ms, 0.5f);
  float pipeline_p95 = series_percentile(&state.pipeline_ms, 0.95f);
  summary->latency_p50 = pipeline_p50 + one_way_ms;
  fprintf(f, "  \"latency_estimate_ms\": {\"p50\": %.2f, \"p95\": %.2f, \"max\": %.2f},\n",
          summary->latency_p50, pipeline_p95 + one_way_ms, series_max(&state.pipeline_ms) + one_way_ms);

  summary->input_lag_p50 = series_percentile(&state.input_lag_ms, 0.5f);
  summary->input_lag_p95 = series_percentile(&state.input_lag_ms, 0.95f);
  float lag_min = state.input_lag_ms.count ? state.input_lag_ms.values[0] : 0;
  fprintf(f, "  \"input_lag_ms\": {\"input\": \"%s\", \"presses\": %u, \"seen\": %u, \"missed\": %u, \"p50\": %.2f, \"p95\": %.2f, "
             "\"min\": %.2f, \"max\": %.2f},\n",
          bench_input_name(bench_input_type()), state.presses_sent, state.presses_seen, state.presses_missed,
          summary->input_lag_p50, summary->input_lag_p95, lag_min, series_max(&state.input_lag_ms));

  // The Vita's own send path for the timed presses
  fprintf(f, "  \"send_ms\": {\"samples\": %d, \"to_enet_p50\": %.2f, \"to_enet_p95\": %.2f, "
             "\"to_wire_p50\": %.2f, \"to_wire_p95\": %.2f, \"to_wire_max\": %.2f},\n",
          send_to_wire_ms.count,
          series_percentile(&send_to_enet_ms, 0.5f), series_percentile(&send_to_enet_ms, 0.95f),
          series_percentile(&send_to_wire_ms, 0.5f), series_percentile(&send_to_wire_ms, 0.95f),
          series_max(&send_to_wire_ms));
  write_timing(f, "network_ms", &state.network_ms, false);
  write_timing(f, "queue_ms", &state.queue_ms, false);
  write_timing(f, "decode_ms", &state.decode_ms, false);
  write_timing(f, "assemble_ms", &state.assemble_ms, false);
  write_timing(f, "hw_decode_ms", &state.hw_decode_ms, false);
  write_timing(f, "render_ms", &state.render_ms, false);
  write_timing(f, "vblank_wait_ms", &state.vblank_wait_ms, false);
  fprintf(f, "  \"rtt_ms\": {\"avg\": %.2f, \"max\": %u, \"variance_avg\": %.2f, \"variance_max\": %u},\n",
          link->rtt_samples ? link->rtt_sum / link->rtt_samples : 0.0, link->rtt_max,
          link->rtt_samples ? link->rtt_var_sum / link->rtt_samples : 0.0, link->rtt_var_max);
  // What actually crossed the Wi-Fi, error correction included (payload only,
  // without IP, UDP and RTP headers), against bitrate_mbps, the video alone
  // common-c only keeps recovered and failed counts for audio, so loss comes
  // from gaps in the RTP sequence numbers (src/netcount.c)
  uint32_t video_packets = link->rtp_end.packetCountVideo - link->rtp_start.packetCountVideo;
  uint32_t fec_packets = link->rtp_end.packetCountFec - link->rtp_start.packetCountFec;
  uint32_t received = link->net_end.received - link->net_start.received;
  uint32_t lost = netcount_lost(&link->net_start, &link->net_end);
  uint32_t late = link->net_end.late - link->net_start.late;
  uint32_t repeats = link->net_end.repeats - link->net_start.repeats;
  fprintf(f, "  \"packets\": {\"video\": %u, \"fec\": %u, \"received\": %u, \"lost\": %u, \"late\": %u, "
             "\"repeats\": %u, \"loss_percent\": %.3f, \"payload_mbps\": %.2f, \"idr_requests_per_s\": %d},\n",
          video_packets, fec_packets, received, lost, late, repeats,
          received + lost ? 100.0f * lost / (received + lost) : 0.0f,
          seconds > 0 ? received * (double)config.stream.packetSize * 8 / seconds / 1000000 : 0.0,
          config.bench_idr_hz);
  // Each press, so a strange result can be traced frame by frame
  fprintf(f, "  \"presses\": [");
  int recorded = state.presses_sent < (uint32_t)state.press_capacity ? (int)state.presses_sent : state.press_capacity;
  for (int i = 0; i < recorded; i++) {
    const press_record *r = &state.presses[i];
    fprintf(f, "%s\n    {\"target\": %d, \"sent_frame\": %d, \"seen_frame\": %d, \"lum\": [%d, %d], "
               "\"to_enet_ms\": %.2f, \"to_wire_ms\": %.2f, \"lag_ms\": %.2f}",
            i ? "," : "", r->target, r->sent_frame, r->seen_frame, r->lum_target, r->lum_other,
            r->to_enet_ms, r->to_wire_ms, r->lag_ms);
  }
  fprintf(f, "\n  ],\n");
  fprintf(f, "  \"grabs\": [");
  for (int i = 0, written = 0; i < GRAB_COUNT; i++) {
    const frame_grab *g = &grabs[i];
    if (g->data != NULL) {
      fprintf(f, "%s\n    {\"tick\": %d, \"file\": \"grab-%d.raw\", \"format\": \"%s\", \"stride\": %d, "
                 "\"height\": %d, \"region\": [%.0f, %.0f, %.0f, %.0f]}",
              written++ ? "," : "", g->tick, g->tick, frame_format(), g->stride_bytes, g->texture_height,
              g->region[0], g->region[1], g->region[2], g->region[3]);
    }
  }
  fprintf(f, "\n  ],\n");
  // Average current from the capacity used, and power from that at the mean voltage
  int used_mah = link->battery_mah_start - link->battery_mah_end;
  float hours = seconds / 3600.0f;
  float drain_ma = hours > 0 ? used_mah / hours : 0;
  float drain_mw = drain_ma * (link->battery_mv_start + link->battery_mv_end) / 2000.0f;
  fprintf(f, "  \"battery\": {\"mah_start\": %d, \"mah_end\": %d, \"percent_start\": %d, \"percent_end\": %d, "
             "\"drain_ma\": %.0f, \"drain_mw\": %.0f, \"on_charger\": %s},\n",
          link->battery_mah_start, link->battery_mah_end, link->battery_percent_start, link->battery_percent_end,
          drain_ma, drain_mw, link->on_charger ? "true" : "false");
  fprintf(f, "  \"wifi\": {\"rssi_avg_percent\": %d, \"rssi_min_percent\": %d, \"channel\": %d, \"device\": %d}\n",
          link->rssi_samples ? link->rssi_sum / link->rssi_samples : -1,
          link->rssi_samples ? link->rssi_min : -1, link->channel, link->device);
  fprintf(f, "}\n");
}

// Writes the report to a temporary file and then moves it into place, so a
// tool polling over FTP never reads a half-written report. The old file is
// removed first, since a rename over an existing file can fail on the Vita.
static void write_report_file(const char *path, const char *started, const char *host, const char *app,
                              const char *error, float seconds, const link_samples *link,
                              report_summary *summary) {
  char tmp[REPORT_PATH_MAX + 48];
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  FILE *f = fopen(tmp, "w");
  if (f == NULL) {
    return;
  }
  write_report(f, started, host, app, error, seconds, link, summary);
  fclose(f);
  sceIoRemove(path);
  sceIoRename(tmp, path);
}

// Written before the report, so a report that lists a grab means it's saved
static void save_grabs(const char *dir) {
  char path[REPORT_PATH_MAX + 32];
  for (int i = 0; i < GRAB_COUNT; i++) {
    frame_grab *g = &grabs[i];
    snprintf(path, sizeof(path), "%s/grab-%d.raw", dir, GRAB_FIRST_TICK + i * GRAB_TICK_STEP);
    sceIoRemove(path);
    if (g->data == NULL) {
      continue;
    }
    FILE *f = fopen(path, "wb");
    bool written = f != NULL && fwrite(g->data, 1, g->bytes, f) == g->bytes;
    if (f != NULL) {
      fclose(f);
    }
    if (!written) {
      free(g->data);
      g->data = NULL;
    }
  }
}

static void save_report(const char *started_compact, const char *started, const char *host, const char *app,
                        const char *error, float seconds, const link_samples *link, report_summary *summary) {
  char dir[REPORT_PATH_MAX];
  snprintf(dir, sizeof(dir), "%sreports", config.key_dir);
  sceIoMkdir(dir, 0777);
  snprintf(summary->path, sizeof(summary->path), "%.*s/bench-%s.json",
           (int)(sizeof(summary->path) - 64), dir, started_compact);

  save_grabs(dir);
  write_report_file(summary->path, started, host, app, error, seconds, link, summary);
  // A fixed name too, so tools can always find the latest run. write_report
  // sorts the series in place, which the first write already did.
  char latest[REPORT_PATH_MAX + 32];
  snprintf(latest, sizeof(latest), "%s/last-bench.json", dir);
  write_report_file(latest, started, host, app, error, seconds, link, summary);
  vita_debug_log("[BENCH] report written to %s", summary->path);
}

int vitabench_run(bool automated) {
  char error[512] = "";
  // Declared up here, since the failure paths jump past where it starts
  pthread_t vblank_tid;
  bool vblank_started = false;
  // Sized for any year the RTC can report
  char started[48];
  char started_compact[48];
  SceDateTime now;
  sceRtcGetCurrentClockLocalTime(&now);
  snprintf(started, sizeof(started), "%04d-%02d-%02dT%02d:%02d:%02d",
           now.year, now.month, now.day, now.hour, now.minute, now.second);
  snprintf(started_compact, sizeof(started_compact), "%04d%02d%02d-%02d%02d%02d",
           now.year, now.month, now.day, now.hour, now.minute, now.second);

  link_samples link = {0};
  float measured_seconds = 0;
  bool streamed = false;

  int capacity = (config.bench_seconds + 5) * MAX_FPS_RECORDED;
  series_init(&state.decode_ms, capacity);
  series_init(&state.assemble_ms, capacity);
  series_init(&state.hw_decode_ms, capacity);
  series_init(&state.network_ms, capacity);
  series_init(&state.queue_ms, capacity);
  series_init(&state.render_ms, capacity);
  series_init(&state.vblank_wait_ms, capacity);
  series_init(&state.pipeline_ms, capacity);
  series_init(&state.input_lag_ms, config.bench_seconds + 5);
  series_init(&send_to_enet_ms, config.bench_seconds + 8);
  series_init(&send_to_wire_ms, config.bench_seconds + 8);
  state.press_capacity = config.bench_seconds + 8;
  state.presses = calloc(state.press_capacity, sizeof(press_record));
  if (state.presses == NULL) {
    state.press_capacity = 0;
  }
  free_grabs();
  reset_measurements();

  char app_name[256] = "";
  if (automated) {
    device_info_t *host = pick_host_for_automation();
    if (host == NULL) {
      snprintf(error, sizeof(error), "No paired computer%s%s",
               config.bench_host[0] ? " called " : "", config.bench_host);
      goto done;
    }
    flash_message("Benchmark: connecting to %s...", host->name);
    if (session_open_headless(host, error, sizeof(error)) != 0) {
      goto done;
    }
  } else if (!session_server_ready()) {
    snprintf(error, sizeof(error), "Connect to a computer first, then open Settings from its menu.");
    goto done;
  }

  // The benchmark only makes sense against the benchmark page, so make sure
  // the host has an app for it before streaming anything
  int app_id = session_find_bench_app(config.bench_app, app_name, sizeof(app_name));
  if (app_id < 0) {
    snprintf(error, sizeof(error),
             "%s has no benchmark app. Add one called \"%s\" in Sunshine that opens "
             "tools/bench/bench.html (see tools/bench/README.md).",
             session_server_name(), config.bench_app);
    goto done;
  }

  vita_debug_log("[BENCH] starting %s on %s for %d s", app_name, session_server_name(), config.bench_seconds);
  flash_message("Benchmark: starting %s on %s...", app_name, session_server_name());

  // Probe from the first frame, so the page can be detected
  phase = PHASE_WARMUP;
  if (session_start_app_by_id(app_id, error, sizeof(error)) != 0) {
    goto done;
  }
  streamed = true;
  if (!wait_for(is_connected, CONNECT_TIMEOUT_US)) {
    snprintf(error, sizeof(error), "The stream didn't start within %llu s", CONNECT_TIMEOUT_US / 1000000);
    goto done;
  }
  int input_type = bench_input_type();
  if (input_type == INPUT_CURSOR || input_type == INPUT_NONE) {
    // The pointer test needs only a dark screen, and none needs nothing, so
    // don't wait for the page or reset it; just let the stream settle
    sceKernelDelayThread(RESET_SETTLE_US * 2);
  } else {
    if (!wait_for(page_is_visible, PAGE_TIMEOUT_US)) {
      snprintf(error, sizeof(error), "The benchmark page didn't appear within %llu s. Is %s showing bench.html?",
               PAGE_TIMEOUT_US / 1000000, app_name);
      goto done;
    }
    // Restart the page's animation, so every run shows the same pictures
    send_key(VK_R);
    sceKernelDelayThread(RESET_SETTLE_US);
  }
  reset_measurements();

  active_input = input_type;
  if (input_type == INPUT_CURSOR) {
    // Park the pointer on the first target, and wait until it's actually seen
    // there. Otherwise the first timed move can be 'seen' a frame later
    // because the pointer was already on its way, an impossibly short lag.
    state.cursor_target = 0;
    move_cursor_to(0);
    if (!wait_for(pointer_parked, CURSOR_PARK_TIMEOUT_US)) {
      snprintf(error, sizeof(error),
               "The mouse pointer wasn't seen at the top right of the screen. Is the screen dark "
               "there, with Windows' standard white pointer?");
      goto done;
    }
    sceKernelDelayThread(RESET_SETTLE_US);
  }
  const RTP_VIDEO_STATS *rtp = LiGetRTPVideoStats();
  if (rtp != NULL) {
    link.rtp_start = *rtp;
  }
  netcount_read(&link.net_start);
  link.battery_mah_start = scePowerGetBatteryRemainCapacity();
  link.battery_percent_start = scePowerGetBatteryLifePercent();
  link.battery_mv_start = scePowerGetBatteryVolt();
  link.on_charger = scePowerIsPowerOnline() || scePowerIsBatteryCharging();
  phase = PHASE_MEASURING;
  vblank_started = pthread_create(&vblank_tid, NULL, vblank_thread, NULL) == 0;
  uint64_t start = LiGetMicroseconds();
  uint64_t end = start + (uint64_t)config.bench_seconds * 1000000;
  uint64_t next_press = start + 500000;
  uint64_t next_sample = start;
  uint64_t next_idr = start;
  uint64_t t;
  while ((t = LiGetMicroseconds()) < end) {
    if (connection_get_status() != LI_CONNECTED) {
      snprintf(error, sizeof(error), "The stream ended early, after %.0f s", (t - start) / 1000000.0f);
      break;
    }
    if (t >= next_sample) {
      sample_link(&link);
      next_sample += 1000000;
    }
    // Keyframes are whole pictures, so even a still screen sends real-sized
    // bursts, which loads the Wi-Fi like a game would
    if (config.bench_idr_hz > 0 && t >= next_idr) {
      LiRequestIdrFrame();
      next_idr += 1000000 / config.bench_idr_hz;
    }
    if (input_type != INPUT_NONE && t >= next_press) {
      pthread_mutex_lock(&bench_mutex);
      if (state.awaiting_marker && t - state.press_sent_us > PRESS_TIMEOUT_US) {
        state.presses_missed++;
        state.awaiting_marker = false;
      }
      bool waiting = state.awaiting_marker;
      pthread_mutex_unlock(&bench_mutex);
      if (!waiting) {
        pthread_mutex_lock(&bench_mutex);
        finish_send_probe();
        state.press_sent_us = LiGetMicroseconds();
        send_call_us = state.press_sent_us;
        send_probe = SEND_PROBE_WAITING_ENET;
        press_down(input_type);
        state.awaiting_marker = true;
        if (state.presses_sent < state.press_capacity) {
          state.presses[state.presses_sent] = (press_record){
            .target = state.cursor_target, .sent_frame = state.newest_frame,
            .seen_frame = -1, .lum_target = -1, .lum_other = -1, .lag_ms = -1,
            .to_enet_ms = -1, .to_wire_ms = -1,
          };
        }
        state.presses_sent++;
        pthread_mutex_unlock(&bench_mutex);
        sceKernelDelayThread(KEY_HOLD_US);
        press_up(input_type);
      }
      next_press += PRESS_INTERVAL_US;
    }
    sceKernelDelayThread(POLL_US);
  }
  measured_seconds = (LiGetMicroseconds() - start) / 1000000.0f;
  rtp = LiGetRTPVideoStats();
  if (rtp != NULL) {
    link.rtp_end = *rtp;
  }
  netcount_read(&link.net_end);
  link.battery_mah_end = scePowerGetBatteryRemainCapacity();
  link.battery_percent_end = scePowerGetBatteryLifePercent();
  link.battery_mv_end = scePowerGetBatteryVolt();
  link.on_charger |= scePowerIsPowerOnline() || scePowerIsBatteryCharging();

done:
  phase = PHASE_IDLE;
  if (vblank_started) {
    // It notices the phase change at the next refresh, within ~17 ms
    pthread_join(vblank_tid, NULL);
  }
  pthread_mutex_lock(&bench_mutex);
  finish_send_probe();
  if (state.awaiting_marker) {
    state.presses_missed++;
    state.awaiting_marker = false;
  }
  pthread_mutex_unlock(&bench_mutex);

  if (streamed) {
    connection_terminate();
    if (config.bench_quit_app) {
      session_quit_app();
    }
    session_return_to_server();
  }

  report_summary summary = {0};
  save_report(started_compact, started, session_server_name(), app_name, error, measured_seconds, &link, &summary);
  if (error[0]) {
    vita_debug_log("[BENCH] failed: %s", error);
  } else {
    vita_debug_log("[BENCH] done: input lag p50 %.1f ms p95 %.1f ms, latency estimate p50 %.1f ms, %.1f fps",
                   summary.input_lag_p50, summary.input_lag_p95, summary.latency_p50, summary.fps_rendered);
  }

  series_free(&state.decode_ms);
  series_free(&state.assemble_ms);
  series_free(&state.hw_decode_ms);
  series_free(&state.network_ms);
  series_free(&state.queue_ms);
  series_free(&state.render_ms);
  series_free(&state.vblank_wait_ms);
  series_free(&state.pipeline_ms);
  series_free(&state.input_lag_ms);
  series_free(&send_to_enet_ms);
  series_free(&send_to_wire_ms);
  free(state.presses);
  state.presses = NULL;
  free_grabs();
  state.press_capacity = 0;

  if (!automated) {
    if (error[0]) {
      display_error("Benchmark failed:\n%s", error);
    } else {
      display_error("Benchmark done (%.0f s)\nInput lag: %.0f ms typical, %.0f ms worst 5%%\n"
                    "Latency estimate: %.0f ms\nDrawn: %.1f fps\nReport: %s",
                    summary.seconds, summary.input_lag_p50, summary.input_lag_p95,
                    summary.latency_p50, summary.fps_rendered, summary.path);
    }
  }
  return error[0] ? -1 : 0;
}
