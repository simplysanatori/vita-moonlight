/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015 Iwan Timmer, Sunguk Lee
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
#include <stdbool.h>
#include <pthread.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/power.h>
#include "../config.h"
#include "../debug.h"

enum {
  ENABLE_ALL = 0,
  DISABLE_SUSPEND = 1,
};

enum {
  BOOST_ARM_CLOCK_MHZ = 444,
  BOOST_BUS_CLOCK_MHZ = 222,
  BOOST_GPU_CLOCK_MHZ = 222,
  BOOST_GPU_XBAR_CLOCK_MHZ = 166,
};

typedef struct {
  int arm;
  int bus;
  int gpu;
  int gpu_xbar;
} clock_state;

static int powermode = ENABLE_ALL;
static bool active_power_thread = false;
static bool clocks_boosted = false;
static clock_state saved_clocks = {0};
// Clocks are boosted while streaming, and while the network work around it
// runs (vitapower_busy_begin): TLS, pairing's RSA and the RTSP handshake are
// CPU-bound, and at stock clocks they can outlast Sunshine's timeouts
static bool streaming = false;
static int busy = 0;
// The stream can end on a common-c thread while the UI thread ends its busy
// work, so the state and the clock change it leads to are one step
static pthread_mutex_t clocks_mutex;

static void boost_clocks() {
  if (clocks_boosted) {
    return;
  }
  saved_clocks.arm = scePowerGetArmClockFrequency();
  saved_clocks.bus = scePowerGetBusClockFrequency();
  saved_clocks.gpu = scePowerGetGpuClockFrequency();
  saved_clocks.gpu_xbar = scePowerGetGpuXbarClockFrequency();

  scePowerSetArmClockFrequency(BOOST_ARM_CLOCK_MHZ);
  scePowerSetBusClockFrequency(BOOST_BUS_CLOCK_MHZ);
  scePowerSetGpuClockFrequency(BOOST_GPU_CLOCK_MHZ);
  scePowerSetGpuXbarClockFrequency(BOOST_GPU_XBAR_CLOCK_MHZ);
  clocks_boosted = true;

  vita_debug_log("Clocks boosted (arm/bus/gpu/xbar): %d/%d/%d/%d -> %d/%d/%d/%d MHz",
                 saved_clocks.arm, saved_clocks.bus, saved_clocks.gpu, saved_clocks.gpu_xbar,
                 scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(),
                 scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency());
}

static void restore_clocks() {
  if (!clocks_boosted) {
    return;
  }
  scePowerSetArmClockFrequency(saved_clocks.arm);
  scePowerSetBusClockFrequency(saved_clocks.bus);
  scePowerSetGpuClockFrequency(saved_clocks.gpu);
  scePowerSetGpuXbarClockFrequency(saved_clocks.gpu_xbar);
  clocks_boosted = false;

  vita_debug_log("Clocks restored (arm/bus/gpu/xbar): %d/%d/%d/%d MHz",
                 scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(),
                 scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency());
}

static void update_clocks() {
  // Read the global config so a change made in the settings menu applies to
  // the next stream, or when turned off from the pause menu, to this one
  if (config.boost_clocks && (streaming || busy > 0)) {
    boost_clocks();
  } else {
    restore_clocks();
  }
}

int vitapower_thread(SceSize args, void *argp) {
  while (1) {
    if (!active_power_thread) {
      sceKernelDelayThread(10 * 1000 * 1000);
      continue;
    }
    if (powermode & DISABLE_SUSPEND) {
      sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
      sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_OLED_OFF);
    }
    if (!scePowerIsBatteryCharging() && scePowerIsLowBattery()) {
      // TODO print warning message
    }
    sceKernelDelayThread(10 * 1000 * 1000);
  }

  return 0;
}

bool vitapower_init() {
  pthread_mutex_init(&clocks_mutex, NULL);
  SceUID thid = sceKernelCreateThread("vitapower_thread", vitapower_thread, 0x10000100, 0x40000, 0, 0, NULL);
  if (thid >= 0)
    sceKernelStartThread(thid, 0, NULL);

  return true;
}

void vitapower_config(CONFIGURATION config) {
  powermode = ENABLE_ALL;

  if (config.disable_powersave) {
    powermode |= DISABLE_SUSPEND;
  }
}

void vitapower_start() {
  active_power_thread = true;
  pthread_mutex_lock(&clocks_mutex);
  streaming = true;
  update_clocks();
  pthread_mutex_unlock(&clocks_mutex);
}

void vitapower_stop() {
  active_power_thread = false;
  pthread_mutex_lock(&clocks_mutex);
  streaming = false;
  update_clocks();
  pthread_mutex_unlock(&clocks_mutex);
}

void vitapower_busy_begin() {
  pthread_mutex_lock(&clocks_mutex);
  busy++;
  update_clocks();
  pthread_mutex_unlock(&clocks_mutex);
}

void vitapower_busy_end() {
  pthread_mutex_lock(&clocks_mutex);
  if (busy > 0) {
    busy--;
  }
  update_clocks();
  pthread_mutex_unlock(&clocks_mutex);
}
