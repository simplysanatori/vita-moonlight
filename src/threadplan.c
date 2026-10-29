#include "threadplan.h"
#include "config.h"
#include "debug.h"

#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/clib.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/threadmgr.h>

#ifndef VITA_THREAD_TUNING
#define VITA_THREAD_TUNING 1
#endif
#ifndef VITA_THREAD_PINNING
#define VITA_THREAD_PINNING 1
#endif

// User threads run on cores 0 to 2; the fourth is the system's
#define CORE_0 SCE_KERNEL_CPU_MASK_USER_0
#define CORE_1 SCE_KERNEL_CPU_MASK_USER_1
#define CORE_2 SCE_KERNEL_CPU_MASK_USER_2
// Floating threads get all three set explicitly when pinning is on, rather than
// whatever they inherited: the headers disagree on whether a mask of 0 means the
// process's or the creating thread's, and a thread made by a pinned one (AsyncTerm,
// from AudioRecv) could otherwise inherit its core
#define ANY_CORE SCE_KERNEL_CPU_MASK_USER_ALL

#if VITA_THREAD_TUNING

typedef struct {
  const char *name;
  int priority; // lower is more urgent, 64 to 191
  int cpu_mask; // CORE_x to pin the thread there, or ANY_CORE; used only with pinning on
} thread_plan;

// Priority 64 is left unused, so the system and middleware keep some headroom
// above everything here. Names are the ones given to PltCreateThread (common-c)
// or sceKernelCreateThread (the app). A thread that isn't listed keeps the
// priority it was created with, usually its creator's (about 160), which is the
// wrong place for anything with a deadline: add new threads here.
//
// Audio is ranked above video: a missed 20 ms audio grain is heard, it costs
// only 5 to 8 % of a core, and video already absorbs jitter in its decode queue.
//
// Cores, when the pinning setting is on: audio on core 2, packet receive on
// core 1, decode and presenting on core 0. Which core is busiest on a Vita isn't
// known (report 03 expects the main thread and system callbacks on core 0), so
// the thread that mostly waits on the hardware decoder takes core 0 and the one
// that must drain the socket does not. Everything small floats over all three.
// Unmeasured: compare with pinning off before trusting any of it.
static const thread_plan plan[] = {
  // The packet path
  {"AudioRecv",   70, CORE_2},
  {"AudioDec",    71, CORE_2},
  {"VideoRecv",   72, CORE_1},
  {"VideoPresent", 73, CORE_0}, // the presenter thread, once it exists: deadline-driven, above VideoDec
  {"VideoDec",    74, CORE_0},
  // Control stream (ENet) and the requests that recover from loss
  {"ControlRecv", 96, ANY_CORE},
  {"ReqIdrFrame", 98, ANY_CORE},
  {"InvRefFrames", 98, ANY_CORE},
  // Input: light, and the latency the user feels most. Close to ControlRecv, since
  // InputSend shares the ENet lock with it and the lock doesn't pass priority on
  {"vitainput_thread", 100, ANY_CORE},
  {"InputSend",  100, ANY_CORE},
  {"vitainput_motion_gyro_thread", 128, ANY_CORE},
  {"vitainput_motion_accel_thread", 128, ANY_CORE},
  // Statistics, keep-alives and callbacks
  {"LossStats",  130, ANY_CORE},
  {"CtrlAsyncCb", 140, ANY_CORE},
  {"VideoPing",  150, ANY_CORE},
  {"AudioPing",  150, ANY_CORE},
  {"AsyncTerm",  160, ANY_CORE},
  // Housekeeping in the app
  {"frame_pacer", 160, ANY_CORE},
  {"vitapower_thread", 170, ANY_CORE},
  {"debug_log_flush", 185, ANY_CORE}, // background writer for the debug log
  // Menu only, nothing to do with a stream
  {"hostscan",   180, ANY_CORE},
  {"mdns",       180, ANY_CORE},
};

static const thread_plan *find_plan(const char *name) {
  for (size_t i = 0; i < sizeof(plan) / sizeof(plan[0]); i++) {
    if (strcmp(plan[i].name, name) == 0) {
      return &plan[i];
    }
  }
  return NULL;
}

#endif

int threadplan_priority(const char *name, int fallback) {
#if VITA_THREAD_TUNING
  const thread_plan *p = find_plan(name);
  if (p != NULL) {
    return p->priority;
  }
#endif
  return fallback;
}

int threadplan_affinity(const char *name) {
#if VITA_THREAD_TUNING && VITA_THREAD_PINNING
  if (config.pin_threads) {
    const thread_plan *p = find_plan(name);
    if (p != NULL) {
      return p->cpu_mask;
    }
  }
#endif
  return SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT;
}

SceUID threadplan_create(const char *name, SceKernelThreadEntry entry, int fallback_priority, SceSize stack_size) {
  return sceKernelCreateThread(name, entry, threadplan_priority(name, fallback_priority), stack_size, 0,
                               threadplan_affinity(name), NULL);
}

#if VITA_THREAD_TUNING

// Applies the table's priority and core to the calling thread. A refusal by the
// kernel is printed even with the debug log off; the log, when on, also says what
// the kernel made of the change.
static void tune_self(const char *name) {
  const thread_plan *p = find_plan(name);
  if (p == NULL) {
    return;
  }

  SceUID self = sceKernelGetThreadId();
  bool logging = config.save_debug_log;
  SceKernelThreadInfo before = {0};
  before.size = sizeof(before);
  if (logging) {
    sceKernelGetThreadInfo(self, &before);
  }

  int priority_rc = sceKernelChangeThreadPriority(self, p->priority);
  int affinity_rc = 0;
  int cpu_mask = threadplan_affinity(name);
  if (cpu_mask != SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT) {
    affinity_rc = sceKernelChangeThreadCpuAffinityMask(self, cpu_mask);
  }
  if (priority_rc < 0 || affinity_rc < 0) {
    sceClibPrintf("[THREAD] %s: priority %d rc 0x%x, mask 0x%x rc 0x%x
",
                  name, p->priority, priority_rc, cpu_mask, affinity_rc);
  }
  if (!logging) {
    return;
  }

  // Read back what the kernel really applied, once per thread at stream start.
  // The name is the table's: pthread-embedded calls every thread "pthread". A
  // thread that changed its own mask moves at its next reschedule, so cpu can
  // still show the old core.
  SceKernelThreadInfo after = {0};
  after.size = sizeof(after);
  if (sceKernelGetThreadInfo(self, &after) < 0) {
    vita_debug_log("[THREAD] %s: wanted priority %d mask 0x%x, set rc 0x%x/0x%x, sceKernelGetThreadInfo failed",
                   name, p->priority, cpu_mask, priority_rc, affinity_rc);
    return;
  }
  vita_debug_log("[THREAD] %s: wanted priority %d mask 0x%x, set rc 0x%x/0x%x, was priority %d mask 0x%x, now priority %d mask 0x%x, cpu %d, stack %u",
                 name, p->priority, cpu_mask, priority_rc, affinity_rc,
                 before.currentPriority, before.currentCpuAffinityMask,
                 after.currentPriority, after.currentCpuAffinityMask,
                 after.currentCpuId, (unsigned)after.stackSize);
}

#endif

// moonlight-common-c makes its threads through PltCreateThread, which gives
// every one the same priority and no core. -Wl,--wrap=PltCreateThread (see
// CMakeLists.txt) routes the calls here, so the new thread can tune itself as
// it starts, from its name, without changing common-c.
typedef struct {
  void (*entry)(void *context);
  void *context;
  const char *name;
} thread_start;

#if VITA_THREAD_TUNING
static void tuned_thread_entry(void *arg) {
  thread_start start = *(thread_start *)arg;
  free(arg);
  tune_self(start.name);
  start.entry(start.context);
}
#endif

// thread is a PLT_THREAD, which the app has no need to see inside
int __real_PltCreateThread(const char *name, void (*entry)(void *context), void *context, void *thread);
int __wrap_PltCreateThread(const char *name, void (*entry)(void *context), void *context, void *thread) {
#if VITA_THREAD_TUNING
  if (find_plan(name) != NULL) {
    thread_start *start = malloc(sizeof(*start));
    if (start != NULL) {
      start->entry = entry;
      start->context = context;
      start->name = name;
      int err = __real_PltCreateThread(name, tuned_thread_entry, start, thread);
      if (err != 0) {
        free(start);
      }
      return err;
    }
  }
#endif
  return __real_PltCreateThread(name, entry, context, thread);
}
