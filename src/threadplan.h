#pragma once

#include <psp2/kernel/threadmgr.h>

// Priority and core placement for the threads that matter while streaming,
// kept in one table (threadplan.c) so it is easy to tune on a device. A new
// thread should be made with threadplan_create and get an entry there.
//
// Priorities apply to every listed thread. Cores are only set when the
// "CPU core pinning" setting (config.pin_threads, off by default) is on.
//
// Compile-time switches (set with add_definitions, see CMakeLists.txt):
//   VITA_THREAD_TUNING=0  leave every thread exactly as before: no priority or
//                         core change anywhere (default 1)
//   VITA_THREAD_PINNING=0 never touch cores, whatever the setting says (default 1)

// Creates (but doesn't start) a thread with the priority and cores the table
// lists for name. A name that isn't listed gets fallback as its priority and
// the cores of the process, as sceKernelCreateThread(name, entry, fallback,
// stack_size, 0, 0, NULL) would.
SceUID threadplan_create(const char *name, SceKernelThreadEntry entry, int fallback_priority, SceSize stack_size);

// The priority a thread named name should be created with, or fallback if the
// table doesn't list it or tuning is off. Vita priorities run from 64 (most
// urgent) to 191 (least); a thread of the SDK's default priority is at 160.
int threadplan_priority(const char *name, int fallback);

// The CPU affinity mask to create a thread named name with: a combination of
// SCE_KERNEL_CPU_MASK_USER_x values, or SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT
// (0, which the kernel resolves itself) when cores are left alone
int threadplan_affinity(const char *name);

// moonlight-common-c's threads can't be created with a priority or core, so they
// are tuned as they start instead: threadplan.c wraps PltCreateThread (see
// -Wl,--wrap=PltCreateThread in CMakeLists.txt) and looks each thread up by name.
// With the debug log on, each of them logs once, at stream start, the priority and
// core it was given and the ones it really got.
