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
#include <string.h>
#include <opus/opus_multistream.h>
#include <psp2/audioout.h>

enum {
  VITA_AUDIO_INIT_OK        = 0,
  VITA_AUDIO_ERROR_BAD_OPUS = 0x80020001,
  VITA_AUDIO_ERROR_PORT     = 0x80020002,
};

// The port's grain: sceAudioOutOpenPort takes a multiple of 64 samples from 64
// to 65472, so a 5 ms Opus frame (240) doesn't fit by itself. 960 (20 ms) is
// the smallest grain that holds whole 5, 10 and 20 ms frames.
#define VITA_SAMPLES 960
#define BUFFER_SIZE (2 * VITA_SAMPLES)
// Output buffers to rotate through; each is one grain of stereo 16-bit samples
#define BUFFER_COUNT 3

// The longest Opus frame is 60 ms (2880 samples at 48 kHz). Packets are decoded
// into a scratch frame of this size, so whatever duration the host sends plays,
// even when it isn't the one asked for.
#define MAX_FRAME_SAMPLES 2880
// Smallest Opus frame at 48 kHz, 2.5 ms; every frame is a multiple of it
#define MIN_FRAME_SAMPLES 120

// Audio queued in common-c but not yet decoded, in ms. sceAudioOutOutput
// blocks for one VITA_SAMPLES buffer (20 ms), so up to 20 ms piles up between
// outputs in normal play. Past two buffers it's a backlog from a network
// stall that would otherwise never drain, so drop packets down to one buffer.
#define PENDING_DROP_START_MS (2 * VITA_SAMPLES / 48)
#define PENDING_DROP_STOP_MS (VITA_SAMPLES / 48)
static int decode_offset;
// -1 while no port is open; a valid port can be 0
static int port = -1;

static int active_audio_thread = true;
static OpusMSDecoder* decoder = NULL;

// sceAudioOutOutput returns while the buffer it was given is still playing,
// so fill the next one in the ring. The hardware wants them 64-byte aligned,
// and the size is a multiple of 64 so every buffer in the array is.
static short buffers[BUFFER_COUNT][BUFFER_SIZE] __attribute__((aligned(64)));
_Static_assert(BUFFER_SIZE * sizeof(short) % 64 == 0, "audio buffers must stay 64-byte aligned");
static int buffer_index;

// Samples in the last frame decoded, which is how much concealment fills in for
// a lost packet. It starts at the negotiated packet duration (240 for 5 ms).
static int last_frame_samples = 240;
// One decoded frame, before it's copied into the output buffer
static short frame_pcm[2 * MAX_FRAME_SAMPLES];
static bool decode_thread_logged = false;

static bool catching_up;
static unsigned int catch_up_dropped;
static unsigned int dropped_packets;
static unsigned int catch_ups;
static unsigned int concealed_packets;

// common-c calls this only after a successful init, once its audio threads
// have exited; init also uses it to undo a partial setup. Safe to call twice.
static void vita_renderer_cleanup() {
  if (dropped_packets > 0 || concealed_packets > 0) {
    vita_debug_log("audio: dropped %u packets in %u catch-ups, concealed %u lost packets\n",
                   dropped_packets, catch_ups, concealed_packets);
    // Logged once; cleanup may run again (init calls it to drop leftovers)
    dropped_packets = 0;
    catch_ups = 0;
    concealed_packets = 0;
  }
  if (port >= 0) {
    int rc = sceAudioOutReleasePort(port);
    vita_debug_log("release port 0x%x: 0x%x\n", port, rc);
    port = -1;
  }
  if (decoder != NULL) {
    opus_multistream_decoder_destroy(decoder);
    decoder = NULL;
  }
}

static int vita_renderer_init(int audioConfiguration, POPUS_MULTISTREAM_CONFIGURATION opusConfig, void* audioContext, int arFlags) {
  int rc;
  // Drop anything left from an earlier stream
  vita_renderer_cleanup();

  // Output is 48 kHz stereo, and common-c says how long each packet is
  int frame_samples = opusConfig->samplesPerFrame;
  if (opusConfig->channelCount != 2 || frame_samples < MIN_FRAME_SAMPLES ||
      frame_samples > MAX_FRAME_SAMPLES || frame_samples % MIN_FRAME_SAMPLES != 0) {
    vita_debug_log("unsupported audio: %d channels, %d samples a frame\n",
                   opusConfig->channelCount, frame_samples);
    return VITA_AUDIO_ERROR_BAD_OPUS;
  }
  last_frame_samples = frame_samples;
  vita_debug_log("audio: %d ms packets\n", frame_samples / 48);

  decode_offset = 0;
  buffer_index = 0;
  decode_thread_logged = false;
  catching_up = false;
  catch_up_dropped = 0;
  dropped_packets = 0;
  catch_ups = 0;
  concealed_packets = 0;
  decoder = opus_multistream_decoder_create(opusConfig->sampleRate,
                                            opusConfig->channelCount,
                                            opusConfig->streams,
                                            opusConfig->coupledStreams,
                                            opusConfig->mapping,
                                            &rc);

  if (rc < 0 || decoder == NULL) {
      vita_debug_log("opus decoder create failed: %d\n", rc);
      vita_renderer_cleanup();
      return VITA_AUDIO_ERROR_BAD_OPUS;
  }

  port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, VITA_SAMPLES, 48000, SCE_AUDIO_OUT_MODE_STEREO);

  if (port < 0) {
      vita_debug_log("open port failed: 0x%x\n", port);
      port = -1;
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

// Appends decoded stereo samples to the current output buffer, and plays it
// each time it fills. A frame can straddle two buffers when the packet
// duration doesn't divide VITA_SAMPLES.
static void vita_renderer_queue_samples(const short *pcm, int samples) {
  while (samples > 0) {
    int count = VITA_SAMPLES - decode_offset;
    if (count > samples)
      count = samples;
    memcpy(buffers[buffer_index] + 2 * decode_offset, pcm, count * 2 * sizeof(short));
    decode_offset += count;
    pcm += 2 * count;
    samples -= count;

    if (decode_offset == VITA_SAMPLES) {
      decode_offset = 0;
      if (active_audio_thread) {
        sceAudioOutOutput(port, buffers[buffer_index]);
        buffer_index = (buffer_index + 1) % BUFFER_COUNT;
      }
    }
  }
}

// common-c calls this with NULL and 0 in place of a packet that was lost and
// that its own FEC couldn't rebuild (AudioStream.c, decodeInputData)
static void vita_renderer_decode_and_play_sample(char* data, int length) {
  if (!decoder)
    return;

  if (!decode_thread_logged) {
    // Runs on common-c's AudioDec thread, so a blocking output doesn't stall
    // AudioRecv, and late packets queue where LiGetPendingAudioDuration sees them
    vita_debug_log_thread("audio decode");
    decode_thread_logged = true;
  }

  bool lost = data == NULL || length <= 0;
  int decodeLen;
  if (lost) {
    // A backlog is being thrown away, so there's nothing to conceal; the gap
    // is already inside what's dropped
    if (catching_up)
      return;
    // Opus extrapolates from what it last played. Opus in-band FEC (decode_fec)
    // would need the next packet's data, which this callback doesn't have yet;
    // the host's recovery is the Reed-Solomon FEC in common-c, already spent.
    decodeLen = opus_multistream_decode(decoder, NULL, 0, frame_pcm, last_frame_samples, 0);
    if (decodeLen > 0)
      concealed_packets++;
  } else {
    if (vita_renderer_should_drop())
      return;
    // Room for the longest frame, so a packet of any duration decodes
    decodeLen = opus_multistream_decode(decoder, (const unsigned char*)data, length, frame_pcm, MAX_FRAME_SAMPLES, 0);
    if (decodeLen > 0)
      last_frame_samples = decodeLen;
  }

  if (decodeLen > 0) {
    vita_renderer_queue_samples(frame_pcm, decodeLen);
  } else {
    vita_debug_log("Opus error from %s: %d\n", lost ? "concealment" : "decode", decodeLen);
  }
}

AUDIO_RENDERER_CALLBACKS audio_callbacks_vita = {
  .init = vita_renderer_init,
  .cleanup = vita_renderer_cleanup,
  .decodeAndPlaySample = vita_renderer_decode_and_play_sample,
  .capabilities = 0,
};


void vitaaudio_set_packet_ms(int ms) {
  // common-c asks the host for 10 ms packets when the renderer says it's a
  // slow decoder; 5 ms is what it uses otherwise. (That flag also keeps it from
  // choosing high-quality surround, which the Vita's stereo output never uses.)
  audio_callbacks_vita.capabilities = ms == 10 ? CAPABILITY_SLOW_OPUS_DECODER : 0;
}

void vitaaudio_start() {
  active_audio_thread = true;
}

void vitaaudio_stop() {
  active_audio_thread = false;
}

unsigned int vitaaudio_dropped_packets() {
  return __atomic_load_n(&dropped_packets, __ATOMIC_RELAXED);
}
