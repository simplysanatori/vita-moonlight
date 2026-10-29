/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015 Iwan Timmer
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

#include "sps.h"

#include "h264_stream.h"

#include "../src/debug.h"

#include <string.h>

static h264_stream_t* h264_stream;
static int initial_width, initial_height;
// What gs_sps_fix last said about the SPS, so it only logs when that changes
static int logged_host_refs, logged_refs, logged_level, logged_length;

void gs_sps_init(int width, int height) {
  h264_stream = h264_new();
  initial_width = width;
  initial_height = height;
  logged_host_refs = -1;
}

void gs_sps_stop() {
  if (h264_stream) {
    h264_free(h264_stream);
    h264_stream = NULL;
  }
}

// How many macroblocks the decoded picture buffer holds at a level (MaxDpbMbs,
// table A-1 of the standard). 0 for a level that isn't in the table.
static int level_max_dpb_mbs(int level_idc) {
  switch (level_idc) {
    case 9: case 10:            return 396;
    case 11:                    return 900;
    case 12: case 13: case 20:  return 2376;
    case 21:                    return 4752;
    case 22: case 30:           return 8100;
    case 31:                    return 18000;
    case 32:                    return 20480;
    case 40: case 41:           return 32768;
    case 42:                    return 34816;
    case 50:                    return 110400;
    case 51: case 52:           return 184320;
    case 60: case 61: case 62:  return 696320;
    default:                    return 0;
  }
}

// How many frames of frame_mbs macroblocks a level can hold, at most 16
static int level_max_dpb_frames(int level_idc, int frame_mbs) {
  int max_dpb_mbs = level_max_dpb_mbs(level_idc);
  if (max_dpb_mbs == 0 || frame_mbs <= 0)
    return 16;
  int frames = max_dpb_mbs / frame_mbs;
  return frames > 16 ? 16 : frames;
}

// Keeps the original SPS as it came, for when it can't be rewritten
static void copy_sps_unchanged(PLENTRY sps, uint8_t* out_buf, uint32_t* out_offset) {
  memcpy(out_buf+*out_offset, sps->data, sps->length);
  *out_offset += sps->length;
}

// max_ref_frames is how many reference frames the decoder was created to hold.
// 1 (what RFI off passes) squeezes the stream down to one reference frame, the
// lowest latency. More keeps the host's num_ref_frames, up to that, so that the
// frames the host encodes after a reference frame invalidation, which point at
// an older picture than the last one, still find it in the decoder.
void gs_sps_fix(PLENTRY sps, int flags, uint8_t* out_buf, uint32_t* out_offset, int max_ref_frames) {
  int start_len = sps->data[2] == 0x01 ? 3 : 4;

  // The host decides what is in here, so only hand the parser what claims to be an SPS
  if (sps->length <= start_len + 1 || (sps->data[start_len] & 0x1F) != NAL_UNIT_TYPE_SPS ||
      read_nal_unit(h264_stream, sps->data+start_len, sps->length-start_len) < 0) {
    vita_debug_log("SPS: can't parse it, passing it on unchanged\n");
    copy_sps_unchanged(sps, out_buf, out_offset);
    return;
  }

  int host_refs = h264_stream->sps->num_ref_frames;
  int host_level = h264_stream->sps->level_idc;
  // Macroblocks in a frame, which with the level says how many frames fit in the DPB
  long long frame_mbs_wide = (long long)(h264_stream->sps->pic_width_in_mbs_minus1 + 1)
                             * (h264_stream->sps->pic_height_in_map_units_minus1 + 1)
                             * (h264_stream->sps->frame_mbs_only_flag ? 1 : 2);
  int frame_mbs = frame_mbs_wide > 1000000 ? 1000000 : (int)frame_mbs_wide;

  if (max_ref_frames < 1)
    max_ref_frames = 1;
  else if (max_ref_frames > 16)
    max_ref_frames = 16;

  // Some decoders requires a reference frame count of 1 to decode successfully.
  // With reference frame invalidation the host needs more than that, so leave
  // its count alone unless the decoder can't hold it.
  if (max_ref_frames == 1 || host_refs < 0 || host_refs > max_ref_frames)
    h264_stream->sps->num_ref_frames = max_ref_frames;

  // Some decoders rely on H264 level to decide how many buffers are needed
  // Since we only need one frame buffered, we'll set level as low as we can
  // for known resolution combinations. Otherwise leave the profile alone (currently 5.0)
  // The level also caps the DPB, so don't go lower than it takes to hold the reference frames.
  int low_level = 0;
  if (initial_width == 1280 && initial_height == 720)
    low_level = 32; // Max 5 buffered frames at 1280x720x60
  else if (initial_width == 1920 && initial_height == 1080)
    low_level = 42; // Max 4 buffered frames at 1920x1080x60
  if (low_level != 0 && level_max_dpb_frames(low_level, frame_mbs) >= h264_stream->sps->num_ref_frames)
    h264_stream->sps->level_idc = low_level;

  // GFE 2.5.11 changed the SPS to add additional extensions
  // Some devices don't like these so we remove them here.
  if (flags & GS_SPS_REMOVE_VST_FIXUP)
    h264_stream->sps->vui.video_signal_type_present_flag = 0;
  if (flags & GS_SPS_REMOVE_CLI_FIXUP)
    h264_stream->sps->vui.chroma_loc_info_present_flag = 0;

  if ((flags & GS_SPS_BITSTREAM_FIXUP) == GS_SPS_BITSTREAM_FIXUP) {
    // Without a VUI the fields below aren't written at all. Only with reference
    // frame invalidation: without it the SPS is rewritten exactly as it always was
    if (max_ref_frames > 1)
      h264_stream->sps->vui_parameters_present_flag = 1;

    // The SPS that comes in the current H264 bytestream doesn't set the bitstream_restriction_flag
    // or the max_dec_frame_buffering which increases decoding latency on some devices.
    // log2_max_mv_length_horizontal and log2_max_mv_length_vertical are set to more
    // conservative values by GFE 2.5.11. We'll let those values stand.
    if (!h264_stream->sps->vui.bitstream_restriction_flag) {
      h264_stream->sps->vui.bitstream_restriction_flag = 1;
      h264_stream->sps->vui.motion_vectors_over_pic_boundaries_flag = 1;
      h264_stream->sps->vui.max_bits_per_mb_denom = 1;
      h264_stream->sps->vui.log2_max_mv_length_horizontal = 16;
      h264_stream->sps->vui.log2_max_mv_length_vertical = 16;
      h264_stream->sps->vui.num_reorder_frames = 0;
    }

    // No frame reordering (the stream has no B-frames), so each frame is output as it is decoded
    if (max_ref_frames > 1)
      h264_stream->sps->vui.num_reorder_frames = 0;

    // Some devices throw errors if max_dec_frame_buffering < num_ref_frames
    h264_stream->sps->vui.max_dec_frame_buffering =
        ((flags & GS_SPS_RFI_MIN_DPB_HINT) || h264_stream->sps->num_ref_frames <= 1)
            ? 1 : h264_stream->sps->num_ref_frames;

    // These values are the default for the fields, but they are more aggressive
    // than what GFE sends in 2.5.11, but it doesn't seem to cause picture problems.
    h264_stream->sps->vui.max_bytes_per_pic_denom = 2;
    h264_stream->sps->vui.max_bits_per_mb_denom = 1;
  }

  // The caller's buffer has room for 128 bytes of rewritten SPS after the start code
  int written = write_nal_unit(h264_stream, out_buf+*out_offset+start_len, 128);
  if (written < 0) {
    vita_debug_log("SPS: can't write it back, passing it on unchanged\n");
    copy_sps_unchanged(sps, out_buf, out_offset);
    return;
  }

  memcpy(out_buf+*out_offset, sps->data, start_len);
  *out_offset += start_len + written;

  // Every keyframe has one, so only say something when it changes
  int new_length = start_len + written;
  if (host_refs != logged_host_refs || h264_stream->sps->num_ref_frames != logged_refs ||
      h264_stream->sps->level_idc != logged_level || new_length != logged_length) {
    logged_host_refs = host_refs;
    logged_refs = h264_stream->sps->num_ref_frames;
    logged_level = h264_stream->sps->level_idc;
    logged_length = new_length;
    vita_debug_log("SPS: host num_ref_frames %d, level %d; sent to the decoder (holds %d): num_ref_frames %d, "
                   "max_dec_frame_buffering %d, level %d; %d MBs a frame, %d bytes rewritten to %d\n",
                   host_refs, host_level, max_ref_frames, h264_stream->sps->num_ref_frames,
                   h264_stream->sps->vui.max_dec_frame_buffering, h264_stream->sps->level_idc,
                   frame_mbs, (int)sps->length, new_length);
  }
}
