// check_host.h
#ifndef CHECK_HOST_H
#define CHECK_HOST_H

#include "device.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Estados posibles para el host
#define HOST_OFFLINE 0
#define HOST_ONLINE  1
#define HOST_IP_CHANGED 2

struct host_status {
    int status; // 0: offline, 1: online, 2: ip changed
    char current_ip[64];
};

// Chequea el estado de un host registrado
struct host_status check_host_status(const device_info_t *info);

#include <psp2/kernel/threadmgr.h>
#define MAX_HOSTS 16

// Array global de resultados de estado de hosts
extern struct host_status g_host_status[MAX_HOSTS];
extern volatile int g_host_scan_thread_status;
extern volatile int g_host_status_changed;

// Starts the host scan, unless one is already running. Never runs two.
void start_host_scan_thread();
// Stops the host scan and waits for its thread to end (normally well under
// 2 s; it gives up after 5 s and keeps the thread for the next call)
void stop_host_scan_thread();
bool find_host_ip_mdns(const char *hostname, char *out_ip, size_t out_len);

// The mDNS sniffer isn't thread-safe: hold this lock from its deinit/init
// through the last poll. Returns false if it wasn't free within timeout_us.
bool mdns_sniffer_lock(SceUInt timeout_us);
void mdns_sniffer_unlock(void);

#ifdef __cplusplus
}
#endif

#endif // CHECK_HOST_H
