#pragma once

// Priority and core placement for the threads that matter while streaming,
// kept in one table (threadplan.c) so it is easy to tune on a device.
//
// Compile-time switches (set with add_definitions, see CMakeLists.txt):
//   VITA_THREAD_TUNING=0  leave every thread exactly as before: no priority or
//                         core change anywhere (default 1)
//   VITA_THREAD_PINNING=0 keep the priorities but never pin a thread to a
//                         core (default 1)

// The priority a thread named name should be created with, or fallback if the
// table doesn't list it or tuning is off. Vita priorities run from 64 (most
// urgent) to 191 (least); a thread of the SDK's default priority is at 160.
int threadplan_priority(const char *name, int fallback);

// The CPU affinity mask to create a thread named name with: one of the
// SCE_KERNEL_CPU_MASK_USER_x values, or SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT
// (0, the cores of the process) if the thread isn't pinned
int threadplan_affinity(const char *name);

// moonlight-common-c's threads can't be created with a priority or core, so they
// are tuned as they start instead: threadplan.c wraps PltCreateThread (see
// -Wl,--wrap=PltCreateThread in CMakeLists.txt) and looks each thread up by name.
// With VITA_THREAD_TUNING on, each of them logs once, at stream start, the
// priority and core it was given and the ones it really got (debug log on).
