/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2016 Ilya Zhuravlev
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

#include "../audio.h"
#include "../debug.h"

#include <stdio.h>
#include <opus/opus_multistream.h>
#include <psp2/audioout.h>

enum {
  VITA_AUDIO_INIT_OK        = 0,
  VITA_AUDIO_ERROR_BAD_OPUS = 0x80020001,
  VITA_AUDIO_ERROR_PORT     = 0x80020002,
};

#define FRAME_SIZE 240
#define VITA_SAMPLES 960
#define BUFFER_SIZE (2 * VITA_SAMPLES)

// Audio queued in common-c but not yet decoded, in ms. sceAudioOutOutput
// blocks for one VITA_SAMPLES buffer (20 ms), so up to 20 ms piles up between
// outputs in normal play. Past two buffers it's a backlog from a network
// stall that would otherwise never drain, so drop packets down to one buffer.
#define PENDING_DROP_START_MS (2 * VITA_SAMPLES / 48)
#define PENDING_DROP_STOP_MS (VITA_SAMPLES / 48)
static int decode_offset;
static int port;

static int active_audio_thread = true;
static OpusMSDecoder* decoder = NULL;

static short buffer[BUFFER_SIZE];
static bool decode_thread_logged = false;

static bool catching_up;
static unsigned int catch_up_dropped;
static unsigned int dropped_packets;
static unsigned int catch_ups;

static void vita_renderer_cleanup() {
  if (dropped_packets > 0) {
    vita_debug_log("audio: dropped %u packets in %u catch-ups\n", dropped_packets, catch_ups);
  }
  if (decoder != NULL) {
    opus_multistream_decoder_destroy(decoder);
    decoder = NULL;
  }
}

static int vita_renderer_init(int audioConfiguration, POPUS_MULTISTREAM_CONFIGURATION opusConfig, void* audioContext, int arFlags) {
  int rc;
  decode_thread_logged = false;
  catching_up = false;
  catch_up_dropped = 0;
  dropped_packets = 0;
  catch_ups = 0;
  decoder = opus_multistream_decoder_create(opusConfig->sampleRate,
                                            opusConfig->channelCount,
                                            opusConfig->streams,
                                            opusConfig->coupledStreams,
                                            opusConfig->mapping,
                                            &rc);

  if (rc < 0) {
      return VITA_AUDIO_ERROR_BAD_OPUS;
  }

  port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, VITA_SAMPLES, 48000, SCE_AUDIO_OUT_MODE_STEREO);

  if (port < 0) {
      vita_renderer_cleanup();
      return VITA_AUDIO_ERROR_PORT;
  }

  vita_debug_log("open port 0x%x\n", port);
  return VITA_AUDIO_INIT_OK;
}

static bool vita_renderer_should_drop() {
  int pending = LiGetPendingAudioDuration();

  if (!catching_up) {
    if (pending <= PENDING_DROP_START_MS)
      return false;
    catching_up = true;
    catch_up_dropped = 0;
    catch_ups++;
  } else if (pending <= PENDING_DROP_STOP_MS) {
    catching_up = false;
    vita_debug_log("audio: dropped %u packets to catch up\n", catch_up_dropped);
    return false;
  }

  catch_up_dropped++;
  // Read by the stats thread
  __atomic_fetch_add(&dropped_packets, 1, __ATOMIC_RELAXED);
  return true;
}

static void vita_renderer_decode_and_play_sample(char* data, int length) {
  if (!data)
    return;

  if (!decode_thread_logged) {
    // Runs on common-c's AudioDec thread, so a blocking output doesn't stall
    // AudioRecv, and late packets queue where LiGetPendingAudioDuration sees them
    vita_debug_log_thread("audio decode");
    decode_thread_logged = true;
  }

  if (vita_renderer_should_drop())
    return;

  int decodeLen = opus_multistream_decode(decoder, data, length, buffer + 2 * decode_offset, FRAME_SIZE, 0);
  if (decodeLen > 0) {
    if (decodeLen != FRAME_SIZE)
      return;
    decode_offset += decodeLen;

    if (decode_offset == VITA_SAMPLES) {
      decode_offset = 0;
      if (active_audio_thread) {
        sceAudioOutOutput(port, buffer);
      }
    }
  } else {
    vita_debug_log("Opus error from decode: %d\n", decodeLen);
  }
}

AUDIO_RENDERER_CALLBACKS audio_callbacks_vita = {
  .init = vita_renderer_init,
  .cleanup = vita_renderer_cleanup,
  .decodeAndPlaySample = vita_renderer_decode_and_play_sample,
  .capabilities = 0,
};


void vitaaudio_start() {
  active_audio_thread = true;
}

void vitaaudio_stop() {
  active_audio_thread = false;
}

unsigned int vitaaudio_dropped_packets() {
  return __atomic_load_n(&dropped_packets, __ATOMIC_RELAXED);
}
