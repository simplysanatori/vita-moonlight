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

#pragma once

#include <Limelight.h>
#include <stdbool.h>
#include <stdint.h>

// Values for config.stats_overlay
enum {
  STATS_OVERLAY_OFF = 0,
  STATS_OVERLAY_FPS = 1,
  STATS_OVERLAY_FULL = 2,
  STATS_OVERLAY_COUNT
};

bool vitastats_init(void);
// Clears all counters, at the start of each stream
void vitastats_reset(void);

// Called from the video decode thread for each decode unit
void vitastats_frame_received(const DECODE_UNIT *du, uint64_t submit_us);
void vitastats_frame_decoded(uint64_t decode_us);
void vitastats_frame_rendered(uint64_t render_us);
void vitastats_frame_dropped_by_pacer(void);

// The presenter (frame pacing Smooth) draws on its own thread, so a frame's
// timings are collected when it is decoded and completed when it is shown.
// The time the frame just decoded has taken so far, to pass to
// vitastats_frame_presented() later. Call right after vitastats_frame_decoded().
uint64_t vitastats_frame_pipeline_us(void);
// A queued frame reached the screen: wait_us in the queue, render_us drawing it
void vitastats_frame_presented(uint64_t pipeline_us, uint64_t wait_us, uint64_t render_us);
// Once per refresh: whether a new frame was shown (else the last stayed up),
// and how many decoded frames were waiting
void vitastats_presenter_vblank(bool presented, int queue_depth);
// A queued frame was thrown away to bring the queue back to its size
void vitastats_frame_dropped_by_presenter(void);

// Rolls the counters into the displayed figures. Call about once a second.
void vitastats_tick(void);

// Draws the overlay for the current config.stats_overlay mode. Call between
// vita2d_start_drawing() and vita2d_end_drawing().
void vitastats_draw(void);
