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

#include "../video.h"
#include "../config.h"
#include "../debug.h"
#include "../gui/guilib.h"
#include "../util.h"
#include "../input/vita.h"
#include "vita.h"
#include "sps.h"
#include "../stats.h"
#include "../bench.h"

#include <Limelight.h>

#include <stdbool.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/display.h>
#include <psp2/videodec.h>
#include <vita2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define printf vita_debug_log

extern void gs_sps_stop();

extern double_click_tracker dc_tracker;

void draw_streaming(vita2d_texture *frame_texture);
void draw_fps();
void draw_indicators();

enum {
  VITA_VIDEO_INIT_OK                    = 0,
  VITA_VIDEO_ERROR_NO_MEM               = 0x80010001,
  VITA_VIDEO_ERROR_INIT_LIB             = 0x80010002,
  VITA_VIDEO_ERROR_QUERY_DEC_MEMSIZE    = 0x80010003,
  VITA_VIDEO_ERROR_ALLOC_MEM            = 0x80010004,
  VITA_VIDEO_ERROR_GET_MEMBASE          = 0x80010005,
  VITA_VIDEO_ERROR_CREATE_DEC           = 0x80010006,
  VITA_VIDEO_ERROR_CREATE_PACER_THREAD  = 0x80010007,
};

#define DECODER_BUFFER_SIZE (128 * 1024)

#define AV_INPUT_BUFFER_PADDING_SIZE 64

// gs_sps_fix writes at most 128 bytes of rewritten SPS NAL after the start code
#define SPS_FIXUP_MAX_GROWTH 128

static char* decoder_buffer = NULL;

static size_t decoder_buffer_size = 0;

enum {
  SCREEN_WIDTH = 960,
  SCREEN_HEIGHT = 544,
  LINE_SIZE = 960,
  FRAMEBUFFER_SIZE = 2 * 1024 * 1024,
  FRAMEBUFFER_ALIGNMENT = 256 * 1024
};

enum VideoStatus {
  NOT_INIT,
  INIT_GS,
  INIT_FRAMEBUFFER,
  INIT_AVC_LIB,
  INIT_DECODER_MEMBLOCK,
  INIT_AVC_DEC,
  INIT_FRAME_PACER_THREAD,
};

// Two textures so the hardware decoder can write the next frame while the GPU
// is still reading the previous one
#define FRAME_TEXTURE_COUNT 2
static vita2d_texture *frame_textures[FRAME_TEXTURE_COUNT] = {NULL};
// The texture the next frame is decoded into. The other one may still be
// queued for display.
static int back_texture_idx = 0;
enum VideoStatus video_status = NOT_INIT;

SceAvcdecCtrl *decoder = NULL;
SceUID displayblock = -1;
SceUID decoderblock = -1;
SceUID pacer_thread = -1;
SceVideodecQueryInitInfoHwAvcdec *init = NULL;
SceAvcdecQueryDecoderInfo *decoder_info = NULL;

typedef struct {
  bool activated;
  uint8_t alpha;
  bool plus;
} indicator_status;

//static unsigned numframes;
static bool active_video_thread = true;
static bool active_pacer_thread = false;
static indicator_status poor_net_indicator = {0};
static bool decode_thread_logged = false;
// A pause mid-stream drops units undecoded, so the decoder's reference
// frames are stale until the next keyframe (IDR). Set by the UI thread
// (vitavideo_stop/start) and the decode thread, so changed atomically
enum {
  KEYFRAME_OK,        // decoding normally, or a new stream not started yet
  KEYFRAME_PAUSED,    // paused mid-stream: drop units
  KEYFRAME_AWAITED,   // resumed: drop units until a keyframe arrives
};
static int keyframe_state = KEYFRAME_OK;
// When we last asked the host for a keyframe after a pause. Written by the UI
// and pacer threads and read by the decode thread, so accessed atomically
static uint64_t keyframe_request_us = 0;
// How long to wait for that keyframe before asking again
#define KEYFRAME_RETRY_US 1000000
// Fixed for a stream at setup, since the textures are created for one format
static bool decode_yuv = false;

// A reminder of how to reach the menu, shown at the start of each stream
#define STREAM_HINT_DURATION_US 5000000
#define STREAM_HINT_FADE_US 1000000
static const char *STREAM_HINT_TEXT = "Press START + L + R for the menu and to quit";
// When the first frame of this stream was drawn, or 0 before that
static uint64_t stream_hint_start_us = 0;

uint32_t frame_count = 0;
uint32_t need_drop = 0;
uint32_t curr_fps[2] = {0, 0};
float carry = 0;

typedef struct {
  unsigned int texture_width;
  unsigned int texture_height;
  float origin_x;
  float origin_y;
  float region_x1;
  float region_y1;
  float region_x2;
  float region_y2;
} image_scaling_settings;

static image_scaling_settings image_scaling = {0};

void update_scaling_settings(int width, int height) {
  image_scaling.texture_width = SCREEN_WIDTH;
  image_scaling.texture_height = SCREEN_HEIGHT;
  image_scaling.origin_x = 0;
  image_scaling.origin_y = 0;
  image_scaling.region_x1 = 0;
  image_scaling.region_y1 = 0;
  image_scaling.region_x2 = image_scaling.texture_width;
  image_scaling.region_y2 = image_scaling.texture_height;

  double scaled_width = (double) SCREEN_HEIGHT * width / height;
  double scaled_height = (double) SCREEN_WIDTH * height / width;

  if (SCREEN_WIDTH * height == SCREEN_HEIGHT * width) {
    // streaming resolution ratio matches Vita's screen ratio
    // use default setting
  } else if (SCREEN_WIDTH * height > SCREEN_HEIGHT * width) {
    // host ratio example: 4:3, 16:10
    // Vita ratio range: 2:16 (64 x 544) - native (960 x 544)
    if (config.center_region_only) {
      image_scaling.texture_height = VITA_DECODER_RESOLUTION(scaled_height);
      image_scaling.region_y1 = VITA_DECODER_RESOLUTION((scaled_height - SCREEN_HEIGHT) / 2);
      image_scaling.region_y2 = VITA_DECODER_RESOLUTION((scaled_height + SCREEN_HEIGHT) / 2);
    } else {
      image_scaling.texture_width = VITA_DECODER_RESOLUTION(scaled_width);
      image_scaling.region_x2 = VITA_DECODER_RESOLUTION(scaled_width);
      image_scaling.origin_x = round((double) (SCREEN_WIDTH - image_scaling.texture_width) / 2);
    }
  } else {
    // host ratio example: 16:9, 21:9, 32:9
    // Vita ratio range: native (960 x 544) - 15:1 (960 x 64)
    if (config.center_region_only) {
      image_scaling.texture_width = VITA_DECODER_RESOLUTION(scaled_width);
      image_scaling.region_x1 = VITA_DECODER_RESOLUTION((scaled_width - SCREEN_WIDTH) / 2);
      image_scaling.region_x2 = VITA_DECODER_RESOLUTION((scaled_width + SCREEN_WIDTH) / 2);
    } else {
      image_scaling.texture_height = VITA_DECODER_RESOLUTION(scaled_height);
      image_scaling.region_y2 = VITA_DECODER_RESOLUTION(scaled_height);
      image_scaling.origin_y = round((double) (SCREEN_HEIGHT - image_scaling.texture_height) / 2);
    }
  }

  printf("update_scaling_settings: width = %u\n", width);
  printf("update_scaling_settings: height = %u\n", height);
  printf("update_scaling_settings: scaled_width = %f\n", scaled_width);
  printf("update_scaling_settings: scaled_height = %f\n", scaled_height);
  printf("update_scaling_settings: image_scaling.texture_width = %u\n", image_scaling.texture_width);
  printf("update_scaling_settings: image_scaling.texture_height = %u\n", image_scaling.texture_height);
  printf("update_scaling_settings: image_scaling.origin_x = %f\n", image_scaling.origin_x);
  printf("update_scaling_settings: image_scaling.origin_y = %f\n", image_scaling.origin_y);
  printf("update_scaling_settings: image_scaling.region_x1 = %f\n", image_scaling.region_x1);
  printf("update_scaling_settings: image_scaling.region_y1 = %f\n", image_scaling.region_y1);
  printf("update_scaling_settings: image_scaling.region_x2 = %f\n", image_scaling.region_x2);
  printf("update_scaling_settings: image_scaling.region_y2 = %f\n", image_scaling.region_y2);
}

static int vita_pacer_thread_main(SceSize args, void *argp) {
  // 1s
  int wait = 1000000;
  //float max_fps = 0;
  //sceDisplayGetRefreshRate(&max_fps);
  //if (config.stream.fps == 30) {
  //  max_fps /= 2;
  //}
  int max_fps = config.stream.fps;
  //uint64_t last_vblank_count = sceDisplayGetVcount();
  uint64_t last_check_time = sceKernelGetSystemTimeWide();
  //float carry = 0;
  need_drop = 0;
  frame_count = 0;
  while (active_pacer_thread) {
    //uint64_t curr_vblank_count = sceDisplayGetVcount();
    //uint32_t vblank_fps = curr_vblank_count - last_vblank_count;
    uint32_t curr_frame_count = frame_count;
    frame_count = 0;

    if (!active_video_thread) {
    //  carry = 0;
    } else {
      if (config.enable_frame_pacer && curr_frame_count > max_fps) {
        //carry += curr_frame_count - max_fps;
        //if (carry > 1) {
        //  need_drop += (int)carry;
        //  carry -= (int)carry;
        //}
        need_drop += curr_frame_count - max_fps;
      }
      //vita_debug_log("fps0/fps1/carry/need_drop: %u/%u/%f/%u\n",
      //               curr_frame_count, vblank_fps, carry, need_drop);
    }

    curr_fps[0] = curr_frame_count;
    curr_fps[1] = max_fps;

    // This loop already runs about once a second for the whole stream
    vitastats_tick();

    // The keyframe asked for on resume hasn't come: ask again. Done here, not
    // when a unit arrives, since a static screen may send none. Read the
    // state, then the time it was asked for, then the clock, so the
    // subtraction can't go negative
    if (__atomic_load_n(&keyframe_state, __ATOMIC_ACQUIRE) == KEYFRAME_AWAITED) {
      uint64_t requested_us = __atomic_load_n(&keyframe_request_us, __ATOMIC_RELAXED);
      uint64_t now_us = sceKernelGetSystemTimeWide();
      if (now_us - requested_us >= KEYFRAME_RETRY_US) {
        vita_debug_log("no keyframe %llu ms after resume, requesting another\n",
                       (now_us - requested_us) / 1000);
        __atomic_store_n(&keyframe_request_us, now_us, __ATOMIC_RELAXED);
        LiRequestIdrFrame();
      }
    }

    //last_vblank_count = curr_vblank_count;
    uint64_t curr_check_time = sceKernelGetSystemTimeWide();
    uint32_t lapse = curr_check_time - last_check_time;
    last_check_time = curr_check_time;
    if (lapse > wait && (lapse - wait) < wait) {
      //vita_debug_log("sleep: %d", wait * 2 - lapse);
      sceKernelDelayThread(wait * 2 - lapse);
    } else {
      sceKernelDelayThread(wait);
    }
  }
  return 0;
}

static void vita_cleanup() {
  if (video_status == INIT_FRAME_PACER_THREAD) {
    active_pacer_thread = false;
    // wait 10sec
    SceUInt timeout = 10000000;
    int ret;
    sceKernelWaitThreadEnd(pacer_thread, &ret, &timeout);
    sceKernelDeleteThread(pacer_thread);
    video_status--;
  }

  if (video_status == INIT_AVC_DEC) {
    sceAvcdecDeleteDecoder(decoder);
    video_status--;
  }

  if (video_status == INIT_DECODER_MEMBLOCK) {
    if (decoderblock >= 0) {
      sceKernelFreeMemBlock(decoderblock);
      decoderblock = -1;
    }
    if (decoder != NULL) {
      free(decoder);
      decoder = NULL;
    }
    if (decoder_info != NULL) {
      free(decoder_info);
      decoder_info = NULL;
    }
    video_status--;
  }

  if (video_status == INIT_AVC_LIB) {
    sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);

    if (init != NULL) {
      free(init);
      init = NULL;
    }
    video_status--;
  }

  if (video_status == INIT_FRAMEBUFFER) {
    // Make sure the GPU is no longer reading the textures before freeing them
    vita2d_wait_rendering_done();
    for (int i = 0; i < FRAME_TEXTURE_COUNT; i++) {
      if (frame_textures[i] != NULL) {
        vita2d_free_texture(frame_textures[i]);
        frame_textures[i] = NULL;
      }
    }

    if (decoder_buffer != NULL) {
      free(decoder_buffer);
      decoder_buffer = NULL;
    }
    video_status--;
  }

  if (video_status == INIT_GS) {
    gs_sps_stop();
    video_status--;
  }
}

static int vita_setup(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
  int ret;
  printf("vita video setup\n");
  decode_thread_logged = false;
  __atomic_store_n(&keyframe_state, KEYFRAME_OK, __ATOMIC_RELEASE);
  stream_hint_start_us = 0;
  vitastats_reset();
  decode_yuv = config.decode_yuv;
  printf("video output: %s\n", decode_yuv ? "YUV420 (NV21), converted by the GPU" : "RGBA8888 from the decoder");

  if (video_status == NOT_INIT) {
    // INIT_GS
    gs_sps_init(width, height);
    video_status++;
  }

  if (video_status == INIT_GS) {
    // INIT_FRAMEBUFFER
    update_scaling_settings(width, height);

    decoder_buffer_size = DECODER_BUFFER_SIZE + AV_INPUT_BUFFER_PADDING_SIZE;
    decoder_buffer = malloc(decoder_buffer_size);
    if (decoder_buffer == NULL) {
      printf("decoder_buffer: not enough memory\n");
      ret = VITA_VIDEO_ERROR_NO_MEM;
      // Let vita_cleanup() run this stage's teardown, which is NULL-safe
      video_status = INIT_FRAMEBUFFER;
      goto cleanup;
    }

    for (int i = 0; i < FRAME_TEXTURE_COUNT; i++) {
      // In YUV mode the decoder writes NV21 (a Y plane, then interleaved V/U,
      // so the YVU format: YUV swaps red and blue) and the GPU's texture unit
      // converts it while sampling. CSC0 is BT.601 limited range, which is
      // what config_parse() asks the host to encode (stream.colorSpace and
      // colorRange; change them together); measured against the decoder's
      // own RGBA output.
      // vita2d sizes unknown formats at 4 bytes per pixel, which is more than
      // the 1.5 that NV21 needs.
      frame_textures[i] = vita2d_create_empty_texture_format(image_scaling.texture_width, image_scaling.texture_height,
                                                             decode_yuv ? SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC0
                                                                        : SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR);
      if (frame_textures[i] == NULL) {
        printf("frame_textures[%d]: not enough memory\n", i);
        ret = VITA_VIDEO_ERROR_NO_MEM;
        // Let vita_cleanup() free the buffer and any texture already created
        video_status = INIT_FRAMEBUFFER;
        goto cleanup;
      }
    }
    back_texture_idx = 0;

    printf("Initalized Framebuffer");

    video_status++;
  }

  if (video_status == INIT_FRAMEBUFFER) {
    // INIT_AVC_LIB
    if (init == NULL) {
      init = calloc(1, sizeof(SceVideodecQueryInitInfoHwAvcdec));
      if (init == NULL) {
        printf("init_framebuffer: not enough memory\n");
        ret = VITA_VIDEO_ERROR_NO_MEM;
        goto cleanup;
      }
    }
    init->size = sizeof(SceVideodecQueryInitInfoHwAvcdec);
    init->horizontal = VITA_DECODER_RESOLUTION(width);
    init->vertical = VITA_DECODER_RESOLUTION(height);
    init->numOfRefFrames = 4;
    init->numOfStreams = 1;

    ret = sceVideodecInitLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC, init);
    if (ret < 0) {
      printf("sceVideodecInitLibrary 0x%x\n", ret);
      ret = VITA_VIDEO_ERROR_INIT_LIB;
      goto cleanup;
    }
    printf("Initalized AVC library");
    video_status++;
  }

  if (video_status == INIT_AVC_LIB) {
    // INIT_DECODER_MEMBLOCK
    if (decoder_info == NULL) {
      decoder_info = calloc(1, sizeof(SceAvcdecQueryDecoderInfo));
      if (decoder_info == NULL) {
        printf("decoder_info: not enough memory\n");
        ret = VITA_VIDEO_ERROR_NO_MEM;
        goto cleanup;
      }
    }
    decoder_info->horizontal = init->horizontal;
    decoder_info->vertical = init->vertical;
    decoder_info->numOfRefFrames = init->numOfRefFrames;

    SceAvcdecDecoderInfo decoder_info_out = {0};

    ret = sceAvcdecQueryDecoderMemSize(SCE_VIDEODEC_TYPE_HW_AVCDEC, decoder_info, &decoder_info_out);
    if (ret < 0) {
      printf("sceAvcdecQueryDecoderMemSize 0x%x size 0x%x\n", ret, decoder_info_out.frameMemSize);
      ret = VITA_VIDEO_ERROR_QUERY_DEC_MEMSIZE;
      goto cleanup;
    }

    decoder = calloc(1, sizeof(SceAvcdecCtrl));
    if (decoder == NULL) {
      printf("not enough memory\n");
      ret = VITA_VIDEO_ERROR_ALLOC_MEM;
      goto cleanup;
    }

    size_t sz = (decoder_info_out.frameMemSize + 0xFFFFF) & ~0xFFFFF;
    decoder->frameBuf.size = sz;
    printf("allocating size 0x%x\n", sz);

    decoderblock = sceKernelAllocMemBlock("decoder", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, sz, NULL);
    if (decoderblock < 0) {
      printf("decoderblock: 0x%08x\n", decoderblock);
      ret = VITA_VIDEO_ERROR_ALLOC_MEM;
      goto cleanup;
    }

    ret = sceKernelGetMemBlockBase(decoderblock, &decoder->frameBuf.pBuf);
    if (ret < 0) {
      printf("sceKernelGetMemBlockBase: 0x%x\n", ret);
      ret = VITA_VIDEO_ERROR_GET_MEMBASE;
      goto cleanup;
    }
    video_status++;
  }

  if (video_status == INIT_DECODER_MEMBLOCK) {
    // INIT_AVC_DEC
    printf("base: 0x%08x\n", (unsigned int)decoder->frameBuf.pBuf);

    ret = sceAvcdecCreateDecoder(SCE_VIDEODEC_TYPE_HW_AVCDEC, decoder, decoder_info);
    if (ret < 0) {
      printf("sceAvcdecCreateDecoder 0x%x\n", ret);
      ret = VITA_VIDEO_ERROR_CREATE_DEC;
      goto cleanup;
    }
    video_status++;
  }

  if (video_status == INIT_AVC_DEC) {
    // INIT_FRAME_PACER_THREAD
    ret = sceKernelCreateThread("frame_pacer", vita_pacer_thread_main, 0, 0x10000, 0, 0, NULL);
    if (ret < 0) {
      printf("sceKernelCreateThread 0x%x\n", ret);
      ret = VITA_VIDEO_ERROR_CREATE_PACER_THREAD;
      goto cleanup;
    }
    pacer_thread = ret;
    active_pacer_thread = true;
    sceKernelStartThread(pacer_thread, 0, NULL);
    video_status++;
  }

  return VITA_VIDEO_INIT_OK;

cleanup:
  vita_cleanup();
  return ret;
}

static int vita_submit_decode_unit(PDECODE_UNIT decodeUnit) {
  uint64_t submit_us = LiGetMicroseconds();
  vitastats_frame_received(decodeUnit, submit_us);

  if (!decode_thread_logged) {
    // Runs on common-c's VideoDec thread, or VideoRecv with config.direct_decode
    vita_debug_log_thread("video decode");
    decode_thread_logged = true;
  }

  int state = __atomic_load_n(&keyframe_state, __ATOMIC_ACQUIRE);
  // The keyframe that ends a resume: always drawn, see the frame pacer below
  bool recovery_keyframe = false;
  bool awaiting_keyframe = false;
  if (state == KEYFRAME_PAUSED) {
    // Paused: the menu covers the stream, so decoding would only burn power.
    // DR_OK, so common-c doesn't treat the dropped units as a decoder error
    // and ask the host for keyframes all through the pause. (Units that come
    // between setup and the connection starting are still decoded, so the
    // stream's first keyframe isn't lost.)
    return DR_OK;
  }
  if (state == KEYFRAME_AWAITED) {
    if (decodeUnit->frameType != FRAME_TYPE_IDR) {
      // P-frames would reference the frames dropped while paused. Keep
      // dropping until the keyframe vitavideo_start() asked for arrives (the
      // pacer thread asks again if it doesn't)
      return DR_OK;
    }
    // Recovery ends only once this keyframe has decoded (below), so a failed
    // decode leaves it awaited, with the pacer thread's retries still on
    awaiting_keyframe = true;
  }

  SceAvcdecAu au = {0};
  SceAvcdecArrayPicture array_picture = {0};
  struct SceAvcdecPicture picture = {0};
  struct SceAvcdecPicture *pictures = { &picture };
  array_picture.numOfElm = 1;
  array_picture.pPicture = &pictures;

  //frame->time = decodeUnit->receiveTimeMs;

  picture.size = sizeof(picture);
  picture.frame.pixelType = decode_yuv ? SCE_AVCDEC_PIXELFORMAT_YUV420_PACKED_RASTER : SCE_AVCDEC_PIXELFORMAT_RGBA8888;
  picture.frame.framePitch = image_scaling.texture_width;
  picture.frame.frameWidth = image_scaling.texture_width;
  picture.frame.frameHeight = image_scaling.texture_height;
  picture.frame.pPicture[0] = vita2d_texture_get_datap(frame_textures[back_texture_idx]);
  if (decode_yuv) {
    // GXM's two-plane YUV420 texture expects the V/U plane straight after Y
    picture.frame.pPicture[1] = (uint8_t *)picture.frame.pPicture[0]
                                + image_scaling.texture_width * image_scaling.texture_height;
  }

  //ensure_buf_size((void *)&decoder_buffer, &decoder_buffer_size, decodeUnit->fullLength + 64);

  // The SPS rewrite below can grow the unit, so leave room for it on top of the padding
  size_t required_size = decodeUnit->fullLength + SPS_FIXUP_MAX_GROWTH + AV_INPUT_BUFFER_PADDING_SIZE;
  if (decoder_buffer_size < required_size) {
    printf("Reallocating decoder buffer to %u bytes", required_size);
    decoder_buffer = realloc(decoder_buffer, required_size);
    decoder_buffer_size = required_size;
    if (decoder_buffer == NULL) {
      printf("Out of memory! could not reallocate buffer!!");
      exit(1);
    }
  }


/*   if (decodeUnit->fullLength >= DECODER_BUFFER_SIZE + 64) {
    printf("Video decode buffer too small\n");
    exit(1);
  } */

  PLENTRY entry = decodeUnit->bufferList;
  uint32_t length = 0;
  while (entry != NULL) {
    if (entry->bufferType == BUFFER_TYPE_SPS) {
      gs_sps_fix(entry, GS_SPS_BITSTREAM_FIXUP, decoder_buffer, &length);
    } else {
      memcpy(decoder_buffer+length, entry->data, entry->length);
      length += entry->length;
    }
    entry = entry->next;
  }

  au.es.pBuf = decoder_buffer;
  // Use the assembled length, not fullLength, since gs_sps_fix may have resized the SPS
  au.es.size = length;
  au.dts.lower = 0xFFFFFFFF;
  au.dts.upper = 0xFFFFFFFF;
  au.pts.lower = 0xFFFFFFFF;
  au.pts.upper = 0xFFFFFFFF;

  int ret = 0;
  // Separates assembling the frame from the hardware decode, for the benchmark
  uint64_t hw_start_us = LiGetMicroseconds();
  ret = sceAvcdecDecode(decoder, &au, &array_picture);
  uint64_t decoded_us = LiGetMicroseconds();
  vitastats_frame_decoded(decoded_us - submit_us);
  if (ret < 0) {
    printf("sceAvcdecDecode (len=0x%x): 0x%x numOfOutput %d\n", decodeUnit->fullLength, ret, array_picture.numOfOutput);
    return DR_NEED_IDR;
  }

  if (awaiting_keyframe) {
    // The decoder has its reference again, so P-frames can follow. Only if
    // no new pause began meanwhile: then this keyframe is decoded but not
    // drawn, and the next resume asks for another
    int expected = KEYFRAME_AWAITED;
    if (__atomic_compare_exchange_n(&keyframe_state, &expected, KEYFRAME_OK, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      recovery_keyframe = true;
      vita_debug_log("keyframe %llu ms after resume\n",
                     (sceKernelGetSystemTimeWide()
                      - __atomic_load_n(&keyframe_request_us, __ATOMIC_RELAXED)) / 1000);
    }
  }

  if (array_picture.numOfOutput != 1) {
    //printf("numOfOutput %d\n", array_picture.numOfOutput);
    return DR_OK;
  }

  if (vitabench_active()) {
    // Read the benchmark page's strip from the texture this frame was decoded into
    vita2d_texture *decoded = frame_textures[back_texture_idx];
    vitabench_frame_decoded(decodeUnit, submit_us, hw_start_us, decoded_us,
                            vita2d_texture_get_datap(decoded),
                            // In YUV mode the probes read the Y (luma) plane, one byte per pixel
                            decode_yuv ? (int)image_scaling.texture_width : vita2d_texture_get_stride(decoded),
                            decode_yuv ? 1 : 4,
                            image_scaling.texture_height,
                            image_scaling.region_x1, image_scaling.region_y1,
                            image_scaling.region_x2, image_scaling.region_y2);
  }

  // Paused while this unit was decoding: don't draw over the menu
  if (active_video_thread) {
    // Never drop the resume keyframe: on a static screen nothing may follow it
    if (need_drop > 0 && !recovery_keyframe) {
      vita_debug_log("remain frameskip: %d\n", need_drop);
      // skip
      need_drop--;
      vitastats_frame_dropped_by_pacer();
    } else {
      // The decode above overlapped with the GPU drawing the previous frame from
      // the other texture. Wait for that to finish here, after the decode instead
      // of after the draw, so the next decode can safely reuse that texture.
      vita2d_wait_rendering_done();

      vita2d_start_drawing();

      draw_streaming(frame_textures[back_texture_idx]);
      draw_fps();
      draw_indicators();

      vita2d_end_drawing();
      vita2d_swap_buffers();
      uint64_t rendered_us = LiGetMicroseconds();
      vitastats_frame_rendered(rendered_us - decoded_us);
      if (vitabench_active()) {
        vitabench_frame_rendered(rendered_us);
      }

      // Only flip after drawing, so a dropped frame's texture is simply reused
      back_texture_idx = (back_texture_idx + 1) % FRAME_TEXTURE_COUNT;
      frame_count++;
    }
  }

  // if (numframes++ % 6 == 0)
  //   return DR_NEED_IDR;

  return DR_OK;
}

void draw_streaming(vita2d_texture *frame_texture) {
  // ui is still rendering in the background, clear the screen first
  vita2d_clear_screen();
  vita2d_draw_texture_part(frame_texture,
                           image_scaling.origin_x,
                           image_scaling.origin_y,
                           image_scaling.region_x1,
                           image_scaling.region_y1,
                           image_scaling.region_x2,
                           image_scaling.region_y2);
}

void draw_fps() {
  vitastats_draw();
}

static void draw_stream_hint(void) {
  uint64_t now = LiGetMicroseconds();
  if (stream_hint_start_us == 0) {
    stream_hint_start_us = now;
  }
  uint64_t shown_for = now - stream_hint_start_us;
  if (shown_for >= STREAM_HINT_DURATION_US) {
    return;
  }

  // Fade out over the last second
  uint64_t remaining = STREAM_HINT_DURATION_US - shown_for;
  float opacity = remaining < STREAM_HINT_FADE_US ? (float)remaining / STREAM_HINT_FADE_US : 1.0f;

  const int font_size = 20;
  int text_width = vita2d_font_text_width(font, font_size, STREAM_HINT_TEXT);
  int box_width = text_width + 32;
  int box_height = 36;
  int box_x = (SCREEN_WIDTH - box_width) / 2;
  int box_y = SCREEN_HEIGHT - box_height - 24;
  vita2d_draw_rectangle(box_x, box_y, box_width, box_height, RGBA8(0, 0, 0, (uint8_t)(0xB0 * opacity)));
  vita2d_font_draw_text(font, box_x + 16, box_y + 25, RGBA8(0xFF, 0xFF, 0xFF, (uint8_t)(0xFF * opacity)),
                        font_size, STREAM_HINT_TEXT);
}

void draw_indicators() {
  draw_stream_hint();

  if (poor_net_indicator.activated) {
    vita2d_font_draw_text(font, 40, 500, RGBA8(0xFF, 0xFF, 0xFF, poor_net_indicator.alpha), 64, ICON_NETWORK);
    poor_net_indicator.alpha += (0x4 * (poor_net_indicator.plus ? 1 : -1));
    if (poor_net_indicator.alpha == 0) {
      poor_net_indicator.plus = !poor_net_indicator.plus;
      poor_net_indicator.alpha += (0x4 * (poor_net_indicator.plus ? 1 : -1));
    }
  }

  if (dc_tracker.currently_sprinting) {
    vita2d_font_draw_text(font, 40, 50, RGBA8(0xFF, 0xFF, 0xFF, 0xAA), 48, ICON_SPRINTING);
  }

}

void vitavideo_start() {
  // Resuming from a pause (a new stream's setup clears the flag, and starts
  // with a keyframe anyway). Units were dropped while paused and the menu was
  // drawn over the last frame: a keyframe repaints the whole picture at once,
  // even on a static screen where the host would otherwise send nothing.
  // Drawing is enabled before the state leaves PAUSED (units are still
  // dropped until then), so the keyframe that ends the wait is always drawn;
  // and it's asked for only after, so it can't be dropped as paused
  bool resuming = __atomic_load_n(&keyframe_state, __ATOMIC_ACQUIRE) == KEYFRAME_PAUSED;
  active_video_thread = true;
  vita2d_set_vblank_wait(config.enable_vita_vblank_wait);
  if (resuming) {
    __atomic_store_n(&keyframe_request_us, sceKernelGetSystemTimeWide(), __ATOMIC_RELAXED);
    __atomic_store_n(&keyframe_state, KEYFRAME_AWAITED, __ATOMIC_RELEASE);
    LiRequestIdrFrame();
  }
}

void vitavideo_stop() {
  vita2d_set_vblank_wait(true);
  __atomic_store_n(&keyframe_state, KEYFRAME_PAUSED, __ATOMIC_RELEASE);
  active_video_thread = false;
}

void vitavideo_show_poor_net_indicator() {
  poor_net_indicator.activated = true;
}

void vitavideo_hide_poor_net_indicator() {
  //poor_net_indicator.activated = false;
  memset(&poor_net_indicator, 0, sizeof(indicator_status));
}

int vitavideo_initialized() {
  return video_status != NOT_INIT;
}

DECODER_RENDERER_CALLBACKS decoder_callbacks_vita = {
  .setup = vita_setup,
  .cleanup = vita_cleanup,
  .submitDecodeUnit = vita_submit_decode_unit,
  // No CAPABILITY_DIRECT_SUBMIT by default: common-c then runs submitDecodeUnit on
  // its own VideoDec thread behind a frame queue, so the blocking decode, GPU wait
  // and buffer swap never stop VideoRecv from draining the socket. ui_connect.c
  // sets the flag when config.direct_decode asks for the old behaviour.
  .capabilities = CAPABILITY_SLICES_PER_FRAME(2)
};
