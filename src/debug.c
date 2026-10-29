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
#include <string.h>
#include "../src/gui/mdns_log.h"
#include "debug.h"
#include "config.h"
#include "threadplan.h"

pthread_mutex_t print_mutex;
// Taken by whoever writes the log buffer out, one at a time
static pthread_mutex_t flush_mutex;

bool vita_debug_init() {
  if (pthread_mutex_init(&print_mutex, NULL) != 0) {
    return false;
  }
  if (pthread_mutex_init(&flush_mutex, NULL) != 0) {
    return false;
  }
  return true;
}

// How the log reaches the memory card. A write there can take several to tens
// of milliseconds, and vita_debug_log() is called from the video, audio and
// input threads (on dropped frames, touch events, common-c's own logging), so
// it only copies the line into a buffer. A low priority thread writes the
// buffer out every LOG_FLUSH_INTERVAL_US, and vita_debug_flush() does the same
// on demand (end of a stream, exit). A crash can lose the last quarter second.
#define LOG_BUFFER_SIZE (64 * 1024)
#define LOG_FLUSH_INTERVAL_US (250 * 1000)
// Below the menu and the streaming threads (higher numbers are lower on the Vita)
#define LOG_THREAD_PRIORITY (0x10000100 + 16)
#define LOG_THREAD_STACK_SIZE 0x4000
#define LOG_LINE_SIZE 8192
#define LOG_TIMESTAMP_SIZE 25

// Lines go into log_buffer under print_mutex. A drain swaps it with log_spare
// and writes the old one out, under flush_mutex so two drains never overlap.
static char *log_buffer = NULL;
static char *log_spare = NULL;
static size_t log_length = 0;
static unsigned log_dropped = 0;
static bool log_thread_started = false;
// Set by the exit handler, after which only it touches the FILE
static volatile bool log_exiting = false;

static void debug_drain(bool from_thread) {
  pthread_mutex_lock(&flush_mutex);
  if (from_thread && log_exiting) {
    pthread_mutex_unlock(&flush_mutex);
    return;
  }
  pthread_mutex_lock(&print_mutex);
  char *out = log_buffer;
  size_t length = log_length;
  unsigned dropped = log_dropped;
  if (out) {
    log_buffer = log_spare;
    log_spare = out;
    log_length = 0;
  }
  log_dropped = 0;
  pthread_mutex_unlock(&print_mutex);

  if (config.log_file && (length > 0 || dropped > 0)) {
    if (length > 0) {
      fwrite(out, 1, length, config.log_file);
    }
    if (dropped > 0) {
      fprintf(config.log_file, "[LOG] %u lines didn't fit in the log buffer and were dropped\n", dropped);
    }
    fflush(config.log_file);
  }
  pthread_mutex_unlock(&flush_mutex);
}

static int debug_flush_thread(SceSize args, void *argp) {
  while (!log_exiting) {
    sceKernelDelayThread(LOG_FLUSH_INTERVAL_US);
    debug_drain(true);
  }
  return 0;
}

// exit() runs this before newlib closes the log file
static void debug_flush_at_exit(void) {
  log_exiting = true;
  debug_drain(false);
}

// Call with print_mutex held. Returns false when there is nowhere to put lines.
static bool debug_start_buffering(void) {
  if (log_buffer && log_spare) {
    return true;
  }
  log_buffer = malloc(LOG_BUFFER_SIZE);
  log_spare = malloc(LOG_BUFFER_SIZE);
  if (!log_buffer || !log_spare) {
    free(log_buffer);
    free(log_spare);
    log_buffer = NULL;
    log_spare = NULL;
    return false;
  }
  log_length = 0;
  log_dropped = 0;
  if (!log_thread_started) {
    // threadplan_create doesn't log, so it's safe with print_mutex held
    SceUID thid = threadplan_create("debug_log_flush", debug_flush_thread, LOG_THREAD_PRIORITY,
                                    LOG_THREAD_STACK_SIZE);
    if (thid >= 0 && sceKernelStartThread(thid, 0, NULL) >= 0) {
      log_thread_started = true;
      // exit() from the UI (Quit) must not lose what hasn't been written yet
      atexit(debug_flush_at_exit);
    }
  }
  return true;
}

void vita_debug_flush(void) {
  if (!log_buffer) {
    return;
  }
  debug_drain(false);
}

// When save_debug_log is on, every call allocates 8 KB and takes a global
// mutex to copy the line into the log buffer; the memory card write happens
// on another thread. Still avoid adding calls on per-frame or per-event
// paths, and don't measure performance with logging enabled.
void vita_debug_log(const char *s, ...) {
  if (!config.save_debug_log) {
    return;
  }

  char* buffer = malloc(LOG_LINE_SIZE);
  if (!buffer) {
    return;
  }

  SceDateTime time;
  sceRtcGetCurrentClock(&time, 0);

  snprintf(buffer, LOG_TIMESTAMP_SIZE + 1, "%04d%02d%02d %02d:%02d:%02d.%06d ",
           time.year, time.month, time.day,
           time.hour, time.minute, time.second,
           time.microsecond);

  va_list va;
  va_start(va, s);
  int len = vsnprintf(&buffer[LOG_TIMESTAMP_SIZE], LOG_LINE_SIZE - LOG_TIMESTAMP_SIZE - 1, s, va);
  va_end(va);
  // vsnprintf returns the length the message would have had
  if (len < 0) {
    len = 0;
  } else if (len > LOG_LINE_SIZE - LOG_TIMESTAMP_SIZE - 2) {
    len = LOG_LINE_SIZE - LOG_TIMESTAMP_SIZE - 2;
  }
  char *message = &buffer[LOG_TIMESTAMP_SIZE];

#ifdef __vita__
  // También imprimir por mdns_log (sceClibPrintf)
  mdns_log("%s", message);
#endif

  bool ended = len > 0 && message[len - 1] == '\n';
  size_t line_length = LOG_TIMESTAMP_SIZE + len;
  if (!ended) {
    buffer[line_length++] = '\n';
  }

  pthread_mutex_lock(&print_mutex);
  bool direct = false;
  if (config.log_file) {
    if (!debug_start_buffering()) {
      // No buffer to put it in: write it out ourselves, as before
      direct = true;
    } else if (log_length + line_length > LOG_BUFFER_SIZE) {
      log_dropped++;
    } else {
      memcpy(&log_buffer[log_length], buffer, line_length);
      log_length += line_length;
    }
  } else {
    // Si no se pudo abrir el log, mostrar por pantalla
    buffer[line_length] = '\0';
    printf("[Moonlight] No se pudo abrir el archivo de log. Mensaje: %s", message);
  }
  pthread_mutex_unlock(&print_mutex);

  if (direct) {
    pthread_mutex_lock(&flush_mutex);
    fwrite(buffer, 1, line_length, config.log_file);
    fflush(config.log_file);
    pthread_mutex_unlock(&flush_mutex);
  } else if (config.log_file && !log_thread_started) {
    // Without the flush thread, write each line out as it comes, as before
    debug_drain(false);
  }
  free(buffer);
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

  vita_debug_log("[THREAD] %s (%s): priority init %d current %d, affinity init 0x%x current 0x%x, cpu %d, stack %u",
                 label, info.name, info.initPriority, info.currentPriority,
                 info.initCpuAffinityMask, info.currentCpuAffinityMask, info.currentCpuId, (unsigned)info.stackSize);
}
