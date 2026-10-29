/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2017 Sunguk Lee
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
#include <stdio.h>
#include <stdarg.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>
#include <stdlib.h>
#include "../src/gui/mdns_log.h"
#include "debug.h"
#include "config.h"

pthread_mutex_t print_mutex;

bool vita_debug_init() {
  if (pthread_mutex_init(&print_mutex, NULL) != 0) {
    return false;
  }
  return true;
}

// Warning: when save_debug_log is on, every call allocates 8 KB, takes a global
// mutex and flushes to the memory card. It is called from the video, audio and
// input threads (on dropped frames, touch events and common-c's own logging),
// so it noticeably slows streaming. Never measure performance with it enabled,
// and avoid adding calls on per-frame or per-event paths.
void vita_debug_log(const char *s, ...) {
  if (!config.save_debug_log) {
    return;
  }

  pthread_mutex_lock(&print_mutex);

  char* buffer = malloc(8192);
  if (!buffer) {
    pthread_mutex_unlock(&print_mutex);
    return;
  }

  SceDateTime time;
  sceRtcGetCurrentClock(&time, 0);

  snprintf(buffer, 26, "%04d%02d%02d %02d:%02d:%02d.%06d ",
           time.year, time.month, time.day,
           time.hour, time.minute, time.second,
           time.microsecond);

  va_list va;
  va_start(va, s);
  int len = vsnprintf(&buffer[25], 8000, s, va);
  va_end(va);

  if (config.log_file) {
    fprintf(config.log_file, "%s", buffer);
    if (buffer[len + 24] != '\n') {
        fprintf(config.log_file, "\n");
    }
    fflush(config.log_file);
  } else {
    // Si no se pudo abrir el log, mostrar por pantalla
    printf("[Moonlight] No se pudo abrir el archivo de log. Mensaje: %s\n", &buffer[25]);
  }

#ifdef __vita__
  // También imprimir por mdns_log (sceClibPrintf)
  mdns_log("%s", &buffer[25]);
#endif

  free(buffer);

  pthread_mutex_unlock(&print_mutex);
}


// Logs the calling thread's scheduling state, to check which priority and cores
// the streaming threads really end up with
void vita_debug_log_thread(const char *label) {
  if (!config.save_debug_log) {
    return;
  }

  SceKernelThreadInfo info = {0};
  info.size = sizeof(info);
  int ret = sceKernelGetThreadInfo(sceKernelGetThreadId(), &info);
  if (ret < 0) {
    vita_debug_log("[THREAD] %s: sceKernelGetThreadInfo failed 0x%x", label, ret);
    return;
  }

  vita_debug_log("[THREAD] %s (%s): priority init %d current %d, affinity init 0x%x current 0x%x, cpu %d, stack %d",
                 label, info.name, info.initPriority, info.currentPriority,
                 info.initCpuAffinityMask, info.currentCpuAffinityMask, info.currentCpuId, info.stackSize);
}
